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
    bool _179 = p.x >= cb.Load(8);
    bool _190;
    if (!_179)
    {
        _190 = p.y >= cb.Load(12);
    }
    else
    {
        _190 = _179;
    }
    return _190;
}

int2 model_size()
{
    uint _200_dummy_parameter;
    return int2(spvTextureSize(t0, uint(0), _200_dummy_parameter));
}

float AF1_x(float a)
{
    return a;
}

float ARcpF1(float x)
{
    float param = 1.0f;
    return AF1_x(param) / x;
}

void FsrEasuCon(inout uint4 con0, inout uint4 con1, inout uint4 con2, inout uint4 con3, float inputViewportInPixelsX, float inputViewportInPixelsY, float inputSizeInPixelsX, float inputSizeInPixelsY, float outputSizeInPixelsX, float outputSizeInPixelsY)
{
    float param = outputSizeInPixelsX;
    con0.x = asuint(inputViewportInPixelsX * ARcpF1(param));
    float param_1 = outputSizeInPixelsY;
    con0.y = asuint(inputViewportInPixelsY * ARcpF1(param_1));
    float param_2 = 0.5f;
    float param_3 = outputSizeInPixelsX;
    float param_4 = 0.5f;
    con0.z = asuint(((AF1_x(param_2) * inputViewportInPixelsX) * ARcpF1(param_3)) - AF1_x(param_4));
    float param_5 = 0.5f;
    float param_6 = outputSizeInPixelsY;
    float param_7 = 0.5f;
    con0.w = asuint(((AF1_x(param_5) * inputViewportInPixelsY) * ARcpF1(param_6)) - AF1_x(param_7));
    float param_8 = inputSizeInPixelsX;
    con1.x = asuint(ARcpF1(param_8));
    float param_9 = inputSizeInPixelsY;
    con1.y = asuint(ARcpF1(param_9));
    float param_10 = 1.0f;
    float param_11 = inputSizeInPixelsX;
    con1.z = asuint(AF1_x(param_10) * ARcpF1(param_11));
    float param_12 = -1.0f;
    float param_13 = inputSizeInPixelsY;
    con1.w = asuint(AF1_x(param_12) * ARcpF1(param_13));
    float param_14 = -1.0f;
    float param_15 = inputSizeInPixelsX;
    con2.x = asuint(AF1_x(param_14) * ARcpF1(param_15));
    float param_16 = 2.0f;
    float param_17 = inputSizeInPixelsY;
    con2.y = asuint(AF1_x(param_16) * ARcpF1(param_17));
    float param_18 = 1.0f;
    float param_19 = inputSizeInPixelsX;
    con2.z = asuint(AF1_x(param_18) * ARcpF1(param_19));
    float param_20 = 2.0f;
    float param_21 = inputSizeInPixelsY;
    con2.w = asuint(AF1_x(param_20) * ARcpF1(param_21));
    float param_22 = 0.0f;
    float param_23 = inputSizeInPixelsX;
    con3.x = asuint(AF1_x(param_22) * ARcpF1(param_23));
    float param_24 = 4.0f;
    float param_25 = inputSizeInPixelsY;
    con3.y = asuint(AF1_x(param_24) * ARcpF1(param_25));
    con3.w = 0u;
    con3.z = 0u;
}

int2 gather_base(float2 p)
{
    return int2(floor((p * float2(model_size())) - 0.5f.xx));
}

float3 fetch(int2 p)
{
    return t0.Load(int3(clamp(p, int2(0, 0), model_size() - int2(1, 1)), 0)).xyz;
}

float4 gather(float2 p, int c)
{
    float2 param = p;
    int2 b = gather_base(param);
    int2 param_1 = b + int2(0, 1);
    int2 param_2 = b + int2(1, 1);
    int2 param_3 = b + int2(1, 0);
    int2 param_4 = b;
    return float4(fetch(param_1)[c], fetch(param_2)[c], fetch(param_3)[c], fetch(param_4)[c]);
}

float4 FsrEasuRF(float2 p)
{
    float2 param = p;
    int param_1 = 0;
    return gather(param, param_1);
}

float4 FsrEasuGF(float2 p)
{
    float2 param = p;
    int param_1 = 1;
    return gather(param, param_1);
}

