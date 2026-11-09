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
struct Ans
{
    float3 O;
    float co;
    float2 uo;
};

struct Ov
{
    float2 dab;
    float f;
    float amp;
    float3 l;
    uint slot;
};

cbuffer Push
{
    uint2 pc_size : packoffset(c0);
    uint2 pc_low : packoffset(c0.z);
    float pc_k : packoffset(c1);
    uint pc_mode : packoffset(c1.y);
    float pc_halo : packoffset(c1.z);
    float pc_chroma : packoffset(c1.w);
    uint pc_satkeep : packoffset(c2);
    float pc_fine : packoffset(c2.y);
    float pc_base : packoffset(c2.z);
    float pc_rho : packoffset(c2.w);
    float pc_lam : packoffset(c3);
    float pc_dcol : packoffset(c3.y);
    float pc_cfloor : packoffset(c3.z);
    float pc_ring : packoffset(c3.w);
    float pc_ccap : packoffset(c4);
    float pc_nohalo : packoffset(c4.y);
    uint pc_restored : packoffset(c4.z);
    float pc_ocap : packoffset(c4.w);
    float pc_darkcol : packoffset(c5);
};

Texture2D<float4> FineH : register(t5);
Texture2D<float4> Fine : register(t1);
Texture2D<float4> CA : register(t2);
Texture2D<float4> CB : register(t3);
RWTexture2D<float4> N : register(u4);
RWTexture2D<float4> P : register(u0);
RWTexture2D<float4> OutA : register(u6);
RWTexture2D<float4> OutB : register(u7);

static uint3 gl_WorkGroupID;
static uint3 gl_LocalInvocationID;
static uint3 gl_GlobalInvocationID;
static uint gl_LocalInvocationIndex;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
    uint gl_LocalInvocationIndex : SV_GroupIndex;
};

groupshared uint s_qn;
groupshared float4 s_qa[128];
groupshared float4 s_qb[128];
groupshared float2 s_qc[128];
static Ov g_v1;
static Ov g_v2;
static float3 g_ofb;
static float4 g_fa;
groupshared float4 s_iv[16][16];

float3 st_dec(float3 c)
{
    float3 _160 = c / 12.9200000762939453125f.xxx;
    float3 _170 = pow((c + 0.054999999701976776123046875f.xxx) / 1.05499994754791259765625f.xxx, 2.400000095367431640625f.xxx);
    bool3 _175 = bool3(c.x > 0.040449999272823333740234375f.xxx.x, c.y > 0.040449999272823333740234375f.xxx.y, c.z > 0.040449999272823333740234375f.xxx.z);
    return float3(_175.x ? _170.x : _160.x, _175.y ? _170.y : _160.y, _175.z ? _170.z : _160.z);
}

float3 st_lab(float3 s)
{
    float3 param = max(s, 0.0f.xxx);
    float3 l = st_dec(param);
    float3 lms = float3(dot(l, float3(0.4122214615345001220703125f, 0.536332547664642333984375f, 0.0514459945261478424072265625f)), dot(l, float3(0.21190349757671356201171875f, 0.680699527263641357421875f, 0.10739696025848388671875f)), dot(l, float3(0.0883024632930755615234375f, 0.28171885013580322265625f, 0.629978716373443603515625f)));
    float3 q = sign(lms) * pow(abs(lms), 0.3333333432674407958984375f.xxx);
    return float3(dot(q, float3(0.2104542553424835205078125f, 0.793617784976959228515625f, -0.004072046838700771331787109375f)), dot(q, float3(1.9779984951019287109375f, -2.428592205047607421875f, 0.4505937099456787109375f)), dot(q, float3(0.025904037058353424072265625f, 0.782771766185760498046875f, -0.8086757659912109375f)));
}

Ans answer(float3 O)
{
    Ans a;
    a.O = O;
    a.co = length(O.yz);
    float2 _575;
    if (a.co > 0.0f)
    {
        _575 = O.yz / a.co.xx;
    }
    else
    {
        _575 = 0.0f.xx;
    }
    a.uo = _575;
    return a;
}

