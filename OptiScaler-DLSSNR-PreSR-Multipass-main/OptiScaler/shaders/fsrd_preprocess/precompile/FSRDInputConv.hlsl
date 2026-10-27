// FSR-RR Conversion & Packing Shader
#include "FSRDPreprocessCommon.hlsli"

#define MainRS \
    "RootFlags(0), " \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 16), visibility = SHADER_VISIBILITY_ALL), " \
    "DescriptorTable(UAV(u0, numDescriptors = 8), visibility = SHADER_VISIBILITY_ALL), "

// Dispatch config
#define THREAD_GROUP_SIZE_X     8
#define THREAD_GROUP_SIZE_Y     8
#define NUM_THREADS             (THREAD_GROUP_SIZE_X * THREAD_GROUP_SIZE_Y)

static const uint2 s_ThreadGroupSize = uint2(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y);
// Largest distance representable in FP16. RR's docs describe the indirect hit
// distance as "must be valid if the signal is active; otherwise negative", which reads
// per-pixel - but AMD's own sample settles it the other way: with the signal enabled
// it writes this value for sky pixels and for untraced checkerboard pixels alike
// (trace_rays_denoiser.hlsl), and never writes a negative. "Active" means the signal
// is enabled at context creation, not that a particular pixel carries a ray.
//
// So a pixel with no ray information takes a very distant hit rather than a negative,
// which is also the physically sensible reading: a miss is not an absence of light, it
// is light arriving from infinity.
static const float s_MaxRayHitDistance = 65504.0f;
static const float s_MissingDiffuseHitDistance = s_MaxRayHitDistance;

// The specular path instead publishes a negative when the title supplies no hit
// distance at all. That predates this note and does not match the sample; see the
// comment at the specular write below.
static const float s_InvalidSpecularHitDistance = -1.0f;
static const float s_Type1RoughnessThreshold = 0.5f / 1023.0f;

// The same floor as a runtime value, so the trade it makes can be tuned.
//
// Flooring the demodulation divisor caps the gain on dark surfaces, but it also breaks the
// demodulate/remodulate round trip: whatever the floored divisor could not represent is
// handed to the skip signal, which reaches the screen without passing the denoiser. On a
// noisy input that remainder is noise, so a high floor trades amplified noise inside the
// denoiser for unfiltered noise beside it. Which side of that trade is cheaper depends on the
// title, so the value is exposed rather than fixed.

// Five-tap Gaussian run along the steered axis. Deliberately broad: the whole point
// of steering is that a long kernel is safe once it is known not to cross an edge.

// Flags

#define FLAGS_LINEAR_DEPTH              (1 << 1)

#define FLAGS_PACKED_ROUGHNESS          (1 << 2)
#define FLAGS_NEGATIVE_VIEW_DEPTH       (1 << 3)
#define FLAGS_HAS_SPEC_HIT_DISTANCE     (1 << 4)
#define FLAGS_SPECULAR_SIGNAL_INDIRECT  (1 << 5)
#define FLAGS_HAS_EMISSIVE_INPUT        (1 << 6)
#define FLAGS_FLOOR_HANDOVER       (1 << 7)
#define FLAGS_MOTION_VECTORS_JITTERED   (1 << 8)
#define FLAGS_DISPLAY_RESOLUTION_MOTION (1 << 9)
#define FLAGS_NORMALS_VIEW_SPACE        (1 << 11)
#define FLAGS_HAS_COMBINED_SPEC_HIT_DISTANCE (1 << 14)
// Title-published optional inputs. Each is inert unless the resource was validated.
#define FLAGS_TITLE_LINEAR_DEPTH       (1 << 12)
#define FLAGS_HAS_RESPONSIVITY_MASK    (1 << 13)
// InBiasMask holds a real DLSS bias-current-color mask rather than an unused binding.
#define FLAGS_HAS_BIAS_MASK            (1 << 15)
// Diagnostic: right half of the frame runs with the floor disabled.
// Debug Flags
#define FLAGS_DEBUG                     (1 << 16)
#define FLAGS_DEBUG_MODE_MASK           (0xFF << 16)

// Inputs
#define FLAGS_DEBUG_IN_SPEC_HIT_DIST    (1 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_IN_MOTION           (2 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_IN_NORMALS          (3 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_IN_ROUGHNESS        (4 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_IN_DIFF_ALBEDO      (5 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_IN_SPEC_ALBEDO      (6 << 17 | FLAGS_DEBUG)

// Outputs
#define FLAGS_DEBUG_OUT_SIGNAL_SPLIT    (7 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_OUT_LINEAR_DEPTH    (8 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_OUT_MOTION          (9 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_OUT_NORMALS         (10 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_OUT_SPEC_ALBEDO     (11 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_OUT_DIFF_ALBEDO     (12 << 17 | FLAGS_DEBUG)

#define FLAGS_DEBUG_OUT_DEPTH_DELTA     (13 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_NORM_DEPTH          (14 << 17 | FLAGS_DEBUG)

#define FLAGS_DEBUG_ALBEDO_OVERSHOOT    (15 << 17 | FLAGS_DEBUG)

// Value 16 was the floor variance view. It visualised the seed's instability channel, which
// is no longer published now the floor confidence gate is gone (see FSRDFloorSeed.hlsl), so
// the view read a constant and went with the channel.
#define FLAGS_DEBUG_FLOOR_COLOR         (17 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_RAW_INDIRECT_SPEC   (18 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_EFFECTIVE_ROUGHNESS (19 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_RAW_ROUGHNESS       (20 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_EMISSIVE_MASK       (21 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_APPLIED_ROUGHNESS_FLOOR (22 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_RESOURCE_INSPECTOR  (23 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_MATERIAL_TYPE       (24 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_IN_EMISSIVE         (25 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_RR_MATERIAL_TYPE    (26 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_ALBEDO_STRUCTURE    (27 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_ZERO_ROUGH_FLOOR    (28 << 17 | FLAGS_DEBUG)

// Optional-input validation views
// Value 30 was the removed reflected-image motion disagreement view; it now shows the specular
// signal's share of the demodulated split. Value 29 went with the input itself.
#define FLAGS_DEBUG_SPECULAR_SPLIT      (30 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_IN_TITLE_DEPTH      (31 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_TITLE_DEPTH_DIFF    (32 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_IN_RESPONSIVITY     (33 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_IN_BIAS_MASK        (34 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DEMOD_GAIN          (35 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_HIT_DIST_GATE       (36 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_DENOISER_FRACTION   (37 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_FLOOR_STRUCTURE     (38 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_SKIP_UNMAPPED       (39 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_SKIP_FLOOR          (40 << 17 | FLAGS_DEBUG)
#define FLAGS_DEBUG_SKIP_RAW_INJECT     (41 << 17 | FLAGS_DEBUG)

// DLSS-RR Inputs
Texture2D<half3> InColor : register(t0); // RGB - NVSDK_NGX_Parameter_Color
Texture2D<float> InDepth : register(t1); // R - NVSDK_NGX_Parameter_Depth - hardware or linear - inverted or not
Texture2D<float3> InMotionVectors : register(t2); // RG - NVSDK_NGX_Parameter_MotionVectors
Texture2D<float4> InNormals : register(t3); // RGB: Normals, A: Roughness (Optional) - NVSDK_NGX_Parameter_GBuffer_Normals
Texture2D<float> InRoughness : register(t4); // R - May be packed in normals. NVSDK_NGX_Parameter_GBuffer_Roughness
Texture2D<float> InSpecHitDist : register(t5); // R - NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance
Texture2D<half3> InDiffAlbedo : register(t6); // RGB - NVSDK_NGX_Parameter_GBuffer_DiffuseAlbedo
Texture2D<half3> InSpecAlbedo : register(t7); // RGB - NVSDK_NGX_Parameter_GBuffer_SpecularAlbedo
Texture2D<half> InBiasMask : register(t8);

Texture2D<half4> InFloorColor : register(t9);
Texture2D<float4> InInspector : register(t10);
Texture2D<float4> InEmissive : register(t11); // Optional NVSDK_NGX_Parameter_GBuffer_Emissive probe
// Optional combined ray-direction resource carrying the specular hit distance in A.
Texture2D<float4> InSpecularRayDirectionHitDistance : register(t12);
// Optional diffuse ray length. Read as .r or .a depending on DiffuseHitDistanceMode.
Texture2D<float4> InDiffuseHitDistance : register(t13);
// Optional already-linearised view depth published by the title.
Texture2D<float> InTitleLinearDepth : register(t14);
// Optional per-pixel responsivity hint. Polarity is a title property, so it is a
// runtime parameter rather than something this shader may assume.
Texture2D<float4> InResponsivityMask : register(t15);

// RR 1.2 typed signals. Resource order matches Conversion::SignalResources.
RWTexture2D<half4> OutIndirectSpecular : register(u0); // RGB: demodulated radiance, A: hit distance
RWTexture2D<half4> OutDirectDiffuse : register(u1);    // RGB: demodulated radiance, A: diffuse ray hit distance

