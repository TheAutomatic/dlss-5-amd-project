#include "FSRDPreprocessCommon.hlsli"
#include "FSRDFloorCommon.hlsli"

#define MainRS                                                                                                         \
    "RootFlags(0), CBV(b0), "                                                                                          \
    "DescriptorTable(SRV(t0, numDescriptors = 17)), "                                                                  \
    "DescriptorTable(UAV(u0, numDescriptors = 3)), "                                                                   \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, "                                                           \
    "addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, addressW = TEXTURE_ADDRESS_CLAMP)"

#define FLAGS_RAW_SOURCE_BLIT (1 << 0)
#define FLAGS_SCALE_SRC (1 << 1)
#define FLAGS_DIFFUSE_SIGNAL_INDIRECT (1 << 2)
#define FLAGS_SPECULAR_SIGNAL_INDIRECT (1 << 3)
#define FLAGS_DEBUG (1 << 16)
#define FLAGS_DEBUG_MODE_MASK (0xFF << 16)
#define FLAGS_DEBUG_DETAIL_CONFIDENCE (1 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_SKIP_SIGNAL (2 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DENOISER_OUTPUT (3 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DIRECT_SPECULAR (4 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DIRECT_DIFFUSE (5 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_INDIRECT_DIFFUSE (6 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DETAIL_CORRECTION (7 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DETAIL_REFERENCE (8 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_INDIRECT_SPECULAR (12 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_RECONSTRUCTED_COLOR (13 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DETAIL_SEED (14 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DETAIL_ANCHORED (15 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_HANDOVER_WEIGHTS (16 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_HANDOVER_LIMITS (17 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_COMPOSITION_BEFORE_CLAMP (18 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_COMPOSITION_FINAL (19 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DETAIL_BOX_ANCHORED (20 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_HANDOVER_ELIGIBILITY (21 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_CHROMA_RECOVERY (22 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_LUMA_RECOVERY (23 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_ALBEDO_TRUST (24 << 17 | FLAGS_DEBUG)

Texture2D<half4> InIndirectSpecular : register(t0);
Texture2D<half4> InSpecularAlbedo : register(t1);
Texture2D<half4> InDirectDiffuse : register(t2);
Texture2D<half4> InDiffuseAlbedo : register(t3);
Texture2D<half4> InSkipSignal : register(t4);
Texture2D<half4> InNormals : register(t5);
Texture2D<half4> InDetailReference : register(t6);
Texture2D<float> InLinearDepth : register(t7);
Texture2D<half4> InMotion : register(t8);
Texture2D<half4> InDecisionHistory : register(t9);
Texture2D<uint4> InHistoryMetadata : register(t10);
// Unsupported-albedo recovery: RR direct-specular output of the unmodulated specular
// share, its conversion input (A: eligibility) and the propagated evidence votes.
Texture2D<half4> InDirectSpecularDenoised : register(t11);
Texture2D<half4> InDirectSpecularSignal : register(t12);
Texture2D<float2> InAlbedoTrust : register(t13);
Texture2D<half4> InIndirectDiffuseDenoised : register(t14);
// The main specular and diffuse RR inputs, still unmodified after RR. Recovery rebuilds from
// them how much energy each lobe's demodulation could not represent and left in Skip.
Texture2D<half4> InIndirectSpecularSignal : register(t15);
Texture2D<half4> InDirectDiffuseSignal : register(t16);
#define FLAGS_EXTRA_DIFFUSE (1 << 6)
#define FLAGS_EXTRA_SPECULAR (1 << 7)
// InIndirectDiffuseDenoised holds RR's denoising of the unmodulated diffuse share.
#define FLAGS_DIFFUSE_ALTERNATE (1 << 8)
#define FLAGS_DIFFUSE_SIGNAL_DISABLED (1 << 4)
#define FLAGS_SPECULAR_SIGNAL_DISABLED (1 << 5)
RWTexture2D<half4> OutColor : register(u0);
// FP32 UAV stores convert to the unchanged RGBA16_FLOAT resource format.
RWTexture2D<float4> OutDecisionHistory : register(u1);
RWTexture2D<uint4> OutHistoryMetadata : register(u2);
SamplerState LinearSampler : register(s0);

cbuffer CB_Comp : register(b0)
{
    float4 DstTexSize;
    uint Flags;
    float DetailPreservation;
    uint RecoveryMask;
    float FloorHandoverAnchorClamp;
    float2 SourceUvScale;
    float2 SourceUvOffset;
    float FloorHandoverCorrelationMix;
    uint HistoryValid;
    float2 HistoryJitterDelta;
    uint WriteHistory;
    float SpecularAlbedoDemodulation;
    float DiffuseAlbedoModulation;
    uint SpatialTemporalMask;
    float LumaRecovery;
    float ChromaRecovery;
    float UnsupportedAlbedoRecovery;
    float DemodDivisorFloor;
}

// 0 keeps the complete Anchor/Light/debug program. Runtime selects variants 1/2
// only after proving that the removed path cannot contribute to this dispatch.
#ifndef FSRD_COMPOSITION_VARIANT
#define FSRD_COMPOSITION_VARIANT 0
#endif
#if FSRD_COMPOSITION_VARIANT < 0 || FSRD_COMPOSITION_VARIANT > 4
#error Invalid FSRD composition variant
#endif

#define THREAD_GROUP_SIZE_X 8
#define THREAD_GROUP_SIZE_Y 8
#define NUM_THREADS 64
static const uint2 s_ThreadGroupSize = uint2(8, 8);
// Regional radius four; no patch search or reference filtering. Every lane loads before the
// barrier, including lanes outside the logical extent of a partial group.
DEFINE_LDS_CONFIG(s_SM, 9);
// Rows first: adjacent X lanes must not stride an entire tile in LDS.
// One padding column also separates the banks used by consecutive rows.
#if FSRD_COMPOSITION_VARIANT == 0 || FSRD_COMPOSITION_VARIANT == 4
groupshared half3 g_RR[16][17];
groupshared half4 g_Reference[16][17];
groupshared float g_Depth[16][17];
groupshared half3 g_Normal[16][17];
groupshared half3 g_Albedo[16][17];
groupshared float3 g_Blurred[16][17];
groupshared float g_QuietPair[16][17];
groupshared uint g_VaryingGuides;
#endif
#if FSRD_COMPOSITION_VARIANT != 2
groupshared uint g_HasHandover;
// The light path needs only a radius-one RR neighbourhood. Keep it in FP32:
// reusing the half-precision handover tile would quantize the recovery input.
groupshared float3 g_LightRR[10][11];
#endif
#if FSRD_COMPOSITION_VARIANT == 0 || FSRD_COMPOSITION_VARIANT == 4
groupshared float4 g_RegionA[16][9];
groupshared float4 g_RegionB[16][9];
groupshared float4 g_RegionC[16][9];
#endif

// Uniform material guides on planar display surfaces are common. Only FP32-scale
// depth roundoff is ignored; normal length remains the actual decoded FP16 value.
float CompositionSurfaceWeight(float z, float tapZ, float2 gradient, float2 offset, float3 n, float3 tapN, float3 a,
                               float3 tapA, uint guideFlags)
{
    if (!isfinite(z) || !isfinite(tapZ) || z * tapZ <= 0)
        return 0;
    float depthWeight = 1;
    if ((guideFlags & 1u) != 0)
    {
        const float prediction = clamp(dot(gradient, offset), -0.25f * abs(z), 0.25f * abs(z));
        depthWeight = Square(saturate(1.0f - abs(z + prediction - tapZ) / max(0.01f * abs(z), 1e-3f)));
    }
    const float normalWeight = Square(saturate((dot(n, tapN) - 0.9f) * 10.0f));
    const float materialWeight = (guideFlags & 2u) != 0 ? FloorMaterialWeight(a, tapA) : 1.0f;
    return depthWeight * normalWeight * materialWeight;
}

static const uint kQuartileNetworkSize = 93;
static const uint QuartileNetwork[2 * kQuartileNetworkSize] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 0,  2,  1,
    3,  4,  6,  5,  7,  8,  10, 9,  11, 12, 14, 13, 15, 16, 18, 17, 19, 20, 22, 21, 24, 0,  4,  1,  5,  2,  6,
    3,  7,  8,  12, 9,  13, 10, 14, 11, 15, 16, 20, 21, 22, 23, 24, 0,  8,  1,  12, 2,  10, 3,  14, 4,  9,  5,
    13, 6,  11, 7,  15, 17, 22, 18, 21, 19, 24, 1,  18, 3,  9,  5,  17, 6,  20, 7,  13, 11, 14, 12, 22, 21, 23,
    1,  16, 3,  12, 5,  21, 6,  18, 7,  11, 10, 17, 14, 23, 19, 20, 0,  1,  2,  5,  4,  16, 6,  8,  7,  18, 9,
    21, 10, 14, 12, 19, 1,  2,  3,  5,  4,  6,  7,  9,  8,  12, 10, 16, 1,  4,  2,  6,  3,  7,  5,  9,  8,  10,
    12, 16, 2,  4,  3,  8,  5,  10, 7,  12, 3,  4,  5,  8,  6,  7,  5,  6,  7,  8,  4,  5,  6,  7
};

