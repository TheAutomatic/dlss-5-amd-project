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
RWTexture2D<float4> u0 : register(u4);
Texture2D<float4> t1 : register(t1);
SamplerState _t1_sampler : register(s1);
Texture2D<float4> t2 : register(t2);
SamplerState _t2_sampler : register(s2);
Texture2D<float4> t3 : register(t3);
SamplerState _t3_sampler : register(s3);
RWTexture2D<float4> u1 : register(u5);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

bool outside(uint2 p)
{
    bool _105 = p.x >= cb.Load(8);
    bool _115;
    if (!_105)
    {
        _115 = p.y >= cb.Load(12);
    }
    else
    {
        _115 = _105;
    }
    return _115;
}

bool finite1(float v)
{
    bool _37 = isnan(v);
    bool _43;
    if (!_37)
    {
        _43 = isinf(v);
    }
    else
    {
        _43 = _37;
    }
    return !_43;
}

float sanitize1(float v, float f)
{
    float param = v;
    return finite1(param) ? v : f;
}

float3 sanitize3(float3 v, float3 f)
{
    float param = v.x;
    float param_1 = f.x;
    float param_2 = v.y;
    float param_3 = f.y;
    float param_4 = v.z;
    float param_5 = f.z;
    return float3(sanitize1(param, param_1), sanitize1(param_2, param_3), sanitize1(param_4, param_5));
}

float3 saturate3(float3 x)
{
    return clamp(x, 0.0f.xxx, 1.0f.xxx);
}

void comp_main()
{
    uint2 p = gl_GlobalInvocationID.xy;
    uint2 param = p;
    if (outside(param))
    {
        return;
    }
    float4 answer = t0.Load(int3(int2(p), 0));
    float3 param_1 = answer.xyz;
    float3 param_2 = 0.5f.xxx;
    float3 param_3 = sanitize3(param_1, param_2);
    u0[int2(p)] = float4(saturate3(param_3), answer.w);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