// ffxDispatchDescDenoiser
// RG: unjittered PreviousUV-CurrentUV, B: corresponding-surface depth delta.
RWTexture2D<half4> OutMotion : register(u2);
RWTexture2D<half4> OutNormals : register(u3); // RG: Octahedrally encoded normals, B: Linear Roughness, A: Material Type (Optional)
RWTexture2D<half4> OutSpecAlbedo : register(u4); // RGB: Specular Albedo, A: dot(Normal, ViewDir)
RWTexture2D<half4> OutDiffAlbedo : register(u5); // RGB: Diffuse Albedo, A: Metalness (not provided)

RWTexture2D<half4> OutSkipSignal : register(u6);

// RGB: the finished handover image for this pixel, A: 1 on handed-over pixels and 0
// everywhere else. Composition reads the weight rather than re-deriving which pixels
// qualify, so the type-1 classification lives in exactly one place.
RWTexture2D<half4> OutHandover : register(u7);

cbuffer CB_Packing : register(b0)
{
    float4x4 InvViewMatrix; // DLSSD WorldToView^-1
    float4x4 InvProjMatrix; // DLSSD ViewToClip^-1
    float4x4 PrevViewMatrix; // DLSSD WorldToView from last frame

    float4 DstTexSize; // Resolution of inputs
    float4 MotionInputSize; // XY: source extent - ZW: reciprocal extent
    float4 MotionTransform; // XY: raw source motion -> UV displacement
    float4 JitterOffsets; // XY: current pixels - ZW: previous pixels

    uint4 InputBase0; // XY: color, ZW: motion
    uint4 InputBase1; // XY: normals, ZW: roughness
    uint4 InputBase2; // XY: spec hit distance, ZW: diffuse albedo
    uint4 InputBase3; // XY: specular albedo, ZW: bias mask
    uint4 InputBase4; // XY: emissive, ZW: reserved
    uint4 InputBase5; // XY: reserved, ZW: specular hit-distance source

    float NearPlane;
    float FarPlane;   
    
    float FloorIsolation;
    float RoughnessFloor;

    uint Flags;
    uint InspectorChannel;
    float InspectorScale;

    float DebugDepthMax;
    // 0 = absent, 1 = scalar in R, 2 = combined ray-direction resource, distance in A.
    uint DiffuseHitDistanceMode;

    // Blends the handover between the isotropic floor (0) and the rank filter (1).
    float FloorHandoverDetail;
    // 0 = off, 1 = zero-roughness pixels only, 2 = every pixel.
    uint FloorHandoverMode;

    // The responsivity hint is inert at zero. It reads the title's per-pixel statement about
    // where the denoiser's temporal history cannot be trusted, whose polarity ResponsivityInvert
    // selects.
    float ResponsivityTrustThreshold;
    uint ResponsivityInvert;
    // Was the trust threshold of the removed reflected-image motion field. The slot is retained
    // so that every parameter below it keeps the offset this cbuffer and the C++ struct agree
    // on; a 4-byte mismatch here would silently shift all of them and the size assert would not
    // see it.
    float _Padding0;
    float _Padding1;

    // Fraction of the DLSS bias mask applied when routing pixels around the denoiser.
    // 0 reproduces the previous behaviour exactly.
    float BiasMaskStrength;

    // Smoothing radius on the floor/raw clamp. 0 reproduces the exact min().
    float FloorSoftMin;

    // Scales the floor handover weight. 1.0 is the full graft.
    float FloorHandoverStrength;

    // Strength of the per-sample floor/raw ceiling clamp. 0 - the default - disables it and
    // closes the residual instead; above 0 it is restored, with the ceiling taken from a low
    // pass of the raw as far as the guide allows.
    float FloorClampSmoothing;

    // Scales the raw-preserving blend inside the floor. 0 is a pure spatial floor and 1.0 the
    // full blend; exposed so the blend's contribution to the image can be removed outright.
    float FloorRawBlend;

    // How far the guide structure gate suppresses that blend on flat surfaces.
    // 0 reproduces the ungated behaviour, 1.0 is fully gated.
    float FloorStructureGate;

    // Floor on the albedo used as the demodulation divisor. Higher caps the gain on dark
    // surfaces but hands more of the pixel to the unfiltered skip signal.
    float DemodDivisorFloor;
};

bool IsSet(uint mask) { return (Flags & mask) == mask; }
uint GetDebugMode() { return (Flags & FLAGS_DEBUG_MODE_MASK); }

int2 GetMotionSourcePx(uint2 px)
{
    float2 localPos = float2(px);
    if (IsSet(FLAGS_DISPLAY_RESOLUTION_MOTION))
    {
        // Match FSR's high-resolution MV addressing: remove current jitter before
        // mapping the render pixel center into the display-resolution grid.
        const float2 unjitteredCenter = float2(px) + 0.5f - JitterOffsets.xy;
        localPos = floor((unjitteredCenter * DstTexSize.zw) * MotionInputSize.xy);
    }

    const int2 maxLocal = max(int2(MotionInputSize.xy) - 1, 0);
    return clamp(int2(localPos), 0, maxLocal) + int2(InputBase0.zw);
}

float3 GetCanonicalMotionUv(uint2 px)
{
    const float2 rawMotion = InMotionVectors[GetMotionSourcePx(px)].rg;
    float2 motionUv = rawMotion * MotionTransform.xy;
    if (IsSet(FLAGS_MOTION_VECTORS_JITTERED))
    {
        const float2 jitterCancellationUv =
            (JitterOffsets.zw - JitterOffsets.xy) * DstTexSize.zw;
        motionUv -= jitterCancellationUv;
    }

    // Do not reinterpret invalid vectors as stationary history. Retain a zero XY
    // fallback for RR; the caller uses the third helper component to decide whether
    // OutMotion.a may publish an independent predicted previous-surface depth.
    const bool valid = all(isfinite(rawMotion)) && all(isfinite(motionUv)) &&
        all(abs(motionUv) <= 16.0f);
    return float3(valid ? motionUv : 0.0f, valid ? 1.0f : 0.0f);
}

// The reflected-image motion input, and the routing it drove, are gone: measured on the one
// title that published the input, the disagreement it reported tracked the camera's speed
// rather than any misalignment - 0% of the frame routed while still, 67% while moving - and
// every routed pixel had its specular radiance republished unfiltered, which put the current
// frame's raw noise on screen. See docs/007FirstLight_PT_Unlock_RE_notes.md.

float3 GetViewSpacePos(const int2 px)
{
    // InDepth is the floor seed's canonical signed-linear depth, produced from the
    // title's published linear depth whenever one was bound and otherwise from the
    // game's own depth. Every consumer of view-space position - this reconstruction,
    // the floor passes' depth guide and the denoiser's own depth input - reads that
    // one field. Re-canonicalising the title's raw resource here instead handed this
    // pass different geometry from the rest of the chain wherever the derived depth
    // disagreed with it: motion-Z and view positions built on one depth while RR
    // reprojected against the other.
    float inDepth = InDepth[px];
    // InvProjMatrix is unjittered, while px addresses the current jittered
    // raster. Remove the current pixel jitter before reconstructing the ray.
    const float2 uv = (float2(px) + 0.5 - JitterOffsets.xy) * DstTexSize.zw;
    const float depthSign = IsSet(FLAGS_NEGATIVE_VIEW_DEPTH) ? -1.0f : 1.0f;
    float3 viewSpacePos;

    [branch]
    if (IsSet(FLAGS_LINEAR_DEPTH))
    {
        // InDepth is the signed-linear output of FloorSeed. Scale the complete
        // view ray so XY and Z describe one internally consistent position.
        inDepth = depthSign * clamp(abs(inDepth), NearPlane, FarPlane);
        // Any non-degenerate NDC depth yields the same ray, because the result is
        // rescaled to inDepth below. Mid-range is the only choice that stays away
        // from the inverse projection's w == 0 singularity for every near/far
        // convention: standard-Z infinite is singular at 1.0, reversed-Z at 0.0.
        viewSpacePos = InvProjectPosition(float3(uv, 0.5f), InvProjMatrix);
        const float safeRayZ = (viewSpacePos.z < 0.0f)
            ? min(viewSpacePos.z, -1e-6f)
            : max(viewSpacePos.z, 1e-6f);
        viewSpacePos *= inDepth / safeRayZ;
        viewSpacePos.z = inDepth;
    }
    else
    {
        // Retained as a defensive fallback for direct hardware-depth callers.
        viewSpacePos = InvProjectPosition(float3(uv, inDepth), InvProjMatrix);
        const float signedDepth = depthSign * clamp(abs(viewSpacePos.z), NearPlane, FarPlane);
        const float safeViewZ = (viewSpacePos.z < 0.0f)
            ? min(viewSpacePos.z, -1e-6f)
            : max(viewSpacePos.z, 1e-6f);
        viewSpacePos *= signedDepth / safeViewZ;
        viewSpacePos.z = signedDepth;
    }

    return viewSpacePos;
}

