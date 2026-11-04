#ifndef FSRD_ADDITIVE_SPLIT
#define FSRD_ADDITIVE_SPLIT

// Adapted from Zakrisson-C's 7x7 channelwise ridge split in
// b151554de7cafd17e886a5d61e9983dc78e7cffc (FSRDSplitFit.hlsl):
// https://github.com/Zakrisson-C/OptiScaler/commit/b151554de7cafd17e886a5d61e9983dc78e7cffc
// This is an estimated signal allocation, not a physical lobe/medium separation.
// There is no share history. Guides, storage and modulation remain unchanged.
// Diagnostic builds reuse this implementation and never run in the normal PSO.
#ifndef FSRD_ADDITIVE_DIAGNOSTICS
#define FSRD_ADDITIVE_DIAGNOSTICS 0
#endif
#if FSRD_ADDITIVE_DIAGNOSTICS
static float3 additiveJournal[48];
static uint3 additiveRejected = 0;
static uint3 additiveEvaluated = 0;
void AdditiveCheck(uint bit, bool3 pass)
{
    additiveEvaluated |= bit;
    additiveRejected |= select(pass, uint3(0,0,0), uint3(bit,bit,bit));
}
#define ADD_RECORD(slot, value) additiveJournal[slot] = (value)
#define ADD_CHECK(bit, pass) AdditiveCheck(bit, pass)
#else
#define ADD_RECORD(slot, value)
#define ADD_CHECK(bit, pass)
#endif
static const int s_AdditiveFitRadius = 3;
static const float s_AdditiveFitMinSamples = 12.0f;

// An 8x8 output group shares its 14x14 input halo. Keep float32 cached values
// and the two eligibility sets separate; the fitting loops retain their original
// y/x reduction order. No full-resolution resource or history is added.
DEFINE_LDS_CONFIG(s_AdditiveSM, 7);
DECLARE_LDS_ARRAY_2D(float3, g_AdditiveSpec, 7);
DECLARE_LDS_ARRAY_2D(float3, g_AdditiveDiff, 7);
DECLARE_LDS_ARRAY_2D(float3, g_AdditiveColor, 7);
DECLARE_LDS_ARRAY_2D(float3, g_AdditiveNormal, 7);
DECLARE_LDS_ARRAY_2D(float2, g_AdditiveDepthRoughness, 7);
DECLARE_LDS_ARRAY_2D(uint, g_AdditiveValid, 7); // bit 0: preflight, bit 1: signal
groupshared uint g_AdditiveReference;
groupshared uint g_AdditiveNeedsSignal;

