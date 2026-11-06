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
Texture2D<float4> t3 : register(t3);
SamplerState _t3_sampler : register(s3);
Texture2D<float4> t0 : register(t0);
SamplerState _t0_sampler : register(s0);
Texture2D<float4> t1 : register(t1);
SamplerState _t1_sampler : register(s1);
Texture2D<float4> t2 : register(t2);
SamplerState _t2_sampler : register(s2);
RWTexture2D<float4> u0 : register(u4);
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
    bool _1294 = p.x >= cb.Load(8);
    bool _1303;
    if (!_1294)
    {
        _1303 = p.y >= cb.Load(12);
    }
    else
    {
        _1303 = _1294;
    }
    return _1303;
}

float2 pixel_uv(uint2 p)
{
    return (float2(p) + 0.5f.xx) / float2(float(cb.Load(8)), float(cb.Load(12)));
}

float3 saturate3(float3 x)
{
    return clamp(x, 0.0f.xxx, 1.0f.xxx);
}

float3 lerp3v(float3 a, float3 b, float3 t)
{
    return a + (t * (b - a));
}

float3 srgb_decode(inout float3 v)
{
    float3 param = v;
    v = saturate3(param);
    float3 a = v / 12.9200000762939453125f.xxx;
    float3 b = pow((v + 0.054999999701976776123046875f.xxx) / 1.05499994754791259765625f.xxx, 2.400000095367431640625f.xxx);
    float3 param_1 = a;
    float3 param_2 = b;
    float3 param_3 = step(0.040449999272823333740234375f.xxx, v);
    return lerp3v(param_1, param_2, param_3);
}

bool finite1(float v)
{
    bool _293 = isnan(v);
    bool _299;
    if (!_293)
    {
        _299 = isinf(v);
    }
    else
    {
        _299 = _293;
    }
    return !_299;
}

float saturate1(float x)
{
    return clamp(x, 0.0f, 1.0f);
}

float lerp1(float a, float b, float t)
{
    return a + (t * (b - a));
}

float sanitize1(float v, float f)
{
    float param = v;
    return finite1(param) ? v : f;
}

float white_point()
{
    float wp = asfloat(cb.Load(4));
    float base = max(wp, 9.9999997473787516355514526367188e-05f);
    uint em = cb.Load(144);
    if ((em == 1u) || (em == 3u))
    {
        float measured = t3.Load(int3(int2(0, 0), 0)).x;
        float param = measured;
        if (finite1(param) && (measured > 9.9999999392252902907785028219223e-09f))
        {
            float _1113;
            if (em == 1u)
            {
                _1113 = asfloat(cb.Load(148)) / measured;
            }
            else
            {
                _1113 = measured;
            }
            base = _1113;
            float trim = asfloat(cb.Load(152));
            uint n = min(cb.Load(160), 8u);
            if (n > 0u)
            {
                trim = asfloat(cb.Load(180));
                for (uint i = 1u; i < 8u; i++)
                {
                    bool _1151 = i < n;
                    bool _1161;
                    if (_1151)
                    {
                        _1161 = base > asfloat(cb.Load((2u * (i - 1u)) * 4 + 176));
                    }
                    else
                    {
                        _1161 = _1151;
                    }
                    if (_1161)
                    {
                        float p0 = asfloat(cb.Load((2u * (i - 1u)) * 4 + 176));
                        float p1 = asfloat(cb.Load((2u * i) * 4 + 176));
                        float r0 = asfloat(cb.Load(((2u * (i - 1u)) + 1u) * 4 + 176));
                        float r1 = asfloat(cb.Load(((2u * i) + 1u) * 4 + 176));
                        float param_1 = log2(max(base, 9.9999999392252902907785028219223e-09f) / p0) / log2(p1 / p0);
                        float t = saturate1(param_1);
                        float param_2 = log2(r0);
                        float param_3 = log2(r1);
                        float param_4 = t;
                        trim = exp2(lerp1(param_2, param_3, param_4));
                    }
                }
            }
            base *= trim;
        }
    }
    float param_5 = base;
    float param_6 = wp;
    return max(sanitize1(param_5, param_6), 9.9999997473787516355514526367188e-05f);
}

float sum3(float3 a, float3 b)
{
    return mad(a.z, b.z, mad(a.x, b.x, a.y * b.y));
}

float luma(float3 c)
{
    float3 param = c;
    float3 param_1 = float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f);
    return sum3(param, param_1);
}

float maxch(float3 c)
{
    return max(c.x, max(c.y, c.z));
}

float3 knee(inout float3 d)
{
    if (cb.Load(32) != 0u)
    {
        return d;
    }
    float3 param = d;
    float L = luma(param);
    if (L > 0.75f)
    {
        float rolled = 0.75f + (0.25f * (1.0f - exp((-(L - 0.75f)) / 0.25f)));
        d *= (rolled / L);
    }
    float3 param_1 = d;
    float peak = maxch(param_1);
    if (peak > 1.0f)
    {
        d /= peak.xxx;
    }
    return d;
}

float hybrid(float m)
{
    if (m <= 0.75f)
    {
        return m;
    }
    float e = (m - 0.75f) / 0.25f;
    return 0.75f + (0.25f * (e * rsqrt((e * e) + 1.0f)));
}