float4 FsrEasuBF(float2 p)
{
    float2 param = p;
    int param_1 = 2;
    return gather(param, param_1);
}

float4 AF4_x(float a)
{
    return a.xxxx;
}

float2 AF2_x(float a)
{
    return a.xx;
}

uint AU1_x(uint a)
{
    return a;
}

float APrxLoRcpF1(float a)
{
    uint param = 2129690299u;
    return asfloat(AU1_x(param) - asuint(a));
}

float ASatF1(float x)
{
    float param = 0.0f;
    float param_1 = 1.0f;
    return clamp(x, AF1_x(param), AF1_x(param_1));
}

void FsrEasuSetF(inout float2 dir, inout float len, float2 pp, bool biS, bool biT, bool biU, bool biV, float lA, float lB, float lC, float lD, float lE)
{
    float param = 0.0f;
    float w = AF1_x(param);
    if (biS)
    {
        float param_1 = 1.0f;
        float param_2 = 1.0f;
        w = (AF1_x(param_1) - pp.x) * (AF1_x(param_2) - pp.y);
    }
    if (biT)
    {
        float param_3 = 1.0f;
        w = pp.x * (AF1_x(param_3) - pp.y);
    }
    if (biU)
    {
        float param_4 = 1.0f;
        w = (AF1_x(param_4) - pp.x) * pp.y;
    }
    if (biV)
    {
        w = pp.x * pp.y;
    }
    float dc = lD - lC;
    float cb_1 = lC - lB;
    float lenX = max(abs(dc), abs(cb_1));
    float param_5 = lenX;
    lenX = APrxLoRcpF1(param_5);
    float dirX = lD - lB;
    dir.x += (dirX * w);
    float param_6 = abs(dirX) * lenX;
    lenX = ASatF1(param_6);
    lenX *= lenX;
    len += (lenX * w);
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    float param_7 = lenY;
    lenY = APrxLoRcpF1(param_7);
    float dirY = lE - lA;
    dir.y += (dirY * w);
    float param_8 = abs(dirY) * lenY;
    lenY = ASatF1(param_8);
    lenY *= lenY;
    len += (lenY * w);
}

float APrxLoRsqF1(float a)
{
    uint param = 1597275508u;
    uint param_1 = 1u;
    return asfloat(AU1_x(param) - (asuint(a) >> AU1_x(param_1)));
}

float3 AMin3F3(float3 x, float3 y, float3 z)
{
    return min(x, min(y, z));
}

float3 AMax3F3(float3 x, float3 y, float3 z)
{
    return max(x, max(y, z));
}

float3 AF3_x(float a)
{
    return a.xxx;
}

void FsrEasuTapF(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c)
{
    float2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = (v.x * v.x) + (v.y * v.y);
    d2 = min(d2, clp);
    float param = 0.4000000059604644775390625f;
    float param_1 = -1.0f;
    float wB = (AF1_x(param) * d2) + AF1_x(param_1);
    float param_2 = -1.0f;
    float wA = (lob * d2) + AF1_x(param_2);
    wB *= wB;
    wA *= wA;
    float param_3 = 1.5625f;
    float param_4 = -0.5625f;
    wB = (AF1_x(param_3) * wB) + AF1_x(param_4);
    float w = wB * wA;
    aC += (c * w);
    aW += w;
}

