// SPDX-License-Identifier: MIT
// rr2 transformer block (TensorOutput only, no merge, no DFN): shifted 8x8 window attention with shared Q/K
// projection and relative-position bias, cubic bit-trick softmax, MLP with the polynomial GELU, residuals
// x + ls * branch in fp16. One workgroup (4 subgroups of 32) per window, FP16 WMMA; accumulators start from
// the bias (as the reference HMMA chains do); everything between the products is fp16.
// Weights are the logical [N][K] fp16 (fp16(e4m3(w))) and the residual stream is kept in the GPU fragment
// channel order of the reference (arena channel 32 b + G[k], FASE3B), so K runs in the reference order.
//
// Two numeric paths:
//   default    fp32 accumulators, one rounding to fp16 per product (96-96.5% of the e4m3 outputs equal)
//   ENC_K8     the reference HMMA.1688.F16 chain: 8 K per step, accumulator rounded to fp16 after each.
//              Each 16-K WMMA runs twice with A masked to K 0..7, then 8..15 (fp32 accumulation, then RN)
//              (gpu/tools/enc_model.py --k8 is the model of it; no 2^-24 truncation of the terms)
//
// Built once per (ENC_C, ENC_X) by enc_block_<C>_<X>[_k8].comp. ENC_STAGE adds the rest of an rr2_encN kernel
// (enc_stage_<C>_<X>_<Co>.comp, k8 path): the DFN head (3 gains from the block output, applied to the DFN image
// of the level), the 2x2 pooling of the DFN image and, when ENC_CO > 0, the 2x2 stride-2 merge to the
// PooledOutput of the next level. Shifts are even, so every 2x2 block lies inside one window.
// Other kernels of the network (k8 path):
//   ENC_MERGE_IN  rr2_enc1: the tokens are a 2x2 stride-2 conv of the enc0 TensorOutput (ENC_CIN channels)
//   DEC_UP        rr2_decN_kernel_0: tokens = fma(skip, s, up), up = Linear + PixelShuffle 2x of the level below
//   DEC_DFN       rr2_decN_kernel_1: block, then the decoder DFN (4 gains, bilinear 2x of the DFN image below)
//   DEC_HEAD      rr2_dec0_kernel_1: block, then the output head (no TensorOutput)
// LDS: one fp16 array (LLPC 26.10 misplaces
// coopMatStore output with several shared arrays) of 64 * (C + X + 32 + 64) halves = 64 KB at C 160, X 256:
//   XS  x, then x1                                   [64][C]
//   QO  Q (all heads), O_h over Q_h, then x2 staging [64][X]
//   VH  V_h, then the MLP hidden slice              [64][32]
//   SS  scores, then P                               [64][64]
#extension GL_KHR_cooperative_matrix : require
#extension GL_KHR_memory_scope_semantics : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_shuffle : require
#extension GL_EXT_shader_subgroup_extended_types_float16 : require
#extension GL_EXT_control_flow_attributes : require

#ifndef ENC_K8
#define ENC_K8 0
#endif
#ifndef ENC_STAGE
#define ENC_STAGE 0
#endif
#ifndef ENC_CO
#define ENC_CO 0
#endif
#ifndef ENC_MERGE_IN
#define ENC_MERGE_IN 0
#endif
#ifndef ENC_CIN
#define ENC_CIN 0
#endif
#ifndef DEC_UP
#define DEC_UP 0
#endif
#ifndef DEC_DFN
#define DEC_DFN 0
#endif
#ifndef DEC_HEAD
#define DEC_HEAD 0
#endif
#ifndef ENC_EMBED
#define ENC_EMBED 0
#endif
#ifndef ENC_PVX
#define ENC_PVX 0
#endif
#ifndef ENC_EXACT
#define ENC_EXACT 0
#endif
#ifndef ENC_K8H
#define ENC_K8H 0
#endif
#ifndef ENC_F32
#define ENC_F32 0
#endif
#ifndef FLOW
#define FLOW 0
#endif
// the runtime path (onr_block_fast.glsl): k8 with every product on WMMA
#define ONR_FAST (ENC_K8 != 0 && ENC_PVX == 0 && ENC_EXACT == 0 && ENC_K8H == 0)
// low-resolution levels (few windows): two subgroups per row tile, 256 lanes per window
#ifndef SPLIT
#if ONR_FAST && ENC_C >= 160
#define SPLIT 2
#else
#define SPLIT 1
#endif
#endif
// C = 32, fast variant: one subgroup per window, everything in registers (onr_block_w1.glsl)
#ifndef W1
#define W1 (ONR_FAST && ENC_F32 != 0 && ENC_C == 32 && (DEC_HEAD != 0 || DEC_UP != 0 || ENC_EMBED != 0))
#endif
// C = 64 and 96, fast variant: two subgroups per window, 32 tokens each in registers (onr_block_w2.glsl)
#ifndef W2
// (C = 96: enc2 and dec2_1; dec2_0 in W2 measures the same as the transposed body with its LDS trimmed, W2_96UP builds it)
#ifndef W2_96
#define W2_96 1
#endif
#ifndef W2_96UP
#define W2_96UP 0
#endif
#define W2 (ONR_FAST && ENC_F32 != 0 && ((ENC_C == 64 && (DEC_DFN != 0 || DEC_UP != 0 || ENC_MERGE_IN != 0)) || \
    (ENC_C == 96 && W2_96 != 0 && (DEC_DFN != 0 || ENC_STAGE != 0 || (DEC_UP != 0 && W2_96UP != 0)))))
#endif
#if W1
layout(local_size_x = 32) in;
#elif W2
layout(local_size_x = 64) in;
#else
layout(local_size_x = 128 * SPLIT) in;
#endif
#define NH (ENC_X / 32)
#define HID (4 * ENC_C)
// the lighter kernels of the optional mode (docs/FASE24-FASTMODE.md): attention heads and MLP neurons kept
#ifndef LT_HEADS
#define LT_HEADS NH
#endif
#ifndef LT_HID
#define LT_HID HID
#endif
// LT_FAR: the key tiles (two window rows each) LT_FAR or more tiles from the own one are left out of the softmax. Their P
// mass is small: attention is local inside the window. tile = own tile, kt = key tile
#ifdef LT_FAR
#define KEPT(tile, kt) ((tile) + uint(LT_FAR) > (kt) && (kt) + uint(LT_FAR) > (tile))
#define IFK(tile, kt) if (KEPT(tile, kt))
#define TK(tile, kt, x) (KEPT(tile, kt) ? (x) : f16vec2(0.0hf))
#else
#define IFK(tile, kt)
#define TK(tile, kt, x) (x)
#endif
// LT_Q16 (W1): keys on the 16 strongest directions of each head, from the rotated in_a after the regular one and its e4m3
// codes (Engine::upload_qv): half the Q and S products for almost none of S
#ifdef LT_Q16
#define Q2_IF if (false)
#define QA_LR(x) (3u * ENC_X * ENC_C / 2u + (x))
#else
#define Q2_IF
#define QA_LR(x) (x)
#endif
// LT_RCP: the softmax's reciprocal by the hardware's fp16 one
#ifdef LT_RCP
#define RCP_SUM(x) (1.0hf / (x))
#else
#define RCP_SUM(x) float16_t(rcp_rn(float(x)))
#endif
#define NT (ENC_C / 16)
// wout [C][X], fc1 [4C][C], fc2 [C][4C] are each followed by a copy in 16x16 tiles (tile (r, k) = 16 contiguous rows
// of 16 at n + r 16 K + 256 k): the transposed and W2 bodies load a fragment from its tile (512 contiguous bytes;
// measured faster there, not in W1)
#ifndef WTILE
#define WTILE (W1 == 0)
#endif
#if WTILE
#define WT(off, K, n) ((n) + ((off) / (16u * (K))) * 16u * (K) + (((off) % (K)) / 16u) * 256u)
#define WT_OUT(off) WT(off, ENC_X, ENC_C * ENC_X)
#define WT_FC1(off) WT(off, ENC_C, HID * ENC_C)
#define WT_FC2(off) WT(off, HID, ENC_C * HID)
#define WS_OUT 16u
#define WS_FC1 16u
#define WS_FC2 16u
// the merge [Co][4C] of the encoder stages (tiles after it when there is no input merge; read from them at C = 128,
// the same at C = 160)
#if SPLIT == 1 && ENC_MERGE_IN == 0
#define WT_MRG(off) WT(off, 4u * ENC_C, ENC_CO * 4u * ENC_C)
#define WS_MRG 16u
#else
#define WT_MRG(off) (off)
#define WS_MRG (4u * ENC_C)
#endif
#else
#define WT_OUT(off) (off)
#define WT_FC1(off) (off)
#define WT_FC2(off) (off)
#define WS_OUT ENC_X
#define WS_FC1 ENC_C
#define WS_FC2 HID
#define WT_MRG(off) (off)
#define WS_MRG (4u * ENC_C)
#endif

