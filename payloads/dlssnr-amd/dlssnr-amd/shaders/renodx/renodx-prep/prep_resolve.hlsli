// DLSSNR-AMD (ours, not RenoDX's): the resolve's ratio when [Preprocess] made the proxy (v6_common.hlsli, built
// with NR_PREP_RESOLVE: renodx_v6_resolve_prep, renodx_v6_resolve_commit_prep, renodx_v6_pedestal_reduce_unshaped_prep).
//
// Preprocess shows the network forward(x) - contrast, saturation, then a display curve - and the resolve multiplies
// the untouched frame by the network's N / P. Taken in the curve's domain that ratio shrinks every edit by the
// curve's compression (at the shoulder most), so here it is taken behind the curve: N and P go back through
// backward() (runtime_prep.comp's, omitting the common linear gain in the ratio), the change is limited as
// runtime_prep.comp limits it (at most CAP times what a plain gain would hand back for the same display change), and
// the ratio is formed there. The none-curve branch explicitly keeps unchanged channels at one; other curves retain their rounded division.
//
// The curves and their inverses are runtime_prep.comp's (linux/shaders/passes, prep_math.glsl), in HLSL.

static const float3 kPrepLuma = float3(0.2126, 0.7152, 0.0722);
static const float kPrepCap = 4.0;

float3 PrepNeutralInv(float3 y) {
  const float start = 0.76, d = 0.24, desat = 0.15;
  const float np = max(y.x, max(y.y, y.z));
  if (np < start) return y;
  const float peak = d * d / max(1.0 - np, 1.0 / 128.0) - d + start;   // bounded as prep_math.glsl's (blotches)
  const float g = 1.0 - 1.0 / (desat * (peak - np) + 1.0);
  return (y - g * np) / (1.0 - g) * (peak / np);
}
float3 PrepReinhardInv(float3 y) { y = min(y, 0.9999); return y / (1.0 - y); }

static const float kHA = 0.15, kHB = 0.50, kHC = 0.10, kHD = 0.20, kHE = 0.02, kHF = 0.30;
float PrepHable1(float x) { return ((x * (kHA * x + kHC * kHB) + kHD * kHE) / (x * (kHA * x + kHB) + kHD * kHF)) - kHE / kHF; }
float PrepHableInv1(float y) {
  const float t = min(y * PrepHable1(11.2) + kHE / kHF, 0.999);
  const float qa = kHA * (1.0 - t), qb = kHC * kHB - kHB * t, qc = kHD * kHE - kHD * kHF * t;
  return max((-qb + sqrt(max(qb * qb - 4.0 * qa * qc, 0.0))) / (2.0 * qa), 0.0);
}
float3 PrepHableInv(float3 y) { return float3(PrepHableInv1(y.x), PrepHableInv1(y.y), PrepHableInv1(y.z)); }

float PrepGt1(float x) {
  const float P = 1.0, a = 1.0, m = 0.22, l = 0.4, c = 1.33, b = 0.0;
  const float l0 = ((P - m) * l) / a, S0 = m + l0, S1 = m + a * l0;
  const float C2 = (a * P) / (P - S1), CP = -C2 / P;
  const float w0 = 1.0 - smoothstep(0.0, m, x);
  const float w2 = step(m + l0, x);
  const float w1 = 1.0 - w0 - w2;
  const float T = m * pow(max(x, 0.0) / m, c) + b;
  const float S = P - (P - S1) * exp(CP * (x - S0));
  const float L = m + a * (x - m);
  return T * w0 + L * w1 + S * w2;
}
// runtime_prep.comp's gt_inv1: closed forms above m, Newton's method in the toe.
float PrepGtInv1(float y) {
  const float P = 1.0, a = 1.0, m = 0.22, l = 0.4, c = 1.33;
  const float l0 = ((P - m) * l) / a, S0 = m + l0, S1 = m + a * l0;
  const float C2 = (a * P) / (P - S1);
  if (y >= PrepGt1(64.0)) return 64.0;
  if (y >= S0) return S0 - log(max((P - y) / (P - S1), 1e-30)) / C2;
  if (y >= m) return y;
  float t = pow(max(y, 0.0) / m, 1.0 / c);
  for (int i = 0; i < 4; ++i) {
    const float s = t * t * (3.0 - 2.0 * t), ds = 6.0 * t * (1.0 - t), tc = pow(t, c);
    const float g = tc * (1.0 - s) + t * s - y / m;
    const float dg = c * pow(max(t, 1e-20), c - 1.0) * (1.0 - s) - tc * ds + s + t * ds;
    t = clamp(t - g / max(dg, 1e-6), 0.0, 1.0);
  }
  return m * t;
}
float3 PrepGtInv(float3 y) { return float3(PrepGtInv1(y.x), PrepGtInv1(y.y), PrepGtInv1(y.z)); }

float3 PrepAcesInv(float3 y) {
  y = min(y, 0.9999);
  const float3 qa = 2.51 - 2.43 * y, qb = 0.03 - 0.59 * y, qc = -0.14 * y;
  return max((-qb + sqrt(max(qb * qb - 4.0 * qa * qc, 0.0))) / (2.0 * qa), 0.0);
}

