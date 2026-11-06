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
Texture2D<float4> t1 : register(t1);
SamplerState _t1_sampler : register(s1);
Texture2D<float4> t2 : register(t2);
SamplerState _t2_sampler : register(s2);
globallycoherent RWTexture2D<uint> u1 : register(u5);
Texture2D<float4> t3 : register(t3);
SamplerState _t3_sampler : register(s3);
RWTexture2D<float4> u0 : register(u4);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

groupshared uint s_hud;
groupshared uint s_coarse;
groupshared uint s_changed;
groupshared uint s_structure;

bool differs(float3 a, float3 b, float tolerance)
{
    float3 scale = max(1.0f.xxx, abs(b));
    float3 _80 = abs(a - b);
    float3 _83 = scale * tolerance;
    return any(bool3(_80.x > _83.x, _80.y > _83.y, _80.z > _83.z));
}

int2 tiles()
{
    return int2((int(cb.Load(8)) + 7) / 8, (int(cb.Load(12)) + 7) / 8);
}

int2 flag_at(int2 t, uint set)
{
    return int2(t.x, (1 + (int(set) * tiles().y)) + t.y);
}

void comp_main()
{
    int2 p = int2(gl_GlobalInvocationID.xy);
    bool _104 = p.x < int(cb.Load(8));
    bool _113;
    if (_104)
    {
        _113 = p.y < int(cb.Load(12));
    }
    else
    {
        _113 = _104;
    }
    bool inside = _113;
    if (!inside)
    {
        return;
    }
    float4 back = t0.Load(int3(p, 0));
    float4 _137;
    if (cb.Load(52) == 3u)
    {
        _137 = back;
    }
    else
    {
        _137 = t1.Load(int3(p, 0));
    }
    float4 hudless = _137;
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
            bool _199 = cb.Load(48) != 0u;
            bool _209;
            if (_199)
            {
                _209 = u1[int2(1, 0)].xxxx.x <= cb.Load(44);
            }
            else
            {
                _209 = _199;
            }
            if (_209)
            {
                float4 _214;
                if (coarse)
                {
                    _214 = back;
                }
                else
                {
                    _214 = float4(back.xyz + (enhanced.xyz - hudless.xyz), back.w);
                }
                result = _214;
            }
            else
            {
                result = back;
            }
        }
        else
        {
            float4 _238;
            if (hud)
            {
                _238 = back;
            }
            else
            {
                _238 = float4(enhanced.xyz, back.w);
            }
            result = _238;
        }
    }
    else
    {
        float alpha;
        if (cb.Load(52) == 3u)
        {
            alpha = 0.0f;
            for (int y = -2; y <= 2; y++)
            {
                for (int x = -2; x <= 2; x++)
                {
                    int2 q = clamp(p + int2(x, y), int2(0, 0), int2(int(cb.Load(8)) - 1, int(cb.Load(12)) - 1));
                    alpha = max(alpha, t3.Load(int3(q, 0)).x);
                }
            }
            int2 param_6 = p / int2(8, 8);
            uint param_7 = 1u - cb.Load(28);
            bool _320 = (u1[flag_at(param_6, param_7)].xxxx.x & 64u) != 0u;
            bool _330;
            if (_320)
            {
                _330 = t3.Load(int3(p, 0)).z > 0.5f;
            }
            else
            {
                _330 = _320;
            }
            if (_330)
            {
                alpha = 1.0f;
            }
        }
        else
        {
            float4 ui = t3.Load(int3(p, 0));
            float _342;
            if (cb.Load(52) == 0u)
            {
                _342 = ui.x;
            }
            else
            {
                _342 = ui.w;
            }
            alpha = clamp(_342, 0.0f, 1.0f);
        }
        float4 _354;
        if (alpha >= 1.0f)
        {
            _354 = back;
        }
        else
        {
            _354 = float4(back.xyz + ((enhanced.xyz - hudless.xyz) * (1.0f - alpha)), back.w);
        }
        result = _354;
        if (cb.Load(24) != 0u)
        {
            result = float4(alpha.xxx, 1.0f);
        }
    }
    bool _391 = cb.Load(24) != 0u;
    bool _397;
    if (_391)
    {
        _397 = cb.Load(52) == 2u;
    }
    else
    {
        _397 = _391;
    }
    if (_397)
    {
        float3 param_8 = back.xyz;
        float3 param_9 = hudless.xyz;
        float param_10 = asfloat(cb.Load(56));
        result = differs(param_8, param_9, param_10) ? 1.0f.xxxx : float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    u0[p] = result;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
