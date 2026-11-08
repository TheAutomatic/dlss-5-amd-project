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
RWTexture2D<float4> u0 : register(u4);
Texture2D<float4> t1 : register(t1);
SamplerState _t1_sampler : register(s1);
Texture2D<float4> t2 : register(t2);
SamplerState _t2_sampler : register(s2);
RWTexture2D<float4> u1 : register(u5);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

bool outside(uint2 p)
{
    bool _307 = p.x >= cb.Load(8);
    bool _317;
    if (!_307)
    {
        _317 = p.y >= cb.Load(12);
    }
    else
    {
        _317 = _307;
    }
    return _317;
}

float3 saturate3(float3 x)
{
    return clamp(x, 0.0f.xxx, 1.0f.xxx);
}

float3 lerp3v(float3 a, float3 b, float3 t)
{
    return a + (t * (b - a));
}

float3 srgb_decode(inout float3 v)
{
    float3 param = v;
    v = saturate3(param);
    float3 a = v / 12.9200000762939453125f.xxx;
    float3 b = pow((v + 0.054999999701976776123046875f.xxx) / 1.05499994754791259765625f.xxx, 2.400000095367431640625f.xxx);
    float3 param_1 = a;
    float3 param_2 = b;
    float3 param_3 = step(0.040449999272823333740234375f.xxx, v);
    return lerp3v(param_1, param_2, param_3);
}

bool finite1(float v)
{
    bool _54 = isnan(v);
    bool _60;
    if (!_54)
    {
        _60 = isinf(v);
    }
    else
    {
        _60 = _54;
    }
    return !_60;
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
            float _190;
            if (em == 1u)
            {
                _190 = asfloat(cb.Load(148)) / measured;
            }
            else
            {
                _190 = measured;
            }
            base = _190;
            float trim = asfloat(cb.Load(152));
            uint n = min(cb.Load(160), 8u);
            if (n > 0u)
            {
                trim = asfloat(cb.Load(180));
                for (uint i = 1u; i < 8u; i++)
                {
                    bool _228 = i < n;
                    bool _239;
                    if (_228)
                    {
                        _239 = base > asfloat(cb.Load((2u * (i - 1u)) * 4 + 176));
                    }
                    else
                    {
                        _239 = _228;
                    }
                    if (_239)
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
    float3 _349;
    if (cb.Load(32) != 0u)
    {
        float3 _356;
        if (cb.Load(240) != 0u)
        {
            float3 param_1 = F;
            float3 _361 = srgb_decode(param_1);
            _356 = _361;
        }
        else
        {
            _356 = F;
        }
        _349 = _356;
    }
    else
    {
        _349 = F / white_point().xxx;
    }
    float3 L = _349;
    u0[int2(p)] = float4(L, S.w);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
