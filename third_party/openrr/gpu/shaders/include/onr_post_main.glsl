// SPDX-License-Identifier: MIT
// rr2_post: anisotropic Gaussian upsample of the network output to the internal resolution, history
// reprojection with up to four motion-vector candidates (5-tap bicubic), gated blend. Stores the new
// InternalHistory, this slice of OutputHistRes (input of the 3:2 downsample) and the 2x2-mean
// DownsampledHistory. A vertical pixel pair per thread.
//
// Two builds of this file: post.comp (main pass) filters with the fp32 fast path only and appends the
// 2x2 blocks where some fp16 rounding was not provably exact (about 0.5% of them) to a list;
// post_fixup.comp (POST_FIXUP) recomputes those blocks with the exact blend, through an indirect
// dispatch. Keeping the exact blend out of the main pass saves a quarter of its time.
//
// POST_DELTA (post_dlt.comp, post_fixup_dlt.comp): the tile's free halves 6, 10, 11 hold the log delta of an
// edited network image (dfn_import.comp), splatted with the same weights as the network's own image. Everything the
// network keeps is computed as without an edit; the edit reaches only the displayed image, as that delta kept in a
// history of its own: reprojected with the strongest of InternalHistory's candidates (Catmull-Rom), blended toward this
// frame's delta at a2 (at least dh_alpha where that candidate moves), added to the displayed blend's fp16 log value.
#include "onr_common.glsl"
#include "onr_post.glsl"
#include "onr_filter.glsl"
#extension GL_EXT_control_flow_attributes : require
#extension GL_KHR_shader_subgroup_quad : require
layout(local_size_x = 256) in;

layout(push_constant, scalar) uniform P {
    F16x4Buf tile;       // post_tile output, 3 x f16vec4 per network pixel
    U8x4Buf mvf;         // MvecFactors unorm8x4 (network resolution)
    F16x4Buf hist_in;    // InternalHistory (read), rgba16f at size
    F16x4Buf mv;         // PrevMvec2 rgba16f (network resolution)
    F16x2Buf mv_eroded;  // PrevMvecEroded rg16f
    F16x2Buf mv_nondil;  // PrevMvecNondilated rg16f
    F32Buf exposure;     // {exp, exp_prev, 1/exp, log ratio}
    F32Buf bicubic;      // 513 x float4 phase table
    F16x4Buf hist_out;   // InternalHistory (write)
    F16x2Buf out_rg;     // OutputHistRes slice, half2, row pitch slice.x
    F16Buf out_b;        // OutputHistRes slice, half
    U32Buf down;         // DownsampledHistory R10G10B10A2, (size + 1) / 2
    U32Buf fix;          // {indirect x, 1, 1, count, (x, y) per 2x2 block to recompute}
    float gate_scale;
    vec2 scale;
    vec2 offset;         // network position = (x + 0.5) * scale + offset
    vec2 inv_size;
    vec2 lr_inv_size;
    ivec2 size;          // internal resolution
    ivec2 lr_size;       // network resolution
    ivec2 origin;        // slice origin
    ivec2 end;           // slice end (exclusive)
    ivec2 slice;         // OutputHistRes row pitch, rows
    uint reset;          // bit 0: history reset; bit 1 (test): list every block for the fixup pass
#ifdef POST_DELTA
                         // bit 2: the delta history starts from this frame's delta; bit 3: no edit this frame (the
                         // delta history is carried over, not blended)
    U32Buf dh_in;        // delta history (read), dpack per internal pixel
    U32Buf dh_out;       // delta history (write)
    float dh_alpha;
#endif
} pc;

#define EPS10 uintBitsToFloat(0x2edbe6ffu)

#if defined(ENC_F32) && ENC_F32
// fast variant (post_f): the fp32 blends rounded to fp16 without the exactness test, no fixup list, hardware
// reciprocal and a plain division by 255
#define POST_PLAIN 1
#define rcp_rn(x) (1.0 / (x))
#define blend_unorm8(W_, c0_, c1_, c2_, c3_) (float(W_.x * int(c0_) + W_.y * int(c1_) + W_.z * int(c2_) + W_.w * int(c3_)) * (1.0 / (255.0 * 65536.0)))
bool g_bad = false;
#define TAP(ax, ay, t0, t1, t2, t3) blend3_plain(ax, ay, t0, t1, t2, t3)
#define HBLEND(w, a, b, c, d) blend_half_plain(w, a, b, c, d)
#elif defined(POST_FIXUP)
#define TAP(ax, ay, t0, t1, t2, t3) blend3(ax, ay, t0, t1, t2, t3)
#define HBLEND(w, a, b, c, d) blend_half(w, a, b, c, d)
#else
bool g_bad = false;   // some rounding of this thread was not provably exact
#define TAP(ax, ay, t0, t1, t2, t3) blend3_fast(ax, ay, t0, t1, t2, t3, g_bad)
#define HBLEND(w, a, b, c, d) blend_half_fast(w, a, b, c, d, g_bad)
#endif

vec4 bicubic_row(int i) { F32Buf b = AT32(F32Buf, pc.bicubic, i, 16u); return vec4(b.v[0], b.v[1], b.v[2], b.v[3]); }