float3 hybrid_encode(inout float3 v)
{
    v = max(v, 0.0f.xxx);
    float3 param = v;
    float m = maxch(param);
    if (m <= 9.9999999747524270787835121154785e-07f)
    {
        return v;
    }
    float param_1 = m;
    return v * (hybrid(param_1) / m);
}

float3 neutwo_encode(inout float3 v)
{
    v = max(v, 0.0f.xxx);
    float3 param = v;
    float m = maxch(param);
    if (m <= 9.9999999747524270787835121154785e-07f)
    {
        return v;
    }
    return v * rsqrt((m * m) + 1.0f);
}

float3 full_proxy(float3 O)
{
    if (cb.Load(32) != 0u)
    {
        float3 param = O;
        return saturate3(param);
    }
    uint rm = cb.Load(76);
    if (rm == 0u)
    {
        float3 param_1 = O;
        float3 _1053 = knee(param_1);
        float3 param_2 = _1053;
        return saturate3(param_2);
    }
    float3 _1060;
    if (rm >= 3u)
    {
        float3 param_3 = O;
        float3 _1065 = hybrid_encode(param_3);
        _1060 = _1065;
    }
    else
    {
        float3 param_4 = O;
        float3 _1069 = neutwo_encode(param_4);
        _1060 = _1069;
    }
    return _1060;
}

float3 sanitize3(float3 v, float3 f)
{
    float param = v.x;
    float param_1 = f.x;
    float param_2 = v.y;
    float param_3 = f.y;
    float param_4 = v.z;
    float param_5 = f.z;
    return float3(sanitize1(param, param_1), sanitize1(param_2, param_3), sanitize1(param_4, param_5));
}

float3 bound_carrier(float2 uv, float3 cand)
{
    uint _1637_dummy_parameter;
    int2 wh = int2(spvTextureSize(t0, uint(0), _1637_dummy_parameter));
    int2 b = int2(floor((uv * float2(wh)) - 0.5f.xx));
    float3 lo = 1.0f.xxx;
    float3 hi = 0.0f.xxx;
    for (int ty = 0; ty <= 1; ty++)
    {
        for (int tx = 0; tx <= 1; tx++)
        {
            float3 param = t0.Load(int3(clamp(b + int2(tx, ty), int2(0, 0), wh - int2(1, 1)), 0)).xyz;
            float3 param_1 = 0.5f.xxx;
            float3 tap = sanitize3(param, param_1);
            lo = min(lo, tap);
            hi = max(hi, tap);
        }
    }
    float3 param_2 = t0.SampleLevel(_t0_sampler, uv, 0.0f).xyz;
    float3 param_3 = 0.5f.xxx;
    float3 ref = clamp(sanitize3(param_2, param_3), lo, hi);
    float3 param_4 = cand;
    float3 param_5 = ref;
    float3 d = sanitize3(param_4, param_5) - ref;
    float a = 1.0f;
    for (int c = 0; c < 3; c++)
    {
        if (d[c] > 0.0f)
        {
            a = min(a, (hi[c] - ref[c]) / d[c]);
        }
        else
        {
            if (d[c] < 0.0f)
            {
                a = min(a, (lo[c] - ref[c]) / d[c]);
            }
        }
    }
    float param_6 = a;
    return clamp(ref + (d * saturate1(param_6)), lo, hi);
}

float3 field_decode(float3 c)
{
    float3 param = c;
    float3 param_1 = 0.5f.xxx;
    float3 d = clamp((sanitize3(param, param_1) * 2.0f) - 1.0f.xxx, (-0.999000012874603271484375f).xxx, 0.999000012874603271484375f.xxx);
    return d / (1.0f.xxx - abs(d));
}

float3 lin(float3 c)
{
    float3 _1368;
    if (cb.Load(32) != 0u)
    {
        _1368 = c;
    }
    else
    {
        float3 param = c;
        float3 _1375 = srgb_decode(param);
        _1368 = _1375;
    }
    return _1368;
}

float2 chroma(float3 c)
{
    float3 param = c;
    float y = luma(param);
    float2 _1348;
    if (y > 9.9999999747524270787835121154785e-07f)
    {
        _1348 = float2(c.x / y, c.z / y);
    }
    else
    {
        _1348 = 1.0f.xx;
    }
    return _1348;
}

float3 paired_field(int2 p, int2 size)
{
    float3 param = t0.Load(int3(p, 0)).xyz;
    float3 s = lin(param);
    float3 param_1 = t1.Load(int3(p, 0)).xyz;
    float3 m = lin(param_1);
    float3 param_2 = s;
    float sY = luma(param_2);
    float3 param_3 = m;
    float mY = luma(param_3);
    if (sY < 0.0009765625f)
    {
        sY = 0.0f;
        mY = 0.0f;
        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                int2 q = clamp(p + int2(dx, dy), int2(0, 0), size - int2(1, 1));
                float3 param_4 = t0.Load(int3(q, 0)).xyz;
                float3 param_5 = lin(param_4);
                sY += luma(param_5);
                float3 param_6 = t1.Load(int3(q, 0)).xyz;
                float3 param_7 = lin(param_6);
                mY += luma(param_7);
            }
        }
    }
    float3 param_8 = m;
    float2 cm = chroma(param_8);
    float3 param_9 = s;
    float2 cs = chroma(param_9);
    float3 param_10 = float3((mY - sY) / max(sY, 0.0009765625f), cm.x - cs.x, cm.y - cs.y);
    float3 param_11 = 0.0f.xxx;
    return sanitize3(param_10, param_11);
}