layout(push_constant, scalar) uniform P {
    U8Buf x;             // input planes [C/32][H][W][32] e4m3
    U8Buf y;             // TensorOutput planes
    F16Buf in_a, in_b;   // [X][C]
    F16Buf wout;         // [C][X]
    F16Buf out_b;        // [C]
    F16Buf fc1, fc1_b;   // [4C][C], [4C]
    F16Buf fc2, fc2_b;   // [C][4C], [C]
    F16Buf bias;         // [NH][64][64], token order of the window
    ivec2 size;
    ivec2 shift;
    uint ls;             // ls1 | ls2 << 16 (fp16 bits)
    int nwx;
    F16Buf mask;         // ENC_K8: two row-major 16x16 A masks, K columns 0..7 and 8..15
#if ENC_STAGE
    F16Buf dfn_w, dfn_b; // [16][C] (rows 3..15 zero), [16]
    F16Buf mrg_w, mrg_b; // [Co][4C], K = (2 ky + kx, channel), [Co]
    F16Buf dfn_in;       // DFN image of this level, rgba16f size.x x size.y
    F16Buf dfn_out;      // DFNOutput, rgba16f
    F16Buf dfn_pool;     // DFNPooledOutput, rgba16f (size / 2)
    U8Buf pooled;        // PooledOutput planes [Co/32][size.y / 2][size.x / 2][32] e4m3
    uint s_dfn;          // fp16 bits of the DFN gain
    ivec2 in_size;       // ENC_MERGE_IN: size of the input planes (ENC_CIN channels, 2x the level)
#if ENC_EMBED
    F16Buf attr;         // [img_h][size.x][48] fp16 attributes (enc0_attr.comp); mrg_w/mrg_b hold the embed MLP
    U16Buf lut;          // blue-noise LUT (2048 x u16)
    uint frame8;         // LUT page
    int img_h;           // real image height (padded rows mirror at it)
    int img_w;           // real image width (padded columns mirror at it); attr row pitch
#endif
#elif DEC_UP
    U8Buf xlo;           // tensor of the level below, planes at lo_size with ENC_CP channels (x is the skip)
    F16Buf up_w, up_b;   // [4][C][Cp], [4][C]; PixelShuffle position 2 ky + kx
    ivec2 lo_size;
    uint s_skip;         // fp16 bits: z = fma(skip, s_skip, up)
    int live_h;          // W1: rows of the TensorOutput the next kernel reads (0: all)
#elif DEC_DFN || DEC_HEAD
    F16Buf dfn_w, dfn_b; // DEC_DFN: [16][C] (rows 4..15 zero), [16]
    F16Buf below;        // DFN image of the level below, rgba16f lo_size
    F16Buf skip;         // encoder DFN image of the level, rgba16f size.x x out_h
    F16Buf dfn_out;      // DFNOutput, rgba16f size.x x out_h
    ivec2 lo_size;
    uint s_dfn;
    int out_h;           // rows of the DFN images (720 at level 0, else size.y)
    U8Buf ajab, mvf;     // DEC_HEAD: unorm8x4 surfaces
    U8Buf hidden;        // snorm8x4
    F16Buf aniso;        // rgba16f
    float k_scale, k_tanh;
    int out_w;           // DEC_HEAD: real width, row pitch of ajab / mvf / hidden / aniso
    F16x4Buf tile;       // DEC_HEAD (runtime path): the post's per-pixel tile (post_tile.comp) when non-zero
#endif
} pc;
#if DEC_HEAD
#include "onr_post.glsl"
#endif

#define MA coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA>
#define MB coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB>
#define MC coopmat<float, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>
#define MH coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>
#ifndef F16ACC
#define F16ACC 1
#endif
#if ENC_K8 && ENC_F32 && F16ACC && ENC_PVX == 0 && ENC_EXACT == 0 && ENC_K8H == 0
// fast variant: fp16-accumulator WMMA, one rounding to fp16 per 16 K (no f32 <-> f16 conversions)
#define ACC MH
#elif ENC_K8 && ENC_K8H
// fp16-accumulator WMMA on 8 real K: the hardware sums in fp32 and rounds the result to fp16 once
#define ACC MH
#define STEP(acc, a, b) { acc = coopMatMulAdd((a) * mlo, b, acc); acc = coopMatMulAdd((a) * mhi, b, acc); }
#elif ENC_K8
#define ACC MC
// fp32 WMMA on 8 real K, then the accumulator is rounded to fp16
#define STEP(acc, a, b) { acc = coopMatMulAdd((a) * mlo, b, acc); acc = MC(MH(acc));                           acc = coopMatMulAdd((a) * mhi, b, acc); acc = MC(MH(acc)); }
#else
#define ACC MC
#define STEP(acc, a, b) acc = coopMatMulAdd((a), b, acc);
#endif
#define ROW gl_CooperativeMatrixLayoutRowMajor
#define COL gl_CooperativeMatrixLayoutColumnMajor

