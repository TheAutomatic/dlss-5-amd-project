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
struct StateT
{
    float s[4];
};

ByteAddressBuffer pc : register(t6);
RWTexture2D<float4> state : register(u5);
Texture2D<float4> t0 : register(t0);
SamplerState _t0_sampler : register(s0);
RWTexture2D<float4> a : register(u4);
Texture2D<float4> t1 : register(t1);
SamplerState _t1_sampler : register(s1);
Texture2D<float4> t2 : register(t2);
SamplerState _t2_sampler : register(s2);
Texture2D<float4> t3 : register(t3);
SamplerState _t3_sampler : register(s3);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

static StateT st;
groupshared uint hist[128];

// Returns the determinant of a 2x2 matrix.
float spvDet2x2(float a1, float a2, float b1, float b2)
{
    return a1 * b2 - b1 * a2;
}

// Returns the inverse of a matrix, by using the algorithm of calculating the classical
// adjoint and dividing by the determinant. The contents of the matrix are changed.
float3x3 spvInverse(float3x3 m)
{
    float3x3 adj;	// The adjoint matrix (inverse after dividing by determinant)

    // Create the transpose of the cofactors, as the classical adjoint of the matrix.
    adj[0][0] =  spvDet2x2(m[1][1], m[1][2], m[2][1], m[2][2]);
    adj[0][1] = -spvDet2x2(m[0][1], m[0][2], m[2][1], m[2][2]);
    adj[0][2] =  spvDet2x2(m[0][1], m[0][2], m[1][1], m[1][2]);

    adj[1][0] = -spvDet2x2(m[1][0], m[1][2], m[2][0], m[2][2]);
    adj[1][1] =  spvDet2x2(m[0][0], m[0][2], m[2][0], m[2][2]);
    adj[1][2] = -spvDet2x2(m[0][0], m[0][2], m[1][0], m[1][2]);

    adj[2][0] =  spvDet2x2(m[1][0], m[1][1], m[2][0], m[2][1]);
    adj[2][1] = -spvDet2x2(m[0][0], m[0][1], m[2][0], m[2][1]);
    adj[2][2] =  spvDet2x2(m[0][0], m[0][1], m[1][0], m[1][1]);

    // Calculate the determinant as a combination of the cofactors of the first row.
    float det = (adj[0][0] * m[0][0]) + (adj[0][1] * m[1][0]) + (adj[0][2] * m[2][0]);

    // Divide the classical adjoint matrix by the determinant.
    // If determinant is zero, matrix is not invertable, so leave it unchanged.
    return (det != 0.0f) ? (adj * (1.0f / det)) : m;
}

bool finite(float3 v)
{
    bool _1412 = !any(isnan(v));
    bool _1419;
    if (_1412)
    {
        _1419 = !any(isinf(v));
    }
    else
    {
        _1419 = _1412;
    }
    return _1419;
}

float gain()
{
    float _1301;
    if ((pc.Load(16) & 1u) != 0u)
    {
        _1301 = st.s[2];
    }
    else
    {
        _1301 = exp2(asfloat(pc.Load(24)));
    }
    return _1301;
}

bool unknee()
{
    return (pc.Load(16) & 2u) != 0u;
}