// Coordinates stay in the render subrect until each resource's origin is added.
// Out-of-bounds taps are omitted by the caller, not counted repeatedly at edges.
bool LoadAdditiveFitTap(int2 p, bool loadSignal,
    out float3 spec, out float3 diff, out float3 color,
    out float depth, out float3 normal, out float roughness)
{
    spec = 0.0f;
    diff = 0.0f;
    color = 0.0f;
    depth = 0.0f;
    normal = 0.0f;
    roughness = 0.0f;

    const float3 sourceSpec = InSpecAlbedo[p + int2(InputBase3.xy)].rgb;
    const float3 sourceDiff = InDiffAlbedo[p + int2(InputBase2.zw)].rgb;
    const float3 sourceColor = InColor[p + int2(InputBase0.xy)].rgb;
    const float4 sourceNormal = InNormals[p + int2(InputBase1.xy)];
    const float sourceRoughness = IsSet(FLAGS_PACKED_ROUGHNESS)
        ? sourceNormal.a : InRoughness[p + int2(InputBase1.zw)];

    // Sanitized invalid values and compatibility/emissive rewrites are not fit
    // evidence. An absent component can be valid, but cannot carry the intercept.
    if (!all(isfinite(sourceSpec)) || !all(isfinite(sourceDiff)) ||
        !all(sourceSpec >= 0.0f) || !all(sourceDiff >= 0.0f) ||
        !all(sourceSpec <= 1.001f) || !all(sourceDiff <= 1.001f) ||
        !all(sourceSpec + sourceDiff <= 1.001f) ||
        !all(isfinite(sourceColor)) || !all(sourceColor >= 0.0f) ||
        !all(sourceColor <= 65504.0f) || !all(isfinite(sourceNormal.xyz)) ||
        !isfinite(sourceRoughness) || !isfinite(InDepth[p]))
        return false;

    normal = sourceNormal.xyz;
    if (IsSet(FLAGS_NORMALS_VIEW_SPACE))
        normal = mul((float3x3)InvViewMatrix, normal);
    const float normalLengthSq = dot(normal, normal);
    if (!all(isfinite(normal)) || !isfinite(normalLengthSq) || normalLengthSq <= 1e-8f)
        return false;
    normal *= rsqrt(normalLengthSq);
    roughness = saturate(sourceRoughness);
    depth = GetViewSpacePos(p).z;
    const float finiteFarPlane = min(FarPlane, 65504.0f);
    if (!isfinite(depth) || abs(depth) < NearPlane ||
        log(abs(depth) + 1.0f) / log(finiteFarPlane + 1.0f) >= 0.99f)
        return false;

    if (IsSet(FLAGS_HAS_BIAS_MASK))
    {
        const float mask = InBiasMask[p + int2(InputBase3.zw)];
        if (!isfinite(mask) || saturate(saturate(mask) * BiasMaskStrength) > 0.0f)
            return false;
    }
    if (IsSet(FLAGS_HAS_RESPONSIVITY_MASK) && ResponsivityTrustThreshold > 0.0f)
    {
        const float responsivity = InResponsivityMask[p].r;
        const bool bypassed = ResponsivityInvert != 0u
            ? responsivity > ResponsivityTrustThreshold
            : responsivity < ResponsivityTrustThreshold;
        if (!isfinite(responsivity) || bypassed)
            return false;
    }
    if (IsSet(FLAGS_HAS_EMISSIVE_INPUT))
    {
        const float3 emission = InEmissive[p + int2(InputBase4.xy)].rgb;
        if (!all(isfinite(emission)) || any(emission != 0.0f))
            return false;
    }

    // Match the stored weights used by conversion, including the small UNORM
    // overshoot that the eligibility tolerance above permits.
    spec = sourceSpec;
    diff = sourceDiff;
    spec = saturate(spec - max(spec + diff - 1.0f, 0.0f));
    diff -= max(spec + diff - 1.0f, 0.0f);
    spec = QuantizeStoredAlbedo(spec);
    diff = QuantizeStoredAlbedo(diff);

    if (IsSet(FLAGS_FLOOR_ENABLED) && roughness <= s_Type1RoughnessThreshold)
        return false;

    if (loadSignal)
    {
        color = FloorRadiance(sourceColor);
        if (IsSet(FLAGS_FLOOR_ENABLED))
        {
            // Outside the zero-roughness and structure handover paths, the
            // actual conversion residual is exactly max(raw - spatialFloor, 0).
            // Fit that residual, not raw radiance with a different signal's share.
            if ((RecoveryMask & 1u) != 0 &&
                HasUnrepresentedSurfaceStructure(p, depth, roughness, false))
                return false;
            const float4 floor = InFloorColor[p];
            if (!all(isfinite(floor)))
                return false;
            color = max(color - FloorRadiance(floor.rgb), 0.0f);
        }
    }
    return true;
}

void PopulateAdditiveFitPreflightMemory(uint2 groupID, uint2 groupThreadID)
{
    const uint flatThread = groupThreadID.x + groupThreadID.y * s_ThreadGroupSize.x;
    const int2 origin = int2(groupID * s_ThreadGroupSize) - int2(s_AdditiveSM_HaloOffset);
    const int2 bounds = int2(DstTexSize.xy) - 1;
    const bool active = FSRD_ADDITIVE_DIAGNOSTICS || (isfinite(AdditiveLightSplit) && AdditiveLightSplit > 0.0f);

    [unroll]
    for (uint i = 0; i < s_AdditiveSM_LoadsPerThread; ++i)
    {
        const uint flat = flatThread + i * NUM_THREADS;
        if (flat >= s_AdditiveSM_ElementCount) continue;
        const uint2 sm = uint2(flat % s_AdditiveSM_Size.x, flat / s_AdditiveSM_Size.x);
        const int2 p = origin + int2(sm);
        float3 spec = 0.0f, diff = 0.0f, color = 0.0f, normal = 0.0f;
        float depth = 0.0f, roughness = 0.0f;
        uint valid = 0u;
        if (active && all(p >= 0) && all(p <= bounds))
        {
            if (LoadAdditiveFitTap(p, false, spec, diff, color, depth, normal, roughness))
                valid = 1u;
        }
        // Every halo location is written, including invalid and partial-group
        // taps. Signal eligibility is evaluated in a later uniform group phase.
        g_AdditiveSpec[sm.x][sm.y] = spec;
        g_AdditiveDiff[sm.x][sm.y] = diff;
        g_AdditiveColor[sm.x][sm.y] = color;
        g_AdditiveNormal[sm.x][sm.y] = normal;
        g_AdditiveDepthRoughness[sm.x][sm.y] = float2(depth, roughness);
        g_AdditiveValid[sm.x][sm.y] = valid;
    }
}