void FsrEasuF(out float3 pix, uint2 ip, uint4 con0, uint4 con1, uint4 con2, uint4 con3)
{
    float2 pp = (float2(ip) * asfloat(con0.xy)) + asfloat(con0.zw);
    float2 fp = floor(pp);
    pp -= fp;
    float2 p0 = (fp * asfloat(con1.xy)) + asfloat(con1.zw);
    float2 p1 = p0 + asfloat(con2.xy);
    float2 p2 = p0 + asfloat(con2.zw);
    float2 p3 = p0 + asfloat(con3.xy);
    float2 param = p0;
    float4 bczzR = FsrEasuRF(param);
    float2 param_1 = p0;
    float4 bczzG = FsrEasuGF(param_1);
    float2 param_2 = p0;
    float4 bczzB = FsrEasuBF(param_2);
    float2 param_3 = p1;
    float4 ijfeR = FsrEasuRF(param_3);
    float2 param_4 = p1;
    float4 ijfeG = FsrEasuGF(param_4);
    float2 param_5 = p1;
    float4 ijfeB = FsrEasuBF(param_5);
    float2 param_6 = p2;
    float4 klhgR = FsrEasuRF(param_6);
    float2 param_7 = p2;
    float4 klhgG = FsrEasuGF(param_7);
    float2 param_8 = p2;
    float4 klhgB = FsrEasuBF(param_8);
    float2 param_9 = p3;
    float4 zzonR = FsrEasuRF(param_9);
    float2 param_10 = p3;
    float4 zzonG = FsrEasuGF(param_10);
    float2 param_11 = p3;
    float4 zzonB = FsrEasuBF(param_11);
    float param_12 = 0.5f;
    float param_13 = 0.5f;
    float4 bczzL = (bczzB * AF4_x(param_12)) + ((bczzR * AF4_x(param_13)) + bczzG);
    float param_14 = 0.5f;
    float param_15 = 0.5f;
    float4 ijfeL = (ijfeB * AF4_x(param_14)) + ((ijfeR * AF4_x(param_15)) + ijfeG);
    float param_16 = 0.5f;
    float param_17 = 0.5f;
    float4 klhgL = (klhgB * AF4_x(param_16)) + ((klhgR * AF4_x(param_17)) + klhgG);
    float param_18 = 0.5f;
    float param_19 = 0.5f;
    float4 zzonL = (zzonB * AF4_x(param_18)) + ((zzonR * AF4_x(param_19)) + zzonG);
    float bL = bczzL.x;
    float cL = bczzL.y;
    float iL = ijfeL.x;
    float jL = ijfeL.y;
    float fL = ijfeL.z;
    float eL = ijfeL.w;
    float kL = klhgL.x;
    float lL = klhgL.y;
    float hL = klhgL.z;
    float gL = klhgL.w;
    float oL = zzonL.z;
    float nL = zzonL.w;
    float param_20 = 0.0f;
    float2 dir = AF2_x(param_20);
    float param_21 = 0.0f;
    float len = AF1_x(param_21);
    float2 param_22 = dir;
    float param_23 = len;
    float2 param_24 = pp;
    bool param_25 = true;
    bool param_26 = false;
    bool param_27 = false;
    bool param_28 = false;
    float param_29 = bL;
    float param_30 = eL;
    float param_31 = fL;
    float param_32 = gL;
    float param_33 = jL;
    FsrEasuSetF(param_22, param_23, param_24, param_25, param_26, param_27, param_28, param_29, param_30, param_31, param_32, param_33);
    dir = param_22;
    len = param_23;
    float2 param_34 = dir;
    float param_35 = len;
    float2 param_36 = pp;
    bool param_37 = false;
    bool param_38 = true;
    bool param_39 = false;
    bool param_40 = false;
    float param_41 = cL;
    float param_42 = fL;
    float param_43 = gL;
    float param_44 = hL;
    float param_45 = kL;
    FsrEasuSetF(param_34, param_35, param_36, param_37, param_38, param_39, param_40, param_41, param_42, param_43, param_44, param_45);
    dir = param_34;
    len = param_35;
    float2 param_46 = dir;
    float param_47 = len;
    float2 param_48 = pp;
    bool param_49 = false;
    bool param_50 = false;
    bool param_51 = true;
    bool param_52 = false;
    float param_53 = fL;
    float param_54 = iL;
    float param_55 = jL;
    float param_56 = kL;
    float param_57 = nL;
    FsrEasuSetF(param_46, param_47, param_48, param_49, param_50, param_51, param_52, param_53, param_54, param_55, param_56, param_57);
    dir = param_46;
    len = param_47;
    float2 param_58 = dir;
    float param_59 = len;
    float2 param_60 = pp;
    bool param_61 = false;
    bool param_62 = false;
    bool param_63 = false;
    bool param_64 = true;
    float param_65 = gL;
    float param_66 = jL;
    float param_67 = kL;
    float param_68 = lL;
    float param_69 = oL;
    FsrEasuSetF(param_58, param_59, param_60, param_61, param_62, param_63, param_64, param_65, param_66, param_67, param_68, param_69);
    dir = param_58;
    len = param_59;
    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    float param_70 = 3.0517578125e-05f;
    bool zro = dirR < AF1_x(param_70);
    float param_71 = dirR;
    dirR = APrxLoRsqF1(param_71);
    float _984;
    if (zro)
    {
        float param_72 = 1.0f;
        _984 = AF1_x(param_72);
    }
    else
    {
        _984 = dirR;
    }
    dirR = _984;
    float _993;
    if (zro)
    {
        float param_73 = 1.0f;
        _993 = AF1_x(param_73);
    }
    else
    {
        _993 = dir.x;
    }
    dir.x = _993;
    float param_74 = dirR;
    dir *= AF2_x(param_74);
    float param_75 = 0.5f;
    len *= AF1_x(param_75);
    len *= len;
    float param_76 = max(abs(dir.x), abs(dir.y));
    float stretch = ((dir.x * dir.x) + (dir.y * dir.y)) * APrxLoRcpF1(param_76);
    float param_77 = 1.0f;
    float param_78 = 1.0f;
    float param_79 = 1.0f;
    float param_80 = -0.5f;
    float2 len2 = float2(AF1_x(param_77) + ((stretch - AF1_x(param_78)) * len), AF1_x(param_79) + (AF1_x(param_80) * len));
    float param_81 = 0.5f;
    float param_82 = -0.2899999916553497314453125f;
    float lob = AF1_x(param_81) + (AF1_x(param_82) * len);
    float param_83 = lob;
    float clp = APrxLoRcpF1(param_83);
    float3 param_84 = float3(ijfeR.z, ijfeG.z, ijfeB.z);
    float3 param_85 = float3(klhgR.w, klhgG.w, klhgB.w);
    float3 param_86 = float3(ijfeR.y, ijfeG.y, ijfeB.y);
    float3 min4 = min(AMin3F3(param_84, param_85, param_86), float3(klhgR.x, klhgG.x, klhgB.x));
    float3 param_87 = float3(ijfeR.z, ijfeG.z, ijfeB.z);
    float3 param_88 = float3(klhgR.w, klhgG.w, klhgB.w);
    float3 param_89 = float3(ijfeR.y, ijfeG.y, ijfeB.y);
    float3 max4 = max(AMax3F3(param_87, param_88, param_89), float3(klhgR.x, klhgG.x, klhgB.x));
    float param_90 = 0.0f;
    float3 aC = AF3_x(param_90);
    float param_91 = 0.0f;
    float aW = AF1_x(param_91);
    float3 param_92 = aC;
    float param_93 = aW;
    float2 param_94 = float2(0.0f, -1.0f) - pp;
    float2 param_95 = dir;
    float2 param_96 = len2;
    float param_97 = lob;
    float param_98 = clp;
    float3 param_99 = float3(bczzR.x, bczzG.x, bczzB.x);
    FsrEasuTapF(param_92, param_93, param_94, param_95, param_96, param_97, param_98, param_99);
    aC = param_92;
    aW = param_93;
    float3 param_100 = aC;
    float param_101 = aW;
    float2 param_102 = float2(1.0f, -1.0f) - pp;
    float2 param_103 = dir;
    float2 param_104 = len2;
    float param_105 = lob;
    float param_106 = clp;
    float3 param_107 = float3(bczzR.y, bczzG.y, bczzB.y);
    FsrEasuTapF(param_100, param_101, param_102, param_103, param_104, param_105, param_106, param_107);
    aC = param_100;
    aW = param_101;
    float3 param_108 = aC;
    float param_109 = aW;
    float2 param_110 = float2(-1.0f, 1.0f) - pp;
    float2 param_111 = dir;
    float2 param_112 = len2;
    float param_113 = lob;
    float param_114 = clp;
    float3 param_115 = float3(ijfeR.x, ijfeG.x, ijfeB.x);
    FsrEasuTapF(param_108, param_109, param_110, param_111, param_112, param_113, param_114, param_115);
    aC = param_108;
    aW = param_109;
    float3 param_116 = aC;
    float param_117 = aW;
    float2 param_118 = float2(0.0f, 1.0f) - pp;
    float2 param_119 = dir;
    float2 param_120 = len2;
    float param_121 = lob;
    float param_122 = clp;
    float3 param_123 = float3(ijfeR.y, ijfeG.y, ijfeB.y);
    FsrEasuTapF(param_116, param_117, param_118, param_119, param_120, param_121, param_122, param_123);
    aC = param_116;
    aW = param_117;
    float3 param_124 = aC;
    float param_125 = aW;
    float2 param_126 = 0.0f.xx - pp;
    float2 param_127 = dir;
    float2 param_128 = len2;
    float param_129 = lob;
    float param_130 = clp;
    float3 param_131 = float3(ijfeR.z, ijfeG.z, ijfeB.z);
    FsrEasuTapF(param_124, param_125, param_126, param_127, param_128, param_129, param_130, param_131);
    aC = param_124;
    aW = param_125;
    float3 param_132 = aC;
    float param_133 = aW;
    float2 param_134 = float2(-1.0f, 0.0f) - pp;
    float2 param_135 = dir;
    float2 param_136 = len2;
    float param_137 = lob;
    float param_138 = clp;
    float3 param_139 = float3(ijfeR.w, ijfeG.w, ijfeB.w);
    FsrEasuTapF(param_132, param_133, param_134, param_135, param_136, param_137, param_138, param_139);
    aC = param_132;
    aW = param_133;
    float3 param_140 = aC;
    float param_141 = aW;
    float2 param_142 = 1.0f.xx - pp;
    float2 param_143 = dir;
    float2 param_144 = len2;
    float param_145 = lob;
    float param_146 = clp;
    float3 param_147 = float3(klhgR.x, klhgG.x, klhgB.x);
    FsrEasuTapF(param_140, param_141, param_142, param_143, param_144, param_145, param_146, param_147);
    aC = param_140;
    aW = param_141;
    float3 param_148 = aC;
    float param_149 = aW;
    float2 param_150 = float2(2.0f, 1.0f) - pp;
    float2 param_151 = dir;
    float2 param_152 = len2;
    float param_153 = lob;
    float param_154 = clp;
    float3 param_155 = float3(klhgR.y, klhgG.y, klhgB.y);
    FsrEasuTapF(param_148, param_149, param_150, param_151, param_152, param_153, param_154, param_155);
    aC = param_148;
    aW = param_149;
    float3 param_156 = aC;
    float param_157 = aW;
    float2 param_158 = float2(2.0f, 0.0f) - pp;
    float2 param_159 = dir;
    float2 param_160 = len2;
    float param_161 = lob;
    float param_162 = clp;
    float3 param_163 = float3(klhgR.z, klhgG.z, klhgB.z);
    FsrEasuTapF(param_156, param_157, param_158, param_159, param_160, param_161, param_162, param_163);
    aC = param_156;
    aW = param_157;
    float3 param_164 = aC;
    float param_165 = aW;
    float2 param_166 = float2(1.0f, 0.0f) - pp;
    float2 param_167 = dir;
    float2 param_168 = len2;
    float param_169 = lob;
    float param_170 = clp;
    float3 param_171 = float3(klhgR.w, klhgG.w, klhgB.w);
    FsrEasuTapF(param_164, param_165, param_166, param_167, param_168, param_169, param_170, param_171);
    aC = param_164;
    aW = param_165;
    float3 param_172 = aC;
    float param_173 = aW;
    float2 param_174 = float2(1.0f, 2.0f) - pp;
    float2 param_175 = dir;
    float2 param_176 = len2;
    float param_177 = lob;
    float param_178 = clp;
    float3 param_179 = float3(zzonR.z, zzonG.z, zzonB.z);
    FsrEasuTapF(param_172, param_173, param_174, param_175, param_176, param_177, param_178, param_179);
    aC = param_172;
    aW = param_173;
    float3 param_180 = aC;
    float param_181 = aW;
    float2 param_182 = float2(0.0f, 2.0f) - pp;
    float2 param_183 = dir;
    float2 param_184 = len2;
    float param_185 = lob;
    float param_186 = clp;
    float3 param_187 = float3(zzonR.w, zzonG.w, zzonB.w);
    FsrEasuTapF(param_180, param_181, param_182, param_183, param_184, param_185, param_186, param_187);
    aC = param_180;
    aW = param_181;
    float param_188 = aW;
    float param_189 = ARcpF1(param_188);
    pix = min(max4, max(min4, aC * AF3_x(param_189)));
}