float3 sample_field(float2 uv, bool encoded)
{
    uint _1496_dummy_parameter;
    int2 wh = int2(spvTextureSize(t0, uint(0), _1496_dummy_parameter));
    float2 q = (uv * float2(wh)) - 0.5f.xx;
    float2 b = floor(q);
    float2 f = q - floor(q);
    float3 field = 0.0f.xxx;
    float3 _1542;
    float _1562;
    float _1575;
    for (int ty = 0; ty <= 1; ty++)
    {
        for (int tx = 0; tx <= 1; tx++)
        {
            int2 p = clamp(int2(b) + int2(tx, ty), int2(0, 0), wh - int2(1, 1));
            if (encoded)
            {
                float3 param = t0.Load(int3(p, 0)).xyz;
                _1542 = field_decode(param);
            }
            else
            {
                int2 param_1 = p;
                int2 param_2 = wh;
                _1542 = paired_field(param_1, param_2);
            }
            float3 tap = _1542;
            if (tx == 1)
            {
                _1562 = f.x;
            }
            else
            {
                _1562 = 1.0f - f.x;
            }
            if (ty == 1)
            {
                _1575 = f.y;
            }
            else
            {
                _1575 = 1.0f - f.y;
            }
            field += ((tap * _1562) * _1575);
        }
    }
    return field;
}

float3 restore_field(float3 base, float3 field)
{
    float3 param = base;
    float y = max(0.0f, luma(param) * (1.0f + field.x));
    float3 param_1 = base;
    float2 c = field.yz + chroma(param_1);
    float r = c.x * y;
    float b = c.y * y;
    return float3(r, ((y - (0.2125999927520751953125f * r)) - (0.072200000286102294921875f * b)) / 0.715200006961822509765625f, b);
}

float3 reconstruct(float3 base, float2 uv, float3 carrier)
{
    bool dlss = cb.Load(68) == 4u;
    float3 _2043;
    if (dlss)
    {
        float2 param = uv;
        float3 param_1 = carrier;
        float3 param_2 = bound_carrier(param, param_1);
        _2043 = field_decode(param_2);
    }
    else
    {
        float2 param_3 = uv;
        bool param_4 = false;
        _2043 = sample_field(param_3, param_4);
    }
    float3 field = _2043;
    float3 param_5 = base;
    float3 param_6 = field;
    float3 r = restore_field(param_5, param_6);
    uint rm = cb.Load(76);
    bool _2075;
    if (dlss)
    {
        _2075 = cb.Load(32) == 0u;
    }
    else
    {
        _2075 = dlss;
    }
    bool _2083;
    if (_2075)
    {
        _2083 = (rm == 2u) || (rm == 4u);
    }
    else
    {
        _2083 = _2075;
    }
    if (_2083)
    {
        float2 param_7 = uv;
        bool param_8 = true;
        float3 param_9 = base;
        float3 param_10 = sample_field(param_7, param_8);
        float3 ref = restore_field(param_9, param_10);
        bool _2099 = any(bool3(r.x >= 1.0f.xxx.x, r.y >= 1.0f.xxx.y, r.z >= 1.0f.xxx.z));
        bool _2105;
        if (_2099)
        {
            _2105 = all(bool3(ref.x < 1.0f.xxx.x, ref.y < 1.0f.xxx.y, ref.z < 1.0f.xxx.z));
        }
        else
        {
            _2105 = _2099;
        }
        if (_2105)
        {
            r = ref;
        }
    }
    float3 param_11 = r;
    float param_12 = luma(param_11);
    float y = saturate1(param_12);
    float3 d = r - y.xxx;
    float a = 1.0f;
    for (int c = 0; c < 3; c++)
    {
        if (d[c] > 0.0f)
        {
            a = min(a, (1.0f - y) / d[c]);
        }
        else
        {
            if (d[c] < 0.0f)
            {
                a = min(a, (-y) / d[c]);
            }
        }
    }
    float param_13 = a;
    float3 param_14 = y.xxx + (d * saturate1(param_13));
    return saturate3(param_14);
}

float3 model_value(float3 v)
{
    float3 _1776;
    if (cb.Load(32) != 0u)
    {
        _1776 = v;
    }
    else
    {
        float3 param = v;
        float3 _1783 = srgb_decode(param);
        _1776 = _1783;
    }
    return _1776;
}

