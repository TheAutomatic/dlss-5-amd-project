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
RWTexture2D<float4> u1 : register(u5);
RWTexture2D<float4> u0 : register(u4);
Texture2D<float4> t0 : register(t0);
SamplerState _t0_sampler : register(s0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    uint2 p = gl_GlobalInvocationID.xy;
    bool _29 = p.x >= cb.Load(8);
    bool _40;
    if (!_29)
    {
        _40 = p.y >= cb.Load(12);
    }
    else
    {
        _40 = _29;
    }
    if (_40)
    {
        return;
    }
    int2 s = int2(p + uint2(cb.Load(44) & 65535u, cb.Load(44) >> uint(16)));
    int2 d = int2(p + uint2(cb.Load(48) & 65535u, cb.Load(48) >> uint(16)));
    float4 v = u1[s];
    u0[d] = v;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
