#include "FSRDPreprocessCommon.hlsli"

#define MainRS \
    "RootFlags(0), " \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 9), visibility = SHADER_VISIBILITY_ALL), " \
    "DescriptorTable(UAV(u0, numDescriptors = 1), visibility = SHADER_VISIBILITY_ALL), " \
    "StaticSampler(s0, " \
        "filter = FILTER_MIN_MAG_MIP_LINEAR, " \
        "addressU = TEXTURE_ADDRESS_CLAMP, " \
        "addressV = TEXTURE_ADDRESS_CLAMP, " \
        "addressW = TEXTURE_ADDRESS_CLAMP, " \
        "visibility = SHADER_VISIBILITY_ALL)"

// Dispatch config
#define THREAD_GROUP_SIZE_X     8
#define THREAD_GROUP_SIZE_Y     8
#define NUM_THREADS             (THREAD_GROUP_SIZE_X * THREAD_GROUP_SIZE_Y)

static const uint2 s_ThreadGroupSize =  uint2(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y);

// Kernel config
#define KERNEL_SIZE             5
#define KERNEL_RANGE_MIN        (-KERNEL_SIZE / 2)
#define KERNEL_RANGE_MAX        (KERNEL_SIZE / 2)

static const float s_InvKernelSize =    1.0f / (KERNEL_SIZE * KERNEL_SIZE);

// Separable binomial weights defining where the frequency split falls. Wider than a
// box of the same extent, so the two bands meet without a ringing seam.
static const half s_SplitWeights[KERNEL_SIZE] = { 1.0h, 4.0h, 6.0h, 4.0h, 1.0h };

// Shared memory config
DEFINE_LDS_CONFIG(s_SM, KERNEL_SIZE);
DECLARE_LDS_ARRAY_2D(half4, g_RawColor, KERNEL_SIZE);
DECLARE_LDS_ARRAY_2D(half4, g_DenoisedColor, KERNEL_SIZE);
// The band split and the RR anchor both need this neighbourhood, so it joins the tile
// rather than being resampled per feature.
DECLARE_LDS_ARRAY_2D(half4, g_Handover, KERNEL_SIZE);

// Feature Flags
#define FLAGS_RAW_SOURCE_BLIT           (1 << 0)
#define FLAGS_SCALE_SRC                 (1 << 1)
#define FLAGS_DIFFUSE_SIGNAL_INDIRECT   (1 << 2)
#define FLAGS_SPECULAR_SIGNAL_INDIRECT  (1 << 3)

// Debug Flags
#define FLAGS_DEBUG                     (1 << 16)
#define FLAGS_DEBUG_MODE_MASK           (0xFF << 16)

#define FLAGS_DEBUG_CORRELATION_BIAS    (1 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_SKIP_SIGNAL         (2 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DENOISER_OUTPUT     (3 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DIRECT_SPECULAR     (4 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DIRECT_DIFFUSE      (5 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_INDIRECT_DIFFUSE    (6 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_HANDOVER_RR_BAND    (7 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_HANDOVER_DETAIL_BAND (8 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_HANDOVER_BAND_MIX   (9 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_HANDOVER_ANCHOR     (10 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_HANDOVER_WEIGHT     (11 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_INDIRECT_SPECULAR   (12 << 17 | FLAGS_DEBUG)
Texture2D<half4> InIndirectSpecular : register(t0);
Texture2D<half4> InSpecularAlbedo : register(t1);
Texture2D<half4> InDirectDiffuse : register(t2);
Texture2D<half4> InDiffuseAlbedo : register(t3);

// Secondary buffers
Texture2D<half4> InSkipSignal : register(t4);
Texture2D<half4> InRawColor : register(t5);

Texture2D<half4> InRawIndirectSpecular : register(t6);
Texture2D<half4> InNormals : register(t7);

// RGB: the zero-roughness handover image, A: 1 on handed-over pixels. The conversion
// pass owns the type-1 classification; this weight is how it reaches composition.
Texture2D<half4> InHandover : register(t8);

RWTexture2D<half4> OutColor : register(u0);

SamplerState LinearSampler : register(s0);

