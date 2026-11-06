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
cbuffer Push
{
    uint2 pc_low : packoffset(c0);
    uint pc_radius : packoffset(c0.z);
    uint pc_coef : packoffset(c0.w);
    float pc_eps : packoffset(c1);
};

Texture2D<float4> InA : register(t0);
Texture2D<float4> InB : register(t1);
RWTexture2D<float4> OutA : register(u4);
RWTexture2D<float4> OutB : register(u5);

static uint3 gl_WorkGroupID;
static uint3 gl_LocalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
};

groupshared float4 s_a[24][16];
groupshared float4 s_b[24][16];

void comp_main()
{
    int2 org = int2(gl_WorkGroupID.xy) * int2(16, 16);
    int2 lid = int2(gl_LocalInvocationID.xy);
    int r = int(pc_radius);
    int col = org.x + lid.x;
    for (int k = lid.y; k < (16 + (2 * r)); k += 16)
    {
        int row = (org.y - r) + k;
        bool _69 = row < 0;
        bool _78;
        if (!_69)
        {
            _78 = row >= int(pc_low.y);
        }
        else
        {
            _78 = _69;
        }
        bool _87;
        if (!_78)
        {
            _87 = col >= int(pc_low.x);
        }
        else
        {
            _87 = _78;
        }
        if (_87)
        {
            continue;
        }
        float4 A = 0.0f.xxxx;
        float4 B = 0.0f.xxxx;
        float c = 0.0f;
        int _101 = -r;
        for (int i = _101; i <= r; i++)
        {
            int2 t = int2(col + i, row);
            bool _118 = t.x < 0;
            bool _128;
            if (!_118)
            {
                _128 = t.x >= int(pc_low.x);
            }
            else
            {
                _128 = _118;
            }
            if (_128)
            {
                continue;
            }
            precise float4 _139 = A + InA.Load(int3(t, 0));
            A = _139;
            precise float4 _145 = B + InB.Load(int3(t, 0));
            B = _145;
            precise float _148 = c + 1.0f;
            c = _148;
        }
        precise float4 _163 = A / c.xxxx;
        s_a[k][lid.x] = _163;
        precise float4 _173 = B / c.xxxx;
        s_b[k][lid.x] = _173;
    }
    GroupMemoryBarrierWithGroupSync();
    int2 q = org + lid;
    int2 _187 = int2(pc_low);
    if (any(bool2(q.x >= _187.x, q.y >= _187.y)))
    {
        return;
    }
    float4 sa = 0.0f.xxxx;
    float4 sb = 0.0f.xxxx;
    float c_1 = 0.0f;
    int _199 = -r;
    for (int i_1 = _199; i_1 <= r; i_1++)
    {
        int row_1 = q.y + i_1;
        bool _214 = row_1 < 0;
        bool _223;
        if (!_214)
        {
            _223 = row_1 >= int(pc_low.y);
        }
        else
        {
            _223 = _214;
        }
        if (_223)
        {
            continue;
        }
        precise float4 _238 = sa + s_a[(lid.y + r) + i_1][lid.x];
        sa = _238;
        precise float4 _250 = sb + s_b[(lid.y + r) + i_1][lid.x];
        sb = _250;
        precise float _252 = c_1 + 1.0f;
        c_1 = _252;
    }
    precise float4 _259 = sa / c_1.xxxx;
    float4 A_1 = _259;
    precise float4 _264 = sb / c_1.xxxx;
    float4 B_1 = _264;
    if (pc_coef != 0u)
    {
        float mI = A_1.x;
        float vI = max(A_1.y - (mI * mI), 0.0f);
        float3 mp = float3(A_1.z, A_1.w, B_1.x);
        float3 mIp = B_1.yzw;
        float3 a = (mIp - (mp * mI)) / (vI + pc_eps).xxx;
        OutA[q] = float4(a, mI);
        OutB[q] = float4(mp - (a * mI), vI);
    }
    else
    {
        OutA[q] = A_1;
        OutB[q] = B_1;
    }
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    comp_main();
}