float maxch(float3 v)
{
    return max(v.x, max(v.y, v.z));
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

float3 knee(inout float3 d)
{
    if (pc.Load(48) != 0u)
    {
        float3 _351;
        if (pc.Load(48) >= 3u)
        {
            float3 param = d;
            float3 _356 = hybrid_encode(param);
            _351 = _356;
        }
        else
        {
            float3 param_1 = d;
            float3 _360 = neutwo_encode(param_1);
            _351 = _360;
        }
        return clamp(_351, 0.0f.xxx, 1.0f.xxx);
    }
    float l = dot(d, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
    if (l > 0.75f)
    {
        d *= ((0.75f + (0.25f * (1.0f - exp((-(l - 0.75f)) / 0.25f)))) / l);
    }
    float peak = max(d.x, max(d.y, d.z));
    if (peak > 1.0f)
    {
        d /= peak.xxx;
    }
    return d;
}

float3 neutral(inout float3 c)
{
    float peak = max(c.x, max(c.y, c.z));
    if (peak < 0.7599999904632568359375f)
    {
        return c;
    }
    float np = 1.0f - (0.057599999010562896728515625f / ((peak + 0.23999999463558197021484375f) - 0.7599999904632568359375f));
    c *= (np / peak);
    float g = 1.0f - (1.0f / ((0.1500000059604644775390625f * (peak - np)) + 1.0f));
    return lerp(c, np.xxx, g.xxx);
}

float3 reinhard(float3 x)
{
    return x / (1.0f.xxx + x);
}

float hable1(float x)
{
    return (((x * ((0.1500000059604644775390625f * x) + 0.0500000007450580596923828125f)) + 0.0040000001899898052215576171875f) / ((x * ((0.1500000059604644775390625f * x) + 0.5f)) + 0.0599999986588954925537109375f)) - 0.066666670143604278564453125f;
}

float3 hable(float3 x)
{
    float param = 11.19999980926513671875f;
    float w = hable1(param);
    float param_1 = x.x;
    float param_2 = x.y;
    float param_3 = x.z;
    return float3(hable1(param_1), hable1(param_2), hable1(param_3)) / w.xxx;
}

float gt1(float x)
{
    float w0 = 1.0f - smoothstep(0.0f, 0.2199999988079071044921875f, x);
    float w2 = step(0.532000005245208740234375f, x);
    float w1 = (1.0f - w0) - w2;
    float T = (0.2199999988079071044921875f * pow(max(x, 0.0f) / 0.2199999988079071044921875f, 1.33000004291534423828125f)) + 0.0f;
    float S = 1.0f - (0.467999994754791259765625f * exp((-2.13675212860107421875f) * (x - 0.532000005245208740234375f)));
    float L = 0.2199999988079071044921875f + (1.0f * (x - 0.2199999988079071044921875f));
    return ((T * w0) + (L * w1)) + (S * w2);
}

float3 gt(float3 x)
{
    float param = x.x;
    float param_1 = x.y;
    float param_2 = x.z;
    return float3(gt1(param), gt1(param_1), gt1(param_2));
}

float3 aces(float3 x)
{
    return (x * ((x * 2.5099999904632568359375f) + 0.02999999932944774627685546875f.xxx)) / ((x * ((x * 2.4300000667572021484375f) + 0.589999973773956298828125f.xxx)) + 0.14000000059604644775390625f.xxx);
}

float agx_sig(float x)
{
    float x2 = x * x;
    float x4 = x2 * x2;
    return (((((((15.5f * x4) * x2) - ((40.1399993896484375f * x4) * x)) + (31.95999908447265625f * x4)) - ((6.868000030517578125f * x2) * x)) + (0.4298000037670135498046875f * x2)) + (0.119099996984004974365234375f * x)) - 0.002319999970495700836181640625f;
}

float3 agx(float3 c)
{
    float3 v = mul(c, float3x3(float3(0.842479050159454345703125f, 0.0423282422125339508056640625f, 0.0423756539821624755859375f), float3(0.0784336030483245849609375f, 0.87846863269805908203125f, 0.0784336030483245849609375f), float3(0.079223744571208953857421875f, 0.07916612923145294189453125f, 0.8791429996490478515625f)));
    v = clamp(log2(max(v, 1.0000000133514319600180897396058e-10f.xxx)), (-12.47393035888671875f).xxx, 4.026069164276123046875f.xxx);
    v = (v - (-12.47393035888671875f).xxx) / 16.4999980926513671875f.xxx;
    float param = v.x;
    float param_1 = v.y;
    float param_2 = v.z;
    v = float3(agx_sig(param), agx_sig(param_1), agx_sig(param_2));
    return pow(max(mul(v, float3x3(float3(1.19687902927398681640625f, -0.0528968535363674163818359375f, -0.0529716350138187408447265625f), float3(-0.0980208814144134521484375f, 1.1519031524658203125f, -0.098043449223041534423828125f), float3(-0.099029742181301116943359375f, -0.098961174488067626953125f, 1.1510736942291259765625f))), 0.0f.xxx), 2.2000000476837158203125f.xxx);
}

float3 curve(float3 x)
{
    switch (pc.Load(20))
    {
        case 1u:
        {
            float3 param = x * asfloat(pc.Load(36));
            float3 _1176 = neutral(param);
            return _1176;
        }
        case 2u:
        {
            float3 param_1 = x * asfloat(pc.Load(36));
            return reinhard(param_1);
        }
        case 3u:
        {
            float3 param_2 = x * asfloat(pc.Load(36));
            return hable(param_2);
        }
        case 4u:
        {
            float3 param_3 = x * asfloat(pc.Load(36));
            return gt(param_3);
        }
        case 5u:
        {
            float3 param_4 = x * asfloat(pc.Load(36));
            return aces(param_4);
        }
        case 6u:
        {
            float3 param_5 = x * asfloat(pc.Load(36));
            return agx(param_5);
        }
        default:
        {
            float3 _1214;
            if (unknee())
            {
                float3 param_6 = x;
                float3 _1219 = knee(param_6);
                _1214 = _1219;
            }
            else
            {
                _1214 = x;
            }
            return _1214;
        }
    }
}

float3 forward(inout float3 x)
{
    x *= gain();
    if (asfloat(pc.Load(28)) != 1.0f)
    {
        x = pow(max(x, 0.0f.xxx) / 0.180000007152557373046875f.xxx, asfloat(pc.Load(28)).xxx) * 0.180000007152557373046875f;
    }
    if (asfloat(pc.Load(32)) != 1.0f)
    {
        float l = dot(x, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
        x = max(l.xxx + ((x - l.xxx) * asfloat(pc.Load(32))), 0.0f.xxx);
    }
    float3 param = x;
    return curve(param);
}

float3 enc(inout float3 v)
{
    v = clamp(v, 0.0f.xxx, 1.0f.xxx);
    return lerp(v * 12.9200000762939453125f, (pow(max(v, 9.9999999392252902907785028219223e-09f.xxx), 0.4166666567325592041015625f.xxx) * 1.05499994754791259765625f) - 0.054999999701976776123046875f.xxx, step(0.003130800090730190277099609375f.xxx, v));
}

float3 dec(inout float3 v)
{
    v = clamp(v, 0.0f.xxx, 1.0f.xxx);
    return lerp(v / 12.9200000762939453125f.xxx, pow((v + 0.054999999701976776123046875f.xxx) / 1.05499994754791259765625f.xxx, 2.400000095367431640625f.xxx), step(0.040449999272823333740234375f.xxx, v));
}

float hybrid_inv(float y)
{
    if (y <= 0.75f)
    {
        return y;
    }
    float u = min((y - 0.75f) / 0.25f, 0.999998986721038818359375f);
    return 0.75f + (0.25f * (u * rsqrt(max(1.0f - (u * u), 9.9999999392252902907785028219223e-09f))));
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

float3 knee_inv(float3 d)
{
    if (pc.Load(48) != 0u)
    {
        float3 _417;
        if (pc.Load(48) >= 3u)
        {
            float3 param = d;
            float3 _422 = hybrid_decode(param);
            _417 = _422;
        }
        else
        {
            float3 param_1 = d;
            float3 _426 = neutwo_decode(param_1);
            _417 = _426;
        }
        return _417;
    }
    float l = dot(d, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
    if (l <= 0.75f)
    {
        return d;
    }
    float lc = min(l, 0.9999997615814208984375f);
    return d * ((0.75f - (0.25f * log(1.0f - ((lc - 0.75f) / 0.25f)))) / l);
}

float3 neutral_inv(float3 y)
{
    float np = max(y.x, max(y.y, y.z));
    if (np < 0.7599999904632568359375f)
    {
        return y;
    }
    float peak = ((0.057599999010562896728515625f / max(1.0f - np, 0.0078125f)) - 0.23999999463558197021484375f) + 0.7599999904632568359375f;
    float g = 1.0f - (1.0f / ((0.1500000059604644775390625f * (peak - np)) + 1.0f));
    return ((y - (g * np).xxx) / (1.0f - g).xxx) * (peak / np);
}

float3 reinhard_inv(inout float3 y)
{
    y = min(y, 0.99989998340606689453125f.xxx);
    return y / (1.0f.xxx - y);
}

float hable_inv1(float y)
{
    float param = 11.19999980926513671875f;
    float t = min((y * hable1(param)) + 0.066666670143604278564453125f, 0.999000012874603271484375f);
    float qa = 0.1500000059604644775390625f * (1.0f - t);
    float qb = 0.0500000007450580596923828125f - (0.5f * t);
    float qc = 0.0040000001899898052215576171875f - (0.0599999986588954925537109375f * t);
    return max(((-qb) + sqrt(max((qb * qb) - ((4.0f * qa) * qc), 0.0f))) / (2.0f * qa), 0.0f);
}

float3 hable_inv(float3 y)
{
    float param = y.x;
    float param_1 = y.y;
    float param_2 = y.z;
    return float3(hable_inv1(param), hable_inv1(param_1), hable_inv1(param_2));
}

float gt_inv1(float y)
{
    float param = 64.0f;
    if (y >= gt1(param))
    {
        return 64.0f;
    }
    if (y >= 0.532000005245208740234375f)
    {
        return 0.532000005245208740234375f - (log(max((1.0f - y) / 0.467999994754791259765625f, 1.0000000031710768509710513471353e-30f)) / 2.13675212860107421875f);
    }
    if (y >= 0.2199999988079071044921875f)
    {
        return y;
    }
    float t = pow(max(y, 0.0f) / 0.2199999988079071044921875f, 0.75187969207763671875f);
    for (int i = 0; i < 4; i++)
    {
        float s = (t * t) * (3.0f - (2.0f * t));
        float ds = (6.0f * t) * (1.0f - t);
        float tc = pow(t, 1.33000004291534423828125f);
        float g = ((tc * (1.0f - s)) + (t * s)) - (y / 0.2199999988079071044921875f);
        float dg = ((((1.33000004291534423828125f * pow(max(t, 9.9999996826552253889678874634872e-21f), 0.3300000131130218505859375f)) * (1.0f - s)) - (tc * ds)) + s) + (t * ds);
        t = clamp(t - (g / max(dg, 9.9999999747524270787835121154785e-07f)), 0.0f, 1.0f);
    }
    return 0.2199999988079071044921875f * t;
}

float3 gt_inv(float3 y)
{
    float param = y.x;
    float param_1 = y.y;
    float param_2 = y.z;
    return float3(gt_inv1(param), gt_inv1(param_1), gt_inv1(param_2));
}

float3 aces_inv(inout float3 y)
{
    y = min(y, 0.99989998340606689453125f.xxx);
    float3 qa = 2.5099999904632568359375f.xxx - (y * 2.4300000667572021484375f);
    float3 qb = 0.02999999932944774627685546875f.xxx - (y * 0.589999973773956298828125f);
    float3 qc = y * (-0.14000000059604644775390625f);
    return max(((-qb) + sqrt(max((qb * qb) - ((qa * 4.0f) * qc), 0.0f.xxx))) / (qa * 2.0f), 0.0f.xxx);
}

float agx_dsig(float x)
{
    float x2 = x * x;
    float x4 = x2 * x2;
    return ((((((93.0f * x4) * x) - (200.6999969482421875f * x4)) + ((127.839996337890625f * x2) * x)) - (20.604000091552734375f * x2)) + (0.859600007534027099609375f * x)) + 0.119099996984004974365234375f;
}

float agx_sig_inv(inout float y)
{
    float param = 0.0f;
    float param_1 = 1.0f;
    y = clamp(y, agx_sig(param), agx_sig(param_1));
    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 8; i++)
    {
        float mid = 0.5f * (lo + hi);
        float param_2 = mid;
        if (agx_sig(param_2) < y)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    float x = 0.5f * (lo + hi);
    for (int i_1 = 0; i_1 < 3; i_1++)
    {
        float param_3 = x;
        float param_4 = x;
        x = clamp(x - ((agx_sig(param_3) - y) / agx_dsig(param_4)), lo, hi);
    }
    return x;
}

float3 agx_inv(float3 y)
{
    float3 v = mul(pow(max(y, 0.0f.xxx), 0.4545454680919647216796875f.xxx), spvInverse(float3x3(float3(1.19687902927398681640625f, -0.0528968535363674163818359375f, -0.0529716350138187408447265625f), float3(-0.0980208814144134521484375f, 1.1519031524658203125f, -0.098043449223041534423828125f), float3(-0.099029742181301116943359375f, -0.098961174488067626953125f, 1.1510736942291259765625f))));
    float param = v.x;
    float _1138 = agx_sig_inv(param);
    float param_1 = v.y;
    float _1142 = agx_sig_inv(param_1);
    float param_2 = v.z;
    float _1146 = agx_sig_inv(param_2);
    v = float3(_1138, _1142, _1146);
    v = exp2((v * 16.4999980926513671875f) + (-12.47393035888671875f).xxx);
    return mul(v, spvInverse(float3x3(float3(0.842479050159454345703125f, 0.0423282422125339508056640625f, 0.0423756539821624755859375f), float3(0.0784336030483245849609375f, 0.87846863269805908203125f, 0.0784336030483245849609375f), float3(0.079223744571208953857421875f, 0.07916612923145294189453125f, 0.8791429996490478515625f))));
}

float3 curve_inv(float3 y)
{
    switch (pc.Load(20))
    {
        case 1u:
        {
            float3 param = y;
            return neutral_inv(param) / asfloat(pc.Load(36)).xxx;
        }
        case 2u:
        {
            float3 param_1 = y;
            float3 _1246 = reinhard_inv(param_1);
            return _1246 / asfloat(pc.Load(36)).xxx;
        }
        case 3u:
        {
            float3 param_2 = y;
            return hable_inv(param_2) / asfloat(pc.Load(36)).xxx;
        }
        case 4u:
        {
            float3 param_3 = y;
            return gt_inv(param_3) / asfloat(pc.Load(36)).xxx;
        }
        case 5u:
        {
            float3 param_4 = y;
            float3 _1270 = aces_inv(param_4);
            return _1270 / asfloat(pc.Load(36)).xxx;
        }
        case 6u:
        {
            float3 param_5 = y;
            return agx_inv(param_5) / asfloat(pc.Load(36)).xxx;
        }
        default:
        {
            float3 _1285;
            if (unknee())
            {
                float3 param_6 = y;
                _1285 = knee_inv(param_6);
            }
            else
            {
                _1285 = y;
            }
            return _1285;
        }
    }
}

float3 backward(float3 y)
{
    float3 param = y;
    float3 x = curve_inv(param);
    if (asfloat(pc.Load(32)) != 1.0f)
    {
        float l = dot(x, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
        x = l.xxx + ((x - l.xxx) / asfloat(pc.Load(32)).xxx);
    }
    if (asfloat(pc.Load(28)) != 1.0f)
    {
        x = pow(max(x, 0.0f.xxx) / 0.180000007152557373046875f.xxx, (1.0f / asfloat(pc.Load(28))).xxx) * 0.180000007152557373046875f;
    }
    return x / gain().xxx;
}

float3 encode_as_proxy(float3 v)
{
    float3 _1426;
    if (pc.Load(52) == 0u)
    {
        float3 param = v;
        float3 _1431 = knee(param);
        float3 param_1 = _1431;
        float3 _1433 = enc(param_1);
        _1426 = _1433;
    }
    else
    {
        float3 _1438;
        if (pc.Load(52) == 1u)
        {
            float3 param_2 = v;
            float3 _1443 = enc(param_2);
            _1438 = _1443;
        }
        else
        {
            _1438 = v;
        }
        _1426 = _1438;
    }
    return _1426;
}

float3 neutral_sdr_bound(float3 original, float3 resolved, float3 nin, float3 nout, float cap)
{
    float3 input_edit = abs(nout - nin);
    float limit = cap * max(input_edit.x, max(input_edit.y, input_edit.z));
    float3 residual = resolved - original;
    float3 magnitude = abs(residual);
    float peak = max(magnitude.x, max(magnitude.y, magnitude.z));
    float3 _1485;
    if (peak > limit)
    {
        _1485 = original + (residual * (limit / peak));
    }
    else
    {
        _1485 = resolved;
    }
    return _1485;
}

void comp_main()
{
    float4 sv = state[int2(0, 0)];
    st.s[0] = sv.x;
    st.s[1] = sv.y;
    st.s[2] = sv.z;
    st.s[3] = sv.w;
    int2 xy = int2(gl_GlobalInvocationID.xy);
    bool _1536 = xy.x >= int(pc.Load(8));
    bool _1546;
    if (!_1536)
    {
        _1546 = xy.y >= int(pc.Load(12));
    }
    else
    {
        _1546 = _1536;
    }
    if (_1546)
    {
        return;
    }
    if (pc.Load(0) == 62u)
    {
        float4 L = t0.Load(int3(xy, 0));
        float3 param = L.xyz;
        float4 _1572;
        if (finite(param))
        {
            float3 param_1 = max(L.xyz, 0.0f.xxx);
            float3 _1579 = forward(param_1);
            float3 param_2 = _1579;
            float3 _1581 = enc(param_2);
            _1572 = float4(_1581, L.w);
        }
        else
        {
            _1572 = float4(0.0f, 0.0f, 0.0f, L.w);
        }
        a[xy] = _1572;
        return;
    }
    float4 nin = t0.Load(int3(xy, 0));
    float4 nout = t1.Load(int3(xy, 0));
    float4 L_1 = t2.Load(int3(xy, 0));
    float4 P = t3.Load(int3(xy, 0));
    float3 M = P.xyz;
    bool _1625 = any(bool3(nout.xyz.x != nin.xyz.x, nout.xyz.y != nin.xyz.y, nout.xyz.z != nin.xyz.z));
    bool _1632;
    if (_1625)
    {
        float3 param_3 = nout.xyz;
        _1632 = finite(param_3);
    }
    else
    {
        _1632 = _1625;
    }
    bool _1639;
    if (_1632)
    {
        float3 param_4 = L_1.xyz;
        _1639 = finite(param_4);
    }
    else
    {
        _1639 = _1632;
    }
    bool _1646;
    if (_1639)
    {
        float3 param_5 = P.xyz;
        _1646 = finite(param_5);
    }
    else
    {
        _1646 = _1639;
    }
    if (_1646)
    {
        float3 param_6 = nin.xyz;
        float3 _1653 = dec(param_6);
        float3 din = _1653;
        float3 param_7 = nout.xyz;
        float3 _1658 = dec(param_7);
        float3 dout = _1658;
        float3 p = max(L_1.xyz, 0.0f.xxx);
        float3 d;
        if ((pc.Load(16) & 16u) != 0u)
        {
            d = p * (clamp(max(dout, 0.0009765625f.xxx) / max(din, 0.0009765625f.xxx), 0.25f.xxx, 4.0f.xxx) - 1.0f.xxx);
        }
        else
        {
            float3 param_8 = 1.0f.xxx;
            float3 _1689 = forward(param_8);
            float3 w = _1689;
            float3 param_9 = min(dout, max(w.x, max(w.y, w.z)).xxx);
            float3 param_10 = din;
            d = backward(param_9) - backward(param_10);
            float3 lim = (abs(dout - din) * asfloat(pc.Load(40))) / gain().xxx;
            d = clamp(d, -lim, lim);
        }
        float3 param_11 = max(p + d, 0.0f.xxx);
        float3 param_12 = p;
        M = max((P.xyz + encode_as_proxy(param_11)) - encode_as_proxy(param_12), 0.0f.xxx);
        bool _1740 = pc.Load(52) == 1u;
        bool _1746;
        if (_1740)
        {
            _1746 = pc.Load(20) == 1u;
        }
        else
        {
            _1746 = _1740;
        }
        bool _1753;
        if (_1746)
        {
            _1753 = (pc.Load(16) & 16u) == 0u;
        }
        else
        {
            _1753 = _1746;
        }
        if (_1753)
        {
            float3 param_13 = P.xyz;
            float3 param_14 = M;
            float3 param_15 = nin.xyz;
            float3 param_16 = nout.xyz;
            float param_17 = asfloat(pc.Load(40));
            M = neutral_sdr_bound(param_13, param_14, param_15, param_16, param_17);
        }
        float3 param_18 = M;
        if (!finite(param_18))
        {
            M = P.xyz;
        }
    }
    a[xy] = float4(M, nout.w);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
