// SPDX-License-Identifier: MIT
// Runtime path of onr_block.glsl (k8, every product on WMMA); same arithmetic as the reference body there
// (ENC_F32: the fast variant, fp32 accumulation over the whole K of each product, no 8-K rounding),
// organised for latency: subgroup s owns the 16 tokens 16 s .. 16 s + 15 of the window and does every
// row-local product for them (Q, V, scores, softmax, P V, out, MLP, heads, outputs) without workgroup
// barriers. Only the keys/values cross rows: per head, Q_h and V_h of all 64 tokens go to LDS and a barrier
// publishes them (QV_BUFS 1: one more barrier before the next head overwrites them). The out projection is
// accumulated head by head (K order unchanged). SPLIT 2 (few windows): two subgroups per row tile, each with
// half of the output tiles of every product.
// LDS (halves): XS x / x1 / x2 [64][C]; QV [QV_BUFS][Q_h 64x32 | V_h 64x32] (also the prologue scratch);
// PR per row tile [16][72]: scores / P, then O_h, then the MLP hidden slice and the head scratch; OB (SPLIT 2) O_h.
// Arena bytes move as u32 (4 arena channels: GPU positions g, g + 1, g + 16, g + 17).

// LDS-only synchronisation: a plain barrier() also drains the global loads in flight (LLPC waits on every
// counter), which serialises the weight fetches with the window's phases
#ifdef PROF_NOSYNC
#define WG_SYNC() memoryBarrierShared()   // timing only: no workgroup barrier
#else
#define WG_SYNC() controlBarrier(gl_ScopeWorkgroup, gl_ScopeWorkgroup, gl_StorageSemanticsShared, gl_SemanticsAcquireRelease)
#endif
#define SG_SYNC() controlBarrier(gl_ScopeSubgroup, gl_ScopeSubgroup, gl_StorageSemanticsShared, gl_SemanticsAcquireRelease)
// a row tile is one subgroup (SPLIT 1) or two (SPLIT 2: then the whole workgroup syncs)
#if SPLIT == 2
#define RT_SYNC() WG_SYNC()
#else
#define RT_SYNC() SG_SYNC()
#endif
#define NTH (128u * SPLIT)
#define NSG (4u * SPLIT)

#if ENC_F32
// fast variant: one fp32-accumulated WMMA per 16 K, one rounding to fp16 per product (as the store)
#define MSTEP(acc, alo, ahi, b) { acc = coopMatMulAdd(alo, b, acc); }
#define ALOAD(alo, ahi, off, stride) { coopMatLoad(alo, lds, off, stride, ROW); }
#else
#define MSTEP(acc, alo, ahi, b) { acc = coopMatMulAdd(alo, b, acc); acc = MC(MH(acc)); acc = coopMatMulAdd(ahi, b, acc); acc = MC(MH(acc)); }
#define ALOAD(alo, ahi, off, stride) { MA a_; coopMatLoad(a_, lds, off, stride, ROW); alo = a_ * mlo; ahi = a_ * mhi; }
#endif
#define GSTEP(acc, alo, ahi, buf, off, stride) { MB b_; coopMatLoad(b_, buf, off, stride, COL); MSTEP(acc, alo, ahi, b_) }

#define NTL (NT / SPLIT)
#define TILE(i) ((i) * uint(SPLIT) + hf)
#if NTL > 1
#define F1(M) M(c1, TILE(1u))
#else
#define F1(M)
#endif
#if NTL > 2
#define F2(M) M(c2, TILE(2u))
#else
#define F2(M)
#endif
#if NTL > 3
#define F3(M) M(c3, TILE(3u))
#else
#define F3(M)
#endif
#if NTL > 4
#define F4(M) M(c4, TILE(4u))
#else
#define F4(M)
#endif
#if NTL > 5
#define F5(M) M(c5, TILE(5u))
#else
#define F5(M)
#endif
#if NTL > 6
#define F6(M) M(c6, TILE(6u))
#else
#define F6(M)
#endif
#if NTL > 7
#define F7(M) M(c7, TILE(7u))
#else
#define F7(M)
#endif
#if NTL > 8
#define F8(M) M(c8, TILE(8u))
#else
#define F8(M)
#endif
#if NTL > 9
#define F9(M) M(c9, TILE(9u))
#else
#define F9(M)
#endif
#ifndef HD_UNROLL
#define HD_UNROLL
#endif
#ifndef MLP64
#define MLP64 (ENC_C <= 64 || SPLIT == 2)
#endif
#ifndef J_UNROLL
#define J_UNROLL
#endif
#define FOR_NT(M) M(c0, TILE(0u)) F1(M) F2(M) F3(M) F4(M) F5(M) F6(M) F7(M) F8(M) F9(M)

// Transposed body (TR, SPLIT 1): every product is computed as its transpose, D^T = W A^T, so the activations
// are always the B operand. On RDNA4 a 16x16 accumulator copied element by element is the B operand of the
// same matrix (probe_wmma), so x, Q_h, P, O_h, the MLP hidden slice and the residuals never pass through LDS;
// only Q_h / V_h of all tokens (the keys) do. Same products, same K order, same roundings as the row body.
#if TR
#if NT > 1
#define G1(M) M(c1, x1, 1u)
#else
#define G1(M)
#endif
#if NT > 2
#define G2(M) M(c2, x2, 2u)
#else
#define G2(M)
#endif
#if NT > 3
#define G3(M) M(c3, x3, 3u)
#else
#define G3(M)
#endif
#if NT > 4
#define G4(M) M(c4, x4, 4u)
#else
#define G4(M)
#endif
#if NT > 5
#define G5(M) M(c5, x5, 5u)
#else
#define G5(M)
#endif
#if NT > 6
#define G6(M) M(c6, x6, 6u)
#else
#define G6(M)
#endif
#if NT > 7
#define G7(M) M(c7, x7, 7u)
#else
#define G7(M)
#endif
#if NT > 8
#define G8(M) M(c8, x8, 8u)
#else
#define G8(M)
#endif
#if NT > 9
#define G9(M) M(c9, x9, 9u)
#else
#define G9(M)
#endif
#define FOR_NX(M) M(c0, x0, 0u) G1(M) G2(M) G3(M) G4(M) G5(M) G6(M) G7(M) G8(M) G9(M)
// the e4m3 B tiles of the input (FP8QV): y0 .. y(NT - 1)
#define FOR_NY(M) M(y0, 0u) M(y1, 1u) Y2(M) Y3(M) Y4(M) Y5(M) Y6(M) Y7(M) Y8(M) Y9(M)
#if NT > 2
#define Y2(M) M(y2, 2u) M(y3, 3u)
#else
#define Y2(M)
#endif
#define Y3(M)
#if NT > 4
#define Y4(M) M(y4, 4u) M(y5, 5u)
#else
#define Y4(M)
#endif
#define Y5(M)
#if NT > 6
#define Y6(M) M(y6, 6u) M(y7, 7u)
#else
#define Y6(M)
#endif
#define Y7(M)
#if NT > 8
#define Y8(M) M(y8, 8u) M(y9, 9u)
#else
#define Y8(M)
#endif
#define Y9(M)
// fp16 accumulator -> B operand (k8: B masked to K rows 0..7, then 8..15)
#if ENC_F32
#define BPREP(blo, bhi, h) { for (int i_ = 0; i_ < h.length(); ++i_) blo[i_] = h[i_]; }
#define TSTEP(acc, a, blo, bhi) { acc = coopMatMulAdd(a, blo, acc); }
#else
#define BPREP(blo, bhi, h) { MB b_; for (int i_ = 0; i_ < h.length(); ++i_) b_[i_] = h[i_]; blo = b_ * mblo; bhi = b_ * mbhi; }
#define TSTEP(acc, a, blo, bhi) { acc = coopMatMulAdd(a, blo, acc); acc = MC(MH(acc)); acc = coopMatMulAdd(a, bhi, acc); acc = MC(MH(acc)); }
#endif
#ifndef FP8PROTO
#define FP8PROTO 0
#endif
#if FP8PROTO && ENC_F32
// FP8 timing prototype (investigation only, numerics not validated): every product on e4m3 WMMA, operands
// requantized from fp16 (weights converted after an fp16 load; a real path would load e4m3 weights)
#extension GL_EXT_float_e4m3 : require
#define MA8 coopmat<floate4m3_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA>
#define MB8 coopmat<floate4m3_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB>
#undef BPREP
#undef TSTEP
#define BPREP(blo, bhi, h) { MB b_; for (int i_ = 0; i_ < h.length(); ++i_) b_[i_] = h[i_]; blo = MB8(b_); }
#define TSTEP(acc, a, blo, bhi) { acc = coopMatMulAdd(MA8(a), blo, acc); }
#if FP8PROTO == 2
// timing only: A operands read as e4m3 straight from the fp16 buffers (no conversion; values meaningless) and B
// converted from the accumulator without the fp16 step
#undef BPREP
#define BPREP(blo, bhi, h) { for (int i_ = 0; i_ < h.length(); ++i_) blo[i_] = floate4m3_t(h[i_]); }
#define TSTEP8(acc, a8, blo) { acc = coopMatMulAdd(a8, blo, acc); }
#endif
#define MBP MB8
#else
#define MBP MB
#endif
// weights as the A operand: W[n0 .. n0 + 15][k0 .. k0 + 15] row-major
#if FP8PROTO == 2 && ENC_F32
#define WSTEP(acc, buf, off, stride, blo, bhi) { MA8 a8_; coopMatLoad(a8_, buf, off, stride, ROW); TSTEP8(acc, a8_, blo) }
#elif defined(PROF_NOGW)
#define WSTEP(acc, buf, off, stride, blo, bhi) { MA a_; coopMatLoad(a_, lds, ((off) & 1023u) & ~15u, 16u, ROW); TSTEP(acc, a_, blo, bhi) }
#else
#define WSTEP(acc, buf, off, stride, blo, bhi) { MA a_; coopMatLoad(a_, buf, off, stride, ROW); TSTEP(acc, a_, blo, bhi) }
#endif
// bias per row of a transposed accumulator
ACC bias_acc_t(F16Buf b, uint n0) {
    MH h;
    coopMatLoad(h, b.v, n0, 0u, COL);
    return ACC(h);
}
// DFN pre-activations from the x2^T tiles: d^T [16 gains][16 rows] = dfn x2^T + b, stored as the row body's d
#define DFN_PRE_T if (hf == 0u) { ACC acc = bias_acc_t(pc.dfn_b, 0u); FOR_NX(DFN_T) coopMatStore(MH(acc), lds, pr, 16u, COL); }
#define DFN_T(cn, xn, n) { MBP blo, bhi; BPREP(blo, bhi, xn) WSTEP(acc, pc.dfn_w.v, (n) * 16u, ENC_C, blo, bhi) }
// SPLIT 2: the two subgroups of a row tile both hold all x^T tiles; each accumulates half of the output tiles
// (own tile i is tile i + NO hf) and they exchange them through XS, one 16x16 block per tile and row tile
#define NO (NT / SPLIT)
#define TO(i) ((i) + hf * NO)
#if NO > 1
#define H1(M) M(c1, 1u)
#else
#define H1(M)
#endif
#if NO > 2
#define H2(M) M(c2, 2u)
#else
#define H2(M)
#endif
#if NO > 3
#define H3(M) M(c3, 3u)
#else
#define H3(M)
#endif
#if NO > 4
#define H4(M) M(c4, 4u)
#else
#define H4(M)
#endif
#define FOR_NO(M) M(c0, 0u) H1(M) H2(M) H3(M) H4(M)
#define EXB(n) (XS + rt * 256u * NT + (n) * 256u)
#define EX_ST(cn, i) coopMatStore(MH(cn), lds, EXB(TO(i)), 16u, ROW);
#endif