vec3 hist_texel(int x, int y) { return vec3(AT32(F16x4Buf, pc.hist_in, y * pc.size.x + x, 8u).v[0].xyz); }
#ifdef POST_PLAIN
// reset bit 4: InternalHistory through the sampler (linear, clamp, unnormalized; tex[bit 5] is a linear image over
// hist_in), at texel coordinates already on the reference's 8-bit grid, rounded to fp16 as a fp16 texture fetch is
layout(set = 0, binding = 0) uniform sampler2D tex[8];
vec3 hist_hw(float x, float y) { return vec3(f16vec3(textureLod(tex[(pc.reset >> 5) & 1u], vec2(x, y), 0.0).xyz)); }
#endif

// The bicubic is a cross of 5 bilinear taps. The outer ones (x - 1.5, x + 1.5 and the same in y) sit on
// texel centres: their coordinate u = (k + 0.5) * inv_size is within (|k| + 0.5) 2^-23 texels of the
// centre, far below 1/512, so the 8-bit fraction is exactly 0 for any size below 16384. Those taps are
// 1D lerps of two texels; only the middle one needs four. (axl/axr/ayt/ayb: second texel unused.)
// the 5-tap sum, called in each branch: no phi of the taps, so the sampler's fp16 fetches fold into v_fma_mix
vec3 bicubic_sum(vec4 tx, vec4 ty, vec3 T0, vec3 T1, vec3 T2, vec3 T3, vec3 T4) {
    precise float w0 = tx.x * ty.y, w1 = tx.y * ty.x, w2 = tx.y * ty.y, w3 = tx.y * ty.z, w4 = tx.z * ty.y;
    precise float S = (((w0 + w1) + w2) + w3) + w4;
    precise vec3 c = w1 * T1;
    c = fma(T0, vec3(w0), c);
    c = fma(vec3(w2), T2, c);
    c = fma(vec3(w3), T3, c);
    c = fma(vec3(w4), T4, c);
    precise vec3 r = rcp_rn(S) * c;
    return r;
}

vec3 bicubic_hist(float px, float py) {
    precise float bx = floor(px - 0.5) + 0.5;
    precise float by = floor(py - 0.5) + 0.5;
    precise float fx = clamp(px - bx, 0.0, 1.0);
    precise float fy = clamp(py - by, 0.0, 1.0);
    int ix = min(int(trunc(fma(fx, 512.0, 0.5))), 512);
    int iy = min(int(trunc(fma(fy, 512.0, 0.5))), 512);
    vec4 tx = bicubic_row(ix), ty = bicubic_row(iy);
    precise float xm = pc.inv_size.x * (bx + tx.w);
    precise float ym = pc.inv_size.y * (by + ty.w);
    int i = int(bx - 0.5), j = int(by - 0.5);
    vec3 T0, T1, T2, T3, T4;
#ifdef POST_PLAIN
    if ((pc.reset & 16u) != 0u) {
        // the taps on the sampler: the outer ones at texel centres across, the middle one at (xm, ym)
        float xq = float(fixed8_coord(xm, pc.size.x)) * (1.0 / 256.0) + 0.5, yq = float(fixed8_coord(ym, pc.size.y)) * (1.0 / 256.0) + 0.5;
        float xl = float(i - 1) + 0.5, xr = float(i + 2) + 0.5, yt = float(j - 1) + 0.5, yb = float(j + 2) + 0.5;
        return bicubic_sum(tx, ty, hist_hw(xl, yq), hist_hw(xq, yt), hist_hw(xq, yq), hist_hw(xq, yb), hist_hw(xr, yq));
    }
#endif
    {
    ivec3 axm = axis_setup(xm, pc.size.x), aym = axis_setup(ym, pc.size.y);
    ivec3 axl = ivec3(clamp(i - 1, 0, pc.size.x - 1), 0, 0), axr = ivec3(clamp(i + 2, 0, pc.size.x - 1), 0, 0);
    ivec3 ayt = ivec3(clamp(j - 1, 0, pc.size.y - 1), 0, 0), ayb = ivec3(clamp(j + 2, 0, pc.size.y - 1), 0, 0);
    T0 = TAP(axl, aym, hist_texel(axl.x, aym.x), vec3(0.0), hist_texel(axl.x, aym.y), vec3(0.0));
    T1 = TAP(axm, ayt, hist_texel(axm.x, ayt.x), hist_texel(axm.y, ayt.x), vec3(0.0), vec3(0.0));
    T2 = TAP(axm, aym, hist_texel(axm.x, aym.x), hist_texel(axm.y, aym.x), hist_texel(axm.x, aym.y), hist_texel(axm.y, aym.y));
    T3 = TAP(axm, ayb, hist_texel(axm.x, ayb.x), hist_texel(axm.y, ayb.x), vec3(0.0), vec3(0.0));
    T4 = TAP(axr, aym, hist_texel(axr.x, aym.x), vec3(0.0), hist_texel(axr.x, aym.y), vec3(0.0));
    return bicubic_sum(tx, ty, T0, T1, T2, T3, T4);
    }
}
#ifdef POST_DELTA
// A log delta in 32 bits: per channel the sign, a 4-bit exponent (2^-13 .. 2^2) and 6 / 6 / 5 mantissa bits (R, G, B),
// rounded to nearest from its fp16; below 2^-13 it is 0. k: fp16 mantissa bits dropped.
uint dpack1(float v, uint k) {
    uint h = packHalf2x16(vec2(isnan(v) ? 0.0 : v, 0.0)) & 0xffffu;
    uint a = h & 0x7fffu, m = 0u;
    if (a >= 0x0800u) {
        a -= 0x0800u;
        m = min((a + (1u << (k - 1u)) - 1u + ((a >> k) & 1u)) >> k, 0x3fffu >> k);
    }
    return ((h >> 15) << (14u - k)) | m;
}
float dunpack1(uint f, uint k) {
    uint m = f & (0x3fffu >> k);
    if (m == 0u) return 0.0;
    return unpackHalf2x16(((f >> (14u - k)) << 15) | ((m << k) + 0x0800u)).x;
}
uint dpack(vec3 v) { return dpack1(v.x, 4u) | (dpack1(v.y, 4u) << 11) | (dpack1(v.z, 5u) << 22); }
vec3 dunpack(uint u) { return vec3(dunpack1(u & 0x7ffu, 4u), dunpack1((u >> 11) & 0x7ffu, 4u), dunpack1(u >> 22, 5u)); }

