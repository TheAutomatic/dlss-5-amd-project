#include "FSRDPreprocessCommon.hlsli"

#define MainRS \
    "RootFlags(0), " \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 4), visibility = SHADER_VISIBILITY_ALL), " \
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

static const uint2 s_ThreadGroupSize = uint2(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y);

// A-Trous kernel config
#define KERNEL_SIZE             3
#define KERNEL_RANGE_MIN        (-KERNEL_SIZE / 2)
#define KERNEL_RANGE_MAX        (KERNEL_SIZE / 2)

static const float s_Kernel1D[2] = { 0.44198f, 0.27901f };

Texture2D<half4> InColor : register(t0);
Texture2D<float> InLinearDepth : register(t1);

// RG: View space depth gradient, BA: Octahedrally encoded world normal
Texture2D<half4> InDepthGradient : register(t2);

// RGB: Diffuse albedo. The material guide - see GetAlbedoAgreement.
Texture2D<half3> InDiffAlbedo : register(t3);

RWTexture2D<half4> OutColor : register(u0);

SamplerState LinearSampler : register(s0);

cbuffer CB_Analysis : register(b0)
{
    float4 DstTexSize;

    float RcpCrossBlNorm;
    float RcpSelfBlNorm;
    
    int StepSize;
    uint FrameIndex;
    

    // Fraction of the Laplacian luminance residual re-injected into the floor.
    // 0 reproduces the previous behaviour exactly. Only non-zero on the final pass.
    float DetailBoost;

    // Exponent on the normal edge-stopping weight. Higher stops harder at creases.
    float NormalSharpness;

    // Fraction of the luminance edge stop released where diffuse albedo says the taps sit
    // on the same material. 0 reproduces the previous behaviour exactly.
    float AlbedoGuideStrength;

    // Blends the luminance normaliser from centre-only (0) to max(centre, tap) (1).
    float LumSymmetry;

    // Additional normal edge-stop exponent applied in proportion to screen space slope.
    float GrazingSharpness;

    // How far each pass returns a downward-biased estimate instead of the bilateral mean.
    // See the envelope blend at the end of CSMain.
    float EnvelopeBias;

    // Origin of the title's diffuse albedo subrect. Every other input here is an internal
    // zero-based buffer, but the material guide is the title's own texture, which is bound
    // whole - so without this the guide is read from wherever the texture starts rather than
    // from the region being rendered, and a title with a non-zero subrect gets its material
    // edges from the wrong pixels. Same origin the conversion pass uses (InputBase2.zw).
    uint2 AlbedoBase;
}


// Ceiling on the plane extrapolation, as a fraction of the centre depth. The gradient is a
// one pixel central difference, so extrapolating it over a 16 pixel stride is only
// trustworthy up to a point; past this the tap is treated as off-plane.
static const float s_MaxPlaneOffset = 0.5f;

// Floor on the luminance range norm scale under full albedo agreement. The appearance term
// is widened, never switched off, so a genuine outlier tap is still rejected.
static const float s_MinLumScale = 0.1f;

float GetSpatialWeight(int x, int y)
{
    return s_Kernel1D[abs(x)] * s_Kernel1D[abs(y)];
}

float GetRangeWeight(float delta, float scale)
{
    // W = ( 1 - ( (center - tap) * scale )^2 )^2
    // scale = 1 / norm
    return Square(max(1.0f - Square(delta * scale), 1e-2f));
}