bool IsSet(uint mask) { return (Flags & mask) == mask; }
uint GetDebugMode() { return Flags & FLAGS_DEBUG_MODE_MASK; }
float3 SpecularMultiplier(int2 p)
{
    return lerp(1.0f, float3(InSpecularAlbedo[p].rgb), saturate(SpecularAlbedoDemodulation));
}
float3 DiffuseMultiplier(int2 p)
{
    return lerp(1.0f, float3(InDiffuseAlbedo[p].rgb), saturate(DiffuseAlbedoModulation));
}
// The demodulated diffuse lobe conversion produced. Two diffuse signals each received half.
float3 DiffuseSignalInput(int2 p)
{
    return float3(InDirectDiffuseSignal[p].rgb) * (IsSet(FLAGS_EXTRA_DIFFUSE) ? 2.0f : 1.0f);
}
// Blend weight toward the unmodulated specular path: how much of the nearby same-surface
// albedo structure the light does not show (see FSRDAlbedoTrustEvidence/Propagate). Zero
// where conversion excluded the pixel or where the surface has no albedo structure at all.
float UnsupportedAlbedoWeight(int2 p)
{
    if (UnsupportedAlbedoRecovery <= 0.0f || InDirectSpecularSignal[p].a < 0.5f)
        return 0.0f;
    const float2 votes = InAlbedoTrust[p];
    const float weight = saturate(UnsupportedAlbedoRecovery) *
        smoothstep(0.4f, 0.8f, votes.x / max(votes.y, 1e-3f)) * smoothstep(0.5f, 3.0f, votes.y);
    // A lobe clamped at the FP16 limit left an unknown amount of energy in Skip, so its
    // specular part cannot be rebuilt. Such isolated highlights keep path A. GetSafeFP16's
    // 65500 ceiling is stored as 65472.
    [branch]
    if (weight > 0.0f && (any(float3(InIndirectSpecularSignal[p].rgb) >= 65472.0f) ||
                          any(DiffuseSignalInput(p) >= 65472.0f)))
        return 0.0f;
    return weight;
}
// The specular part of Skip. Below the demodulation divisor floor (an albedo channel under
// DemodDivisorFloor) conversion cannot represent a lobe's full energy and leaves the rest in
// Skip; that loss belongs to its own lobe, not to the albedo ratio. Rebuild each lobe's loss
// from its RR input exactly as conversion divided it; the remaining Skip (the Floor base)
// was split by the albedo ratio, like the unmodulated alternate.
float3 SpecularSkip(int2 p, float3 spec, float3 diff, float3 share)
{
    const float3 specLoss = float3(InIndirectSpecularSignal[p].rgb) * (max(max(spec, DemodDivisorFloor), 1e-4f) - spec);
    const float3 diffLoss = DiffuseSignalInput(p) * (max(max(diff, DemodDivisorFloor), 1e-4f) - diff);
    return (float3(InSkipSignal[p].rgb) - specLoss - diffLoss) * share + specLoss;
}
float3 SpecularRadiance(int2 p)
{
    return float3(InIndirectSpecular[p].rgb) +
        (IsSet(FLAGS_EXTRA_SPECULAR) ? float3(InDirectSpecularDenoised[p].rgb) : 0.0f);
}
float3 DiffuseRadiance(int2 p)
{
    return float3(InDirectDiffuse[p].rgb) +
        (IsSet(FLAGS_EXTRA_DIFFUSE) ? float3(InIndirectDiffuseDenoised[p].rgb) : 0.0f);
}
// Replaces the specular part of the reconstruction - RR's remodulated specular plus the
// specular part of Skip - with RR's denoising of the same share never divided by albedo.
// With the diffuse alternate the diffuse part follows on mostly reflective surfaces: the
// diffuse albedo of water or glass is the surface seen through it, while on a diffuse-
// dominant surface it is the surface itself (real-RR replays: the stain edge drops by a
// further quarter to two fifths, with no change off the water).
float3 UnsupportedAlbedoCorrection(int2 p, float3 specular)
{
    const float weight = UnsupportedAlbedoWeight(p);
    if (weight <= 0.0f)
        return 0.0f;
    const float3 spec = InSpecularAlbedo[p].rgb, diff = InDiffuseAlbedo[p].rgb;
    const float3 total = spec + diff;
    const float3 splitT = saturate((total - 0.5f * DemodDivisorFloor) / (0.5f * DemodDivisorFloor));
    const float3 share = spec * rcp(max(total, DemodDivisorFloor)) * splitT * splitT * (3.0f - 2.0f * splitT);
    const float3 specularSkip = SpecularSkip(p, spec, diff, share);
    float3 correction = weight * (float3(InDirectSpecularDenoised[p].rgb) - (specular + specularSkip));
    [branch]
    if (IsSet(FLAGS_DIFFUSE_ALTERNATE))
    {
        const float reflective = smoothstep(0.5f, 0.7f, GetLuminance(spec) / max(GetLuminance(total), 1e-4f));
        // Skip less its specular part is the diffuse part, divisor-floor loss included.
        const float3 diffusePath = DiffuseRadiance(p) * DiffuseMultiplier(p) + float3(InSkipSignal[p].rgb) - specularSkip;
        correction += weight * reflective * (float3(InIndirectDiffuseDenoised[p].rgb) - diffusePath);
    }
    return correction;
}
float3 Reconstruct(int2 p)
{
    const float3 specular = SpecularRadiance(p) * SpecularMultiplier(p);
    float3 radiance = specular +
                      DiffuseRadiance(p) * DiffuseMultiplier(p) + float3(InSkipSignal[p].rgb);
    [branch]
    if (UnsupportedAlbedoRecovery > 0.0f)
        radiance += UnsupportedAlbedoCorrection(p, specular);
    return FloorRadiance(radiance);
}

// Albedo split is only an estimate when the title supplies combined colour.
// Keep the RGB ratio: a coloured metal can have a different share per channel.
void RecoveryWeights(int2 p, out float3 anchor, out float3 filtered)
{
    anchor = filtered = 0;
    const bool flat = (RecoveryMask & 1u) != 0 && abs(float(InNormals[p].a) - 1.0f / 3.0f) < 0.1f;
    if (flat)
    {
        if ((SpatialTemporalMask & 1u) != 0) filtered = 1;
        else anchor = 1;
        return;
    }
    if ((RecoveryMask & 6u) == 0) return;
    const float3 spec = InSpecularAlbedo[p].rgb, diff = InDiffuseAlbedo[p].rgb;
    const float3 sum = spec + diff;
    const float3 share = saturate(spec / max(sum, 1e-4f));
    // Missing material guides must not authorize arbitrary full-frame recovery.
    const float3 valid = float3(sum > 1e-4f);
    if ((RecoveryMask & 2u) != 0)
    {
        if ((SpatialTemporalMask & 2u) != 0) filtered += share * valid;
        else anchor += share * valid;
    }
    if ((RecoveryMask & 4u) != 0)
    {
        if ((SpatialTemporalMask & 4u) != 0) filtered += (1-share) * valid;
        else anchor += (1-share) * valid;
    }
}

// Diagnostic fraction of a candidate-to-RR difference removed by one limiter.
// Zero is no change; one means a change as large as the incoming difference.
// This is a bounded display metric, not a radiance or an energy-loss estimate.
float LimitFraction(float3 before, float3 after, float3 rr)
{
    return saturate(length(before - after) / max(length(before - rr), 1e-6f));
}

// Rotate the symmetric RGB covariance and its orthonormal basis together.
// Fixed-size Jacobi sweeps avoid choosing a privileged luminance/chroma axis:
// equal-luminance coloured lettering is structure, just as grey lettering is.
void RotateAnchorCovariance(inout float3x3 c, inout float3x3 basis, uint a, uint b)
{
    const float off = c[a][b];
    if (abs(off) <= max(c[0][0] + c[1][1] + c[2][2], 1e-20f) * 1e-6f)
        return;
    const float tau = (c[b][b] - c[a][a]) / (2.0f * off);
    const float t = (tau >= 0 ? 1.0f : -1.0f) / (abs(tau) + sqrt(1.0f + tau * tau));
    const float cosine = rsqrt(1.0f + t * t), sine = t * cosine;
    const float aa = c[a][a], bb = c[b][b];
    c[a][a] = aa - t * off;
    c[b][b] = bb + t * off;
    c[a][b] = c[b][a] = 0;
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        if (k != a && k != b)
        {
            const float ka = c[k][a], kb = c[k][b];
            c[k][a] = c[a][k] = cosine * ka - sine * kb;
            c[k][b] = c[b][k] = sine * ka + cosine * kb;
        }
        const float va = basis[k][a], vb = basis[k][b];
        basis[k][a] = cosine * va - sine * vb;
        basis[k][b] = sine * va + cosine * vb;
    }
}

float3 AnchorColour(float3 candidate, float3 mean, float3 variance, float3 crossVariance, float anchor)
{
    float3x3 covariance = float3x3(variance.x, crossVariance.x, crossVariance.y, crossVariance.x, variance.y,
                                   crossVariance.z, crossVariance.y, crossVariance.z, variance.z);
    float3x3 basis = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
    [unroll] for (uint sweep = 0; sweep < 3; ++sweep)
    {
        RotateAnchorCovariance(covariance, basis, 0, 1);
        RotateAnchorCovariance(covariance, basis, 0, 2);
        RotateAnchorCovariance(covariance, basis, 1, 2);
    }
    const float3 extent = anchor * sqrt(max(float3(covariance[0][0], covariance[1][1], covariance[2][2]), 0));
    const float3 local = mul(candidate - mean, basis);
    candidate = mean + mul(basis, clamp(local, -extent, extent));
    // Keep the original per-channel contract as well. Zero Anchor never calls
    // this function; constant RR therefore still has an exact constant anchor.
    const float3 tolerance = anchor * sqrt(variance);
    return clamp(candidate, max(mean - tolerance, 0), mean + tolerance);
}

// A per-centre 7x7 acceptance mask reuses exactly the same guide decision
// across the blur hypothesis and four directional noise pairs.
bool Accepted(uint2 mask, int2 o)
{
    const uint bit = uint((o.y + 3) * 7 + o.x + 3);
    return bit < 32 ? ((mask.x >> bit) & 1) != 0 : ((mask.y >> (bit - 32)) & 1) != 0;
}

bool BlurAccepted(uint2 mask, int2 o)
{
    // The same nine bits, tested together instead of nine dynamic shifts and
    // early branches for each of the 25 overlapping blur hypotheses.
    const uint pattern = 0x1c387u; // Three bits in each of three seven-bit rows.
    const uint shift = uint((o.y + 2) * 7 + o.x + 2);
    const uint lo = shift < 32 ? pattern << shift : 0u;
    const uint hi = shift == 32 ? pattern : (shift > 15 ? pattern >> (32-shift) : 0u);
    return (mask.x & lo) == lo && (mask.y & hi) == hi;
}

