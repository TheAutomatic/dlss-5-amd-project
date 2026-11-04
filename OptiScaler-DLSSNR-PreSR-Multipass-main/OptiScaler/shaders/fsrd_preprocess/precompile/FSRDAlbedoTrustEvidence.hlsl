// Unsupported-albedo evidence, the first of the recovery passes that run after RR.
//
// A transparent layer such as water hands the denoiser the material guides of the surface
// beneath it: the sea floor's albedo and roughness arrive with radiance that is the water's
// reflection and volume. Demodulating by that albedo and remodulating after RR prints the
// sea floor into the image, which the water's own volume would have hidden. The local test
// for it is that the albedo varies while the light does not: on a real texture the light
// carries the albedo's structure (or structure of its own), on a hidden surface it is flat.
//
// The light is judged on RR's own output of the unmodulated specular path plus the
// remodulated diffuse path and the diffuse part of Skip. Before RR these parts add up to the
// title's colour; the albedo reaches the witness only through RR's denoising of the
// demodulated diffuse part, a small share on the reflective surfaces this targets. So a
// strong existing stain can barely vouch for the albedo that caused it. Output: x = unsupported structure, y = albedo
// structure mass; FSRDAlbedoTrustPropagate spreads both over the surface before
// composition takes their ratio.
#include "FSRDPreprocessCommon.hlsli"

#define MainRS \
    "RootFlags(0), CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors = 11)), " \
    "DescriptorTable(UAV(u0, numDescriptors = 1))"

Texture2D<half4> InDirectSpecularDenoised : register(t0);
Texture2D<half4> InDirectDiffuse : register(t1);
Texture2D<half4> InSpecularAlbedo : register(t2);
Texture2D<half4> InDiffuseAlbedo : register(t3);
Texture2D<half4> InSkipSignal : register(t4);
Texture2D<float> InLinearDepth : register(t5);
Texture2D<half4> InNormals : register(t6);
// A: conversion's eligibility; ineligible pixels neither vote nor receive the blend.
Texture2D<half4> InDirectSpecularSignal : register(t7);
Texture2D<half4> InIndirectDiffuseDenoised : register(t8);
// The main specular and diffuse RR inputs: each lobe's divisor-floor loss in Skip.
Texture2D<half4> InIndirectSpecularSignal : register(t9);
Texture2D<half4> InDirectDiffuseSignal : register(t10);
RWTexture2D<float2> OutAlbedoTrust : register(u0);

cbuffer CB_AlbedoTrust : register(b0)
{
    float4 DstTexSize;
    int StepSize;
    uint Flags;
    float DemodDivisorFloor;
    float _Reserved0;
}

// A 16x16 group loads a 24x24 tile, 2.25 source samples per output instead of 4 with 8x8.
// TrustEvidence::kThreadGroupSize (FSRDShaderData.h) dispatches it.
#define THREAD_GROUP_SIZE_X 16
#define THREAD_GROUP_SIZE_Y 16
#define NUM_THREADS (THREAD_GROUP_SIZE_X * THREAD_GROUP_SIZE_Y)
// Radius four: the 9x9 window the replayed captures were evaluated with.
#define TILE_X (THREAD_GROUP_SIZE_X + 8)
#define TILE_Y (THREAD_GROUP_SIZE_Y + 8)
DEFINE_LDS_CONFIG(s_SM, 9);
groupshared float g_LogAlbedo[TILE_Y][TILE_X];
groupshared float g_LogLight[TILE_Y][TILE_X];
groupshared float g_Depth[TILE_Y][TILE_X];
// Padded to half4: one aligned LDS read per tap.
groupshared half4 g_Normal[TILE_Y][TILE_X];

// The share of a channel the conversion allocated to the specular lobe.
float3 SpecularShare(float3 spec, float3 diff)
{
    const float3 total = spec + diff;
    const float3 splitT = saturate((total - 0.5f * DemodDivisorFloor) / (0.5f * DemodDivisorFloor));
    return spec * rcp(max(total, DemodDivisorFloor)) * splitT * splitT * (3.0f - 2.0f * splitT);
}

// The diffuse part of Skip, which belongs to the diffuse half of the witness. Each lobe's
// divisor-floor loss stays with its own lobe; the remaining Skip follows the albedo ratio
// (see SpecularSkip in FSRDOutputComp).
float3 DiffuseSkip(int2 q, float3 spec, float3 diff)
{
    const float3 specLoss = float3(InIndirectSpecularSignal[q].rgb) * (max(max(spec, DemodDivisorFloor), 1e-4f) - spec);
    const float3 diffLoss = float3(InDirectDiffuseSignal[q].rgb) * ((Flags & 1u) != 0 ? 2.0f : 1.0f) *
        (max(max(diff, DemodDivisorFloor), 1e-4f) - diff);
    return (float3(InSkipSignal[q].rgb) - specLoss - diffLoss) * (1.0f - SpecularShare(spec, diff)) + diffLoss;
}

