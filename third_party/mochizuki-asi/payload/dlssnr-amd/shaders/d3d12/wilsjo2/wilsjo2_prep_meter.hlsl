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
RWTexture2D<float4> a : register(u4);
RWTexture2D<float4> state : register(u5);
Texture2D<float4> t0 : register(t0);
SamplerState _t0_sampler : register(s0);
Texture2D<float4> t1 : register(t1);
SamplerState _t1_sampler : register(s1);
Texture2D<float4> t2 : register(t2);
SamplerState _t2_sampler : register(s2);
Texture2D<float4> t3 : register(t3);
SamplerState _t3_sampler : register(s3);

static uint gl_LocalInvocationIndex;
struct SPIRV_Cross_Input
{
    uint gl_LocalInvocationIndex : SV_GroupIndex;
};

groupshared uint hist[128];
static StateT st;

bool finite(float3 v)
{
    bool _43 = !any(isnan(v));
    bool _50;
    if (_43)
    {
        _50 = !any(isinf(v));
    }
    else
    {
        _50 = _43;
    }
    return _50;
}

float3 light(float3 P)
{
    return max(P, 0.0f.xxx);
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
    uint sx = max(1u, (pc.Load(8) / 160u));
    uint sy = max(1u, (pc.Load(12) / 90u));
    uint nx = ((pc.Load(8) + sx) - 1u) / sx;
    uint ny = ((pc.Load(12) + sy) - 1u) / sy;
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
        uint _174;
        InterlockedAdd(hist[uint(clamp((log2(l) + 16.0f) * 6.400000095367431640625f, 0.0f, 127.0f))], 1u, _174);
    }
    GroupMemoryBarrierWithGroupSync();
    if (t != 0u)
    {
        return;
    }
    uint n = 0u;
    for (uint b = 0u; b < 128u; b++)
    {
        n += hist[b];
    }
    bool _212 = st.s[1] == 1.0f;
    bool _221;
    if (_212)
    {
        _221 = (pc.Load(16) & 8u) == 0u;
    }
    else
    {
        _221 = _212;
    }
    bool valid = _221;
    float _224;
    if (valid)
    {
        _224 = st.s[0];
    }
    else
    {
        _224 = 0.0f;
    }
    float s = _224;
    if (n >= 64u)
    {
        float lo = 0.100000001490116119384765625f * float(n);
        float hi = 0.89999997615814208984375f * float(n);
        float below = 0.0f;
        float sum = 0.0f;
        float weight = 0.0f;
        for (uint b_1 = 0u; b_1 < 128u; b_1++)
        {
            float c = float(hist[b_1]);
            float from = max(below, lo);
            float to = min(below + c, hi);
            if (to > from)
            {
                sum += ((to - from) * (((float(b_1) + 0.5f) / 6.400000095367431640625f) - 16.0f));
                weight += (to - from);
            }
            below += c;
        }
        float target = clamp((-2.4739000797271728515625f) - (sum / max(weight, 1.0f)), -4.0f, 8.0f);
        st.s[3] = target;
        float speed = (target < s) ? 3.0f : 1.0f;
        float _318;
        if (valid)
        {
            _318 = s + ((target - s) * (1.0f - exp((-clamp(asfloat(pc.Load(44)), 0.0f, 0.25f)) * speed)));
        }
        else
        {
            _318 = target;
        }
        s = _318;
    }
    s = clamp(s, -4.0f, 8.0f);
    st.s[0] = s;
    st.s[1] = 1.0f;
    st.s[2] = exp2(s + asfloat(pc.Load(24)));
}

void comp_main()
{
    float4 sv = state[int2(0, 0)];
    st.s[0] = sv.x;
    st.s[1] = sv.y;
    st.s[2] = sv.z;
    st.s[3] = sv.w;
    meter();
    if (gl_LocalInvocationIndex == 0u)
    {
        state[int2(0, 0)] = float4(st.s[0], st.s[1], st.s[2], st.s[3]);
    }
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_LocalInvocationIndex = stage_input.gl_LocalInvocationIndex;
    comp_main();
}