// FP8QV (fast path, transposed body, input straight from the arena): Q and V on e4m3 x e4m3 -> fp32 WMMA. The
// weights are exact e4m3 (an OCP e4m3 copy follows the fp16 one in the in_a / in_b buffers); the arena codes with
// exponent field 0 ((1 + m/8) 2^-7 on sm_86) map to the nearest OCP subnormal (exact for even m, ties to even).
// One rounding to fp16 per product instead of one per 16 K.
// VROW: V_h computed as x W_v^T (not transposed) and stored COL, one 16-byte LDS store per lane and tile
// (on at C >= 64; the C = 32 body is left as it is)
#ifndef VROW
#define VROW (TR && SPLIT == 1 && ENC_C >= 64)
#endif
#ifndef FP8QV
// on where it measured faster: C >= 64 with the input from the arena (not at C = 32: slower there; in the W2 body
// at C = 64 too)
#define FP8QV (ENC_F32 && TR && ENC_EMBED == 0 && ENC_MERGE_IN == 0 && DEC_UP == 0 && ENC_C >= 64)
#endif
#if FP8QV
#extension GL_EXT_float_e4m3 : require
#define QA8 coopmat<floate4m3_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA>
#define QB8 coopmat<floate4m3_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB>
#define QC8 coopmat<float, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>
#ifndef QV8_OFF
#define QV8_OFF (2u * ENC_X * ENC_C)
#endif
layout(buffer_reference, scalar, buffer_reference_align = 1) buffer E4View { floate4m3_t v[]; };
// four sm_86 codes -> OCP codes: where (b & 0x78) == 0, mantissa k = 4 + (m >> 1) + (m & 3 == 3)
uint ocp4(uint w) {
    uint z = ~((w & 0x78787878u) + 0x78787878u) & 0x80808080u;
    uint m = w & 0x07070707u;
    uint k = 0x04040404u + ((m >> 1) & 0x03030303u) + (m & (m >> 1) & 0x01010101u);
    uint zm = (z >> 7) * 0xffu;
    return (w & ~zm) | (((w & 0x80808080u) | k) & zm);
}
#define W8STEP(acc, addr, off, b) { QA8 a_; coopMatLoad(a_, E4View(addr).v, off, ENC_C, ROW); acc = coopMatMulAdd(a_, b, acc); }
#endif

// GPU fragment position of arena channel a (inverse of arena_channel within a plane)
uint arena_pos(uint a) { return (a & ~31u) | (a & 1u) | (((a >> 2) & 3u) << 1) | (((a >> 4) & 1u) << 3) | (((a >> 1) & 1u) << 4); }

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer U32View { uint v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) buffer U32x4View { uvec4 v[]; };

// transposed body: lane l holds token r0 + l % 16 at GPU positions 8 h .. 8 h + 7 of every tile (h = l / 16); in a plane of
// 32 channels those of its two tiles are exactly bytes 16 h .. 16 h + 15 of the token's 32-byte record (arena_pos), so the
// block's input and output go straight between that record and the registers. XREG_IN: the input is the arena itself.
#if TR
#define XREG_IN (ENC_EMBED == 0 && ENC_MERGE_IN == 0)
#else
#define XREG_IN 0
#endif

// e4m3 codes 0, 1 (e4m3_lo) or 2, 3 (e4m3_hi) of w -> two packed fp16, sm_86 expansion (e4m3_in): each code << 8 in
// its half (one v_perm), then sign | ((code & 0x7f) << 7) + (8 << 10): the arithmetic shift by 1 per half leaves the
// sign in bits 15 and 14, bit 14 is cleared
uint e4m3_x2(uint t) { return (pack32(u16vec2(i16vec2(unpack16(t)) >> 1s)) & 0xbf80bf80u) + 0x20002000u; }
// the byte moves as masks of shifts, which LLVM turns into one v_perm_b32 (u8vec4 packing gave three ops)
uint e4m3_lo(uint w) { return e4m3_x2(((w << 8) & 0x0000ff00u) | ((w << 16) & 0xff000000u)); }
uint e4m3_hi(uint w) { return e4m3_x2(((w >> 8) & 0x0000ff00u) | (w & 0xff000000u)); }

// e4m3_out on two halves at once: codes in bits 8..15 and 24..31. Rounding half away from zero at mantissa bit 7 is
// |b| + 0x40 truncated, and the clamp of a positive magnitude is monotonic in its bits (same codes as e4m3_out for
// every finite value; inf / NaN give 448). The clamped magnitude m (0x2001 .. 0x5f00 per half) gives the code as
// (m << 1) - 0x4000 in the half's high byte, no carry across halves
uint e4m3_out2(f16vec2 x) {
    uint b = packFloat2x16(x);
    u16vec2 m = clamp(unpack16((b & 0x7fff7fffu) + 0x00400040u), u16vec2(0x2001us), u16vec2(0x5f00us));
    return ((pack32(m) << 1) + 0xbfffc000u) | (b & 0x80008000u);
}

// codes of a.x, a.y, b.x, b.y in bytes 0..3
uint e4m3x4(f16vec2 a, f16vec2 b) {
    uint u = e4m3_out2(a), v = e4m3_out2(b);
    // bytes 1, 3 of u and of v: one v_perm_b32
    u8vec4 p = unpack8(u), q = unpack8(v);
    return pack32(u8vec4(p.y, p.w, q.y, q.w));
}

// arena channels a .. a + 15 (a % 16 == 0) sit at two runs of 8 GPU positions of the row: g .. g + 7 (bytes 0, 1 of
// each u32) and g + 16 .. g + 23 (bytes 2, 3); put16 / get16 move them as two 16-byte LDS accesses
uint pos16(uint a) { return (a & ~31u) + ((a & 16u) >> 1); }

void put16(uint base, uint a, uvec4 w) {
    uint g = base + pos16(a);
    lds4[g >> 3] = uvec4(e4m3_lo(w.x), e4m3_lo(w.y), e4m3_lo(w.z), e4m3_lo(w.w));
    lds4[(g + 16u) >> 3] = uvec4(e4m3_hi(w.x), e4m3_hi(w.y), e4m3_hi(w.z), e4m3_hi(w.w));
}

uvec4 get16(uint base, uint a) {
    uint g = base + pos16(a);
    uvec4 lo = lds4[g >> 3], hi = lds4[(g + 16u) >> 3];
    return uvec4(e4m3x4(unpackFloat2x16(lo.x), unpackFloat2x16(hi.x)), e4m3x4(unpackFloat2x16(lo.y), unpackFloat2x16(hi.y)),
                 e4m3x4(unpackFloat2x16(lo.z), unpackFloat2x16(hi.z)), e4m3x4(unpackFloat2x16(lo.w), unpackFloat2x16(hi.w)));
}


// exp_bits on two scores: the cubic run on 2 z, so it gives 8 yv (exact: every scale is a power of two). 8 yv is in
// [8, 16), exponent field 10010b, and a 16-bit shift by 5 per half drops it whole: the bits of yv << 5 & 0x7fe0
uint exp_bits2(f16vec2 s) {
#ifdef PROF_NOEXP
    return packFloat2x16(s * SCORE_SCALE);   // timing only
#endif
#ifdef LT_EXP
    // the clamp by the fma's clamp bit, z back by a second fma: one operation less (7% of the yv move by 1 ulp, FASE22)
    const float16_t ws = 2.0hf * SCORE_SCALE / (4.0hf * CAP);
    precise f16vec2 w = min(max(fma(s, f16vec2(ws), f16vec2(0.5hf)), f16vec2(0.0hf)), f16vec2(1.0hf));
    precise f16vec2 zl = fma(w, f16vec2(4.0hf * CAP), f16vec2(-2.0hf * CAP));
    precise f16vec2 tl = fma(zl, -zl, f16vec2(4.0hf * POLY_A));
    precise f16vec2 yl = fma(zl, tl, f16vec2(8.0hf * POLY_B));
    return pack32(unpack16(packFloat2x16(yl)) << 5us);
#endif
    precise f16vec2 z = clamp(s * (2.0hf * SCORE_SCALE), -2.0hf * CAP, 2.0hf * CAP);
    precise f16vec2 t = fma(z, -z, f16vec2(4.0hf * POLY_A));
    precise f16vec2 yv = fma(z, t, f16vec2(8.0hf * POLY_B));
    return pack32(unpack16(packFloat2x16(yv)) << 5us);
}

f16vec2 gelu2(f16vec2 x) {
#ifdef PROF_NOGELU
    return x * 0.5hf;   // timing only
#endif
    // same bits as clamping x to [-2, 2]: on |x| <= 2 nothing changes, past it u is 0.25 and the clamp of g to [0, 1]
    // (the fma's clamp bit) gives the 0 or 1 the clamped x gives. Written as min / max: clamp() is split into two
    // scalar v_max ... clamp
    f16vec2 m = min(abs(x), f16vec2(2.0hf));
    precise f16vec2 u = fma(-m, f16vec2(float16_t(0.08108)), f16vec2(GELU_A));
    precise f16vec2 g = min(max(fma(x, u, f16vec2(0.5hf)), f16vec2(0.0hf)), f16vec2(1.0hf));
    precise f16vec2 r = x * g;
    return r;
}

