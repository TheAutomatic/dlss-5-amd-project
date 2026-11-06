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
globallycoherent RWTexture2D<uint> u1 : register(u5);
RWTexture2D<float4> u0 : register(u4);
Texture2D<float4> t2 : register(t2);
SamplerState _t2_sampler : register(s2);
Texture2D<float4> t0 : register(t0);
SamplerState _t0_sampler : register(s0);
Texture2D<float4> t1 : register(t1);
SamplerState _t1_sampler : register(s1);
Texture2D<float4> t3 : register(t3);
SamplerState _t3_sampler : register(s3);

static uint3 gl_WorkGroupID;
static uint3 gl_GlobalInvocationID;
static uint gl_LocalInvocationIndex;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
    uint gl_LocalInvocationIndex : SV_GroupIndex;
};

groupshared uint s_changed;
groupshared uint s_structure;
groupshared uint s_hud;
groupshared uint s_coarse;

uint2 spvTextureSize(Texture2D<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

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
    if (gl_LocalInvocationIndex == 0u)
    {
        s_changed = 0u;
        s_structure = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    float moving = float(u1[int2(2, 0)].xxxx.x) / max(float(cb.Load(8)) * float(cb.Load(12)), 1.0f);
    bool _148 = !inside;
    bool _155;
    if (!_148)
    {
        _155 = cb.Load(32) != 0u;
    }
    else
    {
        _155 = _148;
    }
    float4 _156;
    if (_155)
    {
        _156 = 0.0f.xxxx;
    }
    else
    {
        _156 = u0[p];
    }
    float4 state = _156;
    bool use_mv = cb.Load(64) != 0u;
    float2 m = 0.0f.xx;
    if (inside && use_mv)
    {
        int2 base = int2(int(cb.Load(68) & 65535u), int(cb.Load(68) >> uint(16)));
        uint _220_dummy_parameter;
        int2 tq = clamp(base + int2((float2(p) + 0.5f.xx) * float2(asfloat(cb.Load(72)), asfloat(cb.Load(76)))), int2(0, 0), int2(spvTextureSize(t2, uint(0), _220_dummy_parameter)) - int2(1, 1));
        m = t2.Load(int3(tq, 0)).xy * float2(asfloat(cb.Load(36)), asfloat(cb.Load(40)));
    }
    bool _249;
    if (inside)
    {
        bool _248;
        if (!use_mv)
        {
            _248 = moving >= asfloat(cb.Load(60));
        }
        else
        {
            _248 = use_mv;
        }
        _249 = _248;
    }
    else
    {
        _249 = inside;
    }
    if (_249)
    {
        float3 c = t0.Load(int3(p, 0)).xyz;
        float3 param = c;
        float3 param_1 = t1.Load(int3(p, 0)).xyz;
        float param_2 = asfloat(cb.Load(56));
        bool same = !differs(param, param_1, param_2);
        bool structure = false;
        int2 hi = int2(int(cb.Load(8)) - 1, int(cb.Load(12)) - 1);
        int _300;
        int _311;
        for (int i = 0; i < 4; i++)
        {
            if (i == 0)
            {
                _300 = 1;
            }
            else
            {
                _300 = (i == 1) ? (-1) : 0;
            }
            if (i == 2)
            {
                _311 = 1;
            }
            else
            {
                _311 = (i == 3) ? (-1) : 0;
            }
            int2 q = clamp(p + int2(_300, _311), int2(0, 0), hi);
            float3 nq = t0.Load(int3(q, 0)).xyz;
            bool _356;
            if (!structure)
            {
                float3 param_3 = c;
                float3 param_4 = nq;
                float param_5 = 0.062745101749897003173828125f;
                bool _339 = differs(param_3, param_4, param_5);
                bool _355;
                if (_339)
                {
                    float3 param_6 = nq;
                    float3 param_7 = t1.Load(int3(q, 0)).xyz;
                    float param_8 = asfloat(cb.Load(56));
                    _355 = !differs(param_6, param_7, param_8);
                }
                else
                {
                    _355 = _339;
                }
                _356 = _355;
            }
            else
            {
                _356 = structure;
            }
            structure = _356;
        }
        bool _360 = !use_mv;
        bool _372;
        if (!_360)
        {
            bool _371;
            if (same)
            {
                _371 = dot(m, m) < 1.0f;
            }
            else
            {
                _371 = same;
            }
            _372 = _371;
        }
        else
        {
            _372 = _360;
        }
        if (_372)
        {
            if (moving >= asfloat(cb.Load(60)))
            {
                float _383;
                if (!same)
                {
                    _383 = state.y - 0.5f;
                }
                else
                {
                    float _391;
                    if (structure)
                    {
                        _391 = state.y + 0.02500000037252902984619140625f;
                    }
                    else
                    {
                        _391 = state.y - 0.0416666679084300994873046875f;
                    }
                    _383 = _391;
                }
                state.y = clamp(_383, 0.0f, 1.0f);
            }
        }
        else
        {
            if (!same)
            {
                state.y = max(state.y - 0.5f, 0.0f);
            }
            else
            {
                int2 from = clamp(int2(floor((float2(p) + 0.5f.xx) + m)), int2(0, 0), hi);
                float3 param_9 = c;
                float3 param_10 = t1.Load(int3(from, 0)).xyz;
                float param_11 = 0.0313725508749485015869140625f;
                bool left_behind = differs(param_9, param_10, param_11);
                bool bright = max(c.x, max(c.y, c.z)) >= 0.941176474094390869140625f;
                if (left_behind)
                {
                    float _458;
                    if (structure)
                    {
                        _458 = 0.02500000037252902984619140625f;
                    }
                    else
                    {
                        _458 = bright ? 0.0f : 0.01666666753590106964111328125f;
                    }
                    state.y = min(state.y + _458, 1.0f);
                }
            }
        }
        state.z = float(same);
        if (!same)
        {
            uint _476;
            InterlockedAdd(s_changed, 1u, _476);
        }
        bool _479 = same && structure;
        bool _486;
        if (_479)
        {
            _486 = state.y >= 0.75f;
        }
        else
        {
            _486 = _479;
        }
        if (_486)
        {
            uint _489;
            InterlockedAdd(s_structure, 1u, _489);
        }
    }
    if (inside)
    {
        state.x = smoothstep(0.5f, 1.0f, state.y);
        u0[p] = float4(state.x, state.y, state.z, 0.0f);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gl_LocalInvocationIndex == 0u)
    {
        int2 t = int2(gl_WorkGroupID.xy);
        int2 n = tiles();
        uint _521;
        if (cb.Load(32) != 0u)
        {
            _521 = 0u;
        }
        else
        {
            int2 param_12 = t;
            uint param_13 = cb.Load(28);
            _521 = u1[flag_at(param_12, param_13)].xxxx.x;
        }
        uint last = _521;
        uint flags = last;
        bool _541 = moving >= asfloat(cb.Load(60));
        bool _548;
        if (!_541)
        {
            _548 = cb.Load(32) != 0u;
        }
        else
        {
            _548 = _541;
        }
        if (_548)
        {
            bool still = s_changed == 0u;
            uint _556;
            if (still)
            {
                _556 = min((((last >> uint(2)) & 15u) + 1u), 15u);
            }
            else
            {
                _556 = 0u;
            }
            uint count = _556;
            uint hud_beside = 0u;
            int _580;
            int _590;
            for (int i_1 = 0; i_1 < 4; i_1++)
            {
                if (i_1 == 0)
                {
                    _580 = 1;
                }
                else
                {
                    _580 = (i_1 == 1) ? (-1) : 0;
                }
                if (i_1 == 2)
                {
                    _590 = 1;
                }
                else
                {
                    _590 = (i_1 == 3) ? (-1) : 0;
                }
                int2 q_1 = t + int2(_580, _590);
                bool _602 = cb.Load(32) == 0u;
                bool _608;
                if (_602)
                {
                    _608 = q_1.x >= 0;
                }
                else
                {
                    _608 = _602;
                }
                bool _614;
                if (_608)
                {
                    _614 = q_1.y >= 0;
                }
                else
                {
                    _614 = _608;
                }
                bool _622;
                if (_614)
                {
                    _622 = q_1.x < n.x;
                }
                else
                {
                    _622 = _614;
                }
                bool _630;
                if (_622)
                {
                    _630 = q_1.y < n.y;
                }
                else
                {
                    _630 = _622;
                }
                bool _645;
                if (_630)
                {
                    int2 param_14 = q_1;
                    uint param_15 = cb.Load(28);
                    _645 = (u1[flag_at(param_14, param_15)].xxxx.x & 65u) != 0u;
                }
                else
                {
                    _645 = _630;
                }
                if (_645)
                {
                    hud_beside++;
                }
            }
            bool structure_1 = s_structure >= 2u;
            bool _660 = still && (count >= 12u);
            bool _667;
            if (_660)
            {
                _667 = structure_1 || (hud_beside >= 2u);
            }
            else
            {
                _667 = _660;
            }
            bool inside_1 = _667;
            flags = (uint(structure_1) | (count << uint(2))) | (inside_1 ? 64u : 0u);
        }
        int2 param_16 = t;
        uint param_17 = 1u - cb.Load(28);
        u1[flag_at(param_16, param_17)] = flags.x;
    }
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    gl_LocalInvocationIndex = stage_input.gl_LocalInvocationIndex;
    comp_main();
}