[RootSignature(MainRS)]
[numthreads(16, 16, 1)]
void CSMain(uint3 groupID : SV_GroupID, uint3 gtID : SV_GroupThreadID)
{
    const int2 bounds = int2(DstTexSize.xy) - 1;
    const int2 origin = int2(groupID.xy * uint2(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y)) - int2(s_SM_HaloOffset);
    const uint tid = gtID.x + gtID.y * THREAD_GROUP_SIZE_X;
    [unroll] for (uint i = 0; i < s_SM_LoadsPerThread; ++i)
    {
        const uint flat = tid + i * NUM_THREADS;
        if (flat < TILE_X * TILE_Y)
        {
            const int2 s = int2(flat % TILE_X, flat / TILE_X);
            const int2 source = origin + s;
            const int2 q = clamp(source, 0, bounds);
            const float3 spec = InSpecularAlbedo[q].rgb, diff = InDiffuseAlbedo[q].rgb;
            const float3 witness = float3(InDirectSpecularDenoised[q].rgb) +
                (float3(InDirectDiffuse[q].rgb) +
                 ((Flags & 1u) != 0 ? float3(InIndirectDiffuseDenoised[q].rgb) : 0.0f)) * diff +
                DiffuseSkip(q, spec, diff);
            // Clamping makes loads safe, but repeated border texels cannot count
            // as independent support. Mark them invalid once while loading LDS.
            const bool eligible = all(source >= 0) && all(source <= bounds) &&
                InDirectSpecularSignal[q].a >= 0.5f;
            const float z = InLinearDepth[q];
            g_LogAlbedo[s.y][s.x] = log(max(GetLuminance(spec + diff), 1.0f / 255.0f));
            g_LogLight[s.y][s.x] = log(max(GetLuminance(max(witness, 0.0f)), 1e-4f));
            // An ineligible tap carries no depth, which removes it from every window.
            g_Depth[s.y][s.x] = eligible && isfinite(z) ? z : 0.0f;
            g_Normal[s.y][s.x] = half4(half3(OctahedralDecode(InNormals[q].xy)), 0.0h);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 p = int2(groupID.xy * uint2(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y) + gtID.xy);
    if (any(p > bounds))
        return;
    const int2 c = int2(gtID.xy) + int2(s_SM_HaloOffset);
    const float z = g_Depth[c.y][c.x];
    if (z == 0.0f)
    {
        OutAlbedoTrust[p] = 0.0f;
        return;
    }
    const float3 n = float3(g_Normal[c.y][c.x].xyz);
    const float depthTolerance = max(0.02f * abs(z), 1e-3f);
    float count = 0, sumA = 0, sumAA = 0, sumL = 0, sumLL = 0;
    [unroll] for (int y = -4; y <= 4; ++y)
    {
        [unroll] for (int x = -4; x <= 4; ++x)
        {
            const int2 s = c + int2(x, y);
            const float tapZ = g_Depth[s.y][s.x];
            const float reach = 1.0f + 0.5f * float(max(abs(x), abs(y)));
            if (tapZ * z > 0.0f && abs(tapZ - z) <= depthTolerance * reach &&
                dot(float3(g_Normal[s.y][s.x].xyz), n) >= 0.9f)
            {
                const float a = g_LogAlbedo[s.y][s.x], l = g_LogLight[s.y][s.x];
                count += 1.0f;
                sumA += a; sumAA += a * a;
                sumL += l; sumLL += l * l;
            }
        }
    }
    if (count <= 10.0f)
    {
        OutAlbedoTrust[p] = 0.0f;
        return;
    }
    const float meanA = sumA / count, meanL = sumL / count;
    const float albedoVariance = max(sumAA / count - meanA * meanA, 0.0f);
    const float lightVariance = max(sumLL / count - meanL * meanL, 0.0f);
    // Structure mass: enough albedo variation over enough same-surface samples that the
    // question is meaningful. Thin poles and silhouettes abstain rather than vote.
    const float structure = smoothstep(0.02f, 0.06f, albedoVariance) * smoothstep(10.0f, 30.0f, count);
    // Unsupported: the light varies much less than the albedo would make it.
    const float unsupported = structure *
        (1.0f - smoothstep(0.05f, 0.2f, lightVariance / max(albedoVariance, 1e-4f)));
    OutAlbedoTrust[p] = float2(unsupported, structure);
}
