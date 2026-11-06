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

static uint3 gl_WorkGroupID;
static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
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
    bool _135 = p.x >= cb.Load(8);
    bool _146;
    if (!_135)
    {
        _146 = p.y >= cb.Load(12);
    }
    else
    {
        _146 = _135;
    }
    return _146;
}

int2 source_size()
{
    uint _156_dummy_parameter;
    return int2(spvTextureSize(t0, uint(0), _156_dummy_parameter));
}

float magic(float x)
{
    if (abs(x) >= 1.5f)
    {
        return 0.0f;
    }
    if (x <= (-0.5f))
    {
        float t = x + 1.5f;
        return (0.5f * t) * t;
    }
    if (x < 0.5f)
    {
        return 0.75f - (x * x);
    }
    float t_1 = x - 1.5f;
    return (0.5f * t_1) * t_1;
}

float3 magic_down(uint2 p)
{
    int2 size = source_size();
    float2 k = float2(float(cb.Load(8)) / float(size.x), float(cb.Load(12)) / float(size.y));
    int2 out_base = int2(gl_WorkGroupID.xy * uint2(8u, 8u));
    float2 o_min = float2(out_base) + 0.5f.xx;
    float2 o_max = float2(out_base + int2(7, 7)) + 0.5f.xx;
    int2 g0 = int2(ceil(((o_min - 1.5f.xx) / k) - 0.5f.xx));
    int2 g1 = int2(floor(((o_max + 1.5f.xx) / k) - 0.5f.xx));
    int2 tile = clamp((g1 - g0) + int2(1, 1), int2(1, 1), int2(32, 32));
    int2 start = clamp(g0, int2(0, 0), max((size - tile), int2(0, 0)));
    float2 o = float2(p) + 0.5f.xx;
    int2 lo = int2(ceil(((o - 1.5f.xx) / k) - 0.5f.xx));
    int2 hi = int2(floor(((o + 1.5f.xx) / k) - 0.5f.xx));
    lo = clamp(lo, int2(0, 0), size - int2(1, 1));
    hi = clamp(hi, int2(0, 0), size - int2(1, 1));
    int2 n = clamp((hi - lo) + int2(1, 1), int2(1, 1), int2(12, 12));
    float ub = (k.x * (float(lo.x) + 0.5f)) - o.x;
    float vb = (k.y * (float(lo.y) + 0.5f)) - o.y;
    float sx = 0.0f;
    float sy = 0.0f;
    float wx[12];
    float _1334;
    float _1353;
    float wy[12];
    float _1369;
    float _1388;
    for (int i = 0; i < 12; i++)
    {
        if (i < n.x)
        {
            float param = ub + (k.x * float(i));
            _1334 = magic(param);
        }
        else
        {
            _1334 = 0.0f;
        }
        wx[i] = _1334;
        if (i < n.x)
        {
            _1353 = wx[i];
        }
        else
        {
            _1353 = 0.0f;
        }
        sx += _1353;
        if (i < n.y)
        {
            float param_1 = vb + (k.y * float(i));
            _1369 = magic(param_1);
        }
        else
        {
            _1369 = 0.0f;
        }
        wy[i] = _1369;
        if (i < n.y)
        {
            _1388 = wy[i];
        }
        else
        {
            _1388 = 0.0f;
        }
        sy += _1388;
    }
    float _1403;
    if (sx > 0.0f)
    {
        _1403 = 1.0f / sx;
    }
    else
    {
        _1403 = 0.0f;
    }
    float ix = _1403;
    float _1413;
    if (sy > 0.0f)
    {
        _1413 = 1.0f / sy;
    }
    else
    {
        _1413 = 0.0f;
    }
    float iy = _1413;
    int2 bl = clamp(lo - start, int2(0, 0), tile - n);
    float3 acc = 0.0f.xxx;
    for (int j = 0; j < n.y; j++)
    {
        float wyj = wy[j] * iy;
        int ty = bl.y + j;
        for (int i_1 = 0; i_1 < 12; i_1++)
        {
            if (i_1 >= n.x)
            {
                continue;
            }
            int2 src = clamp(start + int2(bl.x + i_1, ty), int2(0, 0), size - int2(1, 1));
            acc += (t0.Load(int3(src, 0)).xyz * ((wx[i_1] * ix) * wyj));
        }
    }
    return acc;
}

