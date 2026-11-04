#include "FSRDPreprocessCommon.hlsli"
#include "FSRDFloorCommon.hlsli"

#define MainRS \
    "RootFlags(0), CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 5)), " \
    "DescriptorTable(UAV(u0, numDescriptors = 5))"

#define FLAGS_LINEAR_DEPTH (1 << 0)
#define FLAGS_NEGATIVE_VIEW_DEPTH (1 << 1)
#define FLAGS_TITLE_LINEAR_DEPTH (1 << 2)
#define THREAD_GROUP_SIZE_X 8
#define THREAD_GROUP_SIZE_Y 8
#define NUM_THREADS 64
static const uint2 s_ThreadGroupSize = uint2(8, 8);
DEFINE_LDS_CONFIG(s_SM, 5);
DECLARE_LDS_ARRAY_2D(half3, g_Color, 5);
DECLARE_LDS_ARRAY_2D(float, g_Depth, 5);
DECLARE_LDS_ARRAY_2D(half3, g_Normal, 5);
DECLARE_LDS_ARRAY_2D(half3, g_Albedo, 5);

Texture2D<float3> InColor : register(t0);
Texture2D<float3> InNormals : register(t1);
Texture2D<float> InDepth : register(t2);
Texture2D<float> InTitleLinearDepth : register(t3);
Texture2D<float3> InDiffAlbedo : register(t4);
RWTexture2D<half4> OutColor : register(u0);
RWTexture2D<float> OutLinearDepth : register(u1);
RWTexture2D<half4> OutDepthGradient : register(u2);
RWTexture2D<half4> OutDetailReference : register(u3);
RWTexture2D<float> OutCanonicalDepth : register(u4);

cbuffer CB_Median : register(b0)
{
    float4x4 InvProjMatrix; // DLSSD ViewToClip^-1
    float4 RenderSize;

    float NearPlane;
    float FarPlane;
    
    uint Flags;
    
    float _Padding;

    float2 CurrentJitter;
    float2 _JitterPadding;

    uint4 InputBase; // XY: color origin, ZW: depth origin

    // Origin of the title's normals, which the orientation guide is read from.
    uint2 NormalBase;
    float2 _NormalPadding;

    // Origin of the title's published linear depth, when it provides one.
    uint2 TitleDepthBase;
    float2 _TitleDepthPadding;
    uint2 AlbedoBase;
    uint _Reserved0;
    uint FloorEnabled;
}

bool IsSet(uint mask) { return (Flags & mask) == mask; }

float3 GetViewSpacePos(const int2 px, bool bounded = true)
{
    // A title that publishes its own linear depth is authoritative: it knows which
    // linearisation it applied, and deriving it from hardware depth is the step that
    // has to guess that convention. This read is where the canonical signed depth is
    // produced - the floor filter, the packing shader and the denoiser's own depth
    // input all consume OutLinearDepth - so steering it steers the geometry of the
    // whole chain, and no consumer is left deriving its own.
    const bool useTitleDepth = IsSet(FLAGS_TITLE_LINEAR_DEPTH);
    float inDepth = useTitleDepth
        ? InTitleLinearDepth[px + int2(TitleDepthBase)]
        : InDepth[px + int2(InputBase.zw)];
    // InvProjMatrix is unjittered. Convert the current jittered raster
    // coordinate back to the matching unjittered projection ray.
    const float2 uv = (float2(px) + 0.5 - CurrentJitter) * RenderSize.zw;
    const float depthSign = IsSet(FLAGS_NEGATIVE_VIEW_DEPTH) ? -1.0f : 1.0f;
    float3 viewSpacePos = 0.0f;
    
    [branch]
    if (IsSet(FLAGS_LINEAR_DEPTH) || useTitleDepth)
    {
        inDepth = bounded ? clamp(abs(inDepth), NearPlane, FarPlane) : abs(inDepth);
        inDepth *= depthSign;
        // Mid-range NDC depth: the ray is rescaled to inDepth below, so the choice
        // is arbitrary except that it must avoid the inverse projection's w == 0
        // singularity, which sits at 1.0 for a standard-Z infinite far plane.
        viewSpacePos = InvProjectPosition(float3(uv, 0.5f), InvProjMatrix);
        const float safeRayZ = (viewSpacePos.z < 0.0f)
            ? min(viewSpacePos.z, -1e-6f)
            : max(viewSpacePos.z, 1e-6f);
        viewSpacePos *= inDepth / safeRayZ;
        viewSpacePos.z = inDepth;
    }
    else
    {
        viewSpacePos = InvProjectPosition(float3(uv, inDepth), InvProjMatrix);
        // Projection handedness, rather than depth direction, defines the RR sign.
        // Clamp the complete position to preserve a coherent view ray at the
        // near/far boundaries instead of replacing Z alone.
        const float signedDepth = depthSign * (bounded ? clamp(abs(viewSpacePos.z), NearPlane, FarPlane) : abs(viewSpacePos.z));
        const float safeViewZ = (viewSpacePos.z < 0.0f)
            ? min(viewSpacePos.z, -1e-6f)
            : max(viewSpacePos.z, 1e-6f);
        viewSpacePos *= signedDepth / safeViewZ;
        viewSpacePos.z = signedDepth;
    }
    
    return viewSpacePos;
}