float3 st_gfq(Texture2D<float4> CA_1, Texture2D<float4> CB_1, float2 pos, float I, uint2 low, out float mI, out float vI)
{
    float2 q = clamp((pos / 4.0f.xx) - 0.5f.xx, 0.0f.xx, float2(low) - 1.0f.xx);
    int2 a = int2(floor(q));
    int2 b = min((a + int2(1, 1)), (int2(low) - int2(1, 1)));
    float2 f = q - float2(a);
    float4 A = lerp(lerp(CA_1.Load(int3(a, 0)), CA_1.Load(int3(int2(b.x, a.y), 0)), f.x.xxxx), lerp(CA_1.Load(int3(int2(a.x, b.y), 0)), CA_1.Load(int3(b, 0)), f.x.xxxx), f.y.xxxx);
    float4 B = lerp(lerp(CB_1.Load(int3(a, 0)), CB_1.Load(int3(int2(b.x, a.y), 0)), f.x.xxxx), lerp(CB_1.Load(int3(int2(a.x, b.y), 0)), CB_1.Load(int3(b, 0)), f.x.xxxx), f.y.xxxx);
    mI = A.w;
    vI = B.w;
    return (A.xyz * I) + B.xyz;
}

float3 st_lin(float3 lab)
{
    float3 q = float3((lab.x + (0.3963377773761749267578125f * lab.y)) + (0.21580375730991363525390625f * lab.z), (lab.x - (0.1055613458156585693359375f * lab.y)) - (0.06385417282581329345703125f * lab.z), (lab.x - (0.089484177529811859130859375f * lab.y)) - (1.2914855480194091796875f * lab.z));
    float3 lms = (q * q) * q;
    return float3(dot(lms, float3(4.076741695404052734375f, -3.30771160125732421875f, 0.2309699356555938720703125f)), dot(lms, float3(-1.26843798160552978515625f, 2.60975742340087890625f, -0.341319382190704345703125f)), dot(lms, float3(-0.0041960864327847957611083984375f, -0.70341861248016357421875f, 1.7076146602630615234375f)));
}

float3 st_enc(inout float3 l)
{
    l = clamp(l, 0.0f.xxx, 1.0f.xxx);
    float3 _186 = l * 12.9200000762939453125f;
    float3 _193 = (pow(l, 0.4166666567325592041015625f.xxx) * 1.05499994754791259765625f) - 0.054999999701976776123046875f.xxx;
    bool3 _197 = bool3(l.x > 0.003130800090730190277099609375f.xxx.x, l.y > 0.003130800090730190277099609375f.xxx.y, l.z > 0.003130800090730190277099609375f.xxx.z);
    return float3(_197.x ? _193.x : _186.x, _197.y ? _193.y : _186.y, _197.z ? _193.z : _186.z);
}

float3 st_dadd(float3 n, float3 Ln, float3 add)
{
    if (pc_darkcol > 0.0f)
    {
        float3 N0 = Ln;
        float3 param = clamp(n + add, 0.0f.xxx, 1.0f.xxx);
        float3 Y0 = st_lab(param);
        float _1083;
        if (pc_dcol >= 1.0f)
        {
            _1083 = 1.0f;
        }
        else
        {
            _1083 = max(pc_dcol, 0.0f);
        }
        float cs = _1083 * smoothstep(0.0f, pc_darkcol, N0.x);
        float3 param_1 = float3(Y0.x, N0.yz + ((Y0.yz - N0.yz) * cs));
        float3 param_2 = st_lin(param_1);
        float3 _1115 = st_enc(param_2);
        return _1115 - n;
    }
    if (pc_dcol >= 1.0f)
    {
        return add;
    }
    float3 N_1 = Ln;
    float3 param_3 = clamp(n + add, 0.0f.xxx, 1.0f.xxx);
    float3 Y = st_lab(param_3);
    if (pc_dcol < 0.0f)
    {
        float2 ab = (N_1.yz * (Y.x / max(N_1.x, 0.001000000047497451305389404296875f))) - ((Y.yz - N_1.yz) * pc_dcol);
        float3 param_4 = float3(Y.x, ab);
        float3 param_5 = st_lin(param_4);
        float3 _1171 = st_enc(param_5);
        return _1171 - n;
    }
    float3 param_6 = float3(Y.x, N_1.yz + ((Y.yz - N_1.yz) * pc_dcol));
    float3 param_7 = st_lin(param_6);
    float3 _1194 = st_enc(param_7);
    return _1194 - n;
}