vec3 dh_texel(int x, int y) { return dunpack(AT32(U32Buf, pc.dh_in, y * pc.size.x + x, 4u).v[0]); }

// Catmull-Rom on the 12 taps without the corners: at a low rate a bilinear reprojection blurs the delta in motion.
// On a texel centre (a still camera) it is that texel.
vec3 dh_catrom(float px, float py) {
    float bx = px - 0.5, by = py - 0.5;
    float ix = floor(bx), iy = floor(by), fx = bx - ix, fy = by - iy;
    vec4 wx = 0.5 * vec4(fx * (fx * (2.0 - fx) - 1.0), fx * fx * (3.0 * fx - 5.0) + 2.0, fx * (fx * (4.0 - 3.0 * fx) + 1.0), fx * fx * (fx - 1.0));
    vec4 wy = 0.5 * vec4(fy * (fy * (2.0 - fy) - 1.0), fy * fy * (3.0 * fy - 5.0) + 2.0, fy * (fy * (4.0 - 3.0 * fy) + 1.0), fy * fy * (fy - 1.0));
    ivec4 cx = clamp(int(ix) + ivec4(-1, 0, 1, 2), ivec4(0), ivec4(pc.size.x - 1));
    ivec4 cy = clamp(int(iy) + ivec4(-1, 0, 1, 2), ivec4(0), ivec4(pc.size.y - 1));
    vec3 r = vec3(0.0);
    float total = 0.0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            if ((i == 0 || i == 3) && (j == 0 || j == 3)) continue;
            float w = wx[i] * wy[j];
            r += w * dh_texel(cx[i], cy[j]);
            total += w;
        }
    return r / total;
}
#ifdef LT_DLT_PAIR
// The same Catmull-Rom for both pixels of a vertical pair at once: the same fractions, the lower one a row down, so 16 taps
// instead of 24. Rows r0 .. r4; the upper pixel takes r0 .. r3 and the lower r1 .. r4, each without its own corners.
vec3 g_d[2];
void dh_catrom_pair(float px, float py) {
    float bx = px - 0.5, by = py - 0.5;
    float ix = floor(bx), iy = floor(by), fx = bx - ix, fy = by - iy;
    vec4 wx = 0.5 * vec4(fx * (fx * (2.0 - fx) - 1.0), fx * fx * (3.0 * fx - 5.0) + 2.0, fx * (fx * (4.0 - 3.0 * fx) + 1.0), fx * fx * (fx - 1.0));
    vec4 wy = 0.5 * vec4(fy * (fy * (2.0 - fy) - 1.0), fy * fy * (3.0 * fy - 5.0) + 2.0, fy * (fy * (4.0 - 3.0 * fy) + 1.0), fy * fy * (fy - 1.0));
    ivec4 cx = clamp(int(ix) + ivec4(-1, 0, 1, 2), ivec4(0), ivec4(pc.size.x - 1));
    int y0 = int(iy) - 1;
    vec3 in_[5], out_[3];
    [[unroll]] for (int j = 0; j < 5; ++j) {
        int y = clamp(y0 + j, 0, pc.size.y - 1);
        in_[j] = wx[1] * dh_texel(cx[1], y) + wx[2] * dh_texel(cx[2], y);
        if (j >= 1 && j <= 3) out_[j - 1] = wx[0] * dh_texel(cx[0], y) + wx[3] * dh_texel(cx[3], y);
    }
    float wi = wx[1] + wx[2], wo = wx[0] + wx[3];
    vec3 u = wy[0] * in_[0] + wy[1] * (in_[1] + out_[0]) + wy[2] * (in_[2] + out_[1]) + wy[3] * in_[3];
    vec3 l = wy[0] * in_[1] + wy[1] * (in_[2] + out_[1]) + wy[2] * (in_[3] + out_[2]) + wy[3] * in_[4];
    float t = (wy[0] + wy[3]) * wi + (wy[1] + wy[2]) * (wi + wo);
    float it = 1.0 / t;
    g_d[0] = u * it, g_d[1] = l * it;
}
#endif
#endif

struct Cand { float f, mx, my; };

