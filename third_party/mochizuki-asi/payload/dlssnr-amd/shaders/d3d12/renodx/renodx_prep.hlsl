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
RWByteAddressBuffer st : register(u0);
cbuffer Push
{
    uint pc_width : packoffset(c0);
    uint pc_height : packoffset(c0.y);
    uint pc_mode : packoffset(c0.z);
    uint pc_flags : packoffset(c0.w);
    uint pc_curve : packoffset(c1);
    float pc_bias : packoffset(c1.y);
    float pc_contrast : packoffset(c1.z);
    float pc_saturation : packoffset(c1.w);
    float pc_k : packoffset(c2);
    float pc_cap : packoffset(c2.y);
    float pc_dt : packoffset(c2.z);
    float pc_divisor : packoffset(c2.w);
};

RWTexture2D<float4> a : register(u1);
Texture2D<float4> Scale : register(t4);
RWTexture2D<float4> b : register(u2);
RWTexture2D<float4> c : register(u3);

static uint3 gl_GlobalInvocationID;
static uint gl_LocalInvocationIndex;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
    uint gl_LocalInvocationIndex : SV_GroupIndex;
};

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
    bool _1184 = !any(isnan(v));
    bool _1191;
    if (_1184)
    {
        _1191 = !any(isinf(v));
    }
    else
    {
        _1191 = _1184;
    }
    return _1191;
}

bool linear_frame()
{
    return (pc_flags & 16u) != 0u;
}

float3 dec(inout float3 v)
{
    v = clamp(v, 0.0f.xxx, 1.0f.xxx);
    return lerp(v / 12.9200000762939453125f.xxx, pow((v + 0.054999999701976776123046875f.xxx) / 1.05499994754791259765625f.xxx, 2.400000095367431640625f.xxx), step(0.040449999272823333740234375f.xxx, v));
}

float3 light(float3 P)
{
    float3 _199;
    if (linear_frame())
    {
        _199 = max(P, 0.0f.xxx);
    }
    else
    {
        float3 param = P;
        float3 _208 = dec(param);
        _199 = _208;
    }
    return _199;
}

float3 light_at(float3 P, int2 xy)
{
    float3 param = P;
    return light(param);
}

void meter()
{
    uint t = gl_LocalInvocationIndex;
    if (t < 128u)
    {
        hist[t] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    uint sx = max(1u, (pc_width / 160u));
    uint sy = max(1u, (pc_height / 90u));
    uint nx = ((pc_width + sx) - 1u) / sx;
    uint ny = ((pc_height + sy) - 1u) / sy;
    for (uint i = t; i < (nx * ny); i += 256u)
    {
        int2 xy = int2(int((i % nx) * sx), int((i / nx) * sy));
        float3 P = a[xy].xyz;
        float3 param = P;
        if (!finite(param))
        {
            continue;
        }
        float3 param_1 = P;
        int2 param_2 = xy;
        float l = max(dot(light_at(param_1, param_2), float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f)), 1.4999999621068127453327178955078e-05f);
        uint _1300;
        InterlockedAdd(hist[uint(clamp((log2(l) + 16.0f) * 6.400000095367431640625f, 0.0f, 127.0f))], 1u, _1300);
    }
    GroupMemoryBarrierWithGroupSync();
    if (t != 0u)
    {
        return;
    }
    uint n = 0u;
    for (uint b_1 = 0u; b_1 < 128u; b_1++)
    {
        n += hist[b_1];
    }
    bool _1329 = asfloat(st.Load(4)) == 1.0f;
    bool _1337;
    if (_1329)
    {
        _1337 = (pc_flags & 8u) == 0u;
    }
    else
    {
        _1337 = _1329;
    }
    bool valid = _1337;
    float _1340;
    if (valid)
    {
        _1340 = asfloat(st.Load(0));
    }
    else
    {
        _1340 = 0.0f;
    }
    float s = _1340;
    if (n >= 64u)
    {
        float lo = 0.100000001490116119384765625f * float(n);
        float hi = 0.89999997615814208984375f * float(n);
        float below = 0.0f;
        float sum = 0.0f;
        float weight = 0.0f;
        for (uint b_2 = 0u; b_2 < 128u; b_2++)
        {
            float c_1 = float(hist[b_2]);
            float from = max(below, lo);
            float to = min(below + c_1, hi);
            if (to > from)
            {
                sum += ((to - from) * (((float(b_2) + 0.5f) / 6.400000095367431640625f) - 16.0f));
                weight += (to - from);
            }
            below += c_1;
        }
        float target = clamp((-2.4739000797271728515625f) - (sum / max(weight, 1.0f)), -4.0f, 8.0f);
        st.Store(12, asuint(target));
        float speed = (target < s) ? 3.0f : 1.0f;
        float _1432;
        if (valid)
        {
            _1432 = s + ((target - s) * (1.0f - exp((-clamp(pc_dt, 0.0f, 0.25f)) * speed)));
        }
        else
        {
            _1432 = target;
        }
        s = _1432;
    }
    s = clamp(s, -4.0f, 8.0f);
    st.Store(0, asuint(s));
    st.Store(4, asuint(1.0f));
    st.Store(8, asuint(exp2(s + pc_bias)));
}

