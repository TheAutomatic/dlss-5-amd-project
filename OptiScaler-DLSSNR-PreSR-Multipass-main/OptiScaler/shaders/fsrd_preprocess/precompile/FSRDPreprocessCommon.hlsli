// Types not recognized by Intellisense
#ifdef __INTELLISENSE__
#define int16_t  int
#define uint16_t uint
#define int64_t  int
#define uint64_t uint
#endif

// Shared memory sizing utilities
#define GET_LDS_HALO(K)          ((K) / 2)
#define GET_LDS_DIM_X(K)         (THREAD_GROUP_SIZE_X + 2 * GET_LDS_HALO(K))
#define GET_LDS_DIM_Y(K)         (THREAD_GROUP_SIZE_Y + 2 * GET_LDS_HALO(K))
#define GET_LDS_TOTAL(K)         (GET_LDS_DIM_X(K) * GET_LDS_DIM_Y(K))
#define GET_LDS_LOADS(K, NumThr) ((GET_LDS_TOTAL(K) + (NumThr) - 1) / (NumThr))

// Defines constants used for shared memory sizing and indexing
#define DEFINE_LDS_CONFIG(Prefix, KSize) \
    static const uint Prefix##_Halo             = GET_LDS_HALO(KSize); \
    static const uint2 Prefix##_HaloOffset      = uint2(Prefix##_Halo, Prefix##_Halo); \
    static const uint2 Prefix##_Size            = uint2(GET_LDS_DIM_X(KSize), GET_LDS_DIM_Y(KSize)); \
    static const uint Prefix##_ElementCount     = GET_LDS_TOTAL(KSize); \
    static const float Prefix##_InvKernelSize   = 1.0f / (KSize * KSize); \
    static const uint Prefix##_LoadsPerThread   = GET_LDS_LOADS(KSize, NUM_THREADS);

// Defines a 2D shared memory array using predefined LDS config
#define DECLARE_LDS_ARRAY_2D(Type, Name, KSize) \
    groupshared Type Name[GET_LDS_DIM_X(KSize)][GET_LDS_DIM_Y(KSize)]

static const float s_PI = 3.14159265358979f;
static const float s_TAU = 2.0f * s_PI;

// Octahedral Encoding from AMD FSR-RR sample
float2 OctahedralEncode(float3 N)
{
    N.xy = float2(N.xy) / (abs(N.x) + abs(N.y) + abs(N.z));
    float2 k;
    k.x = N.x > 0.0f ? 1.0f : -1.0f;
    k.y = N.y > 0.0f ? 1.0f : -1.0f;
    
    if (N.z < 0.0f)
        N.xy = (1.0f - abs(float2(N.yx))) * k;
    
    return N.xy * 0.5f + 0.5f;
}

// Octahedral Decoding (For debug)
float3 OctahedralDecode(float2 UV)
{
    UV = 2.0f * (UV - 0.5f);
    float3 N = float3(UV, 1.0f - abs(UV.x) - abs(UV.y));
    float t = max(-N.z, 0.0f);
    float2 k;
    k.x = N.x >= 0.0f ? -t : t;
    k.y = N.y >= 0.0f ? -t : t;
    N.xy += k;
    return normalize(N);
}

float2 ClipToUV(float4 clipPosH)
{
    float2 coord = clipPosH.xy / clipPosH.w; // NDC
    coord.xy = 0.5f * coord.xy + 0.5f;
    coord.y = (1.0f - coord.y);  
    return coord;
}

float2 NDCToUV(float2 coord)
{
    coord.xy = 0.5f * coord.xy + 0.5f;
    coord.y = (1.0f - coord.y);
    return coord;
}

float2 UVToNDC(float2 coord)
{
    coord.y = (1.0f - coord.y);
    coord.xy = 2.0f * coord.xy - 1.0f;
    return coord;
}

float3 InvProjectPosition(float3 coord, float4x4 mat)
{
    coord.xy = UVToNDC(coord.xy);
    float4 projected = mul(mat, float4(coord, 1.0f));
    // An infinite far plane drives w to exactly zero at the NDC depth that maps to
    // infinity: z = 1 for standard-Z, z = 0 for reversed-Z. Dividing there yields
    // Inf, and a caller that then rescales the ray by depth/rayZ turns Inf * 0 into
    // NaN. Clamp the magnitude so the result stays a finite, very distant point that
    // the callers' near/far clamps can resolve.
    const float safeW = (projected.w < 0.0f)
        ? min(projected.w, -1e-6f)
        : max(projected.w, 1e-6f);
    projected.xyz /= safeW;

    return projected.xyz;
}

