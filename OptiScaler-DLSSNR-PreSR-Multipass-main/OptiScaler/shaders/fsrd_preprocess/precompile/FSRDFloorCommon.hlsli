#ifndef FSRD_FLOOR_COMMON
#define FSRD_FLOOR_COMMON

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

// Shared surface tests for the seed, wavelet and detail reconstruction. All depth
// arguments are canonical signed view Z; colours are linear radiance.
float3 FloorRadiance(float3 c)
{
    return float3(isfinite(c.x) ? clamp(c.x, 0.0f, 65500.0f) : 0.0f,
                  isfinite(c.y) ? clamp(c.y, 0.0f, 65500.0f) : 0.0f,
                  isfinite(c.z) ? clamp(c.z, 0.0f, 65500.0f) : 0.0f);
}

float3 FloorNormal(float3 n)
{
    const float l2 = dot(n, n);
    return all(isfinite(n)) && l2 > 1e-6f ? n * rsqrt(l2) : float3(0, 0, 1);
}

float FloorMaterialWeight(float3 a, float3 b)
{
    // Sanitize once while loading the guide, not again for every neighbour.
    const float scale = max(max(GetLuminance(a), GetLuminance(b)), 0.02f);
    const float3 delta = a - b;
    return Square(saturate(1.0f - 4.0f * dot(delta, delta) / (scale * scale)));
}

float FloorSurfaceWeight(float z, float tapZ, float2 gradient, float2 offset,
                         float3 n, float3 tapN, float3 a, float3 tapA)
{
    if (!isfinite(z) || !isfinite(tapZ) || z * tapZ <= 0.0f)
        return 0.0f;
    const float prediction = clamp(dot(gradient, offset), -0.25f * abs(z), 0.25f * abs(z));
    const float error = abs(z + prediction - tapZ);
    const float depthWeight = Square(saturate(1.0f - error / max(0.01f * abs(z), 1e-3f)));
    const float normalWeight = Square(saturate((dot(n, tapN) - 0.9f) * 10.0f));
    return depthWeight * normalWeight * FloorMaterialWeight(a, tapA);
}

// At silhouettes a central difference extrapolates across two different surfaces.
// Use the smaller one-sided derivative; a plane still produces the exact slope.
float FloorDepthDerivative(float left, float center, float right)
{
    const float a = center - left;
    const float b = right - center;
    return isfinite(a) && isfinite(b) ? (abs(a) < abs(b) ? a : b) : 0.0f;
}

float FloorRangeWeight(float delta, float scale)
{
    return Square(saturate(1.0f - Square(delta / max(scale, 1e-5f))));
}

#endif