// Decisions and validated history share the existing metadata allocation.
// Round positive RGB to nearest; truncation would bias recursively filtered colour dark.
uint PackHistoryColour(float3 c)
{
    const uint3 v = min(f32tof16(FloorRadiance(c)) + uint3(8, 8, 16), uint3(0x7bf0, 0x7bf0, 0x7be0));
    return ((v.x >> 4) & 2047u) | (((v.y >> 4) & 2047u) << 11) | (((v.z >> 5) & 1023u) << 22);
}
float3 UnpackHistoryColour(uint v)
{
    return float3(f16tof32((v & 2047u) << 4), f16tof32(((v >> 11) & 2047u) << 4), f16tof32((v >> 22) << 5));
}
uint PackHistoryGuide(float4 n)
{
    const uint4 v = uint4(round(saturate(n) * float4(1023, 1023, 255, 3)));
    return v.x | (v.y << 10) | (v.z << 20) | (v.w << 28);
}
float4 UnpackHistoryGuide(uint v)
{
    return float4(v & 1023u, (v >> 10) & 1023u, (v >> 20) & 255u, (v >> 28) & 3u) / float4(1023, 1023, 255, 3);
}
uint PackHistoryAlbedo(float3 a)
{
    const uint3 v = uint3(round(saturate(a) * 255.0f));
    return v.x | (v.y << 8) | (v.z << 16);
}
float3 UnpackHistoryAlbedo(uint v) { return float3(v & 255u, (v >> 8) & 255u, (v >> 16) & 255u) / 255.0f; }
float4 LoadDecisionHistory(int2 p, float4 reference, out float reuse)
{
    reuse = 0;
    if (HistoryValid == 0 || reference.a < 0)
        return 0;
    const float4 motion = InMotion[p];
    if (motion.a < 0.5f || !all(isfinite(motion)))
        return 0;
    const float2 previous = float2(p) + 0.5f + motion.xy * DstTexSize.xy + HistoryJitterDelta;
    if (any(previous < 0) || any(previous >= DstTexSize.xy))
        return 0;
    // Point history jumped between unrelated decisions at subpixel pan boundaries.
    // Validate the entire bilinear footprint BEFORE combining decisions. Even if
    // its nearest tap matches, a footprint touching an occluder is not reusable.
    const float2 samplePosition = previous - 0.5f;
    const int2 origin = int2(floor(samplePosition));
    float2 fraction = frac(samplePosition);
    // Canonical motion is FP16. Sub-millipixel error around an integer must
    // not turn an exact reprojection into a second-surface footprint.
    fraction = float2(fraction.x < 0.001f ? 0 : (fraction.x > 0.999f ? 1 : fraction.x),
                      fraction.y < 0.001f ? 0 : (fraction.y > 0.999f ? 1 : fraction.y));
    const float4 n = InNormals[p];
    const float z = InLinearDepth[p], expectedZ = z + motion.z;
    float4 decisions = 0;
    float4 invalidDecisions = 0;
    float total = 0, acceptance = 1;
    [unroll]
    for (uint tap = 0; tap < 4; ++tap)
    {
        const int2 offset = int2(tap & 1, tap >> 1);
        const float weight = (offset.x ? fraction.x : 1-fraction.x)*
                             (offset.y ? fraction.y : 1-fraction.y);
        if (weight <= 1e-5f) continue;
        const int2 q = origin+offset;
        if (any(q < 0) || any(q >= int2(DstTexSize.xy))) return 0;
        const uint4 meta = InHistoryMetadata[q];
        if ((meta.y & 0x80000000u) == 0) return 0;
        const float4 oldN = UnpackHistoryGuide(meta.y);
        const float oldZ = asfloat(meta.x);
        if (!isfinite(oldZ) || !isfinite(z) || z * oldZ <= 0 ||
            abs(oldZ - expectedZ) > max(abs(expectedZ) * 0.01f, 0.002f) || abs(n.a - oldN.a) > 0.1f ||
            abs(n.z - oldN.z) > 0.04f || dot(OctahedralDecode(n.xy), OctahedralDecode(oldN.xy)) < 0.98f ||
            any(abs(float3(InDiffuseAlbedo[p].rgb) - UnpackHistoryAlbedo(meta.z)) > 0.025f)) return 0;
        const float4 oldDecisions = InDecisionHistory[q];
        if (!all(isfinite(oldDecisions))) return 0;
        // Stored colour is the reference the previous frame composited from;
        // a large change means the surface content itself moved on.
        const float3 oldColour = UnpackHistoryColour(meta.w);
        const float scale = max(max(length(oldColour), length(reference.rgb)), 1e-4f);
        const float change = length(oldColour-reference.rgb)/scale;
        acceptance = min(acceptance, 1.0f-smoothstep(0.025f,0.08f,change));
        invalidDecisions = max(invalidDecisions, float4(oldDecisions < 0));
        decisions += weight*max(oldDecisions,0);
        total += weight;
    }
    reuse = 0.5f*acceptance;
    return float4(invalidDecisions.x > 0 ? -1 : decisions.x/max(total,1e-5f),
                  invalidDecisions.y > 0 ? -1 : decisions.y/max(total,1e-5f),
                  invalidDecisions.z > 0 ? -1 : decisions.z/max(total,1e-5f),
                  invalidDecisions.w > 0 ? -1 : decisions.w/max(total,1e-5f));
}
float StableDecision(float current, float previous, float reuse)
{
    // Never turn a rejected current test into acceptance, or override hard gates.
    if (current <= 0 || previous < 0 || !isfinite(previous))
        return current;
    return lerp(current, clamp(previous, max(current - 0.125f, 0.0f), min(current + 0.125f, 1.0f)), reuse);
}

float3 FilterRecoveryDelta(float3 filtered, float3 rr, float noise)
{
    // Do not inject the remaining reference grain into an already clean RR result.
    // One RGB-ray factor avoids creating chromatic shifts with per-channel thresholds.
    const float3 delta = filtered - rr;
    const float magnitude = sqrt(dot(delta, delta) / 3.0f);
    const float support = saturate(1.0f - Square(0.9f * max(noise, 0.0f)) / max(magnitude*magnitude, 1e-12f));
    return support * delta;
}

