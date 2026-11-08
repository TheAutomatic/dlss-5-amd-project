#include "look_common.hlsli"

// The look compose pass, one thread per NR pixel: the network's edit, its
// bands (after look_band), its temporal filter (history per NR handle),
// then every gain, written as N' = n * 2^(t * (e' - e)) into LookOutput.
// With the transport flag the alpha channel carries G_low, the log2
// luminance of the reference over this pixel's footprint, for look_upsample.

// (a_B, a_D, c.r, c.b) of the current frame at `pixel`, plus the mid band
// `aM` (v8: the scales between the narrow and the wide transport) and both
// witnesses (`peak`, `trough`).  Without the bands (Detail stability's
// temporal filter, rc11) a_B is the whole edit and a_D and a_M are 0, so the
// filter and its 3x3 clip act on the edit NR made.
float4 BandValues(int2 pixel, out Edit x, out float peak, out float trough,
                  out float aM) {
  x = ReadEdit(pixel);
  peak = x.I;
  trough = x.I;
  float aB = x.a;
  float aD = 0.0;
  aM = 0.0;
  if ((Flags & kFlagBands) != 0u) {
    const BandSplit bands = BandCoefficients(pixel);
    aB = bands.narrow.x * x.I + bands.narrow.y;
    aD = x.a - aB;
    aM = aB - (bands.wide.x * x.I + bands.wide.y);
    peak = bands.peak;
    trough = bands.trough;
  }
  return float4(aB, aD, x.c.r, x.c.b);
}

float4 LoadHistory(uint2 pixel, out float history_i, out float history_m) {
#if defined(RENODX_VULKAN) && defined(DLSSNR_HISTORY_SPLIT)
  const uint3 words = uint3(HistoryIn[pixel], HistoryInZ[pixel]);
#elif defined(RENODX_VULKAN)
  const uint3 words = HistoryIn[pixel].xyz;
#else
  const uint3 words = HistoryIn.Load3(HistoryAddress(pixel));
#endif
  history_i = f16tof32(words.z);
  history_m = f16tof32(words.z >> 16);
  return float4(f16tof32(words.x), f16tof32(words.x >> 16),
                f16tof32(words.y), f16tof32(words.y >> 16));
}

