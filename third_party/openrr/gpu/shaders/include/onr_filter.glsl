// SPDX-License-Identifier: MIT
// Texture filtering done by hand so it matches the reference GPU: clamp addressing, texel coordinate
// u*w - 0.5 with the fraction rounded to 8 bits, exact blend, fp16 textures return an fp16 (RN) value.
// All of it is exact without fp64 (fp64 is slow on consumer RDNA). The AMD sampler is not usable for
// this: its weights are on the same 8-bit grid, but its blend is not exact (harness `probe`).
#extension GL_EXT_control_flow_attributes : require

struct Bilerp { ivec2 i0, i1; ivec2 w; };   // integer taps (clamped) and 8-bit fractions

// floor(u * n * 256 - 127.5) exactly: the fixed-point code of the texel coordinate (u*n - 0.5) * 256
// rounded to nearest, i.e. 256 * integer part + 8-bit fraction.
int fixed8_coord(float u, int n) {
#if defined(ENC_F32) && ENC_F32
    // fast variant: the same code from an fp32 product (can differ by one 1/256 step next to a code boundary)
    return int(floor(fma(u, float(n) * 256.0, -127.5)));
#endif
    int b = floatBitsToInt(u);
    int e = (b >> 23) & 255;
    if (e == 0) return -128;                                  // zero or denormal: u*n*256 < 1
    int64_t m = int64_t((b & 0x7fffff) | 0x800000) * int64_t(n);
    if (b < 0) m = -m;
    int sh = e - 150 + 8 + 32;                                // value * 2^32
    int64_t q = sh >= 0 ? (m << sh) : (m >> (-sh));          // arithmetic shift = floor
    q -= int64_t(0x7f80000000l);                              // 127.5 * 2^32
    return int(q >> 32);
}

Bilerp bilerp_setup(float u, float v, ivec2 size) {
    int xq = fixed8_coord(u, size.x);
    int yq = fixed8_coord(v, size.y);
    int x0 = xq >> 8, y0 = yq >> 8;
    Bilerp b;
    b.w = ivec2(xq & 255, yq & 255);
    b.i0 = clamp(ivec2(x0, y0), ivec2(0), size - 1);
    b.i1 = clamp(ivec2(x0 + 1, y0 + 1), ivec2(0), size - 1);
    return b;
}

// weights scaled by 65536 for taps (x0,y0) (x1,y0) (x0,y1) (x1,y1)
ivec4 bilerp_weights(Bilerp b) {
    int ax = b.w.x, ay = b.w.y;
    return ivec4((256 - ax) * (256 - ay), ax * (256 - ay), (256 - ax) * ay, ax * ay);
}

void two_sum(float a, float b, out float s, out float e) {
    precise float ss = a + b;
    precise float bb = ss - a;
    precise float ee = (a - (ss - bb)) + (b - bb);
    s = ss;
    e = ee;
}

// exact blend of 4 fp16 texels -> fp16 RN -> float. Products and sums are kept as float-float.
float blend_half_exact(ivec4 w, float t0, float t1, float t2, float t3) {
    vec4 wf = vec4(w), t = vec4(t0, t1, t2, t3);
    precise vec4 p = wf * t;
    precise vec4 pe = fma(wf, t, -p);                        // exact product errors
    float s, c1, c2, c3;
    two_sum(p.x, p.y, s, c1);
    two_sum(s, p.z, s, c2);
    two_sum(s, p.w, s, c3);
    precise float lo = ((pe.x + pe.y) + (pe.z + pe.w)) + ((c1 + c2) + c3);
    float hi, l2;
    two_sum(s, lo, hi, l2);
    return float(half_rn_ff(hi * (1.0 / 65536.0), l2 * (1.0 / 65536.0)));
}

// Same result: an fp32 fma chain s rounded to fp16, when s - err and s + err round to the same fp16
// bits (the exact value lies between them and rounding is monotonic). err bounds the chain's rounding
// plus one ulp of s, so the rounding of s -/+ err itself cannot pull the bounds inside.
bool half_safe(float s, float err, out float hv) {
    uint b = packFloat2x16(f16vec2(s - err, s + err));
    hv = float(unpackFloat2x16(b).x);
    return (b & 0xffffu) == (b >> 16) && (b & 0x7c00u) != 0x7c00u;
}
float blend_half_fast(ivec4 w, float t0, float t1, float t2, float t3, inout bool bad) {
    vec4 wf = vec4(w), t = vec4(t0, t1, t2, t3);
    precise float s = fma(wf.w, t.w, fma(wf.z, t.z, fma(wf.y, t.y, wf.x * t.x))) * (1.0 / 65536.0);
    precise float a = dot(wf, abs(t)) * (1.0 / 65536.0);
    float hv;
    bool ok = half_safe(s, a * (1.0 / 2700000.0), hv);    // 4 roundings (2^-22 a) + ulp(s) (2^-23 a)
    bad = bad || !ok;
    return hv;
}

float blend_half(ivec4 w, float t0, float t1, float t2, float t3) {
    bool bad = false;
    float r = blend_half_fast(w, t0, t1, t2, t3, bad);
    return bad ? blend_half_exact(w, t0, t1, t2, t3) : r;
}

