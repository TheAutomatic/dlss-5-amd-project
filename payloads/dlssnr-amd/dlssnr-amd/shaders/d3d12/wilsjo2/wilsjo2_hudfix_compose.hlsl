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
globallycoherent RWTexture2D<uint> u1 : register(u5);
Texture2D<float4> t0 : register(t0);
SamplerState _t0_sampler : register(s0);
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

groupshared uint s_hud;
groupshared uint s_coarse;

bool differs(float3 a, float3 b, float tolerance)
{
    float3 scale = max(1.0f.xxx, abs(b));
    float3 _26 = abs(a - b);
    float3 _29 = scale * tolerance;
    return any(bool3(_26.x > _29.x, _26.y > _29.y, _26.z > _29.z));
}

void comp_main()
{
    int2 p = int2(gl_GlobalInvocationID.xy);
    bool _63 = p.x < int(cb.Load(8));
    bool _74;
    if (_63)
    {
        _74 = p.y < int(cb.Load(12));
    }
    else
    {
        _74 = _63;
    }
    bool inside = _74;
    if (!inside)
    {
        return;
    }
    if (cb.Load(52) == 5u)
    {
        float4 _107;
        if (u1[int2(0, 0)].xxxx.x > cb.Load(44))
        {
            _107 = t0.Load(int3(p, 0));
        }
        else
        {
            _107 = t1.Load(int3(p, 0));
        }
        u0[p] = _107;
        return;
    }
    float4 back = t0.Load(int3(p, 0));
    float4 hudless = t1.Load(int3(p, 0));
    float4 enhanced = t2.Load(int3(p, 0));
    float4 result;
    if (cb.Load(52) == 2u)
    {
        float3 param = back.xyz;
        float3 param_1 = hudless.xyz;
        float param_2 = asfloat(cb.Load(56));
        bool hud = differs(param, param_1, param_2);
        float3 param_3 = back.xyz;
        float3 param_4 = hudless.xyz;
        float param_5 = 0.0625f;
        bool coarse = differs(param_3, param_4, param_5);
        if (u1[int2(0, 0)].xxxx.x > cb.Load(44))
        {
            if (cb.Load(48) == 2u)
            {
                result = float4(enhanced.xyz, back.w);
            }
            else
            {
                bool _197 = cb.Load(48) != 0u;
                bool _208;
                if (_197)
                {
                    _208 = u1[int2(1, 0)].xxxx.x <= cb.Load(44);
                }
                else
                {
                    _208 = _197;
                }
                if (_208)
                {
                    float4 _212;
                    if (coarse)
                    {
                        _212 = back;
                    }
                    else
                    {
                        _212 = float4(back.xyz + (enhanced.xyz - hudless.xyz), back.w);
                    }
                    result = _212;
                }
                else
                {
                    result = back;
                }
            }
        }
        else
        {
            float4 _236;
            if (hud)
            {
                _236 = back;
            }
            else
            {
                _236 = float4(enhanced.xyz, back.w);
            }
            result = _236;
        }
    }
    else
    {
        float4 ui = t3.Load(int3(p, 0));
        float _261;
        if (cb.Load(52) == 0u)
        {
            _261 = ui.x;
        }
        else
        {
            _261 = ui.w;
        }
        float alpha = clamp(_261, 0.0f, 1.0f);
        float4 _274;
        if (alpha >= 1.0f)
        {
            _274 = back;
        }
        else
        {
            _274 = float4(back.xyz + ((enhanced.xyz - hudless.xyz) * (1.0f - alpha)), back.w);
        }
        result = _274;
        if (cb.Load(24) != 0u)
        {
            result = float4(alpha.xxx, 1.0f);
        }
    }
    bool _311 = cb.Load(24) != 0u;
    bool _317;
    if (_311)
    {
        _317 = cb.Load(52) == 2u;
    }
    else
    {
        _317 = _311;
    }
    if (_317)
    {
        float3 param_6 = back.xyz;
        float3 param_7 = hudless.xyz;
        float param_8 = asfloat(cb.Load(56));
        result = differs(param_6, param_7, param_8) ? 1.0f.xxxx : float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    u0[p] = result;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