float st_fit(float3 base, float3 add)
{
    float s = 1.0f;
    for (int c = 0; c < 3; c++)
    {
        if (add[c] > 1.0000000116860974230803549289703e-07f)
        {
            s = min(s, max(1.0f - base[c], 0.0f) / add[c]);
        }
        else
        {
            if (add[c] < (-1.0000000116860974230803549289703e-07f))
            {
                s = min(s, max(base[c], 0.0f) / (-add[c]));
            }
        }
    }
    return s;
}

float st_soft(float anchor, float extra)
{
    float a = clamp(anchor, 9.9999997473787516355514526367188e-05f, 0.99989998340606689453125f);
    float _462;
    if (extra < 0.0f)
    {
        _462 = a * exp(extra / a);
    }
    else
    {
        _462 = 1.0f - ((1.0f - a) * exp((-extra) / (1.0f - a)));
    }
    return _462;
}

float3 at7(Ans a, float amp, float2 dab, float f, float t)
{
    float3 O = a.O;
    float param = O.x;
    float param_1 = (t - 1.0f) * amp;
    float L = st_soft(param, param_1);
    float _613;
    if (pc_mode >= 7u)
    {
        _613 = pow(L / max(O.x, 9.9999997473787516355514526367188e-05f), asfloat(pc_satkeep));
    }
    else
    {
        _613 = 1.0f;
    }
    float g = _613;
    if (pc_mode >= 8u)
    {
        g = min(g, 1.0f);
    }
    float tc = t;
    if (pc_ccap > 1.0f)
    {
        float e = (t - 1.0f) / (pc_ccap - 1.0f);
        tc = 1.0f + ((t - 1.0f) / pow(1.0f + (((e * e) * e) * e), 0.25f));
    }
    float2 ab = (O.yz + (dab * ((tc - 1.0f) * f))) * g;
    float c = length(ab);
    bool _684 = pc_rho > 0.0f;
    bool _691;
    if (_684)
    {
        _691 = a.co > 0.004999999888241291046142578125f;
    }
    else
    {
        _691 = _684;
    }
    if (_691 && (c > 9.9999999747524270787835121154785e-07f))
    {
        float2 u = ab / c.xx;
        float d = atan2((u.x * a.uo.y) - (u.y * a.uo.x), dot(u, a.uo));
        float r = pc_rho * d;
        float sr = sin(r);
        float cr = cos(r);
        ab = float2((u.x * cr) - (u.y * sr), (u.x * sr) + (u.y * cr)) * c;
    }
    return float3(L, ab);
}

bool st_in(float3 l)
{
    bool _329 = all(bool3(l.x >= (-9.9999999747524270787835121154785e-07f).xxx.x, l.y >= (-9.9999999747524270787835121154785e-07f).xxx.y, l.z >= (-9.9999999747524270787835121154785e-07f).xxx.z));
    bool _337;
    if (_329)
    {
        _337 = all(bool3(l.x <= 1.00000095367431640625f.xxx.x, l.y <= 1.00000095367431640625f.xxx.y, l.z <= 1.00000095367431640625f.xxx.z));
    }
    else
    {
        _337 = _329;
    }
    return _337;
}

Ov ov_begin(float3 X, Ans A, float amp_in)
{
    float3 O = A.O;
    Ov v;
    v.dab = O.yz - X.yz;
    float rel = (length(v.dab) / (length(X.yz) + 0.00999999977648258209228515625f)) / pc_chroma;
    float fl = pc_cfloor * smoothstep(0.4000000059604644775390625f, 0.64999997615814208984375f, O.x);
    v.f = fl + ((1.0f - fl) / (1.0f + (rel * rel)));
    float _805;
    if (amp_in > (-100000000.0f))
    {
        _805 = amp_in;
    }
    else
    {
        _805 = O.x - X.x;
    }
    v.amp = _805;
    Ans param = A;
    float param_1 = v.amp;
    float2 param_2 = v.dab;
    float param_3 = v.f;
    float param_4 = pc_k;
    float3 param_5 = at7(param, param_1, param_2, param_3, param_4);
    v.l = st_lin(param_5);
    v.slot = 4294967294u;
    float3 param_6 = v.l;
    if (!st_in(param_6))
    {
        uint _848;
        InterlockedAdd(s_qn, 1u, _848);
        v.slot = _848;
        if (v.slot < 128u)
        {
            s_qa[v.slot] = float4(O, v.amp);
            s_qb[v.slot] = float4(A.uo, v.dab);
            s_qc[v.slot] = float2(v.f, A.co);
        }
        else
        {
            v.slot = 4294967295u;
        }
    }
    return v;
}

