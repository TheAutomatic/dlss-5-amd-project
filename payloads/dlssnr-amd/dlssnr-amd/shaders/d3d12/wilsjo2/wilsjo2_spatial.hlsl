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
ByteAddressBuffer sp : register(t6);
Texture2D<float4> t0 : register(t0);
SamplerState _t0_sampler : register(s0);
RWTexture2D<float4> u0 : register(u4);
RWTexture2D<float4> u1 : register(u5);
Texture2D<float4> t1 : register(t1);
SamplerState _t1_sampler : register(s1);
Texture2D<float4> t2 : register(t2);
SamplerState _t2_sampler : register(s2);
Texture2D<float4> t3 : register(t3);
SamplerState _t3_sampler : register(s3);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

float unpack_radius(inout float r, float c, float w, float k, float e)
{
    r = abs(r);
    if (((w >= 1.0f) || (c >= 1.0f)) || (r <= c))
    {
        return r;
    }
    if (r > w)
    {
        return 1.0f + ((r - w) / e);
    }
    float y = (r - c) / (w - c);
    float t = (k * y) / (1.0f - ((1.0f - k) * y));
    return c + ((1.0f - c) * t);
}

float unpack_off(float d, int a)
{
    bool pos = d >= 0.0f;
    float _274;
    if (pos)
    {
        _274 = asfloat(sp.Load(a * 4 + 112));
    }
    else
    {
        _274 = asfloat(sp.Load(a * 4 + 96));
    }
    float span = _274;
    float _287;
    if (pos)
    {
        _287 = asfloat(sp.Load((2 + a) * 4 + 112));
    }
    else
    {
        _287 = asfloat(sp.Load((2 + a) * 4 + 96));
    }
    float c = _287;
    float _302;
    if (pos)
    {
        _302 = asfloat(sp.Load(a * 4 + 144));
    }
    else
    {
        _302 = asfloat(sp.Load(a * 4 + 128));
    }
    float w = _302;
    float _315;
    if (pos)
    {
        _315 = asfloat(sp.Load((2 + a) * 4 + 144));
    }
    else
    {
        _315 = asfloat(sp.Load((2 + a) * 4 + 128));
    }
    float k = _315;
    float _330;
    if (pos)
    {
        _330 = asfloat(sp.Load((2 + a) * 4 + 160));
    }
    else
    {
        _330 = asfloat(sp.Load(a * 4 + 160));
    }
    float e = _330;
    float param = abs(d) / (asfloat(sp.Load(a * 4 + 176)) * span);
    float param_1 = c;
    float param_2 = w;
    float param_3 = k;
    float param_4 = e;
    float _360 = unpack_radius(param, param_1, param_2, param_3, param_4);
    float radius = _360;
    return (span * (pos ? 1.0f : (-1.0f))) * radius;
}

float2 unpack_offset(float2 u)
{
    float param = u.x - asfloat(sp.Load(88));
    int param_1 = 0;
    float param_2 = u.y - asfloat(sp.Load(92));
    int param_3 = 1;
    return float2(unpack_off(param, param_1), unpack_off(param_2, param_3));
}

float saturate1(float x)
{
    return clamp(x, 0.0f, 1.0f);
}

float pack_radius(inout float r, float c, float w, float k, float e)
{
    r = abs(r);
    if (((w >= 1.0f) || (c >= 1.0f)) || (r <= c))
    {
        return r;
    }
    if (r > 1.0f)
    {
        return w + ((r - 1.0f) * e);
    }
    float t = (r - c) / (1.0f - c);
    return c + (((w - c) * t) / (k + ((1.0f - k) * t)));
}