void PopulateAdditiveFitSignalMemory(uint2 groupID, uint2 groupThreadID)
{
    const uint flatThread = groupThreadID.x + groupThreadID.y * s_ThreadGroupSize.x;
    const int2 origin = int2(groupID * s_ThreadGroupSize) - int2(s_AdditiveSM_HaloOffset);
    [unroll]
    for (uint i = 0; i < s_AdditiveSM_LoadsPerThread; ++i)
    {
        const uint flat = flatThread + i * NUM_THREADS;
        if (flat >= s_AdditiveSM_ElementCount) continue;
        const uint2 sm = uint2(flat % s_AdditiveSM_Size.x, flat / s_AdditiveSM_Size.x);
        if ((g_AdditiveValid[sm.x][sm.y] & 1u) == 0u) continue;
        float3 spec, diff, color, normal;
        float depth, roughness;
        const bool signalValid = LoadAdditiveFitTap(origin + int2(sm), true,
            spec, diff, color, depth, normal, roughness);
        // Full-load rejection happens only after the same preflight guide and
        // geometry values have been produced; preserve the preflight validity.
        g_AdditiveSpec[sm.x][sm.y] = spec;
        g_AdditiveDiff[sm.x][sm.y] = diff;
        g_AdditiveColor[sm.x][sm.y] = color;
        g_AdditiveNormal[sm.x][sm.y] = normal;
        g_AdditiveDepthRoughness[sm.x][sm.y] = float2(depth, roughness);
        g_AdditiveValid[sm.x][sm.y] = signalValid ? 3u : 1u;
    }
}