cbuffer CB_Comp : register(b0)
{
    float4 DstTexSize;
    uint4 SourceBase; // XY = raw color origin, ZW unused
    
    float CorrelationBias;
    uint Flags;

    float2 SourceUvScale;
    float2 SourceUvOffset;

    // Handover refinements, each inert at zero.
    float FloorHandoverAnchorClamp;
    float FloorHandoverCorrelationMix;
    float _Padding0;
}

bool IsSet(uint mask) { return (Flags & mask) == mask; }
uint GetDebugMode() { return (Flags & FLAGS_DEBUG_MODE_MASK); }

// Correlates raw noisy input with denoised color using a modified SSIM.
half GetRawColorSimilarity(const uint2 gtID)
{
    const int2 smCenter = gtID + s_SM_HaloOffset;    
    float meanD = 0.0f;
    float meanR = 0.0f;
    float meanDD = 0.0f; // D^2
    float meanRR = 0.0f; // R^2
    float meanRD = 0.0f; // R*D

    static const float s_RcpSigma = 1.0f / 1.2f;
    float totalWeight = 0.0f;
    
    [unroll]
    for (int x1 = KERNEL_RANGE_MIN; x1 <= KERNEL_RANGE_MAX; x1++)
    {
        [unroll]
        for (int y1 = KERNEL_RANGE_MIN; y1 <= KERNEL_RANGE_MAX; y1++)
        {
            const int2 smID = smCenter + int2(x1, y1);
            float w = exp(-(Square(x1) + Square(y1)) * s_RcpSigma); // This is precomputed by DXC
            totalWeight += w;
            
            const float lumD = g_DenoisedColor[smID.x][smID.y].a;
            meanD += w * lumD;
            meanDD += w * Square(lumD);
  
            const float lumR = g_RawColor[smID.x][smID.y].a;
            meanR += w * lumR;
            meanRR += w * Square(lumR);
            meanRD += w * lumR * lumD;
        }
    }
    
    const float rcpTotalWeight = rcp(totalWeight);   
    meanD *= rcpTotalWeight;
    meanR *= rcpTotalWeight;
    meanDD *= rcpTotalWeight;
    meanRR *= rcpTotalWeight;
    meanRD *= rcpTotalWeight;
    
    const float meanDSq = Square(meanD);
    const float meanRSq = Square(meanR);
    
    // Variances (std.dev^2)
    // E[X^2] - (E[X])^2 - Average of squares, less the square of the average
    const float varD = max(meanDD - meanDSq, 0.0f);
    const float varR = max(meanRR - meanRSq, 2e-3f);
    
    // Std. Deviation
    const float devD = sqrt(varD);
    const float devR = sqrt(varR);
    
    // Covariance
    // E[X*Y] - E[X]E[Y] - Average of R*D product, less product of their averages
    const float covRD = meanRD - (meanD * meanR);
        
    // Correlation
    static const float s_SSIMRelaxation = 0.1f;
    static const float s_COVThreshold = 0.2f;
    
    static const float c1 = Square(1e-2f * s_SSIMRelaxation) + 0.1f;
    static const float c2 = Square(3e-2f * s_SSIMRelaxation);
    static const float c3 = 1.0f * c2;
    
    // Standard SSIM components
    const float strucCorrelation = ((covRD + c3) * rcp(devD * devR + c3));
    const float conCorrelation = (2.0f * devD * devR + c2) * rcp(varD + varR + c2);
    const float lumCorrelation = (2.0f * meanD * meanR) * rcp(meanDSq + meanRSq + c1);
    const half ssim = half(strucCorrelation * conCorrelation * lumCorrelation);
    
    // Variance gating. The denoiser doesn't destroy genuine detail. It might attenuate details, or even
    // hallucinate, but if it says it's flat, then almost certainly flat.
    const half covD = half(devD * rcp(max(meanD, 1e-2f)));
    const half similarity = half(smoothstep(0.0f, 0.5f, ssim) * smoothstep(0.0f, s_COVThreshold, covD));
    
    return min(max(similarity, 0.0h), 1.0h);
}

