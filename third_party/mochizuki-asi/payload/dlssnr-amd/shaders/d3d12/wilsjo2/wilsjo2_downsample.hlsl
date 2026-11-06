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
RWTexture2D<float4> u0 : register(u4);
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

uint2 spvTextureSize(Texture2D<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

bool outside(uint2 p)
{
    bool _36 = p.x >= cb.Load(8);
    bool _47;
    if (!_36)
    {
        _47 = p.y >= cb.Load(12);
    }
    else
    {
        _47 = _36;
    }
    return _47;
}

float2 pixel_uv(uint2 p)
{
    return (float2(p) + 0.5f.xx) / float2(float(cb.Load(8)), float(cb.Load(12)));
}

void comp_main()
{
    uint2 p = gl_GlobalInvocationID.xy;
    uint2 param = p;
    if (outside(param))
    {
        return;
    }
    uint _87_dummy_parameter;
    int2 src = int2(spvTextureSize(t0, uint(0), _87_dummy_parameter));
    float Ws = float(src.x);
    float Hs = float(src.y);
    float Wd = float(cb.Load(8));
    float Hd = float(cb.Load(12));
    bool _111 = uint(src.x) == cb.Load(8);
    bool _120;
    if (_111)
    {
        _120 = uint(src.y) == cb.Load(12);
    }
    else
    {
        _120 = _111;
    }
    if (_120)
    {
        u0[int2(p)] = t0.Load(int3(int2(p), 0));
        return;
    }
    if (cb.Load(52) == 1u)
    {
        uint2 param_1 = p;
        u0[int2(p)] = t0.SampleLevel(_t0_sampler, pixel_uv(param_1), 0.0f);
        return;
    }
    float x0 = (float(p.x) * Ws) / Wd;
    float x1 = (float(p.x + 1u) * Ws) / Wd;
    float y0 = (float(p.y) * Hs) / Hd;
    float y1 = (float(p.y + 1u) * Hs) / Hd;
    float area = (x1 - x0) * (y1 - y0);
    int i0 = int(floor(x0));
    int i1 = int(ceil(x1)) - 1;
    int j0 = int(floor(y0));
    int j1 = int(ceil(y1)) - 1;
    float3 acc = 0.0f.xxx;
    for (int j = j0; j <= j1; j++)
    {
        int jj = clamp(j, 0, src.y - 1);
        float wy = max(min(y1, float(j + 1)) - max(y0, float(j)), 0.0f);
        for (int i = i0; i <= i1; i++)
        {
            int ii = clamp(i, 0, src.x - 1);
            float wx = max(min(x1, float(i + 1)) - max(x0, float(i)), 0.0f);
            acc += (t0.Load(int3(int2(ii, jj), 0)).xyz * (wx * wy));
        }
    }
    int ax = clamp(int(floor(((float(p.x) + 0.5f) * Ws) / Wd)), 0, src.x - 1);
    int ay = clamp(int(floor(((float(p.y) + 0.5f) * Hs) / Hd)), 0, src.y - 1);
    u0[int2(p)] = float4(acc / area.xxx, t0.Load(int3(int2(ax, ay), 0)).w);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
