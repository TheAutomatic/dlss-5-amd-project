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
struct NrGrade
{
    float black;
    float white;
    float exposure;
    float gamma;
    float contrast;
    float saturation;
    float sat_power;
    float tint1;
    float tint2;
    float band0;
    float band1;
    float band2;
    float band3;
    float band4;
};

#ifndef SPIRV_CROSS_CONSTANT_ID_0
#define SPIRV_CROSS_CONSTANT_ID_0 65535u
#endif
static const uint kStep = SPIRV_CROSS_CONSTANT_ID_0;
static const bool _2196 = (kStep != 65535u);
static const bool _2236 = (kStep != 65535u);
static const bool _2255 = (kStep != 65535u);
static const bool _2295 = (kStep != 65535u);
static const bool _2462 = (kStep != 65535u);

static const NrGrade _182 = { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

cbuffer Push
{
    uint2 pc_size : packoffset(c0);
    uint2 pc_mbase : packoffset(c0.z);
    uint2 pc_mgrid : packoffset(c1);
    float2 pc_mscale : packoffset(c1.z);
    uint pc_mode : packoffset(c2);
    uint pc_slot : packoffset(c2.y);
    uint pc_n : packoffset(c2.z);
    uint pc_flags : packoffset(c2.w);
    float pc_k0 : packoffset(c3);
    float pc_k1 : packoffset(c3.y);
    uint2 pc_dbase : packoffset(c3.z);
    uint2 pc_dgrid : packoffset(c4);
    uint pc_styles : packoffset(c4.z);
    uint pc_pad0 : packoffset(c4.w);
    float4 pc_inten : packoffset(c5);
};

Texture2D<float4> DepthIn : register(t29);
Texture2D<float4> S0 : register(t4);
Texture2D<float4> S1 : register(t5);
Texture2D<float4> S2 : register(t6);
Texture2D<float4> S3 : register(t7);
Texture2D<float4> S4 : register(t8);
Texture2D<float4> S5 : register(t9);
Texture2D<float4> S6 : register(t10);
Texture2D<float4> S7 : register(t11);
Texture2D<float4> BasePrev : register(t21);
RWTexture2D<float4> D0 : register(u12);
RWTexture2D<float4> D1 : register(u13);
RWTexture2D<float4> D2 : register(u14);
RWTexture2D<float4> D3 : register(u15);
RWTexture2D<float4> D4 : register(u16);
RWTexture2D<float4> D5 : register(u17);
RWTexture2D<float4> D6 : register(u18);
RWTexture2D<float4> D7 : register(u19);
Texture2D<float4> L1 : register(t23);
Texture2D<float4> L2 : register(t24);
Texture2D<float4> L3 : register(t25);
Texture2D<float4> L4 : register(t26);
Texture2D<float4> L5 : register(t27);
Texture2D<float4> L6 : register(t28);
Texture2D<float4> DP1 : register(t37);
Texture2D<float4> DP2 : register(t38);
Texture2D<float4> DP3 : register(t39);
Texture2D<float4> DP4 : register(t40);
Texture2D<float4> DP5 : register(t41);
Texture2D<float4> DP6 : register(t42);
Texture2D<float4> Motion : register(t3);
Texture2D<float4> MvPrev : register(t36);
RWTexture2D<float> Trust : register(u22);
RWTexture2D<float> DepthCur : register(u30);
RWTexture2D<float4> L1Out : register(u44);
RWTexture2D<float2> DP1Out : register(u45);
RWTexture2D<float4> Base : register(u2);
RWTexture2D<float4> Proxy : register(u0);
RWTexture2D<float4> Hist : register(u20);
Texture2D<float4> DepthPrev : register(t31);
RWTexture2D<float4> TrustDbg : register(u43);
RWTexture2D<float4> Answer : register(u1);

static uint3 gl_LocalInvocationID;
static uint3 gl_GlobalInvocationID;
static uint gl_LocalInvocationIndex;
struct SPIRV_Cross_Input
{
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
    uint gl_LocalInvocationIndex : SV_GroupIndex;
};

groupshared float4 s_pp[256];
groupshared float2 s_dp[256];

float near_at(uint2 mt)
{
    uint2 t = pc_dbase + min((((mt - pc_mbase) * pc_dgrid) / pc_mgrid), (pc_dgrid - uint2(1u, 1u)));
    float d = DepthIn.Load(int3(int2(t), 0)).x;
    float _956;
    if ((pc_flags & 1024u) != 0u)
    {
        _956 = d;
    }
    else
    {
        _956 = 1.0f - d;
    }
    return _956;
}

void lvgeo(uint l, float2 p, inout int2 c[4], inout int wx, inout float2 t)
{
    uint2 w = pc_size;
    for (uint k = 0u; k < l; k++)
    {
        w = (w + uint2(1u, 1u)) / uint2(2u, 2u);
    }
    float2 f = (p / float(1u << l).xx) - 0.5f.xx;
    float2 b0 = floor(f);
    t = f - b0;
    int2 i0 = int2(b0);
    int2 hi = int2(w) - int2(1, 1);
    c[0] = clamp(i0, int2(0, 0), hi);
    c[1] = clamp(i0 + int2(1, 0), int2(0, 0), hi);
    c[2] = clamp(i0 + int2(0, 1), int2(0, 0), hi);
    c[3] = clamp(i0 + int2(1, 1), int2(0, 0), hi);
    wx = int(w.x);
}

float2 dlv(uint l, int2 c)
{
    switch (l)
    {
        case 1u:
        {
            return DP1.Load(int3(c, 0)).xy;
        }
        case 2u:
        {
            return DP2.Load(int3(c, 0)).xy;
        }
        case 3u:
        {
            return DP3.Load(int3(c, 0)).xy;
        }
        case 4u:
        {
            return DP4.Load(int3(c, 0)).xy;
        }
        case 5u:
        {
            return DP5.Load(int3(c, 0)).xy;
        }
        default:
        {
            return DP6.Load(int3(c, 0)).xy;
        }
    }
}

float4 lvw(uint l, float2 p, float near)
{
    uint param = l;
    float2 param_1 = p;
    int2 param_2[4];
    int param_3;
    float2 param_4;
    lvgeo(param, param_1, param_2, param_3, param_4);
    int2 c[4] = param_2;
    int wx = param_3;
    float2 t = param_4;
    float4 wb = float4((1.0f - t.x) * (1.0f - t.y), t.x * (1.0f - t.y), (1.0f - t.x) * t.y, t.x * t.y);
    float _1490;
    float _1504;
    for (int k = 0; k < 4; k++)
    {
        uint param_5 = l;
        int2 param_6 = c[k];
        float2 dn = dlv(param_5, param_6);
        if (dn.y > 9.9999999747524270787835121154785e-07f)
        {
            _1490 = dn.x / dn.y;
        }
        else
        {
            _1490 = -1.0f;
        }
        float nk = _1490;
        if (nk < 0.0f)
        {
            _1504 = 1.0f;
        }
        else
        {
            _1504 = abs(nk - near) / max(max(nk, near), 1.0000000116860974230803549289703e-07f);
        }
        float rel = _1504;
        wb[k] *= (1.0f - smoothstep(0.0500000007450580596923828125f, 0.20000000298023223876953125f, rel));
    }
    return wb;
}

float4 lv(uint l, int2 c)
{
    switch (l)
    {
        case 1u:
        {
            return L1.Load(int3(c, 0));
        }
        case 2u:
        {
            return L2.Load(int3(c, 0));
        }
        case 3u:
        {
            return L3.Load(int3(c, 0));
        }
        case 4u:
        {
            return L4.Load(int3(c, 0));
        }
        case 5u:
        {
            return L5.Load(int3(c, 0));
        }
        default:
        {
            return L6.Load(int3(c, 0));
        }
    }
}

float4 lvb(uint l, uint i, float2 p, bool bil, float4 wb)
{
    uint param = l;
    float2 param_1 = p;
    int2 param_2[4];
    int param_3;
    float2 param_4;
    lvgeo(param, param_1, param_2, param_3, param_4);
    int2 c[4] = param_2;
    int wx = param_3;
    float2 t = param_4;
    int2 off = int2(int(i) * wx, 0);
    uint param_5 = l;
    int2 param_6 = c[0] + off;
    float4 a = lv(param_5, param_6);
    uint param_7 = l;
    int2 param_8 = c[1] + off;
    float4 b = lv(param_7, param_8);
    uint param_9 = l;
    int2 param_10 = c[2] + off;
    float4 cc = lv(param_9, param_10);
    uint param_11 = l;
    int2 param_12 = c[3] + off;
    float4 d = lv(param_11, param_12);
    if (!bil)
    {
        return lerp(lerp(a, b, t.x.xxxx), lerp(cc, d, t.x.xxxx), t.y.xxxx);
    }
    float sw = ((wb.x + wb.y) + wb.z) + wb.w;
    float4 _1626;
    if (sw > 9.9999997473787516355514526367188e-05f)
    {
        _1626 = (((((a * wb.x) + (b * wb.y)) + (cc * wb.z)) + (d * wb.w)) / sw.xxxx) * min(sw * 4.0f, 1.0f);
    }
    else
    {
        _1626 = 0.0f.xxxx;
    }
    return _1626;
}

float3 pull(uint i, float2 p, bool bil, float4 wl[6])
{
    uint param = 6u;
    uint param_1 = i;
    float2 param_2 = p;
    bool param_3 = false;
    float4 param_4 = 0.0f.xxxx;
    float4 top0 = lvb(param, param_1, param_2, param_3, param_4);
    float4 _1671;
    if (bil)
    {
        uint param_5 = 6u;
        uint param_6 = i;
        float2 param_7 = p;
        bool param_8 = true;
        float4 param_9 = wl[5];
        _1671 = lvb(param_5, param_6, param_7, param_8, param_9);
    }
    else
    {
        _1671 = top0;
    }
    float4 topd = _1671;
    float4 top = (topd.w > 9.9999997473787516355514526367188e-05f) ? topd : top0;
    float3 _1697;
    if (top.w > 9.9999997473787516355514526367188e-05f)
    {
        _1697 = top.xyz / top.w.xxx;
    }
    else
    {
        _1697 = 0.0f.xxx;
    }
    float3 e = _1697;
    for (uint l = 5u; l >= 1u; l--)
    {
        uint param_10 = l;
        uint param_11 = i;
        float2 param_12 = p;
        bool param_13 = bil;
        float4 indexable[6] = wl;
        float4 param_14 = indexable[l - 1u];
        float4 m = lvb(param_10, param_11, param_12, param_13, param_14);
        e = m.xyz + (e * (1.0f - m.w));
    }
    return e;
}

float3 ld(uint i, int2 p)
{
    switch (i)
    {
        case 0u:
        {
            return D0[p].xyz;
        }
        case 1u:
        {
            return D1[p].xyz;
        }
        case 2u:
        {
            return D2[p].xyz;
        }
        case 3u:
        {
            return D3[p].xyz;
        }
        case 4u:
        {
            return D4[p].xyz;
        }
        case 5u:
        {
            return D5[p].xyz;
        }
        case 6u:
        {
            return D6[p].xyz;
        }
        default:
        {
            return D7[p].xyz;
        }
    }
}

void st(uint i, int2 p, float3 v)
{
    float4 o = float4(v, 0.0f);
    switch (i)
    {
        case 0u:
        {
            D0[p] = o;
            break;
        }
        case 1u:
        {
            D1[p] = o;
            break;
        }
        case 2u:
        {
            D2[p] = o;
            break;
        }
        case 3u:
        {
            D3[p] = o;
            break;
        }
        case 4u:
        {
            D4[p] = o;
            break;
        }
        case 5u:
        {
            D5[p] = o;
            break;
        }
        case 6u:
        {
            D6[p] = o;
            break;
        }
        default:
        {
            D7[p] = o;
            break;
        }
    }
}

NrGrade nr_grade_of(uint style)
{
    NrGrade g = _182;
    if (style == 1u)
    {
        g.exposure = -0.100000001490116119384765625f;
        g.contrast = -0.25f;
        g.saturation = -0.100000001490116119384765625f;
    }
    if (style == 2u)
    {
        g.saturation = -0.1500000059604644775390625f;
    }
    return g;
}

float3 nr_grade_tint(float3 c, float a, float hue_pos, float hue_neg, bool warm_first)
{
    float m = abs(a);
    if (m < 9.9999999747524270787835121154785e-07f)
    {
        return c;
    }
    float mx = max(max(c.x, c.y), c.z);
    float mn = min(min(c.x, c.y), c.z);
    float L = (mx + mn) * 0.5f;
    float _262;
    if (L < 0.5f)
    {
        _262 = L + L;
    }
    else
    {
        _262 = (L + 1.0f) - L;
    }
    float hi = _262;
    float lo = (L + L) - hi;
    float3 t;
    if (warm_first)
    {
        if (a > 0.0f)
        {
            t = float3(hi, mad((hi - lo) * 6.0f, hue_pos, lo), lo);
        }
        else
        {
            t = float3(lo, mad((hi - lo) * hue_neg, 6.0f, lo), hi);
        }
    }
    else
    {
        if (a > 0.0f)
        {
            t = float3(mad((hi - lo) * hue_pos, 6.0f, lo), hi, lo);
        }
        else
        {
            t = float3(mad((hi - lo) * 6.0f, hue_neg, lo), lo, hi);
        }
    }
    return clamp(mad(m.xxx, t - c, c), 0.0f.xxx, 1.0f.xxx);
}

float nr_grade_sat(float x)
{
    return clamp(x, 0.0f, 1.0f);
}

float nr_grade_contrast(inout float x, float k)
{
    float param = x;
    x = nr_grade_sat(param);
    return mad(k, ((x * x) * (3.0f - (x + x))) - x, x);
}

float nr_grade_smooth(inout float t)
{
    float param = t;
    t = nr_grade_sat(param);
    return (t * t) * (3.0f - (t + t));
}

float nr_grade_bands(float y, NrGrade g)
{
    float param = y;
    float x = nr_grade_sat(param);
    float param_1 = (x - 0.25f) / (-0.25f);
    float _590 = nr_grade_smooth(param_1);
    float w0 = _590;
    float param_2 = x / 0.25f;
    float _595 = nr_grade_smooth(param_2);
    float param_3 = (x - 0.5f) / (-0.25f);
    float _600 = nr_grade_smooth(param_3);
    float w1 = _595 * _600;
    float param_4 = (x - 0.25f) / 0.25f;
    float _607 = nr_grade_smooth(param_4);
    float param_5 = (x - 0.75f) / (-0.25f);
    float _613 = nr_grade_smooth(param_5);
    float w2 = _607 * _613;
    float param_6 = (x - 0.5f) / 0.25f;
    float _620 = nr_grade_smooth(param_6);
    float param_7 = (x - 1.0f) / (-0.25f);
    float _625 = nr_grade_smooth(param_7);
    float w3 = _620 * _625;
    float param_8 = (x - 0.75f) / 0.25f;
    float _632 = nr_grade_smooth(param_8);
    float w4 = _632;
    float tot = mad(w4, g.band4, mad(w3, g.band3, mad(w2, g.band2, mad(w0, g.band0, w1 * g.band1))));
    return exp2(log2(x) * exp2(-tot));
}

float3 nr_grade_hsl(float3 c)
{
    float mx = max(max(c.x, c.y), c.z);
    float mn = min(min(c.x, c.y), c.z);
    float sum = mx + mn;
    float L = sum * 0.5f;
    float h = 0.0f;
    float s = 0.0f;
    if (mx > mn)
    {
        float d = mx - mn;
        float _386;
        if (L > 0.5f)
        {
            _386 = d / ((2.0f - mx) - mn);
        }
        else
        {
            _386 = d / sum;
        }
        s = _386;
        if (mx == c.x)
        {
            h = (((c.y < c.z) ? 6.0f : 0.0f) + ((c.y - c.z) / d)) / 6.0f;
        }
        else
        {
            if (mx == c.y)
            {
                h = (((c.z - c.x) / d) + 2.0f) / 6.0f;
            }
            else
            {
                h = (((c.x - c.y) / d) + 4.0f) / 6.0f;
            }
        }
    }
    return float3(h, s, L);
}

float nr_grade_hue(inout float t, float hi, float lo)
{
    if (t < 0.0f)
    {
        t += 1.0f;
    }
    if (t > 1.0f)
    {
        t -= 1.0f;
    }
    if (t < 0.16666667163372039794921875f)
    {
        return mad(t, (hi - lo) * 6.0f, lo);
    }
    if (t < 0.5f)
    {
        return hi;
    }
    if (t < 0.666666686534881591796875f)
    {
        return mad((0.666666686534881591796875f - t) * (hi - lo), 6.0f, lo);
    }
    return lo;
}

float3 nr_grade_rgb(float h, float s, float L)
{
    if (s <= 0.0f)
    {
        return L.xxx;
    }
    float _513;
    if (L < 0.5f)
    {
        _513 = L * (s + 1.0f);
    }
    else
    {
        _513 = (L + s) - (L * s);
    }
    float hi = _513;
    float lo = (L + L) - hi;
    float param = h + 0.3333333432674407958984375f;
    float param_1 = hi;
    float param_2 = lo;
    float _543 = nr_grade_hue(param, param_1, param_2);
    float param_3 = h;
    float param_4 = hi;
    float param_5 = lo;
    float _550 = nr_grade_hue(param_3, param_4, param_5);
    float param_6 = h - 0.3333333432674407958984375f;
    float param_7 = hi;
    float param_8 = lo;
    float _558 = nr_grade_hue(param_6, param_7, param_8);
    return float3(_543, _550, _558);
}

float3 nr_style_grade(uint style, float3 base, float3 rgb, float intensity)
{
    uint param = style;
    NrGrade g = nr_grade_of(param);
    float3 c = clamp(rgb, 0.0f.xxx, 1.0f.xxx);
    c = clamp((c - g.black.xxx) / ((g.white - g.black) + 1.0000000133514319600180897396058e-10f).xxx, 0.0f.xxx, 1.0f.xxx);
    float3 param_1 = c;
    float param_2 = g.tint1;
    float param_3 = asfloat(1031425659u);
    float param_4 = asfloat(1037577752u);
    bool param_5 = true;
    c = nr_grade_tint(param_1, param_2, param_3, param_4, param_5);
    float3 param_6 = c;
    float param_7 = g.tint2;
    float param_8 = asfloat(1018564032u);
    float param_9 = asfloat(1041492216u);
    bool param_10 = false;
    c = nr_grade_tint(param_6, param_7, param_8, param_9, param_10);
    c *= exp2(g.exposure);
    float param_11 = c.x;
    float param_12 = g.contrast;
    float _733 = nr_grade_contrast(param_11, param_12);
    float param_13 = c.y;
    float param_14 = g.contrast;
    float _740 = nr_grade_contrast(param_13, param_14);
    float param_15 = c.z;
    float param_16 = g.contrast;
    float _747 = nr_grade_contrast(param_15, param_16);
    c = float3(_733, _740, _747);
    float param_17 = c.x;
    NrGrade param_18 = g;
    float param_19 = c.y;
    NrGrade param_20 = g;
    float param_21 = c.z;
    NrGrade param_22 = g;
    c = float3(nr_grade_bands(param_17, param_18), nr_grade_bands(param_19, param_20), nr_grade_bands(param_21, param_22));
    float gp = exp2(-g.gamma);
    c = exp2(log2(max(c, 0.0f.xxx)) * gp);
    float3 param_23 = c;
    float3 hsl = nr_grade_hsl(param_23);
    float param_24 = hsl.y * (g.saturation + 1.0f);
    float param_25 = hsl.x;
    float param_26 = nr_grade_sat(param_24);
    float param_27 = hsl.z;
    c = nr_grade_rgb(param_25, param_26, param_27);
    float3 param_28 = c;
    hsl = nr_grade_hsl(param_28);
    float param_29 = exp2(exp2(-g.sat_power) * log2(hsl.y));
    float param_30 = hsl.x;
    float param_31 = nr_grade_sat(param_29);
    float param_32 = hsl.z;
    c = nr_grade_rgb(param_30, param_31, param_32);
    c = clamp(c, 0.0f.xxx, 1.0f.xxx);
    float param_33 = intensity;
    return clamp(mad(nr_grade_sat(param_33).xxx, c - base, base), 0.0f.xxx, 1.0f.xxx);
}

float3 post(uint k, float3 base, inout float3 rgb)
{
    if ((pc_flags & 2048u) == 0u)
    {
        return rgb;
    }
    uint kk = min(k, 3u);
    uint st_1 = (pc_styles >> (2u * kk)) & 3u;
    float I = pc_inten[kk];
    rgb = clamp(mad(min(I, 1.0f).xxx, rgb - base, base), 0.0f.xxx, 1.0f.xxx);
    if (I > 1.0f)
    {
        float ratio = (dot(rgb, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f)) + 0.001953125f) / (dot(base, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f)) + 0.001953125f);
        float amp = clamp(pow(max(ratio, 9.9999999747524270787835121154785e-07f), I), 0.5f, 2.0f);
        rgb = clamp(rgb * (amp / max(ratio, 9.9999999747524270787835121154785e-07f)), 0.0f.xxx, 1.0f.xxx);
    }
    if (st_1 != 0u)
    {
        uint param = st_1;
        float3 param_1 = base;
        float3 param_2 = rgb;
        float param_3 = I;
        rgb = nr_style_grade(param, param_1, param_2, param_3);
    }
    return rgb;
}