// Agreement between the two paths, as a modified SSIM over their luminance. Same
// construction as GetRawColorSimilarity, but neither image carries a precomputed luma
// in alpha here - the handover's alpha is its mix weight - so both are derived from
// RGB in the loop.
//
// High agreement means RR preserved the structure the handover has, and RR is the
// better pick there because it also carries temporal stability. Low agreement means
// RR removed something the handover kept, which is the case the handover exists for.
half GetHandoverAgreement(const uint2 gtID)
{
    const int2 smCenter = gtID + s_SM_HaloOffset;
    float meanD = 0.0f;
    float meanH = 0.0f;
    float meanDD = 0.0f;
    float meanHH = 0.0f;
    float meanDH = 0.0f;

    static const float s_RcpSigma = 1.0f / 1.2f;
    float totalWeight = 0.0f;

    [unroll]
    for (int x1 = KERNEL_RANGE_MIN; x1 <= KERNEL_RANGE_MAX; x1++)
    {
        [unroll]
        for (int y1 = KERNEL_RANGE_MIN; y1 <= KERNEL_RANGE_MAX; y1++)
        {
            const int2 smID = smCenter + int2(x1, y1);
            const float w = exp(-(Square(x1) + Square(y1)) * s_RcpSigma);
            totalWeight += w;

            const float lumD = GetLuminance(g_DenoisedColor[smID.x][smID.y].rgb);
            const float lumH = GetLuminance(g_Handover[smID.x][smID.y].rgb);

            meanD += w * lumD;
            meanH += w * lumH;
            meanDD += w * Square(lumD);
            meanHH += w * Square(lumH);
            meanDH += w * lumD * lumH;
        }
    }

    const float rcpTotalWeight = rcp(totalWeight);
    meanD *= rcpTotalWeight;
    meanH *= rcpTotalWeight;
    meanDD *= rcpTotalWeight;
    meanHH *= rcpTotalWeight;
    meanDH *= rcpTotalWeight;

    const float varD = max(meanDD - Square(meanD), 0.0f);
    const float varH = max(meanHH - Square(meanH), 0.0f);
    const float covDH = meanDH - (meanD * meanH);

    static const float c1 = 1e-2f;
    static const float c2 = 1e-3f;

    const float structure = (2.0f * covDH + c2) * rcp(varD + varH + c2);
    const float luminance =
        (2.0f * meanD * meanH + c1) * rcp(Square(meanD) + Square(meanH) + c1);

    return half(saturate(structure * luminance));
}

// The bands the handover combination is built from.
//
// Returned as a group rather than computed inline so the debug views read exactly the
// values composition uses. A separate reimplementation for visualisation would be free
// to drift from the real path, which is the failure mode that makes a debug view worse
// than none at all.
struct HandoverBands
{
    float3 RRLowBand;    // RR's lowpass - what RR contributes to the result
    float3 DetailHigh;   // handover minus its own lowpass - what the handover contributes
    float3 RRDeviation;  // RR's local standard deviation, for the anchor clamp
};

HandoverBands GetHandoverBands(const int2 smID, const float3 handoverColor)
{
    // Keep the reductions in FP32. Squaring an HDR FP16 sample can overflow well
    // before the input itself reaches FP16's maximum and poisons the deviation with
    // INF/NaN, which then propagates through the anchor clamp.
    float3 lowpassDenoised = 0.0f;
    float3 lowpassHandover = 0.0f;
    float3 denoisedSquared = 0.0f;
    float totalSplitWeight = 0.0f;

    [unroll]
    for (int sy = KERNEL_RANGE_MIN; sy <= KERNEL_RANGE_MAX; sy++)
    {
        [unroll]
        for (int sx = KERNEL_RANGE_MIN; sx <= KERNEL_RANGE_MAX; sx++)
        {
            const int2 tapID = smID + int2(sx, sy);
            const float w = float(s_SplitWeights[sx - KERNEL_RANGE_MIN]) *
                            float(s_SplitWeights[sy - KERNEL_RANGE_MIN]);
            const float3 tapDenoised = float3(g_DenoisedColor[tapID.x][tapID.y].rgb);

            lowpassDenoised += w * tapDenoised;
            denoisedSquared += w * tapDenoised * tapDenoised;
            lowpassHandover += w * float3(g_Handover[tapID.x][tapID.y].rgb);
            totalSplitWeight += w;
        }
    }

    const float rcpSplitWeight = rcp(max(totalSplitWeight, 1e-4f));
    lowpassDenoised *= rcpSplitWeight;
    lowpassHandover *= rcpSplitWeight;
    denoisedSquared *= rcpSplitWeight;

    HandoverBands bands;
    bands.RRLowBand = lowpassDenoised;
    // Subtracting a signal's own lowpass leaves only what that lowpass could not
    // represent, so this is a highpass by construction - and signed.
    bands.DetailHigh = handoverColor - lowpassHandover;
    bands.RRDeviation =
        sqrt(max(denoisedSquared - lowpassDenoised * lowpassDenoised, 0.0f));
    return bands;
}

