#include "look_common.hlsli"

// Group D's band split: the fast guided filter (He & Sun, "Fast Guided
// Filter", 2015) of the achromatic edit `a`, guided by the input's log
// luminance I, with coefficients at half NR resolution.  Five passes over the
// half-resolution grid (Size), chosen by Mode, each separated by a UAV
// barrier on the host:
//   0 down    BandA = 2x2 means of (I, a, I*I, I*a); BandMaxA = 2x2 extrema
//             of I (.r max, .g the NEGATED min - dilating a negated min
//             makes the round penalty add to it, the min chain's shape)
//   1 H       BandB = horizontal box of BandA; BandMaxB = horizontal
//             dilation of both channels
//   2 V+coef  box/dilation vertically, then A = cov(I, a) / (var(I) + GfEps)
//             and B = mean(a) - A * mean(I) into BandA.xy; BandMaxA = the
//             dilated input at both polarities, the halo neighbourhoods
//             look_compose reads
//   3 H       BandB.xy = horizontal box of (A, B) at Radius (narrow);
//             BandB.zw = the same at MidRadius (wide)
//   4 V       BandA.xy = vertical box of BandB.zw at MidRadius (wide);
//             BandA.zw = vertical box of BandB.xy at Radius (narrow)
// So the (A, B) coefficients computed once from the Radius-boxed moments
// transport at TWO scales: narrow (today's a_B) in BandA.zw, wide (v8's mid
// band) in BandA.xy.  Box windows are clamped at the border and normalised
// by the in-bounds count, so a constant field stays constant.
// look_reference.py (bands_half) mirrors the order of every sum.
//
// The dilation is round and falls off with distance: the max over the
// window of I - kHaloPenalty * (d / dilate_radius)^2, d in half-res pixels,
// over 2 * dilate_radius (where the penalty reaches 8 stops) - elementwise
// on the float2 channels, cs_5_1-safe.  The two separable passes together
// subtract the penalty of the Euclidean distance.  The max chain (.r)
// dilates at Radius; the negated-min chain (.g) at MinRadius, where the
// penalty ADDS to the min (it dilates -I) - the mode handlers pick the
// channel each call feeds.  Until rc6 it was a flat max over a square of
// Radius: the halo weight stayed full to the square's edge and then
// dropped, which drew the square.
static const float kHaloPenalty = 2.0;  // stops at the band radius

float4 BoxMoments(int2 pixel, bool horizontal, uint box_radius) {
  const int2 last = int2(Size) - 1;
  const int radius = int(box_radius);
  const int centre = horizontal ? pixel.x : pixel.y;
  const int lo = max(centre - radius, 0);
  const int hi = min(centre + radius, horizontal ? last.x : last.y);
  float4 sum = 0.0;
  for (int i = lo; i <= hi; ++i) {
    sum += horizontal ? BandA[int2(i, pixel.y)] : BandB[int2(pixel.x, i)];
  }
  return sum / float(hi - lo + 1);
}

float2 Dilate(int2 pixel, bool horizontal, uint dilate_radius) {
  const int2 last = int2(Size) - 1;
  const int radius = int(dilate_radius);
  const float k = kHaloPenalty / float(radius * radius);
  const int centre = horizontal ? pixel.x : pixel.y;
  const int lo = max(centre - 2 * radius, 0);
  const int hi = min(centre + 2 * radius, horizontal ? last.x : last.y);
  float2 peak = -1e30;
  for (int i = lo; i <= hi; ++i) {
    const float d = float(i - centre);
    peak = max(peak, (horizontal ? BandMaxA[int2(i, pixel.y)] : BandMaxB[int2(pixel.x, i)])
                         - k * d * d);
  }
  return peak;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const int2 pixel = int2(dispatch_id.xy);
  if (any(dispatch_id.xy >= Size)) return;
  if (Mode == 0u) {
    float4 moments = 0.0;
    float peak = -1e30;
    float trough = 1e30;
    float count = 0.0;
    for (int dy = 0; dy <= 1; ++dy) {
      for (int dx = 0; dx <= 1; ++dx) {
        const int2 source = pixel * 2 + int2(dx, dy);
        if (any(source >= int2(NrSize))) continue;
        const Edit x = ReadEdit(source);
        moments += float4(x.I, x.a, x.I * x.I, x.I * x.a);
        peak = max(peak, x.I);
        trough = min(trough, x.I);
        count += 1.0;
      }
    }
    BandA[pixel] = moments / count;
    BandMaxA[pixel] = float2(peak, -trough);
  } else if (Mode == 1u) {
    BandB[pixel] = BoxMoments(pixel, true, Radius);
    const float2 dilated = Dilate(pixel, true, Radius);
    const float2 dilated_min = Dilate(pixel, true, MinRadius);
    BandMaxB[pixel] = float2(dilated.x, dilated_min.y);
  } else if (Mode == 2u) {
    const float4 m = BoxMoments(pixel, false, Radius);
    const float variance = max(m.z - m.x * m.x, 0.0);
    const float covariance = m.w - m.x * m.y;
    const float slope = covariance / (variance + GfEps);
    BandA[pixel] = float4(slope, m.y - slope * m.x, 0.0, 0.0);
    const float2 dilated = Dilate(pixel, false, Radius);
    const float2 dilated_min = Dilate(pixel, false, MinRadius);
    BandMaxA[pixel] = float2(dilated.x, dilated_min.y);
  } else if (Mode == 3u) {
    BandB[pixel] = float4(BoxMoments(pixel, true, Radius).xy,
                          BoxMoments(pixel, true, MidRadius).xy);
  } else {
    BandA[pixel] = float4(BoxMoments(pixel, false, MidRadius).zw,
                          BoxMoments(pixel, false, Radius).xy);
  }
}
