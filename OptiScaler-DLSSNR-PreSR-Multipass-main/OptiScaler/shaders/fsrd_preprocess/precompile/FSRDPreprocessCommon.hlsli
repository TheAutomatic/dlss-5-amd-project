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

// Branch-free rank selection. Both forms are sorting networks over min/max, so they
// compile to plain ALU with no divergence.
#define FSRD_SORT_PAIR(x, y) { const float fsrdSortTmp = min(x, y); y = max(x, y); x = fsrdSortTmp; }

float Median3(float a, float b, float c)
{
    return max(min(a, b), min(max(a, b), c));
}

// Optimal 9-comparator sorting network for five elements. Only the middle rank is
// read, but the network is cheap enough that stopping early saves nothing useful.
float Median5(float v0, float v1, float v2, float v3, float v4)
{
    FSRD_SORT_PAIR(v0, v1);
    FSRD_SORT_PAIR(v3, v4);
    FSRD_SORT_PAIR(v2, v4);
    FSRD_SORT_PAIR(v2, v3);
    FSRD_SORT_PAIR(v1, v4);
    FSRD_SORT_PAIR(v0, v3);
    FSRD_SORT_PAIR(v0, v2);
    FSRD_SORT_PAIR(v1, v3);
    FSRD_SORT_PAIR(v1, v2);
    return v2;
}

// Partial network that resolves only the middle rank of nine elements: the standard
// 19-comparator median-of-9. Ranks either side of the middle are left unsorted, which
// is what makes it cheaper than a full sort.
float Median9(float v0, float v1, float v2, float v3, float v4,
              float v5, float v6, float v7, float v8)
{
    // Reduce the first six to a known min and max, discarding both from contention.
    FSRD_SORT_PAIR(v0, v3); FSRD_SORT_PAIR(v1, v4); FSRD_SORT_PAIR(v2, v5);
    FSRD_SORT_PAIR(v0, v1); FSRD_SORT_PAIR(v0, v2);
    FSRD_SORT_PAIR(v4, v5); FSRD_SORT_PAIR(v3, v5);

    // Fold in the remaining three one at a time, each time dropping the new extremes.
    FSRD_SORT_PAIR(v1, v2); FSRD_SORT_PAIR(v3, v4);
    FSRD_SORT_PAIR(v1, v3); FSRD_SORT_PAIR(v1, v6);
    FSRD_SORT_PAIR(v4, v6); FSRD_SORT_PAIR(v2, v6);

    FSRD_SORT_PAIR(v2, v3); FSRD_SORT_PAIR(v4, v7);
    FSRD_SORT_PAIR(v2, v4); FSRD_SORT_PAIR(v3, v7);

    FSRD_SORT_PAIR(v4, v8); FSRD_SORT_PAIR(v3, v8);
    FSRD_SORT_PAIR(v3, v4);

    return v4;
}

// Computes statistical variance/spread using the mean of squares and mean.
// Var(X) = E[X^2] - (E[X])^2 = sigma^2
float GetVariance(float meanSq, float mean)
{
    return max(meanSq - Square(mean), 0.0f);
}

// Computes squared coefficient of variation for a scale-invariant measure 
// of variance. (sigma / mu)^2
float GetCOVSquared(float meanSq, float mean)
{
    const float squaredMean = Square(mean);
    const float variance = max(meanSq - squaredMean, 0.0f);
    return variance * rcp(max(squaredMean, 1e-4f));
}

// Computes coefficient of variation for a scale-invariant measure 
// of variance. sigma / mu
float GetCOV(float meanSq, float mean)
{
    const float variance = max(meanSq - Square(mean), 0.0f);
    return sqrt(variance) * rcp(max(mean, 1e-2f));
}

// Computes a relative similarity score between value and baseline.
// Returns 1.0 when values are identical, and falls off based on relative difference.
float GetRelativeSimilarity(float value, float baseline)
{
    const float den = max(max(value, baseline), 1e-2f);
    const float delta = abs(baseline - value) * rcp(den);
    return min(max(1.0f - delta, 0.0f), 1.0f);
}

// Computes a relative similarity score between value and baseline. 
// Returns 0 below threshold, smoothly transitions to 1.0 as similarity approaches 1.
float GetRelativeSimilarity(float value, float baseline, float threshold)
{
    return smoothstep(threshold, 1.0f, GetRelativeSimilarity(value, baseline));
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

// Edge-stopping weight for surface orientation: 1.0 for coplanar normals, falling off as
// the angle between them grows. Higher sharpness stops harder at creases.
float GetNormalWeight(float3 n0, float3 n1, float sharpness)
{
    // Floored so pow() stays defined at sharpness 0, where the term disables itself.
    return pow(max(saturate(dot(n0, n1)), 1e-4f), sharpness);
}

// Guards the zero vector produced by uninitialized or skipped pixels.
float3 SafeNormalize(float3 n, float3 fallback)
{
    const float len = length(n);
    return (len > 1e-3f) ? (n * rcp(len)) : fallback;
}

// Quadratic smooth minimum. k is the blend radius in the units of the inputs; k <= 0
// returns the exact min(). The result never exceeds min(a, b), and dips below it by up
// to k/4 where the two inputs agree. Callers that need a bound on the result - such as
// a radiance floor that must stay non-negative - must clamp it themselves.
float3 SoftMin(float3 a, float3 b, float k)
{
    if (k <= 0.0f)
        return min(a, b);

    const float3 h = saturate(0.5f + 0.5f * (b - a) * rcp(k));
    return lerp(b, a, h) - (k * h) * (1.0f - h);
}

// Material agreement between two albedo samples.
//
// Luminance cannot tell an albedo edge, where a filter should stop, from a shadow or
// reflection edge, where it should not. Diffuse albedo is the material itself: view
// independent, illumination free, and free of the Fresnel sweep that makes specular albedo
// vary across a curved panel with no material change under it.
//
// Compared in two parts so a dark and a bright patch of the same paint agree: relative
// intensity, and chromaticity with intensity divided out.
float GetAlbedoAgreement(float3 a0, float3 a1)
{
    const float l0 = GetLuminance(a0);
    const float l1 = GetLuminance(a1);
    const float wIntensity = GetRelativeSimilarity(l0, l1);

    const float3 c0 = a0 * rcp(max(l0, 1e-3f));
    const float3 c1 = a1 * rcp(max(l1, 1e-3f));
    const float3 dc = abs(c0 - c1);

    const float wChroma = saturate(1.0f - 0.5f * (dc.r + dc.g + dc.b));

    return wIntensity * wChroma;
}
