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
    uint2 pc_source_size : packoffset(c0.z);
};

Texture2D<float4> Original : register(t0);
RWTexture2D<float4> Output : register(u4);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

float3 srgb_encode(inout float3 c)
{
    c = clamp(c, 0.0f.xxx, 1.0f.xxx);
    float3 _27 = (pow(c, 0.4166666567325592041015625f.xxx) * 1.05499994754791259765625f) - 0.054999999701976776123046875f.xxx;
    float3 _30 = c * 12.9200000762939453125f;
    bool3 _36 = bool3(c.x <= 0.003130800090730190277099609375f.xxx.x, c.y <= 0.003130800090730190277099609375f.xxx.y, c.z <= 0.003130800090730190277099609375f.xxx.z);
    return float3(_36.x ? _30.x : _27.x, _36.y ? _30.y : _27.y, _36.z ? _30.z : _27.z);
}

void comp_main()
{
    uint2 pixel = gl_GlobalInvocationID.xy;
    if (any(bool2(pixel.x >= pc_size.x, pixel.y >= pc_size.y)))
    {
        return;
    }
    uint2 source_size = max(pc_source_size, uint2(1u, 1u));
    float2 scale = float2(source_size) / float2(pc_size);
    float2 center = (float2(pixel) + 0.5f.xx) * scale;
    float2 footprint_min = max(center - (scale * 0.5f), 0.0f.xx);
    float2 footprint_max = min(center + (scale * 0.5f), float2(source_size));
    int2 first = int2(floor(footprint_min));
    int2 last = int2(ceil(footprint_max));
    float3 sum = 0.0f.xxx;
    float weight = 0.0f;
    for (int y = first.y; y < last.y; y++)
    {
        for (int x = first.x; x < last.x; x++)
        {
            float2 overlap = min(float2(float(x + 1), float(y + 1)), footprint_max) - max(float2(float(x), float(y)), footprint_min);
            float w = max(overlap.x, 0.0f) * max(overlap.y, 0.0f);
            float3 texel = Original.Load(int3(int2(x, y), 0)).xyz;
            uint3 bits = asuint(texel) & uint3(2139095040u, 2139095040u, 2139095040u);
            bool3 _191 = bool3(texel.x > 0.0f.xxx.x, texel.y > 0.0f.xxx.y, texel.z > 0.0f.xxx.z);
            float3 _192 = float3(_191.x ? 65504.0f.xxx.x : 0.0f.xxx.x, _191.y ? 65504.0f.xxx.y : 0.0f.xxx.y, _191.z ? 65504.0f.xxx.z : 0.0f.xxx.z);
            bool3 _195 = bool3(bits.x == uint3(2139095040u, 2139095040u, 2139095040u).x, bits.y == uint3(2139095040u, 2139095040u, 2139095040u).y, bits.z == uint3(2139095040u, 2139095040u, 2139095040u).z);
            sum += (float3(_195.x ? _192.x : texel.x, _195.y ? _192.y : texel.y, _195.z ? _192.z : texel.z) * w);
            weight += w;
        }
    }
    float3 _211;
    if (weight > 0.0f)
    {
        _211 = max(sum / weight.xxx, 0.0f.xxx);
    }
    else
    {
        _211 = 0.0f.xxx;
    }
    float3 proxy = _211;
    float3 param = proxy;
    float3 _229 = srgb_encode(param);
    Output[int2(pixel)] = float4(_229, 1.0f);
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