#if ONR_FAST
// transposed body (onr_block_fast.glsl): on the levels with one subgroup per row tile, and with two (TR2)
#ifndef TR2
#define TR2 (SPLIT == 2)
#endif
#ifndef TR
#define TR (SPLIT == 1 || TR2 != 0)
#endif
// dec2_0 fast: with one Q_h / V_h buffer 16 KB, 4 workgroups per CU instead of 2 (measured faster; slower at C = 128)
#ifndef XS_TRIM
#define XS_TRIM (ENC_F32 != 0 && ENC_C == 96 && DEC_UP != 0)
#endif
#define XS 0u
// TR with the merge: x2 rows go to XS permuted, so the merge's A tiles load straight from them
#define XPERM (TR && ENC_CO > 0)
// TR SPLIT 1 encoders with the merge: XS over the keys of the last head; DFN scratch and merge output after XS
#ifndef XS_ALIAS
#define XS_ALIAS (TR && SPLIT == 1 && ENC_CO > 0 && ENC_MERGE_IN == 0)
#endif
// TR keeps x in registers: XS_TRIM sizes XS to what is read back by rows (the merge-in tokens, the merge's and
// the head's x2, the decoder's low-res pixels). SPLIT 2: XS is the exchange of x tiles between the halves
#if XS_ALIAS
#define QV 0u
#elif TR && SPLIT == 1 && XS_TRIM && DEC_UP
#define QV (16u * ENC_CP)
#elif TR && SPLIT == 1 && XS_TRIM && ENC_CO == 0 && ENC_MERGE_IN == 0 && DEC_HEAD == 0
#define QV 0u
#else
#define QV (64u * ENC_C)
#endif
// decoder prologue: low-res pixels and U (SPLIT 2 TR: U over XS, the pixels over QV)
#if TR && SPLIT == 2
#define UPLO QV
#define UPU XS
#else
#define UPLO XS
#define UPU QV
#endif
// two Q_h / V_h buffers (one barrier per head less) where measured faster: C = 160, C = 96 and enc1; slower at C = 32,
// 64 (decoder) and 128
#ifndef QV_BUFS
#if (SPLIT == 2 && !TR) || (ENC_C == 96 && !XS_TRIM) || ENC_MERGE_IN != 0
#define QV_BUFS 2
#else
#define QV_BUFS 1
#endif
#endif
#define SST 72u
// QV-region scratch: the merge output (TR) or its A rows, the decoder's U (DEC_UP, but over XS in the SPLIT 2
// TR body)
#if ENC_CO > 0 && TR
#define SCR (16u * ENC_CO)
#elif ENC_CO > 0 || (DEC_UP != 0 && !(TR && SPLIT == 2))
#define SCR (64u * ENC_C)
#else
#define SCR 0u
#endif
#if TR
// the block keeps everything but Q_h / V_h in registers: the prologue, head and merge scratch alias QV
#if XS_ALIAS
#define PR (64u * ENC_C)
#define PRS 320u   // the DFN's d [16][16] and outputs [16][4] per row tile
#define MO PR
#else
#define PR QV
#define MO QV
#endif
#define VST 72u
#define QST 40u   // Q_h rows padded: the 16 rows of an A load fall in distinct banks
#define QVB (64u * QST + 32u * VST)   // one Q_h / V_h buffer
#if SPLIT == 2 && DEC_UP
#define SCR2 (16u * ENC_CP)
#elif SPLIT == 2
#define SCR2 4096u   // the MLP's GELU tiles, two buffers
#else
#define SCR2 0u
#endif
#if W1
#define LDS_N (DEC_UP != 0 ? 64u * ENC_C : 8u)   // DEC_UP: U [4][16][C]
#elif W2
// Q_h | V_h^T with padded rows (9.5 KB: 6 windows per CU, measured faster than 8 at 8 KB); also the
// DEC_UP U [4][16][C] and the enc1 merge rows [16][4 C] before / after
#define LDS_N (DEC_DFN != 0 ? 64u * QST + 32u * VST : max(64u * QST + 32u * VST, 64u * ENC_C))
#elif XS_ALIAS
#define LDS_N max(QV_BUFS * QVB, PR + max(4u * PRS, SCR))
#else
#define LDS_N (QV + max(max(max(QV_BUFS * QVB, SCR), SCR2), max(ENC_EMBED != 0 ? 64u * SST : 0u, ENC_MERGE_IN != 0 ? 256u * ENC_CIN : 0u)))
#endif
#else
// QV .. end also holds the prologue / merge scratch (64 C halves)
#define PR (QV + max(4096u * QV_BUFS + 64u * SST, SCR) - 64u * SST)
#define OB (PR + 64u * SST)
#define MO XS
#define LDS_N (OB + (SPLIT == 2 ? 2048u : 0u))
#endif
// one LDS, four aliased views (shared blocks alias at offset 0): halves, u32 pairs, u32x2 quads, u32x4 octets
#extension GL_EXT_shared_memory_block : require
#ifndef PRS
#define PRS (16u * SST)
#endif
shared LdsH { float16_t lds[LDS_N]; };
shared LdsW { uint lds1[LDS_N / 2u]; };
shared LdsD { uvec2 lds2[LDS_N / 4u]; };
shared LdsQ { uvec4 lds4[LDS_N / 8u]; };
#else
#define XS 0u
#define QO (64u * ENC_C)
#define VH (QO + 64u * ENC_X)
#define SS (VH + 64u * 32u)
// score rows padded to 72 halves (2-way instead of 32-way LDS bank conflicts in the softmax) where it fits
#if 64 * (ENC_C + ENC_X + 32 + 72) <= 32768
#define SST 72u
#else
#define SST 64u
#endif
shared float16_t lds[SS + 64u * SST];
#endif
#ifdef PROF_CUT
// profiling only: stop after phase n, keeping the LDS work alive
#define CUT(n) if (PROF_CUT == n) { barrier(); if (lds[gl_LocalInvocationIndex] == float16_t(-1234.0)) pc.y.v[gl_WorkGroupID.x] = uint8_t(1); return; }
#else
#define CUT(n)
#endif

const float16_t SCORE_SCALE = 0.017181396484375hf;
const float16_t CAP = 0.55615234375hf;
const float16_t POLY_A = 0.92724609375hf, POLY_B = 1.375hf;
const float16_t GELU_A = 0.412109375hf;

uint sg() { return gl_SubgroupID; }

float16_t e4m3_in(uint b) {
    // sm_86 expansion: exponent field 0 is read as a normal, so 0x00 is 2^-7
    return uint16BitsToFloat16(uint16_t(((b & 0x80u) << 8) | ((((b >> 3) & 15u) + 8u) << 10) | ((b & 7u) << 7)));
}

uint e4m3_out(float16_t x) {
    // sm_86 packing: add the weight of mantissa bit 6 (round half away), clamp to [2^-7 + ulp, 448]
    uint b = uint(float16BitsToUint16(x));
    precise float16_t r = x + uint16BitsToFloat16(uint16_t(b & 0xfc40u));
    r = r - uint16BitsToFloat16(uint16_t(b & 0xfc00u));
    float16_t m = min(max(abs(r), 0.00782012939453125hf), 448.0hf);
    return ((uint(float16BitsToUint16(m)) >> 7) - 0x40u) & 0x7fu | ((b >> 8) & 0x80u);
}

int reflect_i(int i, int n) { i = abs(i); return min(i, 2 * n - 2 - i); }

float16_t exp_bits(float16_t s) {
    precise float16_t z = clamp(s * SCORE_SCALE, -CAP, CAP);
    precise float16_t t = fma(z, -z, POLY_A);
    precise float16_t yv = fma(z, t, POLY_B);
    return uint16BitsToFloat16(uint16_t((uint(float16BitsToUint16(yv)) << 5) & 0x7fe0u));
}

float16_t gelu_poly(float16_t x) {
    float16_t t = clamp(x, -2.0hf, 2.0hf);
    precise float16_t u = fma(-abs(t), float16_t(0.08108), GELU_A);
    precise float16_t g = fma(t, u, 0.5hf);
    precise float16_t r = x * g;
    return r;
}

// token t of window w -> image pixel (reflected), and whether the window owns it
ivec2 token_pixel(int t, ivec2 w, out bool own) {
    ivec2 g = 8 * w + ivec2(t & 7, t >> 3) - pc.shift;
    own = g.x >= 0 && g.y >= 0 && g.x < pc.size.x && g.y < pc.size.y;
    return ivec2(reflect_i(g.x, pc.size.x), reflect_i(g.y, pc.size.y));
}

// token tile tt (window rows 2 tt, 2 tt + 1) has a token whose output is written
bool tile_live(int tt, ivec2 w) {
    ivec2 g = 8 * w + ivec2(0, 2 * tt) - pc.shift;
#if DEC_HEAD
    int lim = min(pc.size.y, pc.out_h);
#else
    int lim = pc.size.y;
#endif
    return g.x + 7 >= 0 && g.x < pc.size.x && g.y + 1 >= 0 && g.y < lim;
}

// arena channel of GPU fragment position c: 32 b + [0, 16, 2, 18][k / 8] + 4 ((k % 8) / 2) + k % 2
int arena_channel(int c) {
    int g = c & 31;
    return (c & ~31) + ((g >> 3) & 1) * 16 + ((g >> 4) & 1) * 2 + 4 * ((g & 7) >> 1) + (g & 1);
}

uint plane_index_sz(ivec2 p, int c, ivec2 sz) {
    return uint(c >> 5) * uint(sz.x * sz.y * 32) + uint((p.y * sz.x + p.x) * 32 + (c & 31));
}

uint plane_index(ivec2 p, int c) { return plane_index_sz(p, c, pc.size); }

// fp16(1 / (1 + 2^(-log2(e) d))), f32 in between (MUFU EX2/RCP of the reference)
// LT_HM: the heads' math without the reference's roundings (reciprocal, toward-zero and 12-bit surface rules, tanh branch)
#ifdef LT_HM
#define SIG_RCP(x) (1.0 / (x))
#define HEAD_RZ(x) float16_t(x)
#else
#define SIG_RCP(x) rcp_rn(x)
#define HEAD_RZ(x) half_rz(x)
#endif
float16_t sigmoid16(float16_t d) {
    precise float e = exp2(float(d) * uintBitsToFloat(0xbfb8aa3bu));
    return float16_t(SIG_RCP(e + 1.0));
}

#if ENC_PVX || ENC_EXACT
// floor(log2 |h|) of a nonzero fp16 (subnormals included)
int ilog2_h(float16_t h) {
    uint b = uint(float16BitsToUint16(h)) & 0x7fffu;
    return (b >> 10) != 0u ? int(b >> 10) - 15 : findMSB(b) - 24;
}