// AgX: runtime_prep.comp's matrices (GLSL column order) - AGX_OUT is the inverse of AGX_IN, so each inverse below is
// the other matrix.
float3 PrepAgxIn(float3 c) {   // AGX_IN * c
  return c.x * float3(0.842479062253094, 0.0423282422610123, 0.0423756549057051) +
         c.y * float3(0.0784335999999992, 0.878468636469772, 0.0784336) +
         c.z * float3(0.0792237451477643, 0.0791661274605434, 0.879142973793104);
}
float3 PrepAgxOut(float3 c) {   // AGX_OUT * c
  return c.x * float3(1.19687900512017, -0.0528968517574562, -0.0529716355144438) +
         c.y * float3(-0.0980208811401368, 1.15190312990417, -0.0980434501171241) +
         c.z * float3(-0.0990297440797205, -0.0989611768448433, 1.15107367264116);
}
static const float kAgxMin = -12.47393, kAgxMax = 4.026069;
float PrepAgxSig(float x) {
  const float x2 = x * x, x4 = x2 * x2;
  return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
float PrepAgxDsig(float x) {
  const float x2 = x * x, x4 = x2 * x2;
  return 93.0 * x4 * x - 200.7 * x4 + 127.84 * x2 * x - 20.604 * x2 + 0.8596 * x + 0.1191;
}
// runtime_prep.comp's agx_sig_inv: 8 bisection steps, then 3 Newton steps inside the bracket.
float PrepAgxSigInv(float y) {
  y = clamp(y, PrepAgxSig(0.0), PrepAgxSig(1.0));
  float lo = 0.0, hi = 1.0;
  for (int i = 0; i < 8; ++i) {
    const float mid = 0.5 * (lo + hi);
    if (PrepAgxSig(mid) < y) lo = mid; else hi = mid;
  }
  float x = 0.5 * (lo + hi);
  for (int i = 0; i < 3; ++i) x = clamp(x - (PrepAgxSig(x) - y) / PrepAgxDsig(x), lo, hi);
  return x;
}
float3 PrepAgxInv(float3 y) {
  float3 v = PrepAgxIn(pow(max(y, 0.0), 1.0 / 2.2));   // inverse(AGX_OUT)
  v = float3(PrepAgxSigInv(v.x), PrepAgxSigInv(v.y), PrepAgxSigInv(v.z));
  v = exp2(v * (kAgxMax - kAgxMin) + kAgxMin);
  return PrepAgxOut(v);   // inverse(AGX_IN)
}

// backward() of runtime_prep.comp without the gain: display-linear -> the frame's light, up to one common factor.
float3 PrepBackward(float3 y) {
  float3 x;
  switch (PrepCurve) {
    case 1u: x = PrepNeutralInv(y) / PrepK; break;
    case 2u: x = PrepReinhardInv(y) / PrepK; break;
    case 3u: x = PrepHableInv(y) / PrepK; break;
    case 4u: x = PrepGtInv(y) / PrepK; break;
    case 5u: x = PrepAcesInv(y) / PrepK; break;
    case 6u: x = PrepAgxInv(y) / PrepK; break;
    case 7u: // Curve = none: v6_encode's display toe followed by its shoulder.
    case 8u: { // Curve = none with the classic codec: the shoulder alone.
      const float peak = max(y.x, max(y.y, y.z));
      const float unshouldered = peak > 0.75
          ? 0.75 - log(max(1.0 - (peak - 0.75) / 0.25, 1e-7)) / 5.770780 : peak;
      float light = PrepCurve == 7u ? DisplayCurveInverse(peak) : unshouldered;
      if (PrepCurve == 7u && peak > 0.0 && peak < DisplayCurve(1e-6)) {
        // v6_encode divides its toe gain by max(m, 1e-6), which changes the very bottom of the curve.
        float lo = 0.0, hi = 1e-6;
        for (int i = 0; i < 20; ++i) {
          const float m = (lo + hi) * 0.5;
          if (DisplayCurve(m) * (m / 1e-6) < peak) lo = m; else hi = m;
        }
        light = (lo + hi) * 0.5;
      }
      x = peak > 0.0 ? y * (light / peak) : y;
      break;
    }
    default: x = y; break;
  }
  if (PrepSaturation != 1.0) {
    const float l = dot(x, kPrepLuma);
    x = l + (x - l) / PrepSaturation;
  }
  if (PrepContrast != 1.0) x = 0.18 * pow(max(x, 0.0) / 0.18, 1.0 / PrepContrast);
  return x;
}

// The ratio the resolve applies, from the proxy P and the network's answer N (display-linear, as the resolve reads
// them). Below the floor the frame's light is the floor's: PrepFloor = PrepBackward(1/1024 grey).y, the resolve's
// own floor behind the curve - the same for every pixel, so the host computes it (nr_renodx.cpp prep_floor).
float3 PrepSafeRatio(float3 proxy, float3 neural) {
  const float3 p = PrepBackward(proxy), n = PrepBackward(neural);
  const float3 lim = kPrepCap * abs(neural - proxy);
  const float3 d = clamp(n - p, -lim, lim);
  const float3 ratio = max(p + d, PrepFloor) / max(p, PrepFloor);
  // The none-curve inverse keeps an unchanged channel at one despite reciprocal rounding.
  if (PrepCurve == 7u || PrepCurve == 8u)
    return float3(proxy.x == neural.x ? 1.0 : ratio.x, proxy.y == neural.y ? 1.0 : ratio.y,
                  proxy.z == neural.z ? 1.0 : ratio.z);
  return ratio;
}