// Two-texel case (the other axis has a zero fraction): weights (256 - f) * 256 and f * 256. The
// products (256 - f) t0 and f t1 are exact (<= 20 bits); when the texels are within a factor 4 of each
// other (or one is 0) their sum fits in 24 bits, so s is exact and needs no check.
float blend_half2_fast(int f, float t0, float t1, inout bool bad) {
    precise float s = fma(float(f), t1, float(256 - f) * t0) * (1.0 / 256.0);
    float mx = max(abs(t0), abs(t1)), mn = min(abs(t0), abs(t1));
    if (mx <= 4.0 * mn || mn == 0.0) return float(float16_t(s));
    precise float a = fma(float(f), abs(t1), float(256 - f) * abs(t0)) * (1.0 / 256.0);
    float hv;
    bool ok = half_safe(s, a * (1.0 / 5500000.0), hv);    // one rounding (2^-24 a) + ulp(s) (2^-23 a)
    bad = bad || !ok;
    return hv;
}

ivec4 bilerp_w(ivec3 ax, ivec3 ay) {
    return ivec4((256 - ax.z) * (256 - ay.z), ax.z * (256 - ay.z), (256 - ax.z) * ay.z, ax.z * ay.z);
}

// RGB bilinear from axis setups; t1..t3 only matter when their weight is non-zero. The fast path flags
// `bad` when a channel may round differently from the exact blend; callers then use blend3_exact,
// once, out of the hot path (inlining the exact blend at every tap made the kernel 50% slower).
vec3 blend3_fast(ivec3 ax, ivec3 ay, vec3 t0, vec3 t1, vec3 t2, vec3 t3, inout bool bad) {
    vec3 r;
    if (ax.z == 0 && ay.z == 0) return t0;
    if (ay.z == 0) { for (int c = 0; c < 3; ++c) r[c] = blend_half2_fast(ax.z, t0[c], t1[c], bad); return r; }
    if (ax.z == 0) { for (int c = 0; c < 3; ++c) r[c] = blend_half2_fast(ay.z, t0[c], t2[c], bad); return r; }
    ivec4 w = bilerp_w(ax, ay);
    for (int c = 0; c < 3; ++c) r[c] = blend_half_fast(w, t0[c], t1[c], t2[c], t3[c], bad);
    return r;
}

// fast variant: the same fp32 chains, rounded to fp16 with no test
float blend_half_plain(ivec4 w, float t0, float t1, float t2, float t3) {
    vec4 wf = vec4(w);
    float s = fma(wf.w, t3, fma(wf.z, t2, fma(wf.y, t1, wf.x * t0))) * (1.0 / 65536.0);
    return float(float16_t(s));
}
vec3 blend3_plain(ivec3 ax, ivec3 ay, vec3 t0, vec3 t1, vec3 t2, vec3 t3) {
    if (ax.z == 0 && ay.z == 0) return t0;
    if (ay.z == 0) return vec3(f16vec3(fma(vec3(float(ax.z)), t1, float(256 - ax.z) * t0) * (1.0 / 256.0)));
    if (ax.z == 0) return vec3(f16vec3(fma(vec3(float(ay.z)), t2, float(256 - ay.z) * t0) * (1.0 / 256.0)));
    ivec4 w = bilerp_w(ax, ay);
    vec3 r;
    for (int c = 0; c < 3; ++c) r[c] = blend_half_plain(w, t0[c], t1[c], t2[c], t3[c]);
    return r;
}

vec3 blend3_exact(ivec3 ax, ivec3 ay, vec3 t0, vec3 t1, vec3 t2, vec3 t3) {
    ivec4 w = bilerp_w(ax, ay);
    vec3 r;
    [[dont_unroll]] for (int c = 0; c < 3; ++c) r[c] = blend_half_exact(w, t0[c], t1[c], t2[c], t3[c]);
    return r;
}

vec3 blend3(ivec3 ax, ivec3 ay, vec3 t0, vec3 t1, vec3 t2, vec3 t3) {
    bool bad = false;
    vec3 r = blend3_fast(ax, ay, t0, t1, t2, t3, bad);
    return bad ? blend3_exact(ax, ay, t0, t1, t2, t3) : r;
}
// s / 255 correctly rounded for 0 <= s < 2^24, no fp64. The residual s - 255 r is computed in integers
// (LLPC folds fma(-(s * c), 255, s) to 0, so an fp32 residual is not reliable). r = M 2^-sh, the
// residual in units of 2^-sh is D = s 2^sh - 255 M and r is right while -127 <= D <= 127 (-63 at the
// bottom of a binade, where the next value down is half an ulp away). No ties: s / 255 is an integer or
// not dyadic.
float div255_rn(int s) {
    if (s == 0) return 0.0;
    uint b = floatBitsToUint(float(s) * (1.0 / 255.0));
    for (int it = 0; it < 2; ++it) {
        int sh = 150 - int(b >> 23);
        uint m = (b & 0x7fffffu) | 0x800000u;
        int d = int((uint(s) << sh) - 255u * m);
        if (d > 127) b += 1u;
        else if (d < (m == 0x800000u ? -63 : -127)) b -= 1u;
    }
    return uintBitsToFloat(b);
}
// exact blend of 4 unorm8 codes -> value / 255 in fp32 (one rounding).
float blend_unorm8(ivec4 w, uint c0, uint c1, uint c2, uint c3) {
    int s = w.x * int(c0) + w.y * int(c1) + w.z * int(c2) + w.w * int(c3);
    return div255_rn(s) * (1.0 / 65536.0);
}

// One axis of a bilinear tap: clamped texel pair and 8-bit fraction.
ivec3 axis_setup(float u, int n) {
    int q = fixed8_coord(u, n);
    int i = q >> 8;
    return ivec3(clamp(i, 0, n - 1), clamp(i + 1, 0, n - 1), q & 255);
}