// n 2^e rounded once to fp16 (n is cut to 24 bits with a sticky bit first)
float rn16_scaled(int n, int e) {
    if (n == 0) return 0.0;
    uint a = uint(abs(n));
    int msb = findMSB(a);
    if (msb > 23) {
        int sh = msb - 23;
        uint m = a >> sh;
        if ((a & ((1u << sh) - 1u)) != 0u) m |= 1u;
        a = m;
        e += sh;
    }
    float r = float(float16_t(ldexp(float(a), e)));
    return n < 0 ? -r : r;
}

// one HMMA.1688.F16 step for one output: 8 products a_k b_k (b from LDS at bo + k * bs) and the fp16 accumulator c,
// each placed by its operand exponents, cut toward zero at 2^(e_max - 24), summed exactly, one RN to fp16
float hmma_step8(float a[8], int ea[8], uint bo, uint bs, float c) {
    float p[8];
    int emax = c != 0.0 ? int((floatBitsToUint(c) >> 23) & 0xffu) - 127 : -1000;
    for (uint k = 0u; k < 8u; ++k) {
        float16_t b = lds[bo + k * bs];
        precise float t = a[k] * float(b);
        p[k] = t;
        if (t != 0.0) emax = max(emax, ea[k] + ilog2_h(b));
    }
    if (emax == -1000) return c;
    int n = int(ldexp(c, 24 - emax));
    for (uint k = 0u; k < 8u; ++k) n += int(ldexp(p[k], 24 - emax));
    return rn16_scaled(n, emax - 24);
}
#endif

#if ENC_EXACT
// ENC_EXACT: every product is the reference HMMA.1688.F16 chain in scalar code, one output per call:
// c + sum_k a_k b_k over K in steps of 8, a from LDS at ao + k, b read by BREAD
#define DOT_BODY(BREAD)     float c = float(c0);     for (uint k0 = 0u; k0 < K; k0 += 8u) {         float p[8];         int emax = c != 0.0 ? int((floatBitsToUint(c) >> 23) & 0xffu) - 127 : -1000;         for (uint k = 0u; k < 8u; ++k) {             float16_t a = lds[ao + k0 + k], b = BREAD;             precise float t = float(a) * float(b);             p[k] = t;             if (t != 0.0) emax = max(emax, ilog2_h(a) + ilog2_h(b));         }         if (emax == -1000) continue;         int n = int(ldexp(c, 24 - emax));         for (uint k = 0u; k < 8u; ++k) n += int(ldexp(p[k], 24 - emax));         c = rn16_scaled(n, emax - 24);     }     return float16_t(c);
float16_t dot_g(F16Buf w, uint bo, uint ao, uint K, float16_t c0) { DOT_BODY(w.v[bo + k0 + k]) }
float16_t dot_l(uint bo, uint bk, uint ao, uint K, float16_t c0) { DOT_BODY(lds[bo + (k0 + k) * bk]) }
#endif

#if DEC_DFN || DEC_HEAD
// bilinear 2x of the DFN image below at pixel p in fp16: weights 0.25 / 0.75 by parity, 0 on the first row/column
f16vec3 upsample_below(ivec2 p) {
    ivec2 a = max((p - 1) >> 1, ivec2(0)), b = min(a + 1, pc.lo_size - 1);
    f16vec2 f = f16vec2(p.x == 0 ? 0.0hf : ((p.x & 1) != 0 ? 0.25hf : 0.75hf), p.y == 0 ? 0.0hf : ((p.y & 1) != 0 ? 0.25hf : 0.75hf));
    f16vec2 g = f16vec2(1.0hf) - f;
    f16vec3 r;
    for (int c = 0; c < 3; ++c) {
        float16_t p00 = pc.below.v[uint(a.y * pc.lo_size.x + a.x) * 4u + uint(c)];
        float16_t p01 = pc.below.v[uint(a.y * pc.lo_size.x + b.x) * 4u + uint(c)];
        float16_t p10 = pc.below.v[uint(b.y * pc.lo_size.x + a.x) * 4u + uint(c)];
        float16_t p11 = pc.below.v[uint(b.y * pc.lo_size.x + b.x) * 4u + uint(c)];
        precise float16_t m00 = p00 * g.x, m01 = p01 * f.x, m01y = m01 * g.y;
        precise float16_t acc = fma(m00, g.y, m01y);
        precise float16_t m10 = p10 * g.x;
        acc = fma(m10, f.y, acc);
        precise float16_t m11 = p11 * f.x;
        precise float16_t o = fma(m11, f.y, acc);
        r[c] = o;
    }
    return r;
}

// surface stores of the reference: value cut to 12 (unorm8) / 11 (snorm8) fraction bits, then nearest code
uint unorm8_code(float16_t v) {
#ifdef LT_HM
    return uint(clamp(float(v), 0.0, 1.0) * 255.0 + 0.5);
#endif
    uint t = uint(floor(clamp(float(v), 0.0, 1.0) * 4096.0));
    return (t * 255u + 2048u) >> 12;
}

uint snorm8_code(float16_t v) {
#ifdef LT_HM
    return uint(int(clamp(float(v), -1.0, 1.0) * 127.0 + (v < 0.0hf ? -0.5 : 0.5))) & 0xffu;
#endif
    uint t = uint(floor(min(abs(float(v)), 1.0) * 2048.0));
    int a = int((t * 127u + 1024u) >> 11);
    return uint(v < 0.0hf ? -a : a) & 0xffu;
}

float tanh_f32(float x) {
#ifdef LT_HM
    return 1.0 - 2.0 / (exp2(x * 2.8853900817779268) + 1.0);
#endif
    float a = abs(x), r;
    if (a < 0.0625) {
        float a2 = a * a;
        r = a * (1.0 - a2 * (0.33333334 - a2 * 0.13333334));
    } else {
        float e = exp2(a * 2.8853900817779268);
        r = 1.0 - 2.0 / (e + 1.0);
    }
    return x < 0.0 ? -r : r;
}
#endif

// fp32 accumulator starting from a bias row vector b[n0 .. n0 + 16) (stride 0: every row the same)
ACC bias_acc(F16Buf b, uint n0) {
    MH h;
    coopMatLoad(h, b.v, n0, 0u, ROW);
    return ACC(h);
}

