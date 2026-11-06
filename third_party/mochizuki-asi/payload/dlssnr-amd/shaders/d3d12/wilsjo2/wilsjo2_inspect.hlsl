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
RWTexture2D<float4> u0 : register(u4);
Texture2D<float4> t0 : register(t0);
SamplerState _t0_sampler : register(s0);
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

void comp_main()
{
    int2 p = int2(gl_GlobalInvocationID.xy);
    bool _36 = p.x >= int(cb.Load(8));
    bool _48;
    if (!_36)
    {
        _48 = p.y >= int(cb.Load(12));
    }
    else
    {
        _48 = _36;
    }
    if (_48)
    {
        return;
    }
    if (cb.Load(80) == 0u)
    {
        u0[p] = t0.Load(int3(p, 0));
        return;
    }
    float2 uv = (float2(p) + 0.5f.xx) / float2(float(cb.Load(8)), float(cb.Load(12)));
    bool off_side = false;
    bool divider = false;
    bool outside = false;
    float2 at = uv;
    if (cb.Load(52) == 1u)
    {
        off_side = (uv.x < 0.5f) != (cb.Load(64) != 0u);
        float _116;
        if (uv.x < 0.5f)
        {
            _116 = uv.x * 2.0f;
        }
        else
        {
            _116 = (uv.x * 2.0f) - 1.0f;
        }
        float2 c = float2(_116, uv.y) - 0.5f.xx;
        at = float2(0.5f + (c.x / asfloat(cb.Load(60))), 0.5f + ((c.y * 2.0f) / asfloat(cb.Load(60))));
        bool _154 = at.x < 0.0f;
        bool _161;
        if (!_154)
        {
            _161 = at.x > 1.0f;
        }
        else
        {
            _161 = _154;
        }
        bool _168;
        if (!_161)
        {
            _168 = at.y < 0.0f;
        }
        else
        {
            _168 = _161;
        }
        bool _175;
        if (!_168)
        {
            _175 = at.y > 1.0f;
        }
        else
        {
            _175 = _168;
        }
        outside = _175;
        divider = abs(uv.x - 0.5f) < (1.0f / float(cb.Load(8)));
    }
    else
    {
        if (cb.Load(52) == 2u)
        {
            off_side = (uv.x < asfloat(cb.Load(56))) != (cb.Load(64) != 0u);
            bool _204 = asfloat(cb.Load(56)) > 0.0f;
            bool _210;
            if (_204)
            {
                _210 = asfloat(cb.Load(56)) < 1.0f;
            }
            else
            {
                _210 = _204;
            }
            bool _224;
            if (_210)
            {
                _224 = abs(uv.x - asfloat(cb.Load(56))) < (1.0f / float(cb.Load(8)));
            }
            else
            {
                _224 = _210;
            }
            divider = _224;
        }
    }
    float4 v;
    if (divider)
    {
        v = 1.0f.xxxx;
    }
    else
    {
        if (outside)
        {
            v = float4(0.0f, 0.0f, 0.0f, 1.0f);
        }
        else
        {
            if (cb.Load(52) == 1u)
            {
                float4 _243;
                if (off_side)
                {
                    _243 = t0.SampleLevel(_t0_sampler, at, 0.0f);
                }
                else
                {
                    _243 = t1.SampleLevel(_t1_sampler, at, 0.0f);
                }
                v = _243;
            }
            else
            {
                float4 _257;
                if (off_side)
                {
                    _257 = t0.Load(int3(p, 0));
                }
                else
                {
                    _257 = t1.Load(int3(p, 0));
                }
                v = _257;
            }
        }
    }
    u0[p] = v;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
