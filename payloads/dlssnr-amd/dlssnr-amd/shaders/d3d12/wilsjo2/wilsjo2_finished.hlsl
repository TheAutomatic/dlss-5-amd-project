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
Texture2D<float4> t2 : register(t2);
SamplerState _t2_sampler : register(s2);
Texture2D<float4> t1 : register(t1);
SamplerState _t1_sampler : register(s1);
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
    bool _73 = p.x >= cb.Load(8);
    bool _84;
    if (!_73)
    {
        _84 = p.y >= cb.Load(12);
    }
    else
    {
        _84 = _73;
    }
    return _84;
}

float3 saturate3(float3 x)
{
    return clamp(x, 0.0f.xxx, 1.0f.xxx);
}

float3 decode_pq(float3 code)
{
    float3 param = code;
    float3 p = pow(saturate3(param), 0.0126833133399486541748046875f.xxx);
    return pow(max(p - 0.8359375f.xxx, 0.0f.xxx) / max(18.8515625f.xxx - (p * 18.6875f), 9.9999999747524270787835121154785e-07f.xxx), 6.277394771575927734375f.xxx) * 125.0f;
}

float3 to709(float3 v)
{
    return float3(dot(float3(1.66049098968505859375f, -0.58764111995697021484375f, -0.0728498995304107666015625f), v), dot(float3(-0.124550499022006988525390625f, 1.13289988040924072265625f, -0.0083494000136852264404296875f), v), dot(float3(-0.01815080083906650543212890625f, -0.100578896701335906982421875f, 1.11872971057891845703125f), v));
}

float3 to2020(float3 v)
{
    return float3(dot(float3(0.627403914928436279296875f, 0.3292829990386962890625f, 0.04331310093402862548828125f), v), dot(float3(0.069097302854061126708984375f, 0.9195404052734375f, 0.011362300254404544830322265625f), v), dot(float3(0.0163914002478122711181640625f, 0.088013298809528350830078125f, 0.8955953121185302734375f), v));
}

float3 encode_pq(float3 light)
{
    float3 param = light / 125.0f.xxx;
    float3 p = pow(saturate3(param), 0.1593017578125f.xxx);
    return pow((0.8359375f.xxx + (p * 18.8515625f)) / (1.0f.xxx + (p * 18.6875f)), 78.84375f.xxx);
}

bool finite1(float v)
{
    bool _39 = isnan(v);
    bool _45;
    if (!_39)
    {
        _45 = isinf(v);
    }
    else
    {
        _45 = _39;
    }
    return !_45;
}

void comp_main()
{
    uint2 p = gl_GlobalInvocationID.xy;
    uint2 param = p;
    if (outside(param))
    {
        return;
    }
    float4 pixel = t0.Load(int3(int2(p), 0));
    if (cb.Load(0) == 20u)
    {
        float3 param_1 = pixel.xyz;
        float3 param_2 = decode_pq(param_1);
        u0[int2(p)] = float4(to709(param_2), pixel.w);
        return;
    }
    float4 base = t2.Load(int3(int2(p), 0));
    float3 param_3 = pixel.xyz;
    float3 param_4 = to2020(param_3);
    float3 result = encode_pq(param_4);
    float param_5 = result.x;
    bool _252 = finite1(param_5);
    bool _259;
    if (_252)
    {
        float param_6 = result.y;
        _259 = finite1(param_6);
    }
    else
    {
        _259 = _252;
    }
    bool _267;
    if (_259)
    {
        float param_7 = result.z;
        _267 = finite1(param_7);
    }
    else
    {
        _267 = _259;
    }
    bool ok = _267;
    float3 _272;
    if (ok)
    {
        _272 = result;
    }
    else
    {
        _272 = base.xyz;
    }
    u0[int2(p)] = float4(_272, base.w);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
