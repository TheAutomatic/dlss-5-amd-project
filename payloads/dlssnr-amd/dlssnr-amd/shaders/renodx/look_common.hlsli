// NR look stage (PLAN_NR_LOOK_V71.md): the bindings of the look root
// signature (look_stage.hpp) and the edit-field math every look program
// shares.  tools/look/look_reference.py mirrors each function in float32 and
// is the oracle the EditRig fixtures compare against; change them together.
//
// DLSSNR is a Neural Rendering enhancer.  Nothing here denoises: the stage
// reshapes the network's EDIT e = log2(n / p) per channel and writes
// N' = n * 2^(t * (e' - e)), so e' == e returns the network's own value.

#include "codec_math.hlsli"
// Vulkan (vulkan_path.hpp, vk_look_*.comp.slang): set 0 bindings 0..4 for
// t0..t4, 5..13 for u0..u8, 14/15 for the history pair; the cbuffer becomes
// the push constants (std430 puts every member at its cbuffer offset).
#include "vk_binding.hlsli"

// Root parameter 0: a codec per-pass SRV set (kDescriptor* in dlssnr.hpp),
// or the pass's look set, whose Neural slot is N' instead of N.
VK_BINDING(0) Texture2D<float4> Reference : register(t0);  // the pass's reference image
VK_BINDING(1) Texture2D<float4> Proxy : register(t1);      // P, the NR input (sRGB-encoded)
VK_BINDING(2) Texture2D<float4> Neural : register(t2);     // N, or N' in the look set
// Root parameter 5: one view of the workset's motion-vector ring.
VK_BINDING(4) Texture2D<float2> Motion : register(t4);
// Root parameter 1: the workset's look UAV region, all resting in
// UNORDERED_ACCESS.
VK_BINDING(5) RWTexture2D<float4> LookOutput : register(u0);  // N' (NR res; .a = G_low)
VK_BINDING(6) RWTexture2D<float4> BandA : register(u1);       // half NR res
VK_BINDING(7) RWTexture2D<float4> BandB : register(u2);
VK_BINDING(8) RWTexture2D<float2> BandMaxA : register(u3);    // half NR res: dilated max,
VK_BINDING(9) RWTexture2D<float2> BandMaxB : register(u4);    // negated dilated min
VK_BINDING(10) RWTexture2D<float4> UpProxy : register(u5);     // full res (transport)
VK_BINDING(11) RWTexture2D<float4> UpNeural : register(u6);
VK_BINDING(12) RWByteAddressBuffer TraceHistogram : register(u7);
VK_BINDING(13) RWTexture2D<float4> TraceBlocks : register(u8);
// Root parameters 4 and 3: the per-NR-handle history ping-pong.  Both halves
// live in UNORDERED_ACCESS for their whole life (like the governor's
// norm_commit), so the host orders frames with UAV barriers only.
#define DLSSNR_HISTORY_SPLIT 1
#ifdef RENODX_VULKAN
// Vulkan keeps each history texel in an RGBA32_UINT storage image (the three
// words and a spare): ReShade creates every storage BUFFER with
// acceleration-structure and device-address usage, extensions a game's
// device need not enable (VUID-VkBufferCreateInfo-None-09499).
#ifdef DLSSNR_HISTORY_SPLIT
// DLSSNR-AMD: the three words in an RG32_UINT and an R32_UINT image (12 bytes a texel, no spare word).
VK_BINDING(14) RWTexture2D<uint2> HistoryOut;
VK_BINDING(15) RWTexture2D<uint2> HistoryIn;
VK_BINDING(16) RWTexture2D<uint> HistoryOutZ;
VK_BINDING(17) RWTexture2D<uint> HistoryInZ;
#else
VK_BINDING(14) RWTexture2D<uint4> HistoryOut;
VK_BINDING(15) RWTexture2D<uint4> HistoryIn;
#endif
#else
VK_BINDING(14) RWByteAddressBuffer HistoryOut : register(u9);
VK_BINDING(15) RWByteAddressBuffer HistoryIn : register(u10);
#endif

