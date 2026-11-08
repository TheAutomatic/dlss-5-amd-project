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
cbuffer Push
{
    uint2 pc_size : packoffset(c0);
    uint2 pc_low : packoffset(c0.z);
    uint2 pc_mbase : packoffset(c1);
    uint2 pc_mgrid : packoffset(c1.z);
    float2 pc_mscale : packoffset(c2);
    float pc_w : packoffset(c2.z);
    uint pc_flags : packoffset(c2.w);
    uint pc_block : packoffset(c3);
    float pc_cstop : packoffset(c3.y);
    float pc_darkcol : packoffset(c3.z);
};

RWTexture2D<float4> Proxy : register(u4);
Texture2D<float4> Motion : register(t1);
Texture2D<float4> CA : register(t0);
Texture2D<float4> CB : register(t2);
RWTexture2D<float4> FbAdd : register(u5);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

float3 st_dec(float3 c)
{
    float3 _43 = c / 12.9200000762939453125f.xxx;
    float3 _53 = pow((c + 0.054999999701976776123046875f.xxx) / 1.05499994754791259765625f.xxx, 2.400000095367431640625f.xxx);
    bool3 _59 = bool3(c.x > 0.040449999272823333740234375f.xxx.x, c.y > 0.040449999272823333740234375f.xxx.y, c.z > 0.040449999272823333740234375f.xxx.z);
    return float3(_59.x ? _53.x : _43.x, _59.y ? _53.y : _43.y, _59.z ? _53.z : _43.z);
}

float3 st_lab(float3 s)
{
    float3 param = max(s, 0.0f.xxx);
    float3 l = st_dec(param);
    float3 lms = float3(dot(l, float3(0.4122214615345001220703125f, 0.536332547664642333984375f, 0.0514459945261478424072265625f)), dot(l, float3(0.21190349757671356201171875f, 0.680699527263641357421875f, 0.10739696025848388671875f)), dot(l, float3(0.0883024632930755615234375f, 0.28171885013580322265625f, 0.629978716373443603515625f)));
    float3 q = sign(lms) * pow(abs(lms), 0.3333333432674407958984375f.xxx);
    return float3(dot(q, float3(0.2104542553424835205078125f, 0.793617784976959228515625f, -0.004072046838700771331787109375f)), dot(q, float3(1.9779984951019287109375f, -2.428592205047607421875f, 0.4505937099456787109375f)), dot(q, float3(0.025904037058353424072265625f, 0.782771766185760498046875f, -0.8086757659912109375f)));
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
    float3 _70 = l * 12.9200000762939453125f;
    float3 _77 = (pow(l, 0.4166666567325592041015625f.xxx) * 1.05499994754791259765625f) - 0.054999999701976776123046875f.xxx;
    bool3 _81 = bool3(l.x > 0.003130800090730190277099609375f.xxx.x, l.y > 0.003130800090730190277099609375f.xxx.y, l.z > 0.003130800090730190277099609375f.xxx.z);
    return float3(_81.x ? _77.x : _70.x, _81.y ? _77.y : _70.y, _81.z ? _77.z : _70.z);
}

void comp_main()
{
    uint2 px = gl_GlobalInvocationID.xy;
    if (any(bool2(px.x >= pc_size.x, px.y >= pc_size.y)))
    {
        return;
    }
    float4 p = Proxy[int2(px)];
    float3 np = p.xyz;
    float conf = 0.0f;
    if ((pc_flags & 1u) != 0u)
    {
        float2 prev = float2(px) + 0.5f.xx;
        if ((pc_flags & 2u) != 0u)
        {
            uint2 t = pc_mbase + min(uint2(((float2(px) + 0.5f.xx) * float2(pc_mgrid)) / float2(pc_size)), (pc_mgrid - uint2(1u, 1u)));
            prev += (Motion.Load(int3(int2(t), 0)).xy * pc_mscale);
        }
        if ((pc_flags & 16u) != 0u)
        {
            bool _425 = all(bool2(prev.x >= 0.0f.xx.x, prev.y >= 0.0f.xx.y));
            bool _434;
            if (_425)
            {
                float2 _431 = float2(pc_size);
                _434 = all(bool2(prev.x <= _431.x, prev.y <= _431.y));
            }
            else
            {
                _434 = _425;
            }
            if (_434)
            {
                float3 param = p.xyz;
                float I = st_lab(param).x;
                float2 param_1 = prev;
                float param_2 = I;
                uint2 param_3 = pc_low;
                float param_4;
                float param_5;
                float3 _457 = st_gfq(CA, CB, param_1, param_2, param_3, param_4, param_5);
                float mI = param_4;
                float vI = param_5;
                float3 q = _457;
                float z = ((I - mI) * (I - mI)) / (vI + 0.00039999998989515006542205810546875f);
                conf = exp((-0.125f) * z);
                float3 add = q * (pc_w * conf);
                if (pc_darkcol > 0.0f)
                {
                    float3 param_6 = p.xyz;
                    float3 X = st_lab(param_6);
                    float3 param_7 = clamp(p.xyz + add, 0.0f.xxx, 1.0f.xxx);
                    float3 Y = st_lab(param_7);
                    float3 param_8 = float3(Y.x, X.yz + ((Y.yz - X.yz) * smoothstep(0.0f, pc_darkcol, X.x)));
                    float3 param_9 = st_lin(param_8);
                    float3 _528 = st_enc(param_9);
                    add = _528 - p.xyz;
                }
                float s = 1.0f;
                for (int c = 0; c < 3; c++)
                {
                    if (add[c] > 1.0000000116860974230803549289703e-07f)
                    {
                        s = min(s, max(1.0f - p[c], 0.0f) / add[c]);
                    }
                    else
                    {
                        if (add[c] < (-1.0000000116860974230803549289703e-07f))
                        {
                            s = min(s, max(p[c], 0.0f) / (-add[c]));
                        }
                    }
                }
                np = p.xyz + (add * s);
            }
        }
    }
    Proxy[int2(px)] = float4(np, p.w);
    FbAdd[int2(px)] = float4(np - p.xyz, conf);
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
