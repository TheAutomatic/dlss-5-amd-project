#include "FSRDPreprocessCommon.hlsli"

#define MainRS \
    "RootFlags(0), " \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 4), visibility = SHADER_VISIBILITY_ALL), " \
    "DescriptorTable(UAV(u0, numDescriptors = 3), visibility = SHADER_VISIBILITY_ALL), " \
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

// Flags
#define FLAGS_LINEAR_DEPTH      (1 << 0)
#define FLAGS_NEGATIVE_VIEW_DEPTH (1 << 1)
// The title publishes its own linearised view depth: InTitleLinearDepth replaces InDepth
// as the source the canonical signed output is derived from, and the linear path is
// taken regardless of FLAGS_LINEAR_DEPTH (which only describes the game's own depth).
#define FLAGS_TITLE_LINEAR_DEPTH (1 << 2)

// 5x5 sorting filter config
#define SORT_KERNEL_SIZE        5
#define SORT_KERNEL_RANGE_MIN   (-SORT_KERNEL_SIZE / 2)
#define SORT_KERNEL_RANGE_MAX   (SORT_KERNEL_SIZE / 2)

DEFINE_LDS_CONFIG(s_SM_Med, SORT_KERNEL_SIZE);
DECLARE_LDS_ARRAY_2D(half4, g_Color, SORT_KERNEL_SIZE);

// 3x3 Depth gradient
#define DEPTH_KERNEL_SIZE       3
#define DEPTH_KERNEL_RANGE_MIN  (-DEPTH_KERNEL_SIZE / 2)
#define DEPTH_KERNEL_RANGE_MAX  (DEPTH_KERNEL_SIZE / 2)

DEFINE_LDS_CONFIG(s_SM_Depth, DEPTH_KERNEL_SIZE);
DECLARE_LDS_ARRAY_2D(float, g_Depth, DEPTH_KERNEL_SIZE);

// Stats config
static const int s_SetSize = 25;
static const int s_SpreadWindowSize = 1;

static const float s_BinIndexToPct = (1.0f / float(s_SetSize - 1));
static const int s_MinBin = s_SpreadWindowSize + 1;
static const int s_MaxBin = s_SetSize - s_SpreadWindowSize - 1;

// Full sorting network for 25 values
static const uint kSortNetworkSize = 131;
static const uint SortNetwork[2 * kSortNetworkSize] =
{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
    0, 2, 1, 3, 4, 6, 5, 7, 8, 10, 9, 11, 12, 14, 13, 15, 16, 18, 17, 19, 20, 22, 21, 24,
    0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15, 16, 20, 21, 22, 23, 24,
    0, 8, 1, 12, 2, 10, 3, 14, 4, 9, 5, 13, 6, 11, 7, 15, 17, 22, 18, 21, 19, 24,
    1, 18, 3, 9, 5, 17, 6, 20, 7, 13, 11, 14, 12, 22, 15, 24, 21, 23,
    1, 16, 3, 12, 5, 21, 6, 18, 7, 11, 10, 17, 14, 23, 19, 20,
    0, 1, 2, 5, 4, 16, 6, 8, 7, 18, 9, 21, 10, 14, 11, 13, 12, 19, 15, 23, 20, 22,
    1, 2, 3, 5, 4, 6, 7, 9, 8, 12, 10, 16, 11, 20, 13, 22, 14, 17, 15, 18, 19, 21,
    1, 4, 2, 6, 3, 7, 5, 9, 8, 10, 11, 14, 12, 16, 13, 17, 15, 19, 18, 20, 22, 23,
    2, 4, 3, 8, 5, 10, 7, 12, 9, 16, 11, 15, 13, 19, 14, 21, 17, 18, 20, 22,
    3, 4, 5, 8, 6, 7, 9, 12, 10, 11, 13, 16, 14, 15, 17, 19, 18, 21,
    5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 21,
    4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19
};

Texture2D<half3> InColor : register(t0);
Texture2D<half3> InNormals : register(t1);
Texture2D<float> InDepth : register(t2);
// Optional title-published linearised view depth. Read instead of InDepth when
// FLAGS_TITLE_LINEAR_DEPTH is set, so the canonical signed output - and therefore
// the geometry every consumer sees - is built on the title's own linearisation.
Texture2D<float> InTitleLinearDepth : register(t3);

