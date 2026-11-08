#include "v6_common.hlsli"

// One workgroup samples a fixed 64x36 lattice from the current linear source
// and builds a log2 histogram. Black/negative/non-finite samples are ignored,
// so letterbox bars do not drag the proxy scale toward zero. The median of the
// positive scene samples is mapped to 0.30 in the model proxy. This is a
// same-command-list transform: no readback, history, warm-up, or stale scale.
//
// One-sided guardrail (v6.1.0): the scale may only ever SHRINK an over-range
// proxy, never brighten a frame just because that frame is dark - a dark
// sample and an underscaled sample are indistinguishable from one frame, so
// under-range inference is left to authoritative exposure evidence. Engage
// only when over-range is PROVEN: at least ~3% of valid lattice samples sit
// above the encode shoulder (0.75) at divisor 1, the flat-white failure class
// measured on AW2 relative-linear HDR (capture planes: p10=p50=p90=0.9995
// whenever the divisor missed). Well-placed sources (display-referred 0..1
// content) carry no shoulder mass and keep divisor 1 unchanged.
//
// Normalization governor (v6.1.2): everything above produces a CANDIDATE.
// What the codec consumes is a COMMITTED divisor that may only move
// SlewStops per frame toward it.  The split matters because the two failure
// classes this addon has shipped pull in opposite directions: v6.0's
// stateful CPU estimator could freeze on a stale garbage factor, and
// v6.1.0's stateless estimator re-derives cleanly every frame but cannot
// notice that it is oscillating.  Keeping estimation per-frame and
// stateless makes a frozen estimate unrepresentable; rate-limiting only the
// commitment makes a single-frame step unrepresentable.  A title whose own
// exposure moves the buffer statistics under us (UE5 PreExposure: the gate
// below then crosses 1/32 back and forth) is the case this exists for.
//
// The committed value lives in Commit (u1: .r committed, .g this shader's
// flags - 1 the hold bit below, 2 mode 3's pending re-prime - stored as
// their sum; .b/.a are v6_exposure_scale's and pass through, as .r/.g pass
// through it, so a guarded v2 frame keeps a pending re-prime), a 1x1 UAV that never leaves
// UNORDERED_ACCESS, is never bound as an SRV, and is read-then-written by
// exactly one thread of the single dispatched group - so the read-modify-
// write needs no barrier beyond the cross-frame UAV barrier the host
// records before this dispatch.  There is no readback and no CPU state: a
// starved fence cannot starve this the way it starved the v6.0 stats feed.
//
// Stable governor (GovernorMode 2, v6.3).  Two defects survive the v6.1.2
// slew, and both show as brightness breathing rather than stepping:
//   - the ESTIMATOR is discontinuous.  The 1/32 gate multiplies a median
//     term, so a scene whose shoulder fraction hovers at the gate (flat fog
//     under a title's own pre-exposure) flips the candidate between 1.0 and
//     median/0.30 - most of a stop - and a symmetric slew only turns each
//     flip into a ramp, i.e. sustained pumping at the slew rate.
//   - the LIMIT is per frame, so the breathing speed scales with the NR
//     frame rate (half under frame generation, double at 120 Hz).
// Mode 2 keeps the estimator stateless and per-frame but CONTINUOUS: the
// gate becomes a smoothstep weight in log2(shoulder fraction) (0 at 1/64,
// 0.5 at the v6.1 gate, 1 at 1/16) scaling the median term in stops, and
// the median is interpolated within its histogram bin.  The commitment adds
// the two properties a control loop needs to be quiet: asymmetric limits
// supplied per frame from wall-clock rates (attack = divisor rising, the
// over-range/clipping direction, fast; release = relaxing, slow), and a
// hysteresis hold - the commit moves only after the candidate has left it
// by kEngageStops and then tracks until within kSettleStops.  The hold state
// is one bit in Commit.g, so the whole loop still lives in this 1x1 UAV:
// no readback, no CPU state, nothing a starved fence can freeze.  Holding
// can leave the applied divisor up to kEngageStops from a live candidate -
// bounded, and re-evaluated from live pixels every frame, which is what
// separates it from the v6.0 lock.
//
// Cut-aware Stable (GovernorMode 3, v7.0.0-rc11, NRNormCutAware): Stable
// read a black frame as evidence.  No lattice sample above the shoulder
// gives candidate 1, so the commit released at 0.5 st/s through every
// loading screen and fade, and the scene after it came back that many stops
// under-divided: measured 4.94 st low after 10 s of black on a divisor-32
// scene, 1.2 s of over-range NR input at the 4 st/s attack.  Mode 3 holds
// the commit on a frame the meter cannot read, which is fewer than
// kReadableSamples lattice samples above the transfer's 1/1024 floor at the
// committed divisor: a frame NR itself sees as black.  A dim scene still
// reads (a night scene sits 4-7 stops under day, the floor is 8.3 under the
// committed median), so it relaxes as before and NR's night look is
// unchanged.  A snap asked for on an unreadable frame (the game's Reset on
// a cut to black, or a first frame) waits in Commit.g (flag 2) for the
// first readable one.  Modes 0-2 never take either path.
static const float kEngageStops = 0.15;
static const float kSettleStops = 0.02;
static const float kReadableFloor = 1.0 / 1024.0;
static const uint kReadableSamples = 36u;  // 1/64 of the 64x36 lattice
VK_BINDING(5) RWTexture2D<float4> Commit : register(u1);