// 35 root constants; LookConstants in look_stage.hpp mirrors it.
PUSH_CONSTANTS(LookConstants) {
  uint2 Size;       // this dispatch's grid
  uint2 NrSize;     // the NR working resolution (P, N, N', history)
  uint2 FullSize;   // the pass's reference / resolve resolution
  uint Flags;       // kFlag* below
  uint Mode;        // program-specific (band pass, trace step)
  float Strength;   // A: stops multiplier of the whole edit
  float Brighten;   // A: gain on a > 0
  float Darken;     // A: gain on a < 0
  float MaxBrighten;  // A: soft knee in stops, 0 = off
  float MaxDarken;
  float ColourGain;   // B: radial (toward/away from grey)
  float HueGain;      // B: tangential (tint)
  float MaxColour;    // B: soft knee on the w-norm, 0 = off
  float Shadows;      // C
  float Midtones;
  float Highlights;
  float Tone;         // D: large-scale band
  float Detail;       // D: fine band
  float Halo;         // D: darkening suppression near brighter pixels
  uint Radius;        // D: box radius, half-res pixels
  float Alpha;        // E: 1 - exp(-dt / tau)
  float2 MotionScale; // E: MV texel -> NR pixels (previous = current + MV)
  uint2 MotionBase;   // E: MV subrect origin
  uint2 MotionGrid;   // E: the pixel grid the MV texels describe
  float GfEps;        // D: guided-filter regularization, stops^2
  float HaloBright;   // D: bright-side suppression, 0 = off (v8)
  uint MinRadius;     // D: dilated-min witness radius, half-res pixels
  uint MidRadius;     // D: wide transport radius, half-res pixels
  uint Padding;
};

static const uint kFlagBands = 1u << 0;
static const uint kFlagTemporal = 1u << 1;
static const uint kFlagMotion = 1u << 2;
static const uint kFlagTemporalDetail = 1u << 3;
static const uint kFlagTransport = 1u << 4;
static const uint kFlagRefLinear = 1u << 5;
static const uint kFlagHistoryValid = 1u << 6;
static const uint kFlagRangeConfidence = 1u << 7;

static const float kRatioFloor = 1.0 / 1024.0;  // v6_resolve's N/P floor
static const float3 kW = float3(0.212639, 0.715169, 0.072192);
static const float kLog2Grey = -2.4739312;      // log2(0.18)
static const float kGuideFloor = 1e-8;
// The mid-band lift gain of the bright-side twin (v8), fixed like the
// defaults' kDetailStabilityAlpha.  1.0 is the measured ship point
// (81/69/52 % of a 4/8/16-px skirt lifted at the ship radii); 0.5 is the
// documented conservative value (56/43/29 %, the M leak halved to
// 0.033 stops).
static const float kMidGain = 1.0;

// The network's edit at one NR pixel (section 2 of the plan).
struct Edit {
  float3 n;   // linear network output
  float3 lp;  // log2 of the linear input, floored
  float3 e;   // per-channel edit, stops
  float a;    // achromatic edit: w-weighted mean of e
  float3 c;   // opponent edit, dot(w, c) = 0
  float I;    // log2 relative luminance of the input
  float t;    // floor trust
};

Edit ReadEdit(int2 pixel) {
  Edit x;
  const float3 p = SrgbDecode(Proxy.Load(int3(pixel, 0)).rgb);
  x.n = SrgbDecode(Neural.Load(int3(pixel, 0)).rgb);
  x.lp = log2(max(p, kRatioFloor));
  x.e = log2(max(x.n, kRatioFloor)) - x.lp;
  x.a = dot(kW, x.e);
  x.c = x.e - x.a;
  x.I = log2(max(dot(kW, p), kRatioFloor));
  x.t = ProxyFloorTrust(p);
  return x;
}

// Radial and tangential parts of the colour edit in the input's own
// log-opponent direction o; s fades the split out near grey.
void ColourSplit(float3 c, float3 lp, out float3 c_r, out float3 c_t, out float s) {
  const float3 o = lp - dot(kW, lp);
  const float oo = dot(kW, o * o);
  c_r = (dot(kW, c * o) / max(oo, 1e-6)) * o;
  c_t = c - c_r;
  s = smoothstep(0.05, 0.25, sqrt(oo));
}

