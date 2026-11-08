#include "legacy_common.hlsli"

RWTexture2D<float4> BlockMean : register(u1);

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;
  const int2 block_max = int2((Size + 31u) / 32u) - int2(1, 1);
  const float2 block_pos = (float2(pixel) + 0.5) / 32.0 - 0.5;
  const int2 b0 = clamp(int2(floor(block_pos)), int2(0, 0), block_max);
  const int2 b1 = min(b0 + int2(1, 1), block_max);
  const float2 f = saturate(block_pos - float2(b0));
  const float4 m00 = BlockMean.Load((int2)b0);
  const float4 m10 = BlockMean.Load((int2)int2(b1.x, b0.y));
  const float4 m01 = BlockMean.Load((int2)int2(b0.x, b1.y));
  const float4 m11 = BlockMean.Load((int2)b1);
  const float decoded_mean =
      m00.x * (1.0 - f.x) * (1.0 - f.y) + m10.x * f.x * (1.0 - f.y)
      + m01.x * (1.0 - f.x) * f.y + m11.x * f.x * f.y;
  const float original_mean =
      m00.y * (1.0 - f.x) * (1.0 - f.y) + m10.y * f.x * (1.0 - f.y)
      + m01.y * (1.0 - f.x) * f.y + m11.y * f.x * f.y;
  // The SDR dark gate (rc11, DATA-11; the v6 commit's rule): only a block
  // whose brightest untouched pixel is near-black gives back NR's lift.
  // Elsewhere the "lift" is NR's own edit plus zero-mean noise, which the
  // clamp below rectifies into a per-block darkening (rc8's HDR pedestal
  // flicker, on the SDR path).  Padding.x (dword 16) is the gate in the
  // surface's encoded luminance; NRSdrBlackRestore=1 sends one no block
  // exceeds, the rc10 behaviour. A hard gate on this interpolated maximum
  // still has a step at its boundary; mode 2 (Padding.y) tapers the restore
  // to zero before that boundary instead.
  const float original_max =
      m00.z * (1.0 - f.x) * (1.0 - f.y) + m10.z * f.x * (1.0 - f.y)
      + m01.z * (1.0 - f.x) * f.y + m11.z * f.x * f.y;
  const float weight = BlackRestoreWeight(original_max, Padding.x, Padding.y == 2.0);
  if (weight <= 0.0) return;
  const float pedestal = clamp(decoded_mean - original_mean, 0.0, 0.025) * weight;
  if (pedestal <= 0.0) return;
  const float4 value = Output.Load((int2)pixel);
  Output[pixel] = float4(max(value.rgb - pedestal, 0.0), value.a);
}