// Lightweight Anchor/Correlation/Chroma/Luma recovery for lobe selections: a
// heavily optimized form of the handover Anchor method. The current-frame
// reference is the only detail source; it is never averaged with neighbours or
// previous frames, so animated radiance cannot be phase-averaged into blur and
// the first frame already behaves like the steady state.
//
// The neighbourhood supplies statistics only. RR's local mean and variance
// anchor the transferred colour: grain, coarse excursions and impulses that RR
// does not carry collapse back toward RR's value instead of being re-added.
// The correlation mix scales the transfer, with a measured-clean patch (quiet
// seed alpha and quiet colour pairs) as its explicit opt-out. Chroma and luma
// extensions restore contrast RR attenuated by amplifying RR's own local
// pattern, bounded by the anchored candidate. Four radius-three reference taps
// extend the structure test to coarse scales the 3x3 footprint cannot see;
// unlike the retired Spatial+Temporal wide pairs they never contribute colour.
// No history is read or written by this path.
#if FSRD_COMPOSITION_VARIANT != 2
float3 LightAnchorRecovery(int2 p, float4 reference, float3 rr, out float3 anchoredReference,
                           out float3 chromaCorrection, out float lumaCorrection)
{
    chromaCorrection = 0;
    lumaCorrection = 0;
    const int2 bounds = int2(DstTexSize.xy) - 1;
    const int2 local = (p & 7) + 1;
    const float z = InLinearDepth[p];
    const float3 n = OctahedralDecode(InNormals[p].xy);
    const float3 a = InDiffuseAlbedo[p].rgb;
    const float2 gradient = float2(
        FloorDepthDerivative(InLinearDepth[max(p - int2(1, 0), 0)], z, InLinearDepth[min(p + int2(1, 0), bounds)]),
        FloorDepthDerivative(InLinearDepth[max(p - int2(0, 1), 0)], z, InLinearDepth[min(p + int2(0, 1), bounds)]));
    float3 lowRR = 0, lowReference = 0;
    float3 rrCenteredMean = 0, rrCenteredSquare = 0;
    float3 refCenteredMean = 0, refCenteredSquare = 0;
    float3 residualX = 0, residualY = 0, residualXY = 0;
    float rrLumaMean = 0, refLumaMean = 0, rrLumaSquare = 0, refLumaSquare = 0, crossLuma = 0;
    float3 rrChromaMean = 0, refChromaMean = 0;
    float rrChromaSquare = 0, refChromaSquare = 0, crossChroma = 0;
    float noiseSquared = 0, rrDifference2 = 0, total = 0;
    float3 referenceMin = reference.rgb, referenceMax = reference.rgb;
    float quietPair = 1e20f, quietRunnerUp = 1e20f;
    uint pairCount = 0, quietNeighbours = 0;
    [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x)
    {
        const int2 q = clamp(p + int2(x, y), 0, bounds);
        const float4 tap = InDetailReference[q];
        const float3 tapRR = g_LightRR[local.y + y][local.x + x];
        float w = CompositionSurfaceWeight(z, InLinearDepth[q], gradient, float2(x, y), n,
                                           OctahedralDecode(InNormals[q].xy), a, InDiffuseAlbedo[q].rgb, 3u);
        w *= tap.a >= 0.0f ? 1.0f : 0.0f;
        w *= (x == 0 ? 2.0f : 1.0f) * (y == 0 ? 2.0f : 1.0f);
        const float3 centeredRR = tapRR - rr;
        const float3 centeredReference = float3(tap.rgb) - reference.rgb;
        const float lr = GetLuminance(centeredRR), lp = GetLuminance(centeredReference);
        rrCenteredMean += w * centeredRR;
        rrCenteredSquare += w * centeredRR * centeredRR;
        refCenteredMean += w * centeredReference;
        refCenteredSquare += w * centeredReference * centeredReference;
        // Three residual modes of a local quadratic fit over the reference:
        // straight edges and linear gradients cancel; random grain does not.
        residualX += w * x * (2 * y * y - 1) * centeredReference;
        residualY += w * y * (2 * x * x - 1) * centeredReference;
        residualXY += w * (2 * x * x - 1) * (2 * y * y - 1) * centeredReference;
        rrLumaMean += w * lr;
        refLumaMean += w * lp;
        rrLumaSquare += w * lr * lr;
        refLumaSquare += w * lp * lp;
        crossLuma += w * lr * lp;
        // Equal-luminance colour transitions are real structure too.
        const float3 cr = centeredRR - lr, cp = centeredReference - lp;
        rrChromaMean += w * cr;
        refChromaMean += w * cp;
        rrChromaSquare += w * dot(cr, cr) / 3.0f;
        refChromaSquare += w * dot(cp, cp) / 3.0f;
        crossChroma += w * dot(cr, cp) / 3.0f;
        lowRR += w * tapRR;
        lowReference += w * float3(tap.rgb);
        noiseSquared += w * max(tap.a, 0) * max(tap.a, 0);
        rrDifference2 += w * dot(float3(tap.rgb) - tapRR, float3(tap.rgb) - tapRR) / 3.0f;
        total += w;
        if (w > 0.1f)
        {
            referenceMin = min(referenceMin, float3(tap.rgb));
            referenceMax = max(referenceMax, float3(tap.rgb));
            if (x != 0 || y != 0)
            {
                // Quietest accepted colour pairs bound fine grain the seed
                // alpha missed. Track the two smallest: one lucky quiet
                // neighbour beside a glyph corner must not certify a patch.
                const float pair = sqrt(dot(centeredReference, centeredReference) / 3.0f);
                if (pair < quietPair)
                {
                    quietRunnerUp = quietPair;
                    quietPair = pair;
                }
                else if (pair < quietRunnerUp)
                    quietRunnerUp = pair;
                quietNeighbours += pair < 1e-3f ? 1u : 0u;
                ++pairCount;
            }
        }
    }
    lowRR /= max(total, 1e-5f);
    lowReference /= max(total, 1e-5f);
    // Gate-only coarse measurement: the widest same-surface excursion of the
    // current reference, at the scale of correlated lighting noise.
    float coarseExcursion = 0;
    [unroll] for (uint direction = 0; direction < 4; ++direction)
    {
        const int2 o = direction == 0 ? int2(3, 0) : direction == 1 ? int2(-3, 0) :
                       direction == 2 ? int2(0, 3) : int2(0, -3);
        const int2 q = clamp(p + o, 0, bounds);
        const float4 tap = InDetailReference[q];
        if (tap.a < 0.0f)
            continue;
        const float w = CompositionSurfaceWeight(z, InLinearDepth[q], gradient, float2(o), n,
                                                 OctahedralDecode(InNormals[q].xy), a, InDiffuseAlbedo[q].rgb, 3u);
        if (w <= 0.5f)
            continue;
        const float3 delta = float3(tap.rgb) - reference.rgb;
        coarseExcursion = max(coarseExcursion, sqrt(dot(delta, delta) / 3.0f));
    }
    const float alphaNoise = sqrt(noiseSquared / max(total, 1e-5f));
    const float mr = rrLumaMean / max(total, 1e-5f), mp = refLumaMean / max(total, 1e-5f);
    const float vr = max(rrLumaSquare / max(total, 1e-5f) - mr * mr, 0);
    const float vp = max(refLumaSquare / max(total, 1e-5f) - mp * mp, 0);
    const float cov = crossLuma / max(total, 1e-5f) - mr * mp;
    const float3 meanDelta = rrCenteredMean / max(total, 1e-5f);
    const float3 varianceRR = max(rrCenteredSquare / max(total, 1e-5f) - meanDelta * meanDelta, 0);
    const float3 refMeanDelta = refCenteredMean / max(total, 1e-5f);
    const float3 refVariance = max(refCenteredSquare / max(total, 1e-5f) - refMeanDelta * refMeanDelta, 0);
    const float amplitude = sqrt(dot(refVariance, 1.0f.xxx) / 3.0f);
    // A patch that measures clean under both independent estimates - quiet seed
    // alpha and genuinely quiet colour pairs - has no noise left to reject:
    // every noise proxy below yields to that direct measurement, the anchor
    // opens and the correlation opt-out keeps the transfer fully weighted.
    // Clean current-frame structure (sharp edges, lettering, flat regions
    // beside them) transfers unclamped; grain never has quiet pairs.
    const float quietTolerance = max(length(reference.rgb) * 0.001f, 1e-8f);
    // Five of eight neighbours quiet, not just the two smallest pairs: smooth
    // lighting noise occasionally lands two near-quiet neighbours, while a
    // clean edge or flat region quiets every same-side neighbour at once.
    const bool cleanStructure = total > 12.0f && pairCount >= 6 && alphaNoise < quietTolerance &&
                                 quietRunnerUp < quietTolerance && quietNeighbours >= 5u;
    // Reference grain beyond what RR's own pattern explains, for the case the
    // seed alpha underestimates it. RR variation that its correlation with the
    // reference cannot explain is RR's own noise; a reference noisier than
    // that cannot sharpen it. Pure attenuation (rr = scaled reference) leaves
    // this at zero, so blur recovery is unaffected.
    const float rrOwnNoise = sqrt(max(vr - cov * cov / max(vp, 1e-12f), 0.0f));
    // Curvature-robust fine-noise floor from the quadratic residuals: smooth
    // structure cancels, so this catches grain the seed alpha missed (shared
    // RR/reference noise, zero-alpha grain). High-frequency pattern curvature
    // inflates it too; that cost is accepted - a per-pixel residual estimate
    // is the only single-frame witness of an alpha lie, and a quiet patch of
    // grain keeps its own small residual as its noise floor.
    const float measuredFineNoise =
        sqrt((2.0f * dot(residualX, residualX) + 2.0f * dot(residualY, residualY) + dot(residualXY, residualXY)) /
             max(0.984375f * total * total, 1e-8f));
    // A step edge has a large quadratic residual yet truly quiet pairs along
    // its extent; grain has neither. Only a patch without a genuinely quiet
    // neighbour pair keeps the residual as its noise floor.
    const float quietPairFloor = pairCount >= 6 ? quietRunnerUp : 1e20f;
    const float pairTolerance = max(length(reference.rgb) * 0.002f, 2e-7f);
    const float effectiveFineNoise = measuredFineNoise * smoothstep(pairTolerance, pairTolerance * 4.0f, quietPairFloor);
    // Smooth (>3px-scale) excursions the 3x3 cannot see: when the patch is
    // locally quiet but the wide taps are not, the excursion itself is the
    // noise floor. This catches the quiet-sample tail of iid grain whose own
    // residual read low; RR-corroborated variance is exempt, so a blurred
    // pattern still correlates where it survives.
    const float quietPatch =
        1.0f - smoothstep(0.0015f, 0.0045f, vp / Square(max(GetLuminance(reference.rgb), 1e-3f)));
    const float structure = max(amplitude, 0.7071f * coarseExcursion);
    const float wideNoise = quietPatch * (1.0f - saturate(cov / max(vp, 1e-12f))) * 0.7071f * coarseExcursion;
    const float patchNoise =
        cleanStructure ? 0.0f : max(max(alphaNoise, effectiveFineNoise), max(rrOwnNoise, wideNoise));
    const float snr = structure / max(patchNoise, 1e-5f);
    float structureWeight =
        step(max(length(lowReference) * 1e-4f, 1e-6f), structure) * smoothstep(1.20f, 1.80f, snr) *
        max(smoothstep(0.22f, 0.28f, structure / max(patchNoise + GetLuminance(lowReference), 1e-5f)),
            smoothstep(2.5f, 6.5f, snr));
    // A measured-clean patch borders RR damage without carrying any local
    // structure of its own (a flat region beside a blurred edge): nothing
    // needs amplifying there, the clean reference simply replaces RR.
    structureWeight = cleanStructure ? 1.0f : structureWeight;
    // If the difference is explained by the measured noise, RR already
    // retained the current structure. Do not put that noise back into it.
    const float rrError = sqrt(rrDifference2 / max(total, 1e-5f));
    const float rrAgreement = 1.0f - smoothstep(0.8f, 1.2f, rrError / max(patchNoise, 1e-5f));
    const float lrMean = GetLuminance(lowRR), lpMean = GetLuminance(lowReference);
    const float3 mcr = rrChromaMean / max(total, 1e-5f), mcp = refChromaMean / max(total, 1e-5f);
    const float vcr = max(rrChromaSquare / max(total, 1e-5f) - dot(mcr, mcr) / 3.0f, 0.0f);
    const float vcp = max(refChromaSquare / max(total, 1e-5f) - dot(mcp, mcp) / 3.0f, 0.0f);
    const float ccp = crossChroma / max(total, 1e-5f) - dot(mcr, mcp) / 3.0f;
    const float lumaCoherence = saturate(cov / max(sqrt(vr * vp), 1e-12f));
    const float chromaCoherence = saturate(ccp / max(sqrt(vcr * vcp), 1e-12f));
    const float patchVariance = patchNoise * patchNoise * 0.75f;
    // Correlation divided by reference variance shrinks with RR's contrast.
    // Use normalized pattern agreement to recognize the same attenuated detail,
    // while keeping the existing noise, structure and already-sharp vetoes.
    // Normalize contrast by the local light level: exposure alone is not blur.
    const float retainedContrast = sqrt((vr + vcr) / max(vp + vcp - 2.0f * patchVariance, 1e-12f)) *
                                   max(lpMean, 1e-5f) / max(lrMean, 1e-5f);
    // A nearly flat RR patch cannot certify smooth reference grain merely
    // because their local slopes align. Likewise, shared noise can correlate
    // strongly but cannot authorize more contrast without a high reference SNR.
    const float rrStructureSupport =
        smoothstep(0.005f, 0.015f, sqrt(vr + vcr) / max(lrMean, 1e-5f)) * smoothstep(2.5f, 6.5f, snr);
    const float supportedAttenuation =
        max(smoothstep(0.85f, 0.97f, lumaCoherence), smoothstep(0.85f, 0.97f, chromaCoherence)) *
        rrStructureSupport * structureWeight * (1.0f - rrAgreement) * saturate(1.0f - retainedContrast);
    // Box anchor into local statistics. Per-channel only: the full colour
    // covariance rotation stays with the handover Anchor algorithm. RR's
    // variance is exactly what attenuation removed, so once the measured
    // noise is subtracted the reference's own structure variance is a valid
    // bound as well - but only when that structure is a significant share of
    // the patch brightness. Unexplained (grain) variance leaves nothing here,
    // and smooth lighting noise stays locally low-contrast, so neither can
    // widen the anchor.
    float3 graft = reference.rgb;
    if (FloorHandoverAnchorClamp > 0 && !cleanStructure)
    {
        const float3 structureVariance = max(refVariance - patchNoise * patchNoise, 0);
        const float structureContrast =
            sqrt(dot(structureVariance, 1.0f.xxx) / 3.0f) / max(GetLuminance(lowReference), 1e-5f);
        // Widening is corroborated by RR itself: only the share of the
        // reference's variance that RR's local pattern also carries may widen
        // the anchor. Grain over flat RR correlates with nothing, so a
        // per-pixel residual underestimate cannot certify it as structure.
        const float relaxEvidence = max(saturate(cov / max(vp, 1e-12f)), supportedAttenuation);
        const float anchorRelax = smoothstep(0.06f, 0.10f, structureContrast) * relaxEvidence;
        const float3 tolerance =
            FloorHandoverAnchorClamp * sqrt(max(varianceRR, anchorRelax * structureVariance));
        graft = clamp(graft, max(lowRR - tolerance, 0), lowRR + tolerance);
    }
    anchoredReference = graft;
    const float lumaAgreement =
        saturate(((2.0f * cov + 1e-3f) / (vr + vp + 1e-3f)) *
                 ((2.0f * lrMean * lpMean + 1e-2f) / (lrMean * lrMean + lpMean * lpMean + 1e-2f)));
    const float chromaStabilizer = max(4.0f * patchNoise * patchNoise, 1e-6f);
    const float chromaAgreement =
        saturate((2.0f * ccp + chromaStabilizer) / max(vcr + vcp + chromaStabilizer, 1e-6f));
    const float colourEvidence = vcr / (vcr + vr + chromaStabilizer);
    float agreement = lerp(lumaAgreement, min(lumaAgreement, chromaAgreement), colourEvidence);
    // Blur-hypothesis stand-in (the handover path probes a blurred reference
    // instead): reference contrast persistently above RR beyond the measured
    // noise is attenuation, not disagreement. Blur raises agreement exactly
    // where recovery should transfer, so scale it by the supported contrast
    // ratio. Structure, anchor and residual-noise gates still protect a clean
    // RR result from grain with inflated variance.
    agreement *= saturate(vr / max(vp - 2.0f * patchVariance, 1e-6f));
    agreement *= 1.0f - supportedAttenuation;
    // The correlation opt-out reuses the measured-clean verdict above.
    const float mixGate = cleanStructure ? 1.0f : 1.0f - saturate(FloorHandoverCorrelationMix) * agreement;
    const float confidence = structureWeight * (1.0f - rrAgreement) * mixGate;
    float3 correction = confidence * FilterRecoveryDelta(graft, rr, patchNoise);
    // Contrast extensions repair partial transfer only: whatever share of the
    // anchored candidate the base already transferred leaves no headroom, and
    // a measured-clean patch transfers exactly, so amplifying on top of
    // either would overshoot the supported reference range.
    const float contrastWeight =
        (1.0f - saturate(length(correction) / max(length(graft - rr), 1e-6f))) * (cleanStructure ? 0.0f : 1.0f);
    {
        // Chromatic contrast: recover only the colour RR's own local pattern
        // predicts, ray-limited so luminance is untouched.
        const float colourSupport =
            smoothstep(0.80f, 0.95f, chromaCoherence) * vcr / (vcr + chromaStabilizer);
        const float missingGain = min(max(ccp - vcr - 2.0f * patchVariance, 0.0f) / max(vcr, 1e-6f), 2.0f);
        const float3 highRR = rr - lowRR;
        const float3 predicted = missingGain * (highRR - GetLuminance(highRR));
        const float3 delta = graft - rr;
        float rayLimit = 1.0f;
        [unroll] for (uint channel = 0; channel < 3; ++channel)
            if (abs(predicted[channel]) > 1e-10f)
                rayLimit = min(rayLimit, saturate(delta[channel] / predicted[channel]));
        chromaCorrection = contrastWeight * saturate(ChromaRecovery) * structureWeight * (1.0f - rrAgreement) *
                           saturate(FloorHandoverCorrelationMix) * agreement * colourSupport * rayLimit * predicted;
        // Luminance contrast: RR attenuated a pattern it still carries. The
        // gain comes from RR's own high band after a noise allowance; a
        // uniform illumination gain must not be read as blur.
        const float lightingGain =
            max((GetLuminance(reference.rgb) + mp) / max(GetLuminance(rr) + mr, 1e-6f), 1.0f);
        const float localLoss = max(cov - vr * lightingGain - 2.0f * patchVariance, 0.0f);
        const float fitResidual = max(vp - cov * cov / max(vr, 1e-12f), 0.0f);
        const float lossEvidence = localLoss / max(sqrt(vr * fitResidual), 1e-12f);
        const float lumaSupport = smoothstep(0.85f, 0.97f, lumaCoherence) * vr / (vr + 4.0f * patchVariance + 1e-6f);
        const float lossSupport = smoothstep(0.75f, 1.75f, lossEvidence);
        const float lumaGain = min(localLoss / max(vr, 1e-6f), 2.0f);
        const float predictedLuma = -lumaGain * mr;
        const float3 colourCandidate = saturate(ChromaRecovery) * colourSupport * rayLimit * predicted;
        float lumaLimit = 1.0f;
        [unroll] for (uint channel = 0; channel < 3; ++channel)
            if (abs(predictedLuma) > 1e-10f)
            {
                const float margin = (predictedLuma > 0 ? max(delta[channel], 0.0f) : min(delta[channel], 0.0f)) -
                                     colourCandidate[channel];
                lumaLimit = min(lumaLimit, saturate(margin / predictedLuma));
            }
        lumaCorrection = contrastWeight * saturate(LumaRecovery) * structureWeight * (1.0f - rrAgreement) *
                         saturate(FloorHandoverCorrelationMix) * agreement * lumaSupport * lossSupport *
                         lumaLimit * predictedLuma;
    }
    // Missing negative contrast must not carve a dark ring below both RR and
    // the supported reference range (likewise for bright overshoot).
    const float3 totalCorrection = correction + chromaCorrection + lumaCorrection;
    return clamp(rr + totalCorrection, min(rr, referenceMin), max(rr, referenceMax)) - rr;
}
#endif
void StoreRecoveryHistory(int2 p, float4 reference, float4 decisions, float3 colour, bool active)
{
    if (WriteHistory == 0) return;
    if (!active)
    {
        OutDecisionHistory[p] = -1.0f;
        OutHistoryMetadata[p] = 0;
        return;
    }
    OutDecisionHistory[p] = decisions;
    const float z = InLinearDepth[p];
    const bool valid = reference.a >= 0 && isfinite(z) && z != 0 && InMotion[p].a >= 0.5f;
    OutHistoryMetadata[p] = uint4(asuint(z), PackHistoryGuide(InNormals[p]) | (valid ? 0x80000000u : 0),
        PackHistoryAlbedo(InDiffuseAlbedo[p].rgb), PackHistoryColour(colour));
}