float keys_w2(float x, float a)
{
    precise float _183 = x - 5.0f;
    precise float _184 = x * _183;
    precise float _186 = _184 + 8.0f;
    precise float _187 = x * _186;
    precise float _189 = _187 - 4.0f;
    precise float _190 = a * _189;
    float r = _190;
    return r;
}

float keys_w1(float x, float a)
{
    precise float _162 = x * x;
    precise float _165 = a + 2.0f;
    precise float _167 = _165 * x;
    precise float _170 = a + 3.0f;
    precise float _171 = _167 - _170;
    precise float _172 = _162 * _171;
    precise float _173 = _172 + 1.0f;
    float r = _173;
    return r;
}

float4 up_weights(float phase)
{
    float d1 = (float(uint(phase * 16.0f)) + 0.5f) / 16.0f;
    float param = 1.0f + d1;
    float param_1 = -0.5f;
    float param_2 = d1;
    float param_3 = -0.5f;
    float param_4 = 1.0f - d1;
    float param_5 = -0.5f;
    float param_6 = 2.0f - d1;
    float param_7 = -0.5f;
    return float4(keys_w2(param, param_1), keys_w1(param_2, param_3), keys_w1(param_4, param_5), keys_w2(param_6, param_7));
}

float3 up_texel(int2 p, int2 size)
{
    bool _230 = any(bool2(p.x < int2(0, 0).x, p.y < int2(0, 0).y));
    bool _238;
    if (!_230)
    {
        _238 = any(bool2(p.x >= size.x, p.y >= size.y));
    }
    else
    {
        _238 = _230;
    }
    float3 _239;
    if (_238)
    {
        _239 = 0.0f.xxx;
    }
    else
    {
        _239 = t0.Load(int3(p, 0)).xyz;
    }
    return _239;
}

float3 dot4(float3 a, float3 b, float3 c, float3 d, float4 w)
{
    return (((a * w.x) + (b * w.y)) + (c * w.z)) + (d * w.w);
}

float3 bicubic_up(uint2 p)
{
    int2 size = source_size();
    float2 rcp_scale = float2(float(size.x) / float(cb.Load(8)), float(size.y) / float(cb.Load(12)));
    float2 top_left = ((float2(p) + 0.5f.xx) * rcp_scale) - 1.5f.xx;
    float2 phase = top_left - floor(top_left);
    int2 t = int2(floor(top_left));
    float param = phase.x;
    float4 xw = up_weights(param);
    float param_1 = phase.y;
    float4 yw = up_weights(param_1);
    float3 rows[4];
    for (int j = 0; j < 4; j++)
    {
        int2 param_2 = t + int2(0, j);
        int2 param_3 = size;
        int2 param_4 = t + int2(1, j);
        int2 param_5 = size;
        int2 param_6 = t + int2(2, j);
        int2 param_7 = size;
        int2 param_8 = t + int2(3, j);
        int2 param_9 = size;
        float3 param_10 = up_texel(param_2, param_3);
        float3 param_11 = up_texel(param_4, param_5);
        float3 param_12 = up_texel(param_6, param_7);
        float3 param_13 = up_texel(param_8, param_9);
        float4 param_14 = xw;
        rows[j] = dot4(param_10, param_11, param_12, param_13, param_14);
    }
    float3 param_15 = rows[0];
    float3 param_16 = rows[1];
    float3 param_17 = rows[2];
    float3 param_18 = rows[3];
    float4 param_19 = yw;
    return dot4(param_15, param_16, param_17, param_18, param_19);
}

float3 fsr1_bilinear(uint2 p)
{
    float2 c0xy = float2(asfloat(cb.Load(176)), asfloat(cb.Load(180)));
    float2 c0zw = float2(asfloat(cb.Load(184)), asfloat(cb.Load(188)));
    float2 c1xy = float2(asfloat(cb.Load(192)), asfloat(cb.Load(196)));
    return t0.SampleLevel(_t0_sampler, c1xy * (((float2(p) * c0xy) + c0zw) + 0.5f.xx), 0.0f).xyz;
}

