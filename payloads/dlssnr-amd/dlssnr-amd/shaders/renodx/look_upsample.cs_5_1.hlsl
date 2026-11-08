#include "look_common.hlsli"

// Group F, edge-aware transport (fixes F-L1): at reduced NR resolution the
// look stage hands the resolve output-resolution P_up and N'_up, so every
// resolve takes its existing 1:1 Load path with no resolve change.
//
// Bound with the pass's LOOK set: Reference = the pass reference at full
// resolution, Proxy = P, Neural = N' (alpha = G_low from look_compose).
// Per output pixel the effective edit e = log2(n' / p) of the 4x4 NR pixels
// around it is averaged with joint-bilateral weights (Kopf et al. 2007):
// spatial on the distance in NR pixels, range on the difference between this
// pixel's log2 luminance G_full and the NR pixel's footprint mean G_low.  The
// weights are normalised, so a constant edit transports exactly.  Where no
// neighbour matches (sum of weights ~ 0) the spatial weights alone decide.
// The window is symmetric about the sample (floor - 1 .. floor + 2) and the
// spatial sigma is half an NR pixel.  Until rc6 it was the 3x3 around the
// nearest NR pixel with sigma 1: wider than the footprint and lopsided, it
// staircased a smooth edit ramp (+/-0.011 stop at 0.1 stop per NR pixel,
// now +/-0.002) and measured 0.190 stop RMSE against a 4K edit, now 0.172
// (Classic 0.248; Alan Wake 2 capture, 2x, tools/look/look_reference.py).
//   HDR (linear reference): P_up = the resolve's own bilinear P, and
//     N'_up = max(P_up, floor) * 2^e_up, so the resolve's ratio is e_up.
//   SDR (sRGB reference): N'_up = the full-resolution original times 2^e_up,
//     which keeps the original's detail where the Classic path showed
//     nearest-neighbour NR output.

static const float kSpatialSigma = 0.5;  // NR pixels
static const float kRangeSigma = 0.5;    // stops

// The group's NR tile (v8.0.2, when Auto made this the default): the edit
// and the decoded P of every NR pixel its 16x16 output pixels read, each
// computed once - at 2x every NR pixel was decoded for 16 taps (0.65 ms of
// the 0.72 ms look stage at 3840x2160).  A pixel's window is base - 1 ..
// base + 2, and base moves at most 16 NR pixels across the group while NR's
// grid is not larger than the output (look::TransportWanted), so a 20x20
// tile from the group's first base - 1 holds every tap and the bilinear P.
// Every value is the per-tap arithmetic unchanged; the index is clamped so a
// grid outside that contract reads wrong taps, never outside the tile.
static const int kTile = 20;
groupshared float4 tile_edit[kTile * kTile];   // e per channel, .a = G_low
groupshared float3 tile_proxy[kTile * kTile];  // P, sRGB-decoded

uint TileIndex(int2 local) {
  const int2 at = clamp(local, 0, kTile - 1);
  return uint(at.y * kTile + at.x);
}

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID, uint3 group_id : SV_GroupID,
          uint group_index : SV_GroupIndex) {
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
  const bool linear_reference = (Flags & kFlagRefLinear) != 0u;
  const float3 reference = Reference.Load(int3(pixel, 0)).rgb;
  const float3 original = linear_reference ? reference : SrgbDecode(reference);
  const float guide_full = log2(max(Luminance(max(original, 0.0)), kGuideFloor));

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

  // The resolve's own bilinear reconstruction of P at this pixel.
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
  const float3 neural_up = linear_reference
      ? max(proxy_up, kRatioFloor) * gain
      : (original >= kRatioFloor ? original * gain
                                 : original + kRatioFloor * (gain - 1.0));
  UpProxy[pixel] = float4(SrgbEncode(proxy_up), 1.0);
  UpNeural[pixel] = float4(SrgbEncode(neural_up), 1.0);
}