float3 st_relight(float3 lab, float Lb, float extra)
{
    float param = Lb;
    float param_1 = extra;
    float3 param_2 = float3(st_soft(param, param_1), lab.yz);
    float3 l = st_lin(param_2);
    float3 param_3 = l;
    if (st_in(param_3))
    {
        float3 param_4 = l;
        float3 _505 = st_enc(param_4);
        return _505;
    }
    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 12; i++)
    {
        float m = 0.5f * (lo + hi);
        float param_5 = Lb;
        float param_6 = m * extra;
        float3 param_7 = float3(st_soft(param_5, param_6), lab.yz);
        float3 param_8 = st_lin(param_7);
        if (st_in(param_8))
        {
            lo = m;
        }
        else
        {
            hi = m;
        }
    }
    float param_9 = Lb;
    float param_10 = lo * extra;
    float3 param_11 = float3(st_soft(param_9, param_10), lab.yz);
    float3 param_12 = st_lin(param_11);
    float3 _561 = st_enc(param_12);
    return _561;
}

float3 st_nohalo(float3 o, float3 alt)
{
    if (pc_nohalo > 1.5f)
    {
        float3 param = o;
        float3 param_1 = alt;
        return (st_lab(param).x > st_lab(param_1).x) ? alt : o;
    }
    return lerp(o, alt, clamp(pc_nohalo, 0.0f, 1.0f).xxx);
}

float3 st_fb_ring(float3 o, float3 Ln, int2 px)
{
    float r0 = 1000000000.0f;
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            r0 = min(r0, FineH.Load(int3(clamp(px + int2(x, y), int2(0, 0), int2(pc_size) - int2(1, 1)), 0)).y);
        }
    }
    float3 param = o;
    float3 O = st_lab(param);
    float floor_l = min(Ln.x, r0);
    if (O.x >= floor_l)
    {
        return o;
    }
    float3 param_1 = float3(floor_l, O.yz);
    float3 l = st_lin(param_1);
    float3 param_2 = l;
    float3 _1350;
    if (st_in(param_2))
    {
        float3 param_3 = l;
        float3 _1355 = st_enc(param_3);
        _1350 = _1355;
    }
    else
    {
        _1350 = o;
    }
    return _1350;
}

void hybrid_begin(uint2 px, float4 n, float3 p, Ans A)
{
    float3 param = p;
    float3 X = st_lab(param);
    float4 fa = Fine.Load(int3(int2(px), 0));
    float2 param_1 = float2(px) + 0.5f.xx;
    float param_2 = X.x;
    uint2 param_3 = pc_low;
    float param_4;
    float param_5;
    float3 _1526 = st_gfq(CA, CB, param_1, param_2, param_3, param_4, param_5);
    float mI = param_4;
    float vI = param_5;
    float3 q = _1526;
    float3 pp = p - fa.xyz;
    float3 param_6 = pp;
    float3 Xp = st_lab(param_6);
    float3 param_7 = n.xyz;
    float3 param_8 = A.O;
    float3 param_9 = ((n.xyz - p) - q) * pc_halo;
    float3 add = st_dadd(param_7, param_8, param_9);
    float3 param_10 = n.xyz;
    float3 param_11 = add;
    float3 ofb = n.xyz + (add * st_fit(param_10, param_11));
    float3 param_12 = Xp;
    Ans param_13 = A;
    float param_14 = -1000000000.0f;
    Ov _1576 = ov_begin(param_12, param_13, param_14);
    g_v1 = _1576;
    if (pc_nohalo > 0.0f)
    {
        float F = FineH.Load(int3(int2(px), 0)).x;
        float fF = pc_halo * F;
        float3 _1595;
        if (fF != 0.0f)
        {
            float3 param_15 = A.O;
            float param_16 = A.O.x;
            float param_17 = fF;
            _1595 = st_relight(param_15, param_16, param_17);
        }
        else
        {
            _1595 = n.xyz;
        }
        float3 param_18 = ofb;
        float3 param_19 = _1595;
        ofb = st_nohalo(param_18, param_19);
        if (pc_ring > 0.0f)
        {
            float3 param_20 = ofb;
            float3 param_21 = A.O;
            int2 param_22 = int2(px);
            ofb = st_fb_ring(param_20, param_21, param_22);
        }
        float3 param_23 = clamp(pp + q, 0.0f.xxx, 1.0f.xxx);
        float Bl = st_lab(param_23).x - Xp.x;
        float3 param_24 = Xp;
        Ans param_25 = A;
        float param_26 = Bl + (pc_fine * F);
        Ov _1656 = ov_begin(param_24, param_25, param_26);
        g_v2 = _1656;
    }
    g_ofb = ofb;
    g_fa = fa;
}