float keys(inout float x, float a)
{
    x = abs(x);
    float x2 = x * x;
    float x3 = x * x2;
    if (x < 1.0f)
    {
        return mad(x3, a + 2.0f, -(x2 * (a + 3.0f))) + 1.0f;
    }
    if (x < 2.0f)
    {
        return mad(x, 8.0f * a, mad(x3, a, -(x2 * (5.0f * a)))) + ((-4.0f) * a);
    }
    return 0.0f;
}

void keys_axis(float t, float a, inout float w01, inout float w23, out float o01, out float o23)
{
    float param = 1.0f + t;
    float param_1 = a;
    float _451 = keys(param, param_1);
    float w0 = _451;
    float param_2 = t;
    float param_3 = a;
    float _457 = keys(param_2, param_3);
    float w1 = _457;
    float param_4 = 1.0f - t;
    float param_5 = a;
    float _464 = keys(param_4, param_5);
    float w2 = _464;
    float param_6 = 2.0f - t;
    float param_7 = a;
    float _471 = keys(param_6, param_7);
    float w3 = _471;
    w01 = w0 + w1;
    w23 = w2 + w3;
    float _481;
    if (w01 != 0.0f)
    {
        _481 = 1.0f / w01;
    }
    else
    {
        _481 = 0.0f;
    }
    float i01 = _481;
    float _491;
    if (w23 != 0.0f)
    {
        _491 = 1.0f / w23;
    }
    else
    {
        _491 = 0.0f;
    }
    float i23 = _491;
    o01 = mad(w1, i01, -1.0f);
    o23 = mad(w3, i23, 1.0f);
}

float3 keys_down(uint2 p, float a, float offset)
{
    int2 size = source_size();
    float2 dst = float2(float(p.x) + 0.5f, float(p.y) + 0.5f);
    float2 scale = float2(float(size.x) / float(cb.Load(8)), float(size.y) / float(cb.Load(12)));
    float2 pos = mad(dst, scale, (-offset).xx);
    float2 ip = floor(pos);
    float2 f = pos - ip;
    float2 base = ip - 1.0f.xx;
    float param = f.x;
    float param_1 = a;
    float param_2;
    float param_3;
    float param_4;
    float param_5;
    keys_axis(param, param_1, param_2, param_3, param_4, param_5);
    float wx01 = param_2;
    float wx23 = param_3;
    float ox01 = param_4;
    float ox23 = param_5;
    float param_6 = f.y;
    float param_7 = a;
    float param_8;
    float param_9;
    float param_10;
    float param_11;
    keys_axis(param_6, param_7, param_8, param_9, param_10, param_11);
    float wy01 = param_8;
    float wy23 = param_9;
    float oy01 = param_10;
    float oy23 = param_11;
    float2 inv = 1.0f.xx / float2(float(size.x), float(size.y));
    float3 s00 = t0.SampleLevel(_t0_sampler, ((base + float2(ox01, oy01)) + 0.5f.xx) * inv, 0.0f).xyz;
    float3 s10 = t0.SampleLevel(_t0_sampler, ((base + float2(ox23, oy01)) + 0.5f.xx) * inv, 0.0f).xyz;
    float3 s01 = t0.SampleLevel(_t0_sampler, ((base + float2(ox01, oy23)) + 0.5f.xx) * inv, 0.0f).xyz;
    float3 s11 = t0.SampleLevel(_t0_sampler, ((base + float2(ox23, oy23)) + 0.5f.xx) * inv, 0.0f).xyz;
    float3 c = mad(mad(s00, wx01.xxx, s10 * wx23), wy01.xxx, mad(s01, wx01.xxx, s11 * wx23) * wy23);
    return clamp(c, min(min(s00, s10), min(s01, s11)), max(max(s00, s10), max(s01, s11)));
}

