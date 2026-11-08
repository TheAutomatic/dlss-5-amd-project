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
    uint2 pc_net_size : packoffset(c0.z);
    uint pc_view : packoffset(c1);
};

Texture2D<float4> Proxy : register(t0);
Texture2D<float4> Neural : register(t1);
RWTexture2D<float4> Output : register(u4);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    uint2 pixel = gl_GlobalInvocationID.xy;
    if (any(bool2(pixel.x >= pc_size.x, pixel.y >= pc_size.y)))
    {
        return;
    }
    uint2 ns = max(pc_net_size, uint2(1u, 1u));
    int2 q = int2(min(uint2(((float2(pixel) + 0.5f.xx) * float2(ns)) / float2(pc_size)), (ns - uint2(1u, 1u))));
    float3 p = Proxy.Load(int3(q, 0)).xyz;
    float3 n = Neural.Load(int3(q, 0)).xyz;
    float3 _84;
    if (pc_view == 1u)
    {
        _84 = p;
    }
    else
    {
        float3 _93;
        if (pc_view == 2u)
        {
            _93 = n;
        }
        else
        {
            _93 = clamp(0.5f.xxx + ((n - p) * 20.0f), 0.0f.xxx, 1.0f.xxx);
        }
        _84 = _93;
    }
    float3 v = _84;
    Output[int2(pixel)] = float4(v, 1.0f);
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