float3 easu(int2 xy)
{
    float2 m = float2(model_size());
    float param_4 = m.x;
    float param_5 = m.y;
    float param_6 = m.x;
    float param_7 = m.y;
    float param_8 = float(cb.Load(8));
    float param_9 = float(cb.Load(12));
    uint4 param;
    uint4 param_1;
    uint4 param_2;
    uint4 param_3;
    FsrEasuCon(param, param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8, param_9);
    uint4 con0 = param;
    uint4 con1 = param_1;
    uint4 con2 = param_2;
    uint4 con3 = param_3;
    uint2 param_11 = uint2(xy);
    uint4 param_12 = con0;
    uint4 param_13 = con1;
    uint4 param_14 = con2;
    uint4 param_15 = con3;
    float3 param_10;
    FsrEasuF(param_10, param_11, param_12, param_13, param_14, param_15);
    float3 c = param_10;
    return c;
}

float catmull_rom(inout float x)
{
    x = abs(x);
    if (x < 1.0f)
    {
        return ((((1.5f * x) * x) * x) - ((2.5f * x) * x)) + 1.0f;
    }
    if (x < 2.0f)
    {
        return ((((((-0.5f) * x) * x) * x) + ((2.5f * x) * x)) - (4.0f * x)) + 2.0f;
    }
    return 0.0f;
}