float bessel_i0(float x)
{
    float ax = abs(x);
    if (ax < 3.75f)
    {
        float t = x / 3.75f;
        float t2_1 = t * t;
        return 1.0f + (t2_1 * (3.5156228542327880859375f + (t2_1 * (3.089942455291748046875f + (t2_1 * (1.2067492008209228515625f + (t2_1 * (0.26597321033477783203125f + (t2_1 * (0.03607679903507232666015625f + (t2_1 * 0.0045813000760972499847412109375f)))))))))));
    }
    float t_1 = 3.75f / ax;
    return (exp(ax) / sqrt(ax)) * (0.398942291736602783203125f + (t_1 * (0.01328592002391815185546875f + (t_1 * (0.00225318991579115390777587890625f + (t_1 * ((-0.00157564994879066944122314453125f) + (t_1 * (0.0091628096997737884521484375f + (t_1 * ((-0.02057706005871295928955078125f) + (t_1 * (0.02635537087917327880859375f + (t_1 * ((-0.0164763294160366058349609375f) + (t_1 * 0.0039237700402736663818359375f))))))))))))))));
}

float sinc(inout float x)
{
    x *= 3.1415927410125732421875f;
    if (abs(x) < 9.9999997473787516355514526367188e-06f)
    {
        return 1.0f;
    }
    return sin(x) / x;
}

float lanczos(float x, float a)
{
    float _707;
    if (abs(x) >= a)
    {
        _707 = 0.0f;
    }
    else
    {
        float param = x;
        float _713 = sinc(param);
        float param_1 = x / a;
        float _718 = sinc(param_1);
        _707 = _713 * _718;
    }
    return _707;
}

float saturate1(float x)
{
    return clamp(x, 0.0f, 1.0f);
}

float kaiser(float x, float a, float beta, float inv_i0_beta)
{
    float ax = abs(x);
    if (ax >= a)
    {
        return 0.0f;
    }
    float r = ax / a;
    float param = x;
    float _822 = sinc(param);
    float param_1 = 1.0f - (r * r);
    float param_2 = beta * sqrt(saturate1(param_1));
    return _822 * (bessel_i0(param_2) * inv_i0_beta);
}

float3 windowed_down(uint2 p, int taps, float a, int kind, float beta)
{
    int2 size = source_size();
    float2 dst = float2(float(p.x) + 0.5f, float(p.y) + 0.5f);
    float2 scale = float2(float(size.x) / float(cb.Load(8)), float(size.y) / float(cb.Load(12)));
    float2 pos = (dst * scale) - 0.5f.xx;
    float2 ip = floor(pos);
    float2 f = pos - ip;
    int back = (taps / 2) - 1;
    int2 base = int2(ip) - back.xx;
    float _893;
    if (kind == 1)
    {
        float param = beta;
        _893 = 1.0f / bessel_i0(param);
    }
    else
    {
        _893 = 0.0f;
    }
    float inv_i0_beta = _893;
    float sx = 0.0f;
    float sy = 0.0f;
    float wx[6];
    float _938;
    float wy[6];
    float _967;
    for (int i = 0; i < taps; i++)
    {
        float dx = (float(i) - float(back)) - f.x;
        float dy = (float(i) - float(back)) - f.y;
        if (kind == 0)
        {
            float param_1 = dx;
            float param_2 = a;
            _938 = lanczos(param_1, param_2);
        }
        else
        {
            float param_3 = dx;
            float param_4 = a;
            float param_5 = beta;
            float param_6 = inv_i0_beta;
            _938 = kaiser(param_3, param_4, param_5, param_6);
        }
        wx[i] = _938;
        sx += wx[i];
        if (kind == 0)
        {
            float param_7 = dy;
            float param_8 = a;
            _967 = lanczos(param_7, param_8);
        }
        else
        {
            float param_9 = dy;
            float param_10 = a;
            float param_11 = beta;
            float param_12 = inv_i0_beta;
            _967 = kaiser(param_9, param_10, param_11, param_12);
        }
        wy[i] = _967;
        sy += wy[i];
    }
    float _997;
    if (sx != 0.0f)
    {
        _997 = 1.0f / sx;
    }
    else
    {
        _997 = 0.0f;
    }
    float ix = _997;
    float _1007;
    if (sy != 0.0f)
    {
        _1007 = 1.0f / sy;
    }
    else
    {
        _1007 = 0.0f;
    }
    float iy = _1007;
    for (int i_1 = 0; i_1 < taps; i_1++)
    {
        wx[i_1] *= ix;
        wy[i_1] *= iy;
    }
    float2 inv = 1.0f.xx / float2(float(size.x), float(size.y));
    float3 acc = 0.0f.xxx;
    float3 lo = 1000000015047466219876688855040.0f.xxx;
    float3 hi = (-1000000015047466219876688855040.0f).xxx;
    for (int j = 0; j < taps; j++)
    {
        int y = clamp(base.y + j, 0, size.y - 1);
        float wyj = wy[j];
        for (int i_2 = 0; i_2 < taps; i_2++)
        {
            int x = clamp(base.x + i_2, 0, size.x - 1);
            float w = wx[i_2] * wyj;
            float3 s = t0.SampleLevel(_t0_sampler, float2(float(x) + 0.5f, float(y) + 0.5f) * inv, 0.0f).xyz;
            lo = min(lo, s);
            hi = max(hi, s);
            acc += (s * w);
        }
    }
    return clamp(acc, lo, hi);
}