float3 lighting(uint2 xy, float3 P)
{
    uint _1790_dummy_parameter;
    int2 wh = int2(spvTextureSize(t0, uint(0), _1790_dummy_parameter));
    float2 p = ((float2(xy) + 0.5f.xx) * (float2(wh) / float2(float(cb.Load(8)), float(cb.Load(12))))) - 0.5f.xx;
    int2 base = int2(floor(p));
    float2 f = p - float2(base);
    int2 hi = wh - int2(1, 1);
    float yp = dot(P, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
    float lp = log2(yp + 0.00390625f);
    float wg = 0.0f;
    float ws_sum = 0.0f;
    float g_g = 0.0f;
    float g_s = 0.0f;
    float3 c_g = 0.0f.xxx;
    float3 c_s = 0.0f.xxx;
    float3 r_g = 0.0f.xxx;
    float3 r_s = 0.0f.xxx;
    for (int j = -1; j <= 2; j++)
    {
        for (int i = -1; i <= 2; i++)
        {
            int2 q = clamp(base + int2(i, j), int2(0, 0), hi);
            float3 param = t1.Load(int3(q, 0)).xyz;
            float3 m = model_value(param);
            float3 param_1 = t0.Load(int3(q, 0)).xyz;
            float3 s = model_value(param_1);
            float ym = dot(m, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
            float ys = dot(s, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
            float g = log2((ym + 0.00390625f) / (ys + 0.00390625f));
            float3 dc = (m / (ym + 0.00390625f).xxx) - (s / (ys + 0.00390625f).xxx);
            float3 res = m - s;
            float2 d = abs(float2(float(i), float(j)) - f);
            float ws = exp((-dot(d, d)) * 1.388888835906982421875f);
            float dl = lp - log2(ys + 0.00390625f);
            float w = ws * exp(((-dl) * dl) * 8.0f);
            g_g += (w * g);
            c_g += (dc * w);
            r_g += (res * w);
            wg += w;
            g_s += (ws * g);
            c_s += (dc * ws);
            r_s += (res * ws);
            ws_sum += ws;
        }
    }
    float G;
    float3 DC;
    float3 RES;
    if (wg > (9.9999997473787516355514526367188e-05f * ws_sum))
    {
        G = g_g / wg;
        DC = c_g / wg.xxx;
        RES = r_g / wg.xxx;
    }
    else
    {
        G = g_s / ws_sum;
        DC = c_s / ws_sum.xxx;
        RES = r_s / ws_sum.xxx;
    }
    float3 rebuilt = max(P + (DC * (yp + 0.00390625f)), 0.0f.xxx) * exp2(G);
    float3 added = P + RES;
    return lerp(added, rebuilt, smoothstep(0.00390625f, 0.03125f, yp).xxx);
}

float3 cube_scale(float3 P, float3 T)
{
    if (cb.Load(32) != 0u)
    {
        return T;
    }
    float3 d = T - P;
    float alpha = 1.0f;
    for (int c = 0; c < 3; c++)
    {
        if (d[c] > 9.9999999747524270787835121154785e-07f)
        {
            alpha = min(alpha, (1.0f - P[c]) / d[c]);
        }
        else
        {
            if (d[c] < (-9.9999999747524270787835121154785e-07f))
            {
                alpha = min(alpha, (0.0f - P[c]) / d[c]);
            }
        }
    }
    float param = alpha;
    return P + (d * saturate1(param));
}

float3 lerp3(float3 a, float3 b, float t)
{
    return a + ((b - a) * t);
}

float3 mat_vec(float3 r0, float3 r1, float3 r2, float3 v)
{
    float3 param = r0;
    float3 param_1 = v;
    float3 param_2 = r1;
    float3 param_3 = v;
    float3 param_4 = r2;
    float3 param_5 = v;
    return float3(sum3(param, param_1), sum3(param_2, param_3), sum3(param_4, param_5));
}

float3 cbrt_signed(float3 x)
{
    return sign(x) * pow(abs(x), 0.3333333432674407958984375f.xxx);
}

float3 to_oklab(float3 c)
{
    float3 param = float3(0.4122214615345001220703125f, 0.536332547664642333984375f, 0.0514459945261478424072265625f);
    float3 param_1 = float3(0.21190349757671356201171875f, 0.680699527263641357421875f, 0.10739696025848388671875f);
    float3 param_2 = float3(0.0883024632930755615234375f, 0.28171885013580322265625f, 0.629978716373443603515625f);
    float3 param_3 = c;
    float3 lms = mat_vec(param, param_1, param_2, param_3);
    float3 param_4 = lms;
    float3 l = cbrt_signed(param_4);
    float3 param_5 = float3(0.2104542553424835205078125f, 0.793617784976959228515625f, -0.004072046838700771331787109375f);
    float3 param_6 = float3(1.9779984951019287109375f, -2.428592205047607421875f, 0.4505937099456787109375f);
    float3 param_7 = float3(0.025904037058353424072265625f, 0.782771766185760498046875f, -0.8086757659912109375f);
    float3 param_8 = l;
    return mat_vec(param_5, param_6, param_7, param_8);
}

float3 from_oklab(float3 L)
{
    float3 param = float3(1.0f, 0.3963377773761749267578125f, 0.21580375730991363525390625f);
    float3 param_1 = float3(1.0f, -0.1055613458156585693359375f, -0.06385417282581329345703125f);
    float3 param_2 = float3(1.0f, -0.089484177529811859130859375f, -1.2914855480194091796875f);
    float3 param_3 = L;
    float3 l = mat_vec(param, param_1, param_2, param_3);
    float3 lms = (l * l) * l;
    float3 param_4 = float3(4.076741695404052734375f, -3.30771160125732421875f, 0.2309699356555938720703125f);
    float3 param_5 = float3(-1.26843798160552978515625f, 2.60975742340087890625f, -0.341319382190704345703125f);
    float3 param_6 = float3(-0.0041960864327847957611083984375f, -0.70341861248016357421875f, 1.7076146602630615234375f);
    float3 param_7 = lms;
    return mat_vec(param_4, param_5, param_6, param_7);
}

float3 bt709_to_lms(float3 c)
{
    float3 param = float3(0.30569589138031005859375f, 0.6227128505706787109375f, 0.0452863611280918121337890625f);
    float3 param_1 = float3(0.1577626168727874755859375f, 0.7696859836578369140625f, 0.0880703032016754150390625f);
    float3 param_2 = float3(0.01933082006871700286865234375f, 0.119194783270359039306640625f, 0.950532138347625732421875f);
    float3 param_3 = c;
    return mat_vec(param, param_1, param_2, param_3);
}

float sum3_split(float3 a, float3 b)
{
    precise float _243 = a.x * b.x;
    precise float _248 = a.y * b.y;
    precise float _249 = _243 + _248;
    float xy = _249;
    return mad(a.z, b.z, xy);
}

float luma_split(float3 c)
{
    float3 param = c;
    float3 param_1 = float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f);
    return sum3_split(param, param_1);
}

float3 neutral_of(float3 A, float Y)
{
    return Y.xxx;
}

float safe_div(float a, float b, float f)
{
    float _597;
    if (abs(b) > 9.9999999392252902907785028219223e-09f)
    {
        _597 = a / b;
    }
    else
    {
        _597 = f;
    }
    return _597;
}

float compression_scale(float3 c, float3 A)
{
    float3 param = c;
    float3 param_1 = 0.0f.xxx;
    float3 cs = sanitize3(param, param_1);
    float3 param_2 = cs;
    float y = luma_split(param_2);
    if (!(y > 9.9999999392252902907785028219223e-09f))
    {
        return 1.0f;
    }
    float3 param_3 = A;
    float param_4 = y;
    float3 n = neutral_of(param_3, param_4);
    float k = 1.0f;
    for (int i = 0; i < 3; i++)
    {
        bool _644 = cs[i] < 0.0f;
        bool _654;
        if (_644)
        {
            _654 = n[i] > cs[i];
        }
        else
        {
            _654 = _644;
        }
        if (_654)
        {
            float param_5 = n[i];
            float param_6 = n[i] - cs[i];
            float param_7 = 1.0f;
            k = min(k, safe_div(param_5, param_6, param_7));
        }
    }
    float param_8 = k;
    float param_9 = 1.0f;
    float param_10 = sanitize1(param_8, param_9);
    return saturate1(param_10);
}

float3 clamp_ap1(float3 c)
{
    float3 param = 0.180000007152557373046875f.xxx;
    float3 A = bt709_to_lms(param);
    float3 param_1 = c;
    float3 param_2 = A;
    float k = compression_scale(param_1, param_2);
    if (k >= 1.0f)
    {
        return c;
    }
    float3 param_3 = c;
    float y = luma_split(param_3);
    float3 param_4 = A;
    float param_5 = y;
    float3 n = neutral_of(param_4, param_5);
    float3 param_6 = n + ((c - n) * k);
    float3 param_7 = max(n, 0.0f.xxx);
    return sanitize3(param_6, param_7);
}

float3 hue_oklab(float3 inc, float3 cor)
{
    float3 param = inc;
    float3 Li = to_oklab(param);
    float3 param_1 = cor;
    float3 Lc = to_oklab(param_1);
    float ci = length(Li.yz);
    float cc = length(Lc.yz);
    float2 _745;
    if (cc > 9.9999997473787516355514526367188e-06f)
    {
        _745 = Lc.yz / cc.xx;
    }
    else
    {
        _745 = 0.0f.xx;
    }
    float2 direction = _745;
    float2 _758 = direction * ci;
    Li.y = _758.x;
    Li.z = _758.y;
    float3 param_2 = Li;
    float3 param_3 = from_oklab(param_2);
    return clamp_ap1(param_3);
}

float3 neutwo_decode(inout float3 y)
{
    y = max(y, 0.0f.xxx);
    float3 param = y;
    float m = min(maxch(param), 0.999998986721038818359375f);
    if (m <= 9.9999999747524270787835121154785e-07f)
    {
        return y;
    }
    return y * rsqrt(max(1.0f - (m * m), 9.9999999392252902907785028219223e-09f));
}

float hybrid_inv(float y)
{
    if (y <= 0.75f)
    {
        return y;
    }
    float u = min((y - 0.75f) / 0.25f, 0.999998986721038818359375f);
    float e = u * rsqrt(max(1.0f - (u * u), 9.9999999392252902907785028219223e-09f));
    return 0.75f + (0.25f * e);
}

float3 hybrid_decode(inout float3 y)
{
    y = max(y, 0.0f.xxx);
    float3 param = y;
    float m = maxch(param);
    if (m <= 9.9999999747524270787835121154785e-07f)
    {
        return y;
    }
    float param_1 = m;
    return y * (hybrid_inv(param_1) / m);
}

float3 srgb_encode(inout float3 v)
{
    float3 param = v;
    v = saturate3(param);
    float3 a = v * 12.9200000762939453125f;
    float3 b = (pow(max(v, 9.9999999392252902907785028219223e-09f.xxx), 0.4166666567325592041015625f.xxx) * 1.05499994754791259765625f) - 0.054999999701976776123046875f.xxx;
    float3 param_1 = a;
    float3 param_2 = b;
    float3 param_3 = step(0.003130800090730190277099609375f.xxx, v);
    return lerp3v(param_1, param_2, param_3);
}

float minch(float3 c)
{
    return min(c.x, min(c.y, c.z));
}

float smoothstep1(float e0, float e1, float x)
{
    float param = (x - e0) / (e1 - e0);
    float t = saturate1(param);
    return (t * t) * (3.0f - (2.0f * t));
}

float skin_weight(inout float3 rgb)
{
    float3 param = rgb;
    rgb = saturate3(param);
    float y = ((0.2989999949932098388671875f * rgb.x) + (0.58700001239776611328125f * rgb.y)) + (0.114000000059604644775390625f * rgb.z);
    float cb_1 = ((rgb.z - y) * 0.56400001049041748046875f) + 0.5f;
    float cr = ((rgb.x - y) * 0.712999999523162841796875f) + 0.5f;
    float2 dist = float2((cb_1 - 0.4050000011920928955078125f) / 0.0900000035762786865234375f, (cr - 0.60000002384185791015625f) / 0.10999999940395355224609375f);
    float3 param_1 = rgb;
    float3 param_2 = rgb;
    float chroma_1 = maxch(param_1) - minch(param_2);
    float param_3 = 0.550000011920928955078125f;
    float param_4 = 1.35000002384185791015625f;
    float param_5 = length(dist);
    float param_6 = 0.0199999995529651641845703125f;
    float param_7 = 0.100000001490116119384765625f;
    float param_8 = chroma_1;
    return (1.0f - smoothstep1(param_3, param_4, param_5)) * smoothstep1(param_6, param_7, param_8);
}

void comp_main()
{
    uint2 p = gl_GlobalInvocationID.xy;
    uint2 param = p;
    if (outside(param))
    {
        return;
    }
    uint2 param_1 = p;
    float2 uv = pixel_uv(param_1);
    float4 Ps = t0.SampleLevel(_t0_sampler, uv, 0.0f);
    float4 Ms = t1.SampleLevel(_t1_sampler, uv, 0.0f);
    bool passthrough = cb.Load(32) != 0u;
    float3 _2203;
    if (passthrough)
    {
        _2203 = Ps.xyz;
    }
    else
    {
        float3 param_2 = Ps.xyz;
        float3 _2212 = srgb_decode(param_2);
        _2203 = _2212;
    }
    float3 P = _2203;
    float3 _2216;
    if (passthrough)
    {
        _2216 = Ms.xyz;
    }
    else
    {
        float3 param_3 = Ms.xyz;
        float3 _2225 = srgb_decode(param_3);
        _2216 = _2225;
    }
    float3 M = _2216;
    float3 Md = M;
    float4 Os = t2.Load(int3(int2(p), 0));
    float _2238;
    if (passthrough)
    {
        _2238 = 1.0f;
    }
    else
    {
        _2238 = white_point();
    }
    float N = _2238;
    float3 O = Os.xyz / N.xxx;
    float3 param_4 = O;
    float YO = luma(param_4);
    float3 param_5 = P;
    float YP = luma(param_5);
    if (cb.Load(80) == 0u)
    {
        u0[int2(p)] = float4(max(Os.xyz, 0.0f.xxx), Os.w);
        return;
    }
    if (cb.Load(24) == 1u)
    {
        u0[int2(p)] = float4(P * asfloat(cb.Load(72)), Os.w);
        return;
    }
    if (cb.Load(24) == 2u)
    {
        u0[int2(p)] = float4(M * asfloat(cb.Load(72)), Os.w);
        return;
    }
    uint _2323_dummy_parameter;
    int2 ps = int2(spvTextureSize(t0, uint(0), _2323_dummy_parameter));
    bool _2330 = uint(ps.x) != cb.Load(8);
    bool _2340;
    if (!_2330)
    {
        _2340 = uint(ps.y) != cb.Load(12);
    }
    else
    {
        _2340 = _2330;
    }
    bool small = _2340;
    bool _2343 = cb.Load(68) == 3u;
    bool _2350;
    if (!_2343)
    {
        _2350 = cb.Load(68) == 4u;
    }
    else
    {
        _2350 = _2343;
    }
    bool _2369;
    if (_2350)
    {
        bool _2358 = uint(ps.x) < cb.Load(8);
        bool _2368;
        if (!_2358)
        {
            _2368 = uint(ps.y) < cb.Load(12);
        }
        else
        {
            _2368 = _2358;
        }
        _2369 = _2368;
    }
    else
    {
        _2369 = _2350;
    }
    if (_2369)
    {
        float3 param_6 = O;
        P = full_proxy(param_6);
        float3 param_7 = P;
        float2 param_8 = uv;
        float3 param_9 = Ms.xyz;
        M = reconstruct(param_7, param_8, param_9);
        Md = M;
        float3 param_10 = P;
        YP = luma(param_10);
    }
    if ((cb.Load(68) == 5u) && small)
    {
        float3 param_11 = O;
        P = full_proxy(param_11);
        uint2 param_12 = p;
        float3 param_13 = P;
        float3 target = lighting(param_12, param_13);
        Md = target;
        float3 param_14 = P;
        float3 param_15 = target;
        M = cube_scale(param_14, param_15);
        float3 param_16 = P;
        YP = luma(param_16);
    }
    float3 E = M - P;
    if (cb.Load(68) == 2u)
    {
        float3 param_17 = Ms.xyz;
        float3 param_18 = 0.5f.xxx;
        float3 s = clamp((sanitize3(param_17, param_18) * 2.0f) - 1.0f.xxx, (-0.999000012874603271484375f).xxx, 0.999000012874603271484375f.xxx);
        E = (s * 0.015625f) / (1.0f.xxx - abs(s));
    }
    if (cb.Load(24) == 3u)
    {
        float3 param_19 = 0.5f.xxx + (E * 20.0f);
        float3 param_20 = saturate3(param_19);
        float3 _2456 = srgb_decode(param_20);
        u0[int2(p)] = float4(_2456 * asfloat(cb.Load(72)), Os.w);
        return;
    }
    bool _2471 = (cb.Load(68) == 1u) && small;
    bool _2478;
    if (!_2471)
    {
        _2478 = cb.Load(68) == 2u;
    }
    else
    {
        _2478 = _2471;
    }
    if (_2478)
    {
        float3 param_21 = O;
        P = full_proxy(param_21);
        float3 param_22 = P;
        YP = luma(param_22);
        float3 param_23 = P;
        float3 param_24 = P + E;
        M = cube_scale(param_23, param_24);
        if (cb.Load(68) == 2u)
        {
            Md = M;
        }
    }
    if (cb.Load(84) != 0u)
    {
        float3 Pf = max(P, 0.0009765625f.xxx);
        float3 lr = log2(max(M, 0.0009765625f.xxx)) - log2(Pf);
        float l = dot(lr, float3(0.2126390039920806884765625f, 0.715169012546539306640625f, 0.07219199836254119873046875f));
        float3 c = lr - l.xxx;
        float3 _2530 = abs(c);
        if (any(bool3(_2530.x > 1.0f.xxx.x, _2530.y > 1.0f.xxx.y, _2530.z > 1.0f.xxx.z)))
        {
            M = Pf * exp2(l.xxx + clamp(c, (-1.0f).xxx, 1.0f.xxx));
            bool _2547 = cb.Load(68) != 2u;
            bool _2553;
            if (_2547)
            {
                _2553 = cb.Load(68) != 5u;
            }
            else
            {
                _2553 = _2547;
            }
            if (_2553)
            {
                Md = M;
            }
        }
    }
    float T = asfloat(cb.Load(16));
    float3 param_25 = M;
    float YM = luma(param_25);
    float3 U;
    if (YM <= 9.9999997473787516355514526367188e-06f)
    {
        U = O;
    }
    else
    {
        if ((cb.Load(68) == 6u) && small)
        {
            float3 param_26 = O;
            float3 param_27 = full_proxy(param_26);
            float headroom = max(0.0f, YO - luma(param_27));
            float param_28 = T;
            float3 param_29 = O;
            float3 param_30 = M * ((YM + headroom) / YM);
            float param_31 = saturate1(param_28);
            U = lerp3(param_29, param_30, param_31);
        }
        else
        {
            float _2609;
            if (YO < YP)
            {
                _2609 = YO / max(YP, 9.9999999747524270787835121154785e-07f);
            }
            else
            {
                _2609 = (YM + max(0.0f, YO - YP)) / YM;
            }
            float ratio = _2609;
            float3 param_32 = M * ratio;
            float3 param_33 = M;
            float param_34 = T;
            float3 param_35 = O;
            float3 param_36 = hue_oklab(param_32, param_33);
            float param_37 = saturate1(param_34);
            U = lerp3(param_35, param_36, param_37);
        }
    }
    float3 param_38 = U;
    float3 param_39 = float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f);
    float YU = sum3_split(param_38, param_39);
    float q = (YU + 0.001953125f) / (YO + 0.001953125f);
    float a = pow(max(q, 9.9999999747524270787835121154785e-07f), 1.0f + max(T - 1.0f, 0.0f));
    float g = max(asfloat(cb.Load(28)), 1.0f);
    float b = clamp(a, 1.0f / g, g);
    float guard_scale = b / max(q, 9.9999999747524270787835121154785e-07f);
    bool _2679 = cb.Load(84) != 0u;
    bool _2686;
    if (_2679)
    {
        _2686 = a < (1.0f / g);
    }
    else
    {
        _2686 = _2679;
    }
    if ((_2686 && (guard_scale > 1.0f)) && (YO > 9.9999999747524270787835121154785e-07f))
    {
        U += (O * ((max(YU, 0.0f) * (guard_scale - 1.0f)) / YO));
    }
    else
    {
        U *= guard_scale;
    }
    float C = asfloat(cb.Load(20));
    float3 param_40 = O * b;
    float3 param_41 = U;
    float param_42 = min(C, 1.0f);
    float3 R = lerp3(param_40, param_41, param_42);
    if (C > 1.0f)
    {
        float3 param_43 = max(R, 0.0f.xxx);
        float3 param_44 = float3(1.0f, C, C) * to_oklab(param_43);
        float3 param_45 = from_oklab(param_44);
        R = clamp_ap1(param_45);
    }
    uint rm = cb.Load(76);
    if (rm == 2u)
    {
        float3 _2748;
        if (passthrough)
        {
            _2748 = Md;
        }
        else
        {
            float3 param_46 = Md;
            float3 _2755 = neutwo_decode(param_46);
            _2748 = _2755;
        }
        R = _2748;
    }
    else
    {
        if (rm == 4u)
        {
            float3 _2763;
            if (passthrough)
            {
                _2763 = Md;
            }
            else
            {
                float3 param_47 = Md;
                float3 _2770 = hybrid_decode(param_47);
                _2763 = _2770;
            }
            R = _2763;
        }
    }
    bool _2776 = (rm == 2u) || (rm == 4u);
    bool _2783;
    if (_2776)
    {
        _2783 = asfloat(cb.Load(136)) > 0.0f;
    }
    else
    {
        _2783 = _2776;
    }
    bool _2789;
    if (_2783)
    {
        _2789 = asfloat(cb.Load(136)) < 0.999000012874603271484375f;
    }
    else
    {
        _2789 = _2783;
    }
    bool _2796;
    if (_2789)
    {
        _2796 = asfloat(cb.Load(132)) > 0.0f;
    }
    else
    {
        _2796 = _2789;
    }
    if (_2796)
    {
        float k = clamp(round(1.0f / asfloat(cb.Load(136))), 1.0f, 4.0f);
        float2 tap = float2(k / float(cb.Load(8)), k / float(cb.Load(12)));
        float3 nb = ((t2.SampleLevel(_t2_sampler, uv + float2(tap.x, 0.0f), 0.0f).xyz + t2.SampleLevel(_t2_sampler, uv - float2(tap.x, 0.0f), 0.0f).xyz) + t2.SampleLevel(_t2_sampler, uv + float2(0.0f, tap.y), 0.0f).xyz) + t2.SampleLevel(_t2_sampler, uv - float2(0.0f, tap.y), 0.0f).xyz;
        float3 param_48 = O + (nb / N.xxx);
        float blur = luma(param_48) / 5.0f;
        float contrast = (YO - blur) / (max(YO, blur) + 0.001953125f);
        R *= exp2(clamp(asfloat(cb.Load(132)), 0.0f, 2.0f) * clamp(contrast, -1.0f, 1.0f));
    }
    if ((rm == 2u) || (rm == 4u))
    {
        float3 param_49 = R;
        float q_1 = (luma(param_49) + 0.001953125f) / (YO + 0.001953125f);
        R *= (clamp(q_1, 1.0f / g, g) / max(q_1, 9.9999999747524270787835121154785e-07f));
    }
    R *= N;
    if (cb.Load(92) != 0u)
    {
        float3 _2919;
        if (passthrough)
        {
            _2919 = O;
        }
        else
        {
            float3 param_50 = O;
            float3 param_51 = saturate3(param_50);
            float3 _2928 = srgb_encode(param_51);
            _2919 = _2928;
        }
        float3 disp = _2919;
        float3 param_52 = disp;
        float _2933 = skin_weight(param_52);
        float m = _2933;
        float param_53 = asfloat(cb.Load(108));
        float param_54 = asfloat(cb.Load(100));
        float param_55 = m;
        float det = lerp1(param_53, param_54, param_55);
        float param_56 = asfloat(cb.Load(112));
        float param_57 = asfloat(cb.Load(104));
        float param_58 = m;
        float col = lerp1(param_56, param_57, param_58);
        float3 param_59 = max(Os.xyz, 0.0f.xxx);
        float bY = luma(param_59);
        float3 param_60 = max(R, 0.0f.xxx);
        float eY = luma(param_60);
        float param_61 = bY;
        float param_62 = eY;
        float param_63 = det;
        float wY = lerp1(param_61, param_62, param_63);
        float3 bC = Os.xyz / max(bY, 9.9999999747524270787835121154785e-07f).xxx;
        float3 eC = R / max(eY, 9.9999999747524270787835121154785e-07f).xxx;
        if ((det == 0.0f) && (col == 0.0f))
        {
            R = Os.xyz;
        }
        else
        {
            if ((det != 1.0f) || (col != 1.0f))
            {
                float3 param_64 = bC;
                float3 param_65 = eC;
                float param_66 = col;
                float3 param_67 = lerp3(param_64, param_65, param_66) * wY;
                R = clamp_ap1(param_67);
            }
        }
    }
    if (cb.Load(96) != 0u)
    {
        float3 _3026;
        if (passthrough)
        {
            _3026 = O;
        }
        else
        {
            float3 param_68 = O;
            float3 param_69 = saturate3(param_68);
            float3 _3035 = srgb_encode(param_69);
            _3026 = _3035;
        }
        float3 param_70 = _3026;
        float _3038 = skin_weight(param_70);
        float m_1 = _3038;
        R = m_1.xxx * N;
    }
    u0[int2(p)] = float4(max(R, 0.0f.xxx), Os.w);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
