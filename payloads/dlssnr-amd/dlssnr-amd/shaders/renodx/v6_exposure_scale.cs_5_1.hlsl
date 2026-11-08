// Feed v2 (NRFeedMode): the NR input scale from the game's own exposure
// instead of from frame content.
//
// The DLSS contract (Programming Guide 3.9 / 3.9.2): the 1x1
// ExposureTexture's first channel is the value "which when multiplied to
// the input color values brings middle gray to an expected level ...
// typically the same value provided to the renderer's tonemapper", with
// DLSS.Exposure.Scale as its correction factor; DLSS.Pre.Exposure is a
// factor the engine pre-multiplied and "later removed (divided out) during
// tonemapping".  The tonemapper's input is therefore
//   color * Exposure * ExposureScale / PreExposure
// (ExposureValue = 0.18 / (AverageLuma * 0.82): mid-grey lands near 0.2).
// Feeding NR that domain puts the game's mid-grey where the game's own
// tonemapper puts it, and it holds still while the game's eye adaptation
// swings: the proxy moves only when the displayed image does.  The divisor
// the encode applies (proxy = color / divisor) is therefore
//   texture present:  divisor = PreExposure / (Exposure * ExposureScale)
//   pre-exposed only: divisor = 1 - the buffer IS the tonemapper input
//                     already; dividing by PreExposure would undo the
//                     game's adaptation (REVIEW_SH2_FLICKER_V630 section 1)
// v1 (v6_autoscale) derives the divisor from a 64x36 content histogram
// under its own attack/release clock; that second clock, racing the game's
// adaptation, is the slow pulsing v2 exists to remove.  There is no
// estimator and no rate limit here: the game's exposure is the signal.
// The read is legal because 3.4 puts every DLSS input, the exposure texture
// included, in NON_PIXEL_SHADER_RESOURCE for the evaluate, and this dispatch
// is recorded inside that window on the same list.
//
// Source (CPU-chosen, see FrameFeed; the FeedSource values): 1 = read the
// exposure texel at t0, 2 = fixed divisor 1, 3 = hold (the exposure view
// ring is full this frame).  A texel that is non-finite or outside
// 2^-24..2^24 - an unwritten texture, a zero, a garbage convention - holds
// the previous committed divisor, or 1 when there is none (SnapNow, or an
// uninitialized texel).
// The cross-frame state lives in Commit (u1), the 1x1 UAV the v1 governor
// also uses, read-then-written by one thread: .r/.g are v1's (v6_autoscale
// owns them; this shader carries them through untouched), .b the value v2
// holds through an unreadable frame, .a the range guard's last decision.
//
// Range guard (Guard != 0: NRFeedMode Auto on a texture feed, see
// PrepareFrameScale).  The divisor above is only as right as the texel's
// convention, and the guide's contract is the only evidence there is: a
// texel that is a log2 EV, a reciprocal, or a value from another pass puts
// the whole frame stops away from mid-grey, and nothing in the formula
// notices - the texel passes the 2^-24..2^24 sanity range and NR receives a
// frame that is black or clipped.  The guard measures what the reading does
// to THIS frame: the geometric-mean luminance of the same 64x36 lattice
// v6_autoscale samples (work0 at t1, valid samples only), exposed by the
// reading, must land in [0.22 * 2^-5, 0.75].  0.22 is the guide's exposed
// mid-grey (ExposureValue = 0.18 / (AverageLuma * 0.82)), five stops under
// it is a night scene the game's own exposure left dark, and 0.75 is the
// encode's shoulder: an average above it is a frame the proxy clips.  A
// reading outside the band - with 0.5 stop of hysteresis once tripped, so a
// scene at the edge does not alternate - uses v1's committed divisor, which
// v6_autoscale wrote this frame before this dispatch (the conservative
// existing feed, rate-limited by its own governor), and reports state 5.  A
// frame with too few valid samples (under 1/16 of the lattice: a loading
// screen) keeps the last decision.  Forced NRFeedMode=2 runs unguarded.
//
// Output (u0, norm_scale) .r is the divisor the encode and resolve read
// (t3 of the pass sets) and the dark gate's unit (v6_commit_exposure);
// .g the divisor this frame's reading implied (0 = none), .b the raw
// exposure texel (-1 = non-finite), .a the state for NRNormTrace: the
// Source for a fresh value (1 texture, 2 fixed), else 3 held, 4 defaulted
// to 1, 5 guarded (v1's divisor in use).