float2 DepthGradient(int2 p)
{
    return float2(FloorDepthDerivative(g_Depth[p.x-1][p.y], g_Depth[p.x][p.y], g_Depth[p.x+1][p.y]),
                  FloorDepthDerivative(g_Depth[p.x][p.y-1], g_Depth[p.x][p.y], g_Depth[p.x][p.y+1]));
}

[RootSignature(MainRS)]
[numthreads(8, 8, 1)]
void CSMain(uint3 groupID : SV_GroupID, uint3 gtID : SV_GroupThreadID)
{
    const int2 origin = int2(groupID.xy * 8) - 2;
    const uint flatID = gtID.x + gtID.y * 8;
    const int2 bounds = int2(RenderSize.xy) - 1;
    // Depth is required even when Floor is disabled. Avoid every colour/guide load
    // and all sorting in that mode, without putting an early return before a barrier.
    [unroll]
    for (uint i = 0; i < s_SM_LoadsPerThread; ++i)
    {
        const uint flat = flatID + i * 64;
        if (flat < s_SM_ElementCount)
        {
            const int2 sm = int2(flat % s_SM_Size.x, flat / s_SM_Size.x);
            const int2 p = clamp(origin + sm, 0, bounds);
            g_Depth[sm.x][sm.y] = GetViewSpacePos(p).z;
            if (FloorEnabled != 0u)
            {
                const float3 c = FloorRadiance(InColor[p + int2(InputBase.xy)]);
                g_Color[sm.x][sm.y] = c;
                g_Normal[sm.x][sm.y] = FloorNormal(InNormals[p + int2(NormalBase)]);
                g_Albedo[sm.x][sm.y] = FloorRadiance(InDiffAlbedo[p + int2(AlbedoBase)]);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 px = int2(groupID.xy * 8 + gtID.xy);
    if (any(px > bounds)) return;
    const int2 sm = int2(gtID.xy) + 2;
    const float z = g_Depth[sm.x][sm.y];
    OutLinearDepth[px] = z;
    // SR consumes physical depth, independent of the RR diagnostic horizon.
    OutCanonicalDepth[px] = GetViewSpacePos(px, false).z;
    if (FloorEnabled == 0u)
    {
        OutColor[px] = 0;
        OutDepthGradient[px] = 0;
        OutDetailReference[px] = 0;
        return;
    }
    const float2 gradient = DepthGradient(sm);
    const float3 normal = g_Normal[sm.x][sm.y];
    const float3 albedo = g_Albedo[sm.x][sm.y];
    const float3 centerRGB = g_Color[sm.x][sm.y].rgb;
    const float4 center = float4(centerRGB, GetLuminance(centerRGB));
    float keys[25];
    half weights[25];
    uint count = 0;
    float surfaceSupport = 0;
    float3 innerMin = 65500.0f, innerMax = 0;
    float3 ringMin = 65500.0f, ringMax = 0;
    uint innerCount = 0, ringCount = 0, matchingNeighbours = 0;
    [unroll]
    for (uint j = 0; j < 25; ++j)
    {
        const int2 off = int2(j % 5, j / 5) - 2;
        const int2 q = sm + off;
        const float w = FloorSurfaceWeight(z, g_Depth[q.x][q.y], gradient,
            float2(clamp(px + off, 0, bounds) - px), normal, g_Normal[q.x][q.y],
            albedo, g_Albedo[q.x][q.y]);
        // Always retain the centre, including far-plane/degenerate geometry.
        // Replicated border texels are not independent evidence for a noisy centre.
        const bool inside = all(px + off >= 0) && all(px + off <= bounds);
        weights[j] = j == 12 ? 1.0f : (inside ? w : 0.0f);
        const bool accepted = weights[j] > 0.1f;
        keys[j] = accepted ? GetLuminance(float3(g_Color[q.x][q.y].rgb)) : 1e20f;
        count += accepted ? 1 : 0;
        surfaceSupport += accepted ? float(weights[j]) : 0.0f;
        if (accepted && j != 12)
        {
            const float3 rgb = g_Color[q.x][q.y].rgb;
            if (length(rgb-center.rgb) <= max(0.08f*length(center.rgb),1e-5f))
                matchingNeighbours++;
            if (all(abs(off) <= 1))
            {
                innerMin = min(innerMin, rgb); innerMax = max(innerMax, rgb); innerCount++;
            }
            else
            {
                ringMin = min(ringMin, rgb); ringMax = max(ringMax, rgb); ringCount++;
            }
        }
    }
    [unroll]
    for (uint k = 0; k < kSortNetworkSize; ++k)
    {
        const uint a = SortNetwork[2*k], b = SortNetwork[2*k+1];
        const float ka = keys[a];
        keys[a] = min(ka, keys[b]);
        keys[b] = max(ka, keys[b]);
    }
    // Static indexing avoids spilling a dynamically indexed private array.
    float median = 0, q25 = 0, q75 = 0;
    [unroll]
    for (uint rank = 0; rank < 25; ++rank)
    {
        median = rank == count/2 ? keys[rank] : median;
        q25 = rank == (count-1)/4 ? keys[rank] : q25;
        q75 = rank == (3*(count-1))/4 ? keys[rank] : q75;
    }
    const float sigma = max((q75 - q25) * 0.7413f, 0.0f);
    const float range = max(3.0f * sigma, max(median * 0.025f, 1e-5f));

    // A one-pixel stroke has two supporting neighbours in one direction; an
    // isolated impulse does not. Test RGB as well, so chromatic fireflies cannot
    // borrow confidence from an unrelated luminance match.
    const int2 axes[4] = { int2(1,0), int2(0,1), int2(1,1), int2(1,-1) };
    float support = 0;
    float directionalNoise = sigma;
    [unroll]
    for (uint a = 0; a < 4; ++a)
    {
        const int2 lo = sm - axes[a], hi = sm + axes[a];
        const uint li = (2-axes[a].y)*5 + 2-axes[a].x;
        const uint ri = (2+axes[a].y)*5 + 2+axes[a].x;
        const float tolerance = max(0.08f * max(center.a, median), 1e-5f);
        const float3 curvature = center.rgb -
            0.5f * (float3(g_Color[lo.x][lo.y].rgb) + float3(g_Color[hi.x][hi.y].rgb));
        const float pairWeight = min(weights[li], weights[ri]);
        const float agreement = Square(saturate(1.0f -
            dot(curvature, curvature) / (tolerance * tolerance)));
        support = max(support, agreement * pairWeight);
        // IQR measures contrast as well as noise. A valid axis along a clean edge
        // or ramp has no curvature: do not classify its contrast as uncertainty.
        const float axisNoise = 0.9428f * length(curvature);
        directionalNoise = min(directionalNoise, lerp(sigma, axisNoise, pairWeight));
    }
    float3 base = 0, reference = 0;
    float baseWeight = 0, refWeight = 0;
    const float target = lerp(median, center.a, support);
    // Few samples cannot establish a stable median colour population. Retain
    // only the lower quartile there; well-supported grain can pool a lower half.
    const float baseCeiling = count >= 9 ? median : q25;
    [unroll]
    for (uint t = 0; t < 25; ++t)
    {
        const int2 q = sm + int2(t % 5, t / 5) - 2;
        const float3 rgb = g_Color[q.x][q.y].rgb;
        const float4 c = float4(rgb, GetLuminance(rgb));
        const float w = weights[t] > 0.1f ? weights[t] : 0.0f;
        // Wide IQR weights admitted colourful bright rays into the pedestal,
        // then a luminance-only cap kept their chroma. Exclude the positive tail,
        // but keep the lower half rather than too few samples of ordinary grain.
        const float wb = c.a <= baseCeiling + max(0.01f*baseCeiling,1e-6f)
            ? w * FloorRangeWeight(c.a - q25, range) : 0.0f;
        const float wr = w * FloorRangeWeight(c.a - target,
            lerp(range, max(0.08f * max(target, median), 1e-5f), support));
        base += c.rgb * wb; baseWeight += wb;
        reference += c.rgb * wr; refWeight += wr;
    }
    // q25 is an accepted sample, so the base always has nonzero support. Reuse
    // that robust RGB estimate if an interpolated reference target has no support.
    base /= max(baseWeight, 1e-6f);
    reference = refWeight > 1e-6f ? reference / refWeight : base;
    // A volume/transparent layer does not follow the background's surface guide.
    // Missing surface support cannot mean missing light. Use the common lower RGB
    // level of independent quadrant means as a conservative non-surface pedestal.
    // A bright impulse affects at most one block and cannot raise their common floor.
    float3 commonBase = 65500.0f;
    float quietNoise = sigma;
    uint blocks = 0;
    [branch]
    if (surfaceSupport < 9.0f)
    {
    [unroll]
    for (uint quadrant = 0; quadrant < 4; ++quadrant)
    {
        const int2 direction = int2((quadrant & 1) ? 1 : -1, (quadrant & 2) ? 1 : -1);
        float3 blockMinimum = 65500.0f;
        float3 blockSecond = 65500.0f;
        float blockCount = 0;
        [unroll]
        for (int by = 1; by <= 2; ++by)
        {
            [unroll]
            for (int bx = 1; bx <= 2; ++bx)
            {
                const int2 off = direction * int2(bx, by);
                if (all(px + off >= 0) && all(px + off <= bounds))
                {
                    const int2 q = sm + off;
                    const float3 rgb = g_Color[q.x][q.y].rgb;
                    blockSecond = min(blockSecond, max(blockMinimum, rgb));
                    blockMinimum = min(blockMinimum, rgb);
                    blockCount += 1;
                }
            }
        }
        if (blockCount > 0)
        {
            // Keep the lower half, not all but one: two independent bright
            // rays in each surviving border quadrant must not raise Skip.
            const float3 blockBase = blockCount >= 3
                ? 0.5f*(blockMinimum+blockSecond) : blockMinimum;
            commonBase = min(commonBase, blockBase);
            blocks++;
        }
    }
    }
    // Independent edge pairs estimate local noise without labelling the central
    // glyph's curvature as noise. A flat supported patch supplies a zero estimate.
    [unroll]
    for (uint corner = 0; corner < 4; ++corner)
    {
        const int2 off = int2((corner & 1) ? 2 : -2, (corner & 2) ? 2 : -2);
        const int2 nearOff = int2(off.x / 2, off.y);
        if (min(weights[(off.y+2)*5+off.x+2], weights[(nearOff.y+2)*5+nearOff.x+2]) > 0.1f)
        {
            const int2 a = sm + off, b = sm + nearOff;
            const float3 delta = float3(g_Color[a.x][a.y]) - float3(g_Color[b.x][b.y]);
            quietNoise = min(quietNoise, sqrt(dot(delta,delta) / 6.0f));
        }
    }
    // The pedestal cannot brighten a foreground silhouette. It is not a detail
    // reference and never authorizes handover across a surface boundary.
    commonBase = blocks >= 2 ? min(commonBase, center.rgb) : 0;
    const float surfaceConfidence = saturate((surfaceSupport - 1.0f) * 0.25f);
    const float baseLuma = GetLuminance(base);
    base *= baseLuma > 0 ? min(1.0f, q25 / baseLuma) : 0;
    // A handful of coincidentally matching guides can still select two bright
    // ray samples. Partial support cannot lift above the independent common level.
    if (surfaceSupport < 9.0f)
    {
        const float3 conservative = lerp(commonBase, min(base,commonBase), surfaceConfidence);
        // Newly revealed surfaces cross five effective samples continuously.
        // The former hard switch could jump from the common pedestal to a much
        // brighter surface estimate in a single camera subpixel step.
        base = lerp(conservative, base, smoothstep(5.0f,9.0f,surfaceSupport));
    }
    // Reference rank semantics: a bounded, supported centre keeps its exact RGB.
    // Averaging every accepted sample erased antialiased curves even when no sample
    // was an outlier. Unlike the old luma/chroma rescale, rejected pixels keep the
    // robust RGB reconstruction. Requiring outer-ring support also rejects a 2x2
    // impulse cluster which the old full-window range could accept circularly.
    const float3 innerMargin = 0.5f * max(innerMax-innerMin, 1e-5f);
    const float3 ringMargin = 0.5f * max(ringMax-ringMin, 1e-5f);
    const bool rankAccepted = innerCount >= 2 && ringCount >= 2 &&
        all(center.rgb >= innerMin-innerMargin) && all(center.rgb <= innerMax+innerMargin) &&
        all(center.rgb >= ringMin-ringMargin) && all(center.rgb <= ringMax+ringMargin);
    // Pool nine mixed derivatives: the median rejects isolated text corners.
    // Each 3x3 stencil cancels colour ramps and axis-aligned strokes. Require the
    // whole independent same-surface patch before using its noise statistic.
    float noiseKeys[9];
    float noiseSupport = 1;
    [unroll]
    for (int ny = -1; ny <= 1; ++ny)
    {
        [unroll]
        for (int nx = -1; nx <= 1; ++nx)
        {
            float3 mixedDifference = 0;
            [unroll]
            for (int sy = -1; sy <= 1; ++sy)
            {
                [unroll]
                for (int sx = -1; sx <= 1; ++sx)
                {
                    const float coefficient = (sx == 0 ? -2.0f : 1.0f)*(sy == 0 ? -2.0f : 1.0f);
                    mixedDifference += coefficient*float3(g_Color[sm.x+nx+sx][sm.y+ny+sy].rgb);
                    noiseSupport = min(noiseSupport, weights[(ny+sy+2)*5+nx+sx+2]);
                }
            }
            noiseKeys[(ny+1)*3+nx+1] = dot(mixedDifference,mixedDifference);
        }
    }
    [unroll]
    for (int phase = 0; phase < 9; ++phase)
    {
        [unroll]
        for (int pair = 0; pair < 4; ++pair)
        {
            const int a = 2*pair + (phase & 1), b = a+1;
            const float lo = min(noiseKeys[a],noiseKeys[b]);
            noiseKeys[b] = max(noiseKeys[a],noiseKeys[b]);
            noiseKeys[a] = lo;
        }
    }
    const float measuredNoise = sqrt(noiseKeys[4]/108.0f)*1.4826f;
    const float sigmaReference = lerp(min(quietNoise, directionalNoise), measuredNoise, noiseSupport);
    // A rank-rejected centre is replaced only when it sits far outside the envelope its own
    // supported neighbours span - that is what an impulse looks like. A centre that merely
    // sits at a local extremum of smooth content overshoots that envelope by a fraction of
    // it, and keeping the robust estimate there would print a smoothed copy of the current
    // frame: the cleaned reference has to be a fixed point on supported clean input, not
    // just on straight edges and flat fields.
    // The outer ring is the envelope to judge against, not the 3x3: an impulse cluster
    // inflates the inner one with its own samples, so a 2x2 cluster would look bounded.
    const float3 envelope = ringMax - ringMin;
    const float3 overshoot = max(ringMin - center.rgb, center.rgb - ringMax);
    const float3 outlier = saturate(overshoot / max(envelope, 1e-5f) - 1.0f);
    const float3 robust = reference; // Impulse rejection only; no reference-to-base smoothing.
    reference = rankAccepted ? center.rgb : lerp(center.rgb, robust, outlier);
    // Extrema in two noisy rings are not independent proof of structure. A
    // positive sparse ray must have local colour or directional continuation.
    // Straight 1px strokes and supported corners retain their exact reference.
    const bool sparsePositiveOutlier = matchingNeighbours < 2 && support < 0.1f &&
        center.a > median + max(3.0f*sigma, max(0.08f*median,1e-5f));
    if (sparsePositiveOutlier)
        reference = robust;
    OutColor[px] = half4(FloorRadiance(base), min(sigma, 65500.0f));
    OutDetailReference[px] = half4(FloorRadiance(reference),
        surfaceSupport >= 2.5f ? min(sigmaReference, 65500.0f) : -1.0f);
    OutDepthGradient[px] = half4(GetSafeSignedFP16(gradient), OctahedralEncode(normal));
}