RWTexture2D<half4> OutColor : register(u0);
RWTexture2D<float> OutLinearDepth : register(u1);
// RG: View space depth gradient (per-pixel central difference)
// BA: Octahedrally encoded world normal, used by the floor filter as an edge stop.
//
// This aliases the conversion's Motion buffer (RGBA16_FLOAT), which is scratch until the
// packing shader overwrites it, so the two extra channels cost nothing.
RWTexture2D<half4> OutDepthGradient : register(u2);

SamplerState LinearSampler : register(s0);

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
}

bool IsSet(uint mask)
{
    return (Flags & mask) == mask;
}

int2 GetSortID(const int i, const int2 gtID)
{
    const int offsetX = i / SORT_KERNEL_SIZE;
    const int offsetY = i % SORT_KERNEL_SIZE;
    return gtID + int2(offsetX, offsetY);
}

half4 GetConservativeColor(const uint2 groupID, const int2 gtID)
{
    const int2 smID = gtID + s_SM_Med_HaloOffset;
    half sortKeys[s_SetSize];
    int16_t sortValues[s_SetSize];
    
    // Populate sorting keys: luminance in X, flat index (0-24) in Y
    [unroll]
    for (int i1 = 0; i1 < s_SetSize; i1++)
    {
        const int2 smID = GetSortID(i1, gtID);
        const half lum = g_Color[smID.x][smID.y].a;
        
        sortKeys[i1] = lum;
        sortValues[i1] = int16_t(i1);
    }

    // Sorting network - ascending order
    [unroll]
    for (int k = 0; k < kSortNetworkSize; k++)
    {
        const int pairIndex = 2 * k;
        const uint lower = SortNetwork[pairIndex];
        const uint upper = SortNetwork[pairIndex + 1];
        
        const half keyA = sortKeys[lower];
        const int16_t valA = sortValues[lower];

        const half keyB = sortKeys[upper];
        const int16_t valB = sortValues[upper];
        
        const bool swap = (keyA > keyB);
        sortKeys[lower] = swap ? keyB : keyA;
        sortValues[lower] = swap ? valB : valA;
        
        sortKeys[upper] = swap ? keyA : keyB;
        sortValues[upper] = swap ? valA : valB;
    }

    // Scan the sorted distribution for jump discontinuities.
    //
    // Jump discontinuities indicate multimodal behavor and/or edges. In general, there 
    // are up to three populations, not counting geometric edges: RT shadows and AO, raster 
    // lighting and alpha effects, and RT lighting.
    //
    // Shadows generally cluster below the 30th percentile, sparse specular RT is fully rejected
    // around the 30th-40th perecntiles, and raster lighting can occur anywhere, as it is the 
    // dominant mode in a composited raster + raw RT image.
    int targetBin = s_MinBin;
    float spread = 0.0f;
    bool isSafe = true;

    [unroll]
    for (int i3 = s_MinBin; i3 <= s_MaxBin; i3++)
    {
        const int lowerID = i3 - s_SpreadWindowSize;
        const int upperID = i3 + s_SpreadWindowSize;
        const float binSpread = abs(sortKeys[lowerID] - sortKeys[upperID]) * rcp(sortKeys[i3] + 1e-2f);
        const bool isDiscontinuous = (binSpread > (spread * 2.0f + 0.1f));

        isSafe = isSafe && !isDiscontinuous;
        targetBin = isSafe ? i3 : targetBin;
        spread = isSafe ? binSpread : spread;
    }
    
    // The harsher the clamp, the more samples are effectively discarded, and the less 
    // trustworthy the signal becomes.
    const half pct = half(float(targetBin) * s_BinIndexToPct);
    const half instability = half(sqrt(smoothstep(0.9f, 0.1f, pct)));
    
    // Get color at the set percentile, clamped below the current color
    const int2 binID = GetSortID(sortValues[targetBin], gtID);
    const half4 binColor = g_Color[binID.x][binID.y];
    half4 stableColor = min(binColor, g_Color[smID.x][smID.y]);

    // Interpolate toward safer lower bound as instability increases
    const half4 minColor = 0.2h * binColor;
    stableColor.rgb = (half3) lerp(stableColor.rgb, minColor.rgb, instability);
    
    // The instability was published here and carried through every filter pass so a runtime
    // gate could withhold the floor where the seed's neighbourhood was inconsistent. Measured
    // against the noise it was meant to remove, the gate changed nothing its default did not
    // already do, so the channel is no longer published. The value still steers the pull toward
    // the safe lower bound above, which is where it earned its place.
    return half4(stableColor.rgb, 0.0h);
}