#if ONR_FAST
#include "onr_block_fast.glsl"
#else
void main() {
    ivec2 win = ivec2(int(gl_WorkGroupID.x) % pc.nwx, int(gl_WorkGroupID.x) / pc.nwx);
    uint lid = gl_LocalInvocationIndex;
    float16_t ls1 = uint16BitsToFloat16(uint16_t(pc.ls & 0xffffu));
    float16_t ls2 = uint16BitsToFloat16(uint16_t(pc.ls >> 16));
#if ENC_K8
    MA mlo, mhi;
    coopMatLoad(mlo, pc.mask.v, 0u, 16u, ROW);
    coopMatLoad(mhi, pc.mask.v, 256u, 16u, ROW);
#endif

#if ENC_MERGE_IN
    // ---- input merge (rr2_enc1): token = conv 2x2 stride 2 of the input planes at the reflected pixel.
    // A (64 x 4 CIN) over QO..SS, K = (2 ky + kx) CIN + c; x = A min^T + b straight into XS (fp16, no e4m3)
    for (uint i = lid; i < 64u * 4u * ENC_CIN; i += 128u) {
        uint t = i / (4u * ENC_CIN), k = i % (4u * ENC_CIN), q = k / ENC_CIN, c = k % ENC_CIN;
        bool own;
        ivec2 p = 2 * token_pixel(int(t), win, own) + ivec2(q & 1u, q >> 1);
        lds[QO + i] = e4m3_in(uint(pc.x.v[plane_index_sz(p, arena_channel(int(c)), pc.in_size)]));
    }
    barrier();
#if ENC_EXACT
    for (uint i = lid; i < 64u * ENC_C; i += 128u) {
        uint m = i / ENC_C, n = i % ENC_C;
        lds[XS + i] = dot_g(pc.mrg_w, ENC_CO * 4u * ENC_C + n * 4u * ENC_CIN, QO + m * 4u * ENC_CIN, 4u * ENC_CIN, pc.mrg_b.v[ENC_CO + n]);
    }
    if (false)
#endif
    for (uint tile = sg(); tile < 4u * NT; tile += 4u) {
        uint mt = tile / NT, nt = tile % NT;
        ACC acc = bias_acc(pc.mrg_b, ENC_CO + nt * 16u);
        for (uint k = 0u; k < 4u * ENC_CIN; k += 16u) {
            MA a;
            MB b;
            coopMatLoad(a, lds, QO + mt * 16u * 4u * ENC_CIN + k, 4u * ENC_CIN, ROW);
            coopMatLoad(b, pc.mrg_w.v, ENC_CO * 4u * ENC_C + nt * 16u * 4u * ENC_CIN + k, 4u * ENC_CIN, COL);
            STEP(acc, a, b)
        }
        coopMatStore(MH(acc), lds, XS + mt * 16u * ENC_C + nt * 16u, ENC_C, ROW);
    }
    barrier();
#elif DEC_UP
    // ---- decoder _0: up (Linear + PixelShuffle 2x) of the level below plus the scaled skip. Shifts are even, so
    // the window covers 4x4 low-res pixels (mirrored on the low-res grid) times the 4 sub-pixel positions.
    for (uint i = lid; i < 16u * ENC_CP; i += 128u) {
        uint r = i / ENC_CP, k = i % ENC_CP;
        ivec2 q = (8 * win - pc.shift) / 2 + ivec2(r & 3u, r >> 2);
        q = ivec2(reflect_i(q.x, pc.lo_size.x), reflect_i(q.y, pc.lo_size.y));
        lds[SS + i] = e4m3_in(uint(pc.xlo.v[plane_index_sz(q, arena_channel(int(k)), pc.lo_size)]));
    }
    barrier();
    // U_pos (16 x C) = A up[pos]^T + up_b[pos] -> QO [4][16][C]
#if ENC_EXACT
    for (uint i = lid; i < 64u * ENC_C; i += 128u) {
        uint pos = i / (16u * ENC_C), r = (i / ENC_C) % 16u, n = i % ENC_C;
        lds[QO + i] = dot_g(pc.up_w, (pos * ENC_C + n) * ENC_CP, SS + r * ENC_CP, ENC_CP, pc.up_b.v[pos * ENC_C + n]);
    }
    if (false)
#endif
    for (uint tile = sg(); tile < 4u * NT; tile += 4u) {
        uint pos = tile / NT, nt = tile % NT;
        ACC acc = bias_acc(pc.up_b, pos * ENC_C + nt * 16u);
        for (uint k = 0u; k < ENC_CP; k += 16u) {
            MA a;
            MB b;
            coopMatLoad(a, lds, SS + k, ENC_CP, ROW);
            coopMatLoad(b, pc.up_w.v, (pos * ENC_C + nt * 16u) * ENC_CP + k, ENC_CP, COL);
            STEP(acc, a, b)
        }
        coopMatStore(MH(acc), lds, QO + pos * 16u * ENC_C + nt * 16u, ENC_C, ROW);
    }
    barrier();
    {
        float16_t s = uint16BitsToFloat16(uint16_t(pc.s_skip));
        for (uint i = lid; i < 64u * ENC_C; i += 128u) {
            uint t = i / ENC_C, c = i % ENC_C;
            uint r = ((t >> 4) << 2) + ((t & 7u) >> 1), pos = (((t >> 3) & 1u) << 1) + (t & 1u);
            bool own;
            ivec2 p = token_pixel(int(t), win, own);
            float16_t sk = e4m3_in(uint(pc.x.v[plane_index(p, arena_channel(int(c)))]));
            precise float16_t z = fma(sk, s, lds[QO + pos * 16u * ENC_C + r * ENC_C + c]);
            lds[XS + i] = z;
        }
    }
    barrier();
#elif ENC_EMBED
    // ---- rr2_enc0: 43 attributes per token, dithered to 3 mantissa bits with the blue-noise LUT (indexed by the
    // padded pixel), then the embed MLP: h = gelu(A fc1^T + b) (64 x 64, K = 64 with zeros), x = h fc2^T + b (64 x 32)
    for (uint i = lid; i < 4096u; i += 128u) {
        uint t = i >> 6, k = i & 63u;
        bool own;
        ivec2 p = token_pixel(int(t), win, own);
        float16_t v = 0.0hf;
        if (k < 43u) {
            int sy = p.y < pc.img_h ? p.y : 2 * pc.img_h - 2 - p.y, sx = p.x < pc.img_w ? p.x : 2 * pc.img_w - 2 - p.x;
            float16_t h = pc.attr.v[uint(sy * pc.img_w + sx) * 48u + k];
            uint n = uint(pc.lut.v[pc.frame8 * 256u + uint((p.y & 15) * 16 + (p.x & 15))]) >> 9;
            uint b = uint(float16BitsToUint16(h)), m = b & 0xfc00u;
            precise float16_t tt = h + uint16BitsToFloat16(uint16_t(m | n));
            precise float16_t r = tt - uint16BitsToFloat16(uint16_t(m));
            v = uint16BitsToFloat16(uint16_t(uint(float16BitsToUint16(r)) & 0xff80u));
        }
        lds[QO + i] = v;
    }
    barrier();
#if ENC_EXACT
    for (uint i = lid; i < 4096u; i += 128u)
        lds[SS + i] = gelu_poly(dot_g(pc.mrg_w, (i & 63u) * 64u, QO + (i >> 6) * 64u, 64u, pc.mrg_b.v[i & 63u]));
    barrier();
    for (uint i = lid; i < 2048u; i += 128u)
        lds[XS + i] = dot_g(pc.mrg_w, 4096u + (i & 31u) * 64u, SS + (i >> 5) * 64u, 64u, pc.mrg_b.v[64u + (i & 31u)]);
#else
    for (uint tile = sg(); tile < 16u; tile += 4u) {
        uint mt = tile >> 2, nt = tile & 3u;
        ACC acc = bias_acc(pc.mrg_b, nt * 16u);
        for (uint k = 0u; k < 64u; k += 16u) {
            MA a;
            MB b;
            coopMatLoad(a, lds, QO + mt * 1024u + k, 64u, ROW);
            coopMatLoad(b, pc.mrg_w.v, nt * 1024u + k, 64u, COL);
            STEP(acc, a, b)
        }
        MH h = MH(acc);
        for (int i = 0; i < h.length(); ++i) h[i] = gelu_poly(h[i]);
        coopMatStore(h, lds, SS + mt * 1024u + nt * 16u, 64u, ROW);
    }
    barrier();
    for (uint tile = sg(); tile < 8u; tile += 4u) {
        uint mt = tile >> 1, nt = tile & 1u;
        ACC acc = bias_acc(pc.mrg_b, 64u + nt * 16u);
        for (uint k = 0u; k < 64u; k += 16u) {
            MA a;
            MB b;
            coopMatLoad(a, lds, SS + mt * 1024u + k, 64u, ROW);
            coopMatLoad(b, pc.mrg_w.v, 4096u + nt * 1024u + k, 64u, COL);
            STEP(acc, a, b)
        }
        coopMatStore(MH(acc), lds, XS + mt * 512u + nt * 16u, 32u, ROW);
    }
#endif
    barrier();
#else
    // ---- gather the 64 window tokens (reflected padding), e4m3 -> fp16
    for (uint i = lid; i < 64u * ENC_C; i += 128u) {
        int t = int(i / ENC_C), c = int(i % ENC_C);
        bool own;
        ivec2 p = token_pixel(t, win, own);
        lds[XS + i] = e4m3_in(uint(pc.x.v[plane_index(p, arena_channel(c))]));
    }
    barrier();
#endif

    CUT(1)
    // ---- Q = x in_a^T (64 x X, K = C)
#if ENC_EXACT
    for (uint i = lid; i < 64u * ENC_X; i += 128u)
        lds[QO + i] = dot_g(pc.in_a, (i % ENC_X) * ENC_C, XS + (i / ENC_X) * ENC_C, ENC_C, 0.0hf);
    if (false)
#endif
    for (uint tile = sg(); tile < 4u * (ENC_X / 16); tile += 4u) {
        uint mt = tile / (ENC_X / 16), nt = tile % (ENC_X / 16);
        ACC acc = ACC(0.0);
        for (uint k = 0u; k < ENC_C; k += 16u) {
            MA a;
            MB b;
            coopMatLoad(a, lds, XS + mt * 16u * ENC_C + k, ENC_C, ROW);
            coopMatLoad(b, pc.in_a.v, nt * 16u * ENC_C + k, ENC_C, COL);
            STEP(acc, a, b)
        }
        coopMatStore(MH(acc), lds, QO + mt * 16u * ENC_X + nt * 16u, ENC_X, ROW);
    }
    barrier();

    CUT(2)
    // ---- attention, one head at a time
    for (uint hd = 0u; hd < NH; ++hd) {
        // V_h = x in_b[32 h .. 32 h + 32)^T (64 x 32) and S = Q_h Q_h^T + bias_h (64 x 64): 8 + 16 tiles
#if ENC_EXACT
        for (uint i = lid; i < 6144u; i += 128u) {
            if (i < 2048u) {
                uint m = i >> 5, n = i & 31u;
                lds[VH + i] = dot_g(pc.in_b, (hd * 32u + n) * ENC_C, XS + m * ENC_C, ENC_C, 0.0hf);
            } else {
                uint j = i - 2048u, m = j >> 6, n = j & 63u;
                lds[SS + m * SST + n] = dot_l(QO + n * ENC_X + hd * 32u, 1u, QO + m * ENC_X + hd * 32u, 32u, pc.bias.v[hd * 4096u + j]);
            }
        }
        if (false)
#endif
        for (uint tile = sg(); tile < 24u; tile += 4u) {
            if (tile < 8u) {
                uint mt = tile >> 1, nt = tile & 1u;
                ACC acc = ACC(0.0);
                for (uint k = 0u; k < ENC_C; k += 16u) {
                    MA a;
                    MB b;
                    coopMatLoad(a, lds, XS + mt * 16u * ENC_C + k, ENC_C, ROW);
                    coopMatLoad(b, pc.in_b.v, (hd * 32u + nt * 16u) * ENC_C + k, ENC_C, COL);
                    STEP(acc, a, b)
                }
                coopMatStore(MH(acc), lds, VH + mt * 16u * 32u + nt * 16u, 32u, ROW);
            } else {
                uint mt = (tile - 8u) >> 2, nt = (tile - 8u) & 3u;
                MH bh;
                coopMatLoad(bh, pc.bias.v, hd * 4096u + mt * 16u * 64u + nt * 16u, 64u, ROW);
                ACC acc = ACC(bh);
                for (uint k = 0u; k < 32u; k += 16u) {
                    MA a;
                    MB b;
                    coopMatLoad(a, lds, QO + mt * 16u * ENC_X + hd * 32u + k, ENC_X, ROW);
                    coopMatLoad(b, lds, QO + nt * 16u * ENC_X + hd * 32u + k, ENC_X, COL);
                    STEP(acc, a, b)
                }
                coopMatStore(MH(acc), lds, SS + mt * 16u * SST + nt * 16u, SST, ROW);
            }
        }
        barrier();
        // softmax numerators and fp16 row sums in the reference order, P = num * (1 / sum). Two lanes per row:
        // lane half h owns the columns 8 a + 4 h .. 8 a + 4 h + 3, i.e. the partial sums v[4 h .. 4 h + 3]
        {
            uint row = SS + (lid >> 1) * SST, h4 = (lid & 1u) * 4u;
            float16_t t[8][4], v[4];
            [[unroll]] for (int a = 0; a < 8; ++a)
                [[unroll]] for (int b = 0; b < 4; ++b) t[a][b] = exp_bits(lds[row + uint(8 * a + b) + h4]);
            [[unroll]] for (int b = 0; b < 4; ++b) {
                precise float16_t s = ((t[0][b] + t[2][b]) + (t[4][b] + t[6][b])) + ((t[1][b] + t[3][b]) + (t[5][b] + t[7][b]));
                v[b] = s;
            }
            // b1[2 l + q] = v[2 l + q] + v[2 (l ^ 1) + q] stays inside the half; b2 pairs b1[q] with b1[4 + q]
            precise float16_t b10 = v[0] + v[2], b11 = v[1] + v[3];
            float16_t o0 = subgroupShuffleXor(b10, 1u), o1 = subgroupShuffleXor(b11, 1u);
            precise float16_t b20 = b10 + o0, b21 = b11 + o1;
            precise float16_t sum = b20 + b21;
            float16_t r = float16_t(rcp_rn(float(sum)));
            [[unroll]] for (int a = 0; a < 8; ++a)
                [[unroll]] for (int b = 0; b < 4; ++b) { precise float16_t p = t[a][b] * r; lds[row + uint(8 * a + b) + h4] = p; }
        }
        barrier();
        // O_h = P V_h (64 x 32, K = 64) over the Q_h columns
#if ENC_EXACT
        for (uint i = lid; i < 2048u; i += 128u) {
            uint m = i >> 5, n = i & 31u;
            lds[QO + m * ENC_X + hd * 32u + n] = dot_l(VH + n, 32u, SS + m * SST, 64u, 0.0hf);
        }
#elif ENC_PVX
        // exact HMMA.1688.F16 chain in scalar code: thread = 1 row x 16 columns
        {
            uint i = lid >> 1, j0 = (lid & 1u) * 16u;
            float acc[16];
            for (int j = 0; j < 16; ++j) acc[j] = 0.0;
            for (uint s = 0u; s < 64u; s += 8u) {
                float pv[8];
                int pe[8];
                for (uint k = 0u; k < 8u; ++k) {
                    float16_t h = lds[SS + i * SST + s + k];
                    pv[k] = float(h);
                    pe[k] = ilog2_h(h);
                }
                for (uint j = 0u; j < 16u; ++j) acc[j] = hmma_step8(pv, pe, VH + s * 32u + j0 + j, 32u, acc[j]);
            }
            for (uint j = 0u; j < 16u; ++j) lds[QO + i * ENC_X + hd * 32u + j0 + j] = float16_t(acc[j]);
        }
#else
        for (uint tile = sg(); tile < 8u; tile += 4u) {
            uint mt = tile >> 1, nt = tile & 1u;
            ACC acc = ACC(0.0);
            for (uint k = 0u; k < 64u; k += 16u) {
                MA a;
                MB b;
                coopMatLoad(a, lds, SS + mt * 16u * SST + k, SST, ROW);
                coopMatLoad(b, lds, VH + k * 32u + nt * 16u, 32u, ROW);
                STEP(acc, a, b)
            }
            coopMatStore(MH(acc), lds, QO + mt * 16u * ENC_X + hd * 32u + nt * 16u, ENC_X, ROW);
        }
#endif
        barrier();
    }

    CUT(3)
    // ---- x1 = fma(O out^T + out_b, ls1, x), in place in XS
#if ENC_EXACT
    for (uint i = lid; i < 64u * ENC_C; i += 128u) {
        uint m = i / ENC_C, n = i % ENC_C;
        float16_t a = dot_g(pc.wout, n * ENC_X, QO + m * ENC_X, ENC_X, pc.out_b.v[n]);
        precise float16_t v = fma(a, ls1, lds[XS + i]);
        lds[XS + i] = v;
    }
    barrier();
    // MLP: the fc2 chain accumulators live in QO as [64][C] (the HMMA accumulator is fp16 anyway)
    for (uint i = lid; i < 64u * ENC_C; i += 128u) lds[QO + i] = pc.fc2_b.v[i % ENC_C];
    for (uint j = 0u; j < HID; j += 32u) {
        for (uint i = lid; i < 2048u; i += 128u) {
            uint m = i >> 5, n = i & 31u;
            lds[VH + i] = gelu_poly(dot_g(pc.fc1, (j + n) * ENC_C, XS + m * ENC_C, ENC_C, pc.fc1_b.v[j + n]));
        }
        barrier();
        for (uint i = lid; i < 64u * ENC_C; i += 128u)
            lds[QO + i] = dot_g(pc.fc2, (i % ENC_C) * HID + j, VH + (i / ENC_C) * 32u, 32u, lds[QO + i]);
        barrier();
    }
    for (uint i = lid; i < 64u * ENC_C; i += 128u) { precise float16_t v = fma(lds[QO + i], ls2, lds[XS + i]); lds[QO + i] = v; }
#else
    for (uint tile = sg(); tile < 4u * NT; tile += 4u) {
        uint mt = tile / NT, nt = tile % NT;
        ACC acc = bias_acc(pc.out_b, nt * 16u);
        for (uint k = 0u; k < ENC_X; k += 16u) {
            MA a;
            MB b;
            coopMatLoad(a, lds, QO + mt * 16u * ENC_X + k, ENC_X, ROW);
            coopMatLoad(b, pc.wout.v, nt * 16u * ENC_X + k, ENC_X, COL);
            STEP(acc, a, b)
        }
        MH a16 = MH(acc), xt;
        coopMatLoad(xt, lds, XS + mt * 16u * ENC_C + nt * 16u, ENC_C, ROW);
        for (int i = 0; i < a16.length(); ++i) { precise float16_t v = fma(a16[i], ls1, xt[i]); xt[i] = v; }
        coopMatStore(xt, lds, XS + mt * 16u * ENC_C + nt * 16u, ENC_C, ROW);
    }
    barrier();

    CUT(4)
    // ---- MLP in 32-neuron slices; subgroup s owns row tile s, its NT fc2 tiles stay in named accumulators
    // (an array of coopmats, or a guard around their update, crashes LLPC 26.10)
#define DECL(cn, n) ACC cn = bias_acc(pc.fc2_b, (n) * 16u);
    DECL(c0, 0u) DECL(c1, 1u)
#if NT > 2
    DECL(c2, 2u) DECL(c3, 3u)
#endif
#if NT > 4
    DECL(c4, 4u) DECL(c5, 5u)
#endif
#if NT > 6
    DECL(c6, 6u) DECL(c7, 7u)
#endif
#if NT > 8
    DECL(c8, 8u) DECL(c9, 9u)
#endif
    for (uint j = 0u; j < HID; j += 32u) {
        // hidden slice h = gelu(x1 fc1[j:j+32]^T + b) -> VH as [64][32]
        for (uint tile = sg(); tile < 8u; tile += 4u) {
            uint mt = tile >> 1, nt = tile & 1u;
            ACC acc = bias_acc(pc.fc1_b, j + nt * 16u);
            for (uint k = 0u; k < ENC_C; k += 16u) {
                MA a;
                MB b;
                coopMatLoad(a, lds, XS + mt * 16u * ENC_C + k, ENC_C, ROW);
                coopMatLoad(b, pc.fc1.v, (j + nt * 16u) * ENC_C + k, ENC_C, COL);
                STEP(acc, a, b)
            }
            MH h = MH(acc);
            for (int i = 0; i < h.length(); ++i) h[i] = gelu_poly(h[i]);
            coopMatStore(h, lds, VH + mt * 16u * 32u + nt * 16u, 32u, ROW);
        }
        barrier();
        MA a0, a1;
        coopMatLoad(a0, lds, VH + sg() * 16u * 32u, 32u, ROW);
        coopMatLoad(a1, lds, VH + sg() * 16u * 32u + 16u, 32u, ROW);
#define FC2(cn, n) { MB b; \
        coopMatLoad(b, pc.fc2.v, (n) * 16u * HID + j, HID, COL); STEP(cn, a0, b) \
        coopMatLoad(b, pc.fc2.v, (n) * 16u * HID + j + 16u, HID, COL); STEP(cn, a1, b) }
        FC2(c0, 0u) FC2(c1, 1u)
#if NT > 2
        FC2(c2, 2u) FC2(c3, 3u)
#endif
#if NT > 4
        FC2(c4, 4u) FC2(c5, 5u)
#endif
#if NT > 6
        FC2(c6, 6u) FC2(c7, 7u)
#endif
#if NT > 8
        FC2(c8, 8u) FC2(c9, 9u)
#endif
        barrier();
    }

    // ---- x2 = fma(m, ls2, x1) staged in QO as [64][C], then e4m3 packing of the pixels this window owns