float3 GetWorldSurfaceNormalAt(int2 px)
{
    px = clamp(px, int2(0, 0), int2(DstTexSize.xy) - 1);
    float3 normal = InNormals[px + int2(InputBase1.xy)].xyz;
    if (IsSet(FLAGS_NORMALS_VIEW_SPACE))
        normal = mul((float3x3)InvViewMatrix, normal);

    const float normalLengthSq = dot(normal, normal);
    return isfinite(normalLengthSq) && normalLengthSq > 1e-8f
        ? normal * rsqrt(normalLengthSq)
        : float3(0.0f, 0.0f, 1.0f);
}

float GetRawRoughnessAt(int2 px)
{
    px = clamp(px, int2(0, 0), int2(DstTexSize.xy) - 1);
    const float4 normal = InNormals[px + int2(InputBase1.xy)];
    return saturate(IsSet(FLAGS_PACKED_ROUGHNESS)
        ? normal.a
        : InRoughness[px + int2(InputBase1.zw)]);
}

// Raw colour for a render-space pixel, taken from the title's colour subrect.
//
// The clamp runs on the render-space coordinate and the origin is added after it, in that
// order - the same two steps LoadLumaWindow5x5 takes. Clamping the texture coordinate instead
// bounds it by the render size, and that range is anchored at the texture origin: with a
// non-zero subrect the neighbourhood is read from pixels the render does not cover, and at the
// subrect's right and bottom edges the taps fold back into its interior instead of stopping at
// the edge.
half3 GetRawColorAt(int2 px)
{
    px = clamp(px, int2(0, 0), int2(DstTexSize.xy) - 1);
    return InColor[px + int2(InputBase0.xy)].rgb;
}

// The albedo outputs are 8-bit UNORM, so the value composition remodulates with is not the
// value this shader computed - it is round(x * 255) / 255. Demodulating against the computed
// value and closing the residual against it assumes a round trip the texture cannot perform,
// and the error is largest where the albedo is smallest: an albedo of 0.01 stores as 3/255,
// which returns 17.6% more light than the residual accounted for, and that surplus is added
// rather than filtered because the denoiser never saw it.
//
// Quantizing here, before the value is used for anything, makes the demodulation divisor,
// the residual closure and the stored texel one number, so an identity denoiser returns
// exactly what was demodulated. Saturating is part of the storage contract as well: a UNORM
// texel cannot hold a reflectance above 1, and the arithmetic has to agree with the texel
// rather than with the title's value.
//
// The level count is 2^bits - 1 for FSRDFormats::SpecAlbedo / DiffAlbedo in
// FSRDPreprocessor_Dx12.cpp; verify_fsrd_mirrors.py ties the two together, and the C++
// static_assert beside those formats fails the build if either stops being 8-bit UNORM.
static const float s_AlbedoStoreLevels = 255.0f;

float3 QuantizeStoredAlbedo(float3 albedo)
{
    // Every result is an exact multiple of 1/255, which is what makes the store lossless:
    // the FP16 conversion and the UNORM round-to-nearest that follow both land back on it.
    return round(saturate(albedo) * s_AlbedoStoreLevels) / s_AlbedoStoreLevels;
}

// Measures whether the title-provided albedos contain spatial material structure in
// the same surface/roughness neighborhood. This is diagnostic only: it neither
// synthesizes an albedo nor changes the signals sent to RR. Coherent vending-screen
// texture should appear bright, while a constant-white display albedo remains dark.
float GetAlbedoStructure(
    int2 centerPx, float centerDepth, float3 centerNormal,
    float centerRoughness, float3 centerSpecular, float3 centerDiffuse)
{
    const float centerSpecularLuma = GetLuminance(centerSpecular);
    const float centerDiffuseLuma = GetLuminance(centerDiffuse);
    const float depthTolerance = max(0.01f, abs(centerDepth) * 0.005f);
    float maximumDelta = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 samplePx = clamp(
                centerPx + int2(x, y), int2(0, 0), int2(DstTexSize.xy) - 1);
            const float sampleRoughness = GetRawRoughnessAt(samplePx);
            const float sampleDepth = GetViewSpacePos(samplePx).z;
            const float3 sampleNormal = GetWorldSurfaceNormalAt(samplePx);
            const bool sameSurface = isfinite(sampleDepth) &&
                abs(sampleDepth - centerDepth) <= depthTolerance &&
                dot(sampleNormal, centerNormal) >= 0.98f &&
                abs(sampleRoughness - centerRoughness) <= max(
                    2.0f / 1023.0f, centerRoughness * 0.05f);

            if (sameSurface)
            {
                const int2 sampleSpecPx = samplePx + int2(InputBase3.xy);
                const int2 sampleDiffPx = samplePx + int2(InputBase2.zw);
                const float sampleSpecularLuma = GetLuminance(max(
                    GetSafeFP16(InSpecAlbedo[sampleSpecPx].rgb), 0.0f));
                const float sampleDiffuseLuma = GetLuminance(max(
                    GetSafeFP16(InDiffAlbedo[sampleDiffPx].rgb), 0.0f));
                maximumDelta = max(maximumDelta, max(
                    abs(sampleSpecularLuma - centerSpecularLuma),
                    abs(sampleDiffuseLuma - centerDiffuseLuma)));
            }
        }
    }

    return saturate(maximumDelta * 8.0f);
}

void LoadLumaWindow5x5(int2 centerPx, out float window[25])
{
    const int2 maxBounds = int2(DstTexSize.xy) - 1;
    const int2 base = int2(InputBase0.xy);

    [unroll]
    for (int wy = 0; wy < 5; ++wy)
    {
        [unroll]
        for (int wx = 0; wx < 5; ++wx)
        {
            const int2 tapPx = clamp(
                centerPx + int2(wx - 2, wy - 2), int2(0, 0), maxBounds) + base;
            window[wy * 5 + wx] = GetLuminance((float3) GetSafeFP16(InColor[tapPx].rgb));
        }
    }
}