float2 GetDepthGradient(const uint2 groupID, const int2 gtID)
{
    const int2 smID = gtID + s_SM_Depth_HaloOffset;
    float2 gradient = 0.0f;
    
    gradient.x = g_Depth[smID.x + 1][smID.y] - g_Depth[smID.x - 1][smID.y];
    gradient.y = g_Depth[smID.x][smID.y + 1] - g_Depth[smID.x][smID.y - 1];
    gradient *= 0.5f;
        
    return gradient;
}

float3 GetViewSpacePos(const int2 px)
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
        inDepth = clamp(abs(inDepth), NearPlane, FarPlane);
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
        const float signedDepth = depthSign * clamp(abs(viewSpacePos.z), NearPlane, FarPlane);
        const float safeViewZ = (viewSpacePos.z < 0.0f)
            ? min(viewSpacePos.z, -1e-6f)
            : max(viewSpacePos.z, 1e-6f);
        viewSpacePos *= signedDepth / safeViewZ;
        viewSpacePos.z = signedDepth;
    }
    
    return viewSpacePos;
}

void PopulateSharedMemory(const uint2 groupID, const int2 gtID)
{
    const uint flatID = gtID.x + gtID.y * s_ThreadGroupSize.x;
    const int2 maxBounds = int2(RenderSize.xy) - 1;
    const int2 pxMedOrigin = groupID.xy * s_ThreadGroupSize - s_SM_Med_HaloOffset;
    
    [unroll]
    for (int i1 = 0; i1 < s_SM_Med_LoadsPerThread; i1++)
    {
        const uint smFlatID = flatID + i1 * NUM_THREADS;
        
        if (smFlatID < s_SM_Med_ElementCount)
        {
            const int2 smID = int2(smFlatID % s_SM_Med_Size.x, smFlatID / s_SM_Med_Size.x);
            const int2 px = clamp(pxMedOrigin + smID, int2(0, 0), maxBounds);
            const float3 color = GetSafeFP16(InColor[px + int2(InputBase.xy)].rgb);
            
            g_Color[smID.x][smID.y] = half4(color, GetLuminance(color));
        }
    }
    
    const int2 pxDepthOrigin = groupID.xy * s_ThreadGroupSize - s_SM_Depth_HaloOffset;
    
    [unroll]
    for (int i2 = 0; i2 < s_SM_Depth_LoadsPerThread; i2++)
    {
        const uint smFlatID = flatID + i2 * NUM_THREADS;
        
        if (smFlatID < s_SM_Depth_ElementCount)
        {
            const int2 smID = int2(smFlatID % s_SM_Depth_Size.x, smFlatID / s_SM_Depth_Size.x);
            const int2 px = clamp(pxDepthOrigin + smID, int2(0, 0), maxBounds);
            g_Depth[smID.x][smID.y] = GetViewSpacePos(px).z;
        }
    }
}

[RootSignature(MainRS)]
[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void CSMain(uint3 groupID : SV_GroupID, uint3 gtID : SV_GroupThreadID)
{
    const int2 px = groupID.xy * s_ThreadGroupSize + gtID.xy;
    PopulateSharedMemory(groupID.xy, gtID.xy);
    GroupMemoryBarrierWithGroupSync();

    if (px.x >= RenderSize.x || px.y >= RenderSize.y)
        return;

    const int2 smID = gtID.xy + s_SM_Depth_HaloOffset;
    const half4 color = GetConservativeColor(groupID.xy, gtID.xy);
    const float depth = g_Depth[smID.x][smID.y];
    const float2 gradient = GetDepthGradient(groupID.xy, gtID.xy);

    OutColor[px] = color;
    OutLinearDepth[px] = depth;
    // Surface orientation guide for the floor filter.
    //
    // InNormals was already bound here but never sampled. Depth alone cannot tell a
    // silhouette from a crease, so the wavelet had to stay conservative everywhere and
    // raster lighting ended up imprinted in the denoiser signal.
    const float3 worldNormal = SafeNormalize(InNormals[px + int2(NormalBase)].rgb,
                                             float3(0.0f, 0.0f, 1.0f));
    const half2 octNormal = half2(OctahedralEncode(worldNormal));

    OutDepthGradient[px] = half4(gradient, octNormal);
}
