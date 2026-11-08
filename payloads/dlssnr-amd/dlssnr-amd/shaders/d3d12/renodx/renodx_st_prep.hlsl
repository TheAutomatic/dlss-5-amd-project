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
static const float _280[5] = { 1.0f, 4.0f, 6.0f, 4.0f, 1.0f };

cbuffer Push
{
    uint2 pc_size : packoffset(c0);
    uint2 pc_low : packoffset(c0.z);
    uint pc_src : packoffset(c1);
};

Texture2D<float4> N : register(t1);
Texture2D<float4> P : register(t0);
RWTexture2D<float2> Fine : register(u4);
RWTexture2D<float4> OutA : register(u5);
RWTexture2D<float4> OutB : register(u6);

static uint3 gl_WorkGroupID;
static uint3 gl_LocalInvocationID;
static uint gl_LocalInvocationIndex;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
    uint gl_LocalInvocationIndex : SV_GroupIndex;
};

groupshared float s_dl[20][20];
groupshared float s_ln[20][20];
groupshared float s_lp[20][20];
groupshared float3 s_v[16][16];

float3 st_dec(float3 c)
{
    float3 _19 = c / 12.9200000762939453125f.xxx;
    float3 _29 = pow((c + 0.054999999701976776123046875f.xxx) / 1.05499994754791259765625f.xxx, 2.400000095367431640625f.xxx);
    bool3 _35 = bool3(c.x > 0.040449999272823333740234375f.xxx.x, c.y > 0.040449999272823333740234375f.xxx.y, c.z > 0.040449999272823333740234375f.xxx.z);
    return float3(_35.x ? _29.x : _19.x, _35.y ? _29.y : _19.y, _35.z ? _29.z : _19.z);
}

float3 st_lab(float3 s)
{
    float3 param = max(s, 0.0f.xxx);
    float3 l = st_dec(param);
    float3 lms = float3(dot(l, float3(0.4122214615345001220703125f, 0.536332547664642333984375f, 0.0514459945261478424072265625f)), dot(l, float3(0.21190349757671356201171875f, 0.680699527263641357421875f, 0.10739696025848388671875f)), dot(l, float3(0.0883024632930755615234375f, 0.28171885013580322265625f, 0.629978716373443603515625f)));
    float3 q = sign(lms) * pow(abs(lms), 0.3333333432674407958984375f.xxx);
    return float3(dot(q, float3(0.2104542553424835205078125f, 0.793617784976959228515625f, -0.004072046838700771331787109375f)), dot(q, float3(1.9779984951019287109375f, -2.428592205047607421875f, 0.4505937099456787109375f)), dot(q, float3(0.025904037058353424072265625f, 0.782771766185760498046875f, -0.8086757659912109375f)));
}

void comp_main()
{
    int2 org = int2(gl_WorkGroupID.xy) * int2(16, 16);
    int2 lid = int2(gl_LocalInvocationID.xy);
    for (uint i = gl_LocalInvocationIndex; i < 400u; i += 256u)
    {
        int2 t = clamp((org - int2(2, 2)) + int2(int(i % 20u), int(i / 20u)), int2(0, 0), int2(pc_size) - int2(1, 1));
        float3 param = N.Load(int3(t, 0)).xyz;
        float ln = st_lab(param).x;
        float3 param_1 = P.Load(int3(t, 0)).xyz;
        float lp = st_lab(param_1).x;
        s_dl[i / 20u][i % 20u] = ln - lp;
        s_ln[i / 20u][i % 20u] = ln;
        s_lp[i / 20u][i % 20u] = lp;
    }
    int2 px = org + lid;
    int2 _218 = int2(pc_size);
    bool inside = all(bool2(px.x < _218.x, px.y < _218.y));
    if ((pc_src != 0u) && inside)
    {
        s_v[lid.y][lid.x] = N.Load(int3(px, 0)).xyz - P.Load(int3(px, 0)).xyz;
    }
    GroupMemoryBarrierWithGroupSync();
    if (inside)
    {
        float s = 0.0f;
        for (int y = -2; y <= 2; y++)
        {
            for (int x = -2; x <= 2; x++)
            {
                s += ((_280[x + 2] * _280[y + 2]) * s_dl[(lid.y + 2) + y][(lid.x + 2) + x]);
            }
        }
        Fine[px] = float4(s_dl[lid.y + 2][lid.x + 2] - (s / 256.0f), s_ln[lid.y + 2][lid.x + 2], 0.0f, 0.0f).xy;
    }
    bool _341 = lid.x < 4;
    bool _347;
    if (_341)
    {
        _347 = lid.y < 4;
    }
    else
    {
        _347 = _341;
    }
    if (_347)
    {
        uint2 q = (gl_WorkGroupID.xy * uint2(4u, 4u)) + uint2(lid);
        if (all(bool2(q.x < pc_low.x, q.y < pc_low.y)))
        {
            float4 A = 0.0f.xxxx;
            float4 B = 0.0f.xxxx;
            float c = 0.0f;
            float3 _422;
            for (uint y_1 = 0u; y_1 < 4u; y_1++)
            {
                for (uint x_1 = 0u; x_1 < 4u; x_1++)
                {
                    uint2 p = (q * uint2(4u, 4u)) + uint2(x_1, y_1);
                    if (any(bool2(p.x >= pc_size.x, p.y >= pc_size.y)))
                    {
                        continue;
                    }
                    uint2 l = p - uint2(org);
                    float I = s_lp[l.y + 2u][l.x + 2u];
                    if (pc_src == 0u)
                    {
                        _422 = float3(s_dl[l.y + 2u][l.x + 2u], 0.0f, 0.0f);
                    }
                    else
                    {
                        _422 = s_v[l.y][l.x];
                    }
                    float3 v = _422;
                    A += float4(I, I * I, v.x, v.y);
                    B += float4(v.z, v * I);
                    c += 1.0f;
                }
            }
            OutA[int2(q)] = A / max(c, 1.0f).xxxx;
            OutB[int2(q)] = B / max(c, 1.0f).xxxx;
        }
    }
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    gl_LocalInvocationIndex = stage_input.gl_LocalInvocationIndex;
    comp_main();
}