#define OUT(cn, n) { MH m16 = MH(cn), xt; \
        coopMatLoad(xt, lds, XS + sg() * 16u * ENC_C + (n) * 16u, ENC_C, ROW); \
        for (int i = 0; i < m16.length(); ++i) { precise float16_t v = fma(m16[i], ls2, xt[i]); xt[i] = v; } \
        coopMatStore(xt, lds, QO + sg() * 16u * ENC_C + (n) * 16u, ENC_C, ROW); }
    OUT(c0, 0u) OUT(c1, 1u)
#if NT > 2
    OUT(c2, 2u) OUT(c3, 3u)
#endif
#if NT > 4
    OUT(c4, 4u) OUT(c5, 5u)
#endif
#if NT > 6
    OUT(c6, 6u) OUT(c7, 7u)
#endif
#if NT > 8
    OUT(c8, 8u) OUT(c9, 9u)
#endif
#endif
    CUT(5)
    barrier();
#if !DEC_HEAD
    for (uint i = lid; i < 64u * ENC_C; i += 128u) {
        int t = int(i / ENC_C), c = int(i % ENC_C);
        bool own;
        ivec2 p = token_pixel(t, win, own);
        if (own) pc.y.v[plane_index(p, arena_channel(c))] = uint8_t(e4m3_out(lds[QO + i]));
    }
