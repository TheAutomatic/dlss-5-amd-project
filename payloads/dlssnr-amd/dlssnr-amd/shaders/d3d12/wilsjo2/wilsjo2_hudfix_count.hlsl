// Made by windows/build/build_d3d12_hlsl.sh from the SPIR-V of the same name (SPIRV-Cross). Do not edit.
bool nr_isnan(float x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
bool2 nr_isnan(float2 x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
bool3 nr_isnan(float3 x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
bool4 nr_isnan(float4 x) { return (asuint(x) & 0x7fffffffu) > 0x7f800000u; }
bool nr_isinf(float x) { return (asuint(x) & 0x7fffffffu) == 0x7f800000u; }
bool2 nr_isinf(float2 x) { return (asuint(x) & 0x7fffffffu) == 0x7f800000u; }
bool3 nr_isinf(float3 x) { return (asuint(x) & 0x7fffffffu) == 0x7f800000u; }
bool4 nr_isinf(float4 x) { return (asuint(x) & 0x7fffffffu) == 0x7f800000u; }
#define isnan nr_isnan
#define isinf nr_isinf
ByteAddressBuffer cb : register(t6);
Texture2D<float4> t0 : register(t0);
SamplerState _t0_sampler : register(s0);
Texture2D<float4> t1 : register(t1);
SamplerState _t1_sampler : register(s1);
globallycoherent RWTexture2D<uint> u1 : register(u5);
Texture2D<float4> t2 : register(t2);
SamplerState _t2_sampler : register(s2);
Texture2D<float4> t3 : register(t3);
SamplerState _t3_sampler : register(s3);
RWTexture2D<float4> u0 : register(u4);

static uint3 gl_GlobalInvocationID;
static uint gl_LocalInvocationIndex;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
    uint gl_LocalInvocationIndex : SV_GroupIndex;
};

groupshared uint s_hud;
groupshared uint s_coarse;

bool differs(float3 a, float3 b, float tolerance)
{
    float3 scale = max(1.0f.xxx, abs(b));
    float3 _26 = abs(a - b);
    float3 _29 = scale * tolerance;
    return any(bool3(_26.x > _29.x, _26.y > _29.y, _26.z > _29.z));
}

void comp_main()
{
    int2 p = int2(gl_GlobalInvocationID.xy);
    bool _63 = p.x < int(cb.Load(8));
    bool _74;
    if (_63)
    {
        _74 = p.y < int(cb.Load(12));
    }
    else
    {
        _74 = _63;
    }
    bool inside = _74;
    if (gl_LocalInvocationIndex == 0u)
    {
        s_hud = 0u;
        s_coarse = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (inside)
    {
        float3 a = t0.Load(int3(p, 0)).xyz;
        float3 b = t1.Load(int3(p, 0)).xyz;
        float3 param = a;
        float3 param_1 = b;
        float param_2 = asfloat(cb.Load(56));
        if (differs(param, param_1, param_2))
        {
            uint _119;
            InterlockedAdd(s_hud, 1u, _119);
        }
        float3 param_3 = a;
        float3 param_4 = b;
        float param_5 = 0.0625f;
        if (differs(param_3, param_4, param_5))
        {
            uint _129;
            InterlockedAdd(s_coarse, 1u, _129);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gl_LocalInvocationIndex == 0u)
    {
        if (s_hud != 0u)
        {
            uint _145;
            InterlockedAdd(u1[int2(0, 0)], s_hud, _145);
        }
        if (s_coarse != 0u)
        {
            uint _154;
            InterlockedAdd(u1[int2(1, 0)], s_coarse, _154);
        }
    }
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    gl_LocalInvocationIndex = stage_input.gl_LocalInvocationIndex;
    comp_main();
}