float lanczos3(inout float x)
{
    x = abs(x);
    if (x < 9.9999997473787516355514526367188e-06f)
    {
        return 1.0f;
    }
    if (x >= 3.0f)
    {
        return 0.0f;
    }
    float px = 3.1415927410125732421875f * x;
    return ((3.0f * sin(px)) * sin(px / 3.0f)) / (px * px);
}

float kernel(int radius, float x)
{
    if (radius == 1)
    {
        return max(0.0f, 1.0f - abs(x));
    }
    float _1617;
    if (radius == 2)
    {
        float param = x;
        float _1622 = catmull_rom(param);
        _1617 = _1622;
    }
    else
    {
        float param_1 = x;
        float _1626 = lanczos3(param_1);
        _1617 = _1626;
    }
    return _1617;
}

float3 separable(int2 xy, int radius)
{
    float2 p = ((float2(xy) + 0.5f.xx) * (float2(model_size()) / float2(float(cb.Load(8)), float(cb.Load(12))))) - 0.5f.xx;
    int2 base = int2(floor(p));
    float2 f = p - float2(base);
    float3 sum = 0.0f.xxx;
    float wsum = 0.0f;
    int _1662 = 1 - radius;
    for (int j = _1662; j <= radius; j++)
    {
        int param = radius;
        float param_1 = float(j) - f.y;
        float wy = kernel(param, param_1);
        int _1683 = 1 - radius;
        for (int i = _1683; i <= radius; i++)
        {
            int param_2 = radius;
            float param_3 = float(i) - f.x;
            float w = wy * kernel(param_2, param_3);
            int2 param_4 = base + int2(i, j);
            sum += (fetch(param_4) * w);
            wsum += w;
        }
    }
    float3 c = sum / wsum.xxx;
    int2 param_5 = base;
    float3 a = fetch(param_5);
    int2 param_6 = base + int2(1, 0);
    float3 b = fetch(param_6);
    int2 param_7 = base + int2(0, 1);
    float3 d = fetch(param_7);
    int2 param_8 = base + int2(1, 1);
    float3 e = fetch(param_8);
    return clamp(c, min(min(a, b), min(d, e)), max(max(a, b), max(d, e)));
}

void comp_main()
{
    uint2 p = gl_GlobalInvocationID.xy;
    uint2 param = p;
    if (outside(param))
    {
        return;
    }
    int2 xy = int2(p);
    uint scaler = cb.Load(24);
    float3 _1786;
    if (scaler == 3u)
    {
        int2 param_1 = xy;
        _1786 = easu(param_1);
    }
    else
    {
        int _1795;
        if (scaler == 2u)
        {
            _1795 = 3;
        }
        else
        {
            _1795 = (scaler == 1u) ? 2 : 1;
        }
        int2 param_2 = xy;
        int param_3 = _1795;
        _1786 = separable(param_2, param_3);
    }
    float3 c = _1786;
    u0[xy] = float4(c, 1.0f);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