#endif
    CUT(6)
#if DEC_DFN || DEC_HEAD
#if DEC_DFN
    // decoder DFN pre-activations d = x2 dfn^T + b (64 x 16, 4 used) -> VH [64][16]
#if ENC_EXACT
    for (uint i = lid; i < 256u; i += 128u)
        lds[VH + (i >> 2) * 16u + (i & 3u)] = dot_g(pc.dfn_w, (i & 3u) * ENC_C, QO + (i >> 2) * ENC_C, ENC_C, pc.dfn_b.v[i & 3u]);
    if (false)
#endif
    {
        ACC acc = bias_acc(pc.dfn_b, 0u);
        for (uint k = 0u; k < ENC_C; k += 16u) {
            MA a;
            MB b;
            coopMatLoad(a, lds, QO + sg() * 16u * ENC_C + k, ENC_C, ROW);
            coopMatLoad(b, pc.dfn_w.v, k, ENC_C, COL);
            STEP(acc, a, b)
        }
        coopMatStore(MH(acc), lds, VH + sg() * 16u * 16u, 16u, ROW);
    }
    barrier();
#define DPRE(t, c) lds[VH + (t) * 16u + uint(c)]
#else
#define DPRE(t, c) lds[QO + (t) * ENC_C + uint(c)]
#endif
    // up = bilinear(below) * fp16(sig(d_c) s); out = min(fma(skip, 1 - sig(d3), up * sig(d3)), 65504)
    if (lid < 64u) {
        bool own;
        ivec2 p = token_pixel(int(lid), win, own);
        if (own && p.y < pc.out_h) {
            uint pi = uint(p.y * pc.size.x + p.x) * 4u;
            float16_t s = uint16BitsToFloat16(uint16_t(pc.s_dfn));
            float16_t g3 = sigmoid16(DPRE(lid, 3));
            precise float16_t omg = 1.0hf - g3;
            f16vec3 u = upsample_below(p);
            for (int c = 0; c < 3; ++c) {
                float16_t g = sigmoid16(DPRE(lid, c));
                precise float16_t gs = g * s;
                precise float16_t ug = u[c] * gs;
                precise float16_t t = ug * g3;
                precise float16_t o = fma(pc.skip.v[pi + uint(c)], omg, t);
                pc.dfn_out.v[pi + uint(c)] = min(o, 65504.0hf);
            }
            precise float16_t ow = pc.skip.v[pi + 3u] * omg;
            pc.dfn_out.v[pi + 3u] = min(ow, 65504.0hf);
#if DEC_HEAD
            if (p.x < pc.out_w) {
            pi = uint(p.y * pc.out_w + p.x) * 4u;
            // block channels (GPU order): 4-6 AjabGateOgate, 7-9 Aniso, 10-13 MvecFactors, 14-17 HiddenHistory
            for (int c = 0; c < 3; ++c) pc.ajab.v[pi + uint(c)] = uint8_t(unorm8_code(sigmoid16(DPRE(lid, 4 + c))));
            pc.ajab.v[pi + 3u] = uint8_t(0);
            // softmax of 4 logits: EX2.F16 (rounded toward zero), fp16 sum (e0 + e2) + (e1 + e3), f32 RCP
            float16_t m[4], e[4];
            for (int c = 0; c < 4; ++c) m[c] = DPRE(lid, 10 + c);
            float16_t mx = max(max(m[0], m[2]), max(m[1], m[3]));
            for (int c = 0; c < 4; ++c) {
                precise float16_t d = m[c] - mx;
                precise float16_t z = d * 1.4423828125hf;
                e[c] = half_rz(exp2(float(z)));
            }
            precise float16_t s02 = e[0] + e[2], s13 = e[1] + e[3], se = s02 + s13;
            float16_t r = float16_t(rcp_rn(float(se)));
            for (int c = 0; c < 4; ++c) { precise float16_t v = e[c] * r; pc.mvf.v[pi + uint(c)] = uint8_t(unorm8_code(v)); }
            // TANH.F16 rounded toward zero
            for (int c = 0; c < 4; ++c) pc.hidden.v[pi + uint(c)] = uint8_t(snorm8_code(half_rz(tanh_f32(float(DPRE(lid, 14 + c))))));
            // Aniso: a = k sig(clamp(u, +-20)), b likewise, (a^2, b^2, a b tanh(k_t t), 0)
            precise float a = float(sigmoid16(clamp(DPRE(lid, 7), -20.0hf, 20.0hf))) * pc.k_scale;
            precise float b = float(sigmoid16(clamp(DPRE(lid, 8), -20.0hf, 20.0hf))) * pc.k_scale;
            precise float tt = float(DPRE(lid, 9)) * pc.k_tanh;
            float th = tanh_f32(tt);
            precise float aa = a * a, bb = b * b, ab = a * b, abt = ab * th;
            pc.aniso.v[pi + 0u] = float16_t(aa);
            pc.aniso.v[pi + 1u] = float16_t(bb);
            pc.aniso.v[pi + 2u] = float16_t(abt);
            pc.aniso.v[pi + 3u] = 0.0hf;
            }
#endif
        }
    }