groupshared uint Histogram[256];
groupshared uint ValidCount;
groupshared uint ShoulderCount;

uint AutoScaleBin(float value) {
  const float stops = clamp(log2(max(value, 5.96046448e-8)), -24.0, 24.0);
  return min((uint)floor((stops + 24.0) * (256.0 / 48.0)), 255u);
}

[numthreads(64, 1, 1)]
void main(uint local : SV_GroupIndex) {
  [unroll]
  for (uint bin = local; bin < 256u; bin += 64u) Histogram[bin] = 0u;
  if (local == 0u) { ValidCount = 0u; ShoulderCount = 0u; }
  GroupMemoryBarrierWithGroupSync();

  const uint2 source_size = max(SourceSize, uint2(1, 1));
  [loop]
  for (uint index = local; index < 64u * 36u; index += 64u) {
    const uint2 cell = uint2(index & 63u, index >> 6u);
    const uint2 pixel = min(
        uint2(
            ((cell.x * 2u + 1u) * source_size.x) / 128u,
            ((cell.y * 2u + 1u) * source_size.y) / 72u),
        source_size - 1u);
    const float3 rgb = Original.Load(int3(pixel, 0)).rgb;
    const float value = max(rgb.x, max(rgb.y, rgb.z));
    const uint bits = asuint(value);
    const bool finite = (bits & 0x7F800000u) != 0x7F800000u;
    if (finite && value > 0.0) {
      InterlockedAdd(Histogram[AutoScaleBin(value)], 1u);
      InterlockedAdd(ValidCount, 1u);
      if (value > 0.75) InterlockedAdd(ShoulderCount, 1u);
    }
  }
  GroupMemoryBarrierWithGroupSync();

  if (local == 0u) {
    // >=1/32 of valid samples above the encode shoulder proves the current
    // scale is destructive (the flat-white class). The median->0.30 map then
    // clamps at 1.0, so the guardrail can reduce but never brighten; a frame
    // without proven over-range keeps divisor 1 (no rescale at all).  Mode 2
    // replaces the step gate with the continuous weight described above; the
    // guardrail stays one-sided (the stops term is never negative).
    const float gate_fraction =
        ValidCount != 0u ? float(ShoulderCount) / float(ValidCount) : 0.0;
    const float gate_weight = GovernorMode >= 2u
        ? smoothstep(-6.0, -4.0, log2(max(gate_fraction, 1e-6)))
        : (ValidCount != 0u && ShoulderCount * 32u >= ValidCount ? 1.0 : 0.0);
    float candidate = 1.0;
    if (gate_weight > 0.0) {
      const uint middle = (ValidCount - 1u) >> 1u;
      uint cumulative = 0u;
      uint median_bin = 0u;
      uint below = 0u;
      [loop]
      for (uint bin = 0u; bin < 256u; ++bin) {
        const uint next = cumulative + Histogram[bin];
        if (next > middle) {
          median_bin = bin;
          below = cumulative;
          break;
        }
        cumulative = next;
      }
      // Mode 2 places the median at its rank within the bin instead of the
      // bin centre, so the estimate moves continuously with the samples
      // rather than in 0.19-stop bin steps.
      const float within = GovernorMode >= 2u
          ? (float(middle - below) + 0.5) / float(max(Histogram[median_bin], 1u))
          : 0.5;
      const float median_stops =
          -24.0 + (float(median_bin) + within) * (48.0 / 256.0);
      // Modes 0/1 keep the shipped expression bit for bit.
      candidate = GovernorMode >= 2u
          ? exp2(gate_weight * max(median_stops - log2(0.30), 0.0))
          : max(exp2(median_stops) / 0.30, 1.0);
    }

    // Commit.  A previous value of 0 is an uninitialized texel (D3D12
    // zero-initializes a committed resource created with HEAP_FLAG_NONE),
    // which snaps like any other discontinuity rather than slewing up out
    // of nothing over hundreds of frames.
    const float4 carried = Commit[uint2(0, 0)];
    const float previous = carried.r;
    const bool cut_aware = GovernorMode >= 3u;
    // .g: 1 = hold bit, 2 = pending re-prime (modes 0-2 only ever store 0/1,
    // which decode exactly as the `carried.g > 0.5` of rc10).
    const uint flags = uint(clamp(carried.g, 0.0, 3.0) + 0.5);
    const bool was_tracking = (flags & 1u) != 0u;
    // Mode 3's limits come from the real dt and are 0 for two evaluates of
    // one frame, so a zero limit there means "no step", not Off.
    const bool requested = SnapNow != 0.0 || (cut_aware && (flags & 2u) != 0u);
    const bool snap = requested || (!cut_aware && SlewStops <= 0.0)
        || !(previous > 0.0);
    bool readable = true;
    if (cut_aware) {
      const uint floor_bin = AutoScaleBin(previous * kReadableFloor);
      uint lit = 0u;
      [loop]
      for (uint bin = floor_bin + 1u; bin < 256u; ++bin) lit += Histogram[bin];
      readable = lit >= kReadableSamples;
    }
    float committed = candidate;
    float tracking = 0.0;
    float pending = 0.0;
    if (!readable) {
      // Hold, keeping the hysteresis state; an uninitialized texel takes the
      // candidate and leaves the snap pending like a requested one.
      if (previous > 0.0) {
        committed = previous;
        tracking = was_tracking ? 1.0 : 0.0;
      }
      pending = requested || !(previous > 0.0) ? 1.0 : 0.0;
    } else if (!snap) {
      const float error = log2(candidate / previous);
      if (GovernorMode >= 2u) {
        tracking =
            abs(error) > (was_tracking ? kSettleStops : kEngageStops) ? 1.0 : 0.0;
        committed = tracking > 0.0
            ? previous * exp2(clamp(error, -ReleaseStops, SlewStops))
            : previous;
      } else {
        committed = previous * exp2(clamp(error, -SlewStops, SlewStops));
      }
    }
    // .b/.a belong to v6_exposure_scale (its held divisor and range-guard
    // state); on a guarded texture feed it runs right after this dispatch.
    Commit[uint2(0, 0)] =
        float4(committed, tracking + 2.0 * pending, carried.b, carried.a);
    // .r is the only channel the encode and resolve read (t3).  .g/.b/.a
    // carry the candidate, the tracking error in stops, and the shoulder-gate
    // fraction (engaged at >= 1/32) for diagnostic captures and the
    // NRNormTrace readback - the governor keeps no CPU-visible state, so
    // those are the only places this can be observed per frame.  A held
    // frame (mode 3) reports its gate fraction negated, -1 when it had no
    // valid sample at all, so the trace can tell a hold from a quiet commit.
    Output[uint2(0, 0)] = float4(
        committed, candidate, abs(log2(candidate / max(committed, 1e-8))),
        readable ? gate_fraction : -max(gate_fraction, ValidCount != 0u ? 1e-6 : 1.0));
  }
}
