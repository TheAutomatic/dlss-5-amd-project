#include "legacy_common.hlsli"

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;
  const uint2 source_size = max(SourceSize, uint2(1, 1));
  float3 proxy;
  [branch]
  if (Padding.x > 0.0) {
    // Area footprint: Padding.x (dword 16) is set only by the explicit
    // Edge-aware (area input) mode.  v8.0.2 enabled this for every transport,
    // silently changing the model's SDR input under a saved Edge-aware=1.
    // The v7 point input remains the fallback and Edge-aware=1's input.
    // Area mode averages in linear light, then encodes back into the domain
    // the branches below expect.  The transport itself is unchanged.
    const float2 scale = float2(source_size) / float2(Size);
    const float2 center = (float2(pixel) + 0.5) * scale;
    const float2 footprint_min = max(center - 0.5 * scale, float2(0.0, 0.0));
    const float2 footprint_max = min(center + 0.5 * scale, float2(source_size));
    const int2 first = int2(floor(footprint_min));
    const int2 last = int2(ceil(footprint_max));
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    for (int y = first.y; y < last.y; ++y) {
      for (int x = first.x; x < last.x; ++x) {
        const float2 overlap = min(float2(x + 1, y + 1), footprint_max)
            - max(float2(x, y), footprint_min);
        const float w = max(overlap.x, 0.0) * max(overlap.y, 0.0);
        const float3 texel = Original.Load(int3(int2(SourceBase) + int2(x, y), 0)).rgb;
        // v6_encode's non-finite guard (rc11, DATA-09): scRGB can hold them.
        const float3 finite = max((asuint(texel) & 0x7F800000u) != 0x7F800000u
                                      ? texel
                                      : (texel > 0.0 ? 65504.0 : 0.0),
                                  0.0);
        sum += (HdrMode == 1 ? finite
                : HdrMode == 2 ? PQToLinear(finite)
                               : SrgbDecode(finite)) * w;
        weight += w;
      }
    }
    const float3 mean = weight > 0.0 ? sum / weight : float3(0.0, 0.0, 0.0);
    proxy = HdrMode == 1 ? mean : HdrMode == 2 ? LinearToPQ(mean) : SrgbEncode(mean);
  } else {
    const uint2 source_pixel = SourceBase + min(
        uint2(((float2(pixel) + 0.5) * float2(source_size)) / float2(Size)),
        source_size - 1);
    proxy = max(Original.Load(int3(source_pixel, 0)).rgb, 0.0);
  }
  if (HdrMode == 1) {
    // Scene-linear HDR (scRGB / linear FP16): normalize to paper white, soft-clip,
    // then encode to sRGB so DLSSNR sees a display-referred proxy.
    // Default PaperWhiteScale = 2.5375 = 203/80: scRGB is 1.0 = 80 nits and the
    // model expects paper white (203 nits) at 1.0, so raw scRGB at scale 1.0
    // leaves the proxy overranged (shoulder starting at only 60 nits).
    proxy /= PaperWhiteScale;
    // Hue-preserving shoulder: compress uniformly from the max channel instead
    // of per channel. Per-channel clipping skewed bright colored content
    // toward primaries ("oddly saturated").
    float m = max(proxy.x, max(proxy.y, proxy.z));
    [branch]
    if (m > 0.75) {
      proxy *= (0.75 + 0.25 * (1.0 - exp(-5.770780 * (m - 0.75)))) / max(m, 1e-6);
    }
    proxy = SrgbEncode(proxy);
  } else if (HdrMode == 2) {
    // PQ/display-encoded HDR (R10G10B10A2): linearize PQ, normalize to paper
    // white, soft-clip, then sRGB.  PQToLinear is absolute (1.0 = 10000 nits),
    // so without the normalization every ordinary tone (<=~300 nits) lands in
    // the bottom ~3% of the proxy; with AutoExposure forced, the model then
    // misreads the scene as near-black and lifts/smears shadow noise.
    // DiffuseWhiteNits (default 203) = BT.2408 reference white over the
    // 10000-nits PQ range - the bridge anchor (NRDiffuseWhiteNits);
    // PaperWhiteScale is the user's live calibration multiplier on top.
    proxy = PQToLinear(proxy) / ((DiffuseWhiteNits / 10000.0) * PaperWhiteScale);
    float m = max(proxy.x, max(proxy.y, proxy.z));
    [branch]
    if (m > 0.75) {
      proxy *= (0.75 + 0.25 * (1.0 - exp(-5.770780 * (m - 0.75)))) / max(m, 1e-6);
    }
    proxy = SrgbEncode(proxy);
  }
  // HdrMode == 0: SDR, already display-referred; pass through unchanged.
  // The DLSSNR color input must be opaque: a 0 alpha (common for R10G10B10A2
  // HDR where the game writes alpha 0) makes the runtime emit an empty/black
  // neural output.  The real output alpha is restored from OutputOriginal in
  // the decode stage, so forcing 1.0 here is safe for every HDR mode.
  Output[pixel] = float4(proxy, 1.0);
}