#if ENC_EMBED && ENC_F32 && !W1
#error "enc0_attr_f writes the dithered 64-byte record (ATTR64), which only the W1 embed reads"
#endif
#if W1
#include "onr_block_w1.glsl"
#elif W2
#include "onr_block_w2.glsl"
#else
#if FLOW
#include "onr_flow.glsl"
void block_main(ivec2 win);
void main() {
    ivec2 win = ivec2(int(gl_WorkGroupID.x) % pc.nwx, int(gl_WorkGroupID.x) / pc.nwx);
    flow_wait(win);
    block_main(win);
    flow_signal(win);
}
void block_main(ivec2 win) {
#else
void main() {
    ivec2 win = ivec2(int(gl_WorkGroupID.x) % pc.nwx, int(gl_WorkGroupID.x) / pc.nwx);
#endif
#if DEC_HEAD
    // the head writes rows below out_h only: a window of padding rows has nothing to do
    if (8 * win.y - pc.shift.y >= pc.out_h) return;
#endif
    uint lid = gl_LocalInvocationIndex, s = gl_SubgroupID, ln = gl_SubgroupInvocationID;
    // SPLIT 2: subgroups s and s + 4 share the row tile s & 3 and split the output tiles of every product
    uint hf = SPLIT == 1 ? 0u : s >> 2, rt = s & 3u;
    uint r0 = rt * 16u, pr = PR + rt * PRS;
    float16_t ls1 = uint16BitsToFloat16(uint16_t(pc.ls & 0xffffu));
    float16_t ls2 = uint16BitsToFloat16(uint16_t(pc.ls >> 16));
#if !ENC_F32
    MA mlo, mhi;
    coopMatLoad(mlo, pc.mask.v, 0u, 16u, ROW);
    coopMatLoad(mhi, pc.mask.v, 256u, 16u, ROW);
#if TR
    MB mblo, mbhi;
    coopMatLoad(mblo, pc.mask.v, 0u, 16u, COL);
    coopMatLoad(mbhi, pc.mask.v, 256u, 16u, COL);
#endif
#endif
    U32View xw = U32View(uint64_t(pc.x));
#if TR
    uint hh = ln >> 4;
    bool town;
    ivec2 tp = token_pixel(int(r0 + (ln & 15u)), win, town);
#endif

    // ---------------- prologue: the 64 tokens x (fp16, GPU channel order) in XS
#if ENC_MERGE_IN
    // rr2_enc1: token = conv 2x2 stride 2 of the enc0 planes at the reflected pixel; A [64][4 CIN] in QV,
    // K = (2 ky + kx) CIN + c; each subgroup then does its own 16 rows
    // 16 record bytes per access, every load of the thread issued before the first is used
#define MI_N (16u * ENC_CIN / NTH)
    uvec4 mw[MI_N];
    [[unroll]] for (uint it = 0u; it < MI_N; ++it) {
        uint i = lid + it * NTH, t = i / (ENC_CIN / 4u), k16 = (i % (ENC_CIN / 4u)) * 16u, q = k16 / ENC_CIN, a = k16 % ENC_CIN;
        bool own;
        ivec2 p = 2 * token_pixel(int(t), win, own) + ivec2(q & 1u, q >> 1);
        mw[it] = U32x4View(uint64_t(pc.x)).v[plane_index_sz(p, int(a), pc.in_size) >> 4];
    }
    [[unroll]] for (uint it = 0u; it < MI_N; ++it) {
        uint i = lid + it * NTH, t = i / (ENC_CIN / 4u), k16 = (i % (ENC_CIN / 4u)) * 16u, q = k16 / ENC_CIN, a = k16 % ENC_CIN;
        put16(QV + t * 4u * ENC_CIN + q * ENC_CIN, a, mw[it]);
    }
    WG_SYNC();
    {
#define DECL_MI(cn, n) ACC cn = bias_acc(pc.mrg_b, ENC_CO + (n) * 16u);
        FOR_NT(DECL_MI)
        for (uint k = 0u; k < 4u * ENC_CIN; k += 16u) {
            MA alo, ahi;
            ALOAD(alo, ahi, QV + r0 * 4u * ENC_CIN + k, 4u * ENC_CIN)
#define STEP_MI(cn, n) GSTEP(cn, alo, ahi, pc.mrg_w.v, ENC_CO * 4u * ENC_C + (n) * 16u * 4u * ENC_CIN + k, 4u * ENC_CIN)
            FOR_NT(STEP_MI)
        }
#define STORE_X(cn, n) coopMatStore(MH(cn), lds, XS + r0 * ENC_C + (n) * 16u, ENC_C, ROW);
        FOR_NT(STORE_X)
    }
    WG_SYNC();
#elif DEC_UP
    // decoder _0: A = the 4x4 low-res pixels of the window (mirrored), U_pos = A up[pos]^T + b in QV [4][16][C],
    // then z = fma(skip, s, U) per token. Shifts are even, so the window covers whole low-res pixels.
    for (uint i = lid; i < ENC_CP; i += NTH) {
        uint r = i / (ENC_CP / 16u), a = (i % (ENC_CP / 16u)) * 16u;
        ivec2 q = (8 * win - pc.shift) / 2 + ivec2(r & 3u, r >> 2);
        q = ivec2(reflect_i(q.x, pc.lo_size.x), reflect_i(q.y, pc.lo_size.y));
        uvec4 w = U32x4View(uint64_t(pc.xlo)).v[plane_index_sz(q, int(a), pc.lo_size) >> 4];
        put16(UPLO + r * ENC_CP, a, w);
    }
    WG_SYNC();
#ifndef PROF_NOUP
    for (uint tile = s; tile < 4u * NT; tile += NSG) {
        uint pos = tile / NT, nt = tile % NT;
        ACC acc = bias_acc(pc.up_b, pos * ENC_C + nt * 16u);
        for (uint k = 0u; k < ENC_CP; k += 16u) {
            MA alo, ahi;
            ALOAD(alo, ahi, UPLO + k, ENC_CP)
            GSTEP(acc, alo, ahi, pc.up_w.v, (pos * ENC_C + nt * 16u) * ENC_CP + k, ENC_CP)
        }
        coopMatStore(MH(acc), lds, UPU + pos * 16u * ENC_C + nt * 16u, ENC_C, ROW);
    }
#endif
    WG_SYNC();
#if TR
    // x^T = fma(skip, s, U) per lane: the skip's record bytes as in XIN, U of the token from QV
        float16_t sc = uint16BitsToFloat16(uint16_t(pc.s_skip));
        uint t_ = r0 + (ln & 15u), ub_ = UPU + ((((t_ >> 3) & 1u) << 1) + (t_ & 1u)) * 16u * ENC_C + (((t_ >> 4) << 2) + ((t_ & 7u) >> 1)) * ENC_C + 8u * hh;
#define XUP(xa, xb, pl) MH xa, xb; { uvec4 w_ = U32x4View(uint64_t(pc.x)).v[plane_index(tp, int(pl) * 32 + int(hh) * 16) >> 4]; \
        uint ua_ = (ub_ + (pl) * 32u) >> 1, ub2_ = ua_ + 8u; \
        [[unroll]] for (int k_ = 0; k_ < 4; ++k_) { \
            f16vec2 a_ = unpackFloat2x16(e4m3_lo(w_[k_])), b_ = unpackFloat2x16(e4m3_hi(w_[k_])); \
            f16vec2 ua = unpackFloat2x16(lds1[ua_ + uint(k_)]), ubv = unpackFloat2x16(lds1[ub2_ + uint(k_)]); \
            precise f16vec2 za = fma(a_, f16vec2(sc), ua), zb = fma(b_, f16vec2(sc), ubv); \
            xa[2 * k_] = za.x, xa[2 * k_ + 1] = za.y, xb[2 * k_] = zb.x, xb[2 * k_ + 1] = zb.y; } }
        XUP(x0, x1, 0u)
#if NT > 2
        XUP(x2, x3, 1u)
#endif
#if NT > 4
        XUP(x4, x5, 2u)
#endif
#if NT > 6
        XUP(x6, x7, 3u)
#endif
#if NT > 8
        XUP(x8, x9, 4u)
#endif
#else
    {
        float16_t sc = uint16BitsToFloat16(uint16_t(pc.s_skip));
        for (uint i = lid; i < 16u * ENC_C; i += NTH) {
            uint t = i / (ENC_C / 4u), a = (i % (ENC_C / 4u)) * 4u, g = arena_pos(a);
            uint r = ((t >> 4) << 2) + ((t & 7u) >> 1), pos = (((t >> 3) & 1u) << 1) + (t & 1u);
            bool own;
            ivec2 p = token_pixel(int(t), win, own);
            uint w = xw.v[plane_index(p, int(a)) >> 2], ub = QV + pos * 16u * ENC_C + r * ENC_C + g;
            precise float16_t z0 = fma(e4m3_in(w & 255u), sc, lds[ub]);
            precise float16_t z1 = fma(e4m3_in((w >> 8) & 255u), sc, lds[ub + 1u]);
            precise float16_t z2 = fma(e4m3_in((w >> 16) & 255u), sc, lds[ub + 16u]);
            precise float16_t z3 = fma(e4m3_in(w >> 24), sc, lds[ub + 17u]);
            uint xb = XS + t * ENC_C + g;
            lds[xb] = z0, lds[xb + 1u] = z1, lds[xb + 16u] = z2, lds[xb + 17u] = z3;
        }
    }
    WG_SYNC();
#endif
#elif ENC_EMBED
    // rr2_enc0: own tokens' 43 attributes, dithered to 3 mantissa bits (blue-noise LUT of the padded pixel),
    // A [16][64] in PR; h = gelu(A fc1^T + b) [16][64] back over A; x = h fc2^T + b [16][32] into XS
    // lane ln: attributes k4 .. k4 + 3 of tokens ln / 16 + 2 it; all loads issued before any is used
    uint k4 = (ln & 15u) * 4u;
    f16vec4 h4s[8];
    uint ns[8];
    {
        F16x4Buf av = F16x4Buf(uint64_t(pc.attr));
        uint lb = pc.frame8 * 256u;
        [[unroll]] for (uint it = 0u; it < 8u; ++it) {
            bool own;
            ivec2 p = token_pixel(int(r0 + (ln >> 4) + 2u * it), win, own);
            int sy = p.y < pc.img_h ? p.y : 2 * pc.img_h - 2 - p.y, sx = p.x < pc.img_w ? p.x : 2 * pc.img_w - 2 - p.x;
            ns[it] = uint(pc.lut.v[lb + uint((p.y & 15) * 16 + (p.x & 15))]);
            h4s[it] = f16vec4(0.0hf);
            if (k4 < 44u) h4s[it] = av.v[(uint(sy * pc.img_w + sx) * 48u + k4) >> 2];
        }
    }
    // two attributes per packed op; zeros stay +0 (n < 0x80), attribute 43 is cleared
    [[unroll]] for (uint it = 0u; it < 8u; ++it) {
        uint tl = (ln >> 4) + 2u * it, n = ns[it] >> 9;
        n |= n << 16;
        uvec2 w;
        [[unroll]] for (uint j = 0u; j < 2u; ++j) {
            f16vec2 h = j == 0u ? h4s[it].xy : h4s[it].zw;
            uint m = packFloat2x16(h) & 0xfc00fc00u;
            precise f16vec2 tt = h + unpackFloat2x16(m | n);
            precise f16vec2 r = tt - unpackFloat2x16(m);
            w[j] = packFloat2x16(r) & 0xff80ff80u;
        }
        if (k4 == 40u) w.y &= 0xffffu;
        lds2[(pr + tl * SST + k4) >> 2] = w;
    }
    SG_SYNC();
#if TR
    // transposed: h^T = gelu(fc1 A^T + b) and x^T = fc2 h^T + b stay in registers (x0, x1 are the x^T tiles)
    MH x0, x1;
    {
#if FP8PROTO && ENC_F32
#define BLOAD(blo, bhi, off) { MB b_; coopMatLoad(b_, lds, off, SST, COL); blo = MB8(b_); }
#elif ENC_F32
#define BLOAD(blo, bhi, off) { coopMatLoad(blo, lds, off, SST, COL); }
#else
#define BLOAD(blo, bhi, off) { MB b_; coopMatLoad(b_, lds, off, SST, COL); blo = b_ * mblo; bhi = b_ * mbhi; }
#endif
        MBP b0l, b0h, b1l, b1h, b2l, b2h, b3l, b3h;
        BLOAD(b0l, b0h, pr) BLOAD(b1l, b1h, pr + 16u) BLOAD(b2l, b2h, pr + 32u)
        ACC e0 = bias_acc_t(pc.mrg_b, 0u), e1 = bias_acc_t(pc.mrg_b, 16u), e2 = bias_acc_t(pc.mrg_b, 32u), e3 = bias_acc_t(pc.mrg_b, 48u);
        // K = 48: attributes 43..63 are zeros, the fourth step would add nothing
#define EMB1T(e, n) WSTEP(e, pc.mrg_w.v, (n) * 1024u, 64u, b0l, b0h) WSTEP(e, pc.mrg_w.v, (n) * 1024u + 16u, 64u, b1l, b1h)         WSTEP(e, pc.mrg_w.v, (n) * 1024u + 32u, 64u, b2l, b2h)
        EMB1T(e0, 0u) EMB1T(e1, 1u) EMB1T(e2, 2u) EMB1T(e3, 3u)
#define GELU_E(e, bl, bh) { MH g = MH(e); for (int i = 0; i < g.length(); i += 2) { f16vec2 q = gelu2(f16vec2(g[i], g[i + 1])); g[i] = q.x, g[i + 1] = q.y; } BPREP(bl, bh, g) }
        GELU_E(e0, b0l, b0h) GELU_E(e1, b1l, b1h) GELU_E(e2, b2l, b2h) GELU_E(e3, b3l, b3h)
        ACC f0 = bias_acc_t(pc.mrg_b, 64u), f1 = bias_acc_t(pc.mrg_b, 80u);
#define EMB2T(e, n) WSTEP(e, pc.mrg_w.v, 4096u + (n) * 1024u, 64u, b0l, b0h) WSTEP(e, pc.mrg_w.v, 4096u + (n) * 1024u + 16u, 64u, b1l, b1h)         WSTEP(e, pc.mrg_w.v, 4096u + (n) * 1024u + 32u, 64u, b2l, b2h) WSTEP(e, pc.mrg_w.v, 4096u + (n) * 1024u + 48u, 64u, b3l, b3h)
        EMB2T(f0, 0u) EMB2T(f1, 1u)
        x0 = MH(f0), x1 = MH(f1);
    }
#else
    {
        MA a0l, a0h, a1l, a1h, a2l, a2h, a3l, a3h;
        ALOAD(a0l, a0h, pr, SST) ALOAD(a1l, a1h, pr + 16u, SST) ALOAD(a2l, a2h, pr + 32u, SST) ALOAD(a3l, a3h, pr + 48u, SST)
        ACC e0 = bias_acc(pc.mrg_b, 0u), e1 = bias_acc(pc.mrg_b, 16u), e2 = bias_acc(pc.mrg_b, 32u), e3 = bias_acc(pc.mrg_b, 48u);
#define EMB1(e, n) GSTEP(e, a0l, a0h, pc.mrg_w.v, (n) * 1024u, 64u) GSTEP(e, a1l, a1h, pc.mrg_w.v, (n) * 1024u + 16u, 64u) \
        GSTEP(e, a2l, a2h, pc.mrg_w.v, (n) * 1024u + 32u, 64u) GSTEP(e, a3l, a3h, pc.mrg_w.v, (n) * 1024u + 48u, 64u)
        EMB1(e0, 0u) EMB1(e1, 1u) EMB1(e2, 2u) EMB1(e3, 3u)
        SG_SYNC();
#define GELU_ST(e, n) { MH h = MH(e); for (int i = 0; i < h.length(); ++i) h[i] = gelu_poly(h[i]); coopMatStore(h, lds, pr + (n) * 16u, SST, ROW); }
        GELU_ST(e0, 0u) GELU_ST(e1, 1u) GELU_ST(e2, 2u) GELU_ST(e3, 3u)
        SG_SYNC();
        ALOAD(a0l, a0h, pr, SST) ALOAD(a1l, a1h, pr + 16u, SST) ALOAD(a2l, a2h, pr + 32u, SST) ALOAD(a3l, a3h, pr + 48u, SST)
        ACC x0 = bias_acc(pc.mrg_b, 64u), x1 = bias_acc(pc.mrg_b, 80u);
#define EMB2(e, n) GSTEP(e, a0l, a0h, pc.mrg_w.v, 4096u + (n) * 1024u, 64u) GSTEP(e, a1l, a1h, pc.mrg_w.v, 4096u + (n) * 1024u + 16u, 64u) \
        GSTEP(e, a2l, a2h, pc.mrg_w.v, 4096u + (n) * 1024u + 32u, 64u) GSTEP(e, a3l, a3h, pc.mrg_w.v, 4096u + (n) * 1024u + 48u, 64u)
        EMB2(x0, 0u) EMB2(x1, 1u)
        coopMatStore(MH(x0), lds, XS + r0 * 32u, 32u, ROW);
        coopMatStore(MH(x1), lds, XS + r0 * 32u + 16u, 32u, ROW);
    }
    SG_SYNC();
#endif
#elif XREG_IN
    // own tokens, e4m3 -> fp16, one 16-byte load per lane and plane into the x^T tiles
#define XIN(xa, xb, pl) MH xa, xb; { uvec4 w_ = U32x4View(uint64_t(pc.x)).v[plane_index(tp, int(pl) * 32 + int(hh) * 16) >> 4]; \
        [[unroll]] for (int k_ = 0; k_ < 4; ++k_) { \
            f16vec2 a_ = unpackFloat2x16(e4m3_lo(w_[k_])), b_ = unpackFloat2x16(e4m3_hi(w_[k_])); \
            xa[2 * k_] = a_.x, xa[2 * k_ + 1] = a_.y, xb[2 * k_] = b_.x, xb[2 * k_ + 1] = b_.y; } }
