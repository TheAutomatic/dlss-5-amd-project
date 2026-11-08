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
    bool _160 = p.x >= cb.Load(8);
    bool _170;
    if (!_160)
    {
        _170 = p.y >= cb.Load(12);
    }
    else
    {
        _170 = _160;
    }
    return _170;
}

float maxch(float3 c)
{
    return max(c.x, max(c.y, c.z));
}

float hybrid(float m)
{
    if (m <= 0.75f)
    {
        return m;
    }
    float e = (m - 0.75f) / 0.25f;
    return 0.75f + (0.25f * (e * rsqrt((e * e) + 1.0f)));
}

float3 hybrid_encode(inout float3 v)
{
    v = max(v, 0.0f.xxx);
    float3 param = v;
    float m = maxch(param);
    if (m <= 9.9999999747524270787835121154785e-07f)
    {
        return v;
    }
    float param_1 = m;
    return v * (hybrid(param_1) / m);
}

float hybrid_inv(float y)
{
    if (y <= 0.75f)
    {
        return y;
    }
    float u = min((y - 0.75f) / 0.25f, 0.999998986721038818359375f);
    float e = u * rsqrt(max(1.0f - (u * u), 9.9999999392252902907785028219223e-09f));
    return 0.75f + (0.25f * e);
}

float3 hybrid_decode(inout float3 y)
{
    y = max(y, 0.0f.xxx);
    float3 param = y;
    float m = maxch(param);
    if (m <= 9.9999999747524270787835121154785e-07f)
    {
        return y;
    }
    float param_1 = m;
    return y * (hybrid_inv(param_1) / m);
}

void comp_main()
{
    uint2 p = gl_GlobalInvocationID.xy;
    uint2 param = p;
    if (outside(param))
    {
        return;
    }
    float4 v = t0.Load(int3(int2(p), 0));
    if (cb.Load(24) == 1u)
    {
        float3 param_1 = v.xyz;
        float3 _208 = hybrid_encode(param_1);
        v.x = _208.x;
        v.y = _208.y;
        v.z = _208.z;
    }
    else
    {
        if (cb.Load(24) == 2u)
        {
            float3 param_2 = v.xyz;
            float3 _224 = hybrid_decode(param_2);
            v.x = _224.x;
            v.y = _224.y;
            v.z = _224.z;
        }
    }
    u0[int2(p)] = float4(min16float4(v));
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
