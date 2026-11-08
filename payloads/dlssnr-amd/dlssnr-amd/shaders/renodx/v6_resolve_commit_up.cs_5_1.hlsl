// DLSSNR-AMD addition: look_upsample and v6_resolve_commit in one pass, below the frame's extent (the edge-aware
// transport, a linear HDR reference, the last pass, no pedestal). look_upsample stored P_up and N'_up into two RGBA16F
// images at the frame's extent and the resolve read each pixel's own texel back; here they are made where the resolve
// needs them, through the same RGBA16F rounding (the store's: toward zero, as f32tof16), so every output bit is theirs.
// Bindings: v6_common's (0 the reference, 1 P at the network's extent, 2 N' with G_low in .a, 3 the scale texel, 4 the
// output); constants: v6_common's, ProxySize = the network's extent (look_upsample's NrSize).
#define NR_UP_FUSED 1
#include "v6_common.hlsli"

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
float WorkHalf(float value) { return f16tof32(f32tof16(value)); }

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID, uint3 group_id : SV_GroupID,
          uint group_index : SV_GroupIndex) {
  // ---- look_upsample, linear reference ----
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
  const int2 ipixel = int2(dispatch_id.xy);
  if (any(dispatch_id.xy >= Size)) return;
  const float2 position = (float2(ipixel) + 0.5) * float2(NrSize) / float2(Size) - 0.5;
  const int2 base = int2(floor(position));
  const float3 reference = Original.Load(int3(ipixel, 0)).rgb;
  const float3 original_up = reference;
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
  const float2 clamped = clamp(position, 0.0, float2(last));
  const int2 b0 = int2(floor(clamped));
  const int2 b1 = min(b0 + 1, last);
  const float2 f = clamped - float2(b0);
  const float3 proxy_up =
      tile_proxy[TileIndex(b0 - origin)] * (1.0 - f.x) * (1.0 - f.y)
      + tile_proxy[TileIndex(int2(b1.x, b0.y) - origin)] * f.x * (1.0 - f.y)
      + tile_proxy[TileIndex(int2(b0.x, b1.y) - origin)] * (1.0 - f.x) * f.y
      + tile_proxy[TileIndex(b1 - origin)] * f.x * f.y;
  const float3 gain = exp2(edit_up);
  const float3 neural_up = max(proxy_up, kRatioFloor) * gain;
  const float3 pe = SrgbEncode(proxy_up), ne = SrgbEncode(neural_up);
  g_up_proxy = float3(StoredHalf(pe.r), StoredHalf(pe.g), StoredHalf(pe.b));
  g_up_neural = float3(StoredHalf(ne.r), StoredHalf(ne.g), StoredHalf(ne.b));

  // ---- v6_resolve_commit ----
  const uint2 pixel = dispatch_id.xy;
  const float4 original = Original.Load(int3(pixel, 0));
  const float3 resolved = ResolvePixel(pixel, original.rgb);
  const float4 value = float4(WorkHalf(resolved.r), WorkHalf(resolved.g), WorkHalf(resolved.b), WorkHalf(original.a));
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
