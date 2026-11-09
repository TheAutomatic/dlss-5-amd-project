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
static const bool _3200 = (kStep != 65535u);
static const bool _3217 = (kStep != 65535u);
static const bool _3257 = (kStep != 65535u);
static const bool _3274 = (kStep != 65535u);
static const bool _3324 = (kStep != 65535u);
static const bool _3496 = (kStep != 65535u);

static const NrGrade _206 = { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

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
RWTexture2D<float4> StableCur : register(u47);
RWTexture2D<float4> Base : register(u2);
RWTexture2D<float4> Proxy : register(u0);
RWTexture2D<float4> Answer : register(u1);
Texture2D<float4> StablePrev : register(t46);
RWTexture2D<float> DepthCur : register(u30);
Texture2D<float4> DepthPrev : register(t31);
RWTexture2D<float> Trust : register(u22);
RWTexture2D<float4> L1Out : register(u44);
RWTexture2D<float2> DP1Out : register(u45);
RWTexture2D<float4> Hist : register(u20);
RWTexture2D<float4> TrustDbg : register(u43);

static uint3 gl_LocalInvocationID;
static uint3 gl_GlobalInvocationID;
static uint gl_LocalInvocationIndex;
struct SPIRV_Cross_Input
{
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
    uint gl_LocalInvocationIndex : SV_GroupIndex;
};

static float g_prev_alpha;
groupshared float4 s_c17jump[256];
groupshared float2 s_c17stay[256];
groupshared float4 s_pp[256];
groupshared float2 s_dp[256];

float4 ld4(uint i, int2 p)
{
    switch (i)
    {
        case 0u:
        {
            return D0[p];
        }
        case 1u:
        {
            return D1[p];
        }
        case 2u:
        {
            return D2[p];
        }
        case 3u:
        {
            return D3[p];
        }
        case 4u:
        {
            return D4[p];
        }
        case 5u:
        {
            return D5[p];
        }
        case 6u:
        {
            return D6[p];
        }
        default:
        {
            return D7[p];
        }
    }
}

void stable_reduce(uint2 px, bool live)
{
    uint li = gl_LocalInvocationIndex;
    uint2 loc = gl_LocalInvocationID.xy;
    float4 j = 0.0f.xxxx;
    float2 st = 0.0f.xx;
    bool _2242;
    if (live)
    {
        _2242 = (loc.y & 3u) == 0u;
    }
    else
    {
        _2242 = live;
    }
    if (_2242)
    {
        int2 ip = int2(px);
        uint param = pc_slot;
        int2 param_1 = ip;
        float4 old = ld4(param, param_1);
        float3 _2261;
        if ((pc_flags & 268435456u) != 0u)
        {
            _2261 = Base[ip].xyz;
        }
        else
        {
            _2261 = Proxy[ip].xyz;
        }
        float3 input_rgb = _2261;
        float3 fresh = Answer[ip].xyz - input_rgb;
        float weight = 1.0f;
        j = float4(old.xyz - fresh, 1.0f) * weight;
        st = float2(float(int(old.w > 0.5f)), 1.0f);
    }
    s_c17jump[li] = j;
    s_c17stay[li] = st;
    GroupMemoryBarrierWithGroupSync();
    bool _2324;
    if (live)
    {
        _2324 = (loc.x & 7u) == 0u;
    }
    else
    {
        _2324 = live;
    }
    bool _2331;
    if (_2324)
    {
        _2331 = (loc.y & 7u) == 0u;
    }
    else
    {
        _2331 = _2324;
    }
    if (_2331)
    {
        float4 qa = 0.0f.xxxx;
        float2 ta = 0.0f.xx;
        uint2 bo = px;
        for (uint y = 0u; y < 8u; y += 4u)
        {
            for (uint x = 0u; x < 8u; x++)
            {
                qa += s_c17jump[(li + (y * 16u)) + x];
                ta += s_c17stay[(li + (y * 16u)) + x];
            }
        }
        float3 _2384;
        if (qa.w > 0.001000000047497451305389404296875f)
        {
            _2384 = qa.xyz / qa.w.xxx;
        }
        else
        {
            _2384 = 0.0f.xxx;
        }
        float3 j_1 = _2384;
        j_1 *= smoothstep(0.25f, 0.75f, qa.w / 16.0f);
        int2 bc = min((int2(bo) + int2(4, 4)), (int2(pc_size) - int2(1, 1)));
        int2 g8 = (int2(pc_size) + int2(7, 7)) / int2(8, 8);
        uint2 mt = pc_mbase + min(uint2(((float2(bc) + 0.5f.xx) * float2(pc_mgrid)) / float2(pc_size)), (pc_mgrid - uint2(1u, 1u)));
        float2 mv = (Motion.Load(int3(int2(mt), 0)).xy * (1.0f - (ta.x / max(ta.y, 1.0f)))) * pc_mscale;
        float2 q8 = ((float2(bc) + 0.5f.xx) + mv) / 8.0f.xx;
        float3 cp = 0.0f.xxx;
        float jw = 0.0f;
        float jy = 0.0f;
        float ap = 1.0f;
        bool _2476 = (pc_flags & 16u) != 0u;
        bool _2483;
        if (_2476)
        {
            _2483 = all(bool2(q8.x >= 0.0f.xx.x, q8.y >= 0.0f.xx.y));
        }
        else
        {
            _2483 = _2476;
        }
        bool _2491;
        if (_2483)
        {
            float2 _2488 = float2(g8);
            _2491 = all(bool2(q8.x <= _2488.x, q8.y <= _2488.y));
        }
        else
        {
            _2491 = _2483;
        }
        if (_2491)
        {
            float2 f = q8 - 0.5f.xx;
            float2 b0 = floor(f);
            float2 w = f - b0;
            int2 i0 = int2(b0);
            cp = lerp(lerp(StablePrev.Load(int3(clamp(i0, int2(0, 0), g8 - int2(1, 1)), 0)).xyz, StablePrev.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), g8 - int2(1, 1)), 0)).xyz, w.x.xxx), lerp(StablePrev.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), g8 - int2(1, 1)), 0)).xyz, StablePrev.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), g8 - int2(1, 1)), 0)).xyz, w.x.xxx), w.y.xxx);
            float3 cw = 0.0f.xxx;
            float nw = 0.0f;
            float aw = 0.0f;
            float ny = 0.0f;
            for (int y_1 = -3; y_1 <= 3; y_1++)
            {
                for (int x_1 = -3; x_1 <= 3; x_1++)
                {
                    int2 pw = int2(floor(q8)) + (int2(x_1, y_1) * int2(4, 4));
                    bool _2589 = all(bool2(pw.x >= int2(0, 0).x, pw.y >= int2(0, 0).y));
                    bool _2596;
                    if (_2589)
                    {
                        _2596 = all(bool2(pw.x < g8.x, pw.y < g8.y));
                    }
                    else
                    {
                        _2596 = _2589;
                    }
                    if (_2596)
                    {
                        float4 c1 = StablePrev.Load(int3(pw, 0));
                        cw += c1.xyz;
                        nw += 1.0f;
                        if (c1.w > 0.25f)
                        {
                            jy += (c1.w - 0.5f);
                            ny += 1.0f;
                        }
                        else
                        {
                            aw += c1.w;
                        }
                    }
                }
            }
            cp -= (cw / max(nw, 1.0f).xxx);
            jw = aw / max(nw, 1.0f);
            jy /= max(ny, 1.0f);
            int2 o2 = int2(g8.x, 0);
            ap = lerp(lerp(StablePrev.Load(int3(o2 + clamp(i0, int2(0, 0), g8 - int2(1, 1)), 0)).x, StablePrev.Load(int3(o2 + clamp(i0 + int2(1, 0), int2(0, 0), g8 - int2(1, 1)), 0)).x, w.x), lerp(StablePrev.Load(int3(o2 + clamp(i0 + int2(0, 1), int2(0, 0), g8 - int2(1, 1)), 0)).x, StablePrev.Load(int3(o2 + clamp(i0 + int2(1, 1), int2(0, 0), g8 - int2(1, 1)), 0)).x, w.x), w.y);
        }
        float ts = 1.0f;
        float wg = 1.0f - smoothstep(0.02999999932944774627685546875f, 0.07999999821186065673828125f, length(mv) / float(pc_size.y));
        float3 cn = ((cp * ts) + j_1) * (0.75f * wg);
        float unc = 0.0f;
        if (true)
        {
            int2 h2 = int2(pc_size) - int2(1, 1);
            float2 mvs = Motion.Load(int3(int2(mt), 0)).xy * pc_mscale;
            for (int k = 0; k < 16; k++)
            {
                int2 pk = min((int2(bo) + int2(1 + (2 * (k & 3)), 1 + (2 * (k >> 2)))), h2);
                float2 qk = (float2(pk) + 0.5f.xx) + mvs;
                bool _2773 = any(bool2(qk.x < 0.0f.xx.x, qk.y < 0.0f.xx.y));
                bool _2783;
                if (!_2773)
                {
                    float2 _2780 = float2(pc_size);
                    _2783 = any(bool2(qk.x >= _2780.x, qk.y >= _2780.y));
                }
                else
                {
                    _2783 = _2773;
                }
                if (_2783)
                {
                    continue;
                }
                float nk = DepthCur[pk].xxxx.x;
                unc += (0.0625f * smoothstep(0.0500000007450580596923828125f, 0.20000000298023223876953125f, (DepthPrev.Load(int3(int2(qk), 0)).x - nk) / max(nk, 1.0000000116860974230803549289703e-07f)));
            }
            unc = smoothstep(0.300000011920928955078125f, 0.699999988079071044921875f, unc);
        }
        float age = min((ap * (1.0f - unc)) + 0.0416666679084300994873046875f, 1.0f);
        float old_1 = smoothstep(0.5f, 1.0f, age);
        StableCur[int2(bo / uint2(8u, 8u)) + int2(g8.x, 0)] = float4(age, 0.0f, 0.0f, 0.0f);
        cn *= old_1;
        cn -= (((((0.0f * float(max((int(pc_n) - 1), 0))) * wg) * jy) * smoothstep(0.125f, 0.20833332836627960205078125f, age)) * (1.0f - old_1)).xxx;
        cn *= min(1.0f, 0.0199999995529651641845703125f / max(max(abs(cn.x), max(abs(cn.y), abs(cn.z))), 9.9999999747524270787835121154785e-07f));
        cn += (((wg * (float(max((int(pc_n) - 2), 0)) / float(max(pc_n, 1u)))) * jw) * old_1).xxx;
        cn *= min(1.0f, 0.0199999995529651641845703125f / max(max(abs(cn.x), max(abs(cn.y), abs(cn.z))), 9.9999999747524270787835121154785e-07f));
        float _2931;
        if (age < ((float(pc_n) + 0.5f) / 24.0f))
        {
            _2931 = 0.0f;
        }
        else
        {
            _2931 = clamp(dot(j_1, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f)), -0.20000000298023223876953125f, 0.20000000298023223876953125f) + ((age < 0.5f) ? 0.5f : 0.0f);
        }
        StableCur[int2(bo / uint2(8u, 8u))] = float4(cn, _2931);
    }
}

