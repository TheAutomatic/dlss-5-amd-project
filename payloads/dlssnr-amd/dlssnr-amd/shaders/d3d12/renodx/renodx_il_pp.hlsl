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
    uint2 pc_ssize : packoffset(c0);
    uint2 pc_dsize : packoffset(c0.z);
    uint pc_mode : packoffset(c1);
    uint pc_n : packoffset(c1.y);
    uint pc_flags : packoffset(c1.z);
};

Texture2D<float4> S0 : register(t1);
Texture2D<float4> S1 : register(t2);
Texture2D<float4> S2 : register(t3);
Texture2D<float4> S3 : register(t4);
Texture2D<float4> S4 : register(t5);
Texture2D<float4> S5 : register(t6);
Texture2D<float4> S6 : register(t7);
Texture2D<float4> S7 : register(t8);
Texture2D<float4> Trust : register(t0);
Texture2D<float4> Near : register(t11);
Texture2D<float4> Src : register(t9);
Texture2D<float4> DSrc : register(t12);
RWTexture2D<float4> Dst : register(u10);
RWTexture2D<float2> DDst : register(u13);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

float3 inc(uint i, int2 c)
{
    switch (i)
    {
        case 0u:
        {
            return S0.Load(int3(c, 0)).xyz;
        }
        case 1u:
        {
            return S1.Load(int3(c, 0)).xyz;
        }
        case 2u:
        {
            return S2.Load(int3(c, 0)).xyz;
        }
        case 3u:
        {
            return S3.Load(int3(c, 0)).xyz;
        }
        case 4u:
        {
            return S4.Load(int3(c, 0)).xyz;
        }
        case 5u:
        {
            return S5.Load(int3(c, 0)).xyz;
        }
        case 6u:
        {
            return S6.Load(int3(c, 0)).xyz;
        }
        default:
        {
            return S7.Load(int3(c, 0)).xyz;
        }
    }
}

void comp_main()
{
    uint2 g = gl_GlobalInvocationID.xy;
    bool _105 = g.x >= (pc_dsize.x * pc_n);
    bool _115;
    if (!_105)
    {
        _115 = g.y >= pc_dsize.y;
    }
    else
    {
        _115 = _105;
    }
    if (_115)
    {
        return;
    }
    uint i = g.x / pc_dsize.x;
    int2 d = int2(int(g.x - (i * pc_dsize.x)), int(g.y));
    float4 s = 0.0f.xxxx;
    float2 dn = 0.0f.xx;
    bool depth = ((pc_flags & 1u) != 0u) && (i == 0u);
    for (int y = 0; y < 2; y++)
    {
        for (int x = 0; x < 2; x++)
        {
            int2 c = (int2(2, 2) * d) + int2(x, y);
            uint2 _183 = uint2(c);
            if (any(bool2(_183.x >= pc_ssize.x, _183.y >= pc_ssize.y)))
            {
                continue;
            }
            if (pc_mode == 0u)
            {
                float w = Trust.Load(int3(c, 0)).x;
                uint param = i;
                int2 param_1 = c;
                s += float4(inc(param, param_1) * w, w);
                if (depth)
                {
                    dn += float2(Near.Load(int3(c, 0)).x * w, w);
                }
            }
            else
            {
                s += Src.Load(int3(int2(c.x + int(i * pc_ssize.x), c.y), 0));
                if (depth)
                {
                    dn += DSrc.Load(int3(c, 0)).xy;
                }
            }
        }
    }
    Dst[int2(g)] = s * 0.25f;
    if (depth)
    {
        DDst[d] = float4(dn * 0.25f, 0.0f, 0.0f).xy;
    }
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