// Metalness Heuristic (Since unavailable in DLSS inputs)
float EstimateMetalness(float3 diffuse, float3 specular)
{
    const float lumDiff = dot(diffuse, float3(0.2126, 0.7152, 0.0722));
    const float lumSpec = dot(specular, float3(0.2126, 0.7152, 0.0722));
    const float maxSpec = max(max(specular.r, specular.g), specular.b);
    const float minSpec = min(min(specular.r, specular.g), specular.b);
    float chromaSpec = (maxSpec > 0.0) ? (maxSpec - minSpec) / maxSpec : 0.0;

    // Smoothly decrease metalness as diffuse luminance increases
    float diffFactor = smoothstep(0.02, 0.005, lumDiff); // Inverted: high when lumDiff low
    // Increase metalness with specular luminance and chromaticity
    float specFactor = smoothstep(0.04, 0.6, lumSpec) * smoothstep(0.0, 0.3, chromaSpec);

    return diffFactor * specFactor;
}

// Visualization Helpers

// High contrast color map for visualizing normalized scalars
// Blue = 0 -> Cyan -> Green -> Orange -> Red = 1
float3 TurboColormap(float x)
{
    const float4 kRedVec4 = float4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
    const float4 kGreenVec4 = float4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
    const float4 kBlueVec4 = float4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
    const float2 kRedVec2 = float2(-152.94239396, 59.28637943);
    const float2 kGreenVec2 = float2(4.27729857, 2.82956604);
    const float2 kBlueVec2 = float2(-89.90310912, 27.34824973);

    x = saturate(x);
    float4 v4 = float4(1.0, x, x * x, x * x * x);
    float2 v2 = v4.zw * v4.z;

    return float3(
        dot(v4, kRedVec4) + dot(v2, kRedVec2),
        dot(v4, kGreenVec4) + dot(v2, kGreenVec2),
        dot(v4, kBlueVec4) + dot(v2, kBlueVec2)
    );
}

// Visualizes 2D vectors as HSV. 
// Right = red, up = cyan/greenish, 
// left = cyan/blue, down = yellow/orange
float3 VisualizeMotionVec(float2 motion, float scalar)
{
    float angle = atan2(motion.y, motion.x);
    float mag = length(motion) * scalar;
    
    // Rough HSV to RGB
    float3 rgb = saturate(abs(fmod(angle / 6.2831853 + float3(0.0, 4.0, 2.0) / 6.0, 1.0) * 6.0 - 3.0) - 1.0);
    return rgb * saturate(mag);
}

float3 VisualizeSignedDiff(float val, float scale)
{
    // Red for negative, green for positive, black for zero
    float v = val * scale;
    return float3(saturate(-v), saturate(v), 0.0f);
}

half4 GetSafeFP16(float4 v)
{
    return (half4) min(max(v, 0.0f), 65500.0f);
}

half3 GetSafeFP16(float3 v)
{
    return (half3) min(max(v, 0.0f), 65500.0f);
}

half2 GetSafeFP16(float2 v)
{
    return (half2) min(max(v, 0.0f), 65500.0f);
}

half1 GetSafeFP16(float v)
{
    return (half) min(max(v, 0.0f), 65500.0f);
}

half3 GetSafeSignedFP16(float3 v)
{
    return (half3) clamp(v, -65500.0f, 65500.0f);
}

half2 GetSafeSignedFP16(float2 v)
{
    return (half2) clamp(v, -65500.0f, 65500.0f);
}

float Square(float x) { return x * x; }

float2 Square(float2 vec) { return float2(vec.x * vec.x, vec.y * vec.y); }

float3 Square(float3 vec) { return float3(vec.x * vec.x, vec.y * vec.y, vec.z * vec.z); }

float4 Square(float4 vec) { return float4(vec.x * vec.x, vec.y * vec.y, vec.z * vec.z, vec.w * vec.w); }

float GetLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

// Soft threshold helpers, carried over from the clarity-drafts branch.
//
// A binary classification test (x > k) flickers whenever the tested quantity hovers near
// k: neighbouring pixels and successive frames land on opposite sides of the step, and any
// branch keyed off the result changes discontinuously. These return the same decision with
// a C1 transition band of the given width, so downstream blends vary continuously.
float SoftAbove(float x, float edge, float width)
{
    return smoothstep(edge - max(width, 1e-6f), edge, x);
}

float SoftBelow(float x, float edge, float width)
{
    return smoothstep(edge, edge - max(width, 1e-6f), x);
}