void PopulateSharedMemory(const uint2 groupID, const int2 gtID)
{
    const int2 pxOrigin = groupID.xy * s_ThreadGroupSize - s_SM_HaloOffset;
    const uint flatID = gtID.x + gtID.y * s_ThreadGroupSize.x;
    const int2 maxBounds = int2(DstTexSize.xy) - 1;

    [unroll]
    for (int i = 0; i < s_SM_LoadsPerThread; i++)
    {
        const uint smFlatID = flatID + i * NUM_THREADS;
        
        if (smFlatID < s_SM_ElementCount)
        {
            const int2 smID = int2(smFlatID % s_SM_Size.x, smFlatID / s_SM_Size.x);
            const int2 px = clamp(pxOrigin + smID, int2(0, 0), maxBounds);
            const float3 denoisedSpecColor = InIndirectSpecular[px].rgb;
            const float3 denoisedDiffColor = InDirectDiffuse[px].rgb;
            const float3 specReflectance = InSpecularAlbedo[px].rgb;
            const float3 diffAlbedo = InDiffuseAlbedo[px].rgb;
            const half3 totalAlbedo = GetSafeFP16(specReflectance + diffAlbedo);
            // Preserve the title's per-channel modulation for every material.
            half3 denoisedColor = GetSafeFP16(
                (denoisedSpecColor * specReflectance) +
                (denoisedDiffColor * diffAlbedo));

            uint rawWidth, rawHeight;
            InRawColor.GetDimensions(rawWidth, rawHeight);
            const int2 rawPx = clamp(
                px + int2(SourceBase.xy),
                int2(0, 0),
                int2(rawWidth, rawHeight) - 1);
            const half3 rawColor = GetSafeFP16(
                InRawColor[rawPx].rgb);
            const half4 skipColor = GetSafeFP16(InSkipSignal[px]);
            const half skipLuma = skipColor.a;
            
            // Use demodulated color for luma references, but use remodulated color for output colors.
            const float rcpTotalAlbedo = rcp(max(GetLuminance(totalAlbedo), 1e-2f));
            const half rawRef = GetSafeFP16(GetLuminance(rawColor) * rcpTotalAlbedo);
            const half denoisedRef = GetSafeFP16((GetLuminance(denoisedColor) + skipColor.a) * rcpTotalAlbedo);
            denoisedColor += skipColor.rgb;

            g_RawColor[smID.x][smID.y] = half4(rawColor, rawRef);
            g_DenoisedColor[smID.x][smID.y] = half4(denoisedColor, denoisedRef);
            g_Handover[smID.x][smID.y] = GetSafeFP16(InHandover[px]);
        }
    }
    
    GroupMemoryBarrierWithGroupSync();
}