#if FP8QV
    // and the same bytes as e4m3 B tiles (OCP codes)
#define XIN8(xa, xb, ya, yb, pl) XIN(xa, xb, pl) QB8 ya, yb; { uvec4 w_ = U32x4View(uint64_t(pc.x)).v[plane_index(tp, int(pl) * 32 + int(hh) * 16) >> 4];         [[unroll]] for (int k_ = 0; k_ < 4; ++k_) { uint o_ = ocp4(w_[k_]);             ya[2 * k_] = uintBitsToFloate4m3EXT(uint8_t(o_ & 0xffu)), ya[2 * k_ + 1] = uintBitsToFloate4m3EXT(uint8_t((o_ >> 8) & 0xffu));             yb[2 * k_] = uintBitsToFloate4m3EXT(uint8_t((o_ >> 16) & 0xffu)), yb[2 * k_ + 1] = uintBitsToFloate4m3EXT(uint8_t(o_ >> 24)); } }
#else
#define XIN8(xa, xb, ya, yb, pl) XIN(xa, xb, pl)
#endif
    XIN8(x0, x1, y0, y1, 0u)
#if NT > 2
    XIN8(x2, x3, y2, y3, 1u)
#endif
#if NT > 4
    XIN8(x4, x5, y4, y5, 2u)
#endif
#if NT > 6
    XIN8(x6, x7, y6, y7, 3u)
#endif
#if NT > 8
    XIN8(x8, x9, y8, y9, 4u)
#endif
#else
    // own tokens, e4m3 -> fp16
    for (uint i = ln + 32u * hf; i < 16u * (ENC_C / 16u); i += 32u * SPLIT) {
        uint tl = i / (ENC_C / 16u), a = (i % (ENC_C / 16u)) * 16u;
        bool own;
        ivec2 p = token_pixel(int(r0 + tl), win, own);
        uvec4 w = U32x4View(uint64_t(pc.x)).v[plane_index(p, int(a)) >> 4];
        put16(XS + (r0 + tl) * ENC_C, a, w);
    }
    RT_SYNC();
#endif
    CUT(1)

#if TR
    // x^T of the own rows: NT tiles [16 channels][16 rows] in registers
#define LOAD_XT(cn, xn, n) MH xn; coopMatLoad(xn, lds, XS + r0 * ENC_C + (n) * 16u, ENC_C, COL);
#if !ENC_EMBED && !XREG_IN
    FOR_NX(LOAD_XT)
#endif
#if SPLIT == 2
#define DECL_OUT_T(cn, i) ACC cn = bias_acc_t(pc.out_b, TO(i) * 16u);
    FOR_NO(DECL_OUT_T)
#else
#define DECL_OUT_T(cn, xn, n) ACC cn = bias_acc_t(pc.out_b, (n) * 16u);
    FOR_NX(DECL_OUT_T)
#endif
    HD_UNROLL for (uint hd = 0u; hd < LT_HEADS; ++hd) {
        // (the enc0 prologue scratch aliases QV). QV_BUFS 2: head h writes buffer h & 1, whose last readers (head
        // h - 2) all passed the barrier of head h - 1
        if ((QV_BUFS == 1 && hd > 0u) || (hd == 0u && (ENC_EMBED != 0 || (DEC_UP != 0 && XREG_IN)))) WG_SYNC();
        uint qb = QV + (QV_BUFS == 1 ? 0u : (hd & 1u) * QVB), vb = qb + 64u * QST;
        // Q_h^T, V_h^T [32 dims][16 rows] = in_a/in_b rows of the head x x^T; K = channels in tile order
#if SPLIT == 2
        // half 0: Q_h, half 1: V_h; half 1 reads its own Q_h back after the barrier
        MH qh0, qh1;
        {
#if FP8QV
            uint64_t w8 = (hf == 0u ? uint64_t(pc.in_a) : uint64_t(pc.in_b)) + uint64_t(QV8_OFF);
            QC8 a0 = QC8(0.0), a1 = QC8(0.0);
#define QV_T8(yn, n) W8STEP(a0, w8, hd * 32u * ENC_C + (n) * 16u, yn) W8STEP(a1, w8, (hd * 32u + 16u) * ENC_C + (n) * 16u, yn)
            FOR_NY(QV_T8)
#else
            F16Buf wqv = F16Buf(hf == 0u ? uint64_t(pc.in_a) : uint64_t(pc.in_b));
            ACC a0 = ACC(0.0), a1 = ACC(0.0);
#define QV_T2(cn, xn, n) { MBP blo, bhi; BPREP(blo, bhi, xn)             WSTEP(a0, wqv.v, hd * 32u * ENC_C + (n) * 16u, ENC_C, blo, bhi) WSTEP(a1, wqv.v, (hd * 32u + 16u) * ENC_C + (n) * 16u, ENC_C, blo, bhi) }
            FOR_NX(QV_T2)
#endif
            qh0 = MH(a0), qh1 = MH(a1);
            if (hf == 0u) {
                coopMatStore(qh0, lds, qb + r0 * QST, QST, COL);
                coopMatStore(qh1, lds, qb + r0 * QST + 16u, QST, COL);
            } else {
                coopMatStore(qh0, lds, vb + r0, VST, ROW);
                coopMatStore(qh1, lds, vb + 16u * VST + r0, VST, ROW);
            }
        }
#else
#if FP8QV
        QC8 q0 = QC8(0.0), q1 = QC8(0.0), v0 = QC8(0.0), v1 = QC8(0.0);
        uint64_t wa8 = uint64_t(pc.in_a) + uint64_t(QV8_OFF), wb8 = uint64_t(pc.in_b) + uint64_t(QV8_OFF);
#define QV_T8(yn, n) W8STEP(q0, wa8, hd * 32u * ENC_C + (n) * 16u, yn) W8STEP(q1, wa8, (hd * 32u + 16u) * ENC_C + (n) * 16u, yn)             W8STEP(v0, wb8, hd * 32u * ENC_C + (n) * 16u, yn) W8STEP(v1, wb8, (hd * 32u + 16u) * ENC_C + (n) * 16u, yn)
        FOR_NY(QV_T8)
#else
        ACC q0 = ACC(0.0), q1 = ACC(0.0), v0 = ACC(0.0), v1 = ACC(0.0);
#define QV_T(cn, xn, n) { MBP blo, bhi; BPREP(blo, bhi, xn) \
            WSTEP(q0, pc.in_a.v, hd * 32u * ENC_C + (n) * 16u, ENC_C, blo, bhi) WSTEP(q1, pc.in_a.v, (hd * 32u + 16u) * ENC_C + (n) * 16u, ENC_C, blo, bhi) \
            WSTEP(v0, pc.in_b.v, hd * 32u * ENC_C + (n) * 16u, ENC_C, blo, bhi) WSTEP(v1, pc.in_b.v, (hd * 32u + 16u) * ENC_C + (n) * 16u, ENC_C, blo, bhi) }
#if VROW
        // V_h not transposed: x as the A operand (the x^T tile copied), W_v^T as B; V [rows][dims] stored COL is V_h^T
#if ENC_F32
#define XA(alo, ahi, xn) { for (int i_ = 0; i_ < xn.length(); ++i_) alo[i_] = xn[i_]; }
#else
#define XA(alo, ahi, xn) { MA a_; for (int i_ = 0; i_ < xn.length(); ++i_) a_[i_] = xn[i_]; alo = a_ * mlo; ahi = a_ * mhi; }
#endif
#undef QV_T
#define QV_T(cn, xn, n) { MBP blo, bhi; BPREP(blo, bhi, xn) \
            WSTEP(q0, pc.in_a.v, hd * 32u * ENC_C + (n) * 16u, ENC_C, blo, bhi) WSTEP(q1, pc.in_a.v, (hd * 32u + 16u) * ENC_C + (n) * 16u, ENC_C, blo, bhi) \
            MA alo, ahi; XA(alo, ahi, xn) \
            GSTEP(v0, alo, ahi, pc.in_b.v, hd * 32u * ENC_C + (n) * 16u, ENC_C) GSTEP(v1, alo, ahi, pc.in_b.v, (hd * 32u + 16u) * ENC_C + (n) * 16u, ENC_C) }
