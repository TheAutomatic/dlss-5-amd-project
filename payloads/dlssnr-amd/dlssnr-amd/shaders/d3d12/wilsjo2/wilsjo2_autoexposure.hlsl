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

static uint3 gl_LocalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
};

groupshared float sum_a[64];
groupshared float sum_b[64];
groupshared float sum_n[64];

bool finite1(float v)
{
    bool _55 = isnan(v);
    bool _61;
    if (!_55)
    {
        _61 = isinf(v);
    }
    else
    {
        _61 = _55;
    }
    return !_61;
}

float saturate1(float x)
{
    return clamp(x, 0.0f, 1.0f);
}

float tile_pixels(uint tx, uint ty, uint w, uint h)
{
    uint x0 = (tx * w) / 64u;
    uint x1 = ((tx + 1u) * w) / 64u;
    uint y0 = (ty * h) / 64u;
    uint y1 = ((ty + 1u) * h) / 64u;
    return float(max((x1 - x0), 1u)) * float(max((y1 - y0), 1u));
}

float sanitize1(float v, float f)
{
    float param = v;
    return finite1(param) ? v : f;
}

float tile_mean(uint tx, uint ty)
{
    float param = t0.Load(int3(int2(int(tx), int(ty)), 0)).x;
    float param_1 = 0.0f;
    return max(sanitize1(param, param_1), 0.0f);
}

void reduce3(uint lane, inout float a, inout float b, inout float n)
{
    sum_a[lane] = a;
    sum_b[lane] = b;
    sum_n[lane] = n;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32u; stride > 0u; stride = stride >> 1u)
    {
        if (lane < stride)
        {
            sum_a[lane] += sum_a[lane + stride];
            sum_b[lane] += sum_b[lane + stride];
            sum_n[lane] += sum_n[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    a = sum_a[0];
    b = sum_b[0];
    n = sum_n[0];
    GroupMemoryBarrierWithGroupSync();
}

float lerp1(float a, float b, float t)
{
    return a + (t * (b - a));
}

float reduce1(uint lane, float v)
{
    sum_a[lane] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32u; stride > 0u; stride = stride >> 1u)
    {
        if (lane < stride)
        {
            sum_a[lane] += sum_a[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    return sum_a[0];
}

void comp_main()
{
    uint lane = (gl_LocalInvocationID.y * 8u) + gl_LocalInvocationID.x;
    float param = asfloat(cb.Load(148));
    bool _275 = finite1(param);
    bool _282;
    if (_275)
    {
        _282 = asfloat(cb.Load(148)) > 9.9999999747524270787835121154785e-07f;
    }
    else
    {
        _282 = _275;
    }
    float _283;
    if (_282)
    {
        _283 = asfloat(cb.Load(148));
    }
    else
    {
        _283 = 1.0f;
    }
    float pe = _283;
    float param_1 = asfloat(cb.Load(156)) * 0.00999999977648258209228515625f;
    float prot = saturate1(param_1);
    uint src_w = max(cb.Load(164), 1u);
    uint src_h = max(cb.Load(168), 1u);
    float a = 0.0f;
    float b = 0.0f;
    float n = 0.0f;
    for (uint index = lane; index < 4096u; index += 64u)
    {
        uint tx = index % 64u;
        uint ty = index / 64u;
        uint param_2 = tx;
        uint param_3 = ty;
        uint param_4 = src_w;
        uint param_5 = src_h;
        float pixels = tile_pixels(param_2, param_3, param_4, param_5);
        uint param_6 = tx;
        uint param_7 = ty;
        float mean = tile_mean(param_6, param_7);
        a += (mean * pixels);
        n += pixels;
        if (prot > 0.0f)
        {
            b += (clamp(log2(max(mean / pe, 9.9999999392252902907785028219223e-09f)), -24.0f, 24.0f) * pixels);
        }
    }
    uint param_8 = lane;
    float param_9 = a;
    float param_10 = b;
    float param_11 = n;
    reduce3(param_8, param_9, param_10, param_11);
    a = param_9;
    b = param_10;
    n = param_11;
    float _386;
    if (n > 0.0f)
    {
        _386 = a / n;
    }
    else
    {
        _386 = 0.0f;
    }
    float avg = _386;
    float metered = avg / pe;
    if ((prot > 0.0f) && (n > 0.0f))
    {
        float ref = b / n;
        float param_12 = 3.0f;
        float param_13 = 1.0f;
        float param_14 = prot;
        float knee = lerp1(param_12, param_13, param_14);
        float param_15 = 1.0f;
        float param_16 = 0.3499999940395355224609375f;
        float param_17 = prot;
        float slope = lerp1(param_15, param_16, param_17);
        float s = 0.0f;
        float _468;
        for (uint index_1 = lane; index_1 < 4096u; index_1 += 64u)
        {
            uint tx_1 = index_1 % 64u;
            uint ty_1 = index_1 / 64u;
            uint param_18 = tx_1;
            uint param_19 = ty_1;
            uint param_20 = src_w;
            uint param_21 = src_h;
            float pixels_1 = tile_pixels(param_18, param_19, param_20, param_21);
            uint param_22 = tx_1;
            uint param_23 = ty_1;
            float log_l = clamp(log2(max(tile_mean(param_22, param_23) / pe, 9.9999999392252902907785028219223e-09f)), -24.0f, 24.0f);
            float delta = log_l - ref;
            if (delta > knee)
            {
                _468 = (ref + knee) + ((delta - knee) * slope);
            }
            else
            {
                _468 = log_l;
            }
            float comp = _468;
            s += (exp2(clamp(comp, -24.0f, 24.0f)) * pixels_1);
        }
        uint param_24 = lane;
        float param_25 = s;
        float _497 = reduce1(param_24, param_25);
        float protected_avg = _497 / n;
        float param_26 = protected_avg;
        if (finite1(param_26) && (protected_avg > 9.9999999392252902907785028219223e-09f))
        {
            metered = protected_avg;
        }
    }
    float white = (pe * metered) * 4.5555553436279296875f;
    if (lane == 0u)
    {
        float param_27 = white;
        u0[int2(0, 0)] = float4((finite1(param_27) && (white > 9.9999999392252902907785028219223e-09f)) ? white : 1.0f, 0.0f, 0.0f, 1.0f);
    }
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    comp_main();
}