float3 GetAdaptiveRankFloor(int2 centerPx, float3 centerColor, float centerLuma)
{
    float window[25];
    LoadLumaWindow5x5(centerPx, window);

    const float center = window[12];

    // Inner 3x3.
    const float innerMedian = Median9(
        window[6], window[7], window[8],
        window[11], window[12], window[13],
        window[16], window[17], window[18]);

    // Extremes of the NEIGHBOURS, with the centre excluded.
    //
    // The question this filter asks is whether the centre is an outlier relative to
    // its surroundings, so the surroundings are the population and the centre is the
    // sample being tested. Including the centre makes the test partly circular: a
    // stroke peak is by definition the brightest pixel of its own neighbourhood, so a
    // window that counts it would find the centre equal to the maximum and reject the
    // very structure the filter exists to keep.
    float innerMin = window[6];
    float innerMax = window[6];
    [unroll]
    for (int iy = 1; iy <= 3; ++iy)
    {
        [unroll]
        for (int ix = 1; ix <= 3; ++ix)
        {
            if (ix == 2 && iy == 2)
                continue;

            const float v = window[iy * 5 + ix];
            innerMin = min(innerMin, v);
            innerMax = max(innerMax, v);
        }
    }

    // Full 5x5. The exact median of 25 needs a 131-comparator network, so this stays
    // an approximation - but a symmetric one.
    //
    // Median-of-row-medians alone is anisotropic: it resolves horizontal structure
    // differently from vertical, because rows are collapsed first and columns never
    // are. On a vertical glyph stem every row median lands off-stem and the estimate
    // is pulled away from the stem entirely, which matters here because this value is
    // not only a decision input - it is the replacement published for any pixel the
    // filter escalates to. Folding in the column pass and the exact inner median
    // cancels that orientation bias for the cost of five more Median5 calls.
    float rowMedians[5];
    float colMedians[5];
    [unroll]
    for (int ry = 0; ry < 5; ++ry)
    {
        rowMedians[ry] = Median5(
            window[ry * 5 + 0], window[ry * 5 + 1], window[ry * 5 + 2],
            window[ry * 5 + 3], window[ry * 5 + 4]);
        colMedians[ry] = Median5(
            window[0 * 5 + ry], window[1 * 5 + ry], window[2 * 5 + ry],
            window[3 * 5 + ry], window[4 * 5 + ry]);
    }

    const float outerMedian = Median3(
        Median5(rowMedians[0], rowMedians[1], rowMedians[2], rowMedians[3], rowMedians[4]),
        Median5(colMedians[0], colMedians[1], colMedians[2], colMedians[3], colMedians[4]),
        innerMedian);

    float outerMin = window[0];
    float outerMax = window[0];
    [unroll]
    for (int oi = 1; oi < 25; ++oi)
    {
        if (oi == 12)
            continue;

        outerMin = min(outerMin, window[oi]);
        outerMax = max(outerMax, window[oi]);
    }

    // How far each median sits from the extremes of its own window, as a fraction of
    // that window's range.
    //
    // A median pinned against an extreme means the window is mostly noise and the
    // median describes it badly; a centred one means the population is well resolved.
    // The classic algorithm makes this a hard yes/no, but the inputs are noisy, so a
    // pixel sitting near the boundary flips stages between frames - and with nothing
    // temporal in this path that reads as flicker. Grading the confidence lets the two
    // stages blend through the ambiguous band instead of snapping across it.
    //
    // Confidence grades the median, not the centre: a median pinned against an
    // extreme can still describe a window that contains the centre perfectly.
    const float innerRange = max(innerMax - innerMin, 1e-5f);
    const float outerRange = max(outerMax - outerMin, 1e-5f);

    const float innerConfidence = saturate(
        2.0f * min(innerMedian - innerMin, innerMax - innerMedian) / innerRange);
    const float outerConfidence = saturate(
        2.0f * min(outerMedian - outerMin, outerMax - outerMedian) / outerRange);

    // Acceptance by magnitude, not by rank.
    //
    // An impulse is a value separated from its population, not merely the largest of
    // its neighbours - and a stroke peak is always the largest of its neighbours. A
    // rank test cannot tell those apart, so it flattens every stroke peak in the
    // image while claiming to remove noise.
    //
    // The margin is a fraction of the neighbourhood's own range, which makes the test
    // self-scaling in the way this content needs: over flat panel background the
    // range collapses and the margin with it, so an isolated firefly is still
    // rejected outright, while across a stroke the range spans background to ink and
    // the peak is comfortably inside a generous margin. Nothing here needs an
    // absolute threshold, which is what previous attempts kept failing to calibrate.
    static const float s_ImpulseMargin = 0.5f;

    const float innerMargin = s_ImpulseMargin * innerRange;
    const bool innerAccepts =
        center >= innerMin - innerMargin && center <= innerMax + innerMargin;
    const float innerResult = innerAccepts ? center : innerMedian;

    const float outerMargin = s_ImpulseMargin * outerRange;
    const bool outerAccepts =
        center >= outerMin - outerMargin && center <= outerMax + outerMargin;
    const float outerResult = outerAccepts ? center : outerMedian;

    // A centre both stages accepted is kept outright. Acceptance and confidence
    // answer different questions: both medians can still sit on the background
    // with zero confidence - a glyph stem one pixel wide in flat ink is exactly
    // that window - and the escalation blend below would publish that background
    // over the accepted centre. Only an escalated pixel rides the confidence:
    // the 3x3 answer is preferred wherever it is well resolved, the 5x5 takes
    // over as that confidence falls, and when neither window resolves a
    // trustworthy median the coarse median is all that is left.
    const float result = innerAccepts && outerAccepts
        ? center
        : lerp(
            lerp(outerMedian, outerResult, outerConfidence),
            innerResult,
            innerConfidence);

    // Luma-to-colour reconstruction.
    //
    // The gain is centreLuma-relative, so the denominator needs a guard - but clamping
    // the denominator rescales everything below the clamp: a constant 1e-5 field the
    // filter leaves untouched would come back at a tenth of its input, breaking the
    // keep-what-is-not-an-outlier promise exactly where that promise matters most. So
    // an unchanged centre skips the ratio and returns the centre colour outright -
    // exact unit gain at any positive luma - and only an escalated pixel divides.
    // Black gets its own guard there: a centre with no measurable luma has no chroma
    // to preserve, so the replacement luma publishes as grey instead of riding a
    // denominator clamp that would darken it.
    if (result == center)
        return centerColor;

    return centerLuma > 1e-6f
        ? centerColor * result * rcp(centerLuma)
        : result.xxx;
}

// Structure of the diffuse albedo at this pixel's scale, used to gate the raw-preserving
// blend. The blend asks whether the raw sample resembles the floor, and on a noisy input the
// noise answers that question itself: flat surfaces pass raw grain through while the floor
// stays smooth. Albedo answers it noiselessly, being material rather than illumination, so
// the gate trusts it where it carries information.
//
// Near-black albedo carries no material information (unlit billboards, very dark paint), so a
// confidence term falls back to the ungated behaviour there rather than treating an absence of
// evidence as evidence of flatness.
float GetGuideStructure(int2 centerPx, float centerAlbedoLuma)
{
    const int2 maxBounds = int2(DstTexSize.xy) - 1;
    const int2 diffBase = int2(InputBase2.zw);

    float minLuma = centerAlbedoLuma;
    float maxLuma = centerAlbedoLuma;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;

            const int2 tapPx = clamp(centerPx + int2(x, y), int2(0, 0), maxBounds);
            const float luma = GetLuminance(
                max((float3) GetSafeFP16(InDiffAlbedo[tapPx + diffBase].rgb), 0.0f));
            minLuma = min(minLuma, luma);
            maxLuma = max(maxLuma, luma);
        }
    }

    const float confidence = SoftAbove(centerAlbedoLuma, 0.02f, 0.015f);
    const float structure = saturate((maxLuma - minLuma) * 8.0f);
    return saturate(lerp(1.0f, saturate(structure), FloorStructureGate * confidence));
}

float3 GetGuideAlbedoAt(int2 px)
{
    px = clamp(px, int2(0, 0), int2(DstTexSize.xy) - 1);
    const float3 spec =
        max((float3) GetSafeFP16(InSpecAlbedo[px + int2(InputBase3.xy)].rgb), 0.0f);
    const float3 diff =
        max((float3) GetSafeFP16(InDiffAlbedo[px + int2(InputBase2.zw)].rgb), 0.0f);
    return spec + diff;
}