[RootSignature(MainRS)][numthreads(8, 8, 1)] void CSMain(uint3 groupID : SV_GroupID, uint3 gtID : SV_GroupThreadID)
{
    const int2 p = int2(groupID.xy * 8 + gtID.xy);
    const int2 bounds = int2(DstTexSize.xy) - 1;
    const bool inBounds = all(p <= bounds);
    if (IsSet(FLAGS_RAW_SOURCE_BLIT))
    {
        if (!inBounds)
            return;
        const float2 uv = (float2(p) + 0.5f) * DstTexSize.zw;
        OutColor[p] = IsSet(FLAGS_SCALE_SRC)
                          ? InIndirectSpecular.SampleLevel(LinearSampler, uv * SourceUvScale + SourceUvOffset, 0)
                          : InIndirectSpecular[p];
        return;
    }
    // Uniform fast path: no reference/guide reads or neighbourhood when detail is off.
#if FSRD_COMPOSITION_VARIANT == 2
    // The CPU predicate proves this original uniform fast path for every lane.
    if (true)
#else
    if ((DetailPreservation <= 0.0f || RecoveryMask == 0) && !IsSet(FLAGS_DEBUG))
#endif
    {
        if (inBounds)
        {
            OutColor[p] = half4(Reconstruct(p), 1);
            if (WriteHistory != 0)
            {
                OutDecisionHistory[p] = -1.0h;
                OutHistoryMetadata[p] = 0;
            }
        }
        return;
    }
#if FSRD_COMPOSITION_VARIANT != 2
    const uint tid = gtID.x + gtID.y * 8;
    if (tid == 0)
    {
#if FSRD_COMPOSITION_VARIANT == 0 || FSRD_COMPOSITION_VARIANT == 4
        g_VaryingGuides = 0;
#endif
        g_HasHandover = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    // Conversion owns the original-albedo classification (type 1). No raw
    // roughness test here: mirrors and textured materials must not opt in.
    float3 anchorWeight = 0, filterWeight = 0;
    if (inBounds) RecoveryWeights(p, anchorWeight, filterWeight);
    const bool handoverSurface = any(anchorWeight > 0);
#if FSRD_COMPOSITION_VARIANT == 1
    // All selected bits are Light; no lane can contribute the Anchor bit.
    const uint recoveryMethods = any(filterWeight > 0) ? 2u : 0u;
#else
    const uint recoveryMethods = (handoverSurface ? 1u : 0u) | (any(filterWeight > 0) ? 2u : 0u);
#endif
    if (recoveryMethods != 0)
        InterlockedOr(g_HasHandover, recoveryMethods);
    GroupMemoryBarrierWithGroupSync();
#if FSRD_COMPOSITION_VARIANT == 3
    // Every lane contributed original RecoveryWeights before this uniform guard.
    // The companion Anchor shader owns every pixel/history write in these tiles.
    if ((g_HasHandover & 1u) != 0) return;
#elif FSRD_COMPOSITION_VARIANT == 4
    // Includes ordinary/no-selection tiles: the small shader owns them all.
    if ((g_HasHandover & 1u) == 0) return;
#endif
    if ((g_HasHandover & 2u) != 0)
    {
        // 100 reconstructions per 64 pixels instead of nine per light pixel.
        // All lanes load the halo, including lanes outside a partial group.
        [unroll] for (uint i = 0; i < 2; ++i)
        {
            const uint index = tid + i * 64;
            if (index < 100)
            {
                const int2 s = int2(index % 10, index / 10);
                const int2 q = clamp(int2(groupID.xy * 8) + s - 1, 0, bounds);
                g_LightRR[s.y][s.x] = Reconstruct(q);
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    // This return is uniform across the group, before all tile barriers.
    // Ordinary surfaces keep spatial Floor + RR without screen reconstruction.
#if FSRD_COMPOSITION_VARIANT == 1 || FSRD_COMPOSITION_VARIANT == 3
    // Proven uniformly by the CPU predicate or the whole-tile guard; keep the existing Light path verbatim.
    if (true)
#else
    if ((g_HasHandover & 1u) == 0 && !IsSet(FLAGS_DEBUG))
#endif
    {
        if (inBounds)
        {
            const float3 rr = Reconstruct(p);
            if (!any(filterWeight > 0))
            {
                OutColor[p] = half4(rr,1);
                if (WriteHistory != 0)
                {
                    OutDecisionHistory[p] = -1.0f;
                    OutHistoryMetadata[p] = 0;
                }
                return;
            }
            const float4 reference = InDetailReference[p];
            const bool active = reference.a >= 0 && DetailPreservation > 0;
            float3 anchoredReference = reference.rgb;
            float3 filterChroma = 0;
            float filterLuma = 0;
            const float3 correction =
                active ? LightAnchorRecovery(p, reference, rr, anchoredReference, filterChroma, filterLuma) : 0;
            OutColor[p] = half4(FloorRadiance(rr + (active ? saturate(DetailPreservation)*filterWeight*correction : 0)), 1);
            StoreRecoveryHistory(p, reference, float4(-1.0f, -1.0f, -1.0f, -1.0f), anchoredReference, active);
        }
        return;
    }
#if FSRD_COMPOSITION_VARIANT == 0 || FSRD_COMPOSITION_VARIANT == 4
    const uint tileSide = s_SM_Size.x;
    const int tileOffset = 0;
    const int2 origin = int2(groupID.xy * 8) - int2(s_SM_HaloOffset);
    [unroll] for (uint i = 0; i < s_SM_LoadsPerThread; ++i)
    {
        const uint flat = tid + i * 64;
        if (flat < tileSide * tileSide)
        {
            const int2 s = int2(flat % tileSide, flat / tileSide) + tileOffset;
            const int2 q = clamp(origin + s, 0, bounds);
            g_RR[s.y][s.x] = Reconstruct(q);
            g_Reference[s.y][s.x] = InDetailReference[q];
            g_Depth[s.y][s.x] = InLinearDepth[q];
            g_Normal[s.y][s.x] = OctahedralDecode(InNormals[q].xy);
            g_Albedo[s.y][s.x] = InDiffuseAlbedo[q].rgb;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint k = 0; k < s_SM_LoadsPerThread; ++k)
    {
        const uint flat = tid + k * 64;
        if (flat < 256)
        {
            const int2 s = int2(flat % 16, flat / 16);
            const float baseZ = g_Depth[4][4];
            // A long baseline avoids amplifying depth-storage roundoff into the
            // fitted slope; all samples still have to pass the planar test.
            const float2 slope = float2(g_Depth[4][15] - g_Depth[4][0], g_Depth[15][4] - g_Depth[0][4]) / 15.0f;
            const float error = abs(g_Depth[s.y][s.x] - (baseZ + dot(slope, float2(s) - 4.0f)));
            // The slope bound also proves that the original 25%-depth prediction
            // clamp cannot activate anywhere in this tile's nine-tap support.
            const bool linearPlane =
                error <= max(abs(baseZ) * 2e-7f, 1e-6f) && 11.0f * (abs(slope.x) + abs(slope.y)) < abs(baseZ) * 0.1f;
            // Perspective projection makes reciprocal view depth affine on a
            // planar TV, not view depth itself. A linear-Z-only certificate
            // sent practically every tilted display through the expensive path.
            const float inverseZ = rcp(baseZ);
            const float2 inverseSlope = float2(rcp(g_Depth[4][15])-rcp(g_Depth[4][0]),
                rcp(g_Depth[15][4])-rcp(g_Depth[0][4])) / 15.0f;
            const float inverseTap = rcp(g_Depth[s.y][s.x]);
            const bool perspectivePlane = isfinite(inverseTap) && inverseTap*inverseZ>0 &&
                abs(inverseTap-(inverseZ+dot(inverseSlope,float2(s)-4.0f))) <= abs(inverseZ)*2e-6f;
            const bool plane = linearPlane || perspectivePlane;
            uint variation = plane ? 0u : 1u;
            variation |= any(g_Albedo[s.y][s.x] != g_Albedo[4][4]) ? 2u : 0u;
            variation |= (any(g_Normal[s.y][s.x] != g_Normal[4][4]) || g_Reference[s.y][s.x].a < 0) ? 4u : 0u;
            if (variation != 0)
                InterlockedOr(g_VaryingGuides, variation);
        }
    }
    // Compute each overlapping blur once per tile, in FP32 with the original
    // summation order. It is evidence only, never the transferred reference.
    [loop] for (uint j = tid; j < 144; j += 64)
    {
        const int2 s = int2(j % 12, j / 12) + 2;
        float3 blurred = 0;
        [unroll] for (int y = -1; y <= 1; ++y)[unroll] for (int x = -1; x <= 1; ++x) blurred +=
            ((x == 0 ? 2.0f : 1.0f) * (y == 0 ? 2.0f : 1.0f) / 16.0f) * float3(g_Reference[s.y + y][s.x + x].rgb);
        g_Blurred[s.y][s.x] = blurred;
        float quiet = 1e20f;
        const int2 directions[4] = { int2(1, 0), int2(0, 1), int2(1, 1), int2(1, -1) };
        [unroll] for (uint d = 0; d < 4; ++d)
        {
            const int2 r = s + directions[d];
            const float3 delta = float3(g_Reference[s.y][s.x].rgb) - float3(g_Reference[r.y][r.x].rgb);
            quiet = min(quiet, dot(delta, delta) / 6.0f);
        }
        g_QuietPair[s.y][s.x] = sqrt(quiet);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint guideFlags = g_VaryingGuides;
    const bool uniformRegion =
        guideFlags == 0 && all(origin >= 0) && all(origin + 15 <= bounds) && isfinite(g_Depth[4][4]) &&
        g_Depth[4][4] != 0 &&
        Square(saturate((dot(float3(g_Normal[4][4]), float3(g_Normal[4][4])) - 0.9f) * 10.0f)) > 0.1f;
    // On a uniform guide tile the regional weight is constant and cancels from
    // every normalized statistic. Share horizontal nine-tap sums, then gather
    // nine rows per pixel. The complete 9x9 footprint is preserved (no decimation).
    if (uniformRegion)
    {
        [unroll] for (uint k = 0; k < 2; ++k)
        {
            const uint i = tid + k * 64;
            const int2 centre = int2(i % 8 + 4, i / 8);
            const float3 originRef = g_Reference[centre.y][centre.x].rgb;
            const float3 originRR = g_RR[centre.y][centre.x];
            float4 a = 0, b = 0, c = 0;
            [unroll] for (int dx = -4; dx <= 4; ++dx)
            {
                const int2 q = centre + int2(dx, 0);
                const float3 ref = float3(g_Reference[q.y][q.x].rgb) - originRef;
                const float3 rr = float3(g_RR[q.y][q.x]) - originRR;
                const float3 difference = float3(g_Reference[q.y][q.x].rgb) - float3(g_RR[q.y][q.x]);
                const float lr = GetLuminance(rr), lp = GetLuminance(ref);
                a += float4(ref, lr);
                b += float4(ref * ref, lp);
                c += float4(lr * lr, lp * lp, lr * lp, dot(difference, difference) / 3.0f);
            }
            g_RegionA[i / 8][i % 8] = a;
            g_RegionB[i / 8][i % 8] = b;
            g_RegionC[i / 8][i % 8] = c;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (!inBounds)
        return;
    const int2 sm = int2(gtID.xy + s_SM_HaloOffset);
    const float3 rr = g_RR[sm.y][sm.x];
    const float4 reference = g_Reference[sm.y][sm.x];
    float historyReuse = 0;
    const float4 previousDecisions = handoverSurface ? LoadDecisionHistory(p, reference, historyReuse) : 0;
    float4 decisions = -1.0f;
    float3 filteredReference = reference.rgb;
    float3 boxReference = reference.rgb, anchoredReference = reference.rgb;
    float3 handoverWeights = 0, handoverLimits = 0;
    float3 beforeRangeClamp = rr;
    float confidence = 0.0f;
    float3 correction = 0.0f;
    float3 chromaCorrection = 0.0f;
    float lumaCorrection = 0.0f;
    // Debug views evaluate the intermediate decisions even at zero detail. The
    // strength still stays zero, so both composition views retain RR exactly.
    if (handoverSurface && reference.a >= 0.0f && (DetailPreservation > 0.0f || IsSet(FLAGS_DEBUG)))
    {
        const float z = g_Depth[sm.y][sm.x];
        const float2 gradient = float2(FloorDepthDerivative(g_Depth[sm.y][sm.x - 1], z, g_Depth[sm.y][sm.x + 1]),
                                       FloorDepthDerivative(g_Depth[sm.y - 1][sm.x], z, g_Depth[sm.y + 1][sm.x]));
        const float3 normal = g_Normal[sm.y][sm.x];
        const float3 albedo = g_Albedo[sm.y][sm.x];
        float3 lowRR = 0, lowReference = 0;
        float3 rrCenteredMean = 0, rrCenteredSquare = 0, rrCenteredCross = 0;
        float3 refCenteredMean = 0, refCenteredSquare = 0;
        float crossRGB = 0;
        float rrLumaMean = 0, refLumaMean = 0, rrLumaSquare = 0, refLumaSquare = 0, crossLuma = 0;
        float3 rrChromaMean = 0, refChromaMean = 0;
        float rrChromaSquare = 0, refChromaSquare = 0, crossChroma = 0;
        float noiseSquared = 0;
        float probeRawError = 0, probeBlurError = 0, probeWeight = 0;
        float noiseKeys[25];
        uint noiseCount = 0;
        float3 referenceMin = reference.rgb, referenceMax = reference.rgb;
        float total = 0;
        float regionTotal = 0, regionDifference2 = 0;
        float3 regionMean = 0, regionSquare = 0;
        float regionRRMean = 0, regionRRSquare = 0;
        float regionLumaMean = 0, regionLumaSquare = 0, regionLumaCross = 0;
        uint2 acceptedMask = 0;
        if (uniformRegion)
        {
            acceptedMask = uint2(0xffffffffu, 0x1ffffu);
            regionTotal = 81;
            [unroll] for (int dy = -4; dy <= 4; ++dy)
            {
                const int row = sm.y + dy;
                const float4 a = g_RegionA[row][gtID.x], b = g_RegionB[row][gtID.x], c = g_RegionC[row][gtID.x];
                const float3 shift = float3(g_Reference[row][sm.x].rgb) - reference.rgb;
                const float lr = GetLuminance(float3(g_RR[row][sm.x]) - rr), lp = GetLuminance(shift);
                regionMean += a.xyz + 9.0f * shift;
                regionSquare += b.xyz + 2.0f * shift * a.xyz + 9.0f * shift * shift;
                regionDifference2 += c.w;
                regionRRMean += a.w + 9.0f * lr;
                regionRRSquare += c.x + 2.0f * lr * a.w + 9.0f * lr * lr;
                regionLumaMean += b.w + 9.0f * lp;
                regionLumaSquare += c.y + 2.0f * lp * b.w + 9.0f * lp * lp;
                regionLumaCross += c.z + lr * b.w + lp * a.w + 9.0f * lr * lp;
            }
        }
        else
        {

            [loop] for (int ry = -4; ry <= 4; ++ry)
            {
                [loop] for (int rx = -4; rx <= 4; ++rx)
                {
                    const int2 offset = int2(rx, ry), q = sm + offset;
                    const float4 tap = g_Reference[q.y][q.x];
                    const bool independent = all(p + offset >= 0) && all(p + offset <= bounds);
                    const float w =
                        (tap.a >= 0 && independent)
                            ? CompositionSurfaceWeight(z, g_Depth[q.y][q.x], gradient, float2(offset), normal,
                                                       g_Normal[q.y][q.x], albedo, g_Albedo[q.y][q.x], guideFlags)
                            : 0.0f;
                    if (abs(rx) <= 3 && abs(ry) <= 3 && w > 0.1f)
                    {
                        const uint bit = uint((ry + 3) * 7 + rx + 3);
                        if (bit < 32)
                            acceptedMask.x |= 1u << bit;
                        else
                            acceptedMask.y |= 1u << (bit - 32);
                    }
                    const float3 centered = tap.rgb - reference.rgb;
                    const float3 difference = tap.rgb - float3(g_RR[q.y][q.x]);
                    regionTotal += w;
                    regionMean += w * centered;
                    regionSquare += w * centered * centered;
                    regionDifference2 += w * dot(difference, difference) / 3.0f;
                    const float r = GetLuminance(float3(g_RR[q.y][q.x]) - rr);
                    const float c = GetLuminance(centered);
                    regionRRMean += w * r;
                    regionRRSquare += w * r * r;
                    regionLumaMean += w * c;
                    regionLumaSquare += w * c * c;
                    regionLumaCross += w * r * c;
                }
            }
        }
        const float kernel[5] = { 1, 4, 6, 4, 1 };
        [unroll] for (int y = -2; y <= 2; ++y)
        {
            [unroll] for (int x = -2; x <= 2; ++x)
            {
                const int2 q = sm + int2(x, y);
                const float4 tapReference = g_Reference[q.y][q.x];
                const float surface = CompositionSurfaceWeight(z, g_Depth[q.y][q.x], gradient,
                                                               float2(clamp(p + int2(x, y), 0, bounds) - p), normal,
                                                               g_Normal[q.y][q.x], albedo, g_Albedo[q.y][q.x], guideFlags);
                // An explicitly bypassed tap cannot lend content to detail at its neighbour.
                const float spatial = kernel[x + 2] * kernel[y + 2];
                const float w =
                    (x == 0 && y == 0) ? spatial : spatial * surface * (tapReference.a >= 0.0f ? 1.0f : 0.0f);
                const float3 tapRR = g_RR[q.y][q.x];
                if (w > 0.1f)
                {
                    const float3 blurred = g_Blurred[q.y][q.x];
                    bool blurAccepted = true;
                    [branch] if (acceptedMask.x != 0xffffffffu || acceptedMask.y != 0x1ffffu) blurAccepted =
                        BlurAccepted(acceptedMask, int2(x, y));
                    if (blurAccepted)
                    {
                        const float3 rawDelta = float3(tapReference.rgb) - tapRR;
                        const float3 blurDelta = blurred - tapRR;
                        probeRawError += w * dot(rawDelta, rawDelta) / 3.0f;
                        probeBlurError += w * dot(blurDelta, blurDelta) / 3.0f;
                        probeWeight += w;
                    }
                }

                const bool independent = all(p + int2(x, y) >= 0) && all(p + int2(x, y) <= bounds);
                const bool accepted = independent && surface > 0.1f && tapReference.a >= 0;
                noiseKeys[(y + 2) * 5 + x + 2] = accepted ? tapReference.a : 1e20f;
                noiseCount += accepted ? 1 : 0;

                const float3 centeredRR = tapRR - rr;
                rrCenteredMean += w * centeredRR;
                rrCenteredSquare += w * centeredRR * centeredRR;
                rrCenteredCross += w * centeredRR.xxy * centeredRR.yzz;
                const float3 centeredReference = tapReference.rgb - reference.rgb;
                refCenteredMean += w * centeredReference;
                refCenteredSquare += w * centeredReference * centeredReference;
                crossRGB += w * dot(centeredRR, centeredReference);
                const float lr = GetLuminance(centeredRR);
                const float lp = GetLuminance(tapReference.rgb - reference.rgb);
                rrLumaMean += w * lr;
                refLumaMean += w * lp;
                rrLumaSquare += w * lr * lr;
                refLumaSquare += w * lp * lp;
                crossLuma += w * lr * lp;
                // Equal-luminance colour transitions are real structure too.
                // Centre the moments before squaring, as for HDR luminance.
                const float3 cr = centeredRR - lr;
                const float3 cp = (tapReference.rgb - reference.rgb) - lp;
                rrChromaMean += w * cr;
                refChromaMean += w * cp;
                rrChromaSquare += w * dot(cr, cr) / 3.0f;
                refChromaSquare += w * dot(cp, cp) / 3.0f;
                crossChroma += w * dot(cr, cp) / 3.0f;

                lowRR += w * tapRR;
                lowReference += w * tapReference.rgb;
                noiseSquared += w * max(tapReference.a, 0) * max(tapReference.a, 0);
                total += w;
                if (w > 0.1f)
                {
                    referenceMin = min(referenceMin, tapReference.rgb);
                    referenceMax = max(referenceMax, tapReference.rgb);
                }
            }
        }
        lowRR /= max(total, 1e-5f);
        lowReference /= max(total, 1e-5f);
        const float3 highReference = reference.rgb - lowReference;
        const float3 highRR = rr - lowRR;
        const float strength = saturate(DetailPreservation);

        [unroll] for (uint k = 0; k < kQuartileNetworkSize; ++k)
        {
            const uint a = QuartileNetwork[2 * k], b = QuartileNetwork[2 * k + 1];
            const float lo = min(noiseKeys[a], noiseKeys[b]);
            noiseKeys[b] = max(noiseKeys[a], noiseKeys[b]);
            noiseKeys[a] = lo;
        }
        // Curved/short lettering can occupy half this window. Estimate fine
        // uncertainty from the quieter quartile so corners do not classify
        // the glyph itself as noise. Coarse uncertainty
        // is handled separately by the unconditional RR controls below.
        // Tiny/unsupported patches retain the conservative RMS fallback.
        float patchNoise = sqrt(noiseSquared / total);
        [unroll] for (uint rank = 0; rank < 25; ++rank) if (noiseCount >= 9 && rank == (noiseCount - 1) / 4)
            patchNoise = noiseKeys[rank];
        // Mixed derivatives in the seed reject straight edges but not dense
        // small glyph corners. Their contrast can fill the entire 5x5 sigma
        // window and falsely reject clean lettering as noise.
        // Bound that estimate with distinct, same-surface colour pairs.
        // A quiet continuation of a colour supplies evidence even when no
        // complete 3x3 patch is flat. Four orientations also cover diagonals.
        // The lower quartile needs nine distinct pair origins; replicated
        // border samples and routed/cross-surface colours cannot lower it.
        const int2 pairDirections[4] = { int2(1, 0), int2(0, 1), int2(1, 1), int2(1, -1) };
        uint pairCount = 0;
        [unroll] for (uint i = 0; i < 25; ++i)
        {
            const int2 offset = int2(i % 5, i / 5) - 2, q = sm + offset;
            float quietPair = 1e20f;
            [branch] if (acceptedMask.x == 0xffffffffu && acceptedMask.y == 0x1ffffu) quietPair = g_QuietPair[q.y][q.x];
            else if (Accepted(acceptedMask, offset))
            {
                [unroll] for (uint direction = 0; direction < 4; ++direction)
                {
                    const int2 otherOffset = offset + pairDirections[direction], r = sm + otherOffset;
                    if (!Accepted(acceptedMask, otherOffset))
                        continue;
                    const float3 delta = float3(g_Reference[q.y][q.x].rgb) - float3(g_Reference[r.y][r.x].rgb);
                    quietPair = min(quietPair, dot(delta, delta) / 6.0f);
                }
                quietPair = quietPair < 1e20f ? sqrt(quietPair) : 1e20f;
            }
            noiseKeys[i] = quietPair;
            pairCount += quietPair < 1e20f ? 1 : 0;
        }
        [unroll] for (uint k = 0; k < kQuartileNetworkSize; ++k)
        {
            const uint a = QuartileNetwork[2 * k], b = QuartileNetwork[2 * k + 1];
            const float lo = min(noiseKeys[a], noiseKeys[b]);
            noiseKeys[b] = max(noiseKeys[a], noiseKeys[b]);
            noiseKeys[a] = lo;
        }
        [unroll] for (uint rank = 0; rank < 25; ++rank)
            // Conservative allowance for selecting the quietest orientation.
            // This only caps the fine-grain estimate; RR Anchor/Mix remain
            // unconditional and still reject correlated illumination noise.
            // A supplied uncertainty larger than all observed colour
            // variation remains authoritative; quiet pairs cannot prove
            // that a heavily uncertain reference is trustworthy.
            if (pairCount >= 9 && rank == (pairCount - 1) / 4 &&
                patchNoise <= length(referenceMax - referenceMin) * 0.577350269f) patchNoise =
                min(patchNoise, 3.0f * noiseKeys[rank]);
        // Reference denoising has been retired. Regional statistics still
        // support Anchor/Mix and contrast recovery, but never average the
        // current reference into a replacement image.
        const float patchVariance = patchNoise * patchNoise * 0.75f;
        regionMean /= max(regionTotal, 1e-5f);
        const float3 regionVariance = max(regionSquare / max(regionTotal, 1e-5f) - regionMean * regionMean, 0);
        const float amplitude = sqrt(dot(regionVariance, 1.0f.xxx) / 3.0f);
        const float regionNoise = patchNoise;
        regionMean += reference.rgb;
        // One current reference sample: no NLM effective-sample gain.
        const float candidateNoise = regionNoise;
        const float snr = amplitude / max(candidateNoise, 1e-5f);
        const float structureWeight =
            step(max(length(regionMean) * 1e-4f, 1e-6f), amplitude) * smoothstep(1.20f, 1.80f, snr) *
            max(smoothstep(0.22f, 0.28f, amplitude / max(candidateNoise + GetLuminance(regionMean), 1e-5f)),
                smoothstep(4.0f, 8.0f, snr));
        // If the difference is explained by the measured noise, RR already
        // retained the current structure. Do not put that noise back into it.
        const float rrError = sqrt(regionDifference2 / max(regionTotal, 1e-5f));
        const float rrAgreement = 1.0f - smoothstep(0.8f, 1.2f, rrError / max(regionNoise, 1e-5f));
        float3 graft = reference.rgb;
        filteredReference = graft;
        boxReference = graft;
        const float3 meanDelta = rrCenteredMean / total;
        const float3 variance = max(rrCenteredSquare / total - meanDelta * meanDelta, 0);
        const float3 refMeanDelta = refCenteredMean / total;
        const float refEnergy = dot(max(refCenteredSquare / total - refMeanDelta * refMeanDelta, 0), 1.0f.xxx);
        const float rrEnergy = dot(variance, 1.0f.xxx);
        const float covarianceRGB = crossRGB / total - dot(meanDelta, refMeanDelta);
        // The reference controls restored on the current, surface-bounded
        // reconstruction. Both are inert at zero.
        if (FloorHandoverAnchorClamp > 0)
        {
            const float3 crossVariance = rrCenteredCross / total - meanDelta.xxy * meanDelta.yzz;
            // Like the reference DLL, a positive anchor always constrains
            // transferred colour. Repeated grain must not switch it off.
            // This intentionally trades some new-texture contrast for RR
            // stability; zero is the explicit opt-out, not a hidden gate.
            const float3 tolerance = FloorHandoverAnchorClamp * sqrt(variance);
            const float3 boxAnchor = clamp(graft, max(lowRR - tolerance, 0), lowRR + tolerance);
            boxReference = boxAnchor;
            // The extra colour constraint is valid only if the two patches
            // agree on their structure. A stale RR palette cannot describe
            // new colours in an animated screen. The original box anchor
            // remains unconditional even when this additional test fails.
            const float paletteAgreement = saturate(covarianceRGB / max(sqrt(rrEnergy * refEnergy), 1e-12f));
            const float colourAnchorWeight =
                StableDecision(smoothstep(0.65f, 0.95f, paletteAgreement), previousDecisions.x, historyReuse);
            decisions.x = colourAnchorWeight;
            graft = boxAnchor;
            if (colourAnchorWeight > 0)
                graft =
                    lerp(boxAnchor, AnchorColour(boxAnchor, lowRR, variance, crossVariance, FloorHandoverAnchorClamp),
                         colourAnchorWeight);
        }
        anchoredReference = graft;
        const float mr = rrLumaMean / total, mp = refLumaMean / total;
        const float vr = max(rrLumaSquare / total - mr * mr, 0);
        const float vp = max(refLumaSquare / total - mp * mp, 0);
        const float cov = crossLuma / total - mr * mp;
        const float lrMean = GetLuminance(lowRR), lpMean = GetLuminance(lowReference);
        const float lumaAgreement =
            saturate(((2 * cov + 1e-3f) / (vr + vp + 1e-3f)) *
                     ((2 * lrMean * lpMean + 1e-2f) / (lrMean * lrMean + lpMean * lpMean + 1e-2f)));
        const float3 mcr = rrChromaMean / total, mcp = refChromaMean / total;
        const float vcr = max(rrChromaSquare / total - dot(mcr, mcr) / 3.0f, 0.0f);
        const float vcp = max(refChromaSquare / total - dot(mcp, mcp) / 3.0f, 0.0f);
        const float ccp = crossChroma / total - dot(mcr, mcp) / 3.0f;
        const float chromaStabilizer = max(4.0f * patchNoise * patchNoise, 1e-6f);
        const float chromaAgreement =
            saturate((2.0f * ccp + chromaStabilizer) / max(vcr + vcp + chromaStabilizer, 1e-6f));
        // Only colour structure supported by RR can dispute its luminance
        // agreement. Random colour grain over neutral/flat RR cannot do so.
        // The user's Mix is still applied directly to the resulting agreement.
        const float colourEvidence = vcr / (vcr + vr + chromaStabilizer);
        float agreement = lerp(lumaAgreement, min(lumaAgreement, chromaAgreement), colourEvidence);
        // Subtract more than the expected fine-noise error before accepting
        // the blur hypothesis. Smoothing random noise can otherwise look
        // like evidence to restore that very noise. Coarse noise normally
        // survives the probe and therefore cannot explain a strong fit gain.
        // Relative units make the evidence independent of title exposure.
        if (probeWeight >= total * 0.5f)
        {
            const float unit2 = max(dot(lowReference, lowReference) / 3.0f, 1e-12f);
            const float unexplained = max(probeRawError / probeWeight - 8.0f * patchVariance, 0.0f);
            const float blurredError = max(probeBlurError / probeWeight - 0.28f * patchVariance, 0.0f);
            const float fit = 1.0f - blurredError / max(unexplained, 1e-6f * unit2);
            const float support = smoothstep(0.60f, 0.85f, fit) * smoothstep(1e-4f * unit2, 1e-3f * unit2, unexplained);
            agreement *= 1.0f - support;
        }
        agreement = StableDecision(agreement, previousDecisions.y, historyReuse);
        decisions.y = agreement;
        // Direct reference-style correlation rejection. Do not attenuate
        // the user's mix by a fourth-power sigma/error term: coarse grain
        // can have a small high-frequency sigma and a very large RR error.
        confidence =
            structureWeight * (1.0f - rrAgreement) * (1.0f - saturate(FloorHandoverCorrelationMix) * agreement);
        correction = strength * confidence * (graft - rr);
        // A matching luminance pattern does not establish matching colour
        // contrast. Recover only the chromatic component predicted by RR's
        // own local pattern, when the reference supports the same direction
        // with higher amplitude. Independent colour grain has no expected
        // positive regression gain over an already-correct RR pattern.
        // Noise estimate below only validates contrast recovery.
        {
            const float chromaCoherence = saturate(ccp / max(sqrt(vcr * vcp), 1e-12f));
            const float colourSupport =
                StableDecision(smoothstep(0.80f, 0.95f, chromaCoherence) * vcr / (vcr + chromaStabilizer),
                               previousDecisions.z, historyReuse);
            decisions.z = colourSupport;
            const float missingGain = min(max(ccp - vcr - 2.0f * patchVariance, 0.0f) / max(vcr, 1e-6f), 2.0f);
            const float3 colourDirection = highRR - GetLuminance(highRR);
            const float3 predicted = missingGain * colourDirection;
            const float3 delta = graft - rr;
            // Stay on a single colour ray and inside the anchored candidate
            // in every channel. One scalar limit preserves zero luminance;
            // per-channel clipping here would create a brightness change.
            float rayLimit = 1.0f;
            [unroll] for (uint channel = 0; channel < 3; ++channel)
            {
                if (abs(predicted[channel]) > 1e-10f)
                    rayLimit = min(rayLimit, saturate(delta[channel] / predicted[channel]));
            }
            chromaCorrection = saturate(ChromaRecovery) * strength * structureWeight * (1.0f - rrAgreement) *
                               saturate(FloorHandoverCorrelationMix) * agreement * colourSupport * rayLimit * predicted;
            correction += chromaCorrection;
            // SSIM can also agree strongly with a luminance pattern whose
            // contrast RR has attenuated. Infer a gain only from aligned
            // RR/reference structure, after subtracting the fine-noise
            // allowance. Predict from RR's high band; no mean brightness or
            // uncorrelated reference residual is copied by this extension.
            const float broadRRMean = regionRRMean / max(regionTotal, 1e-5f);
            const float broadRefMean = regionLumaMean / max(regionTotal, 1e-5f);
            const float broadVR = max(regionRRSquare / max(regionTotal, 1e-5f) - Square(broadRRMean), 0);
            const float broadVP = max(regionLumaSquare / max(regionTotal, 1e-5f) - Square(broadRefMean), 0);
            const float broadCov = regionLumaCross / max(regionTotal, 1e-5f) - broadRRMean * broadRefMean;
            const float lumaCoherence = saturate(cov / max(sqrt(vr * vp), 1e-12f));
            const float broadCoherence = saturate(broadCov / max(sqrt(broadVR * broadVP), 1e-12f));
            const float lumaSupport =
                smoothstep(0.85f, 0.97f, min(lumaCoherence, broadCoherence)) * vr / (vr + 4.0f * patchVariance + 1e-6f);
            // A uniform illumination/exposure gain raises both the mean and
            // contrast. It must not be interpreted as blur in this path.
            const float lightingGain =
                max((GetLuminance(reference.rgb) + broadRefMean) / max(GetLuminance(rr) + broadRRMean, 1e-6f), 1.0f);
            const float localLoss = max(cov - vr * lightingGain - 2.0f * patchVariance, 0.0f);
            const float broadLoss = max(broadCov - broadVR * lightingGain - 2.0f * patchVariance, 0.0f);
            const float localGain = localLoss / max(vr, 1e-6f);
            const float broadGain = broadLoss / max(broadVR, 1e-6f);
            // Correlated lighting can accidentally align with a small patch.
            // Require the contrast loss to exceed the unexplained residual
            // of the broad affine fit. Do not divide by sample count: coarse
            // illumination samples are not independent noise observations.
            const float fitResidual = max(broadVP - broadCov * broadCov / max(broadVR, 1e-12f), 0.0f);
            const float lossEvidence = broadLoss / max(sqrt(broadVR * fitResidual), 1e-12f);
            const float lossSupport = smoothstep(1.0f, 2.0f, lossEvidence);
            const float temporalLumaSupport =
                StableDecision(lumaSupport * lossSupport, previousDecisions.w, historyReuse);
            decisions.w = temporalLumaSupport;
            const float lumaGain = min(min(localGain, broadGain), 2.0f);
            // Confirm loss in both 5x5 and 9x9 support. Their samples overlap;
            // this is corroboration across scale, not independent evidence.
            // Centre against the broader RR mean to avoid attenuating the
            // very contrast being restored with another narrow low-pass.
            const float predictedLuma = -lumaGain * broadRRMean;
            // Chroma already consumes part of the rejected-transfer budget.
            // Limit the luminance ray to what remains in every RGB channel,
            // so the combined addition stays between RR and anchored G.
            const float3 colourCandidate = saturate(ChromaRecovery) * colourSupport * rayLimit * predicted;
            float lumaLimit = 1.0f;
            [unroll] for (uint channel = 0; channel < 3; ++channel)
            {
                if (abs(predictedLuma) > 1e-10f)
                {
                    const float margin = (predictedLuma > 0 ? max(delta[channel], 0.0f) : min(delta[channel], 0.0f)) -
                                         colourCandidate[channel];
                    lumaLimit = min(lumaLimit, saturate(margin / predictedLuma));
                }
            }
            lumaCorrection = saturate(LumaRecovery) * strength * structureWeight * (1.0f - rrAgreement) * saturate(FloorHandoverCorrelationMix) *
                             agreement * temporalLumaSupport * lumaLimit * predictedLuma;
            correction += lumaCorrection;
        }
        if (IsSet(FLAGS_DEBUG))
        {
            // RGB are independent permission weights: white allows transfer.
            handoverWeights =
                float3(structureWeight, 1.0f - rrAgreement, 1.0f - saturate(FloorHandoverCorrelationMix) * agreement);
            handoverLimits.r = LimitFraction(filteredReference, boxReference, rr);
            handoverLimits.g = LimitFraction(boxReference, anchoredReference, rr);
        }

        // Missing negative contrast must not carve a dark ring below both RR and
        // the supported reference range (likewise for bright overshoot).
        beforeRangeClamp = rr + correction;
        correction = clamp(rr + correction, min(rr, referenceMin), max(rr, referenceMax)) - rr;
        if (IsSet(FLAGS_DEBUG))
            handoverLimits.b = LimitFraction(beforeRangeClamp, rr + correction, rr);
    }
    correction *= anchorWeight;
    chromaCorrection *= anchorWeight;
    lumaCorrection *= GetLuminance(anchorWeight);
    float3 filterReference = reference.rgb;
    const bool filterActive = any(filterWeight > 0) && reference.a >= 0 && DetailPreservation > 0;
    if (filterActive)
    {
        float3 filterChroma = 0;
        float filterLuma = 0;
        const float3 filterCorrection = LightAnchorRecovery(p, reference, rr, filterReference, filterChroma, filterLuma);
        filteredReference = filterReference;
        chromaCorrection += saturate(DetailPreservation) * filterWeight * filterChroma;
        lumaCorrection += GetLuminance(saturate(DetailPreservation) * filterWeight) * filterLuma;
        correction += saturate(DetailPreservation) * filterWeight * filterCorrection;
    }
    if (filterActive || any(anchorWeight != 1.0f)) beforeRangeClamp = rr + correction;
    StoreRecoveryHistory(p, reference, handoverSurface ? decisions : float4(-1.0f, -1.0f, -1.0f, -1.0f),
                         filterReference, handoverSurface || filterActive);
    float3 output = FloorRadiance(rr + correction);
    if (IsSet(FLAGS_DEBUG))
    {
        const uint mode = GetDebugMode();
        switch (mode)
        {
        case FLAGS_DEBUG_DETAIL_CONFIDENCE:
            output = confidence.xxx;
            break;
        case FLAGS_DEBUG_SKIP_SIGNAL:
            output = InSkipSignal[p].rgb;
            break;
        // Historical meaning: demodulated RR signals, with no albedo or Skip added.
        case FLAGS_DEBUG_DENOISER_OUTPUT:
            output = SpecularRadiance(p) + DiffuseRadiance(p);
            break;
        case FLAGS_DEBUG_RECONSTRUCTED_COLOR:
            output = rr;
            break;
        case FLAGS_DEBUG_DETAIL_REFERENCE:
            output = reference.a >= 0.0f ? filteredReference : 0.0f;
            break;
        case FLAGS_DEBUG_DETAIL_SEED:
            output = reference.a >= 0.0f ? reference.rgb : 0.0f;
            break;
        case FLAGS_DEBUG_DETAIL_BOX_ANCHORED:
            output = reference.a >= 0.0f ? boxReference : 0.0f;
            break;
        case FLAGS_DEBUG_DETAIL_ANCHORED:
            output = reference.a >= 0.0f ? anchoredReference : 0.0f;
            break;
        case FLAGS_DEBUG_HANDOVER_WEIGHTS:
            output = handoverWeights;
            break;
        case FLAGS_DEBUG_HANDOVER_LIMITS:
            output = handoverLimits;
            break;
        case FLAGS_DEBUG_HANDOVER_ELIGIBILITY:
            output = float3(handoverSurface ? 1.0f : 0.0f, reference.a >= 0.0f ? 1.0f : 0.0f, saturate(DetailPreservation));
            break;
        case FLAGS_DEBUG_COMPOSITION_BEFORE_CLAMP:
            output = beforeRangeClamp;
            break;
        // Same production composition, displayed without the upscaler like the
        // other composition diagnostics. Do not compare its pixel sharpness to
        // None as if both views had the same presentation path.
        case FLAGS_DEBUG_COMPOSITION_FINAL:
            break;
        case FLAGS_DEBUG_DETAIL_CORRECTION:
            output = saturate(0.5f + correction / max(GetLuminance(rr), 1e-3f));
            break;
        case FLAGS_DEBUG_CHROMA_RECOVERY:
            output = saturate(0.5f + chromaCorrection / max(GetLuminance(rr), 1e-3f));
            break;
        case FLAGS_DEBUG_LUMA_RECOVERY:
            output = saturate(0.5f + lumaCorrection / max(GetLuminance(rr), 1e-3f));
            break;
        // Red: applied unsupported-albedo blend weight. Green/blue: dimmed scene for orientation.
        case FLAGS_DEBUG_ALBEDO_TRUST:
        {
            const float scene = 0.35f * GetLuminance(rr) / (1.0f + GetLuminance(rr));
            output = float3(UnsupportedAlbedoWeight(p), scene, scene);
            break;
        }
        case FLAGS_DEBUG_DIRECT_SPECULAR:
            output = IsSet(FLAGS_SPECULAR_SIGNAL_DISABLED) ? float3(1, 0, 1) :
                (UnsupportedAlbedoRecovery > 0.0f ? float3(InDirectSpecularDenoised[p].rgb) :
                 IsSet(FLAGS_EXTRA_SPECULAR) ? float3(InDirectSpecularDenoised[p].rgb) * SpecularMultiplier(p) :
                 !IsSet(FLAGS_SPECULAR_SIGNAL_INDIRECT) ? float3(InIndirectSpecular[p].rgb) * SpecularMultiplier(p) : float3(1, 0, 1));
            break;
        case FLAGS_DEBUG_INDIRECT_SPECULAR:
            output = !IsSet(FLAGS_SPECULAR_SIGNAL_DISABLED) && IsSet(FLAGS_SPECULAR_SIGNAL_INDIRECT)
                         ? float3(InIndirectSpecular[p].rgb) * SpecularMultiplier(p)
                         : float3(1, 0, 1);
            break;
        case FLAGS_DEBUG_DIRECT_DIFFUSE:
            output = !IsSet(FLAGS_DIFFUSE_SIGNAL_DISABLED) && !IsSet(FLAGS_DIFFUSE_SIGNAL_INDIRECT)
                         ? float3(InDirectDiffuse[p].rgb) * DiffuseMultiplier(p) : float3(1, 0, 1);
            break;
        case FLAGS_DEBUG_INDIRECT_DIFFUSE:
            output = IsSet(FLAGS_DIFFUSE_SIGNAL_DISABLED) ? float3(1, 0, 1) :
                (IsSet(FLAGS_EXTRA_DIFFUSE) ? float3(InIndirectDiffuseDenoised[p].rgb) * DiffuseMultiplier(p) :
                 IsSet(FLAGS_DIFFUSE_SIGNAL_INDIRECT) ? float3(InDirectDiffuse[p].rgb) * DiffuseMultiplier(p) : float3(1, 0, 1));
            break;
        }
    }
    OutColor[p] = half4(FloorRadiance(output), 1);
#endif
#endif
}