void comp_main()
{
    uint2 p = gl_GlobalInvocationID.xy;
    bool _1543 = cb.Load(0) == 31u;
    bool _1551;
    if (_1543)
    {
        _1551 = cb.Load(24) == 7u;
    }
    else
    {
        _1551 = _1543;
    }
    if (_1551)
    {
        uint2 param = p;
        if (outside(param))
        {
            return;
        }
        uint2 param_1 = p;
        u0[int2(p)] = float4(magic_down(param_1), 1.0f);
        return;
    }
    uint2 param_2 = p;
    if (outside(param_2))
    {
        return;
    }
    float3 c;
    if (cb.Load(0) == 30u)
    {
        uint2 param_3 = p;
        c = bicubic_up(param_3);
    }
    else
    {
        if (cb.Load(0) == 32u)
        {
            uint2 param_4 = p;
            c = fsr1_bilinear(param_4);
        }
        else
        {
            switch (cb.Load(24))
            {
                case 2u:
                {
                    uint2 param_5 = p;
                    float param_6 = -0.5f;
                    float param_7 = 0.449999988079071044921875f;
                    c = keys_down(param_5, param_6, param_7);
                    break;
                }
                case 3u:
                {
                    uint2 param_8 = p;
                    int param_9 = 4;
                    float param_10 = 2.0f;
                    int param_11 = 0;
                    float param_12 = 0.0f;
                    c = windowed_down(param_8, param_9, param_10, param_11, param_12);
                    break;
                }
                case 4u:
                {
                    uint2 param_13 = p;
                    int param_14 = 6;
                    float param_15 = 3.0f;
                    int param_16 = 0;
                    float param_17 = 0.0f;
                    c = windowed_down(param_13, param_14, param_15, param_16, param_17);
                    break;
                }
                case 5u:
                {
                    uint2 param_18 = p;
                    int param_19 = 4;
                    float param_20 = 2.0f;
                    int param_21 = 1;
                    float param_22 = 5.0f;
                    c = windowed_down(param_18, param_19, param_20, param_21, param_22);
                    break;
                }
                case 6u:
                {
                    uint2 param_23 = p;
                    int param_24 = 6;
                    float param_25 = 3.0f;
                    int param_26 = 1;
                    float param_27 = 6.0f;
                    c = windowed_down(param_23, param_24, param_25, param_26, param_27);
                    break;
                }
                default:
                {
                    uint2 param_28 = p;
                    float param_29 = -0.60000002384185791015625f;
                    float param_30 = 0.5f;
                    c = keys_down(param_28, param_29, param_30);
                    break;
                }
            }
        }
    }
    u0[int2(p)] = float4(c, 1.0f);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