float3 st_edit(float3 nE, float3 pE)
{
    float3 param = clamp(pE, 0.0f.xxx, 1.0f.xxx);
    float3 p = st_dec(param);
    float3 param_1 = clamp(nE, 0.0f.xxx, 1.0f.xxx);
    float3 n = st_dec(param_1);
    float3 e = log2(max(n, 0.0009765625f.xxx)) - log2(max(p, 0.0009765625f.xxx));
    float pm = max(p.x, max(p.y, p.z));
    float3 t = smoothstep((pm / 256.0f).xxx, (pm / 64.0f).xxx, p) * smoothstep(0.0009765625f, 0.015625f, pm);
    float3 param_2 = n * exp2((t * (pc_k - 1.0f)) * e);
    float3 _2226 = st_enc(param_2);
    return _2226;
}

float3 ocap_tail(uint2 px, float3 p, inout float3 o)
{
    bool _1364 = pc_ocap != 0.0f;
    bool _1378;
    if (_1364)
    {
        bool _1370 = pc_mode == 6u;
        bool _1377;
        if (!_1370)
        {
            _1377 = pc_mode == 8u;
        }
        else
        {
            _1377 = _1370;
        }
        _1378 = _1377;
    }
    else
    {
        _1378 = _1364;
    }
    if (_1378)
    {
        float3 own = p - Fine.Load(int3(int2(px), 0)).xyz;
        float3 param = own;
        float3 Xo = st_lab(param);
        float3 param_1 = o;
        float3 Y = st_lab(param_1);
        float2 dab = Y.yz - Xo.yz;
        float c0 = length(Xo.yz);
        float2 _1411;
        if (c0 > 9.9999997473787516355514526367188e-05f)
        {
            _1411 = Xo.yz / c0.xx;
        }
        else
        {
            _1411 = 0.0f.xx;
        }
        float2 u0 = _1411;
        float2 _1425;
        if (pc_ocap < 0.0f)
        {
            _1425 = u0 * min(dot(dab, u0), 0.0f);
        }
        else
        {
            _1425 = 0.0f.xx;
        }
        float2 desat = _1425;
        float2 rest = dab - desat;
        float lim = abs(pc_ocap) * (c0 + 0.00999999977648258209228515625f);
        float m = length(rest);
        if (m > 9.9999999747524270787835121154785e-07f)
        {
            float e = m / lim;
            float mo = m / pow(1.0f + (((e * e) * e) * e), 0.25f);
            float3 param_2 = float3(Y.x, (Xo.yz + desat) + (rest * (mo / m)));
            float3 l = st_lin(param_2);
            float3 param_3 = l;
            if (st_in(param_3))
            {
                float3 param_4 = l;
                float3 _1495 = st_enc(param_4);
                o = _1495;
            }
        }
    }
    return o;
}