[RootSignature(MainRS)]
[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void CSMain(uint3 groupID : SV_GroupID, uint3 gtID : SV_GroupThreadID)
{
    const uint2 px = groupID.xy * s_ThreadGroupSize + gtID.xy;
    const float2 uv = (float2(px) + 0.5f) * DstTexSize.zw;
    const bool inBounds = px.x < DstTexSize.x && px.y < DstTexSize.y;
    
    [branch]
    if (IsSet(FLAGS_RAW_SOURCE_BLIT))
    {
        if (!inBounds)
            return;

        [branch]
        if (IsSet(FLAGS_SCALE_SRC))
            OutColor[px] = InIndirectSpecular.SampleLevel(
                LinearSampler, uv * SourceUvScale + SourceUvOffset, 0);
        else
            OutColor[px] = InIndirectSpecular[px];
    }
    else
    {
        const int2 smID = gtID.xy + s_SM_HaloOffset;      
        PopulateSharedMemory(groupID.xy, gtID.xy);

        // All lanes in a partial group must reach PopulateSharedMemory's group
        // barrier. Out-of-range lanes may leave only after synchronization.
        if (!inBounds)
            return;

        // Correlate raw RT input with denoiser output
        const half similarity = GetRawColorSimilarity(gtID.xy);
        const float baseRawWeight = float(similarity) * CorrelationBias;
        const half rawWeight = half(saturate(baseRawWeight));
        
        [branch]
        if (IsSet(FLAGS_DEBUG))
        {
            switch (GetDebugMode())
            {
                case FLAGS_DEBUG_CORRELATION_BIAS:
                    OutColor[px] = half4(TurboColormap(rawWeight), 1.0f);
                    break;
                case FLAGS_DEBUG_SKIP_SIGNAL:
                    OutColor[px] = half4(InSkipSignal[px].rgb, 1.0f);
                    break;
                // The specular signal occupies one pair of resources whichever type
                // it was dispatched as, so these two views differ only in which
                // dispatch they claim to be showing. Magenta where the claim does not
                // match the active type, so a mismatched pick is unmistakable rather
                // than quietly displaying the other signal.
                case FLAGS_DEBUG_DIRECT_SPECULAR:
                    OutColor[px] = IsSet(FLAGS_SPECULAR_SIGNAL_INDIRECT)
                        ? half4(1.0f, 0.0f, 1.0f, 1.0f)
                        : half4(
                            InIndirectSpecular[px].rgb * InSpecularAlbedo[px].rgb,
                            1.0f);
                    break;

                case FLAGS_DEBUG_INDIRECT_SPECULAR:
                    OutColor[px] = IsSet(FLAGS_SPECULAR_SIGNAL_INDIRECT)
                        ? half4(
                            InIndirectSpecular[px].rgb * InSpecularAlbedo[px].rgb,
                            1.0f)
                        : half4(1.0f, 0.0f, 1.0f, 1.0f);
                    break;
                case FLAGS_DEBUG_DIRECT_DIFFUSE:
                    OutColor[px] = IsSet(FLAGS_DIFFUSE_SIGNAL_INDIRECT)
                        ? half4(1.0f, 0.0f, 1.0f, 1.0f)
                        : half4(
                            InDirectDiffuse[px].rgb * InDiffuseAlbedo[px].rgb,
                            1.0f);
                    break;
                case FLAGS_DEBUG_INDIRECT_DIFFUSE:
                    OutColor[px] = IsSet(FLAGS_DIFFUSE_SIGNAL_INDIRECT)
                        ? half4(
                            InDirectDiffuse[px].rgb * InDiffuseAlbedo[px].rgb,
                            1.0f)
                        : half4(1.0f, 0.0f, 1.0f, 1.0f);
                    break;
                case FLAGS_DEBUG_HANDOVER_RR_BAND:
                case FLAGS_DEBUG_HANDOVER_DETAIL_BAND:
                case FLAGS_DEBUG_HANDOVER_BAND_MIX:
                {
                    // Handover band inspection. Only handed-over pixels have a split
                    // at all, so everything else stays black and the affected
                    // surfaces are unambiguous.
                    const half4 handover = g_Handover[smID.x][smID.y];
                    half3 debugColor = 0.0h;

                    [branch]
                    if (saturate(handover.a) > 0.0h)
                    {
                        const HandoverBands bands =
                            GetHandoverBands(smID, handover.rgb);

                        const float lowLuma = GetLuminance(bands.RRLowBand);
                        const float detailLuma = GetLuminance(bands.DetailHigh);

                        if (GetDebugMode() == FLAGS_DEBUG_HANDOVER_RR_BAND)
                        {
                            // RR's contribution on its own: the base the detail is
                            // grafted onto. Soft by nature - that is the point.
                            debugColor = GetSafeFP16(max(bands.RRLowBand, 0.0f));
                        }
                        else if (GetDebugMode() == FLAGS_DEBUG_HANDOVER_DETAIL_BAND)
                        {
                            // The handover's contribution, which is signed - a dark
                            // stroke on a bright panel is negative. Mid grey is zero,
                            // brighter is positive, darker is negative. Scaled
                            // against local brightness so it reads the same in HDR as
                            // in a dim scene.
                            const float rcpLocal = rcp(max(lowLuma, 1e-3f));
                            debugColor = GetSafeFP16(saturate(
                                0.5f + bands.DetailHigh * rcpLocal * 0.5f));
                        }
                        else
                        {
                            // Detail strength relative to local brightness.
                            //
                            // Comparing the bands' raw magnitudes is misleading: the
                            // low band carries the panel's DC level while the detail
                            // band carries only its variation, so their ratio is
                            // driven by how bright the panel is and reads near-zero
                            // even where the detail is doing real work. What matters
                            // is detail as a fraction of the level it modulates.
                            //
                            // Full red is s_MixFullScale, i.e. detail swinging that
                            // fraction of local brightness - strong contrast for a
                            // stroke edge. Blue is a pixel the detail band barely
                            // touches, which is what flat panel interior should be.
                            static const float s_MixFullScale = 0.5f;
                            const float relativeDetail =
                                abs(detailLuma) * rcp(max(abs(lowLuma), 1e-3f));
                            debugColor = (half3) TurboColormap(
                                saturate(relativeDetail * rcp(s_MixFullScale)));
                        }
                    }

                    OutColor[px] = half4(GetSafeFP16((float3) debugColor), 1.0f);
                    break;
                }

                case FLAGS_DEBUG_HANDOVER_ANCHOR:
                {
                    // How far the RR anchor had to move the handover, as a fraction
                    // of its own tolerance.
                    //
                    // Black is untouched - the value was already inside RR's local
                    // distribution, which is the case sharpness depends on, so a
                    // healthy panel is mostly black with warm specks where genuine
                    // outliers were pulled in. A panel that is warm everywhere means
                    // the anchor is overriding the handover rather than stabilising
                    // it, and the tolerance is set too tight for this content.
                    const half4 handover = g_Handover[smID.x][smID.y];
                    half3 debugColor = 0.0h;

                    [branch]
                    if (saturate(handover.a) > 0.0h && FloorHandoverAnchorClamp > 0.0f)
                    {
                        const HandoverBands bands =
                            GetHandoverBands(smID, handover.rgb);

                        const float3 preClamp =
                            max(bands.RRLowBand + bands.DetailHigh, 0.0f);
                        const float3 tolerance =
                            FloorHandoverAnchorClamp * bands.RRDeviation;
                        const float3 postClamp = clamp(
                            preClamp,
                            max(bands.RRLowBand - tolerance, 0.0f),
                            bands.RRLowBand + tolerance);

                        const float displacement =
                            abs(GetLuminance((float3) (postClamp - preClamp))) *
                            rcp(max(GetLuminance((float3) tolerance), 1e-3f));
                        debugColor = displacement > 1e-4f
                            ? (half3) TurboColormap(saturate(displacement))
                            : 0.0h;
                    }

                    OutColor[px] = half4(GetSafeFP16((float3) debugColor), 1.0f);
                    break;
                }

                case FLAGS_DEBUG_HANDOVER_WEIGHT:
                {
                    // The weight the handover actually ends up with once the
                    // agreement mix has modulated it - what the mix does, rather than
                    // what it measures.
                    //
                    // Red is a pixel taken entirely from the handover, blue one left
                    // to RR. With the mix off this is flat red across every type-1
                    // surface; as it rises, the pixels where the two paths already
                    // agree fall away to blue because RR wins those on stability.
                    const half4 handover = g_Handover[smID.x][smID.y];
                    half weight = saturate(handover.a);

                    [branch]
                    if (FloorHandoverCorrelationMix > 0.0f && weight > 0.0h)
                    {
                        const half agreement = GetHandoverAgreement(gtID.xy);
                        weight *= lerp(
                            1.0h, 1.0h - agreement,
                            half(saturate(FloorHandoverCorrelationMix)));
                    }

                    const half3 debugColor = saturate(handover.a) > 0.0h
                        ? (half3) TurboColormap(saturate(float(weight)))
                        : 0.0h;
                    OutColor[px] = half4(GetSafeFP16((float3) debugColor), 1.0f);
                    break;
                }

                default:
                    OutColor[px] = half4(
                        InIndirectSpecular[px].rgb + InDirectDiffuse[px].rgb, 1.0f);
                    break;
            }    
        }
        else
        {
            const half4 denoisedColor = g_DenoisedColor[smID.x][smID.y];
            const half4 rawColor = g_RawColor[smID.x][smID.y];
            half3 outColor = GetSafeFP16(lerp(denoisedColor.rgb, rawColor.rgb, baseRawWeight));
            
            // Clamp final color within +/- 50% of the denoiser output. The SSIM metric generally stays well 
            // clear if this threshold, but not always.
            const half3 minColor = half3(0.5f * denoisedColor.rgb);
            const half3 maxColor = half3(1.5f * denoisedColor.rgb);
            outColor.rgb = clamp(outColor.rgb, minColor, maxColor);

            // Handover combination. Both sides are finished images by this point -
            // RR's result has been remodulated, correlated against raw and clamped,
            // and the handover carries its own composed colour - so this is a mix of
            // two complete frames rather than of two partial signals.
            const half4 handover = g_Handover[smID.x][smID.y];
            half3 handoverColor = handover.rgb;
            half handoverWeight = saturate(handover.a);

            // The three refinements below all need RR's local distribution, so the
            // 5x5 pass is shared rather than repeated per feature.
            const bool needsRRStatistics = handoverWeight > 0.0h;

            [branch]
            if (needsRRStatistics)
            {
                // Frequency split rather than a ratio mix.
                //
                // Blending two whole images trades their strengths against each other:
                // RR is blurry but temporally informed, the handover is sharp but
                // spatially filtered only, and any ratio gives a fraction of each
                // fault. They fail in different bands though. RR's blur is invisible
                // below the split frequency and fatal above it; the handover's
                // shrinkage is tuned for exactly the band where text lives.
                //
                // So take the low band from RR - where its temporal information is
                // real and its softness costs nothing - and the high band from the
                // handover, where the strokes are.
                const HandoverBands bands = GetHandoverBands(smID, handover.rgb);

                // The high band is signed, so the sum can undershoot where a dark
                // stroke sits over a darker RR low band.
                handoverColor = GetSafeFP16(max(bands.RRLowBand + bands.DetailHigh, 0.0f));

                [branch]
                if (FloorHandoverAnchorClamp > 0.0f)
                {
                    // RR-anchored clamp.
                    //
                    // The handover path is purely spatial, so it filters an
                    // independent noise realization every frame and flickers even on
                    // a static scene. Nothing in the blend modes can fix that: a
                    // ratio of two images inherits the instability of whichever one
                    // is unstable.
                    //
                    // RR's output is temporally stable, so bound the handover to RR's
                    // own local distribution. Values already inside that range pass
                    // untouched, which is why sharpness survives - the clamp only
                    // moves what RR's neighbourhood does not support. This is TAA
                    // history rectification, applied across paths instead of frames.
                    const float3 tolerance = FloorHandoverAnchorClamp * bands.RRDeviation;

                    handoverColor = GetSafeFP16(clamp(
                        float3(handoverColor),
                        max(bands.RRLowBand - tolerance, 0.0f),
                        bands.RRLowBand + tolerance));
                }
            }

            [branch]
            if (FloorHandoverCorrelationMix > 0.0f && handoverWeight > 0.0h)
            {
                // One global ratio is the wrong instrument, because whether RR damaged
                // a pixel is a per-pixel fact. Where the two paths agree structurally
                // the choice is moot and RR is the better pick for its stability;
                // where they diverge, RR removed something the handover kept, which is
                // exactly the case the handover exists to cover.
                const half agreement = GetHandoverAgreement(gtID.xy);
                handoverWeight *= lerp(
                    1.0h, 1.0h - agreement, half(saturate(FloorHandoverCorrelationMix)));
            }

            outColor.rgb = lerp(outColor.rgb, handoverColor, handoverWeight);
            
            OutColor[px] = (half4)GetSafeFP16(float4(outColor, 1.0f));
        }
    }
}