#endif
        FOR_NX(QV_T)
#endif
        MH qh0 = MH(q0), qh1 = MH(q1);
        coopMatStore(qh0, lds, qb + r0 * QST, QST, COL);
        coopMatStore(qh1, lds, qb + r0 * QST + 16u, QST, COL);
        // V_h^T [32 dims][VST], so the A operand of P V loads by rows
#if VROW && !FP8QV
        coopMatStore(MH(v0), lds, vb + r0, VST, COL);
        coopMatStore(MH(v1), lds, vb + 16u * VST + r0, VST, COL);
#else
        coopMatStore(MH(v0), lds, vb + r0, VST, ROW);
        coopMatStore(MH(v1), lds, vb + 16u * VST + r0, VST, ROW);
#endif
#endif
        // S^T [64 keys][16 rows] = Q_keys Q_own^T + bias^T, four key tiles
        MH bh;
        coopMatLoad(bh, pc.bias.v, hd * 4096u + r0 * 64u, 64u, COL);
        ACC s0 = ACC(bh);
        coopMatLoad(bh, pc.bias.v, hd * 4096u + r0 * 64u + 16u, 64u, COL);
        ACC s1 = ACC(bh);
        coopMatLoad(bh, pc.bias.v, hd * 4096u + r0 * 64u + 32u, 64u, COL);
        ACC s2 = ACC(bh);
        coopMatLoad(bh, pc.bias.v, hd * 4096u + r0 * 64u + 48u, 64u, COL);
        ACC s3 = ACC(bh);
        WG_SYNC();
#if SPLIT == 2
        if (hf != 0u) {
            coopMatLoad(qh0, lds, qb + r0 * QST, QST, COL);
            coopMatLoad(qh1, lds, qb + r0 * QST + 16u, QST, COL);
        }
#endif
        {
            MBP blo, bhi;
            BPREP(blo, bhi, qh0)
            IFK(rt, 0u) { WSTEP(s0, lds, qb, QST, blo, bhi) } IFK(rt, 1u) { WSTEP(s1, lds, qb + 16u * QST, QST, blo, bhi) }
            IFK(rt, 2u) { WSTEP(s2, lds, qb + 32u * QST, QST, blo, bhi) } IFK(rt, 3u) { WSTEP(s3, lds, qb + 48u * QST, QST, blo, bhi) }
            BPREP(blo, bhi, qh1)
            IFK(rt, 0u) { WSTEP(s0, lds, qb + 16u, QST, blo, bhi) } IFK(rt, 1u) { WSTEP(s1, lds, qb + 16u * QST + 16u, QST, blo, bhi) }
            IFK(rt, 2u) { WSTEP(s2, lds, qb + 32u * QST + 16u, QST, blo, bhi) } IFK(rt, 3u) { WSTEP(s3, lds, qb + 48u * QST + 16u, QST, blo, bhi) }
        }
        // softmax in registers. Lane (h, row) holds keys 16 kt + 8 h + e of tile kt, element e: column class e,
        // group a = 2 kt + h. Per class: ((a0 + a2) + (a4 + a6)) + ((a1 + a3) + (a5 + a7)), the even groups in
        // lane half 0; then (P0 + P2) + (P4 + P6) and (P1 + P3) + (P5 + P7), as the row body
        MH p0 = MH(s0), p1 = MH(s1), p2 = MH(s2), p3 = MH(s3);
#ifndef PROF_NOSM
        f16vec2 e2[4];
        [[unroll]] for (int e = 0; e < 8; e += 2) {
            f16vec2 t0 = TK(rt, 0u, unpackFloat2x16(exp_bits2(f16vec2(p0[e], p0[e + 1]))));
            f16vec2 t1 = TK(rt, 1u, unpackFloat2x16(exp_bits2(f16vec2(p1[e], p1[e + 1]))));
            f16vec2 t2 = TK(rt, 2u, unpackFloat2x16(exp_bits2(f16vec2(p2[e], p2[e + 1]))));
            f16vec2 t3 = TK(rt, 3u, unpackFloat2x16(exp_bits2(f16vec2(p3[e], p3[e + 1]))));
            p0[e] = t0.x, p0[e + 1] = t0.y, p1[e] = t1.x, p1[e + 1] = t1.y;
            p2[e] = t2.x, p2[e + 1] = t2.y, p3[e] = t3.x, p3[e + 1] = t3.y;
            precise f16vec2 sm = (t0 + t1) + (t2 + t3);
            e2[e >> 1] = sm;
        }
        f16vec2 pc2[4];
        [[unroll]] for (int i = 0; i < 4; ++i) {
            f16vec2 o = unpackFloat2x16(subgroupShuffleXor(packFloat2x16(e2[i]), 16u));
            precise f16vec2 sp = e2[i] + o;
            pc2[i] = sp;
        }
        precise f16vec2 b2_ = (pc2[0] + pc2[1]) + (pc2[2] + pc2[3]);
        precise float16_t sum = b2_.x + b2_.y;
        float16_t r = RCP_SUM(sum);
        [[unroll]] for (int e = 0; e < 8; e += 2) {
            precise f16vec2 u0 = f16vec2(p0[e], p0[e + 1]) * r, u1 = f16vec2(p1[e], p1[e + 1]) * r;
            precise f16vec2 u2 = f16vec2(p2[e], p2[e + 1]) * r, u3 = f16vec2(p3[e], p3[e + 1]) * r;
            p0[e] = u0.x, p0[e + 1] = u0.y, p1[e] = u1.x, p1[e + 1] = u1.y;
            p2[e] = u2.x, p2[e + 1] = u2.y, p3[e] = u3.x, p3[e + 1] = u3.y;
        }
#endif
        // O_h^T [32 dims][16 rows] = V_h^T P^T, K = keys
        ACC o0 = ACC(0.0), o1 = ACC(0.0);
#define PV_T(p, kt) { MBP blo, bhi; BPREP(blo, bhi, p) MA a_; \
            coopMatLoad(a_, lds, vb + (kt) * 16u, VST, ROW); TSTEP(o0, a_, blo, bhi) \
            coopMatLoad(a_, lds, vb + 16u * VST + (kt) * 16u, VST, ROW); TSTEP(o1, a_, blo, bhi) }
        IFK(rt, 0u) PV_T(p0, 0u) IFK(rt, 1u) PV_T(p1, 1u) IFK(rt, 2u) PV_T(p2, 2u) IFK(rt, 3u) PV_T(p3, 3u)
        // out projection, K = 32 hd + dims
        {
            MBP blo, bhi;
            MH oh = MH(o0);
            BPREP(blo, bhi, oh)
#if SPLIT == 2
#define OUT_T0(cn, i) WSTEP(cn, pc.wout.v, WT_OUT(TO(i) * 16u * ENC_X + hd * 32u), WS_OUT, blo, bhi)
            FOR_NO(OUT_T0)
            oh = MH(o1);
            BPREP(blo, bhi, oh)
#define OUT_T1(cn, i) WSTEP(cn, pc.wout.v, WT_OUT(TO(i) * 16u * ENC_X + hd * 32u + 16u), WS_OUT, blo, bhi)
            FOR_NO(OUT_T1)
#else
#define OUT_T0(cn, xn, n) WSTEP(cn, pc.wout.v, WT_OUT((n) * 16u * ENC_X + hd * 32u), WS_OUT, blo, bhi)
            FOR_NX(OUT_T0)
            oh = MH(o1);
            BPREP(blo, bhi, oh)
#define OUT_T1(cn, xn, n) WSTEP(cn, pc.wout.v, WT_OUT((n) * 16u * ENC_X + hd * 32u + 16u), WS_OUT, blo, bhi)
            FOR_NX(OUT_T1)
#endif
        }
    }
    CUT(3)
    // x1 = fma(O out^T + b, ls1, x)
#define RESID_T(cn, xn, l) { MH a16 = MH(cn); for (int i = 0; i < a16.length(); i += 2) { \
        precise f16vec2 v = fma(f16vec2(a16[i], a16[i + 1]), f16vec2(l), f16vec2(xn[i], xn[i + 1])); xn[i] = v.x, xn[i + 1] = v.y; } }
#if SPLIT == 2
    // own output tiles -> XS, then every tile back from both halves: x1 = fma(O out^T + b, ls1, x)
#define RESID_X(cn, xn, n, l) { MH a16; coopMatLoad(a16, lds, EXB(n), 16u, ROW); for (int i = 0; i < a16.length(); i += 2) {         precise f16vec2 v = fma(f16vec2(a16[i], a16[i + 1]), f16vec2(l), f16vec2(xn[i], xn[i + 1])); xn[i] = v.x, xn[i + 1] = v.y; } }
    FOR_NO(EX_ST)
    WG_SYNC();
#define X1_X(cn, xn, n) RESID_X(cn, xn, n, ls1)
    FOR_NX(X1_X)
    CUT(4)
#define FC2B_T(cn, i) cn = bias_acc_t(pc.fc2_b, TO(i) * 16u);
    FOR_NO(FC2B_T)
    // 32-neuron slices: half hf computes hidden tile j + 16 hf, the two GELU tiles meet in a double buffer over
    // the keys (all key reads are behind the exchange barrier); fc2 of the own output tiles over both
    for (uint j = 0u; j < LT_HID; j += 32u) {
        uint jt = j + 16u * hf;
        ACC h0 = bias_acc_t(pc.fc1_b, jt);
#define FC1_T2(cn, xn, n) { MBP blo, bhi; BPREP(blo, bhi, xn) WSTEP(h0, pc.fc1.v, WT_FC1(jt * ENC_C + (n) * 16u), WS_FC1, blo, bhi) }
        FOR_NX(FC1_T2)
        MH g = MH(h0);
        for (int i = 0; i < g.length(); i += 2) { f16vec2 q = gelu2(f16vec2(g[i], g[i + 1])); g[i] = q.x, g[i + 1] = q.y; }
        uint hb = QV + ((j >> 5) & 1u) * 2048u + rt * 512u;
        coopMatStore(g, lds, hb + hf * 256u, 16u, ROW);
        WG_SYNC();
        MBP b0l, b0h, b1l, b1h;
        {
            MH ga, gb;
            coopMatLoad(ga, lds, hb, 16u, ROW);
            coopMatLoad(gb, lds, hb + 256u, 16u, ROW);
            BPREP(b0l, b0h, ga) BPREP(b1l, b1h, gb)
        }
#define FC2_T2(cn, i) WSTEP(cn, pc.fc2.v, WT_FC2(TO(i) * 16u * HID + j), WS_FC2, b0l, b0h) WSTEP(cn, pc.fc2.v, WT_FC2(TO(i) * 16u * HID + j + 16u), WS_FC2, b1l, b1h)
        FOR_NO(FC2_T2)
    }
    FOR_NO(EX_ST)
    WG_SYNC();
#define X2_X(cn, xn, n) RESID_X(cn, xn, n, ls2)
    FOR_NX(X2_X)
#else
#define X1_T(cn, xn, n) RESID_T(cn, xn, ls1)
    FOR_NX(X1_T)
    CUT(4)
#define FC2B_T(cn, xn, n) cn = bias_acc_t(pc.fc2_b, (n) * 16u);
    FOR_NX(FC2B_T)
