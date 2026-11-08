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
Texture2D<float4> t3 : register(t3);
SamplerState _t3_sampler : register(s3);
Texture2D<float4> t0 : register(t0);
SamplerState _t0_sampler : register(s0);
RWTexture2D<float4> u1 : register(u5);
RWTexture2D<float4> u0 : register(u4);
Texture2D<float4> t1 : register(t1);
SamplerState _t1_sampler : register(s1);
Texture2D<float4> t2 : register(t2);
SamplerState _t2_sampler : register(s2);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

bool outside(uint2 p)
{
    bool _487 = p.x >= cb.Load(8);
    bool _497;
    if (!_487)
    {
        _497 = p.y >= cb.Load(12);
    }
    else
    {
        _497 = _487;
    }
    return _497;
}

bool finite1(float v)
{
    bool _84 = isnan(v);
    bool _90;
    if (!_84)
    {
        _90 = isinf(v);
    }
    else
    {
        _90 = _84;
    }
    return !_90;
}

float saturate1(float x)
{
    return clamp(x, 0.0f, 1.0f);
}

float lerp1(float a, float b, float t)
{
    return a + (t * (b - a));
}

float sanitize1(float v, float f)
{
    float param = v;
    return finite1(param) ? v : f;
}

float white_point()
{
    float wp = asfloat(cb.Load(4));
    float base = max(wp, 9.9999997473787516355514526367188e-05f);
    uint em = cb.Load(144);
    if ((em == 1u) || (em == 3u))
    {
        float measured = t3.Load(int3(int2(0, 0), 0)).x;
        float param = measured;
        if (finite1(param) && (measured > 9.9999999392252902907785028219223e-09f))
        {
            float _371;
            if (em == 1u)
            {
                _371 = asfloat(cb.Load(148)) / measured;
            }
            else
            {
                _371 = measured;
            }
            base = _371;
            float trim = asfloat(cb.Load(152));
            uint n = min(cb.Load(160), 8u);
            if (n > 0u)
            {
                trim = asfloat(cb.Load(180));
                for (uint i = 1u; i < 8u; i++)
                {
                    bool _409 = i < n;
                    bool _419;
                    if (_409)
                    {
                        _419 = base > asfloat(cb.Load((2u * (i - 1u)) * 4 + 176));
                    }
                    else
                    {
                        _419 = _409;
                    }
                    if (_419)
                    {
                        float p0 = asfloat(cb.Load((2u * (i - 1u)) * 4 + 176));
                        float p1 = asfloat(cb.Load((2u * i) * 4 + 176));
                        float r0 = asfloat(cb.Load(((2u * (i - 1u)) + 1u) * 4 + 176));
                        float r1 = asfloat(cb.Load(((2u * i) + 1u) * 4 + 176));
                        float param_1 = log2(max(base, 9.9999999392252902907785028219223e-09f) / p0) / log2(p1 / p0);
                        float t = saturate1(param_1);
                        float param_2 = log2(r0);
                        float param_3 = log2(r1);
                        float param_4 = t;
                        trim = exp2(lerp1(param_2, param_3, param_4));
                    }
                }
            }
            base *= trim;
        }
    }
    float param_5 = base;
    float param_6 = wp;
    return max(sanitize1(param_5, param_6), 9.9999997473787516355514526367188e-05f);
}

float luma(float3 c)
{
    return dot(c, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
}

float maxch(float3 c)
{
    return max(c.x, max(c.y, c.z));
}

float3 knee(inout float3 d)
{
    if (cb.Load(32) != 0u)
    {
        return d;
    }
    float3 param = d;
    float L = luma(param);
    if (L > 0.75f)
    {
        float rolled = 0.75f + (0.25f * (1.0f - exp((-(L - 0.75f)) / 0.25f)));
        d *= (rolled / L);
    }
    float3 param_1 = d;
    float peak = maxch(param_1);
    if (peak > 1.0f)
    {
        d /= peak.xxx;
    }
    return d;
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

float3 neutwo_encode(inout float3 v)
{
    v = max(v, 0.0f.xxx);
    float3 param = v;
    float m = maxch(param);
    if (m <= 9.9999999747524270787835121154785e-07f)
    {
        return v;
    }
    return v * rsqrt((m * m) + 1.0f);
}

float3 encode_curve(float3 n)
{
    uint rm = cb.Load(76);
    float3 _304;
    if (rm == 0u)
    {
        float3 param = n;
        float3 _309 = knee(param);
        _304 = _309;
    }
    else
    {
        float3 _314;
        if (rm >= 3u)
        {
            float3 param_1 = n;
            float3 _319 = hybrid_encode(param_1);
            _314 = _319;
        }
        else
        {
            float3 param_2 = n;
            float3 _323 = neutwo_encode(param_2);
            _314 = _323;
        }
        _304 = _314;
    }
    return _304;
}

float3 saturate3(float3 x)
{
    return clamp(x, 0.0f.xxx, 1.0f.xxx);
}

float3 lerp3v(float3 a, float3 b, float3 t)
{
    return a + (t * (b - a));
}

float3 srgb_encode(inout float3 v)
{
    float3 param = v;
    v = saturate3(param);
    float3 a = v * 12.9200000762939453125f;
    float3 b = (pow(max(v, 9.9999999392252902907785028219223e-09f.xxx), 0.4166666567325592041015625f.xxx) * 1.05499994754791259765625f) - 0.054999999701976776123046875f.xxx;
    float3 param_1 = a;
    float3 param_2 = b;
    float3 param_3 = step(0.003130800090730190277099609375f.xxx, v);
    return lerp3v(param_1, param_2, param_3);
}

void comp_main()
{
    uint2 p = gl_GlobalInvocationID.xy;
    uint2 param = p;
    if (outside(param))
    {
        return;
    }
    float4 S = t0.Load(int3(int2(p), 0));
    float3 F = max(S.xyz, 0.0f.xxx);
    u1[int2(p)] = float4(F, S.w);
    if (cb.Load(32) != 0u)
    {
        u0[int2(p)] = float4(F, S.w);
        return;
    }
    float3 n = F / white_point().xxx;
    float3 param_1 = n;
    float3 d = encode_curve(param_1);
    float _567;
    if (cb.Load(76) != 0u)
    {
        _567 = 1.0f;
    }
    else
    {
        _567 = S.w;
    }
    float alpha = _567;
    float3 param_2 = d;
    float3 _579 = srgb_encode(param_2);
    u0[int2(p)] = float4(_579, alpha);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