[RootSignature(MainRS)]
[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void CSMain(uint3 groupID : SV_GroupID, uint3 gtID : SV_GroupThreadID)
{
    const int2 px = groupID.xy * s_ThreadGroupSize + gtID.xy;
    
    if (px.x >= DstTexSize.x || px.y >= DstTexSize.y)
        return;
    
    const float4 centerColor = InColor[px];    
    const float centerLum = GetLuminance(centerColor.rgb);
    // The albedo is the title's texture, so its subrect origin applies here the way it does in
    // the conversion pass. The taps below are clamped to the render bounds, which the feature
    // has already validated to lie inside the albedo subrect, so adding the origin keeps every
    // read in range without a second clamp.
    const int2 albedoPx = px + int2(AlbedoBase);
    const float3 centerAlbedo = InDiffAlbedo[albedoPx];

    // Albedo carries no material information where it is near black - unlit billboards, very
    // dark paint, blended transparents whose albedo mixes two surfaces. Relaxing the
    // luminance stop there would be relaxing it on no evidence, so confidence gates the whole
    // term and the filter falls back to its previous behaviour.
    const float albedoConfidence = SoftAbove(GetLuminance(centerAlbedo), 0.02f, 0.015f);
    const float guideStrength = AlbedoGuideStrength * albedoConfidence;
    
    const float centerDepth = InLinearDepth[px];
    const half4 centerGuide = InDepthGradient[px];
    const float2 centerDepthGrad = centerGuide.xy;
    const float3 centerNormal = OctahedralDecode(centerGuide.zw);

    // As the scaling increases, bilateral weighting becomes stricter. As smoothness increases,
    // blur strength should decrease. Where smoothness remains low, the weights should allow
    // more blending.
    //
    // StepSize scaling keeps luma strictness consistent as the stride increases.
    const float smoothness = saturate(1.0f - 2.0f * centerColor.a);
    const float adaptiveScale = float(StepSize) * (1.0f + 2.0f * smoothness);
    const float selfNormScale = adaptiveScale * RcpSelfBlNorm;

    // Depth tolerance is deliberately stride-independent. The tap test below measures
    // distance from the center pixel's tangent plane, not raw depth difference, so the
    // first-order change across the tap offset is already predicted and a coplanar tap
    // scores the same residual at every stride.
    //
    // The old form wrote this as StepSize * ... * rcp(... * StepSize), which cancels to
    // the same value. That cancellation was never the defect: it is the correct
    // behaviour for a plane-relative test. The defect was that the plane term was
    // missing, so the outer passes compared a stride-16 depth difference against a
    // stride-1 tolerance and rejected nearly every tap on sloped geometry.
    const float depthNormScale =
        (1.0f + 2.0f * smoothness) * RcpCrossBlNorm * rcp(1.0f + abs(centerDepth));

    // View depth is signed, so every scale taken from it uses a magnitude: with a negative
    // centre depth the plane clamp would invert and the slope term would saturate.
    const float centerDepthMagnitude = max(abs(centerDepth), 1e-2f);
    const float maxPlaneOffset = s_MaxPlaneOffset * centerDepthMagnitude;

    // Grazing incidence, measured as screen space surface slope.
    //
    // The gradient is a one pixel central difference, so on a steeply slanted surface the
    // kernel spans a large depth range and the plane extrapolation is least reliable exactly
    // where it is asked to reach furthest. Tightening the orientation stop in proportion to
    // slope is the geometric counterweight.
    const float slope = length(centerDepthGrad) * rcp(centerDepthMagnitude);
    const float grazing = saturate(slope * 64.0f);
    const float normalSharpness = NormalSharpness + GrazingSharpness * grazing;
    
    const int2 maxBounds = int2(DstTexSize.xy) - 1;
    float4 mean = 0;
    float3 envelope = 0;
    float totalWeight = 0;
    
    [unroll]
    for (int x = KERNEL_RANGE_MIN; x <= KERNEL_RANGE_MAX; x++)
    {
        [unroll]
        for (int y = KERNEL_RANGE_MIN; y <= KERNEL_RANGE_MAX; y++)
        {
            const bool isTap = (x != 0 || y != 0);
            const int2 tapOffset = StepSize * int2(x, y);
            const int2 tapPX = clamp(px + tapOffset, 0, maxBounds);
            const float4 color = isTap ? InColor[tapPX] : centerColor;
            const float lum = isTap ? GetLuminance(color.rgb) : centerLum;

            // Bilateral luma weight
            //
            // Normalising by the centre luminance alone makes the test asymmetric: a dark
            // centre beside a bright tap stops hard while the bright centre looking back at
            // the same pair blurs freely, so the two sides of one luminance edge are routed
            // differently - one into the skip path, one into the denoiser.
            const float lumNorm = max(lerp(centerLum, max(centerLum, lum), LumSymmetry), 1e-1f);
            const float lumDelta = (centerLum - lum) * rcp(lumNorm);

            // Material guide.
            //
            // Where albedo says the taps sit on the same material, a luminance difference
            // between them is illumination - a shadow, or a reflection - and it belongs in the
            // denoiser rather than being preserved into the floor and returned blurred through
            // the skip signal. The depth and orientation weights still multiply in below, so
            // this can never blur across a crease or a silhouette; it only releases the
            // appearance term.
            const float3 tapAlbedo = isTap ? (float3) InDiffAlbedo[tapPX + int2(AlbedoBase)] : centerAlbedo;
            const float agreement = isTap ? GetAlbedoAgreement(centerAlbedo, tapAlbedo) : 1.0f;
            const float lumRelax = lerp(1.0f, s_MinLumScale, guideStrength * agreement);
            const float wLum = GetRangeWeight(lumDelta, selfNormScale * lumRelax);

            // Coplanarity weight. FloorSeed stores a per-pixel central difference of
            // view-space depth, so scaling it by the tap displacement predicts the depth
            // this tap would carry if the surface were locally planar. Measuring the
            // residual against that plane is what makes this a coplanarity test rather than a
            // plain depth difference, and it is why the gradient is produced at all.
            // The displacement is taken from the clamped position, not the kernel offset:
            // a border tap clamped back onto the centre reads the centre's depth, so
            // predicting from the unclamped stride would score that read against a plane
            // point that was never sampled and skew the edge weights on sloped surfaces.
            const float depth = InLinearDepth[tapPX];
            const float planeOffset = clamp(dot(centerDepthGrad, float2(tapPX - px)),
                                            -maxPlaneOffset, maxPlaneOffset);
            const float depthDelta = (centerDepth + planeOffset) - depth;
            const float wDepth = GetRangeWeight(depthDelta, depthNormScale);

            // Orientation weight.
            //
            // Stops the kernel at creases and silhouettes that the plane test cannot see,
            // which is what previously forced the depth and luma norms to stay tight.
            const float3 tapNormal = OctahedralDecode(InDepthGradient[tapPX].zw);
            const float wNormal = isTap ? GetNormalWeight(centerNormal, tapNormal, normalSharpness) : 1.0f;

            const float wSpatial = GetSpatialWeight(x, y);
            const float w = wSpatial * wDepth * wLum * wNormal;

            mean += w * color;
            // The tap clamped below the centre, accumulated with the same weights. Every term is
            // bounded by the centre, so this sum is too, and it stays an average of the kernel's
            // lower side rather than a rank filter. The alpha is carried through untouched: the
            // instability the seed published is a separate channel with its own contract.
            envelope += w * min(color.rgb, centerColor.rgb);
            totalWeight += w;
        }
    }

    mean *= rcp(max(totalWeight, 1e-2f));
    envelope *= rcp(max(totalWeight, 1e-2f));
    // Envelope bias, and it is off by default.
    //
    // The seed's floor is already a lower estimate, but averaging lets it drift above the raw
    // colour on some pixels, where it then takes the pixel over: the residual collapses to zero
    // and the skip signal publishes this filter's own low pass. Blending toward the
    // clamped-below-centre average bounds each pass's output by its own input, so by induction
    // the floor can no longer exceed the raw colour.
    //
    // Measured on a real frame, the crossing is about 0.1% of pixels, with spikes where detail
    // boost lifts the floor; the softness this was written for came from something else. See the
    // envelope-bias entry in docs/fsrd_pipeline_contract.md.
    mean.rgb = lerp(mean.rgb, envelope, saturate(EnvelopeBias));

    // Microcontrast restoration.
    //
    // The floor is subtracted from the raw colour to form the denoiser input, and the
    // remainder travels around the denoiser in the skip signal. Pushing part of the high
    // frequency residual back into the floor therefore routes texture detail through the skip
    // path untouched rather than through the denoiser, which is what attenuates it.
    // DetailBoost is zero on every pass but the last.
    const float meanLum = GetLuminance(mean.rgb);
    const float residualLum = centerLum - meanLum;
    const float3 chroma = mean.rgb * rcp(max(meanLum, 1e-3f));

    float4 outColor = mean;
    outColor.rgb = max(mean.rgb + (DetailBoost * residualLum) * chroma, 0.0f);

    OutColor[px] = GetSafeFP16(outColor);
}