void sw(inout Cand p, inout Cand q) {
    if (!(p.f >= q.f)) { Cand t = p; p = q; q = t; }
}

// Stage 1 for a vertical pixel pair (Y, Y + 1), as the reference thread does it: both pixels use the
// 4x4 network neighbourhood around the base row of the upper pixel, so the taps are read once.
struct Splat {
    f16vec3 c, a;
    float w;
#ifdef POST_DELTA
    f16vec3 e;
#endif
};

#ifdef LT_POST_EXP
// 2^e of the splat weights, both pixels at once: e clamped to [-16, 0], (1 + e ln2 / 32)^32 as five squarings
f16vec2 lt_exp2(f16vec2 e) {
    f16vec2 b = max(1.0hf + e * float16_t(0.6931471805599453 / 32.0), f16vec2(0.0hf));
    b *= b; b *= b; b *= b; b *= b; b *= b;
    return b;
}
#endif

void splat_pair(int X, float fx, float fy0, float fy1, out Splat s0, out Splat s1) {
    int r8 = int(floor(fx - 0.5));
    int ry0 = int(floor(fy0 - 0.5)), ry1 = int(floor(fy1 - 0.5));
    precise float dxf = (float(r8) + 0.5) - fx;
    precise float dyf0 = (float(ry0) + 0.5) - fy0;
    precise float dyf1 = (float(ry1) + 0.5) - fy1;
    float16_t dx0 = float16_t(dxf), dy0 = float16_t(dyf0), dy1 = float16_t(dyf1);
    int base = ry0;
    if (base != ry1) dy1 = dy1 + (-1.0hf);
    // both pixels' sums packed: (c.xyz, c.w) and (a.x, a.y) per pixel, the second pixel's terms in the high halves
    precise f16vec4 c0 = f16vec4(0.0hf), c1 = f16vec4(0.0hf);
    precise f16vec2 a0 = f16vec2(0.0hf), a1 = f16vec2(0.0hf);
#ifdef POST_DELTA
    precise f16vec3 x0 = f16vec3(0.0hf), x1 = f16vec3(0.0hf);
#endif
    precise float w0 = 0.0, w1 = 0.0;
    for (int k = -1; k <= 2; ++k) {
        precise float16_t dxk = dx0 + float16_t(k);
        precise float16_t dx2 = dxk * dxk;
        int cx = clamp(r8 + k, 0, pc.lr_size.x - 1);
        for (int j = -1; j <= 2; ++j) {
#ifdef LT_POST_NOCORNER
            if ((k == -1 || k == 2) && (j == -1 || j == 2)) continue;
#endif
            int cy = clamp(base + j, 0, pc.lr_size.y - 1);
            F16x4Buf tb = AT32(F16x4Buf, pc.tile, cy * pc.lr_size.x + cx, 24u);
            f16vec4 c = tb.v[0], a = tb.v[1];
#ifdef POST_DELTA
            f16vec4 t2 = tb.v[2];
            f16vec2 n = t2.xy;
            f16vec3 ed = f16vec3(a.z, t2.z, t2.w);   // the edit's log delta (dfn_import)
#else
            f16vec2 n = tb.v[2].xy;
#endif
            // the two pixels' exponents in one packed chain (x: upper pixel, y: lower)
            precise f16vec2 dyj = f16vec2(dy0, dy1) + float16_t(j);
            precise f16vec2 dy2 = dyj * dyj;
            precise float16_t e0 = dx2 * a.w;
            precise f16vec2 e = fma(dy2, f16vec2(n.x), f16vec2(e0));
            precise f16vec2 dxy = dxk * dyj;
            e = fma(dxy, f16vec2(n.y), e);
#ifdef LT_POST_EXP
            f16vec2 wv = lt_exp2(e);
            float16_t w = wv.x, v = wv.y;
            float w32 = float(w), v32 = float(v);
#else
            float w32 = exp2(float(e.x));
            float16_t w = float16_t(w32);
#endif
            c0 = fma(f16vec4(w), c, c0);
#ifdef POST_DELTA
            x0 = fma(f16vec3(w), ed, x0);
#endif
            a0 = fma(f16vec2(w), a.xy, a0);
            w0 = w0 + w32;
#ifndef LT_POST_EXP
            float v32 = exp2(float(e.y));
            float16_t v = float16_t(v32);
#endif
            c1 = fma(f16vec4(v), c, c1);
#ifdef POST_DELTA
            x1 = fma(f16vec3(v), ed, x1);
#endif
            a1 = fma(f16vec2(v), a.xy, a1);
            w1 = w1 + v32;
        }
    }
#ifdef POST_DELTA
    s0 = Splat(c0.xyz, f16vec3(c0.w, a0), w0, x0);
    s1 = Splat(c1.xyz, f16vec3(c1.w, a1), w1, x1);
#else
    s0 = Splat(c0.xyz, f16vec3(c0.w, a0), w0);
    s1 = Splat(c1.xyz, f16vec3(c1.w, a1), w1);
#endif
}