float4 src(uint i, int2 c)
{
    switch (i)
    {
        case 0u:
        {
            return S0.Load(int3(c, 0));
        }
        case 1u:
        {
            return S1.Load(int3(c, 0));
        }
        case 2u:
        {
            return S2.Load(int3(c, 0));
        }
        case 3u:
        {
            return S3.Load(int3(c, 0));
        }
        case 4u:
        {
            return S4.Load(int3(c, 0));
        }
        case 5u:
        {
            return S5.Load(int3(c, 0));
        }
        case 6u:
        {
            return S6.Load(int3(c, 0));
        }
        default:
        {
            return S7.Load(int3(c, 0));
        }
    }
}

float3 prevb(float2 p)
{
    float2 f = p - 0.5f.xx;
    float2 b0 = floor(f);
    float2 w = f - b0;
    int2 i0 = int2(b0);
    int2 hi = int2(pc_size) - int2(1, 1);
    float3 a = BasePrev.Load(int3(clamp(i0, int2(0, 0), hi), 0)).xyz;
    float3 b = BasePrev.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), hi), 0)).xyz;
    float3 c = BasePrev.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), hi), 0)).xyz;
    float3 d = BasePrev.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), hi), 0)).xyz;
    return lerp(lerp(a, b, w.x.xxx), lerp(c, d, w.x.xxx), w.y.xxx);
}