float pack_off(float d, int a)
{
    bool pos = d >= 0.0f;
    float _159;
    if (pos)
    {
        _159 = asfloat(sp.Load(a * 4 + 112));
    }
    else
    {
        _159 = asfloat(sp.Load(a * 4 + 96));
    }
    float span = _159;
    float _181;
    if (pos)
    {
        _181 = asfloat(sp.Load((2 + a) * 4 + 112));
    }
    else
    {
        _181 = asfloat(sp.Load((2 + a) * 4 + 96));
    }
    float c = _181;
    float _197;
    if (pos)
    {
        _197 = asfloat(sp.Load(a * 4 + 144));
    }
    else
    {
        _197 = asfloat(sp.Load(a * 4 + 128));
    }
    float w = _197;
    float _212;
    if (pos)
    {
        _212 = asfloat(sp.Load((2 + a) * 4 + 144));
    }
    else
    {
        _212 = asfloat(sp.Load((2 + a) * 4 + 128));
    }
    float k = _212;
    float _227;
    if (pos)
    {
        _227 = asfloat(sp.Load((2 + a) * 4 + 160));
    }
    else
    {
        _227 = asfloat(sp.Load(a * 4 + 160));
    }
    float e = _227;
    float param = abs(d) / span;
    float param_1 = c;
    float param_2 = w;
    float param_3 = k;
    float param_4 = e;
    float _254 = pack_radius(param, param_1, param_2, param_3, param_4);
    float radius = _254;
    return (((pos ? 1.0f : (-1.0f)) * asfloat(sp.Load(a * 4 + 176))) * span) * radius;
}

void comp_main()
{
    uint2 p = gl_GlobalInvocationID.xy;
    bool _409 = p.x >= sp.Load(8);
    bool _419;
    if (!_409)
    {
        _419 = p.y >= sp.Load(12);
    }
    else
    {
        _419 = _409;
    }
    if (_419)
    {
        return;
    }
    float2 pixel = float2(float(p.x) + 0.5f, float(p.y) + 0.5f);
    float2 native = asfloat(sp.Load4(16)).xy;
    if (sp.Load(0) == 100u)
    {
        float2 param = pixel;
        float2 n = unpack_offset(param) + asfloat(sp.Load4(80)).xy;
        float2 nuv = n / native;
        float2 param_1 = pixel + 0.5f.xx;
        float2 param_2 = pixel - 0.5f.xx;
        float2 fp = max(abs(unpack_offset(param_1) - unpack_offset(param_2)), 1.0f.xx);
        float param_3 = (max(fp.x, fp.y) - 1.0f) * 3.3333332538604736328125f;
        float spread = 0.375f * saturate1(param_3);
        float4 c;
        if (spread > 0.0f)
        {
            float2 d = fp * spread;
            float2 lo = (n - d) / native;
            float2 hi = (d + n) / native;
            c = t0.SampleLevel(_t0_sampler, nuv, 0.0f) + t0.SampleLevel(_t0_sampler, lo, 0.0f);
            c += t0.SampleLevel(_t0_sampler, float2(hi.x, lo.y), 0.0f);
            c += t0.SampleLevel(_t0_sampler, float2(lo.x, hi.y), 0.0f);
            c += t0.SampleLevel(_t0_sampler, hi, 0.0f);
            c *= 0.20000000298023223876953125f;
        }
        else
        {
            c = t0.SampleLevel(_t0_sampler, nuv, 0.0f);
        }
        u0[int2(p)] = c;
    }
    else
    {
        if (sp.Load(0) == 102u)
        {
            float2 n_1 = (native * pixel) / float2(float(sp.Load(8)), float(sp.Load(12)));
            float param_4 = n_1.x - asfloat(sp.Load(80));
            int param_5 = 0;
            float param_6 = n_1.y - asfloat(sp.Load(84));
            int param_7 = 1;
            float2 puv = (float2(pack_off(param_4, param_5), pack_off(param_6, param_7)) + asfloat(sp.Load4(80)).zw) / asfloat(sp.Load4(16)).zw;
            u0[int2(p)] = t0.SampleLevel(_t0_sampler, puv, 0.0f);
            u1[int2(p)] = t1.SampleLevel(_t1_sampler, puv, 0.0f);
        }
    }
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