#endif
#if ENC_STAGE
    // ---- merge input in XS (free now): row r = pooled pixel (r / 4, r % 4) of the window, K = (2 ky + kx) C + c
#if ENC_CO > 0
    for (uint i = lid; i < 64u * ENC_C; i += 128u) {
        uint r = i / (4u * ENC_C), k = i % (4u * ENC_C), q = k / ENC_C, c = k % ENC_C;
        uint tok = (2u * (r >> 2) + (q >> 1)) * 8u + 2u * (r & 3u) + (q & 1u);
        lds[XS + i] = lds[QO + tok * ENC_C + c];
    }
#endif
    barrier();
    // DFN pre-activations d = x2 dfn^T + b (64 x 16, 3 used) -> VH [64][16]
#if ENC_EXACT
    for (uint i = lid; i < 192u; i += 128u)
        lds[VH + (i / 3u) * 16u + (i % 3u)] = dot_g(pc.dfn_w, (i % 3u) * ENC_C, QO + (i / 3u) * ENC_C, ENC_C, pc.dfn_b.v[i % 3u]);
    if (false)
#endif
    {
        ACC acc = bias_acc(pc.dfn_b, 0u);
        for (uint k = 0u; k < ENC_C; k += 16u) {
            MA a;
            MB b;
            coopMatLoad(a, lds, QO + sg() * 16u * ENC_C + k, ENC_C, ROW);
            coopMatLoad(b, pc.dfn_w.v, k, ENC_C, COL);
            STEP(acc, a, b)
        }
        coopMatStore(MH(acc), lds, VH + sg() * 16u * 16u, 16u, ROW);
    }
#if ENC_CO > 0
    // merge: pooled (16 x Co) = A (16 x 4C) mrg^T + b -> SS [16][Co]
#if ENC_EXACT
    for (uint i = lid; i < 16u * ENC_CO; i += 128u)
        lds[SS + i] = dot_g(pc.mrg_w, (i % ENC_CO) * 4u * ENC_C, XS + (i / ENC_CO) * 4u * ENC_C, 4u * ENC_C, pc.mrg_b.v[i % ENC_CO]);
    if (false)
#endif
    for (uint nt = sg(); nt < ENC_CO / 16u; nt += 4u) {
        ACC acc = bias_acc(pc.mrg_b, nt * 16u);
        for (uint k = 0u; k < 4u * ENC_C; k += 16u) {
            MA a;
            MB b;
            coopMatLoad(a, lds, XS + k, 4u * ENC_C, ROW);
            coopMatLoad(b, pc.mrg_w.v, nt * 16u * 4u * ENC_C + k, 4u * ENC_C, COL);
            STEP(acc, a, b)
        }
        coopMatStore(MH(acc), lds, SS + nt * 16u, ENC_CO, ROW);
    }
#endif
    barrier();
    // DFN gains: g = fp16(1 / (1 + 2^(-log2(e) d))) * s; out.rgb = min(img.rgb * g, 65504), out.a = img.a * 0
    if (lid < 64u) {
        bool own;
        ivec2 p = token_pixel(int(lid), win, own);
        f16vec4 o = f16vec4(0.0hf);
        if (own) {
            uint pi = uint(p.y * pc.size.x + p.x) * 4u;
            float16_t s = uint16BitsToFloat16(uint16_t(pc.s_dfn));
            for (int c = 0; c < 3; ++c) {
                precise float e = exp2(float(lds[VH + lid * 16u + uint(c)]) * uintBitsToFloat(0xbfb8aa3bu));
                float16_t g = float16_t(rcp_rn(e + 1.0));
                precise float16_t gs = g * s;
                precise float16_t v = pc.dfn_in.v[pi + uint(c)] * gs;
                o[c] = min(v, 65504.0hf);
                pc.dfn_out.v[pi + uint(c)] = o[c];
            }
            precise float16_t a = pc.dfn_in.v[pi + 3u] * 0.0hf;
            o.w = a;
            pc.dfn_out.v[pi + 3u] = a;
        }
        for (int c = 0; c < 4; ++c) lds[VH + 1024u + lid * 4u + uint(c)] = o[c];
    }
#if ENC_CO > 0
    // PooledOutput: e4m3 of the pooled pixels the window owns, channel n (GPU order) to its arena channel
    for (uint i = lid; i < 16u * ENC_CO; i += 128u) {
        uint r = i / ENC_CO, n = i % ENC_CO;
        bool own;
        ivec2 p = token_pixel(int((2u * (r >> 2)) * 8u + 2u * (r & 3u)), win, own);
        if (!own) continue;
        ivec2 q = p >> 1, ps = pc.size >> 1;
        int an = arena_channel(int(n));
        uint idx = uint(an >> 5) * uint(ps.x * ps.y * 32) + uint((q.y * ps.x + q.x) * 32 + (an & 31));
        pc.pooled.v[idx] = uint8_t(e4m3_out(lds[SS + i]));
    }
#endif
    barrier();
    // DFNPooledOutput: ((p00 + p01) + (p10 + p11)) * 0.25 in fp16 (enc0: each texel * 0.25 first, at most 65504)
    if ((ENC_CO > 0 || ENC_EMBED != 0) && lid < 16u) {
        uint t0 = (2u * (lid >> 2)) * 8u + 2u * (lid & 3u);
        bool own;
        ivec2 p = token_pixel(int(t0), win, own);
        if (own && p.x + 1 < pc.size.x && p.y + 1 < pc.size.y) {
            ivec2 q = p >> 1;
            uint pi = uint(q.y * (pc.size.x >> 1) + q.x) * 4u;
            for (uint c = 0u; c < 4u; ++c) {
#if ENC_EMBED
                precise float16_t q00 = lds[VH + 1024u + t0 * 4u + c] * 0.25hf, q01 = lds[VH + 1024u + (t0 + 1u) * 4u + c] * 0.25hf;
                precise float16_t q10 = lds[VH + 1024u + (t0 + 8u) * 4u + c] * 0.25hf, q11 = lds[VH + 1024u + (t0 + 9u) * 4u + c] * 0.25hf;
                precise float16_t v = (q00 + q01) + (q10 + q11);
                pc.dfn_pool.v[pi + c] = min(v, 65504.0hf);
#else
                precise float16_t s0 = lds[VH + 1024u + t0 * 4u + c] + lds[VH + 1024u + (t0 + 1u) * 4u + c];
                precise float16_t s1 = lds[VH + 1024u + (t0 + 8u) * 4u + c] + lds[VH + 1024u + (t0 + 9u) * 4u + c];
                precise float16_t v = (s0 + s1) * 0.25hf;
                pc.dfn_pool.v[pi + c] = v;
#endif
            }
        }
    }
#endif
}
#endif
