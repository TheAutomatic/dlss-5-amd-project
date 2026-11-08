#include "v6_common.hlsli"

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;
  const uint2 source_size = max(SourceSize, uint2(1, 1));
  // Past the source window (pre-SR's render subrect inside a larger Color
  // allocation) the frame is mirrored about its last row/column, so NR sees
  // image-like content there, not black or stale texels or a smeared edge.
  // Inside the window this is the identity.
  const uint2 period = max(source_size * 2u, uint2(3, 3)) - 2u;
  const uint2 folded = pixel % period;
  const uint2 mirrored =
      min(folded < source_size ? folded : period - folded, source_size - 1u);
  const uint2 source_pixel = SourceBase + mirrored;
  const float4 source = Original.Load(int3(source_pixel, 0));
  float3 linear_rgb = source.rgb;
  // Encoding: 1 scene-linear, 2 PQ, 3 sRGB, 4 PQ with BT.2020 primaries
  // (V6EncodingConstant).
  if (Encoding == 2u || Encoding == 4u) linear_rgb = PQToLinear(linear_rgb);
  if (Encoding == 3u) linear_rgb = SrgbDecodeExtended(linear_rgb);
  if (Encoding == 4u) linear_rgb = Bt2020ToBt709(linear_rgb);
  Output[pixel] = float4(linear_rgb, source.a);
}