#if MLP64
#define SLICE 64u
#else
#define SLICE 32u
#endif
    J_UNROLL for (uint j = 0u; j < LT_HID; j += SLICE) {
        // hidden slice h^T [SLICE neurons][16 rows] = fc1 x1^T + b, GELU in registers
        ACC h0 = bias_acc_t(pc.fc1_b, j), h1 = bias_acc_t(pc.fc1_b, j + 16u);
#if MLP64
        ACC h2 = bias_acc_t(pc.fc1_b, j + 32u), h3 = bias_acc_t(pc.fc1_b, j + 48u);
#define FC1_T(cn, xn, n) { MBP blo, bhi; BPREP(blo, bhi, xn) \
            WSTEP(h0, pc.fc1.v, WT_FC1(j * ENC_C + (n) * 16u), WS_FC1, blo, bhi) WSTEP(h1, pc.fc1.v, WT_FC1((j + 16u) * ENC_C + (n) * 16u), WS_FC1, blo, bhi) \
            WSTEP(h2, pc.fc1.v, WT_FC1((j + 32u) * ENC_C + (n) * 16u), WS_FC1, blo, bhi) WSTEP(h3, pc.fc1.v, WT_FC1((j + 48u) * ENC_C + (n) * 16u), WS_FC1, blo, bhi) }
#else
#define FC1_T(cn, xn, n) { MBP blo, bhi; BPREP(blo, bhi, xn) \
            WSTEP(h0, pc.fc1.v, WT_FC1(j * ENC_C + (n) * 16u), WS_FC1, blo, bhi) WSTEP(h1, pc.fc1.v, WT_FC1((j + 16u) * ENC_C + (n) * 16u), WS_FC1, blo, bhi) }
#endif
        FOR_NX(FC1_T)
#define GELU_TT(h, g) MH g = MH(h); for (int i = 0; i < g.length(); i += 2) { f16vec2 q = gelu2(f16vec2(g[i], g[i + 1])); g[i] = q.x, g[i + 1] = q.y; }
        GELU_TT(h0, g0) GELU_TT(h1, g1)
        MBP b0l, b0h, b1l, b1h;
        BPREP(b0l, b0h, g0) BPREP(b1l, b1h, g1)
#if MLP64
        GELU_TT(h2, g2) GELU_TT(h3, g3)
        MBP b2l, b2h, b3l, b3h;
        BPREP(b2l, b2h, g2) BPREP(b3l, b3h, g3)
#define FC2_T(cn, xn, n) WSTEP(cn, pc.fc2.v, WT_FC2((n) * 16u * HID + j), WS_FC2, b0l, b0h) WSTEP(cn, pc.fc2.v, WT_FC2((n) * 16u * HID + j + 16u), WS_FC2, b1l, b1h) \
            WSTEP(cn, pc.fc2.v, WT_FC2((n) * 16u * HID + j + 32u), WS_FC2, b2l, b2h) WSTEP(cn, pc.fc2.v, WT_FC2((n) * 16u * HID + j + 48u), WS_FC2, b3l, b3h)
#else
#define FC2_T(cn, xn, n) WSTEP(cn, pc.fc2.v, WT_FC2((n) * 16u * HID + j), WS_FC2, b0l, b0h) WSTEP(cn, pc.fc2.v, WT_FC2((n) * 16u * HID + j + 16u), WS_FC2, b1l, b1h)
#endif
        FOR_NX(FC2_T)
    }
#define X2_T(cn, xn, n) RESID_T(cn, xn, ls2)
    FOR_NX(X2_T)
#endif
#if !DEC_HEAD
    // TensorOutput of the own tokens straight from the x^T tiles, one 16-byte store per lane and plane
#define XOUT(xa, xb, pl) if (town && (SPLIT == 1 || ((pl) & 1u) == hf)) { uvec4 w_; [[unroll]] for (int k_ = 0; k_ < 4; ++k_) \
        w_[k_] = e4m3x4(f16vec2(xa[2 * k_], xa[2 * k_ + 1]), f16vec2(xb[2 * k_], xb[2 * k_ + 1])); \
        U32x4View(uint64_t(pc.y)).v[plane_index(tp, int(pl) * 32 + int(hh) * 16) >> 4] = w_; }
    XOUT(x0, x1, 0u)
#if NT > 2
    XOUT(x2, x3, 1u)
#endif
#if NT > 4
    XOUT(x4, x5, 2u)
#endif
#if NT > 6
    XOUT(x6, x7, 3u)
#endif
#if NT > 8
    XOUT(x8, x9, 4u)
#endif
#endif
    // x2 back to the token rows for the merge and the output head
#if ENC_CO > 0 || DEC_HEAD
#if SPLIT == 2 || XS_ALIAS
    WG_SYNC();   // XS still holds the x2 exchange / the keys of the last head
#endif
#if XPERM
    // token (ty, tx) to row 16 q + r, q = 2 (ty & 1) + (tx & 1) the tap, r = 4 (ty >> 1) + (tx >> 1) the pooled pixel
    uint tk_ = r0 + (ln & 15u), xr_ = XS + (16u * (2u * ((tk_ >> 3) & 1u) + (tk_ & 1u)) + 4u * (tk_ >> 4) + ((tk_ & 7u) >> 1)) * ENC_C + 8u * hh;
#define STORE_XT(cn, xn, n) if (SPLIT == 1 || ((n) & 1u) == hf) lds4[(xr_ + (n) * 16u) >> 3] = uvec4(packFloat2x16(f16vec2(xn[0], xn[1])),         packFloat2x16(f16vec2(xn[2], xn[3])), packFloat2x16(f16vec2(xn[4], xn[5])), packFloat2x16(f16vec2(xn[6], xn[7])));
#else
#define STORE_XT(cn, xn, n) if (SPLIT == 1 || ((n) & 1u) == hf) coopMatStore(xn, lds, XS + r0 * ENC_C + (n) * 16u, ENC_C, COL);
#endif
    FOR_NX(STORE_XT)
#endif
#if ENC_STAGE || DEC_DFN
    WG_SYNC();   // the head scratch aliases the keys of the last head
#else
    RT_SYNC();
#endif
#else
    // ---------------- attention, head by head; the out projection accumulates over the heads (K = 32 h + k)
#define DECL_OUT(cn, n) ACC cn = bias_acc(pc.out_b, (n) * 16u);
#define STEP_OUT(cn, n) GSTEP(cn, alo, ahi, pc.wout.v, (n) * 16u * ENC_X + hd * 32u + k, ENC_X)
    FOR_NT(DECL_OUT)
    HD_UNROLL for (uint hd = 0u; hd < LT_HEADS; ++hd) {
#if QV_BUFS == 1
        if (hd > 0u) WG_SYNC();
        uint qb = QV, vb = qb + 2048u;
#else
        uint qb = QV + (hd & 1u) * 4096u, vb = qb + 2048u;
#endif
#if SPLIT == 2
        {
            // half 0: Q_h, half 1: V_h
            F16Buf wq = F16Buf(hf == 0u ? uint64_t(pc.in_a) : uint64_t(pc.in_b));
            ACC q0 = ACC(0.0), q1 = ACC(0.0);
            for (uint k = 0u; k < ENC_C; k += 16u) {
                MA alo, ahi;
                ALOAD(alo, ahi, XS + r0 * ENC_C + k, ENC_C)
                GSTEP(q0, alo, ahi, wq.v, hd * 32u * ENC_C + k, ENC_C)
                GSTEP(q1, alo, ahi, wq.v, (hd * 32u + 16u) * ENC_C + k, ENC_C)
            }
            uint ob = (hf == 0u ? qb : vb) + r0 * 32u;
            coopMatStore(MH(q0), lds, ob, 32u, ROW);
            coopMatStore(MH(q1), lds, ob + 16u, 32u, ROW);
        }
#else
        {
            ACC q0 = ACC(0.0), q1 = ACC(0.0), v0 = ACC(0.0), v1 = ACC(0.0);
            for (uint k = 0u; k < ENC_C; k += 16u) {
                MA alo, ahi;
                ALOAD(alo, ahi, XS + r0 * ENC_C + k, ENC_C)
                GSTEP(q0, alo, ahi, pc.in_a.v, hd * 32u * ENC_C + k, ENC_C)
                GSTEP(q1, alo, ahi, pc.in_a.v, (hd * 32u + 16u) * ENC_C + k, ENC_C)
                GSTEP(v0, alo, ahi, pc.in_b.v, hd * 32u * ENC_C + k, ENC_C)
                GSTEP(v1, alo, ahi, pc.in_b.v, (hd * 32u + 16u) * ENC_C + k, ENC_C)
            }
            coopMatStore(MH(q0), lds, qb + r0 * 32u, 32u, ROW);
            coopMatStore(MH(q1), lds, qb + r0 * 32u + 16u, 32u, ROW);
            coopMatStore(MH(v0), lds, vb + r0 * 32u, 32u, ROW);
            coopMatStore(MH(v1), lds, vb + r0 * 32u + 16u, 32u, ROW);
        }
#endif
        // S = Q_h Q_h^T + bias_h for the own rows -> PR (bias tiles fetched before the barrier)
#if SPLIT == 2
        {
            // score tiles 2 hf, 2 hf + 1 of the own rows
            uint n0 = 32u * hf;
            MH bh;
            coopMatLoad(bh, pc.bias.v, hd * 4096u + r0 * 64u + n0, 64u, ROW);
            ACC s0 = ACC(bh);
            coopMatLoad(bh, pc.bias.v, hd * 4096u + r0 * 64u + n0 + 16u, 64u, ROW);
            ACC s1 = ACC(bh);
            WG_SYNC();
            for (uint k = 0u; k < 32u; k += 16u) {
                MA alo, ahi;
                ALOAD(alo, ahi, qb + r0 * 32u + k, 32u)
                MB b;
                coopMatLoad(b, lds, qb + n0 * 32u + k, 32u, COL);
                MSTEP(s0, alo, ahi, b)
                coopMatLoad(b, lds, qb + (n0 + 16u) * 32u + k, 32u, COL);
                MSTEP(s1, alo, ahi, b)
            }
            coopMatStore(MH(s0), lds, pr + n0, SST, ROW);
            coopMatStore(MH(s1), lds, pr + n0 + 16u, SST, ROW);
        }
        WG_SYNC();
#else
        {
            MH bh;
            coopMatLoad(bh, pc.bias.v, hd * 4096u + r0 * 64u, 64u, ROW);
            ACC s0 = ACC(bh);
            coopMatLoad(bh, pc.bias.v, hd * 4096u + r0 * 64u + 16u, 64u, ROW);
            ACC s1 = ACC(bh);
            coopMatLoad(bh, pc.bias.v, hd * 4096u + r0 * 64u + 32u, 64u, ROW);
            ACC s2 = ACC(bh);
            coopMatLoad(bh, pc.bias.v, hd * 4096u + r0 * 64u + 48u, 64u, ROW);
            ACC s3 = ACC(bh);
            WG_SYNC();
            for (uint k = 0u; k < 32u; k += 16u) {
                MA alo, ahi;
                ALOAD(alo, ahi, qb + r0 * 32u + k, 32u)
                MB b;
                coopMatLoad(b, lds, qb + k, 32u, COL);
                MSTEP(s0, alo, ahi, b)
                coopMatLoad(b, lds, qb + 512u + k, 32u, COL);
                MSTEP(s1, alo, ahi, b)
                coopMatLoad(b, lds, qb + 1024u + k, 32u, COL);
                MSTEP(s2, alo, ahi, b)
                coopMatLoad(b, lds, qb + 1536u + k, 32u, COL);
                MSTEP(s3, alo, ahi, b)
            }
            coopMatStore(MH(s0), lds, pr, SST, ROW);
            coopMatStore(MH(s1), lds, pr + 16u, SST, ROW);
            coopMatStore(MH(s2), lds, pr + 32u, SST, ROW);
            coopMatStore(MH(s3), lds, pr + 48u, SST, ROW);
        }
        SG_SYNC();
#endif
        // softmax numerators and fp16 row sums in the reference order, P = num * (1 / sum); two lanes per row,
        // lane half h owns the columns 8 a + 4 h .. 8 a + 4 h + 3 (the partial sums v[4 h .. 4 h + 3])
#if SPLIT == 2
        if (ln < 16u)
#endif
        {
            uint row = (pr + (8u * hf * (SPLIT - 1u) + (ln >> 1)) * SST + (ln & 1u) * 4u) >> 2;   // u32x2 index of columns 8 a + 4 h ..
            f16vec2 t[8][2];
            [[unroll]] for (int a = 0; a < 8; ++a) {
                uvec2 w = lds2[row + uint(2 * a)];
                t[a][0] = unpackFloat2x16(exp_bits2(unpackFloat2x16(w.x)));
                t[a][1] = unpackFloat2x16(exp_bits2(unpackFloat2x16(w.y)));
            }
            f16vec2 v[2];
            [[unroll]] for (int b = 0; b < 2; ++b) {
                precise f16vec2 sm = ((t[0][b] + t[2][b]) + (t[4][b] + t[6][b])) + ((t[1][b] + t[3][b]) + (t[5][b] + t[7][b]));
                v[b] = sm;
            }
            precise float16_t b10 = v[0].x + v[1].x, b11 = v[0].y + v[1].y;
            float16_t o0 = subgroupShuffleXor(b10, 1u), o1 = subgroupShuffleXor(b11, 1u);
            precise float16_t b20 = b10 + o0, b21 = b11 + o1;
            precise float16_t sum = b20 + b21;
            float16_t r = float16_t(rcp_rn(float(sum)));
            [[unroll]] for (int a = 0; a < 8; ++a) {
                precise f16vec2 p0 = t[a][0] * r, p1 = t[a][1] * r;
                lds2[row + uint(2 * a)] = uvec2(packFloat2x16(p0), packFloat2x16(p1));
            }
        }
#if SPLIT == 2
        WG_SYNC();
        // O_h column tile hf -> OB [64][32], then the out projection of the own output tiles over K = 32 h ..
        {
            ACC o0 = ACC(0.0);
            for (uint k = 0u; k < 64u; k += 16u) {
                MA alo, ahi;
                ALOAD(alo, ahi, pr + k, SST)
                MB b;
                coopMatLoad(b, lds, vb + k * 32u + 16u * hf, 32u, ROW);
                MSTEP(o0, alo, ahi, b)
            }
            coopMatStore(MH(o0), lds, OB + r0 * 32u + 16u * hf, 32u, ROW);
        }
        WG_SYNC();
        for (uint k = 0u; k < 32u; k += 16u) {
            MA alo, ahi;
            ALOAD(alo, ahi, OB + r0 * 32u + k, 32u)
            FOR_NT(STEP_OUT)
        }
    }