#ifdef LT_POST_PAIR
// Candidates, their slots and weights once per vertical pair: the motion and factors at the pair's middle, the slots from
// the upper pixel, the lower pixel's taps a row down
Cand cs[4];
float sat[4], rs;
int g_nl, g_ns, g_sl[4];
vec2 g_q[4];
float g_wc[4], g_cum;
void pair_cands(int X, int Y0, float fx, float fy) {
    precise float lu = fx * pc.lr_inv_size.x;
    precise float lv = fy * pc.lr_inv_size.y;
    int lx = int(floor(fx)), ly = int(floor(fy));
    // ---- four motion-vector candidates with their factors
    Bilerp bl = bilerp_setup(lu, lv, pc.lr_size);
    ivec4 bw = bilerp_weights(bl);
    uint m0 = uint(bl.i0.y * pc.lr_size.x + bl.i0.x), m1 = uint(bl.i0.y * pc.lr_size.x + bl.i1.x);
    uint m2 = uint(bl.i1.y * pc.lr_size.x + bl.i0.x), m3 = uint(bl.i1.y * pc.lr_size.x + bl.i1.x);
    uvec4 f0 = uvec4(AT32(U8x4Buf, pc.mvf, m0, 4u).v[0]), f1 = uvec4(AT32(U8x4Buf, pc.mvf, m1, 4u).v[0]);
    uvec4 f2 = uvec4(AT32(U8x4Buf, pc.mvf, m2, 4u).v[0]), f3 = uvec4(AT32(U8x4Buf, pc.mvf, m3, 4u).v[0]);
    vec4 mvf;
    for (int c = 0; c < 4; ++c) mvf[c] = blend_unorm8(bw, f0[c], f1[c], f2[c], f3[c]);
    vec2 n0 = vec2(AT32(F16x2Buf, pc.mv_nondil, m0, 4u).v[0]), n1 = vec2(AT32(F16x2Buf, pc.mv_nondil, m1, 4u).v[0]);
    vec2 n2 = vec2(AT32(F16x2Buf, pc.mv_nondil, m2, 4u).v[0]), n3 = vec2(AT32(F16x2Buf, pc.mv_nondil, m3, 4u).v[0]);
    vec2 mvn;
    for (int c = 0; c < 2; ++c) mvn[c] = HBLEND(bw, n0[c], n1[c], n2[c], n3[c]);
    vec4 mv = vec4(0.0);
    vec2 mve = vec2(0.0);
    if (lx >= 0 && lx < pc.lr_size.x && ly >= 0 && ly < pc.lr_size.y) {
        uint i = uint(ly * pc.lr_size.x + lx);
        mv = vec4(AT32(F16x4Buf, pc.mv, i, 8u).v[0]);
        mve = vec2(AT32(F16x2Buf, pc.mv_eroded, i, 4u).v[0]);
    }
    Cand s1 = Cand(mvf.x, mv.x, mv.y), s2 = Cand(mvf.y, mvn.x, mvn.y);
    Cand s3 = Cand(mvf.z, mve.x, mve.y), s4 = Cand(mvf.w, mv.z, mv.w);
    sw(s1, s2); sw(s3, s4); sw(s1, s3); sw(s2, s4); sw(s2, s3);
    cs = Cand[4](s1, s2, s3, s4);
    [[unroll]] for (int i = 0; i < 4; ++i) sat[i] = clamp(cs[i].f, 0.0, 1.0);
    precise float tot = ((sat[0] + sat[1]) + sat[2]) + sat[3];
    rs = rcp_rn(max(tot, EPS10));

    precise float fx7 = float(X) + 0.5, fy10 = float(Y0) + 0.5;
    precise float cum = 0.0;
    bool live = true;
    g_nl = 0, g_ns = 0;
    g_q = vec2[4](vec2(0.0), vec2(0.0), vec2(0.0), vec2(0.0));
    [[unroll]] for (int i = 0; i < 4; ++i) {
        g_sl[i] = -1;
        g_wc[i] = 0.0;
        if (!live) continue;
        g_nl = i + 1;
        precise float w = sat[i] * rs;
        g_wc[i] = w;
        precise float px = fx7 + cs[i].mx;
        precise float py = cs[i].my + fy10;
        bool ok = !(px < 0.0) && !(px > float(pc.size.x)) && !(py < 0.0) && !(py > float(pc.size.y)) && (pc.reset & 1u) == 0u;
        bool same = false;
        [[unroll]] for (int k = 0; k < i; ++k)
            if (!same && cs[k].mx == cs[i].mx && cs[k].my == cs[i].my) { g_sl[i] = g_sl[k]; same = true; }
        if (!same && ok) {
            [[unroll]] for (int k = 0; k <= i; ++k) if (g_ns == k) g_q[k] = vec2(px, py);
            g_sl[i] = g_ns;
            g_ns = g_ns + 1;
        }
        if (i == 0) cum = w; else cum = cum + w;
        live = !(cum >= uintBitsToFloat(0x3f666666u)) && !isnan(cum);
    }
    g_cum = cum;
#if defined(POST_DELTA) && defined(LT_DLT_PAIR)
    dh_catrom_pair(g_q[0].x, g_q[0].y);
#endif
}
#endif

