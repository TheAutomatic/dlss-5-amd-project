// ours: legacy_decode and the black-level restore's reduce (legacy_reduce, or
// legacy_reduce_unshaped with MEASURE_UNSHAPED in Padding.x) in one pass, so that the frame can be read and
// written where it is: one group a 32 x 32 block, each thread its 2 x 2 pixels in the reduce's order. A pixel is
// read (t3, OutputOriginal) before it is written (u0, Output), by the same thread; nothing else reads it here.
// The values are the two programs': the decode's store, the reduce's sums over the stored value (the unshaped
// measure: the resolve of the unshaped answer, t6 / t7 at the sizes in Padding.zw) and the untouched pixel, summed
// in the reduce's order, thread 0 over the group's 256 partial sums.
#include "legacy_common.hlsli"

VK_BINDING(5) RWTexture2D<float4> BlockMean : register(u1);
VK_BINDING(6) Texture2D<float4> ProxyUnshaped : register(t4);
VK_BINDING(7) Texture2D<float4> NeuralUnshaped : register(t5);

// The 256 partial sums of the block's four quantities, one row each: 0 decoded, 1 original, 2 the original's max,
// 3 the valid count (an integer <= 4, kept as a float: every partial sum of them is exact).
groupshared float BlockPart[4][256];

// legacy_common.hlsli's ResolvePixel on the unshaped pair (HdrMode 0, the legacy codec's only caller here).
float3 ResolveUnshaped(uint2 pixel, float3 source) {
  const uint2 sizes = uint2(asuint(Padding.z), asuint(Padding.w));
  const uint2 proxy_size = max(sizes, uint2(1, 1));
  const uint2 proxy_pixel = min(
      uint2(((float2(pixel) + 0.5) * float2(proxy_size)) / float2(Size)),
      proxy_size - 1);
  const uint2 neural_size = max(sizes, uint2(1, 1));
  const uint2 neural_pixel = min(
      uint2(((float2(pixel) + 0.5) * float2(neural_size)) / float2(Size)),
      neural_size - 1);
  const float3 original = source;
  const float3 proxy = ProxyUnshaped.Load(int3(proxy_pixel, 0)).rgb;
  const float3 neural = NeuralUnshaped.Load(int3(neural_pixel, 0)).rgb;
  const float3 upgraded = neural;
  const float original_y = Luminance(original);
  const float upgraded_y = Luminance(upgraded);
  const float ratio = original_y == 0.0 ? 1.0 : clamp(upgraded_y / original_y, 0.0, 4.0);
  const float3 luminance_only = original * ratio;
  const float3 result = lerp(luminance_only, upgraded, ColorStrength);
  return result;
}

[numthreads(16, 16, 1)]
void main(uint3 group_id : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  const uint thread_index = group_thread.y * 16u + group_thread.x;
  const uint2 base = group_id.xy * 32u + group_thread.xy * 2u;
  // The decode of the thread's 2 x 2 first, then the reduce's own loop over them, written as legacy_reduce writes it:
  // its sums are the compiler's choice of fused multiply-adds, which follow the loop's shape (one loop with the decode
  // inside folded the running sum differently: one pixel in 1000-odd blocks a code off after the restore, DL:TB seq01).
  for (uint dy = 0u; dy < 2u; ++dy) {
    for (uint dx = 0u; dx < 2u; ++dx) {
      const uint2 pixel = base + uint2(dx, dy);
      if (all(pixel < Size)) {
        const float4 original = OutputOriginal.Load(int3(pixel, 0));
        Output[VK_OUTPUT_PIXEL(pixel)] = float4(ResolvePixel(pixel, original.rgb), original.a);
      }
    }
  }
  float sum_decoded = 0.0;
  float sum_original = 0.0;
  float max_original = 0.0;
  uint valid = 0u;
  [branch]
  if (Padding.x != 0.0) {
    for (uint y = 0u; y < 2u; ++y) {
      for (uint x = 0u; x < 2u; ++x) {
        const uint2 pixel = base + uint2(x, y);
        if (all(pixel < Size)) {
          const float4 original = OutputOriginal.Load(int3(pixel, 0));
          const float4 decoded = float4(ResolveUnshaped(pixel, original.rgb), 0.0);
          sum_decoded += Luminance(decoded.rgb);
          const float untouched = Luminance(max(original.rgb, 0.0));
          sum_original += untouched;
          max_original = max(max_original, untouched);
          ++valid;
        }
      }
    }
  } else {
    for (uint y = 0u; y < 2u; ++y) {
      for (uint x = 0u; x < 2u; ++x) {
        const uint2 pixel = base + uint2(x, y);
        if (all(pixel < Size)) {
          const float4 decoded = Output.Load((int2)VK_OUTPUT_PIXEL(pixel));
          const float4 original = OutputOriginal.Load(int3(pixel, 0));
          sum_decoded += Luminance(decoded.rgb);
          const float untouched = Luminance(max(original.rgb, 0.0));
          sum_original += untouched;
          max_original = max(max_original, untouched);
          ++valid;
        }
      }
    }
  }
  BlockPart[0][thread_index] = sum_decoded;
  BlockPart[1][thread_index] = sum_original;
  BlockPart[2][thread_index] = max_original;
  BlockPart[3][thread_index] = float(valid);
  GroupMemoryBarrierWithGroupSync();
  // The reduce's four running totals, each in its order (partial 0, 1, ... 255), one thread each instead of all four
  // on thread 0: the same additions (and maxima) in the same order, side by side.
  if (thread_index < 4u) {
    const uint row = thread_index;
    float total = 0.0;
    [unroll(16)]
    for (uint i = 0u; i < 256u; ++i) {
      const float v = BlockPart[row][i];
      total = row == 2u ? max(total, v) : total + v;
    }
    BlockPart[row][0] = total;
  }
  GroupMemoryBarrierWithGroupSync();
  if (thread_index == 0u) {
    const float total_valid = BlockPart[3][0];
    const float count = max(total_valid, 1.0);
    BlockMean[group_id.xy] =
        float4(BlockPart[0][0] / count, BlockPart[1][0] / count, BlockPart[2][0], 0.0);
  }
}