#else
        SG_SYNC();
        // O_h = P V_h (16 x 32, K = 64) over P in PR
        {
            ACC o0 = ACC(0.0), o1 = ACC(0.0);
            for (uint k = 0u; k < 64u; k += 16u) {
                MA alo, ahi;
                ALOAD(alo, ahi, pr + k, SST)
                MB b;
                coopMatLoad(b, lds, vb + k * 32u, 32u, ROW);
                MSTEP(o0, alo, ahi, b)
                coopMatLoad(b, lds, vb + k * 32u + 16u, 32u, ROW);
                MSTEP(o1, alo, ahi, b)
            }
            SG_SYNC();
            coopMatStore(MH(o0), lds, pr, SST, ROW);
            coopMatStore(MH(o1), lds, pr + 16u, SST, ROW);
        }
        SG_SYNC();
        for (uint k = 0u; k < 32u; k += 16u) {
            MA alo, ahi;
            ALOAD(alo, ahi, pr + k, SST)
            FOR_NT(STEP_OUT)
        }
        SG_SYNC();
    }
#endif
    CUT(3)

    // ---------------- x1 = fma(O out^T + out_b, ls1, x), then the MLP in 32-neuron slices; x2 = fma(m, ls2, x1)
#define RESID(cn, n, l) { MH a16 = MH(cn), xt; coopMatLoad(xt, lds, XS + r0 * ENC_C + (n) * 16u, ENC_C, ROW); \
        for (int i = 0; i < a16.length(); i += 2) { \
            precise f16vec2 v = fma(f16vec2(a16[i], a16[i + 1]), f16vec2(l), f16vec2(xt[i], xt[i + 1])); xt[i] = v.x, xt[i + 1] = v.y; } \
        coopMatStore(xt, lds, XS + r0 * ENC_C + (n) * 16u, ENC_C, ROW); }
#define X1(cn, n) RESID(cn, n, ls1)
    FOR_NT(X1)
    RT_SYNC();
    CUT(4)
#define FC2B(cn, n) cn = bias_acc(pc.fc2_b, (n) * 16u);
    FOR_NT(FC2B)
#if MLP64 && SPLIT == 2
    J_UNROLL for (uint j = 0u; j < HID; j += 64u) {
        {
            uint j0 = j + 32u * hf;
            ACC h0 = bias_acc(pc.fc1_b, j0), h1 = bias_acc(pc.fc1_b, j0 + 16u);
            for (uint k = 0u; k < ENC_C; k += 16u) {
                MA alo, ahi;
                ALOAD(alo, ahi, XS + r0 * ENC_C + k, ENC_C)
                GSTEP(h0, alo, ahi, pc.fc1.v, j0 * ENC_C + k, ENC_C)
                GSTEP(h1, alo, ahi, pc.fc1.v, (j0 + 16u) * ENC_C + k, ENC_C)
            }
#define GELU_T(h, n) { MH g = MH(h); for (int i = 0; i < g.length(); i += 2) { f16vec2 q = gelu2(f16vec2(g[i], g[i + 1])); g[i] = q.x, g[i + 1] = q.y; } \
            coopMatStore(g, lds, pr + (n) * 16u, SST, ROW); }
            GELU_T(h0, 2u * hf) GELU_T(h1, 2u * hf + 1u)
        }
        WG_SYNC();
        MA a0l, a0h, a1l, a1h, a2l, a2h, a3l, a3h;
        ALOAD(a0l, a0h, pr, SST)
        ALOAD(a1l, a1h, pr + 16u, SST)
        ALOAD(a2l, a2h, pr + 32u, SST)
        ALOAD(a3l, a3h, pr + 48u, SST)
#define STEP_FC2(cn, n) GSTEP(cn, a0l, a0h, pc.fc2.v, (n) * 16u * HID + j, HID) GSTEP(cn, a1l, a1h, pc.fc2.v, (n) * 16u * HID + j + 16u, HID) \
        GSTEP(cn, a2l, a2h, pc.fc2.v, (n) * 16u * HID + j + 32u, HID) GSTEP(cn, a3l, a3h, pc.fc2.v, (n) * 16u * HID + j + 48u, HID)
        FOR_NT(STEP_FC2)
        WG_SYNC();
    }
#elif MLP64
    // 64-neuron slices: four fc1 tiles, then fc2 over K = 64 (the K order of the fc2 chain is unchanged)
    J_UNROLL for (uint j = 0u; j < HID; j += 64u) {
        {
            ACC h0 = bias_acc(pc.fc1_b, j), h1 = bias_acc(pc.fc1_b, j + 16u), h2 = bias_acc(pc.fc1_b, j + 32u), h3 = bias_acc(pc.fc1_b, j + 48u);
            for (uint k = 0u; k < ENC_C; k += 16u) {
                MA alo, ahi;
                ALOAD(alo, ahi, XS + r0 * ENC_C + k, ENC_C)
                GSTEP(h0, alo, ahi, pc.fc1.v, j * ENC_C + k, ENC_C)
                GSTEP(h1, alo, ahi, pc.fc1.v, (j + 16u) * ENC_C + k, ENC_C)
                GSTEP(h2, alo, ahi, pc.fc1.v, (j + 32u) * ENC_C + k, ENC_C)
                GSTEP(h3, alo, ahi, pc.fc1.v, (j + 48u) * ENC_C + k, ENC_C)
            }
#define GELU_T(h, n) { MH g = MH(h); for (int i = 0; i < g.length(); i += 2) { f16vec2 q = gelu2(f16vec2(g[i], g[i + 1])); g[i] = q.x, g[i + 1] = q.y; }             coopMatStore(g, lds, pr + (n) * 16u, SST, ROW); }
            GELU_T(h0, 0u) GELU_T(h1, 1u) GELU_T(h2, 2u) GELU_T(h3, 3u)
        }
        SG_SYNC();
        MA a0l, a0h, a1l, a1h, a2l, a2h, a3l, a3h;
        ALOAD(a0l, a0h, pr, SST)
        ALOAD(a1l, a1h, pr + 16u, SST)
        ALOAD(a2l, a2h, pr + 32u, SST)
        ALOAD(a3l, a3h, pr + 48u, SST)
#define STEP_FC2(cn, n) GSTEP(cn, a0l, a0h, pc.fc2.v, (n) * 16u * HID + j, HID) GSTEP(cn, a1l, a1h, pc.fc2.v, (n) * 16u * HID + j + 16u, HID)         GSTEP(cn, a2l, a2h, pc.fc2.v, (n) * 16u * HID + j + 32u, HID) GSTEP(cn, a3l, a3h, pc.fc2.v, (n) * 16u * HID + j + 48u, HID)
        FOR_NT(STEP_FC2)
        SG_SYNC();
    }
#else
    J_UNROLL for (uint j = 0u; j < LT_HID; j += 32u) {
        {
            ACC h0 = bias_acc(pc.fc1_b, j), h1 = bias_acc(pc.fc1_b, j + 16u);
            for (uint k = 0u; k < ENC_C; k += 16u) {
                MA alo, ahi;
                ALOAD(alo, ahi, XS + r0 * ENC_C + k, ENC_C)
                GSTEP(h0, alo, ahi, pc.fc1.v, j * ENC_C + k, ENC_C)
                GSTEP(h1, alo, ahi, pc.fc1.v, (j + 16u) * ENC_C + k, ENC_C)
            }
            MH g0 = MH(h0), g1 = MH(h1);
            for (int i = 0; i < g0.length(); i += 2) {
                f16vec2 p = gelu2(f16vec2(g0[i], g0[i + 1])), q = gelu2(f16vec2(g1[i], g1[i + 1]));
                g0[i] = p.x, g0[i + 1] = p.y, g1[i] = q.x, g1[i + 1] = q.y;
            }
            coopMatStore(g0, lds, pr, SST, ROW);
            coopMatStore(g1, lds, pr + 16u, SST, ROW);
        }
        SG_SYNC();
        MA a0l, a0h, a1l, a1h;
        ALOAD(a0l, a0h, pr, SST)
        ALOAD(a1l, a1h, pr + 16u, SST)
#define STEP_FC2(cn, n) GSTEP(cn, a0l, a0h, pc.fc2.v, (n) * 16u * HID + j, HID) GSTEP(cn, a1l, a1h, pc.fc2.v, (n) * 16u * HID + j + 16u, HID)
        FOR_NT(STEP_FC2)
        SG_SYNC();
    }
#endif
#define X2(cn, n) RESID(cn, n, ls2)
    FOR_NT(X2)
    RT_SYNC();
#endif
    CUT(5)

    // ---------------- outputs of the own tokens
#if !DEC_HEAD && !TR
    {
        U32View yw = U32View(uint64_t(pc.y));
        for (uint i = ln + 32u * hf; i < 16u * (ENC_C / 16u); i += 32u * SPLIT) {
            uint tl = i / (ENC_C / 16u), a = (i % (ENC_C / 16u)) * 16u;
            bool own;
            ivec2 p = token_pixel(int(r0 + tl), win, own);
            uint b = XS + (r0 + tl) * ENC_C;
            if (own) U32x4View(uint64_t(pc.y)).v[plane_index(p, int(a)) >> 4] = get16(b, a);
        }
    }
#endif
    CUT(6)
#if DEC_DFN || DEC_HEAD
#if DEC_DFN
    // decoder DFN pre-activations d = x2 dfn^T + b (16 x 16 per subgroup, 4 used) -> PR