float ZoneGain(float I) {
  const float L = I - kLog2Grey;
  const float w_sh = 1.0 - smoothstep(-3.5, -1.5, L);
  const float w_hi = smoothstep(0.5, 2.5, L);
  return w_sh * Shadows + (1.0 - w_sh - w_hi) * Midtones + w_hi * Highlights;
}

// Every gain acts on stops; the operators are C0 and monotonic.  The
// direction split is piecewise linear: the plan's smoothstep blend
// a * lerp(Darken, Brighten, s(a)) is not monotonic when the gains differ
// (look_reference.py --selftest measures the bump).
//
// Halo lifts only the darkening DETAIL a_D next to brighter pixels (plan
// section 3 D).  Until rc6 it lifted the whole darkening there: a surface NR
// darkened evenly came back lighter inside the neighbourhood of every bright
// feature, a lighter box around the letters on a dark jacket (Alan Wake 2).
// NR's measured overshoot there is 2-4 NR px and 0.1 stop deep, well inside
// the fine band; wider halos need a larger Detail radius.
//
// HaloBright (v8, the twin, PLAN_HALO_BRIGHTSIDE variant C1) is the other
// polarity: the field artifact is a soft dark SKIRT on the bright side of
// dark objects (0.3-0.55 stops, 5-15+ px, gradual), where m - I = 0 and
// rc6's guard never opens - measured 0.0 % lift at every width.  The
// polarity table, from the field screenshots: bright side of a dark object
// m - I = 0.00 stops (max guard closed forever), I - mn = +2.58 stops (min
// guard wide open); dark side of a bright object, the reverse.  So:
//   - the fine band lifts under EITHER witness: wD = max(ss(m-I), ss(I-mn));
//   - the MID band (a_M = a_Bn - a_Bw, the scales between Radius and
//     MidRadius) lifts under the min witness alone - wide skirts live there
//     (band energy of a 8-px skirt: fine/mid/broad = 3/35/62 %; of a 16-px
//     one, 1/15/84 %).
// Each product clamps at 1: h * w > 1 would sign-flip darkening into
// brightening (the reason per-pass Halo scaling was rejected).  A darkening
// the scene MEANS is even, lands wholly in the broad band (a_M = a_D = 0)
// and is untouched - the jacket scene reads -0.200 stops exactly; its
// mirror beside a DARK silhouette leaks a smooth -0.155..-0.134 ramp over
// ~24 px (0.062 stops, 0.033 at half gain), the accepted leak.  rc6's line
// above is unchanged, so the dark-side case keeps its 31 % lift; both knobs
// together lift the fine band twice in sequence.
// The mid lift rides Detail like the fine one: Detail 0 mutes every band
// but Tone's.  Tone still scales the narrow transport alone (a_M enters
// only through its lift), which is what keeps HaloBright 0 bit-identical at
// any Tone/Detail.
float3 ShapeEdit(Edit x, float aB, float aD, float aM, float3 c, float m, float mn) {
  const float wmax = smoothstep(1.0, 3.0, m - x.I);
  aD -= Halo * wmax * min(aD, 0.0);
  float mid_lift = 0.0;
  if (HaloBright > 0.0) {
    const float wmin = smoothstep(1.0, 3.0, x.I - mn);
    const float wD = max(wmax, wmin);
    aD -= min(HaloBright * wD, 1.0) * min(aD, 0.0);
    mid_lift = -min(HaloBright * kMidGain * wmin, 1.0) * min(aM, 0.0);
  }
  float a = Tone * aB + Detail * aD + Detail * mid_lift;
  a = Brighten * max(a, 0.0) + Darken * min(a, 0.0);
  float3 c_r;
  float3 c_t;
  float s;
  ColourSplit(c, x.lp, c_r, c_t, s);
  c = s * (ColourGain * c_r + HueGain * c_t) + (1.0 - s) * ColourGain * c;
  const float g = Strength * ZoneGain(x.I);
  a *= g;
  c *= g;
  // fxc expands tanh(x) as (e^x - e^-x) / (e^x + e^-x): past |x| ~ 88 e^x
  // overflows and the result is Inf * 0 = NaN, a white pixel (EditRig T7,
  // every gain 2 under 0.25-stop knees).  tanh(10) is 1.0 in float32, so the
  // clamp changes nothing else.
  if (a > 0.0 && MaxBrighten > 0.0) a = MaxBrighten * tanh(min(a / MaxBrighten, 10.0));
  if (a < 0.0 && MaxDarken > 0.0) a = MaxDarken * tanh(max(a / MaxDarken, -10.0));
  if (MaxColour > 0.0) {
    const float norm = sqrt(dot(kW, c * c));
    if (norm > 1e-6) c *= MaxColour * tanh(min(norm / MaxColour, 10.0)) / norm;
  }
  return a + c;
}