float3 strengthened(uint2 px, float4 n, float3 p)
{
    float3 param = p;
    float3 X = st_lab(param);
    float3 param_1 = n.xyz;
    float3 param_2 = st_lab(param_1);
    Ans A = answer(param_2);
    float3 o = n.xyz;
    float mI;
    float vI;
    if (pc_mode == 9u)
    {
        float3 O = A.O;
        float dL = O.x - X.x;
        float2 param_3 = float2(px) + 0.5f.xx;
        float param_4 = X.x;
        uint2 param_5 = pc_low;
        float param_6;
        float param_7;
        float3 _1787 = st_gfq(CA, CB, param_3, param_4, param_5, param_6, param_7);
        mI = param_6;
        vI = param_7;
        float B = _1787.x;
        float F = FineH.Load(int3(int2(px), 0)).x;
        float _1811;
        if (pc_nohalo > 0.5f)
        {
            _1811 = 0.0f;
        }
        else
        {
            _1811 = pc_halo;
        }
        float amp = ((pc_base * B) + (pc_fine * F)) + (_1811 * ((dL - B) - F));
        float2 dab = O.yz - X.yz;
        float rel = (length(dab) / (length(X.yz) + 0.00999999977648258209228515625f)) / pc_chroma;
        float fl = pc_cfloor * smoothstep(0.4000000059604644775390625f, 0.64999997615814208984375f, O.x);
        float f = fl + ((1.0f - fl) / (1.0f + (rel * rel)));
        float tt = pc_k;
        Ans param_8 = A;
        float param_9 = amp;
        float2 param_10 = dab;
        float param_11 = f;
        float param_12 = pc_k;
        float3 lab = at7(param_8, param_9, param_10, param_11, param_12);
        float3 param_13 = lab;
        float3 l = st_lin(param_13);
        float3 param_14 = l;
        if (!st_in(param_14))
        {
            float lo = 1.0f;
            float hi = pc_k;
            for (int i = 0; i < 12; i++)
            {
                float m = 0.5f * (lo + hi);
                Ans param_15 = A;
                float param_16 = amp;
                float2 param_17 = dab;
                float param_18 = f;
                float param_19 = m;
                float3 param_20 = at7(param_15, param_16, param_17, param_18, param_19);
                float3 param_21 = st_lin(param_20);
                if (st_in(param_21))
                {
                    lo = m;
                }
                else
                {
                    hi = m;
                }
            }
            tt = lo;
            Ans param_22 = A;
            float param_23 = amp;
            float2 param_24 = dab;
            float param_25 = f;
            float param_26 = lo;
            lab = at7(param_22, param_23, param_24, param_25, param_26);
            float3 param_27 = lab;
            l = st_lin(param_27);
        }
        bool _1941 = pc_ring > 0.0f;
        bool _1947;
        if (_1941)
        {
            _1947 = pc_fine != 1.0f;
        }
        else
        {
            _1947 = _1941;
        }
        if (_1947)
        {
            float r0 = 1000000000.0f;
            float r1 = -1000000000.0f;
            for (int y = -1; y <= 1; y++)
            {
                for (int x = -1; x <= 1; x++)
                {
                    float v = FineH.Load(int3(clamp(int2(px) + int2(x, y), int2(0, 0), int2(pc_size) - int2(1, 1)), 0)).y;
                    r0 = min(r0, v);
                    r1 = max(r1, v);
                }
            }
            float param_28 = O.x;
            float param_29 = (tt - 1.0f) * (amp - ((pc_fine - 1.0f) * F));
            float lnf = st_soft(param_28, param_29);
            float _2013;
            if (pc_ring > 1.5f)
            {
                _2013 = max(lab.x, min(lnf, r0));
            }
            else
            {
                _2013 = clamp(lab.x, min(lnf, r0), max(lnf, r1));
            }
            lab.x = _2013;
            float3 param_30 = lab;
            float3 l2 = st_lin(param_30);
            float3 param_31 = l2;
            if (st_in(param_31))
            {
                l = l2;
            }
        }
        float3 param_32 = l;
        if (st_in(param_32))
        {
            float3 param_33 = l;
            float3 _2051 = st_enc(param_33);
            o = _2051;
        }
    }
    else
    {
        if (pc_mode == 6u)
        {
            float2 param_34 = float2(px) + 0.5f.xx;
            float param_35 = X.x;
            uint2 param_36 = pc_low;
            float param_37;
            float param_38;
            float3 _2078 = st_gfq(CA, CB, param_34, param_35, param_36, param_37, param_38);
            mI = param_37;
            vI = param_38;
            float3 param_39 = n.xyz;
            float3 param_40 = A.O;
            float3 param_41 = ((n.xyz - p) - _2078) * pc_k;
            float3 add = st_dadd(param_39, param_40, param_41);
            float3 param_42 = n.xyz;
            float3 param_43 = add;
            o = n.xyz + (add * st_fit(param_42, param_43));
            if (pc_nohalo > 0.0f)
            {
                float fF = pc_k * FineH.Load(int3(int2(px), 0)).x;
                float3 _2118;
                if (fF != 0.0f)
                {
                    float3 param_44 = A.O;
                    float param_45 = A.O.x;
                    float param_46 = fF;
                    _2118 = st_relight(param_44, param_45, param_46);
                }
                else
                {
                    _2118 = n.xyz;
                }
                float3 param_47 = o;
                float3 param_48 = _2118;
                o = st_nohalo(param_47, param_48);
            }
            bool _2140 = pc_nohalo > 0.0f;
            bool _2146;
            if (_2140)
            {
                _2146 = pc_ring > 0.0f;
            }
            else
            {
                _2146 = _2140;
            }
            if (_2146)
            {
                float3 param_49 = o;
                float3 param_50 = A.O;
                int2 param_51 = int2(px);
                o = st_fb_ring(param_49, param_50, param_51);
            }
        }
    }
    uint2 param_52 = px;
    float3 param_53 = p;
    float3 param_54 = o;
    float3 _2164 = ocap_tail(param_52, param_53, param_54);
    return _2164;
}