#if TR
    DFN_PRE_T
#else
    {
        ACC acc = bias_acc(pc.dfn_b, 0u);
        for (uint k = 0u; k < ENC_C; k += 16u) {
            MA alo, ahi;
            ALOAD(alo, ahi, XS + r0 * ENC_C + k, ENC_C)
            GSTEP(acc, alo, ahi, pc.dfn_w.v, k, ENC_C)
        }
        if (hf == 0u) coopMatStore(MH(acc), lds, pr, 16u, ROW);
    }
#endif
    SG_SYNC();
#define DPRE(t, c) lds[pr + (t) * 16u + uint(c)]
#else
#define DPRE(t, c) lds[XS + (r0 + (t)) * ENC_C + uint(c)]
#endif
    // up = bilinear(below) * fp16(sig(d_c) s); out = min(fma(skip, 1 - sig(d3), up * sig(d3)), 65504)
    if (ln < 16u && hf == 0u) {
        bool own;
        ivec2 p = token_pixel(int(r0 + ln), win, own);
        if (own && p.y < pc.out_h) {
            uint pi = uint(p.y * pc.size.x + p.x) * 4u;
            float16_t sd = uint16BitsToFloat16(uint16_t(pc.s_dfn));
            float16_t g3 = sigmoid16(DPRE(ln, 3));
            precise float16_t omg = 1.0hf - g3;
            f16vec3 u = upsample_below(p);
            f16vec4 sk = F16x4Buf(uint64_t(pc.skip)).v[pi >> 2], o;
            for (int c = 0; c < 3; ++c) {
                float16_t g = sigmoid16(DPRE(ln, c));
                precise float16_t gs = g * sd;
                precise float16_t ug = u[c] * gs;
                precise float16_t tt = ug * g3;
                precise float16_t oc = fma(sk[c], omg, tt);
                o[c] = min(oc, 65504.0hf);
            }
            precise float16_t ow = sk.w * omg;
            o.w = min(ow, 65504.0hf);
            F16x4Buf(uint64_t(pc.dfn_out)).v[pi >> 2] = o;
#if DEC_HEAD
            if (p.x < pc.out_w) {
            pi = uint(p.y * pc.out_w + p.x) * 4u;
            // block channels (GPU order): 4-6 AjabGateOgate, 7-9 Aniso, 10-13 MvecFactors, 14-17 HiddenHistory
            u8vec4 aj = u8vec4(0);
            for (int c = 0; c < 3; ++c) aj[c] = uint8_t(unorm8_code(sigmoid16(DPRE(ln, 4 + c))));
            U8x4Buf(uint64_t(pc.ajab)).v[pi >> 2] = aj;
            // softmax of 4 logits: EX2.F16 (rounded toward zero), fp16 sum (e0 + e2) + (e1 + e3), f32 RCP
            float16_t m[4], e[4];
            for (int c = 0; c < 4; ++c) m[c] = DPRE(ln, 10 + c);
            float16_t mx = max(max(m[0], m[2]), max(m[1], m[3]));
            for (int c = 0; c < 4; ++c) {
                precise float16_t d = m[c] - mx;
                precise float16_t z = d * 1.4423828125hf;
                e[c] = half_rz(exp2(float(z)));
            }
            precise float16_t s02 = e[0] + e[2], s13 = e[1] + e[3], se = s02 + s13;
            float16_t r = float16_t(rcp_rn(float(se)));
            u8vec4 mv, hh;
            for (int c = 0; c < 4; ++c) { precise float16_t v = e[c] * r; mv[c] = uint8_t(unorm8_code(v)); }
            U8x4Buf(uint64_t(pc.mvf)).v[pi >> 2] = mv;
            // TANH.F16 rounded toward zero
            for (int c = 0; c < 4; ++c) hh[c] = uint8_t(snorm8_code(half_rz(tanh_f32(float(DPRE(ln, 14 + c))))));
            U8x4Buf(uint64_t(pc.hidden)).v[pi >> 2] = hh;
            // Aniso: a = k sig(clamp(u, +-20)), b likewise, (a^2, b^2, a b tanh(k_t t), 0)
            precise float a = float(sigmoid16(clamp(DPRE(ln, 7), -20.0hf, 20.0hf))) * pc.k_scale;
            precise float b = float(sigmoid16(clamp(DPRE(ln, 8), -20.0hf, 20.0hf))) * pc.k_scale;
            precise float tt = float(DPRE(ln, 9)) * pc.k_tanh;
            float th = tanh_f32(tt);
            precise float aa = a * a, bb = b * b, ab = a * b, abt = ab * th;
            F16x4Buf(uint64_t(pc.aniso)).v[pi >> 2] = f16vec4(float16_t(aa), float16_t(bb), float16_t(abt), 0.0hf);
            if (uint64_t(pc.tile) != 0ul) {
                // post_tile fused: enc(DFN.rgb), ajab as fp16, aniso * -1/(2 ln 2), -1/(2 ln 2), -1/ln 2
                vec3 en = post_encode(vec3(o.xyz));
                precise float t0 = float(float16_t(aa)) * fbits(0xbf38a8c1u);
                precise float t1 = float(float16_t(bb)) * fbits(0xbf38a8c1u);
                precise float t2 = float(float16_t(abt)) * fbits(0xbfb8a8c1u);
                uint ti = (pi >> 2) * 3u;
                pc.tile.v[ti] = f16vec4(f16vec3(en), unorm8_half(uint(aj.x)));
                pc.tile.v[ti + 1u] = f16vec4(unorm8_half(uint(aj.y)), unorm8_half(uint(aj.z)), 0.0hf, float16_t(t0));
                pc.tile.v[ti + 2u] = f16vec4(float16_t(t1), float16_t(t2), 0.0hf, 0.0hf);
            }
            }
#endif
        }
    }
#endif
#if ENC_STAGE
    // DFN pre-activations d = x2 dfn^T + b (16 x 16 per subgroup, 3 used) -> PR
#if TR
    DFN_PRE_T
#else
    {
        ACC acc = bias_acc(pc.dfn_b, 0u);
        for (uint k = 0u; k < ENC_C; k += 16u) {
            MA alo, ahi;
            ALOAD(alo, ahi, XS + r0 * ENC_C + k, ENC_C)
            GSTEP(acc, alo, ahi, pc.dfn_w.v, k, ENC_C)
        }
        if (hf == 0u) coopMatStore(MH(acc), lds, pr, 16u, ROW);
    }
#endif
    SG_SYNC();
    // DFN gains: g = fp16(1 / (1 + 2^(-log2(e) d))) * s; out.rgb = min(img.rgb * g, 65504), out.a = img.a * 0
    if (ln < 16u && hf == 0u) {
        bool own;
        ivec2 p = token_pixel(int(r0 + ln), win, own);
        f16vec4 o = f16vec4(0.0hf);
        if (own) {
            uint pi = uint(p.y * pc.size.x + p.x);
            float16_t sd = uint16BitsToFloat16(uint16_t(pc.s_dfn));
            f16vec4 im = F16x4Buf(uint64_t(pc.dfn_in)).v[pi];
            for (int c = 0; c < 3; ++c) {
                precise float e = exp2(float(lds[pr + ln * 16u + uint(c)]) * uintBitsToFloat(0xbfb8aa3bu));
                float16_t g = float16_t(rcp_rn(e + 1.0));
                precise float16_t gs = g * sd;
                precise float16_t v = im[c] * gs;
                o[c] = min(v, 65504.0hf);
            }
            precise float16_t a = im.w * 0.0hf;
            o.w = a;
            F16x4Buf(uint64_t(pc.dfn_out)).v[pi] = o;
        }
        for (int c = 0; c < 4; ++c) lds[pr + 256u + ln * 4u + uint(c)] = o[c];
    }
    SG_SYNC();
    // DFNPooledOutput: the 4 pooled pixels of the own rows; ((p00 + p01) + (p10 + p11)) * 0.25 in fp16
    // (enc0: each texel * 0.25 first, at most 65504)
    if ((ENC_CO > 0 || ENC_EMBED != 0) && ln < 4u && hf == 0u) {
        uint t0 = 2u * ln;   // own-local token of the top-left texel (rows 16 s, 16 s + 8)
        bool own;
        ivec2 p = token_pixel(int(r0 + t0), win, own);
        if (own && p.x + 1 < pc.size.x && p.y + 1 < pc.size.y) {
            ivec2 q = p >> 1;
            uint pi = uint(q.y * (pc.size.x >> 1) + q.x);
            uint d = pr + 256u + t0 * 4u;
            f16vec4 o;
            for (uint c = 0u; c < 4u; ++c) {
#if ENC_EMBED
                precise float16_t q00 = lds[d + c] * 0.25hf, q01 = lds[d + 4u + c] * 0.25hf;
                precise float16_t q10 = lds[d + 32u + c] * 0.25hf, q11 = lds[d + 36u + c] * 0.25hf;
                precise float16_t v = (q00 + q01) + (q10 + q11);
                o[c] = min(v, 65504.0hf);
#else
                precise float16_t s0 = lds[d + c] + lds[d + 4u + c];
                precise float16_t s1 = lds[d + 32u + c] + lds[d + 36u + c];
                precise float16_t v = (s0 + s1) * 0.25hf;
                o[c] = v;
#endif
            }
            F16x4Buf(uint64_t(pc.dfn_pool)).v[pi] = o;
        }
    }
#if ENC_CO > 0
    // merge: A row r = pooled pixel (r / 4, r % 4) of the window, K = (2 ky + kx) C + c, in QV; pooled (16 x Co)
    // = A mrg^T + b -> XS
    WG_SYNC();
#if !XPERM
    // 8 halves per access (XS, QV and C are multiples of 8)
    for (uint i = lid; i < 8u * ENC_C; i += NTH) {
        uint r = i / (ENC_C / 2u), k = (i % (ENC_C / 2u)) * 8u, q = k / ENC_C, c = k % ENC_C;
        uint tok = (2u * (r >> 2) + (q >> 1)) * 8u + 2u * (r & 3u) + (q & 1u);
        lds4[(QV >> 3) + i] = lds4[(XS + tok * ENC_C + c) >> 3];
    }
    WG_SYNC();
#endif
    for (uint nt = s; nt < ENC_CO / 16u; nt += NSG) {
        ACC acc = bias_acc(pc.mrg_b, nt * 16u);
        for (uint k = 0u; k < 4u * ENC_C; k += 16u) {
            MA alo, ahi;
#if XPERM
            ALOAD(alo, ahi, XS + (k / ENC_C) * 16u * ENC_C + k % ENC_C, ENC_C)
#else
            ALOAD(alo, ahi, QV + k, 4u * ENC_C)
#endif
            GSTEP(acc, alo, ahi, pc.mrg_w.v, WT_MRG(nt * 16u * 4u * ENC_C + k), WS_MRG)
        }
        coopMatStore(MH(acc), lds, MO + nt * 16u, ENC_CO, ROW);
    }
    WG_SYNC();
    // PooledOutput: e4m3 of the pooled pixels the window owns
    {
        ivec2 ps = pc.size >> 1;
        for (uint i = lid; i < ENC_CO; i += NTH) {
            uint r = i / (ENC_CO / 16u), a = (i % (ENC_CO / 16u)) * 16u;
            bool own;
            ivec2 p = token_pixel(int((2u * (r >> 2)) * 8u + 2u * (r & 3u)), win, own);
            if (!own) continue;
            ivec2 q = p >> 1;
            uint b = MO + r * ENC_CO;
            U32x4View(uint64_t(pc.pooled)).v[(uint(a >> 5) * uint(ps.x * ps.y * 32) + uint((q.y * ps.x + q.x) * 32) + (a & 31u)) >> 4] =
                get16(b, a);
        }
    }
#endif
#endif
}
#endif