#include "vk_binding.hlsli"
VK_BINDING(0) Texture2D<float4> Exposure : register(t0);
VK_BINDING(1) Texture2D<float4> Frame : register(t1);  // work0; read only when guarded
VK_BINDING(4) RWTexture2D<float4> Output : register(u0);
VK_BINDING(5) RWTexture2D<float4> Commit : register(u1);

// Its own view of the codec root constants (dwords 0..6); the host writes
// them after BindCodecV6, see PrepareFrameScale.
PUSH_CONSTANTS(FeedConstants) {
  uint Source;
  float PreExposure;
  float ExposureScale;
  float SnapNow;
  uint Guard;
  uint FrameWidth;
  uint FrameHeight;
};

static const float kMinDivisor = 5.96046448e-8;  // 2^-24
static const float kMaxDivisor = 16777216.0;     // 2^24
static const float kGuardLowStops = -7.1876;     // log2(0.18 / 0.82) - 5
static const float kGuardHighStops = -0.4150;    // log2(0.75)
static const float kGuardHysteresisStops = 0.5;
static const uint kLatticeSamples = 64u * 36u;

bool Finite(float value) {
  return (asuint(value) & 0x7F800000u) != 0x7F800000u;
}

groupshared float LogSum[64];
groupshared uint ValidSum[64];

[numthreads(64, 1, 1)]
void main(uint local : SV_GroupIndex) {
  float log_sum = 0.0;
  uint valid = 0u;
  if (Guard != 0u) {
    const uint2 size = max(uint2(FrameWidth, FrameHeight), uint2(1, 1));
    [loop]
    for (uint index = local; index < kLatticeSamples; index += 64u) {
      const uint2 cell = uint2(index & 63u, index >> 6u);
      const uint2 pixel = min(
          uint2(((cell.x * 2u + 1u) * size.x) / 128u, ((cell.y * 2u + 1u) * size.y) / 72u),
          size - 1u);
      const float luma =
          dot(Frame.Load(int3(pixel, 0)).rgb, float3(0.2126, 0.7152, 0.0722));
      if (Finite(luma) && luma > 0.0) {
        log_sum += clamp(log2(luma), -24.0, 24.0);
        ++valid;
      }
    }
  }
  LogSum[local] = log_sum;
  ValidSum[local] = valid;
  GroupMemoryBarrierWithGroupSync();
  if (local != 0u) return;

  float exposure = 0.0;
  float reading = Source == 2u ? 1.0 : 0.0;
  if (Source == 1u) {
    exposure = Exposure.Load(int3(0, 0, 0)).r;
    const float divisor = PreExposure / (exposure * ExposureScale);
    // Finite first: the range compares are only meaningful on a number.
    if (Finite(divisor) && divisor >= kMinDivisor && divisor <= kMaxDivisor) {
      reading = divisor;
    }
    if (!Finite(exposure)) exposure = -1.0;
  }
  const float4 carried = Commit[uint2(0, 0)];
  const float previous = carried.b;
  const bool can_hold =
      SnapNow == 0.0 && Finite(previous) && previous >= kMinDivisor
      && previous <= kMaxDivisor;
  float committed = reading;
  float state = float(Source);
  if (!(reading > 0.0)) {
    committed = can_hold ? previous : 1.0;
    state = can_hold ? 3.0 : 4.0;
  }
  const float held = committed;
  float tripped = 0.0;
  if (Guard != 0u) {
    float log_total = 0.0;
    uint valid_total = 0u;
    [unroll]
    for (uint lane = 0u; lane < 64u; ++lane) {
      log_total += LogSum[lane];
      valid_total += ValidSum[lane];
    }
    const bool was_tripped = SnapNow == 0.0 && carried.a > 0.5;
    tripped = was_tripped ? 1.0 : 0.0;
    if (reading > 0.0 && valid_total * 16u >= kLatticeSamples) {
      const float exposed = log_total / float(valid_total) - log2(reading);
      const float margin = was_tripped ? kGuardHysteresisStops : 0.0;
      tripped = exposed >= kGuardLowStops + margin && exposed <= kGuardHighStops - margin
          ? 0.0 : 1.0;
    }
    // Nothing of v2's own to hold (no reading has ever been accepted) is
    // not a reason to feed a guessed 1: v1 is the measured fallback.
    if (state == 4.0) tripped = 1.0;
    if (tripped > 0.0) {
      committed = carried.r;
      state = 5.0;
    }
  }
  Commit[uint2(0, 0)] = float4(carried.r, carried.g, held, tripped);
  Output[uint2(0, 0)] = float4(committed, reading, exposure, state);
}
