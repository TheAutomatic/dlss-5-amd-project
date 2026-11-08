// DLSSNR-AMD addition: v6_resolve of the last pass and the commit in one pass, where the commit has no pedestal to
// take off (PedestalGate 0: v6_commit then stores the resolved pixel as it is, in the output's encoding). The resolved
// pixel goes through RGBA16F as it would through the work image between the two, so every output bit is theirs.
#include "v6_common.hlsli"

float WorkHalf(float value) { return f16tof32(f32tof16(value)); }

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;
  const float4 original = Original.Load(int3(pixel, 0));
  const float3 resolved = ResolvePixel(pixel, original.rgb);
  const float4 value = float4(WorkHalf(resolved.r), WorkHalf(resolved.g), WorkHalf(resolved.b), WorkHalf(original.a));
  // v6_commit with no pedestal
  float3 result = value.rgb;
  if (Encoding == 4u) result = Bt709ToBt2020(result);
  if (Encoding == 2u || Encoding == 4u) {
    Output[VK_OUTPUT_PIXEL(pixel)] = float4(LinearToPQ(max(result, 0.0)), value.a);
  } else if (Encoding == 3u) {
    Output[VK_OUTPUT_PIXEL(pixel)] = float4(SrgbEncodeExtended(result), value.a);
  } else {
    Output[VK_OUTPUT_PIXEL(pixel)] = float4(result, value.a);
  }
}
