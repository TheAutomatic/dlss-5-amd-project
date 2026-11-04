// Unsupported-albedo propagation: one a-trous step of a same-surface normalized
// convolution over FSRDAlbedoTrustEvidence's (unsupported, structure) pair.
//
// A hidden surface's albedo varies only in places - the edge of a sand bank - while the
// water over it is one continuous surface whose radiance must be demodulated the same way
// everywhere: RR returns visibly different levels for two regions of equal light divided by
// different albedo levels. Six steps (strides 1..32) carry the vote across the flat interior
// of such a surface. Composition takes the ratio, so isolated false evidence on a textured
// facade is outvoted by the surrounding structure that the light does show, and a region
// without albedo structure keeps the original demodulation. Only geometry stops the spread.
// A stride compares its two ends only, so a thin occluder between two pieces of one plane
// does not stop it, and pixels conversion excluded (particles, bypassed surfaces) relay
// votes across the surface they cover; composition still leaves those pixels unchanged.
#include "FSRDPreprocessCommon.hlsli"

#define MainRS \
    "RootFlags(0), CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 3)), " \
    "DescriptorTable(UAV(u0, numDescriptors = 1))"

Texture2D<float2> InAlbedoTrust : register(t0);
Texture2D<float> InLinearDepth : register(t1);
Texture2D<half4> InNormals : register(t2);
RWTexture2D<float2> OutAlbedoTrust : register(u0);

cbuffer CB_AlbedoTrust : register(b0)
{
    float4 DstTexSize;
    int StepSize;
    uint Flags;
    float DemodDivisorFloor;
    float _Reserved0;
}

[RootSignature(MainRS)]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const int2 p = int2(id.xy), bounds = int2(DstTexSize.xy) - 1;
    if (any(p > bounds))
        return;
    float2 votes = InAlbedoTrust[p];
    const float z = InLinearDepth[p];
    if (isfinite(z) && z != 0.0f)
    {
        const float3 n = OctahedralDecode(InNormals[p].xy);
        const float tolerance = max(0.01f * abs(z), 1e-3f) * (1.0f + 0.25f * float(StepSize));
        const int2 offsets[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
        [unroll] for (uint i = 0; i < 4; ++i)
        {
            // Clamped taps repeat the border sample, as the replayed estimator did.
            const int2 q = clamp(p + StepSize * offsets[i], 0, bounds);
            const float tapZ = InLinearDepth[q];
            if (tapZ * z > 0.0f && abs(tapZ - z) <= tolerance &&
                dot(OctahedralDecode(InNormals[q].xy), n) >= 0.95f)
                votes += InAlbedoTrust[q];
        }
    }
    OutAlbedoTrust[p] = votes;
}