[RootSignature(MainRS)]
[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void CSMain(uint3 groupID : SV_GroupID, uint3 gtID : SV_GroupThreadID)
{
    const uint2 px = groupID.xy * s_ThreadGroupSize + gtID.xy;
    const float2 uv = (float2(px) + 0.5f) * DstTexSize.zw;
    
    if (px.x >= DstTexSize.x || px.y >= DstTexSize.y)
        return;

    // Albedo / reflectance
    //
    // Zeroed albedos are unusable sentinels and must be skipped.
    // Depth values at the far plane indicate a skybox or other skippable content.
    //
    // DLSS-RR specular albedo is hemispherical specular reflectance at (NoV, roughness).
    // Diffuse albedo is the diffuse component of reflectance.     
    const int2 colorPx = int2(px) + int2(InputBase0.xy);
    const int2 normalPx = int2(px) + int2(InputBase1.xy);
    const int2 roughnessPx = int2(px) + int2(InputBase1.zw);
    const int2 diffAlbedoPx = int2(px) + int2(InputBase2.zw);
    const int2 specAlbedoPx = int2(px) + int2(InputBase3.xy);

    // Retained unmodified for the albedo-structure diagnostic, which must read the
    // title's values rather than the emissive-compatibility rewrite below.
    const float3 inputSpecReflectance =
        max(GetSafeFP16(InSpecAlbedo[specAlbedoPx].rgb), 0.0f);
    const float3 inputDiffAlbedo =
        max(GetSafeFP16(InDiffAlbedo[diffAlbedoPx].rgb), 0.0f);
    float3 specReflectance = inputSpecReflectance;
    float3 diffAlbedo = inputDiffAlbedo;
    const float rawRoughness = saturate(
        IsSet(FLAGS_PACKED_ROUGHNESS)
            ? InNormals[normalPx].a
            : InRoughness[roughnessPx]);

    // R10G10B10A2_UNORM stores material type in two normalized alpha bits.
    // Select only roughness values that quantize to exact zero, avoiding the
    // unstable 1/1023 boundary. All exact-zero candidates share type 1.
    // Resolved here rather than at the normal write below because the floor split
    // needs it too.
    const float isZeroRoughness = step(rawRoughness, s_Type1RoughnessThreshold);

    const float totalAlbedo = dot(specReflectance.rgb + diffAlbedo.rgb, 1.0f);
    // Emissive reinterpretation, softened.
    //
    // As a hard step this flips per frame on any surface whose albedo sits near the
    // threshold - animated signage and video billboards in particular - and the two sides of
    // the branch demodulate very differently, so the classification itself becomes a source
    // of temporal instability. The transition band costs nothing.
    const float isEmissive = SoftAbove(totalAlbedo, 5.9f, 0.5f);
    // Emissive primary surfaces do not have a meaningful reflection hit distance.
    // Route their radiance through direct diffuse instead of disguising it as
    // indirect specular, where an invalid hit distance can deactivate denoising.
    static const float s_EmissiveSpecularWeight = 1e-4f;
    diffAlbedo.rgb = lerp(
        diffAlbedo.rgb, 1.0f - s_EmissiveSpecularWeight, isEmissive);
    specReflectance.rgb = lerp(
        specReflectance.rgb, s_EmissiveSpecularWeight, isEmissive);
    
    // Clamp albedo
    const float3 albedoOvershoot = max((specReflectance.rgb + diffAlbedo.rgb) - 1.0f, 0.0f);
    specReflectance.rgb = saturate(specReflectance.rgb - albedoOvershoot);
    diffAlbedo.rgb -= max((specReflectance.rgb + diffAlbedo.rgb) - 1.0f, 0.0f);

    // Everything downstream - the demodulation divisor, the residual closure, the albedos the
    // denoiser is handed and the ones composition remodulates with - has to be the value the
    // 8-bit texture holds, so the quantization is applied once here rather than modeled at
    // each use. A reflectance that rounds to zero is not a special case to guard against:
    // composition cannot remodulate it either, so its radiance belongs to the residual by the
    // same arithmetic that already routes it there, and the demodulation divisor carries its
    // own floor rather than depending on the albedo for one.
    specReflectance = QuantizeStoredAlbedo(specReflectance);
    diffAlbedo = QuantizeStoredAlbedo(diffAlbedo);
    
    // Denoiser input color and floor residual
    const float3 rawColor = GetSafeFP16(InColor[colorPx].rgb);
    const float rawLuma = GetLuminance(rawColor);
    float4 floorColor = InFloorColor[px];
    const float floorLuma = GetLuminance(floorColor.rgb);

    // The filtered floor before any blend toward raw. This is the median and a-trous
    // result on its own, which is what the zero-roughness handover below publishes.
    const float3 filteredFloor = min(rawColor, floorColor.rgb);

    // Floor color blending
    //
    // Diffuse dominant surfaces are relatively well behaved.
    const float avgSpecular = dot(specReflectance.rgb, 0.33f);
    const float diffuseDominance = smoothstep(0.08f, 0.0f, avgSpecular);
    const float similarityThreshold = lerp(0.5f, 0.2f, diffuseDominance);

    // Clamp floor to minimum and blend in raw values where similar to preserve microcontrast.
    const float floorSimilarity = GetRelativeSimilarity(floorLuma, rawLuma, similarityThreshold);
    const float isolation = FloorIsolation;

    // The raw-preserving blend is gated by the guide. Where the surface has structure the raw
    // sample carries detail the floor's own filtering would have flattened, so keeping it is
    // the point; where it does not, the only variation a noisy raw sample can contribute is
    // noise, and the gate refuses it. FloorRawBlend scales the whole term, so the floor can be
    // reduced to a pure spatial filter for comparison.
    const float guideStructure = GetGuideStructure(int2(px), GetLuminance(inputDiffAlbedo));
    const float rawBlend =
        saturate(floorSimilarity) * guideStructure * saturate(FloorRawBlend);

    floorColor.rgb = isolation * lerp(floorColor.rgb, rawColor, rawBlend);
    // Transparency / bias mask routing.
    //
    // InBiasMask was bound at t8 but never sampled. DLSS-RR marks here every pixel whose
    // colour should come from the current frame rather than from history: particles, alpha
    // layers, decals, and animated or video textures. The floor mechanism already provides
    // the right escape hatch - anything pushed into the floor is subtracted from the denoiser
    // input and re-added verbatim from the skip signal after denoising - so driving the floor
    // to the raw colour routes flagged content around the denoiser entirely.
    const float biasMask = IsSet(FLAGS_HAS_BIAS_MASK)
        ? saturate((float) InBiasMask[px + int2(InputBase3.zw)])
        : 0.0f;
    const float biasWeight = saturate(biasMask * BiasMaskStrength);
    floorColor.rgb = lerp(floorColor.rgb, rawColor, biasWeight);
    // Non-negative residual.
    //
    // The floor is subtracted from the raw to form the denoiser's input, so a floor above the raw
    // would ask the denoiser for negative radiance. That guard belongs on the residual, not on the
    // floor: clamping the floor down to the raw sample would republish the raw's noise through the
    // skip path on every pixel where the floor wins, which is what the opt-in ceiling clamp below
    // does when it is enabled. The closure here is deliberately hard - a knee would lift small
    // negative residuals above zero and publish those pixels on both paths at once.
    const float3 unclampedFloor = floorColor.rgb;
    float3 denoiserColor = max(0.0f, rawColor - unclampedFloor);

    // The per-sample ceiling is kept as an opt-in energy guard, off at zero, for a title whose
    // floor overshoots for a reason other than noise. The share it clamps away is exactly the
    // share it replaces with the raw sample, so it is also the control that puts the grain
    // back; the SkipRawInject view shows the pixels it takes.
    float3 rawInject = 0.0f;
    [branch]
    if (FloorClampSmoothing > 0.0f)
    {
        // A low pass of the raw is a less noisy ceiling than a single sample: it is what the
        // surface's neighbourhood supports rather than what one sample happened to read. Where
        // the guide says the surface carries structure the raw sample is detail worth keeping,
        // so the ceiling follows the guide between the two.
        const int2 localPx = int2(px);
        const half3 smoothedRaw = GetSafeFP16(
            (GetRawColorAt(localPx + int2(-1, -1)) +
             GetRawColorAt(localPx + int2( 0, -1)) * 2.0h +
             GetRawColorAt(localPx + int2( 1, -1)) +
             GetRawColorAt(localPx + int2(-1,  0)) * 2.0h +
             GetRawColorAt(localPx                     ) * 4.0h +
             GetRawColorAt(localPx + int2( 1,  0)) * 2.0h +
             GetRawColorAt(localPx + int2(-1,  1)) +
             GetRawColorAt(localPx + int2( 0,  1)) * 2.0h +
             GetRawColorAt(localPx + int2( 1,  1))) * (1.0f / 16.0f));

        const float clampSmoothing = saturate(FloorClampSmoothing) * (1.0f - guideStructure);
        const float3 clampCeiling = lerp(rawColor, (float3) smoothedRaw, clampSmoothing);
        // SoftMin dips up to k/4 below min(a, b) where its two candidates agree, and
        // both are near zero on genuinely black pixels. The dip would publish a negative
        // floor: the residual gains the light the floor lost, while the skip signal's
        // GetSafeFP16 silently zeroes the negative compensation - so a black input on
        // nonblack albedo brightens by up to k/4 after remodulation. The floor is
        // radiance, so the smoothed clamp may not push it below zero.
        const float3 clampedFloor = max(SoftMin(clampCeiling, unclampedFloor, FloorSoftMin), 0.0f);
        rawInject = unclampedFloor - clampedFloor;
        floorColor.rgb = clampedFloor;
        denoiserColor = max(0.0f, rawColor - clampedFloor);
    }

    // Which pixels take the floor handover, and how strongly.
    //
    // The graft combines RR's low frequencies with the floor's high frequencies in composition,
    // so unlike the floor split it takes nothing from the denoiser's input. That is what makes
    // it safe to widen past the exact-zero-roughness pixels it was written for. See the handover
    // section of docs/fsrd_pipeline_contract.md for why those pixels needed it and why a partial
    // handover was rejected.
    float floorHandover = 0.0f;
    if (IsSet(FLAGS_FLOOR_HANDOVER))
    {
        const float handoverMask = (FloorHandoverMode == 2u) ? 1.0f : isZeroRoughness;
        floorHandover = saturate(handoverMask * FloorHandoverStrength);
    }

    // The isotropic floor is the wrong tool for panel content: it is what removes the
    // text. Blend toward a directional median that keeps thin structure and leaves
    // non-outlier pixels at their original colour.
    float3 handoverColor = filteredFloor;
    [branch]
    if (floorHandover > 0.0f)
    {
        // One filter, not a menu of five. The rank filter was the only one of the set that
        // left a pixel which was not an outlier exactly as it found it, and the only one that
        // survived comparison against the others on real content; the averaging variants were
        // an A/B that had already answered its question.
        const float3 detailFloor = GetAdaptiveRankFloor(int2(px), rawColor, rawLuma);

        handoverColor = lerp(filteredFloor, detailFloor, saturate(FloorHandoverDetail));
    }

    // RR's input is never attenuated. The two paths meet in composition, where they
    // are separated by frequency rather than mixed by ratio, so RR stays on the
    // absolute scale its radiance clipping and learned noise priors expect.

    // Depth - full position needed for reprojected depth delta
    const float3 viewSpacePos = GetViewSpacePos(px);
    // An infinite far plane reaches this shader as FLT_MAX. Normalising a log
    // against it puts log(3.4e38) = 88.7 in the denominator, which squeezes every
    // realistic depth into the bottom tenth of the range: the depth debug views
    // read as one flat colour, and the far-plane skip test below can never reach
    // its 0.99 threshold, so skybox rejection silently stops working. Normalise
    // against a finite horizon so both behave on infinite projections.
    const float finiteFarPlane = min(FarPlane, 65504.0f);
    const float compressedDepth =
        log(abs(viewSpacePos.z) + 1.0f) / log(finiteFarPlane + 1.0f);
    
    if (((compressedDepth < 0.99f) && totalAlbedo > 1e-2f) || IsSet(FLAGS_DEBUG))
    {
        // RR consumes world-space normals. DLSS-RR does not carry normal-space
        // metadata, so the feature exposes an explicit per-title setting.
        float4 worldSurfaceNormal = InNormals[normalPx];
        if (IsSet(FLAGS_NORMALS_VIEW_SPACE))
        {
            worldSurfaceNormal.xyz =
                mul((float3x3)InvViewMatrix, worldSurfaceNormal.xyz);
        }
        const float normalLengthSq = dot(worldSurfaceNormal.xyz, worldSurfaceNormal.xyz);
        worldSurfaceNormal.xyz = isfinite(normalLengthSq) && normalLengthSq > 1e-8f
            ? worldSurfaceNormal.xyz * rsqrt(normalLengthSq)
            : float3(0.0f, 0.0f, 1.0f);
        const float2 octNormal = OctahedralEncode(worldSurfaceNormal.rgb);
    
        // DLSS-RR provides 3D normals
        // Linear roughness optionally included in the A channel, or in a separate single-channel 
        // buffer (InRoughness).
        // Emission and microsurface roughness are independent material properties.
        // Preserve the game's roughness instead of coercing emissive surfaces into
        // perfect mirrors solely to route their radiance through the specular signal.
        const float inputRoughness = rawRoughness;

        // isZeroRoughness is resolved above, where the floor split also consumes it.
        const float materialType = isZeroRoughness * (1.0f / 3.0f);

        // Exact-zero roughness makes RR treat the surface as a perfect mirror. The
        // optional compatibility floor lifts it to a value the denoiser can filter.
        const float appliedRoughnessFloor =
            saturate(RoughnessFloor) * isZeroRoughness;
        const float roughness = max(inputRoughness, appliedRoughnessFloor);
        
        // Output: RG=OctNormal, B=Roughness, A=MaterialID
        OutNormals[px] = GetSafeFP16(float4(octNormal, roughness, materialType));
   
        // Motion Vectors & Depth Delta. XY is canonicalized once into unjittered
        // PreviousUV-CurrentUV. RR's B contract is the previous-camera depth of the
        // corresponding current surface minus its current depth; sampling B from the
        // XY destination would make RR's own disocclusion test validate that target
        // against itself.
        const float3 worldSpacePos = mul(InvViewMatrix, float4(viewSpacePos, 1.0f)).xyz;
        float3 prevViewSpacePos = mul(PrevViewMatrix, float4(worldSpacePos, 1.0f)).xyz;

        const float3 canonicalMotion = GetCanonicalMotionUv(px);

        // Specular motion tracking handover.
        //
        // Scaling the ray length by roughness is a continuous handover with a defensible
        // meaning: a shorter virtual hit distance places the reflection nearer the surface,
        // so the specular reprojects with the surface as the weight goes to zero. As a hard
        // cut it toggles per pixel and per frame wherever roughness varies around the
        // threshold - clearcoat, wet asphalt, painted metal at grazing angles - and each
        // toggle discontinuously changes how RR reprojects that pixel.
        //
        // Bias-masked pixels are excluded: their colour is not a surface reflection and the
        // hit distance that comes with them is not meaningful.
        const float specularTracking =
            SoftBelow(roughness, 0.30f, 0.15f) * (1.0f - isEmissive) * (1.0f - biasWeight);

        // The title's own statement that the specular signal's temporal history cannot be
        // trusted. Inert at zero, so it does nothing until asked for.
        const float responsivityRaw = IsSet(FLAGS_HAS_RESPONSIVITY_MASK)
            ? InResponsivityMask[px].r
            : 0.0f;

        // How much of the specular radiance leaves the temporal path, 0..1, driven only by the
        // title's own responsivity hint. A reflected-image motion field used to drive it too and
        // was measured harmful; see the rejected list in docs/fsrd_pipeline_contract.md.
        float specularRouteWeight = 0.0f;

        // A responsivity hint is a statement rather than a gradient, so it hands the pixel
        // over whole.
        if (IsSet(FLAGS_HAS_RESPONSIVITY_MASK) && ResponsivityTrustThreshold > 0.0f)
        {
            const bool unstable = ResponsivityInvert != 0u
                ? responsivityRaw > ResponsivityTrustThreshold
                : responsivityRaw < ResponsivityTrustThreshold;
            specularRouteWeight = max(specularRouteWeight, unstable ? 1.0f : 0.0f);
        }

        // The specular ray length feeds RR's indirect-specular signal. It does not
        // alter RR's primary-surface motion.
        const float rawHitDist = IsSet(FLAGS_HAS_SPEC_HIT_DISTANCE)
            ? InSpecHitDist[int2(px) + int2(InputBase5.zw)]
            : (IsSet(FLAGS_HAS_COMBINED_SPEC_HIT_DISTANCE)
                ? InSpecularRayDirectionHitDistance[
                    int2(px) + int2(InputBase5.zw)].a
                : s_InvalidSpecularHitDistance);
        const bool hasInputHitDist =
            isfinite(rawHitDist) && rawHitDist >= 0.0f && rawHitDist <= 65504.0f;
        // The title's classification is preserved verbatim: a finite value is a geometry hit and
        // FP16-max is a real environment miss, and primary-surface depth is not a substitute for
        // either. Where a title publishes neither - 007 First Light does not - the indirect path
        // falls back to the primary surface's view distance, because RR writes no denoised output
        // at all for a signal with no finite ray length. See the title quirks in
        // docs/fsrd_pipeline_contract.md.
        const float reflectionHitDistance = hasInputHitDist
            ? rawHitDist
            : (IsSet(FLAGS_SPECULAR_SIGNAL_INDIRECT)
                ? max(abs(viewSpacePos.z), 1e-3f)
                : s_InvalidSpecularHitDistance);

        const float2 motionUv = canonicalMotion.xy;
        const float depthDelta = isfinite(prevViewSpacePos.z)
            ? prevViewSpacePos.z - viewSpacePos.z
            : 0.0f;

        const float3 motionOut = float3(motionUv, depthDelta);
        OutMotion[px] = half4(GetSafeSignedFP16(motionOut), 0.0f);

        const float3 specWeight = saturate(specReflectance.rgb);
        const float3 diffWeight = saturate(diffAlbedo.rgb);
        // Split the composited radiance between the two signals by reflectance ratio. Where
        // both are effectively zero the ratio is meaningless, so the pixel goes down the
        // diffuse path - it carries no reprojection state and cannot smear.
        const float3 totalWeight = diffWeight + specWeight;
        const float3 specFraction = specWeight * rcp(max(totalWeight, DemodDivisorFloor));
        const float3 isSplitValid =
            smoothstep(0.5f * DemodDivisorFloor, DemodDivisorFloor, totalWeight);

        const float3 specularColor = denoiserColor * (specFraction * isSplitValid);
        const float3 diffuseColor = denoiserColor - specularColor;

        // A pixel the title itself reports as unresponsive cannot be reprojected with the
        // primary motion. Publish it through the skip signal instead - composition adds that at
        // full sharpness from the current frame - and hand the denoiser zero radiance there, so
        // there is no misaligned history to smear. The pixel's energy is preserved exactly.
        const float3 routedRadiance = specularColor * specularRouteWeight;
        floorColor.rgb += routedRadiance;

        // Demodulate against a floored divisor. The remodulation below runs on the same
        // stored albedo composition will remodulate with, so the only gap left is the one the
        // floor creates - the share of the divisor it could not represent - and the residual
        // catches exactly that rather than a quantization error compounded by it.
        //
        // The divisor carries its own floor: an albedo that quantizes to zero is a legitimate
        // stored value, so nothing upstream is a promise of a finite divisor any more. This
        // mirrors the lower bound the feature clamps DemodDivisorFloor to.
        const float3 specDenom = max(max(specReflectance.rgb, DemodDivisorFloor), 1e-4f);
        const float3 diffDenom = max(max(diffAlbedo.rgb, DemodDivisorFloor), 1e-4f);

        const half3 demodSpecular =
            GetSafeFP16((specularColor - routedRadiance) / specDenom);
        const float demodGain = rcp(min(GetLuminance(specDenom), GetLuminance(diffDenom)));

        const half3 demodDiffuse = GetSafeFP16(diffuseColor / diffDenom);

        // Anything that cannot survive modulation and FP16 clamping remains in the skip signal.
        const float3 remodColor = (demodSpecular * specReflectance.rgb) + (demodDiffuse * diffAlbedo.rgb);
        // The share of the pixel that modulation could not represent. It travels around the
        // denoiser in the skip signal, which is what preserves the pixel's energy - and also
        // what puts it on screen unfiltered, so it is the first place to look when the skip
        // signal reads noisier than the floor it also carries.
        const float3 unmappedShare = max(0.0f, denoiserColor - remodColor - routedRadiance);
        // The routed radiance is already part of the floor colour by this point, so this is the
        // floor's own share of the skip signal rather than a second contribution to it.
        const float3 skipFloorShare = floorColor.rgb;

        // The routed radiance is already in the skip signal, so it is excluded here to keep
        // the split exactly energy conserving.
        floorColor.rgb += unmappedShare;

        // A finite value is a geometry hit and FP16-max is a real environment miss;
        // both are valid distances and are preserved verbatim.
        //
        // The negative fallback is the one value AMD's sample never emits - it writes
        // FP16-max even for pixels it did not trace. In practice the exposure is
        // small: the automatic specular classification only selects Indirect once a
        // validated hit-distance guide exists, and without one it selects Direct,
        // whose alpha the contract leaves undefined. A negative can therefore only
        // reach an Indirect dispatch on a per-pixel read that fails validation.
        //
        // RR's Virtual Hit Pos view reconstructs correctly on titles that supply the
        // guide, so this is a conformance gap rather than an observed fault.
        const half hitDist = IsSet(FLAGS_SPECULAR_SIGNAL_INDIRECT)
            ? half(reflectionHitDistance * specularTracking)
            : half(0.0f);

        [branch]
        if (!IsSet(FLAGS_DEBUG))
        {
            // Supply the real ray length whenever the title provides one. Without one
            // the pixel keeps the FP16-max miss, matching what AMD's sample writes for
            // its own untraced and sky pixels.
            float diffuseHitDist = s_MissingDiffuseHitDistance;
            [branch]
            if (DiffuseHitDistanceMode != 0u)
            {
                const int2 diffuseHitPx = int2(px) + int2(InputBase4.zw);
                const float4 diffuseHitSample = InDiffuseHitDistance[diffuseHitPx];
                const float rawDiffuseHit = DiffuseHitDistanceMode == 2u
                    ? diffuseHitSample.a
                    : diffuseHitSample.r;
                if (isfinite(rawDiffuseHit) && rawDiffuseHit >= 0.0f &&
                    rawDiffuseHit <= s_MaxRayHitDistance)
                {
                    diffuseHitDist = rawDiffuseHit;
                }
            }

            OutIndirectSpecular[px] = half4(demodSpecular, hitDist);
            OutDirectDiffuse[px] = half4(demodDiffuse, diffuseHitDist);
        }
        else
        {
            // A debug mode overwrites OutIndirectSpecular with the debug colour below,
            // but nothing writes u1. FLAGS_DEBUG also routes every pixel through this
            // branch, so the skip path no longer clears it either: without this write
            // the diffuse signal keeps the last non-debug frame for the whole session.
            OutDirectDiffuse[px] = half4(0.0f, 0.0f, 0.0f, s_MissingDiffuseHitDistance);
        }

        
        // How much of this pixel's radiance the denoiser receives through the specular signal
        // rather than the diffuse one. It is the albedo ratio's share of the demodulated split,
        // so it is the title's own material split rather than anything this shader decided, and
        // it is what the debug view and the probe both report.
        const float specularShare = saturate(GetLuminance(specularColor) *
                                            rcp(max(GetLuminance(denoiserColor), 1e-3f)));
        // The specular albedo's alpha has no consumer. Carry that share in it, so the probe can
        // report how much of the frame reaches the denoiser through the specular signal at all -
        // a signal it may then refuse to denoise if the ray-length guide is unusable.
        OutSpecAlbedo[px] = half4(GetSafeFP16(specReflectance), half(specularShare));
        // The diffuse albedo's alpha has no consumer. Carry whether this pixel's published floor
        // exceeds its raw sample, so the probe can report the share of the frame the floor path
        // takes over. That share is what decides how soft the image looks: on it the residual
        // collapses to zero and the skip signal is the filter's own low pass.
        const float floorCrossing = GetLuminance(rawColor) <= GetLuminance(unclampedFloor) ? 1.0f : 0.0f;
        OutDiffAlbedo[px] = half4(GetSafeFP16(diffAlbedo), half(floorCrossing));
        // Skip-signal alpha is the luminance of its own RGB - the composition pass
        // adds it to the denoised luminance to build the raw-correlation reference,
        // and the skip path below writes it that way. floorColor.rgb has been
        // rewritten three times since floorLuma was sampled (isolation blend, raw
        // clamp, unrepresentable residual), so derive it from the final colour
        // instead of shipping the stale raw-floor luminance.
        // Emissive rejoins here rather than in the denoised signal, so composition
        // restores it at full sharpness alongside the floor.
        const float3 safeFloorColor = GetSafeFP16(floorColor.rgb);
        OutSkipSignal[px] = half4(safeFloorColor, GetLuminance(safeFloorColor));

        OutHandover[px] = half4(GetSafeFP16(handoverColor), half(floorHandover));
        
        // Values the optional-input views below report, read once so every view shows
        // exactly what the logic above used. The title depth is no longer consumed by
        // any production path - the canonical copy in InDepth replaced it - so this is
        // strictly the raw published value the two title-depth views visualise.
        const float titleDepthRaw = IsSet(FLAGS_TITLE_LINEAR_DEPTH)
            ? InTitleLinearDepth[clamp(int2(px), int2(0, 0), int2(DstTexSize.xy) - 1) +
                                 int2(InputBase5.xy)]
            : 0.0f;

        [branch]
        if (IsSet(FLAGS_DEBUG))
        {
            float3 debugColor = float3(0, 0, 0);
        
            switch (GetDebugMode())
            {
                // Inputs
                case FLAGS_DEBUG_IN_SPEC_HIT_DIST:
                    // Magenta is an invalid/inactive indirect sample. Otherwise,
                    // logarithmic scaling keeps both nearby and distant hits
                    // visible without wrapping the color range.
                    debugColor = (hitDist < 0.0f)
                        ? float3(1.0f, 0.0f, 1.0f)
                        : TurboColormap(saturate(log2(max((float)hitDist, 0.0f) + 1.0f) / 16.0f));
                    break;
                
                case FLAGS_DEBUG_NORM_DEPTH:
                    debugColor = TurboColormap(compressedDepth);
                    break;
                
                case FLAGS_DEBUG_IN_MOTION:
                    debugColor = VisualizeMotionVec(motionUv * DstTexSize.xy, 0.1f);
                    break;
                
                case FLAGS_DEBUG_IN_NORMALS:
                    debugColor = worldSurfaceNormal.rgb * 0.5 + 0.5;
                    break;
                
                case FLAGS_DEBUG_IN_ROUGHNESS:
                    debugColor = inputRoughness;
                    break;
                
                case FLAGS_DEBUG_IN_DIFF_ALBEDO:
                    debugColor = InDiffAlbedo[diffAlbedoPx];
                    break;
                
                case FLAGS_DEBUG_IN_SPEC_ALBEDO:
                    debugColor = InSpecAlbedo[specAlbedoPx];
                    break;

                case FLAGS_DEBUG_IN_EMISSIVE:
                {
                    if (IsSet(FLAGS_HAS_EMISSIVE_INPUT))
                    {
                        const uint2 emissivePx = px + InputBase4.xy;
                        debugColor = InEmissive[emissivePx].rgb;
                    }
                    else
                    {
                        // Missing optional input. Magenta distinguishes absence from a valid black buffer.
                        debugColor = float3(1.0f, 0.0f, 1.0f);
                    }
                    break;
                }
                // Outputs
                case FLAGS_DEBUG_OUT_SIGNAL_SPLIT:
                    debugColor = demodSpecular + demodDiffuse;
                    break;
                
                case FLAGS_DEBUG_OUT_LINEAR_DEPTH:
                    // Linear against an adjustable full scale. The previous frac()
                    // encoding cycled every ten units, which aliases into a flat
                    // field at city depths and cannot be told apart from a depth
                    // buffer that is genuinely constant.
                    debugColor = TurboColormap(
                        saturate(abs(viewSpacePos.z) / max(DebugDepthMax, 1e-3f)));
                    break;
                
                case FLAGS_DEBUG_OUT_MOTION:
                    debugColor = VisualizeMotionVec(motionOut.xy * DstTexSize.xy, 0.1f);
                    break;

                case FLAGS_DEBUG_OUT_DEPTH_DELTA:
                    debugColor = VisualizeSignedDiff(motionOut.z, 5.0f);
                    break;
                
                case FLAGS_DEBUG_OUT_NORMALS:
                    debugColor = OctahedralDecode(octNormal) * 0.5 + 0.5;
                    break;

                case FLAGS_DEBUG_OUT_SPEC_ALBEDO:
                    debugColor = specReflectance.rgb;
                    break;
                
                case FLAGS_DEBUG_OUT_DIFF_ALBEDO:
                    debugColor = diffAlbedo.rgb;
                    break;

                case FLAGS_DEBUG_FLOOR_COLOR:
                    debugColor = InFloorColor[px].rgb;
                    break;

                case FLAGS_DEBUG_RAW_INDIRECT_SPEC:
                    // Match DenoisedSpecularSignal after remodulation.
                    debugColor = demodSpecular * specReflectance.rgb;
                    break;

                case FLAGS_DEBUG_EFFECTIVE_ROUGHNESS:
                    debugColor = roughness;
                    break;

                case FLAGS_DEBUG_RAW_ROUGHNESS:
                    debugColor = rawRoughness;
                    break;

                case FLAGS_DEBUG_EMISSIVE_MASK:
                    debugColor = isEmissive;
                    break;
                case FLAGS_DEBUG_APPLIED_ROUGHNESS_FLOOR:
                    debugColor = (appliedRoughnessFloor > 0.0f)
                        ? TurboColormap(saturate(
                            appliedRoughnessFloor / max(RoughnessFloor, 2.0f / 1023.0f)))
                        : 0.0f;
                    break;
                case FLAGS_DEBUG_RESOURCE_INSPECTOR:
                    debugColor = saturate(InInspector[px][min(InspectorChannel, 3u)] * InspectorScale);
                    break;

                case FLAGS_DEBUG_MATERIAL_TYPE:
                    // Unified type 1 = white, type 0 = black.
                    debugColor = isZeroRoughness.xxx;
                    break;

                case FLAGS_DEBUG_RR_MATERIAL_TYPE:
                    // RR-facing encoded material type. Type 0 = black, type 1 = red,
                    // type 2 = green, type 3 = blue.
                    debugColor = materialType < (0.5f / 3.0f)
                        ? 0.0f
                        : (materialType < (1.5f / 3.0f)
                            ? float3(1.0f, 0.0f, 0.0f)
                            : (materialType < (2.5f / 3.0f)
                                ? float3(0.0f, 1.0f, 0.0f)
                                : float3(0.0f, 0.0f, 1.0f)));
                    break;

                case FLAGS_DEBUG_ALBEDO_STRUCTURE:
                {
                    // Unmasked by design: compare structured vending-machine albedo
                    // directly against the flat albedo supplied for Type-1 displays.
                    const float structure = GetAlbedoStructure(
                        int2(px), viewSpacePos.z, worldSurfaceNormal.xyz,
                        rawRoughness, inputSpecReflectance, inputDiffAlbedo);
                    debugColor = structure > 1e-4f
                        ? TurboColormap(structure)
                        : 0.0f;
                    break;
                }

                case FLAGS_DEBUG_ZERO_ROUGH_FLOOR:
                    // What the handover actually publishes. Non-handover pixels stay
                    // black so the affected surfaces are unambiguous, and the colour
                    // shown is the exact value composition will receive.
                    debugColor = floorHandover > 0.0f ? handoverColor : 0.0f;
                    break;

                case FLAGS_DEBUG_ALBEDO_OVERSHOOT:
                    debugColor = albedoOvershoot;
                    break;

                // Optional-input validation views.

                // The specular signal's share of the demodulated split: how much of this pixel's
                // radiance the denoiser receives through the specular signal rather than the
                // diffuse one. Read from the albedo ratio, so it is the title's material split
                // rather than anything this shader decided.
                case FLAGS_DEBUG_SPECULAR_SPLIT:
                    debugColor = TurboColormap(specularShare);
                    break;

                case FLAGS_DEBUG_IN_TITLE_DEPTH:
                    debugColor = IsSet(FLAGS_TITLE_LINEAR_DEPTH)
                        ? TurboColormap(saturate(
                            abs(titleDepthRaw) / max(DebugDepthMax, 1e-3f)))
                        : float3(1.0f, 0.0f, 1.0f);
                    break;

                case FLAGS_DEBUG_TITLE_DEPTH_DIFF:
                {
                    // The title's published depth against the canonical field this chain
                    // actually consumes. Both come from the title's resource now - the
                    // canonical copy is what the floor seed built from it - so a black
                    // field means the canonicalisation changed nothing, and colour shows
                    // the sign and range adjustments it applied.
                    if (!IsSet(FLAGS_TITLE_LINEAR_DEPTH))
                        debugColor = float3(1.0f, 0.0f, 1.0f);
                    else
                    {
                        // Signed, not magnitude. A title's linear depth may be a positive
                        // distance while the canonical field is a signed view Z, and
                        // comparing magnitudes cannot see that. Green means the title
                        // reports the surface further along +Z than the canonical field
                        // carries, red means the opposite.
                        debugColor = VisualizeSignedDiff(titleDepthRaw - InDepth[px], 1.0f);
                    }
                    break;
                }

                // Where the game is asking for the current frame to be trusted over
                // history. Should light up on billboards, particles and alpha layers.
                // Where the guide permits the raw-preserving blend. Blue means the surface
                // reads as flat and the raw sample is refused, red means it is kept.
                // The two halves of the skip signal, separated. The floor share is what the
                // floor path contributes; the unmapped share is what modulation could not
                // represent and therefore reaches the screen without passing the denoiser.
                // Grain that lives in the second and not the first is not the floor's.
                case FLAGS_DEBUG_SKIP_UNMAPPED:
                    debugColor = TurboColormap(saturate(
                        GetLuminance(unmappedShare) * rcp(max(GetLuminance(rawColor), 1e-3f))));
                    break;

                case FLAGS_DEBUG_SKIP_FLOOR:
                    debugColor = TurboColormap(saturate(
                        GetLuminance(skipFloorShare) * rcp(max(GetLuminance(rawColor), 1e-3f))));
                    break;

                // The share of each pixel the ceiling clamp took from the raw sample rather
                // than from the floor. This is the grain the clamp injects, so it is black
                // whenever the clamp is off and lights up on exactly the pixels the skip signal
                // publishes unfiltered instead of filtered.
                case FLAGS_DEBUG_SKIP_RAW_INJECT:
                    debugColor = TurboColormap(saturate(
                        GetLuminance(rawInject) * rcp(max(GetLuminance(rawColor), 1e-3f))));
                    break;

                case FLAGS_DEBUG_FLOOR_STRUCTURE:
                    debugColor = TurboColormap(guideStructure);
                    break;

                case FLAGS_DEBUG_IN_BIAS_MASK:
                    debugColor = TurboColormap(biasWeight);
                    break;

                // Demodulation amplification, log2 scaled over [1, 128]. Red areas are where
                // radiance noise is being multiplied hardest.
                case FLAGS_DEBUG_DEMOD_GAIN:
                    debugColor = TurboColormap(saturate(log2(max(demodGain, 1.0f)) * (1.0f / 7.0f)));
                    break;

                // The specular tracking ramp itself, separated from the buffer it scales.
                // Blue = closed (the specular reprojects with the surface), red = open.
                case FLAGS_DEBUG_HIT_DIST_GATE:
                    debugColor = TurboColormap(specularTracking);
                    break;

                case FLAGS_DEBUG_DENOISER_FRACTION:
                {
                    // Share of the pixel routed to the denoiser rather than around it through
                    // the skip signal. Blue = travelling around the denoiser, blurred by the
                    // floor; red = being denoised. Reflections and shadows reading blue is the
                    // floor capturing lighting it should have passed through.
                    const float rawLum = GetLuminance(rawColor);
                    const float denLum = GetLuminance(denoiserColor);
                    debugColor = TurboColormap(saturate(denLum * rcp(max(rawLum, 1e-3f))));
                    break;
                }

                case FLAGS_DEBUG_IN_RESPONSIVITY:
                    // Magenta when absent. The polarity is printed by the probe rather
                    // than assumed here, because it is a property of the title.
                    debugColor = IsSet(FLAGS_HAS_RESPONSIVITY_MASK)
                        ? TurboColormap(saturate(responsivityRaw))
                        : float3(1.0f, 0.0f, 1.0f);
                    break;
                
                default:
                    debugColor = demodSpecular + demodDiffuse;
                    break;
            }
        
            OutIndirectSpecular[px] = half4(debugColor, 1.0f);
        }
    }
    else // Skip
    {
        // FloorSeed temporarily stores depth gradients in OutMotion. Every packing
        // path must overwrite it before RR consumes the texture as motion vectors.
        OutMotion[px] = 0.0f;
        OutNormals[px] = 0.0f;
        OutSpecAlbedo[px] = 0.0f;
        OutDiffAlbedo[px] = 0.0f;
        OutIndirectSpecular[px] = half4(
            0.0f, 0.0f, 0.0f,
            IsSet(FLAGS_SPECULAR_SIGNAL_INDIRECT) ? s_InvalidSpecularHitDistance : 0.0f);
        OutDirectDiffuse[px] = half4(0.0f, 0.0f, 0.0f, s_MissingDiffuseHitDistance);
        // Nothing was demodulated on this path, so the composite colour passes through whole.
        OutSkipSignal[px] = half4(rawColor, rawLuma);
        // Skipped pixels have no handover image; a zero weight leaves composition's
        // own result untouched.
        OutHandover[px] = 0.0f;
    }
}
