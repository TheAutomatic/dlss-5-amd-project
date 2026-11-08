#include "legacy_common.hlsli"

RWTexture2D<float4> BlockMean : register(u1);

// DLSSNR-AMD: the 256 partial sums of the block's four quantities, one row each (the sum, the untouched sum, the
// untouched max, the valid count - an integer <= 4 kept as a float: every partial sum of them is exact).
groupshared float BlockPart[4][256];

[numthreads(16, 16, 1)]
void main(uint3 group_id : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  const uint thread_index = group_thread.y * 16u + group_thread.x;
  const uint2 base = group_id.xy * 32u + group_thread.xy * 2u;
  float sum_decoded = 0.0;
  float sum_original = 0.0;
  float max_original = 0.0;
  uint valid = 0u;
  for (uint y = 0u; y < 2u; ++y) {
    for (uint x = 0u; x < 2u; ++x) {
      const uint2 pixel = base + uint2(x, y);
      if (all(pixel < Size)) {
#ifdef MEASURE_UNSHAPED
        // NR's own lift: this pass's resolve of the UNSHAPED model output
        // (t2 of the pass's codec set), not the look-shaped write surface.
        const float4 decoded =
            float4(ResolvePixel(pixel, OutputOriginal.Load(int3(pixel, 0)).rgb), 0.0);
#else
        const float4 decoded = Output.Load((int2)pixel);
#endif
        const float4 original = OutputOriginal.Load(int3(pixel, 0));
        sum_decoded += Luminance(decoded.rgb);
        const float untouched = Luminance(max(original.rgb, 0.0));
        sum_original += untouched;
        max_original = max(max_original, untouched);
        ++valid;
      }
    }
  }
  BlockPart[0][thread_index] = sum_decoded;
  BlockPart[1][thread_index] = sum_original;
  BlockPart[2][thread_index] = max_original;
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
    const float total_decoded = BlockPart[0][0];
    const float total_original = BlockPart[1][0];
    const float total_max_original = BlockPart[2][0];
    const float count = max(BlockPart[3][0], 1.0);
    // z: the untouched block MAX, for legacy_apply's dark gate (the v6
    // pedestal reduce's contract, rc11).
    BlockMean[group_id.xy] =
        float4(total_decoded / count, total_original / count, total_max_original, 0.0);
  }
}