// Half-res band data at an NR pixel: the (A, B) coefficients at BOTH
// transport scales - BandA.zw narrow (Radius, today's a_B) and BandA.xy wide
// (MidRadius, v8's mid band) - plus both witnesses at the same taps: peak,
// the round-dilated input maximum (rc6's halo neighbourhood), and trough,
// the round-dilated minimum (the bright-side twin's).  The min chain is
// stored negated (.g of the R32G32 band-max surfaces) so the round penalty
// adds to it; trough negates it back.  Everything bilinear at
// (x + 0.5) * 0.5 - 0.5, clamped: the round dilation is continuous, so the
// weights have no 2-pixel steps.
struct BandSplit {
  float2 narrow;
  float2 wide;
  float peak;
  float trough;
};

BandSplit BandCoefficients(int2 pixel) {
  const uint2 half_size = (NrSize + 1u) / 2u;
  const float2 pos = clamp((float2(pixel) + 0.5) * 0.5 - 0.5, 0.0,
                           float2(half_size - 1u));
  const int2 b0 = int2(floor(pos));
  const int2 b1 = min(b0 + 1, int2(half_size) - 1);
  const float2 f = pos - float2(b0);
  BandSplit s;
  const float2 q00 = BandA[b0].zw;
  const float2 q10 = BandA[int2(b1.x, b0.y)].zw;
  const float2 q01 = BandA[int2(b0.x, b1.y)].zw;
  const float2 q11 = BandA[b1].zw;
  s.narrow = q00 * (1.0 - f.x) * (1.0 - f.y) + q10 * f.x * (1.0 - f.y)
      + q01 * (1.0 - f.x) * f.y + q11 * f.x * f.y;
  const float2 r00 = BandA[b0].xy;
  const float2 r10 = BandA[int2(b1.x, b0.y)].xy;
  const float2 r01 = BandA[int2(b0.x, b1.y)].xy;
  const float2 r11 = BandA[b1].xy;
  s.wide = r00 * (1.0 - f.x) * (1.0 - f.y) + r10 * f.x * (1.0 - f.y)
      + r01 * (1.0 - f.x) * f.y + r11 * f.x * f.y;
  const float2 m00 = BandMaxA[b0];
  const float2 m10 = BandMaxA[int2(b1.x, b0.y)];
  const float2 m01 = BandMaxA[int2(b0.x, b1.y)];
  const float2 m11 = BandMaxA[b1];
  const float2 extrema = m00 * (1.0 - f.x) * (1.0 - f.y)
      + m10 * f.x * (1.0 - f.y) + m01 * (1.0 - f.x) * f.y + m11 * f.x * f.y;
  s.peak = extrema.x;
  s.trough = -extrema.y;
  return s;
}

// History texel: (a_B, a_D, c.r, c.b), the input's I, and (v8) the mid
// band a_M, as halves in three words - a_M rides the spare high half of the
// input word, so the texel stays 12 bytes; c.g follows from dot(w, c) = 0.
uint HistoryAddress(uint2 pixel) {
  return (pixel.y * NrSize.x + pixel.x) * 12u;
}