float near_at(uint2 mt)
{
    uint2 t = pc_dbase + min((((mt - pc_mbase) * pc_dgrid) / pc_mgrid), (pc_dgrid - uint2(1u, 1u)));
    float d = DepthIn.Load(int3(int2(t), 0)).x;
    float _980;
    if ((pc_flags & 1024u) != 0u)
    {
        _980 = d;
    }
    else
    {
        _980 = 1.0f - d;
    }
    return _980;
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
    float _1514;
    float _1528;
    for (int k = 0; k < 4; k++)
    {
        uint param_5 = l;
        int2 param_6 = c[k];
        float2 dn = dlv(param_5, param_6);
        if (dn.y > 9.9999999747524270787835121154785e-07f)
        {
            _1514 = dn.x / dn.y;
        }
        else
        {
            _1514 = -1.0f;
        }
        float nk = _1514;
        if (nk < 0.0f)
        {
            _1528 = 1.0f;
        }
        else
        {
            _1528 = abs(nk - near) / max(max(nk, near), 1.0000000116860974230803549289703e-07f);
        }
        float rel = _1528;
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
    float4 _1650;
    if (sw > 9.9999997473787516355514526367188e-05f)
    {
        _1650 = (((((a * wb.x) + (b * wb.y)) + (cc * wb.z)) + (d * wb.w)) / sw.xxxx) * min(sw * 4.0f, 1.0f);
    }
    else
    {
        _1650 = 0.0f.xxxx;
    }
    return _1650;
}

float3 pull(uint i, float2 p, bool bil, float4 wl[6])
{
    uint param = 6u;
    uint param_1 = i;
    float2 param_2 = p;
    bool param_3 = false;
    float4 param_4 = 0.0f.xxxx;
    float4 top0 = lvb(param, param_1, param_2, param_3, param_4);
    float4 _1695;
    if (bil)
    {
        uint param_5 = 6u;
        uint param_6 = i;
        float2 param_7 = p;
        bool param_8 = true;
        float4 param_9 = wl[5];
        _1695 = lvb(param_5, param_6, param_7, param_8, param_9);
    }
    else
    {
        _1695 = top0;
    }
    float4 topd = _1695;
    float4 top = (topd.w > 9.9999997473787516355514526367188e-05f) ? topd : top0;
    float3 _1721;
    if (top.w > 9.9999997473787516355514526367188e-05f)
    {
        _1721 = top.xyz / top.w.xxx;
    }
    else
    {
        _1721 = 0.0f.xxx;
    }
    float3 e = _1721;
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

void st_meta(uint i, int2 p, float3 rgb, float meta)
{
    float4 o = float4(rgb, meta);
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
    NrGrade g = _206;
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
    float _286;
    if (L < 0.5f)
    {
        _286 = L + L;
    }
    else
    {
        _286 = (L + 1.0f) - L;
    }
    float hi = _286;
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
    float _614 = nr_grade_smooth(param_1);
    float w0 = _614;
    float param_2 = x / 0.25f;
    float _619 = nr_grade_smooth(param_2);
    float param_3 = (x - 0.5f) / (-0.25f);
    float _624 = nr_grade_smooth(param_3);
    float w1 = _619 * _624;
    float param_4 = (x - 0.25f) / 0.25f;
    float _631 = nr_grade_smooth(param_4);
    float param_5 = (x - 0.75f) / (-0.25f);
    float _637 = nr_grade_smooth(param_5);
    float w2 = _631 * _637;
    float param_6 = (x - 0.5f) / 0.25f;
    float _644 = nr_grade_smooth(param_6);
    float param_7 = (x - 1.0f) / (-0.25f);
    float _649 = nr_grade_smooth(param_7);
    float w3 = _644 * _649;
    float param_8 = (x - 0.75f) / 0.25f;
    float _656 = nr_grade_smooth(param_8);
    float w4 = _656;
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
        float _410;
        if (L > 0.5f)
        {
            _410 = d / ((2.0f - mx) - mn);
        }
        else
        {
            _410 = d / sum;
        }
        s = _410;
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
    float _537;
    if (L < 0.5f)
    {
        _537 = L * (s + 1.0f);
    }
    else
    {
        _537 = (L + s) - (L * s);
    }
    float hi = _537;
    float lo = (L + L) - hi;
    float param = h + 0.3333333432674407958984375f;
    float param_1 = hi;
    float param_2 = lo;
    float _567 = nr_grade_hue(param, param_1, param_2);
    float param_3 = h;
    float param_4 = hi;
    float param_5 = lo;
    float _574 = nr_grade_hue(param_3, param_4, param_5);
    float param_6 = h - 0.3333333432674407958984375f;
    float param_7 = hi;
    float param_8 = lo;
    float _582 = nr_grade_hue(param_6, param_7, param_8);
    return float3(_567, _574, _582);
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
    float _757 = nr_grade_contrast(param_11, param_12);
    float param_13 = c.y;
    float param_14 = g.contrast;
    float _764 = nr_grade_contrast(param_13, param_14);
    float param_15 = c.z;
    float param_16 = g.contrast;
    float _771 = nr_grade_contrast(param_15, param_16);
    c = float3(_757, _764, _771);
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
    uint st = (pc_styles >> (2u * kk)) & 3u;
    float I = pc_inten[kk];
    rgb = clamp(mad(min(I, 1.0f).xxx, rgb - base, base), 0.0f.xxx, 1.0f.xxx);
    if (I > 1.0f)
    {
        float ratio = (dot(rgb, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f)) + 0.001953125f) / (dot(base, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f)) + 0.001953125f);
        float amp = clamp(pow(max(ratio, 9.9999999747524270787835121154785e-07f), I), 0.5f, 2.0f);
        rgb = clamp(rgb * (amp / max(ratio, 9.9999999747524270787835121154785e-07f)), 0.0f.xxx, 1.0f.xxx);
    }
    if (st != 0u)
    {
        uint param = st;
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
    float2 _1936;
    if (l > m)
    {
        _1936 = x * (m / l);
    }
    else
    {
        _1936 = x;
    }
    return _1936;
}

float3 bil_meta(uint i, float2 p)
{
    float2 f = p - 0.5f.xx;
    float2 b0 = floor(f);
    float2 w = f - b0;
    int2 i0 = int2(b0);
    int2 hi = int2(pc_size) - int2(1, 1);
    uint param = i;
    int2 param_1 = clamp(i0, int2(0, 0), hi);
    float4 aa = src(param, param_1);
    uint param_2 = i;
    int2 param_3 = clamp(i0 + int2(1, 0), int2(0, 0), hi);
    float4 bb = src(param_2, param_3);
    uint param_4 = i;
    int2 param_5 = clamp(i0 + int2(0, 1), int2(0, 0), hi);
    float4 cc = src(param_4, param_5);
    uint param_6 = i;
    int2 param_7 = clamp(i0 + int2(1, 1), int2(0, 0), hi);
    float4 dd = src(param_6, param_7);
    float3 a = aa.xyz;
    float3 b = bb.xyz;
    float3 c = cc.xyz;
    float3 d = dd.xyz;
    g_prev_alpha = lerp(lerp(aa.w, bb.w, w.x), lerp(cc.w, dd.w, w.x), w.y);
    return lerp(lerp(a, b, w.x.xxx), lerp(c, d, w.x.xxx), w.y.xxx);
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

float3 stable_at(float2 p)
{
    int2 grid = int2((pc_size + uint2(7u, 7u)) / uint2(8u, 8u));
    float2 f = (p / 8.0f.xx) - 0.5f.xx;
    float2 b = floor(f);
    float2 w = f - b;
    int2 k = int2(b);
    return lerp(lerp(StableCur[clamp(k, int2(0, 0), grid - int2(1, 1))].xyz, StableCur[clamp(k + int2(1, 0), int2(0, 0), grid - int2(1, 1))].xyz, w.x.xxx), lerp(StableCur[clamp(k + int2(0, 1), int2(0, 0), grid - int2(1, 1))].xyz, StableCur[clamp(k + int2(1, 1), int2(0, 0), grid - int2(1, 1))].xyz, w.x.xxx), w.y.xxx);
}

void body(uint2 px)
{
    int2 ip = int2(px);
    uint _3258;
    if (_3257)
    {
        _3258 = kStep;
    }
    else
    {
        _3258 = pc_mode;
    }
    if (_3258 == 3u)
    {
        Base[ip] = Proxy[ip];
        return;
    }
    uint _3275;
    if (_3274)
    {
        _3275 = kStep;
    }
    else
    {
        _3275 = pc_mode;
    }
    if (_3275 == 5u)
    {
        uint2 mt = pc_mbase + min(uint2(((float2(px) + 0.5f.xx) * float2(pc_mgrid)) / float2(pc_size)), (pc_mgrid - uint2(1u, 1u)));
        float _3314;
        if ((pc_flags & 131072u) != 0u)
        {
            uint2 param = mt;
            _3314 = near_at(param);
        }
        else
        {
            _3314 = 1.0f;
        }
        DepthCur[ip] = _3314.x;
        return;
    }
    uint _3325;
    if (_3324)
    {
        _3325 = kStep;
    }
    else
    {
        _3325 = pc_mode;
    }
    if (_3325 == 4u)
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
        float3 _3390;
        if (keep)
        {
            _3390 = Proxy[ip].xyz;
        }
        else
        {
            _3390 = Base[ip].xyz;
        }
        float3 z = _3390;
        for (uint i = 0u; i < pc_n; i++)
        {
            uint param_4 = i;
            int2 param_5 = ip;
            float4 old = ld4(param_4, param_5);
            uint param_6 = i;
            float2 param_7 = float2(px) + 0.5f.xx;
            bool param_8 = bil_1;
            float3 v = lerp(pull(param_6, param_7, param_8, wl), old.xyz, trust.xxx);
            uint param_9 = i;
            int2 param_10 = ip;
            float3 param_11 = v;
            float param_12 = old.w;
            st_meta(param_9, param_10, param_11, param_12);
            if (i < pc_slot)
            {
                uint param_13 = i;
                float3 param_14 = z;
                float3 param_15 = z + v;
                float3 _3460 = post(param_13, param_14, param_15);
                z = _3460;
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
    uint _3497;
    if (_3496)
    {
        _3497 = kStep;
    }
    else
    {
        _3497 = pc_mode;
    }
    if (_3497 == 0u)
    {
        bool keep_1 = (pc_flags & 268435456u) != 0u;
        float4 _3514;
        if (keep_1)
        {
            _3514 = Proxy[ip];
        }
        else
        {
            _3514 = Base[ip];
        }
        float4 b = _3514;
        if ((pc_flags & 2u) != 0u)
        {
            float3 h = BasePrev.Load(int3(ip, 0)).xyz;
            uint i_1 = 0u;
            for (;;)
            {
                bool _3545 = i_1 < pc_slot;
                bool _3552;
                if (_3545)
                {
                    _3552 = i_1 < pc_n;
                }
                else
                {
                    _3552 = _3545;
                }
                if (_3552)
                {
                    uint param_16 = i_1;
                    int2 param_17 = ip;
                    uint param_18 = i_1;
                    float3 param_19 = h;
                    float3 param_20 = h + src(param_16, param_17).xyz;
                    float3 _3566 = post(param_18, param_19, param_20);
                    h = _3566;
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
                uint param_21 = pc_slot;
                int2 param_22 = ip;
                h += src(param_21, param_22).xyz;
            }
            uint param_23 = ((pc_slot + pc_n) - 1u) % pc_n;
            int2 param_24 = ip;
            float prior_lead = src(param_23, param_24).w;
            h += (((abs(prior_lead) < 0.5f) ? prior_lead : 0.0f).xxx * 0.0f);
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
                uint2 param_25 = t;
                float bn = near_at(param_25);
                for (int y = -1; y <= 1; y++)
                {
                    for (int x = -1; x <= 1; x++)
                    {
                        uint2 c = uint2(clamp(int2(t) + int2(x, y), int2(pc_mbase), int2(pc_mbase + pc_mgrid) - int2(1, 1)));
                        uint2 param_26 = c;
                        float nn = near_at(param_26);
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
            bool _3731 = all(bool2(q.x >= 0.0f.xx.x, q.y >= 0.0f.xx.y));
            bool _3740;
            if (_3731)
            {
                float2 _3737 = float2(pc_size);
                _3740 = all(bool2(q.x <= _3737.x, q.y <= _3737.y));
            }
            else
            {
                _3740 = _3731;
            }
            inside = _3740;
        }
        bool raw_inside = inside;
        float raw_depth = 1.0f;
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
            float3 _3795;
            for (int y_1 = -1; y_1 <= 1; y_1++)
            {
                for (int x_1 = -1; x_1 <= 1; x_1++)
                {
                    int2 c_1 = clamp(ip + int2(x_1, y_1), int2(0, 0), int2(pc_size) - int2(1, 1));
                    if (keep_1)
                    {
                        _3795 = Proxy[c_1].xyz;
                    }
                    else
                    {
                        _3795 = Base[c_1].xyz;
                    }
                    float3 cb = _3795;
                    float2 param_27 = q + float2(float(x_1), float(y_1));
                    float3 pm = prevb(param_27);
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
                float2 _3936;
                if (vprev)
                {
                    float2 param_28 = q;
                    _3936 = mvprev(param_28);
                }
                else
                {
                    _3936 = Motion.Load(int3(int2(t2), 0)).xy * pc_mscale;
                }
                float2 vq = _3936;
                float dv = length(vq - v_1);
                if (vboth)
                {
                    float2 param_29 = q;
                    dv = min(dv, length(mvprev(param_29) - v_1));
                }
                float lv_1 = length(v_1);
                if ((pc_flags & 524288u) != 0u)
                {
                    int2 tc = int2(pc_mbase + min(uint2(((float2(px) + 0.5f.xx) * float2(pc_mgrid)) / float2(pc_size)), (pc_mgrid - uint2(1u, 1u))));
                    int2 param_30 = tc;
                    float2 param_31 = v_1;
                    float2 gp = grad_v(param_30, param_31);
                    float cmax = 0.5f * lv_1;
                    float2 param_32 = gp;
                    float param_33 = cmax;
                    dv = min(dv, length((vq - v_1) - clampl(param_32, param_33)));
                    if ((pc_flags & 1048576u) != 0u)
                    {
                        int2 param_34 = int2(t2);
                        float2 param_35 = v_1;
                        float2 gq = grad_v(param_34, param_35);
                        float2 param_36 = gq;
                        float param_37 = cmax;
                        float2 param_38 = (gp + gq) * 0.5f;
                        float param_39 = cmax;
                        dv = min(dv, min(length((vq - v_1) - clampl(param_36, param_37)), length((vq - v_1) - clampl(param_38, param_39))));
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
        bool _4122 = (pc_flags & 512u) != 0u;
        bool _4129;
        if (_4122)
        {
            _4129 = (pc_flags & 1u) != 0u;
        }
        else
        {
            _4129 = _4122;
        }
        if (_4129 && inside)
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
            raw_depth = 1.0f - smoothstep(0.0500000007450580596923828125f, 0.20000000298023223876953125f, r);
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
        float3 _4244;
        if (!inside)
        {
            _4244 = 0.0f.xxx;
        }
        else
        {
            float3 _4252;
            if ((pc_flags & 1u) != 0u)
            {
                uint param_40 = pc_slot;
                float2 param_41 = q;
                float3 _4260 = bil_meta(param_40, param_41);
                _4252 = _4260;
            }
            else
            {
                uint param_42 = pc_slot;
                int2 param_43 = ip;
                _4252 = src(param_42, param_43).xyz;
            }
            _4244 = _4252;
        }
        float3 cached = _4244;
        if ((pc_flags & 1u) == 0u)
        {
            uint param_44 = pc_slot;
            int2 param_45 = ip;
            g_prev_alpha = src(param_44, param_45).w;
        }
        if (stay > 0.0f)
        {
            uint param_46 = pc_slot;
            int2 param_47 = ip;
            cached = lerp(cached, src(param_46, param_47).xyz, stay.xxx);
            uint param_48 = pc_slot;
            int2 param_49 = ip;
            g_prev_alpha = lerp(g_prev_alpha, src(param_48, param_49).w, stay);
        }
        float age = 45.0f;
        if (((pc_flags & 2u) != 0u) && inside)
        {
            float _4322;
            if (g_prev_alpha < (-0.5f))
            {
                _4322 = min(-g_prev_alpha, 45.0f);
            }
            else
            {
                _4322 = 45.0f;
            }
            age = _4322;
            if (((pc_flags & 512u) != 0u) && (raw_depth < 0.5f))
            {
                age = 0.0f;
            }
        }
        if (((pc_flags & 2u) != 0u) && (!raw_inside))
        {
            age = 0.0f;
        }
        float3 _4363;
        float3 _4370;
        float3 _4378;
        float _4421;
        float _4434;
        for (uint i_2 = 0u; i_2 < pc_n; i_2++)
        {
            if (i_2 == pc_slot)
            {
                _4363 = cached;
            }
            else
            {
                if (!inside)
                {
                    _4370 = 0.0f.xxx;
                }
                else
                {
                    if ((pc_flags & 1u) != 0u)
                    {
                        uint param_50 = i_2;
                        float2 param_51 = q;
                        _4378 = bil(param_50, param_51);
                    }
                    else
                    {
                        uint param_52 = i_2;
                        int2 param_53 = ip;
                        _4378 = src(param_52, param_53).xyz;
                    }
                    _4370 = _4378;
                }
                _4363 = _4370;
            }
            float3 v_2 = _4363;
            bool _4397 = stay > 0.0f;
            bool _4404;
            if (_4397)
            {
                _4404 = i_2 != pc_slot;
            }
            else
            {
                _4404 = _4397;
            }
            if (_4404)
            {
                uint param_54 = i_2;
                int2 param_55 = ip;
                v_2 = lerp(v_2, src(param_54, param_55).xyz, stay.xxx);
            }
            if (i_2 == pc_slot)
            {
                _4421 = stay;
            }
            else
            {
                if (i_2 == ((pc_slot + 1u) % pc_n))
                {
                    _4434 = -(age + 1.0f);
                }
                else
                {
                    _4434 = 0.0f;
                }
                _4421 = _4434;
            }
            uint param_56 = i_2;
            int2 param_57 = ip;
            float3 param_58 = v_2;
            float param_59 = _4421;
            st_meta(param_56, param_57, param_58, param_59);
            if (i_2 < pc_slot)
            {
                uint param_60 = i_2;
                float3 param_61 = z_1;
                float3 param_62 = z_1 + (v_2 * trust_1);
                float3 _4467 = post(param_60, param_61, param_62);
                z_1 = _4467;
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
        float4 _4510;
        if (keep_2)
        {
            _4510 = Base[ip];
        }
        else
        {
            _4510 = Proxy[ip];
        }
        float4 zin = _4510;
        float4 _4523;
        if (keep_2)
        {
            _4523 = Proxy[ip];
        }
        else
        {
            _4523 = Base[ip];
        }
        float4 b_1 = _4523;
        float3 fresh = a.xyz - zin.xyz;
        bool _4545 = (pc_flags & 65536u) != 0u;
        bool _4553;
        if (_4545)
        {
            _4553 = (pc_flags & 64u) == 0u;
        }
        else
        {
            _4553 = _4545;
        }
        float _4554;
        if (_4553)
        {
            _4554 = Trust[ip].xxxx.x;
        }
        else
        {
            _4554 = 1.0f;
        }
        float trust_2 = _4554;
        if ((pc_flags & 32u) != 0u)
        {
            uint param_63 = pc_slot;
            int2 param_64 = ip;
            float3 old_1 = ld(param_63, param_64);
            float dl = length(fresh - old_1);
            float r_1 = dl / (max(length(fresh), length(old_1)) + 0.0040000001899898052215576171875f);
            trust_2 = min(trust_2, 1.0f - (smoothstep(pc_k0, pc_k1, r_1) * smoothstep(0.006000000052154064178466796875f, 0.0199999995529651641845703125f, dl)));
        }
        float history_lead = 0.0f;
        float3 o_1 = b_1.xyz;
        float display_age = 45.0f;
        float4 _4625;
        float _4656;
        for (uint i_3 = 0u; i_3 < pc_n; i_3++)
        {
            if (i_3 == pc_slot)
            {
                _4625 = float4(fresh, 0.0f);
            }
            else
            {
                uint param_65 = i_3;
                int2 param_66 = ip;
                _4625 = ld4(param_65, param_66);
            }
            float4 value = _4625;
            float3 v_3 = value.xyz;
            if (i_3 == ((pc_slot + 1u) % pc_n))
            {
                if (value.w < (-0.5f))
                {
                    _4656 = clamp((-value.w) - 1.0f, 0.0f, 45.0f);
                }
                else
                {
                    _4656 = 45.0f;
                }
                display_age = _4656;
            }
            if ((i_3 != pc_slot) && (trust_2 < 1.0f))
            {
                v_3 = lerp(fresh, v_3, trust_2.xxx);
                uint param_67 = i_3;
                int2 param_68 = ip;
                float3 param_69 = v_3;
                st(param_67, param_68, param_69);
            }
            uint param_70 = i_3;
            float3 param_71 = o_1;
            float3 param_72 = o_1 + v_3;
            float3 _4695 = post(param_70, param_71, param_72);
            o_1 = _4695;
        }
        if ((pc_flags & 16u) != 0u)
        {
            float2 param_73 = float2(px) + 0.5f.xx;
            o_1 += stable_at(param_73);
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
        uint param_74 = pc_slot;
        int2 param_75 = ip;
        float3 param_76 = fresh;
        float param_77 = history_lead;
        st_meta(param_74, param_75, param_76, param_77);
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
    bool _2964;
    if (live)
    {
        _2964 = (l.x & 1u) == 0u;
    }
    else
    {
        _2964 = live;
    }
    bool _2971;
    if (_2964)
    {
        _2971 = (l.y & 1u) == 0u;
    }
    else
    {
        _2971 = _2964;
    }
    bool lead = _2971;
    bool depth = (pc_flags & 1073741824u) != 0u;
    uint dw = (pc_size.x + 1u) / 2u;
    float _2985;
    if (live)
    {
        _2985 = Trust[ip].xxxx.x;
    }
    else
    {
        _2985 = 0.0f;
    }
    float w = _2985;
    float4 _3010;
    float2 _3037;
    for (uint i = 0u; i < pc_n; i++)
    {
        if (live)
        {
            uint param = i;
            int2 param_1 = ip;
            _3010 = float4(ld(param, param_1) * w, w);
        }
        else
        {
            _3010 = 0.0f.xxxx;
        }
        s_pp[li] = _3010;
        if ((i == 0u) && depth)
        {
            if (live)
            {
                _3037 = float2(DepthCur[ip].xxxx.x * w, w);
            }
            else
            {
                _3037 = 0.0f.xx;
            }
            s_dp[li] = _3037;
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
    g_prev_alpha = 0.0f;
    uint2 px = gl_GlobalInvocationID.xy;
    bool live = all(bool2(px.x < pc_size.x, px.y < pc_size.y));
    uint _3201;
    if (_3200)
    {
        _3201 = kStep;
    }
    else
    {
        _3201 = pc_mode;
    }
    if (_3201 == 6u)
    {
        uint2 param = px;
        bool param_1 = live;
        stable_reduce(param, param_1);
        return;
    }
    uint _3218;
    if (_3217)
    {
        _3218 = kStep;
    }
    else
    {
        _3218 = pc_mode;
    }
    bool _3225 = _3218 == 0u;
    bool _3233;
    if (_3225)
    {
        _3233 = (pc_flags & 536870912u) != 0u;
    }
    else
    {
        _3233 = _3225;
    }
    if (_3233)
    {
        if (live)
        {
            uint2 param_2 = px;
            body(param_2);
        }
        uint2 param_3 = px;
        bool param_4 = live;
        pyr1(param_3, param_4);
        return;
    }
    if (live)
    {
        uint2 param_5 = px;
        body(param_5);
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