float2 mvprev(float2 p)
{
    float2 f = p - 0.5f.xx;
    float2 b0 = floor(f);
    float2 w = f - b0;
    int2 i0 = int2(b0);
    int2 hi = int2(pc_size) - int2(1, 1);
    float2 a = MvPrev.Load(int3(clamp(i0, int2(0, 0), hi), 0)).xy;
    float2 b = MvPrev.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), hi), 0)).xy;
    float2 c = MvPrev.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), hi), 0)).xy;
    float2 d = MvPrev.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), hi), 0)).xy;
    return lerp(lerp(a, b, w.x.xx), lerp(c, d, w.x.xx), w.y.xx);
}

float2 grad_v(int2 c, float2 d)
{
    int2 lo = int2(pc_mbase);
    int2 hi = int2(pc_mbase + pc_mgrid) - int2(1, 1);
    float2 _step = (float2(pc_size) * 2.0f) / float2(pc_mgrid);
    float2 mx = ((Motion.Load(int3(clamp(c + int2(2, 0), lo, hi), 0)).xy - Motion.Load(int3(clamp(c - int2(2, 0), lo, hi), 0)).xy) * pc_mscale) / (2.0f * _step.x).xx;
    float2 my = ((Motion.Load(int3(clamp(c + int2(0, 2), lo, hi), 0)).xy - Motion.Load(int3(clamp(c - int2(0, 2), lo, hi), 0)).xy) * pc_mscale) / (2.0f * _step.y).xx;
    return (mx * d.x) + (my * d.y);
}