void PrepareAdditiveFitSharedMemory(uint2 groupID, uint2 groupThreadID)
{
    const uint flatThread = groupThreadID.x + groupThreadID.y * s_ThreadGroupSize.x;
    PopulateAdditiveFitPreflightMemory(groupID, groupThreadID);
    GroupMemoryBarrierWithGroupSync();

    if (flatThread == 0u)
    {
        g_AdditiveReference = s_AdditiveSM_ElementCount;
        g_AdditiveNeedsSignal = 0u;
        [loop]
        for (uint flat = 0u; flat < s_AdditiveSM_ElementCount; ++flat)
        {
            const uint2 sm = uint2(flat % s_AdditiveSM_Size.x, flat / s_AdditiveSM_Size.x);
            if ((g_AdditiveValid[sm.x][sm.y] & 1u) != 0u)
            {
                g_AdditiveReference = flat;
                break;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    bool differs = false;
    if (g_AdditiveReference < s_AdditiveSM_ElementCount)
    {
        const uint2 reference = uint2(g_AdditiveReference % s_AdditiveSM_Size.x,
                                      g_AdditiveReference / s_AdditiveSM_Size.x);
        const float3 referenceAlbedo = g_AdditiveSpec[reference.x][reference.y] +
                                      g_AdditiveDiff[reference.x][reference.y];
        [unroll]
        for (uint i = 0; i < s_AdditiveSM_LoadsPerThread; ++i)
        {
            const uint flat = flatThread + i * NUM_THREADS;
            if (flat >= s_AdditiveSM_ElementCount) continue;
            const uint2 sm = uint2(flat % s_AdditiveSM_Size.x, flat / s_AdditiveSM_Size.x);
            if ((g_AdditiveValid[sm.x][sm.y] & 1u) == 0u) continue;
            const float3 albedo = g_AdditiveSpec[sm.x][sm.y] + g_AdditiveDiff[sm.x][sm.y];
            differs = differs || any(albedo != referenceAlbedo);
        }
    }
    if (differs)
        InterlockedOr(g_AdditiveNeedsSignal, 1u);
    GroupMemoryBarrierWithGroupSync();

    // Exact equality only: every possible <=49-tap subset then has constant A.
    // Float32 moment-rounding error is far below the existing 1%*mean(A)^2+
    // 1e-6 evidence threshold, so no fit can be accepted. No contrast threshold
    // or sample set is changed. Avoid signal/HasStructure work for those groups.
    if (g_AdditiveNeedsSignal != 0u || FSRD_ADDITIVE_DIAGNOSTICS)
        PopulateAdditiveFitSignalMemory(groupID, groupThreadID);
    GroupMemoryBarrierWithGroupSync();
}

bool AdditiveFitSameSurface(float z, float3 n, float r,
    float tapZ, float3 tapN, float tapR)
{
    return z * tapZ > 0.0f &&
        abs(tapZ - z) <= max(0.01f, 0.02f * abs(z)) &&
        dot(n, tapN) >= 0.9f &&
        abs(tapR - r) <= max(0.02f, 0.1f * r);
}

float3 GetAdditiveSplitShare(int2 centerPx, float3 centerSpec, float3 centerDiff,
    float centerDepth, float3 centerNormal, float centerRoughness,
    float3 remodSpec, float3 baselineShare, out bool3 fittedChannels)
{
    fittedChannels = false;
    ADD_CHECK(1u, true);
    const int2 smCenter = centerPx % int2(s_ThreadGroupSize) + int2(s_AdditiveSM_HaloOffset);
    ADD_CHECK(2u, (g_AdditiveValid[smCenter.x][smCenter.y] & 2u) != 0u);
    if ((g_AdditiveValid[smCenter.x][smCenter.y] & 2u) == 0u)
        return baselineShare;

    // Preflight guide contrast before the second moment reduction. The cached
    // preflight sample set includes the same taps as the original source scan.
    float count = 0.0f;
    float3 sumA = 0.0f, sumAA = 0.0f, sumS = 0.0f;
    float3 minS = 1.0f, maxS = 0.0f;
    const int2 bounds = int2(DstTexSize.xy) - 1;
    [loop]
    for (int y = -s_AdditiveFitRadius; y <= s_AdditiveFitRadius; ++y)
    {
        [loop]
        for (int x = -s_AdditiveFitRadius; x <= s_AdditiveFitRadius; ++x)
        {
            const int2 p = centerPx + int2(x, y);
            if (any(p < 0) || any(p > bounds)) continue;
            const int2 sm = smCenter + int2(x, y);
            if ((g_AdditiveValid[sm.x][sm.y] & 1u) == 0u) continue;
            const float3 spec = g_AdditiveSpec[sm.x][sm.y];
            const float3 diff = g_AdditiveDiff[sm.x][sm.y];
            const float3 normal = g_AdditiveNormal[sm.x][sm.y];
            const float2 depthRoughness = g_AdditiveDepthRoughness[sm.x][sm.y];
            if (!AdditiveFitSameSurface(centerDepth, centerNormal, centerRoughness,
                    depthRoughness.x, normal, depthRoughness.y))
                continue;
            const float3 albedo = spec + diff;
            count += 1.0f;
            sumA += albedo;
            sumAA += albedo * albedo;
            sumS += spec;
            minS = min(minS, spec);
            maxS = max(maxS, spec);
        }
    }
    ADD_RECORD(3, count);
    ADD_CHECK(4u, count >= s_AdditiveFitMinSamples);
    if (count < s_AdditiveFitMinSamples) return baselineShare;
    float3 meanA = sumA / count;
    float3 varA = max(sumAA / count - meanA * meanA, 0.0f);
    const float3 meanS = sumS / count;
    const float safeFloor = max(DemodDivisorFloor, 1e-4f);
    // A small/floored specular guide would move additive noise into Skip. A
    // textured specular multiplier would merely transfer the albedo imprint.
    const bool3 stableSpec = and(minS >= 4.0f / 255.0f,
        and(lerp(1.0f, minS, saturate(SpecularAlbedoDemodulation)) >= safeFloor,
            maxS - minS <= 0.10f * meanS + 1e-6f));
    ADD_RECORD(5, meanA); ADD_RECORD(6, varA);
    ADD_RECORD(7, minS); ADD_RECORD(8, maxS); ADD_RECORD(9, meanS);
    ADD_CHECK(8u, minS >= 4.0f / 255.0f);
    ADD_CHECK(16u, lerp(1.0f, minS, saturate(SpecularAlbedoDemodulation)) >= safeFloor);
    ADD_CHECK(32u, maxS - minS <= 0.10f * meanS + 1e-6f);
    ADD_CHECK(64u, varA >= 0.01f * meanA * meanA + 1e-6f);
    if (!FSRD_ADDITIVE_DIAGNOSTICS && !any(and(stableSpec, varA >= 0.01f * meanA * meanA + 1e-6f)))
        return baselineShare;

    count = 0.0f;
    sumA = 0.0f;
    sumAA = 0.0f;
    float3 sumC = 0.0f, sumCC = 0.0f, sumAC = 0.0f;
    [loop]
    for (int y = -s_AdditiveFitRadius; y <= s_AdditiveFitRadius; ++y)
    {
        [loop]
        for (int x = -s_AdditiveFitRadius; x <= s_AdditiveFitRadius; ++x)
        {
            const int2 p = centerPx + int2(x, y);
            if (any(p < 0) || any(p > bounds)) continue;
            const int2 sm = smCenter + int2(x, y);
            if ((g_AdditiveValid[sm.x][sm.y] & 2u) == 0u) continue;
            const float3 spec = g_AdditiveSpec[sm.x][sm.y];
            const float3 diff = g_AdditiveDiff[sm.x][sm.y];
            const float3 color = g_AdditiveColor[sm.x][sm.y];
            const float3 normal = g_AdditiveNormal[sm.x][sm.y];
            const float2 depthRoughness = g_AdditiveDepthRoughness[sm.x][sm.y];
            if (!AdditiveFitSameSurface(centerDepth, centerNormal, centerRoughness,
                    depthRoughness.x, normal, depthRoughness.y))
                continue;
            const float3 albedo = spec + diff;
            count += 1.0f;
            sumA += albedo;
            sumAA += albedo * albedo;
            sumC += color;
            sumCC += color * color;
            sumAC += albedo * color;
        }
    }
    ADD_RECORD(4, count);
    ADD_CHECK(128u, count >= s_AdditiveFitMinSamples);
    if (count < s_AdditiveFitMinSamples) return baselineShare;

    meanA = sumA / count;
    const float3 meanC = sumC / count;
    varA = max(sumAA / count - meanA * meanA, 0.0f);
    const float3 covAC = sumAC / count - meanA * meanC;
    const float3 varC = max(sumCC / count - meanC * meanC, 0.0f);
    const float3 safeMeanA = max(meanA, 1e-3f);
    const float3 ratioSlope = meanC / safeMeanA;
    const float3 lambda = 0.02f * meanA * meanA + 1e-6f;
    const float3 fitSlope = (covAC + lambda * ratioSlope) / (varA + lambda);
    const float3 intercept = clamp(meanC - fitSlope * meanA, 0.0f, meanC);

    // Reject intercepts indistinguishable from sampling uncertainty. This is
    // approximate local regression evidence, not a denoiser confidence/history.
    const float3 residualVariance = max(
        varC + fitSlope * fitSlope * varA - 2.0f * fitSlope * covAC, 0.0f);
    const float3 interceptError = sqrt(residualVariance *
        (1.0f + meanA * meanA / max(varA, 1e-6f)) / count);
    const float3 b = intercept * saturate(AdditiveLightSplit);
    const float3 a = (meanC - b) / safeMeanA;
    const float3 model = (centerSpec + centerDiff) * a + b;
    const float3 share = saturate((centerSpec * a + b) / max(model, 1e-6f));
    const float3 rawCenter = FloorRadiance(GetRawColorAt(centerPx));
    fittedChannels = and(stableSpec, varA >= 0.01f * meanA * meanA + 1e-6f);
    fittedChannels = and(fittedChannels,
        intercept > max(0.01f * meanC, 2.0f * interceptError));
    fittedChannels = and(fittedChannels, and(fitSlope >= 0.0f, model > 1e-6f));
    fittedChannels = and(fittedChannels, and(isfinite(share), isfinite(intercept)));
    fittedChannels = and(fittedChannels, rawCenter * share <= 65504.0f * remodSpec);
    ADD_RECORD(5, meanA); ADD_RECORD(6, varA);
    ADD_RECORD(10, meanC); ADD_RECORD(11, varC); ADD_RECORD(12, covAC);
    ADD_RECORD(13, lambda); ADD_RECORD(14, varA / (varA + lambda));
    ADD_RECORD(15, ratioSlope); ADD_RECORD(16, covAC / max(varA, 1e-20f));
    ADD_RECORD(17, fitSlope); ADD_RECORD(18, meanC - fitSlope * meanA);
    ADD_RECORD(19, intercept); ADD_RECORD(20, interceptError);
    ADD_RECORD(21, sqrt(residualVariance)); ADD_RECORD(22, a); ADD_RECORD(23, b);
    ADD_RECORD(24, model); ADD_RECORD(25, g_AdditiveColor[smCenter.x][smCenter.y] - model);
    ADD_CHECK(256u, varA >= 0.01f * meanA * meanA + 1e-6f);
    ADD_CHECK(512u, intercept > max(0.01f * meanC, 2.0f * interceptError));
    ADD_CHECK(1024u, fitSlope >= 0.0f);
    ADD_CHECK(2048u, model > 1e-6f);
    ADD_CHECK(4096u, and(isfinite(share), isfinite(intercept)));
    ADD_CHECK(8192u, rawCenter * share <= 65504.0f * remodSpec);
    ADD_RECORD(2, select(fittedChannels, 1.0f, 0.0f));
    return select(fittedChannels, max(share, baselineShare), baselineShare);
}

#endif