float3 ov_search(Ans A, float amp, float2 dab, float f)
{
    float lo = 1.0f;
    float hi = pc_k;
    for (int i = 0; i < 12; i++)
    {
        float m = 0.5f * (lo + hi);
        Ans param = A;
        float param_1 = amp;
        float2 param_2 = dab;
        float param_3 = f;
        float param_4 = m;
        float3 param_5 = at7(param, param_1, param_2, param_3, param_4);
        float3 param_6 = st_lin(param_5);
        if (st_in(param_6))
        {
            lo = m;
        }
        else
        {
            hi = m;
        }
    }
    Ans param_7 = A;
    float param_8 = amp;
    float2 param_9 = dab;
    float param_10 = f;
    float param_11 = lo;
    float3 param_12 = at7(param_7, param_8, param_9, param_10, param_11);
    return st_lin(param_12);
}

void ov_drain()
{
    GroupMemoryBarrierWithGroupSync();
    uint n = min(s_qn, 128u);
    Ans A;
    for (uint i = gl_LocalInvocationIndex; i < n; i += 256u)
    {
        float4 a = s_qa[i];
        float4 b = s_qb[i];
        float2 c = s_qc[i];
        A.O = a.xyz;
        A.co = c.y;
        A.uo = b.xy;
        Ans param = A;
        float param_1 = a.w;
        float2 param_2 = b.zw;
        float param_3 = c.x;
        float3 _1005 = ov_search(param, param_1, param_2, param_3);
        s_qa[i].x = _1005.x;
        s_qa[i].y = _1005.y;
        s_qa[i].z = _1005.z;
    }
    GroupMemoryBarrierWithGroupSync();
}

float3 ov_end(Ov v, Ans A, float3 o)
{
    float3 l = v.l;
    if (v.slot == 4294967295u)
    {
        Ans param = A;
        float param_1 = v.amp;
        float2 param_2 = v.dab;
        float param_3 = v.f;
        l = ov_search(param, param_1, param_2, param_3);
    }
    else
    {
        if (v.slot != 4294967294u)
        {
            l = s_qa[v.slot].xyz;
        }
    }
    float3 param_4 = l;
    float3 _1050;
    if (st_in(param_4))
    {
        float3 param_5 = l;
        float3 _1055 = st_enc(param_5);
        _1050 = _1055;
    }
    else
    {
        _1050 = o;
    }
    return _1050;
}

float3 hybrid_end(uint2 px, float4 n, float3 p, Ans A)
{
    Ov param = g_v1;
    Ans param_1 = A;
    float3 param_2 = n.xyz;
    float3 oov = ov_end(param, param_1, param_2);
    if (pc_nohalo > 0.0f)
    {
        Ov param_3 = g_v2;
        Ans param_4 = A;
        float3 param_5 = n.xyz;
        float3 param_6 = oov;
        float3 param_7 = ov_end(param_3, param_4, param_5);
        oov = st_nohalo(param_6, param_7);
    }
    uint2 param_8 = px;
    float3 param_9 = p;
    float3 param_10 = lerp(oov, g_ofb, clamp(g_fa.w, 0.0f, 1.0f).xxx);
    float3 _1702 = ocap_tail(param_8, param_9, param_10);
    return _1702;
}