// The 3x3 clip's neighbour values, computed once per pixel for the group's 16x16 tile and its 1-pixel border
// (DLSSNR-AMD addition: the same BandValues of the same clamped pixels the loop below read, which it used to
// recompute for each of the 9 windows a pixel is in). DLSSNR-AMD: a thread's own entry is its own
// pixel's values (computed once, for the tile and for the pixel: the same BandValues of the same clamped pixel), the
// 68 border entries by the first 68 threads.
groupshared float4 g_value[18][18];
groupshared float g_m[18][18];
groupshared float g_i[18][18];

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID, uint3 group_thread : SV_GroupThreadID, uint3 group : SV_GroupID) {
  const int2 pixel = int2(dispatch_id.xy);
  const bool clip = (Flags & kFlagTemporal) != 0u && (Flags & kFlagHistoryValid) != 0u;
  Edit x;
  float peak;
  float trough;
  float aM;
  float4 current = BandValues(clamp(pixel, 0, int2(NrSize) - 1), x, peak, trough, aM);
  if (clip) {   // uniform: every thread of the group gets here
    g_value[group_thread.y + 1u][group_thread.x + 1u] = current;
    g_m[group_thread.y + 1u][group_thread.x + 1u] = aM;
    g_i[group_thread.y + 1u][group_thread.x + 1u] = x.I;
    const uint k = group_thread.y * 16u + group_thread.x;
    if (k < 68u) {   // the ring: rows 0 and 17 (18 each), then columns 0 and 17 of rows 1..16
      const int2 at = k < 18u ? int2(k, 0) : k < 36u ? int2(k - 18u, 17) : k < 52u ? int2(0, k - 35u) : int2(17, k - 51u);
      const int2 origin = int2(group.xy) * 16 - 1;
      const int2 neighbour = clamp(origin + at, 0, int2(NrSize) - 1);
      Edit around;
      float unused_peak;
      float unused_trough;
      float around_m;
      g_value[at.y][at.x] = BandValues(neighbour, around, unused_peak, unused_trough, around_m);
      g_m[at.y][at.x] = around_m;
      g_i[at.y][at.x] = around.I;
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (any(dispatch_id.xy >= NrSize)) return;
  float3 c = x.c;

  if ((Flags & kFlagTemporal) != 0u) {
    float4 filtered = current;
    float filtered_m = aM;
    if ((Flags & kFlagHistoryValid) != 0u) {
      // DLSS motion vectors point to the PREVIOUS position: previous =
      // current + MV (DLSS Programming Guide, section 3.6).
      float2 previous = float2(pixel);
      if ((Flags & kFlagMotion) != 0u) {
        const uint2 texel = MotionBase
            + min(uint2((float2(pixel) + 0.5) * float2(MotionGrid) / float2(NrSize)),
                  MotionGrid - 1u);
        previous += Motion.Load(int3(texel, 0)).xy * MotionScale;
      }
      const float2 limit = float2(NrSize) - 0.5;
      const bool onscreen = all(previous >= -0.5) && all(previous <= limit);
      const float2 pos = clamp(previous, 0.0, float2(NrSize - 1u));
      const uint2 h0 = uint2(floor(pos));
      const uint2 h1 = min(h0 + 1u, NrSize - 1u);
      const float2 f = pos - float2(h0);
      float i00, i10, i01, i11;
      float m00, m10, m01, m11;
      const float4 q00 = LoadHistory(h0, i00, m00);
      const float4 q10 = LoadHistory(uint2(h1.x, h0.y), i10, m10);
      const float4 q01 = LoadHistory(uint2(h0.x, h1.y), i01, m01);
      const float4 q11 = LoadHistory(h1, i11, m11);
      const float4 history = q00 * (1.0 - f.x) * (1.0 - f.y) + q10 * f.x * (1.0 - f.y)
          + q01 * (1.0 - f.x) * f.y + q11 * f.x * f.y;
      const float history_i = i00 * (1.0 - f.x) * (1.0 - f.y) + i10 * f.x * (1.0 - f.y)
          + i01 * (1.0 - f.x) * f.y + i11 * f.x * f.y;
      const float history_m = m00 * (1.0 - f.x) * (1.0 - f.y) + m10 * f.x * (1.0 - f.y)
          + m01 * (1.0 - f.x) * f.y + m11 * f.x * f.y;
      // Neighbourhood clipping: history may not leave the 3x3 range of the
      // current frame's values.
      float4 lo = current;
      float4 hi = current;
      float lo_m = aM;
      float hi_m = aM;
      float i_lo = x.I;
      float i_hi = x.I;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) continue;
          // the neighbour clamp(pixel + (dx, dy)): its tile entry (the tile entries hold clamped pixels too)
          const int2 at = int2(group_thread.xy) + 1 + int2(dx, dy);
          const float4 value = g_value[at.y][at.x];
          const float around_m = g_m[at.y][at.x];
          const float around_i = g_i[at.y][at.x];
          lo = min(lo, value);
          hi = max(hi, value);
          lo_m = min(lo_m, around_m);
          hi_m = max(hi_m, around_m);
          i_lo = min(i_lo, around_i);
          i_hi = max(i_hi, around_i);
        }
      }
      // The history is rejected where the input changed under it.  The
      // player's Stabilize compares the pixel's own input; Detail stability
      // asks whether the reprojected input lies inside the 3x3 range of the
      // current one.  Under sub-pixel motion the bilinear history of a
      // high-contrast pixel (a grass blade's edge) always differs from the
      // pixel by more than 0.3 stop, so the point test rejected it on every
      // frame and the edit boiled on at half NR resolution; a disocclusion
      // still lands outside the range and is rejected.
      const float input_change = (Flags & kFlagRangeConfidence) != 0u
          ? max(max(i_lo - history_i, history_i - i_hi), 0.0)
          : abs(x.I - history_i);
      const float confidence = onscreen ? 1.0 - smoothstep(0.1, 0.3, input_change) : 0.0;
      const float beta = max(Alpha, 1.0 - confidence);
      const float4 clipped = clamp(history, lo, hi);
      filtered = clipped + beta * (current - clipped);
      const float clipped_m = clamp(history_m, lo_m, hi_m);
      filtered_m = clipped_m + beta * (aM - clipped_m);
      if ((Flags & kFlagTemporalDetail) == 0u) {
        filtered.y = current.y;
        filtered_m = aM;
      }
    }
    // The mid band rides the SPARE HIGH HALF of the input word, so the
    // history stays 12 bytes per pixel (v8; the halves before it carried
    // nothing).  Not modeled by look_reference.py - no fixture combines the
    // temporal filter with HaloBright, and at 0 the half is never read.
    const uint3 words = uint3(
        f32tof16(filtered.x) | (f32tof16(filtered.y) << 16),
        f32tof16(filtered.z) | (f32tof16(filtered.w) << 16),
        f32tof16(x.I) | (f32tof16(filtered_m) << 16));
#if defined(RENODX_VULKAN) && defined(DLSSNR_HISTORY_SPLIT)
    HistoryOut[uint2(pixel)] = words.xy;
    HistoryOutZ[uint2(pixel)] = words.z;
#elif defined(RENODX_VULKAN)
    HistoryOut[uint2(pixel)] = uint4(words, 0u);
#else
    HistoryOut.Store3(HistoryAddress(uint2(pixel)), words);
#endif
    current = filtered;
    aM = filtered_m;
    c = float3(current.z, -(kW.x * current.z + kW.z * current.w) / kW.y, current.w);
  }

  const float3 shaped = ShapeEdit(x, current.x, current.y, aM, c, peak, trough);
  float guide = 1.0;
  if ((Flags & kFlagTransport) != 0u) {
    // v6_encode's area footprint over the reference.
    const float2 scale = float2(FullSize) / float2(NrSize);
    const float2 footprint_min = float2(pixel) * scale;
    const float2 footprint_max = min(footprint_min + scale, float2(FullSize));
    const int2 first = int2(floor(footprint_min));
    const int2 last = int2(ceil(footprint_max));
    float3 sum = 0.0;
    float weight = 0.0;
    for (int y = first.y; y < last.y; ++y) {
      for (int xx = first.x; xx < last.x; ++xx) {
        const float2 overlap = min(float2(xx + 1, y + 1), footprint_max)
            - max(float2(xx, y), footprint_min);
        const float w = max(overlap.x, 0.0) * max(overlap.y, 0.0);
        const float3 texel = Reference.Load(int3(xx, y, 0)).rgb;
        sum += max((Flags & kFlagRefLinear) != 0u ? texel : SrgbDecode(texel), 0.0) * w;
        weight += w;
      }
    }
    guide = log2(max(Luminance(sum / max(weight, 1e-12)), kGuideFloor));
  }
  LookOutput[pixel] = float4(SrgbEncode(x.n * exp2(x.t * (shaped - x.e))), guide);
}
