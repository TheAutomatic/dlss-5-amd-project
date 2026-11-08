#include "v6_common.hlsli"

VK_BINDING(4) RWTexture2D<float4> BlockMean : register(u0);

// DLSSNR-AMD: the 256 partial sums of the block's four quantities, one row each (the sum, the untouched sum, the
// untouched max, the valid count - an integer <= 4 kept as a float: every partial sum of them is exact).
groupshared float BlockPart[4][256];

[numthreads(16, 16, 1)]
void main(uint3 group_id : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  const uint thread_index = group_thread.y * 16u + group_thread.x;
  const uint2 base = group_id.xy * 32u + group_thread.xy * 2u;
  float sum_final = 0.0;
  float sum_untouched = 0.0;
  float max_untouched = 0.0;
  uint valid = 0u;
  for (uint y = 0u; y < 2u; ++y) {
    for (uint x = 0u; x < 2u; ++x) {
      const uint2 pixel = base + uint2(x, y);
      if (all(pixel < Size)) {
#ifdef MEASURE_UNSHAPED
        // Pass 0's codec set [work0, P, N, norm_scale]: the resolve of the
        // UNSHAPED model output - NR's own lift - against the untouched work0.
        const float3 untouched_rgb = Original.Load(int3(pixel, 0)).rgb;
        sum_final += Luminance(ResolvePixel(pixel, untouched_rgb));
        const float untouched = Luminance(max(untouched_rgb, 0.0));
#else
        sum_final += Luminance(Original.Load(int3(pixel, 0)).rgb);
        const float untouched =
            Luminance(max(OutputOriginal.Load(int3(pixel, 0)).rgb, 0.0));
#endif
        sum_untouched += untouched;
        max_untouched = max(max_untouched, untouched);
        ++valid;
      }
    }
  }
  BlockPart[0][thread_index] = sum_final;
  BlockPart[1][thread_index] = sum_untouched;
  BlockPart[2][thread_index] = max_untouched;
  BlockPart[3][thread_index] = float(valid);
  GroupMemoryBarrierWithGroupSync();
  // DLSSNR-AMD: the four running totals on four threads instead of all four on thread 0, each in its order
  // (partial 0, 1, ... 255): the same additions and maxima, side by side.
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
    const float total_final = BlockPart[0][0];
    const float total_untouched = BlockPart[1][0];
    const float total_max_untouched = BlockPart[2][0];
    const float count = max(BlockPart[3][0], 1.0);
    // z carries the untouched block MAX: the commit's dark gate needs to know
    // whether the block is genuinely near-black, not merely dark ON AVERAGE -
    // a bright feature crossing a dark block lifts the mean lift estimate and
    // turns the per-block subtraction into visible blotches (KCD2 barn-scene
    // capture: 98% of pedestal mass in 32% mean-gated mixed blocks).
    BlockMean[group_id.xy] =
        float4(total_final / count, total_untouched / count, total_max_untouched, 0.0);
  }
}
