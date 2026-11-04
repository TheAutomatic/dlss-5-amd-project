#include "FSRDPreprocessCommon.hlsli"

// Encodes this frame's canonical RR linear depth (R32 view-space distance) into
// the zero-origin device-depth convention the SR context consumes. The terms
// come from the FINAL effective upscaler descriptor (user overrides resolved):
// encoding with the raw RR projection while SR decodes with different overrides
// would produce a distorted depth field.
//
// finite:      d = far * (1 - near / L) / (far - near)
// infinite:    d = 1 - near / L
// inverted output (reversed-Z SR context) uses the numerically stable forms
// finite:      d = near * (far / L - 1) / (far - near)
// infinite:    d = near / L
// L = abs(signed view depth); a non-finite or non-positive L has no depth
// meaning and encodes to the far plane.

#define MainRS \
    "RootFlags(0), CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 1)), " \
    "DescriptorTable(UAV(u0, numDescriptors = 1))"

#define FLAGS_INFINITE_FAR (1 << 0)
#define FLAGS_INVERTED_OUTPUT (1 << 1)
#define THREAD_GROUP_SIZE_X 8
#define THREAD_GROUP_SIZE_Y 8

Texture2D<float> InLinearDepth : register(t0);
RWTexture2D<float> OutEncodedDepth : register(u0);

cbuffer CB_UpscalerDepth : register(b0)
{
    float4 RenderSize; // XY: resolution - ZW: reciprocal

    float NearPlane; // > 0, physical view-space units
    float FarPlane;  // physical, or FLT_MAX with FLAGS_INFINITE_FAR; never 0
    uint Flags;
}

bool IsSet(uint mask) { return (Flags & mask) == mask; }

float EncodeDeviceDepth(float linearDepth)
{
    const float L = abs(linearDepth);
    if (!(L > 0.0f) || isinf(L))
        return IsSet(FLAGS_INVERTED_OUTPUT) ? 0.0f : 1.0f;

    if (IsSet(FLAGS_INVERTED_OUTPUT))
    {
        // Reversed forms avoid the cancellation in 1 - (1 - near/L) and
        // 1 - far*(1-near/L)/(far-near).
        if (IsSet(FLAGS_INFINITE_FAR))
            return saturate(NearPlane / L);
        return saturate((NearPlane / L - NearPlane / FarPlane) / (1.0f - NearPlane / FarPlane));
    }

    if (IsSet(FLAGS_INFINITE_FAR))
        return saturate(1.0f - NearPlane / L);
    return saturate((1.0f - NearPlane / L) / (1.0f - NearPlane / FarPlane));
}

[RootSignature(MainRS)]
[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]
void CSMain(uint3 dispatchID : SV_DispatchThreadID)
{
    if (any(dispatchID.xy >= uint2(RenderSize.xy)))
        return;

    OutEncodedDepth[dispatchID.xy] = EncodeDeviceDepth(InLinearDepth[dispatchID.xy]);
}
