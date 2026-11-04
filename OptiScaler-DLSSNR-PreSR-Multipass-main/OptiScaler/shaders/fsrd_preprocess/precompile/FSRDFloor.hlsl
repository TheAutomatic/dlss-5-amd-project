#include "FSRDPreprocessCommon.hlsli"
#include "FSRDFloorCommon.hlsli"

#define MainRS \
    "RootFlags(0), CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 4)), " \
    "DescriptorTable(UAV(u0, numDescriptors = 1))"
Texture2D<half4> InColor : register(t0);
Texture2D<float> InLinearDepth : register(t1);
Texture2D<half4> InDepthGradient : register(t2);
Texture2D<float3> InDiffAlbedo : register(t3);
RWTexture2D<half4> OutColor : register(u0);
cbuffer CB_Analysis : register(b0)
{
    float4 DstTexSize;
    int StepSize;
    uint _Reserved0;
    uint2 AlbedoBase;
}

[RootSignature(MainRS)]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const int2 p = int2(id.xy), bounds = int2(DstTexSize.xy)-1;
    if (any(p > bounds)) return;
    const float4 center = InColor[p];
    const float lum = GetLuminance(center.rgb);
    const float noise = max(center.a, 0.0f);
    const float range = max(lum * 0.04f, noise * 3.25f);
    // No noisy evidence: only the small kernel is needed. All five dispatches have
    // the same contract, so later passes can return locally without another buffer.
    if (StepSize > 2 && noise < max(lum * 0.005f, 1e-5f))
    {
        OutColor[p] = half4(center);
        return;
    }
    const float z = InLinearDepth[p];
    const float4 guide = InDepthGradient[p];
    const float3 n = OctahedralDecode(guide.zw);
    const float3 a = FloorRadiance(InDiffAlbedo[p + int2(AlbedoBase)]);
    float3 sum = center.rgb;
    float total = 1.0f;
    // Alternate axial/diagonal support across scales. Four surface-tested taps
    // avoid reading both crosses at every scale while retaining both orientations.
    const bool diagonal = StepSize == 2 || StepSize == 8;
    const int2 offsets[4] = {int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};
    [unroll]
    for (uint i=0; i<4; ++i)
    {
        const int2 o = offsets[i];
        const int2 offset = diagonal ? int2(o.x-o.y,o.x+o.y) : o;
        const int2 q = clamp(p + StepSize * offset, 0, bounds);
        const float4 c = InColor[q];
        const float4 g = InDepthGradient[q];
        const float surface = FloorSurfaceWeight(z, InLinearDepth[q], guide.xy, float2(q-p),
            n, OctahedralDecode(g.zw), a, FloorRadiance(InDiffAlbedo[q + int2(AlbedoBase)]));
        const float appearance = FloorRangeWeight(GetLuminance(c.rgb)-lum,
            max(range, max(c.a, noise) * 3.25f));
        const float spatial = diagonal ? 0.25f : 0.5f;
        const float w = surface * appearance * spatial;
        sum += c.rgb*w; total += w;
    }
    // Alpha is the original local uncertainty, not a vanished confidence channel.
    OutColor[p] = half4(FloorRadiance(sum/total), center.a);
}
