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
Texture2D<float4> t1 : register(t1);
SamplerState _t1_sampler : register(s1);
Texture2D<float4> t2 : register(t2);
SamplerState _t2_sampler : register(s2);
RWTexture2D<float4> u0 : register(u4);
RWTexture2D<float4> u1 : register(u5);
Texture2D<float4> t0 : register(t0);
SamplerState _t0_sampler : register(s0);
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
    float _269;
    if (pos)
    {
        _269 = asfloat(sp.Load(a * 4 + 112));
    }
    else
    {
        _269 = asfloat(sp.Load(a * 4 + 96));
    }
    float span = _269;
    float _282;
    if (pos)
    {
        _282 = asfloat(sp.Load((2 + a) * 4 + 112));
    }
    else
    {
        _282 = asfloat(sp.Load((2 + a) * 4 + 96));
    }
    float c = _282;
    float _297;
    if (pos)
    {
        _297 = asfloat(sp.Load(a * 4 + 144));
    }
    else
    {
        _297 = asfloat(sp.Load(a * 4 + 128));
    }
    float w = _297;
    float _310;
    if (pos)
    {
        _310 = asfloat(sp.Load((2 + a) * 4 + 144));
    }
    else
    {
        _310 = asfloat(sp.Load((2 + a) * 4 + 128));
    }
    float k = _310;
    float _325;
    if (pos)
    {
        _325 = asfloat(sp.Load((2 + a) * 4 + 160));
    }
    else
    {
        _325 = asfloat(sp.Load(a * 4 + 160));
    }
    float e = _325;
    float param = abs(d) / (asfloat(sp.Load(a * 4 + 176)) * span);
    float param_1 = c;
    float param_2 = w;
    float param_3 = k;
    float param_4 = e;
    float _355 = unpack_radius(param, param_1, param_2, param_3, param_4);
    float radius = _355;
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
    float _154;
    if (pos)
    {
        _154 = asfloat(sp.Load(a * 4 + 112));
    }
    else
    {
        _154 = asfloat(sp.Load(a * 4 + 96));
    }
    float span = _154;
    float _176;
    if (pos)
    {
        _176 = asfloat(sp.Load((2 + a) * 4 + 112));
    }
    else
    {
        _176 = asfloat(sp.Load((2 + a) * 4 + 96));
    }
    float c = _176;
    float _192;
    if (pos)
    {
        _192 = asfloat(sp.Load(a * 4 + 144));
    }
    else
    {
        _192 = asfloat(sp.Load(a * 4 + 128));
    }
    float w = _192;
    float _207;
    if (pos)
    {
        _207 = asfloat(sp.Load((2 + a) * 4 + 144));
    }
    else
    {
        _207 = asfloat(sp.Load((2 + a) * 4 + 128));
    }
    float k = _207;
    float _222;
    if (pos)
    {
        _222 = asfloat(sp.Load((2 + a) * 4 + 160));
    }
    else
    {
        _222 = asfloat(sp.Load(a * 4 + 160));
    }
    float e = _222;
    float param = abs(d) / span;
    float param_1 = c;
    float param_2 = w;
    float param_3 = k;
    float param_4 = e;
    float _249 = pack_radius(param, param_1, param_2, param_3, param_4);
    float radius = _249;
    return (((pos ? 1.0f : (-1.0f)) * asfloat(sp.Load(a * 4 + 176))) * span) * radius;
}

float2 pack_offset(float2 d)
{
    float param = d.x;
    int param_1 = 0;
    float param_2 = d.y;
    int param_3 = 1;
    return float2(pack_off(param, param_1), pack_off(param_2, param_3));
}

void comp_main()
{
    uint2 p = gl_GlobalInvocationID.xy;
    bool _417 = p.x >= sp.Load(8);
    bool _427;
    if (!_417)
    {
        _427 = p.y >= sp.Load(12);
    }
    else
    {
        _427 = _417;
    }
    if (_427)
    {
        return;
    }
    float2 pixel = float2(float(p.x) + 0.5f, float(p.y) + 0.5f);
    float2 native = asfloat(sp.Load4(16)).xy;
    if (sp.Load(0) != 101u)
    {
        return;
    }
    float2 param = pixel;
    float2 off = unpack_offset(param);
    float2 nuv = (off + asfloat(sp.Load4(80)).xy) / native;
    int2 dp = clamp(int2((asfloat(sp.Load4(192)).zw * nuv) + asfloat(sp.Load4(192)).xy), int2(asfloat(sp.Load4(192)).xy), int2(asfloat(sp.Load4(192)).zw + asfloat(sp.Load4(192)).xy) - int2(1, 1));
    int2 mp = clamp(int2((asfloat(sp.Load4(208)).zw * nuv) + asfloat(sp.Load4(208)).xy), int2(asfloat(sp.Load4(208)).xy), int2(asfloat(sp.Load4(208)).zw + asfloat(sp.Load4(208)).xy) - int2(1, 1));
    float depth = t1.Load(int3(dp, 0)).x;
    float2 mv = t2.Load(int3(mp, 0)).xy * asfloat(sp.Load2(224));
    u0[int2(p)] = depth.xxxx;
    float2 param_1 = off + mv;
    float2 param_2 = off;
    float2 _packed = pack_offset(param_1) - pack_offset(param_2);
    u1[int2(p)] = float4(_packed, _packed.x, _packed.x);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
