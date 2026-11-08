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

static uint3 gl_WorkGroupID;
static uint3 gl_LocalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
};

groupshared float values[64];

uint2 spvTextureSize(Texture2D<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

float sum3(float3 a, float3 b)
{
    return mad(a.z, b.z, mad(a.x, b.x, a.y * b.y));
}

bool finite1(float v)
{
    bool _42 = isnan(v);
    bool _48;
    if (!_42)
    {
        _48 = isinf(v);
    }
    else
    {
        _48 = _42;
    }
    return !_48;
}

void comp_main()
{
    uint2 g = gl_WorkGroupID.xy;
    uint2 l = gl_LocalInvocationID.xy;
    uint lane = (l.y * 8u) + l.x;
    uint _84_dummy_parameter;
    int2 size = int2(spvTextureSize(t0, uint(0), _84_dummy_parameter));
    uint fw = uint(size.x);
    uint fh = uint(size.y);
    uint tx0 = (g.x * fw) / 64u;
    uint tx1 = ((g.x + 1u) * fw) / 64u;
    uint ty0 = (g.y * fh) / 64u;
    uint ty1 = ((g.y + 1u) * fh) / 64u;
    uint end_x = max(tx1, (tx0 + 1u));
    uint end_y = max(ty1, (ty0 + 1u));
    float sum = 0.0f;
    uint _137 = ty0 + l.y;
    float _190;
    for (uint ty = _137; ty < end_y; ty += 8u)
    {
        uint _150 = tx0 + l.x;
        for (uint tx = _150; tx < end_x; tx += 8u)
        {
            float3 c = max(t0.Load(int3(int2(int(min(tx, (fw - 1u))), int(min(ty, (fh - 1u)))), 0)).xyz, 0.0f.xxx);
            float3 param = c;
            float3 param_1 = float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f);
            float y = sum3(param, param_1);
            float param_2 = y;
            if (finite1(param_2))
            {
                _190 = max(y, 0.0f);
            }
            else
            {
                _190 = 0.0f;
            }
            sum += _190;
        }
    }
    values[lane] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32u; stride > 0u; stride = stride >> 1u)
    {
        if (lane < stride)
        {
            values[lane] += values[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0u)
    {
        float taken = float((end_x - tx0) * (end_y - ty0));
        float _258;
        if (taken > 0.0f)
        {
            _258 = values[0] / taken;
        }
        else
        {
            _258 = 0.0f;
        }
        u0[int2(g)] = float4(_258, 0.0f, 0.0f, 1.0f);
    }
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    comp_main();
}