float gain()
{
    float _1068;
    if ((pc_flags & 1u) != 0u)
    {
        _1068 = asfloat(st.Load(8));
    }
    else
    {
        _1068 = exp2(pc_bias);
    }
    return _1068;
}

bool unknee()
{
    return false;
}

float3 knee(float3 d)
{
    return d;
}

float3 neutral(inout float3 c_1)
{
    float peak = max(c_1.x, max(c_1.y, c_1.z));
    if (peak < 0.7599999904632568359375f)
    {
        return c_1;
    }
    float np = 1.0f - (0.057599999010562896728515625f / ((peak + 0.23999999463558197021484375f) - 0.7599999904632568359375f));
    c_1 *= (np / peak);
    float g = 1.0f - (1.0f / ((0.1500000059604644775390625f * (peak - np)) + 1.0f));
    return lerp(c_1, np.xxx, g.xxx);
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

float3 agx(float3 c_1)
{
    float3 v = mul(c_1, float3x3(float3(0.842479050159454345703125f, 0.0423282422125339508056640625f, 0.0423756539821624755859375f), float3(0.0784336030483245849609375f, 0.87846863269805908203125f, 0.0784336030483245849609375f), float3(0.079223744571208953857421875f, 0.07916612923145294189453125f, 0.8791429996490478515625f)));
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
    switch (pc_curve)
    {
        case 1u:
        {
            float3 param = x * pc_k;
            float3 _943 = neutral(param);
            return _943;
        }
        case 2u:
        {
            float3 param_1 = x * pc_k;
            return reinhard(param_1);
        }
        case 3u:
        {
            float3 param_2 = x * pc_k;
            return hable(param_2);
        }
        case 4u:
        {
            float3 param_3 = x * pc_k;
            return gt(param_3);
        }
        case 5u:
        {
            float3 param_4 = x * pc_k;
            return aces(param_4);
        }
        case 6u:
        {
            float3 param_5 = x * pc_k;
            return agx(param_5);
        }
        default:
        {
            float3 _981;
            if (unknee())
            {
                float3 param_6 = x;
                _981 = knee(param_6);
            }
            else
            {
                _981 = x;
            }
            return _981;
        }
    }
}

float3 forward(inout float3 x)
{
    x *= gain();
    if (pc_contrast != 1.0f)
    {
        x = pow(max(x, 0.0f.xxx) / 0.180000007152557373046875f.xxx, pc_contrast.xxx) * 0.180000007152557373046875f;
    }
    if (pc_saturation != 1.0f)
    {
        float l = dot(x, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
        x = max(l.xxx + ((x - l.xxx) * pc_saturation), 0.0f.xxx);
    }
    float3 param = x;
    return curve(param);
}

float3 enc(inout float3 v)
{
    v = clamp(v, 0.0f.xxx, 1.0f.xxx);
    return lerp(v * 12.9200000762939453125f, (pow(max(v, 9.9999999392252902907785028219223e-09f.xxx), 0.4166666567325592041015625f.xxx) * 1.05499994754791259765625f) - 0.054999999701976776123046875f.xxx, step(0.003130800090730190277099609375f.xxx, v));
}

float3 half_rtz(float3 v)
{
    float3 _1470 = trunc(v * 16777216.0f) * 5.9604644775390625e-08f;
    float3 _1477 = asfloat(asuint(v) & uint3(4294959104u, 4294959104u, 4294959104u));
    float3 _1479 = abs(v);
    bool3 _1482 = bool3(_1479.x >= 6.103515625e-05f.xxx.x, _1479.y >= 6.103515625e-05f.xxx.y, _1479.z >= 6.103515625e-05f.xxx.z);
    return float3(_1482.x ? _1477.x : _1470.x, _1482.y ? _1477.y : _1470.y, _1482.z ? _1477.z : _1470.z);
}

float3 knee_inv(float3 d)
{
    return d;
}

float3 neutral_inv(float3 y)
{
    float np = max(y.x, max(y.y, y.z));
    if (np < 0.7599999904632568359375f)
    {
        return y;
    }
    float peak = ((0.057599999010562896728515625f / max(1.0f - np, 9.9999999747524270787835121154785e-07f)) - 0.23999999463558197021484375f) + 0.7599999904632568359375f;
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
    float _907 = agx_sig_inv(param);
    float param_1 = v.y;
    float _911 = agx_sig_inv(param_1);
    float param_2 = v.z;
    float _915 = agx_sig_inv(param_2);
    v = float3(_907, _911, _915);
    v = exp2((v * 16.4999980926513671875f) + (-12.47393035888671875f).xxx);
    return mul(v, spvInverse(float3x3(float3(0.842479050159454345703125f, 0.0423282422125339508056640625f, 0.0423756539821624755859375f), float3(0.0784336030483245849609375f, 0.87846863269805908203125f, 0.0784336030483245849609375f), float3(0.079223744571208953857421875f, 0.07916612923145294189453125f, 0.8791429996490478515625f))));
}

float3 curve_inv(float3 y)
{
    switch (pc_curve)
    {
        case 1u:
        {
            float3 param = y;
            return neutral_inv(param) / pc_k.xxx;
        }
        case 2u:
        {
            float3 param_1 = y;
            float3 _1013 = reinhard_inv(param_1);
            return _1013 / pc_k.xxx;
        }
        case 3u:
        {
            float3 param_2 = y;
            return hable_inv(param_2) / pc_k.xxx;
        }
        case 4u:
        {
            float3 param_3 = y;
            return gt_inv(param_3) / pc_k.xxx;
        }
        case 5u:
        {
            float3 param_4 = y;
            float3 _1037 = aces_inv(param_4);
            return _1037 / pc_k.xxx;
        }
        case 6u:
        {
            float3 param_5 = y;
            return agx_inv(param_5) / pc_k.xxx;
        }
        default:
        {
            float3 _1052;
            if (unknee())
            {
                float3 param_6 = y;
                _1052 = knee_inv(param_6);
            }
            else
            {
                _1052 = y;
            }
            return _1052;
        }
    }
}

float3 backward(float3 y)
{
    float3 param = y;
    float3 x = curve_inv(param);
    if (pc_saturation != 1.0f)
    {
        float l = dot(x, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
        x = l.xxx + ((x - l.xxx) / pc_saturation.xxx);
    }
    if (pc_contrast != 1.0f)
    {
        x = pow(max(x, 0.0f.xxx) / 0.180000007152557373046875f.xxx, (1.0f / pc_contrast).xxx) * 0.180000007152557373046875f;
    }
    return x / gain().xxx;
}

float3 reencode(float3 v)
{
    float3 _213;
    if (linear_frame())
    {
        _213 = v;
    }
    else
    {
        float3 param = v;
        float3 _220 = enc(param);
        _213 = _220;
    }
    return _213;
}

void comp_main()
{
    if (pc_mode == 0u)
    {
        meter();
        return;
    }
    int2 xy = int2(gl_GlobalInvocationID.xy);
    bool _1505 = xy.x >= int(pc_width);
    bool _1515;
    if (!_1505)
    {
        _1515 = xy.y >= int(pc_height);
    }
    else
    {
        _1515 = _1505;
    }
    if (_1515)
    {
        return;
    }
    if (pc_mode == 1u)
    {
        float4 P = a[xy];
        if (linear_frame())
        {
            float3 param = P.xyz;
            float3 x = light(param);
            if ((pc_flags & 32u) != 0u)
            {
                float _1548;
                if (pc_divisor > 0.0f)
                {
                    _1548 = pc_divisor;
                }
                else
                {
                    _1548 = max(Scale.Load(int3(int2(0, 0), 0)).x, 9.9999999392252902907785028219223e-09f);
                }
                x /= _1548.xxx;
            }
            float3 param_1 = P.xyz;
            float4 _1573;
            if (finite(param_1))
            {
                float3 param_2 = x;
                float3 _1578 = forward(param_2);
                _1573 = float4(_1578, P.w);
            }
            else
            {
                _1573 = P;
            }
            b[xy] = _1573;
            return;
        }
        if ((pc_flags & 128u) != 0u)
        {
            float3 param_3 = P.xyz;
            float4 _1602;
            if (finite(param_3))
            {
                float3 param_4 = P.xyz;
                float3 param_5 = light(param_4);
                float3 _1610 = forward(param_5);
                float3 param_6 = _1610;
                float3 _1612 = enc(param_6);
                _1602 = float4(_1612, P.w);
            }
            else
            {
                _1602 = P;
            }
            b[xy] = _1602;
            return;
        }
        b[xy] = P;
        float3 param_7 = P.xyz;
        if (!finite(param_7))
        {
            return;
        }
        float3 param_8 = P.xyz;
        float3 param_9 = light(param_8);
        float3 _1641 = forward(param_9);
        float3 param_10 = _1641;
        float3 _1643 = enc(param_10);
        a[xy] = float4(_1643, P.w);
        return;
    }
    float4 nout = c[xy];
    float4 P_1 = b[xy];
    float4 nin;
    if ((pc_flags & 256u) != 0u)
    {
        float3 param_11 = P_1.xyz;
        float4 _1671;
        if (finite(param_11))
        {
            float3 param_12 = P_1.xyz;
            float3 param_13 = light(param_12);
            float3 _1679 = forward(param_13);
            float3 param_14 = _1679;
            float3 _1681 = enc(param_14);
            float3 param_15 = _1681;
            _1671 = float4(half_rtz(param_15), P_1.w);
        }
        else
        {
            _1671 = P_1;
        }
        nin = _1671;
    }
    else
    {
        nin = a[xy];
    }
    float3 M = P_1.xyz;
    bool _1705 = any(bool3(nout.xyz.x != nin.xyz.x, nout.xyz.y != nin.xyz.y, nout.xyz.z != nin.xyz.z));
    bool _1712;
    if (_1705)
    {
        float3 param_16 = nout.xyz;
        _1712 = finite(param_16);
    }
    else
    {
        _1712 = _1705;
    }
    bool _1719;
    if (_1712)
    {
        float3 param_17 = P_1.xyz;
        _1719 = finite(param_17);
    }
    else
    {
        _1719 = _1712;
    }
    if (_1719)
    {
        float3 param_18 = nin.xyz;
        float3 _1726 = dec(param_18);
        float3 din = _1726;
        float3 param_19 = nout.xyz;
        float3 _1731 = dec(param_19);
        float3 dout = _1731;
        float3 param_20 = P_1.xyz;
        float3 p = light(param_20);
        float3 d;
        if ((pc_flags & 64u) != 0u)
        {
            d = p * (clamp(max(dout, 0.0009765625f.xxx) / max(din, 0.0009765625f.xxx), 0.25f.xxx, 4.0f.xxx) - 1.0f.xxx);
        }
        else
        {
            float3 param_21 = dout;
            float3 param_22 = din;
            d = backward(param_21) - backward(param_22);
            float3 lim = (abs(dout - din) * pc_cap) / gain().xxx;
            d = clamp(d, -lim, lim);
        }
        float3 param_23 = max(p + d, 0.0f.xxx);
        float3 param_24 = p;
        M = max((P_1.xyz + reencode(param_23)) - reencode(param_24), 0.0f.xxx);
        float3 param_25 = M;
        if (!finite(param_25))
        {
            M = P_1.xyz;
        }
    }
    c[xy] = float4(M, nout.w);
    if ((pc_flags & 4u) != 0u)
    {
        a[xy] = P_1;
    }
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    gl_LocalInvocationIndex = stage_input.gl_LocalInvocationIndex;
    comp_main();
}