float2 clampl(float2 x, float m)
{
    float l = length(x);
    float2 _1912;
    if (l > m)
    {
        _1912 = x * (m / l);
    }
    else
    {
        _1912 = x;
    }
    return _1912;
}

float3 bil(uint i, float2 p)
{
    float2 f = p - 0.5f.xx;
    float2 b0 = floor(f);
    float2 w = f - b0;
    int2 i0 = int2(b0);
    int2 hi = int2(pc_size) - int2(1, 1);
    uint param = i;
    int2 param_1 = clamp(i0, int2(0, 0), hi);
    float3 a = src(param, param_1).xyz;
    uint param_2 = i;
    int2 param_3 = clamp(i0 + int2(1, 0), int2(0, 0), hi);
    float3 b = src(param_2, param_3).xyz;
    uint param_4 = i;
    int2 param_5 = clamp(i0 + int2(0, 1), int2(0, 0), hi);
    float3 c = src(param_4, param_5).xyz;
    uint param_6 = i;
    int2 param_7 = clamp(i0 + int2(1, 1), int2(0, 0), hi);
    float3 d = src(param_6, param_7).xyz;
    return lerp(lerp(a, b, w.x.xxx), lerp(c, d, w.x.xxx), w.y.xxx);
}

void body(uint2 px)
{
    int2 ip = int2(px);
    uint _2237;
    if (_2236)
    {
        _2237 = kStep;
    }
    else
    {
        _2237 = pc_mode;
    }
    if (_2237 == 3u)
    {
        Base[ip] = Proxy[ip];
        return;
    }
    uint _2256;
    if (_2255)
    {
        _2256 = kStep;
    }
    else
    {
        _2256 = pc_mode;
    }
    if (_2256 == 5u)
    {
        uint2 mt = pc_mbase + min(uint2(((float2(px) + 0.5f.xx) * float2(pc_mgrid)) / float2(pc_size)), (pc_mgrid - uint2(1u, 1u)));
        uint2 param = mt;
        DepthCur[ip] = near_at(param).x;
        return;
    }
    uint _2296;
    if (_2295)
    {
        _2296 = kStep;
    }
    else
    {
        _2296 = pc_mode;
    }
    if (_2296 == 4u)
    {
        float trust = Trust[ip].xxxx.x;
        if (trust >= 1.0f)
        {
            return;
        }
        bool bil_1 = (pc_flags & 67108864u) != 0u;
        float4 wl[6];
        if (bil_1)
        {
            float near = DepthCur[ip].xxxx.x;
            for (uint l = 1u; l <= 6u; l++)
            {
                uint param_1 = l;
                float2 param_2 = float2(px) + 0.5f.xx;
                float param_3 = near;
                wl[l - 1u] = lvw(param_1, param_2, param_3);
            }
        }
        bool keep = (pc_flags & 268435456u) != 0u;
        float3 _2362;
        if (keep)
        {
            _2362 = Proxy[ip].xyz;
        }
        else
        {
            _2362 = Base[ip].xyz;
        }
        float3 z = _2362;
        for (uint i = 0u; i < pc_n; i++)
        {
            uint param_4 = i;
            float2 param_5 = float2(px) + 0.5f.xx;
            bool param_6 = bil_1;
            uint param_7 = i;
            int2 param_8 = ip;
            float3 v = lerp(pull(param_4, param_5, param_6, wl), ld(param_7, param_8), trust.xxx);
            uint param_9 = i;
            int2 param_10 = ip;
            float3 param_11 = v;
            st(param_9, param_10, param_11);
            if (i < pc_slot)
            {
                uint param_12 = i;
                float3 param_13 = z;
                float3 param_14 = z + v;
                float3 _2426 = post(param_12, param_13, param_14);
                z = _2426;
            }
        }
        if (keep)
        {
            Base[ip] = float4(clamp(z, 0.0f.xxx, 1.0f.xxx), Proxy[ip].w);
        }
        else
        {
            Proxy[ip] = float4(clamp(z, 0.0f.xxx, 1.0f.xxx), Base[ip].w);
        }
        return;
    }
    uint _2463;
    if (_2462)
    {
        _2463 = kStep;
    }
    else
    {
        _2463 = pc_mode;
    }
    if (_2463 == 0u)
    {
        bool keep_1 = (pc_flags & 268435456u) != 0u;
        float4 _2480;
        if (keep_1)
        {
            _2480 = Proxy[ip];
        }
        else
        {
            _2480 = Base[ip];
        }
        float4 b = _2480;
        if ((pc_flags & 2u) != 0u)
        {
            float3 h = BasePrev.Load(int3(ip, 0)).xyz;
            uint i_1 = 0u;
            for (;;)
            {
                bool _2511 = i_1 < pc_slot;
                bool _2518;
                if (_2511)
                {
                    _2518 = i_1 < pc_n;
                }
                else
                {
                    _2518 = _2511;
                }
                if (_2518)
                {
                    uint param_15 = i_1;
                    int2 param_16 = ip;
                    uint param_17 = i_1;
                    float3 param_18 = h;
                    float3 param_19 = h + src(param_15, param_16).xyz;
                    float3 _2532 = post(param_17, param_18, param_19);
                    h = _2532;
                    i_1++;
                    continue;
                }
                else
                {
                    break;
                }
            }
            if (pc_slot < pc_n)
            {
                uint param_20 = pc_slot;
                int2 param_21 = ip;
                h += src(param_20, param_21).xyz;
            }
            Hist[ip] = float4(clamp(h, 0.0f.xxx, 1.0f.xxx), 1.0f);
        }
        float2 q = float2(px) + 0.5f.xx;
        bool inside = true;
        if ((pc_flags & 1u) != 0u)
        {
            uint2 t = pc_mbase + min(uint2(((float2(px) + 0.5f.xx) * float2(pc_mgrid)) / float2(pc_size)), (pc_mgrid - uint2(1u, 1u)));
            if ((pc_flags & 256u) != 0u)
            {
                uint2 best = t;
                uint2 param_22 = t;
                float bn = near_at(param_22);
                for (int y = -1; y <= 1; y++)
                {
                    for (int x = -1; x <= 1; x++)
                    {
                        uint2 c = uint2(clamp(int2(t) + int2(x, y), int2(pc_mbase), int2(pc_mbase + pc_mgrid) - int2(1, 1)));
                        uint2 param_23 = c;
                        float nn = near_at(param_23);
                        if (nn > bn)
                        {
                            bn = nn;
                            best = c;
                        }
                    }
                }
                t = best;
            }
            q += (Motion.Load(int3(int2(t), 0)).xy * pc_mscale);
            bool _2673 = all(bool2(q.x >= 0.0f.xx.x, q.y >= 0.0f.xx.y));
            bool _2682;
            if (_2673)
            {
                float2 _2679 = float2(pc_size);
                _2682 = all(bool2(q.x <= _2679.x, q.y <= _2679.y));
            }
            else
            {
                _2682 = _2673;
            }
            inside = _2682;
        }
        float stay = 0.0f;
        float trust_1 = 1.0f;
        float t_v = 1.0f;
        float t_b = 1.0f;
        float t_d = 1.0f;
        float3 lo = 1000000000.0f.xxx;
        float3 hi = (-1000000000.0f).xxx;
        float3 pq = 0.0f.xxx;
        if ((pc_flags & 1u) != 0u)
        {
            float l_1 = 0.0f;
            float em = 0.0f;
            float es = 0.0f;
            float3 _2734;
            for (int y_1 = -1; y_1 <= 1; y_1++)
            {
                for (int x_1 = -1; x_1 <= 1; x_1++)
                {
                    int2 c_1 = clamp(ip + int2(x_1, y_1), int2(0, 0), int2(pc_size) - int2(1, 1));
                    if (keep_1)
                    {
                        _2734 = Proxy[c_1].xyz;
                    }
                    else
                    {
                        _2734 = Base[c_1].xyz;
                    }
                    float3 cb = _2734;
                    float2 param_24 = q + float2(float(x_1), float(y_1));
                    float3 pm = prevb(param_24);
                    l_1 += dot(cb, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
                    em += dot(pm, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
                    es += dot(BasePrev.Load(int3(c_1, 0)).xyz, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
                    lo = min(lo, cb);
                    hi = max(hi, cb);
                    if ((x_1 == 0) && (y_1 == 0))
                    {
                        pq = pm;
                    }
                }
            }
            em = abs(l_1 - em) / 9.0f;
            es = abs(l_1 - es) / 9.0f;
            stay = smoothstep(0.0f, 0.02999999932944774627685546875f, em - es);
            if (stay > 0.0f)
            {
                inside = true;
            }
            if ((pc_flags & 4u) != 0u)
            {
                trust_1 = 1.0f - smoothstep(0.014999999664723873138427734375f, 0.0500000007450580596923828125f, min(em, es));
            }
            if ((pc_flags & 8u) != 0u)
            {
                float2 v_1 = q - (float2(px) + 0.5f.xx);
                float2 qc = clamp(q, 0.0f.xx, float2(pc_size) - 0.001000000047497451305389404296875f.xx);
                uint2 t2 = pc_mbase + min(uint2((qc * float2(pc_mgrid)) / float2(pc_size)), (pc_mgrid - uint2(1u, 1u)));
                bool vprev = (pc_flags & 8388608u) != 0u;
                bool vboth = (pc_flags & 16777216u) != 0u;
                float2 _2878;
                if (vprev)
                {
                    float2 param_25 = q;
                    _2878 = mvprev(param_25);
                }
                else
                {
                    _2878 = Motion.Load(int3(int2(t2), 0)).xy * pc_mscale;
                }
                float2 vq = _2878;
                float dv = length(vq - v_1);
                if (vboth)
                {
                    float2 param_26 = q;
                    dv = min(dv, length(mvprev(param_26) - v_1));
                }
                float lv_1 = length(v_1);
                if ((pc_flags & 524288u) != 0u)
                {
                    int2 tc = int2(pc_mbase + min(uint2(((float2(px) + 0.5f.xx) * float2(pc_mgrid)) / float2(pc_size)), (pc_mgrid - uint2(1u, 1u))));
                    int2 param_27 = tc;
                    float2 param_28 = v_1;
                    float2 gp = grad_v(param_27, param_28);
                    float cmax = 0.5f * lv_1;
                    float2 param_29 = gp;
                    float param_30 = cmax;
                    dv = min(dv, length((vq - v_1) - clampl(param_29, param_30)));
                    if ((pc_flags & 1048576u) != 0u)
                    {
                        int2 param_31 = int2(t2);
                        float2 param_32 = v_1;
                        float2 gq = grad_v(param_31, param_32);
                        float2 param_33 = gq;
                        float param_34 = cmax;
                        float2 param_35 = (gp + gq) * 0.5f;
                        float param_36 = cmax;
                        dv = min(dv, min(length((vq - v_1) - clampl(param_33, param_34)), length((vq - v_1) - clampl(param_35, param_36))));
                    }
                }
                t_v = lerp(1.0f - smoothstep(0.5f + (0.0500000007450580596923828125f * lv_1), 2.0f + (0.1500000059604644775390625f * lv_1), dv), 1.0f, stay);
                trust_1 = min(trust_1, t_v);
            }
        }
        if ((pc_flags & 129u) == 129u)
        {
            float3 pv = lerp(pq, BasePrev.Load(int3(ip, 0)).xyz, stay.xxx);
            float3 o = max(pv - hi, lo - pv);
            t_b = 1.0f - smoothstep(pc_k0, pc_k1, max(o.x, max(o.y, o.z)));
            trust_1 = min(trust_1, t_b);
        }
        bool _3064 = (pc_flags & 512u) != 0u;
        bool _3071;
        if (_3064)
        {
            _3071 = (pc_flags & 1u) != 0u;
        }
        else
        {
            _3071 = _3064;
        }
        if (_3071 && inside)
        {
            int2 b0 = int2(floor(q - 0.5f.xx));
            int2 hi_1 = int2(pc_size) - int2(1, 1);
            float np = 0.0f;
            for (int y_2 = 0; y_2 <= 1; y_2++)
            {
                for (int x_2 = 0; x_2 <= 1; x_2++)
                {
                    np = max(np, DepthPrev.Load(int3(clamp(b0 + int2(x_2, y_2), int2(0, 0), hi_1), 0)).x);
                }
            }
            float nc = DepthCur[ip].xxxx.x;
            float r = (np - nc) / max(nc, 1.0000000116860974230803549289703e-07f);
            t_d = lerp(1.0f - smoothstep(0.0500000007450580596923828125f, 0.20000000298023223876953125f, r), 1.0f, stay);
            trust_1 = min(trust_1, t_d);
        }
        if (((pc_flags & 33554433u) == 33554433u) && (!inside))
        {
            trust_1 = 0.0f;
        }
        if ((pc_flags & 652u) != 0u)
        {
            Trust[ip] = trust_1.x;
        }
        if ((pc_flags & 2097152u) != 0u)
        {
            TrustDbg[ip] = float4(trust_1, t_v, t_b, t_d);
        }
        float3 z_1 = b.xyz;
        float3 _3194;
        float3 _3202;
        for (uint i_2 = 0u; i_2 < pc_n; i_2++)
        {
            if (!inside)
            {
                _3194 = 0.0f.xxx;
            }
            else
            {
                if ((pc_flags & 1u) != 0u)
                {
                    uint param_37 = i_2;
                    float2 param_38 = q;
                    _3202 = bil(param_37, param_38);
                }
                else
                {
                    uint param_39 = i_2;
                    int2 param_40 = ip;
                    _3202 = src(param_39, param_40).xyz;
                }
                _3194 = _3202;
            }
            float3 v_2 = _3194;
            if (stay > 0.0f)
            {
                uint param_41 = i_2;
                int2 param_42 = ip;
                v_2 = lerp(v_2, src(param_41, param_42).xyz, stay.xxx);
            }
            uint param_43 = i_2;
            int2 param_44 = ip;
            float3 param_45 = v_2;
            st(param_43, param_44, param_45);
            if (i_2 < pc_slot)
            {
                uint param_46 = i_2;
                float3 param_47 = z_1;
                float3 param_48 = z_1 + (v_2 * trust_1);
                float3 _3256 = post(param_46, param_47, param_48);
                z_1 = _3256;
            }
        }
        if (keep_1)
        {
            Base[ip] = float4(clamp(z_1, 0.0f.xxx, 1.0f.xxx), b.w);
        }
        else
        {
            Proxy[ip] = float4(clamp(z_1, 0.0f.xxx, 1.0f.xxx), b.w);
        }
    }
    else
    {
        bool keep_2 = (pc_flags & 268435456u) != 0u;
        float4 a = Answer[ip];
        float4 _3300;
        if (keep_2)
        {
            _3300 = Base[ip];
        }
        else
        {
            _3300 = Proxy[ip];
        }
        float4 zin = _3300;
        float4 _3313;
        if (keep_2)
        {
            _3313 = Proxy[ip];
        }
        else
        {
            _3313 = Base[ip];
        }
        float4 b_1 = _3313;
        float3 fresh = a.xyz - zin.xyz;
        bool _3335 = (pc_flags & 65536u) != 0u;
        bool _3343;
        if (_3335)
        {
            _3343 = (pc_flags & 64u) == 0u;
        }
        else
        {
            _3343 = _3335;
        }
        float _3344;
        if (_3343)
        {
            _3344 = Trust[ip].xxxx.x;
        }
        else
        {
            _3344 = 1.0f;
        }
        float trust_2 = _3344;
        if ((pc_flags & 32u) != 0u)
        {
            uint param_49 = pc_slot;
            int2 param_50 = ip;
            float3 old = ld(param_49, param_50);
            float dl = length(fresh - old);
            float r_1 = dl / (max(length(fresh), length(old)) + 0.0040000001899898052215576171875f);
            trust_2 = min(trust_2, 1.0f - (smoothstep(pc_k0, pc_k1, r_1) * smoothstep(0.006000000052154064178466796875f, 0.0199999995529651641845703125f, dl)));
        }
        uint param_51 = pc_slot;
        int2 param_52 = ip;
        float3 param_53 = fresh;
        st(param_51, param_52, param_53);
        float3 o_1 = b_1.xyz;
        float3 _3422;
        for (uint i_3 = 0u; i_3 < pc_n; i_3++)
        {
            if (i_3 == pc_slot)
            {
                _3422 = fresh;
            }
            else
            {
                uint param_54 = i_3;
                int2 param_55 = ip;
                _3422 = ld(param_54, param_55);
            }
            float3 v_3 = _3422;
            if ((i_3 != pc_slot) && (trust_2 < 1.0f))
            {
                v_3 = lerp(fresh, v_3, trust_2.xxx);
                uint param_56 = i_3;
                int2 param_57 = ip;
                float3 param_58 = v_3;
                st(param_56, param_57, param_58);
            }
            uint param_59 = i_3;
            float3 param_60 = o_1;
            float3 param_61 = o_1 + v_3;
            float3 _3462 = post(param_59, param_60, param_61);
            o_1 = _3462;
        }
        if ((pc_flags & 4194304u) != 0u)
        {
            float n = DepthCur[ip].xxxx.x;
            o_1 = clamp(1.0f + (log2(max(n, 9.9999999747524270787835121154785e-07f)) / 12.0f), 0.0f, 1.0f).xxx;
        }
        else
        {
            if ((pc_flags & 2097152u) != 0u)
            {
                float4 t_1 = TrustDbg[ip];
                o_1 = lerp(o_1, float3(1.0f - t_1.y, 1.0f - t_1.z, 1.0f - t_1.w), (0.699999988079071044921875f * (1.0f - min(t_1.y, min(t_1.z, t_1.w)))).xxx);
            }
            else
            {
                if ((pc_flags & 262144u) != 0u)
                {
                    o_1 = lerp(o_1, float3(1.0f, 0.0f, 0.0f), (0.699999988079071044921875f * (1.0f - Trust[ip].xxxx.x)).xxx);
                }
            }
        }
        Answer[ip] = float4(clamp(o_1, 0.0f.xxx, 1.0f.xxx), a.w);
        if (keep_2)
        {
            Base[ip] = b_1;
        }
        else
        {
            Proxy[ip] = b_1;
        }
    }
}

void pyr1(uint2 px, bool live)
{
    int2 ip = int2(px);
    uint2 l = gl_LocalInvocationID.xy;
    uint li = gl_LocalInvocationIndex;
    bool _1946;
    if (live)
    {
        _1946 = (l.x & 1u) == 0u;
    }
    else
    {
        _1946 = live;
    }
    bool _1953;
    if (_1946)
    {
        _1953 = (l.y & 1u) == 0u;
    }
    else
    {
        _1953 = _1946;
    }
    bool lead = _1953;
    bool depth = (pc_flags & 1073741824u) != 0u;
    uint dw = (pc_size.x + 1u) / 2u;
    float _1967;
    if (live)
    {
        _1967 = Trust[ip].xxxx.x;
    }
    else
    {
        _1967 = 0.0f;
    }
    float w = _1967;
    float4 _1995;
    float2 _2025;
    for (uint i = 0u; i < pc_n; i++)
    {
        if (live)
        {
            uint param = i;
            int2 param_1 = ip;
            _1995 = float4(ld(param, param_1) * w, w);
        }
        else
        {
            _1995 = 0.0f.xxxx;
        }
        s_pp[li] = _1995;
        if ((i == 0u) && depth)
        {
            if (live)
            {
                _2025 = float2(DepthCur[ip].xxxx.x * w, w);
            }
            else
            {
                _2025 = 0.0f.xx;
            }
            s_dp[li] = _2025;
        }
        GroupMemoryBarrierWithGroupSync();
        if (lead)
        {
            float4 s = 0.0f.xxxx;
            float2 dn = 0.0f.xx;
            s += s_pp[li];
            if ((i == 0u) && depth)
            {
                dn += s_dp[li];
            }
            if ((px.x + 1u) < pc_size.x)
            {
                s += s_pp[li + 1u];
                if ((i == 0u) && depth)
                {
                    dn += s_dp[li + 1u];
                }
            }
            if ((px.y + 1u) < pc_size.y)
            {
                s += s_pp[li + 16u];
                if ((i == 0u) && depth)
                {
                    dn += s_dp[li + 16u];
                }
                if ((px.x + 1u) < pc_size.x)
                {
                    s += s_pp[li + 17u];
                    if ((i == 0u) && depth)
                    {
                        dn += s_dp[li + 17u];
                    }
                }
            }
            int2 d = ip / int2(2, 2);
            L1Out[int2(int(uint(d.x) + (i * dw)), d.y)] = s * 0.25f;
            if ((i == 0u) && depth)
            {
                DP1Out[d] = float4(dn * 0.25f, 0.0f, 0.0f).xy;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

void comp_main()
{
    uint2 px = gl_GlobalInvocationID.xy;
    bool live = all(bool2(px.x < pc_size.x, px.y < pc_size.y));
    uint _2197;
    if (_2196)
    {
        _2197 = kStep;
    }
    else
    {
        _2197 = pc_mode;
    }
    bool _2204 = _2197 == 0u;
    bool _2212;
    if (_2204)
    {
        _2212 = (pc_flags & 536870912u) != 0u;
    }
    else
    {
        _2212 = _2204;
    }
    if (_2212)
    {
        if (live)
        {
            uint2 param = px;
            body(param);
        }
        uint2 param_1 = px;
        bool param_2 = live;
        pyr1(param_1, param_2);
        return;
    }
    if (live)
    {
        uint2 param_3 = px;
        body(param_3);
    }
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    gl_LocalInvocationIndex = stage_input.gl_LocalInvocationIndex;
    comp_main();
}
