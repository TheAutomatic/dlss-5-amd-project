// DLSSNR-AMD addition: look_upsample and legacy_decode in one pass, below the frame's extent (the edge-aware transport,
// SDR). look_upsample stored P_up and N'_up into two RGBA16F images at the frame's extent and legacy_decode read each
// pixel's own texel back; here each pixel's N'_up is made where the decode needs it, through the same RGBA16F rounding
// (the store's: toward zero, as f32tof16), so every output bit is theirs. The SDR decode reads no P (HdrMode 0).
// Bindings: legacy_common's (0 the frame, 1 P at the network's extent, 2 N' with G_low in .a, 3 the frame, 4 the
// output); constants: legacy_common's, ProxySize = the network's extent (look_upsample's NrSize).
#include "legacy_common.hlsli"

static const float kRatioFloor = 1.0 / 1024.0;  // look_common.hlsli
static const float kGuideFloor = 1e-8;          // look_common.hlsli
static const float kSpatialSigma = 0.5;         // look_upsample
static const float kRangeSigma = 0.5;
static const int kTile = 20;
groupshared float4 tile_edit[kTile * kTile];
groupshared float3 tile_proxy[kTile * kTile];

uint TileIndex(int2 local) {
  const int2 at = clamp(local, 0, kTile - 1);
  return uint(at.y * kTile + at.x);
}

float StoredHalf(float value) { return f16tof32(f32tof16(value)); }

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID, uint3 group_id : SV_GroupID,
          uint group_index : SV_GroupIndex) {
  // ---- look_upsample, its N'_up (SDR reference) ----
  const uint2 NrSize = ProxySize;
  const int2 last = int2(NrSize) - 1;
  const int2 origin = int2(floor((float2(group_id.xy * 16u) + 0.5) * float2(NrSize)
                                 / float2(Size) - 0.5)) - 1;
  for (uint i = group_index; i < uint(kTile * kTile); i += 256u) {
    const int2 tap = clamp(origin + int2(i % uint(kTile), i / uint(kTile)), 0, last);
    const float4 neural = Neural.Load(int3(tap, 0));
    const float3 proxy = SrgbDecode(Proxy.Load(int3(tap, 0)).rgb);
    tile_proxy[i] = proxy;
    tile_edit[i] = float4(log2(max(SrgbDecode(neural.rgb), kRatioFloor))
                              - log2(max(proxy, kRatioFloor)),
                          neural.a);
  }
  GroupMemoryBarrierWithGroupSync();
  const int2 pixel = int2(dispatch_id.xy);
  if (any(dispatch_id.xy >= Size)) return;
  const float2 position = (float2(pixel) + 0.5) * float2(NrSize) / float2(Size) - 0.5;
  const int2 base = int2(floor(position));
  const float3 reference = Original.Load(int3(pixel, 0)).rgb;
  const float3 original_up = SrgbDecode(reference);
  const float guide_full = log2(max(Luminance(max(original_up, 0.0)), kGuideFloor));

  float3 joint = 0.0;
  float joint_weight = 0.0;
  float3 spatial = 0.0;
  float spatial_weight = 0.0;
  for (int dy = -1; dy <= 2; ++dy) {
    for (int dx = -1; dx <= 2; ++dx) {
      const int2 tap = base + int2(dx, dy);
      const float2 distance = float2(tap) - position;
      const float ws = exp(-dot(distance, distance)
                           / (2.0 * kSpatialSigma * kSpatialSigma));
      const float4 edit = tile_edit[TileIndex(tap - origin)];
      const float difference = guide_full - edit.a;
      const float wr = exp(-(difference * difference)
                           / (2.0 * kRangeSigma * kRangeSigma));
      joint += ws * wr * edit.rgb;
      joint_weight += ws * wr;
      spatial += ws * edit.rgb;
      spatial_weight += ws;
    }
  }
  const float3 edit_up = joint_weight > 1e-6 ? joint / joint_weight
                                             : spatial / spatial_weight;
  const float3 gain = exp2(edit_up);
  const float3 neural_up = original_up >= kRatioFloor ? original_up * gain
                                                      : original_up + kRatioFloor * (gain - 1.0);
  const float3 encoded_up = SrgbEncode(neural_up);
  const float3 neural_stored = float3(StoredHalf(encoded_up.r), StoredHalf(encoded_up.g), StoredHalf(encoded_up.b));

  // ---- legacy_decode, ResolvePixel with HdrMode 0 (P unread there) ----
  const float4 source = OutputOriginal.Load(int3(pixel, 0));
  const float3 original = source.rgb;
  const float3 upgraded = neural_stored;
  const float original_y = Luminance(original);
  const float upgraded_y = Luminance(upgraded);
  const float ratio = original_y == 0.0 ? 1.0 : clamp(upgraded_y / original_y, 0.0, 4.0);
  const float3 luminance_only = original * ratio;
  const float3 result = lerp(luminance_only, upgraded, ColorStrength);
  Output[VK_OUTPUT_PIXEL(uint2(pixel))] = float4(result, source.a);
}