float st_h(float x)
{
    float _1709;
    if (abs(x) >= 6.103515625e-05f)
    {
        _1709 = asfloat(asuint(x) & 4294959104u);
    }
    else
    {
        _1709 = trunc(x * 16777216.0f) * 5.9604644775390625e-08f;
    }
    return _1709;
}

float3 st_h(float3 x)
{
    float param = x.x;
    float param_1 = x.y;
    float param_2 = x.z;
    return float3(st_h(param), st_h(param_1), st_h(param_2));
}

void comp_main()
{
    uint2 px = gl_GlobalInvocationID.xy;
    bool inside = all(bool2(px.x < pc_size.x, px.y < pc_size.y));
    bool hybrid = pc_mode == 8u;
    if (hybrid)
    {
        if (gl_LocalInvocationIndex == 0u)
        {
            s_qn = 0u;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    float4 n = 0.0f.xxxx;
    float4 p = 0.0f.xxxx;
    float3 o = 0.0f.xxx;
    Ans ans;
    if (inside)
    {
        n = N[int2(px)];
        p = P[int2(px)];
        if (hybrid)
        {
            float3 param = n.xyz;
            float3 param_1 = st_lab(param);
            ans = answer(param_1);
            uint2 param_2 = px;
            float4 param_3 = n;
            float3 param_4 = p.xyz;
            Ans param_5 = ans;
            hybrid_begin(param_2, param_3, param_4, param_5);
        }
        else
        {
            float3 _2299;
            if (pc_mode == 10u)
            {
                float3 param_6 = n.xyz;
                float3 param_7 = p.xyz;
                _2299 = st_edit(param_6, param_7);
            }
            else
            {
                uint2 param_8 = px;
                float4 param_9 = n;
                float3 param_10 = p.xyz;
                _2299 = strengthened(param_8, param_9, param_10);
            }
            o = _2299;
        }
    }
    if (hybrid)
    {
        ov_drain();
        if (inside)
        {
            uint2 param_11 = px;
            float4 param_12 = n;
            float3 param_13 = p.xyz;
            Ans param_14 = ans;
            o = hybrid_end(param_11, param_12, param_13, param_14);
        }
    }
    if (inside)
    {
        N[int2(px)] = float4(o, n.w);
        if (pc_restored != 0u)
        {
            float3 back = p.xyz - Fine.Load(int3(int2(px), 0)).xyz;
            P[int2(px)] = float4(back, p.w);
            float3 param_15 = back;
            float3 pk = st_h(param_15);
            float3 param_16 = pk;
            float3 param_17 = o;
            s_iv[gl_LocalInvocationID.y][gl_LocalInvocationID.x] = float4(st_lab(param_16).x, st_h(param_17) - pk);
        }
    }
    if (pc_restored == 0u)
    {
        return;
    }
    GroupMemoryBarrierWithGroupSync();
    uint2 lid = gl_LocalInvocationID.xy;
    bool _2414 = lid.x >= 4u;
    bool _2421;
    if (!_2414)
    {
        _2421 = lid.y >= 4u;
    }
    else
    {
        _2421 = _2414;
    }
    if (_2421)
    {
        return;
    }
    uint2 q = (gl_WorkGroupID.xy * uint2(4u, 4u)) + lid;
    if (any(bool2(q.x >= pc_low.x, q.y >= pc_low.y)))
    {
        return;
    }
    float4 A = 0.0f.xxxx;
    float4 B = 0.0f.xxxx;
    float c = 0.0f;
    for (uint y = 0u; y < 4u; y++)
    {
        for (uint x = 0u; x < 4u; x++)
        {
            uint2 t = (q * uint2(4u, 4u)) + uint2(x, y);
            if (any(bool2(t.x >= pc_size.x, t.y >= pc_size.y)))
            {
                continue;
            }
            float4 iv = s_iv[t.y - (gl_WorkGroupID.y * 16u)][t.x - (gl_WorkGroupID.x * 16u)];
            float I = iv.x;
            float3 v = iv.yzw;
            A += float4(I, I * I, v.x, v.y);
            B += float4(v.z, v * I);
            c += 1.0f;
        }
    }
    OutA[int2(q)] = A / max(c, 1.0f).xxxx;
    OutB[int2(q)] = B / max(c, 1.0f).xxxx;
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    gl_LocalInvocationIndex = stage_input.gl_LocalInvocationIndex;
    comp_main();
}