// Everything after the splat for one internal pixel. out1 (linear) feeds InternalHistory and
// DownsampledHistory, out2 the OutputHistRes.
// POST_DELTA: dlt is the new delta history, added to out2's encoding for the display.
#ifdef POST_DELTA
void pixel(int X, int Y, float fx, float fy, Splat sp, out vec3 out1, out vec3 out2, out vec3 dlt) {
#else
void pixel(int X, int Y, float fx, float fy, Splat sp, out vec3 out1, out vec3 out2) {
#endif
    f16vec3 accc = sp.c, acca = sp.a;
    float wsum = sp.w;

    // ---- stage 2: network colour
    float e0 = pc.exposure.v[0], e1 = pc.exposure.v[1], e2 = pc.exposure.v[2];
    precise float lu = fx * pc.lr_inv_size.x;
    precise float lv = fy * pc.lr_inv_size.y;
    int lx = int(floor(fx)), ly = int(floor(fy));    float inv_w = rcp_rn(max(wsum, EPS10));
    precise vec3 nrm = inv_w * vec3(accc);
    precise vec3 nra = inv_w * vec3(acca);
    vec3 net = post_decode(nrm);
    precise vec3 net_e = e1 * (e2 * net);
#ifdef LT_POST_NETH
    // only where no history candidate is valid; the exposure's change over one frame left out
    vec3 net_h = (pc.reset & 1u) != 0u ? post_encode(net_e) : nrm;
#else
    vec3 net_h = post_encode(net_e);
#endif
#ifdef POST_DELTA
    const bool edited = (pc.reset & 8u) == 0u;
    vec3 dcur = edited ? inv_w * vec3(sp.e) : vec3(0.0);
    vec3 dacc = vec3(0.0);
#endif

#ifndef LT_POST_PAIR
    // ---- four motion-vector candidates with their factors
    Bilerp bl = bilerp_setup(lu, lv, pc.lr_size);
    ivec4 bw = bilerp_weights(bl);
    uint m0 = uint(bl.i0.y * pc.lr_size.x + bl.i0.x), m1 = uint(bl.i0.y * pc.lr_size.x + bl.i1.x);
    uint m2 = uint(bl.i1.y * pc.lr_size.x + bl.i0.x), m3 = uint(bl.i1.y * pc.lr_size.x + bl.i1.x);
    uvec4 f0 = uvec4(AT32(U8x4Buf, pc.mvf, m0, 4u).v[0]), f1 = uvec4(AT32(U8x4Buf, pc.mvf, m1, 4u).v[0]);
    uvec4 f2 = uvec4(AT32(U8x4Buf, pc.mvf, m2, 4u).v[0]), f3 = uvec4(AT32(U8x4Buf, pc.mvf, m3, 4u).v[0]);
    vec4 mvf;
    for (int c = 0; c < 4; ++c) mvf[c] = blend_unorm8(bw, f0[c], f1[c], f2[c], f3[c]);
    vec2 n0 = vec2(AT32(F16x2Buf, pc.mv_nondil, m0, 4u).v[0]), n1 = vec2(AT32(F16x2Buf, pc.mv_nondil, m1, 4u).v[0]);
    vec2 n2 = vec2(AT32(F16x2Buf, pc.mv_nondil, m2, 4u).v[0]), n3 = vec2(AT32(F16x2Buf, pc.mv_nondil, m3, 4u).v[0]);
    vec2 mvn;
    for (int c = 0; c < 2; ++c) mvn[c] = HBLEND(bw, n0[c], n1[c], n2[c], n3[c]);
    vec4 mv = vec4(0.0);
    vec2 mve = vec2(0.0);
    if (lx >= 0 && lx < pc.lr_size.x && ly >= 0 && ly < pc.lr_size.y) {
        uint i = uint(ly * pc.lr_size.x + lx);
        mv = vec4(AT32(F16x4Buf, pc.mv, i, 8u).v[0]);
        mve = vec2(AT32(F16x2Buf, pc.mv_eroded, i, 4u).v[0]);
    }
    Cand s1 = Cand(mvf.x, mv.x, mv.y), s2 = Cand(mvf.y, mvn.x, mvn.y);
    Cand s3 = Cand(mvf.z, mve.x, mve.y), s4 = Cand(mvf.w, mv.z, mv.w);
    sw(s1, s2); sw(s3, s4); sw(s1, s3); sw(s2, s4); sw(s2, s3);
    Cand cs[4] = Cand[4](s1, s2, s3, s4);
    float sat[4];
    [[unroll]] for (int i = 0; i < 4; ++i) sat[i] = clamp(cs[i].f, 0.0, 1.0);
    precise float tot = ((sat[0] + sat[1]) + sat[2]) + sat[3];
    float rs = rcp_rn(max(tot, EPS10));

#endif
    // ---- history: candidates in order until their factors reach 0.9
    // the cut depends on the factors only, so first the live candidates and their distinct in-history motions, packed
    // per lane into slots 0 .. ns - 1; then one bicubic per slot (the wave runs max ns of them, not one per candidate
    // index any lane needs), then the sum in candidate order
#ifdef LT_POST_PAIR
    float pl = float(Y & 1);
    vec3 sm0 = vec3(0.0), sm1 = vec3(0.0), sm2 = vec3(0.0), sm3 = vec3(0.0);
    if (g_ns > 0) sm0 = bicubic_hist(g_q[0].x, g_q[0].y + pl);
    if (g_ns > 1) sm1 = bicubic_hist(g_q[1].x, g_q[1].y + pl);
    if (g_ns > 2) sm2 = bicubic_hist(g_q[2].x, g_q[2].y + pl);
    if (g_ns > 3) sm3 = bicubic_hist(g_q[3].x, g_q[3].y + pl);
#ifdef POST_DELTA
#ifdef LT_DLT_PAIR
    dacc = g_sl[0] == 0 ? g_d[Y & 1] : dcur;
#else
    dacc = g_sl[0] == 0 ? dh_catrom(g_q[0].x, g_q[0].y + pl) : dcur;
#endif
#endif
    precise vec3 accu = vec3(0.0);
    [[unroll]] for (int i = 0; i < 4; ++i) {
        if (i >= g_nl) break;
        int k = g_sl[i];
        vec3 smp = k < 0 ? net_h : k == 0 ? sm0 : k == 1 ? sm1 : k == 2 ? sm2 : sm3;
        precise float w = g_wc[i];
        if (i == 0) accu = w * smp;
        else accu = fma(vec3(w), smp, accu);
    }
    float cum = g_cum;
#else
    precise float fx7 = float(X) + 0.5, fy10 = float(Y) + 0.5;
    precise float cum = 0.0;
    bool live = true;
    int nl = 0, ns = 0, sl[4];
    vec2 q[4] = vec2[4](vec2(0.0), vec2(0.0), vec2(0.0), vec2(0.0));
    float wc[4];
    [[unroll]] for (int i = 0; i < 4; ++i) {
        sl[i] = -1;
        wc[i] = 0.0;
        if (!live) continue;
        nl = i + 1;
        precise float w = sat[i] * rs;
        wc[i] = w;
        precise float px = fx7 + cs[i].mx;
        precise float py = cs[i].my + fy10;
        bool ok = !(px < 0.0) && !(px > float(pc.size.x)) && !(py < 0.0) && !(py > float(pc.size.y)) && (pc.reset & 1u) == 0u;
        // a candidate with the motion of an earlier one samples the same texels: it takes that slot
        bool same = false;
        [[unroll]] for (int k = 0; k < i; ++k)
            if (!same && cs[k].mx == cs[i].mx && cs[k].my == cs[i].my) { sl[i] = sl[k]; same = true; }
        if (!same && ok) {
            [[unroll]] for (int k = 0; k <= i; ++k) if (ns == k) q[k] = vec2(px, py);
            sl[i] = ns;
            ns = ns + 1;
        }
        if (i == 0) {
            cum = w;
#ifdef POST_DELTA
            dacc = ok ? dh_catrom(px, py) : dcur;   // the first (strongest) candidate only
#endif
        } else {
            cum = cum + w;
        }
#ifdef LT_POST_CUM
        live = !(cum >= float(LT_POST_CUM)) && !isnan(cum);
#else
        live = !(cum >= uintBitsToFloat(0x3f666666u)) && !isnan(cum);
#endif
    }
    vec3 sm0 = vec3(0.0), sm1 = vec3(0.0), sm2 = vec3(0.0), sm3 = vec3(0.0);
    if (ns > 0) sm0 = bicubic_hist(q[0].x, q[0].y);
    if (ns > 1) sm1 = bicubic_hist(q[1].x, q[1].y);
    if (ns > 2) sm2 = bicubic_hist(q[2].x, q[2].y);
    if (ns > 3) sm3 = bicubic_hist(q[3].x, q[3].y);
    precise vec3 accu = vec3(0.0);
    [[unroll]] for (int i = 0; i < 4; ++i) {
        if (i >= nl) break;
        int k = sl[i];
        vec3 smp = k < 0 ? net_h : k == 0 ? sm0 : k == 1 ? sm1 : k == 2 ? sm2 : sm3;
        precise float w = wc[i];
        if (i == 0) accu = w * smp;
        else accu = fma(vec3(w), smp, accu);
    }
#endif
    precise vec3 h_log = accu * rcp_rn(cum);
    precise vec3 h_lin = e0 * (post_decode(h_log) * rcp_rn(e1));

    // ---- gates
    precise float g = clamp((pc.gate_scale * wsum) * nra.x, 0.0, 1.0);
    precise float a1 = fma(g, 1.0 - nra.y, nra.y);
    precise float a2 = fma(g, 1.0 - nra.z, nra.z);
    precise vec3 d = net - h_lin;
    precise vec3 o1 = fma(d, vec3(a1), h_lin);
    precise vec3 o2 = fma(d, vec3(a2), h_lin);
#ifdef POST_DELTA
    vec3 dprev = (pc.reset & 4u) != 0u ? dcur : dacc;
    // the display's own rate a2, so the delta averages over the jitter as the image does; a floor of dh_alpha where
    // the camera moves (half a pixel or more), and a2 itself where RR drops its history (a disocclusion)
    float dmove = clamp(2.0 * length(vec2(cs[0].mx, cs[0].my)), 0.0, 1.0);
    dlt = mix(dprev, dcur, edited ? max(pc.dh_alpha * dmove, a2) : 0.0);
    if (any(isnan(dlt))) dlt = vec3(0.0);
#endif
    precise float s1v = abs((o1.x + o1.y) + o1.z);
    precise float s2v = abs((o2.x + o2.y) + o2.z);
    out1 = (s1v <= uintBitsToFloat(0x7f800000u)) ? o1 : net;
    out2 = (s2v <= uintBitsToFloat(0x7f800000u)) ? o2 : net;
}

void main() {
    // 16 x 32 pixels per workgroup, a vertical pixel pair per thread. Lanes li and li ^ 1 hold
    // horizontally adjacent pairs, so the DownsampledHistory 2x2 mean is (top + bottom) in the thread,
    // then one quad swap for the column sum, the reference's order.
    uint li = gl_LocalInvocationIndex;
#ifdef POST_FIXUP
    uint item = gl_WorkGroupID.x * 128u + (li >> 1);
    if (item >= pc.fix.v[3]) return;
    int X = int(pc.fix.v[4u + 2u * item]) + int(li & 1u);
    int Y0 = int(pc.fix.v[5u + 2u * item]);
#else
    // workgroups walk the frame in strips 16 groups wide
    uint gx = gl_NumWorkGroups.x, gy = gl_NumWorkGroups.y, idx = gl_WorkGroupID.y * gx + gl_WorkGroupID.x;
    uint strip = idx / (16u * gy), base = strip * 16u, sw = min(16u, gx - base), within = idx - strip * 16u * gy;
    ivec2 tile = ivec2(base + within % sw, within / sw) * ivec2(16, 32);
    int X = pc.origin.x + tile.x + int(li & 15u);
    int Y0 = pc.origin.y + tile.y + 2 * int(li >> 4);
#endif
    int x1 = min(pc.end.x, pc.size.x), y1 = min(pc.end.y, pc.size.y);
    bool v0 = X < x1 && Y0 < y1, v1 = X < x1 && Y0 + 1 < y1;
    precise float fx = fma(float(X) + 0.5, pc.scale.x, pc.offset.x);
    precise float fy0 = fma(pc.scale.y, float(Y0) + 0.5, pc.offset.y);
    precise float fy1 = fma(pc.scale.y, float(Y0 + 1) + 0.5, pc.offset.y);
    Splat s0, s1;
    if (v0) splat_pair(X, fx, fy0, fy1, s0, s1);
#ifdef LT_POST_PAIR
    if (v0) pair_cands(X, Y0, fx, 0.5 * (fy0 + fy1));
#endif
    vec3 o1t = vec3(0.0), o1b = vec3(0.0);
    [[dont_unroll]] for (int p = 0; p < 2; ++p) {
        if (!(p == 0 ? v0 : v1)) continue;
        int Y = Y0 + p;
        vec3 o1, o2;
#ifdef POST_DELTA
        vec3 dl;
        pixel(X, Y, fx, p == 0 ? fy0 : fy1, p == 0 ? s0 : s1, o1, o2, dl);
        AT32(U32Buf, pc.dh_out, Y * pc.size.x + X, 4u).v[0] = dpack(dl);
#else
        pixel(X, Y, fx, p == 0 ? fy0 : fy1, p == 0 ? s0 : s1, o1, o2);
#endif
        if (p == 0) o1t = o1; else o1b = o1;
        vec3 h = post_encode(o1);
        AT32(F16x4Buf, pc.hist_out, Y * pc.size.x + X, 8u).v[0] = f16vec4(f16vec3(h), 0.0hf);
        int sx = X - pc.origin.x, sy = Y - pc.origin.y;
        if (sx < pc.slice.x && sy < pc.slice.y) {
            vec3 o = post_encode(o2);
            uint si = uint(sy * pc.slice.x + sx);
#ifdef POST_DELTA
            // added in fp16 to the stored value: a zero delta leaves the network's display bits as they are
            f16vec3 oh = f16vec3(o) + f16vec3(dl);
            AT32(F16x2Buf, pc.out_rg, si, 4u).v[0] = oh.xy;
            AT32(F16Buf, pc.out_b, si, 2u).v[0] = oh.z;
#else
            AT32(F16x2Buf, pc.out_rg, si, 4u).v[0] = f16vec2(o.xy);
            AT32(F16Buf, pc.out_b, si, 2u).v[0] = float16_t(o.z);
#endif
        }
    }
    precise vec3 col = o1t + o1b;
    precise vec3 s = col + subgroupQuadSwapHorizontal(col);
    int c = int(v0) + int(v1);
    c += subgroupQuadSwapHorizontal(c);
#if !defined(POST_FIXUP) && !defined(POST_PLAIN)
    uint qbad = uint(g_bad) | (pc.reset >> 1);
    qbad |= subgroupQuadSwapHorizontal(qbad);
    if ((li & 1u) == 0u && v0 && qbad != 0u) {
        uint i = atomicAdd(pc.fix.v[3], 1u);
        pc.fix.v[4u + 2u * i] = uint(X);
        pc.fix.v[5u + 2u * i] = uint(Y0);
        atomicMax(pc.fix.v[0], i / 128u + 1u);
    }
#endif
    if ((li & 1u) != 0u || !v0) return;
    precise vec3 m = s * rcp_rn(float(max(c, 1)));
    vec3 v = post_encode(m);
    precise vec3 qv = v * uintBitsToFloat(0x3ed0fac6u);            // 1/2.45
    int dw = (pc.size.x + 1) / 2;
    AT32(U32Buf, pc.down, (Y0 / 2) * dw + X / 2, 4u).v[0] = pack1010102(qv);
}