// SPDX-License-Identifier: MIT
// Two subgroups per window (W2: C = 64 and 96, fast variant), included by onr_block_fast.glsl in place of its main.
// Subgroup w holds the 32 tokens 32 w .. 32 w + 31 (token tiles 2 w, 2 w + 1) as x^T, 2 token tiles x NT channel
// tiles in registers, and does every row-local product for them with each weight fragment serving both tiles.
// Per head only the keys cross: each subgroup stores its Q_h (token rows) and V_h^T into one LDS buffer, a barrier
// publishes them and every key tile is read back as an A operand in key order (one more barrier before the next head
// overwrites them). The prologue (decoder _0 up, enc1 input merge) and the heads work from registers; the enc1 merge to
// the next level goes through the LDS. Same products, same K order, same roundings as the transposed body.

#define CPB(b, h) { for (int i_ = 0; i_ < h.length(); ++i_) b[i_] = h[i_]; }
#define CPA(a, h) { for (int i_ = 0; i_ < h.length(); ++i_) a[i_] = h[i_]; }
#define MMA(acc, a, b) acc = coopMatMulAdd(a, b, acc);
#define W2Q 0u   // Q_h [64 tokens][QST]
#define W2V (64u * QST)   // V_h^T [32 dims][VST]
#ifdef PROF_NOGW
// timing only: weight fragments read from the LDS (values meaningless)
#define WLD(m, buf, off, stride, lay) coopMatLoad(m, lds, ((off) & 1023u) & ~15u, 16u, lay)
#else
#define WLD(m, buf, off, stride, lay) coopMatLoad(m, buf, off, stride, lay)
#endif
// 16 record bytes of a 32-channel plane -> the lane's elements of its two channel tiles (as XIN)
#define REC2(xa, xb, w_) [[unroll]] for (int k_ = 0; k_ < 4; ++k_) { \
        f16vec2 a_ = unpackFloat2x16(e4m3_lo(w_[k_])), b_ = unpackFloat2x16(e4m3_hi(w_[k_])); \
        xa[2 * k_] = a_.x, xa[2 * k_ + 1] = a_.y, xb[2 * k_] = b_.x, xb[2 * k_ + 1] = b_.y; }
// channel tiles n (x0n, x1n: own token tiles a, b) and planes (pl: tiles a, b)
#if NT > 4
#define W2T45(M) M(4) M(5)
#define W2P2(M) M(2, 4, 5)
#else
#define W2T45(M)
#define W2P2(M)
#endif
#define W2T(M) M(0) M(1) M(2) M(3) W2T45(M)
#define W2P(M) M(0, 0, 1) M(1, 2, 3) W2P2(M)
// C = 96: the out projection after both token tiles' attention (each wout fragment loaded once, at use) and the
// MLP in 32-neuron slices
#ifndef W2_DEFER
#if NT > 4
#define W2_DEFER 1
#else
#define W2_DEFER 0
#endif
#endif
#ifndef W2_SL
#if NT > 4
#define W2_SL 32
#else
#define W2_SL 64
#endif
#endif

#if FLOW
#include "onr_flow.glsl"
void block_main(ivec2 win);
void main() {
#if DEC_UP
    int wid_ = pc.nwx * ((pc.size.y + pc.shift.y + 7) / 8) - 1 - int(gl_WorkGroupID.x);
#else
    int wid_ = int(gl_WorkGroupID.x);
#endif
    ivec2 win = ivec2(wid_ % pc.nwx, wid_ / pc.nwx);
    flow_wait(win);
    block_main(win);
    flow_signal(win);
}
void block_main(ivec2 win) {
#else
void main() {
#if DEC_UP
    // dec1_0 walks the grid backwards, so dec1_1 (forwards) starts on the rows dec1_0 wrote last (measured: the DLAA
    // frame -0.03 ms); the enc1 skip it reads comes from DRAM either way
    int wid_ = pc.nwx * ((pc.size.y + pc.shift.y + 7) / 8) - 1 - int(gl_WorkGroupID.x);
#else
    int wid_ = int(gl_WorkGroupID.x);
#endif
    ivec2 win = ivec2(wid_ % pc.nwx, wid_ / pc.nwx);
#endif
    uint w = gl_SubgroupID, ln = gl_SubgroupInvocationID, hh = ln >> 4;
    uint ra = 32u * w, rb = ra + 16u;
    float16_t ls1 = uint16BitsToFloat16(uint16_t(pc.ls & 0xffffu));
    float16_t ls2 = uint16BitsToFloat16(uint16_t(pc.ls >> 16));
    // lane l: token r + l % 16 of each own token tile, GPU channels 8 h .. 8 h + 7 of each channel tile
    bool towna, townb;
    ivec2 tpa = token_pixel(int(ra + (ln & 15u)), win, towna), tpb = token_pixel(int(rb + (ln & 15u)), win, townb);
#if DEC_UP
    // U^T [channels][16 low-res pixels] per PixelShuffle position = up_pos A^T + b, A^T straight from the planes below
    // (lane: pixel l % 16); subgroup w does positions 2 w, 2 w + 1 into the LDS [4][16][C], read by both
    {
        ivec2 q = (8 * win - pc.shift) / 2 + ivec2(ln & 3u, (ln >> 2) & 3u);
        q = ivec2(reflect_i(q.x, pc.lo_size.x), reflect_i(q.y, pc.lo_size.y));
        U32x4View lo = U32x4View(uint64_t(pc.xlo));
        uvec4 w0 = lo.v[plane_index_sz(q, int(hh) * 16, pc.lo_size) >> 4], w1 = lo.v[plane_index_sz(q, 32 + int(hh) * 16, pc.lo_size) >> 4];
        uvec4 w2 = lo.v[plane_index_sz(q, 64 + int(hh) * 16, pc.lo_size) >> 4];
        MB a0, a1, a2, a3, a4, a5;

        REC2(a0, a1, w0) REC2(a2, a3, w1) REC2(a4, a5, w2)
#if ENC_CP > 96
        uvec4 w3 = lo.v[plane_index_sz(q, 96 + int(hh) * 16, pc.lo_size) >> 4];
        MB a6, a7;
        REC2(a6, a7, w3)
#define UP_K3(o_) coopMatLoad(w_, pc.up_w.v, o_ + 96u, ENC_CP, ROW); MMA(u, w_, a6) coopMatLoad(w_, pc.up_w.v, o_ + 112u, ENC_CP, ROW); MMA(u, w_, a7)
#else
#define UP_K3(o_)
#endif
#define UP_T(p, n) { ACC u = bias_acc_t(pc.up_b, (p) * ENC_C + (n) * 16u); MA w_; uint o_ = ((p) * ENC_C + (n) * 16u) * ENC_CP; \
            coopMatLoad(w_, pc.up_w.v, o_, ENC_CP, ROW); MMA(u, w_, a0) coopMatLoad(w_, pc.up_w.v, o_ + 16u, ENC_CP, ROW); MMA(u, w_, a1) \
            coopMatLoad(w_, pc.up_w.v, o_ + 32u, ENC_CP, ROW); MMA(u, w_, a2) coopMatLoad(w_, pc.up_w.v, o_ + 48u, ENC_CP, ROW); MMA(u, w_, a3) \
            coopMatLoad(w_, pc.up_w.v, o_ + 64u, ENC_CP, ROW); MMA(u, w_, a4) coopMatLoad(w_, pc.up_w.v, o_ + 80u, ENC_CP, ROW); MMA(u, w_, a5) UP_K3(o_) \
            MH h_ = MH(u); lds4[(((p) * 16u + (ln & 15u)) * ENC_C + (n) * 16u + 8u * hh) >> 3] = uvec4(packFloat2x16(f16vec2(h_[0], h_[1])), \
                packFloat2x16(f16vec2(h_[2], h_[3])), packFloat2x16(f16vec2(h_[4], h_[5])), packFloat2x16(f16vec2(h_[6], h_[7]))); }
        uint p0 = 2u * w, p1 = p0 + 1u;
#define UP_P0(n) UP_T(p0, uint(n))
#define UP_P1(n) UP_T(p1, uint(n))
#ifndef PROF_NOUP
        W2T(UP_P0) W2T(UP_P1)
#endif
    }
    WG_SYNC();
    // x^T = fma(skip, s, U) per lane: the skip's record bytes as in XIN, U of the token (position, low-res pixel)
    float16_t sc = uint16BitsToFloat16(uint16_t(pc.s_skip));
#define XIN2(xa, xb, tp, t0, pl) MH xa, xb; { uvec4 w_ = U32x4View(uint64_t(pc.x)).v[plane_index(tp, int(pl) * 32 + int(hh) * 16) >> 4]; \
        uint t_ = (t0) + (ln & 15u), ua_ = (((((t_ >> 3) & 1u) << 1) + (t_ & 1u)) * 16u * ENC_C + (((t_ >> 4) << 2) + ((t_ & 7u) >> 1)) * ENC_C + (pl) * 32u + 8u * hh) >> 1; \
        [[unroll]] for (int k_ = 0; k_ < 4; ++k_) { \
            f16vec2 a_ = unpackFloat2x16(e4m3_lo(w_[k_])), b_ = unpackFloat2x16(e4m3_hi(w_[k_])); \
            f16vec2 ua = unpackFloat2x16(lds1[ua_ + uint(k_)]), ubv = unpackFloat2x16(lds1[ua_ + 8u + uint(k_)]); \
            precise f16vec2 za = fma(a_, f16vec2(sc), ua), zb = fma(b_, f16vec2(sc), ubv); \
            xa[2 * k_] = za.x, xa[2 * k_ + 1] = za.y, xb[2 * k_] = zb.x, xb[2 * k_ + 1] = zb.y; } }
#elif ENC_MERGE_IN
#if NT != 4
#error "W2 input merge: C = 64 only"
#endif
    // rr2_enc1 input merge, transposed: x^T [channel tile][token tile] = mrg_in A^T + b, K = (2 ky + kx) CIN + c; the
    // B operands come straight from the enc0 planes (lane: token l % 16, 16 record bytes per input pixel = K tiles
    // 2 q, 2 q + 1), each weight fragment serves both token tiles
    ACC mi00 = bias_acc_t(pc.mrg_b, ENC_CO), mi01 = bias_acc_t(pc.mrg_b, ENC_CO + 16u), mi02 = bias_acc_t(pc.mrg_b, ENC_CO + 32u), mi03 = bias_acc_t(pc.mrg_b, ENC_CO + 48u);
    ACC mi10 = mi00, mi11 = mi01, mi12 = mi02, mi13 = mi03;
    {
        uvec4 wa[4], wb[4];
        [[unroll]] for (uint q = 0u; q < 4u; ++q) {
            ivec2 o = ivec2(q & 1u, q >> 1);
            wa[q] = U32x4View(uint64_t(pc.x)).v[plane_index_sz(2 * tpa + o, int(hh) * 16, pc.in_size) >> 4];
            wb[q] = U32x4View(uint64_t(pc.x)).v[plane_index_sz(2 * tpb + o, int(hh) * 16, pc.in_size) >> 4];
        }
#define MI_K(kt, ba, bb) { MA m_; uint o_ = ENC_CO * 4u * ENC_C + (kt) * 16u; \
            WLD(m_, pc.mrg_w.v, o_, 4u * ENC_CIN, ROW); MMA(mi00, m_, ba) MMA(mi10, m_, bb) \
            WLD(m_, pc.mrg_w.v, o_ + 16u * 4u * ENC_CIN, 4u * ENC_CIN, ROW); MMA(mi01, m_, ba) MMA(mi11, m_, bb) \
            WLD(m_, pc.mrg_w.v, o_ + 32u * 4u * ENC_CIN, 4u * ENC_CIN, ROW); MMA(mi02, m_, ba) MMA(mi12, m_, bb) \
            WLD(m_, pc.mrg_w.v, o_ + 48u * 4u * ENC_CIN, 4u * ENC_CIN, ROW); MMA(mi03, m_, ba) MMA(mi13, m_, bb) }
        [[unroll]] for (uint q = 0u; q < 4u; ++q) {
            MB ba0, ba1, bb0, bb1;
            REC2(ba0, ba1, wa[q]) REC2(bb0, bb1, wb[q])
            MI_K(2u * q, ba0, bb0) MI_K(2u * q + 1u, ba1, bb1)
        }
    }
    MH x00 = MH(mi00), x01 = MH(mi01), x02 = MH(mi02), x03 = MH(mi03), x10 = MH(mi10), x11 = MH(mi11), x12 = MH(mi12), x13 = MH(mi13);
#define XIN2(xa, xb, tp, t0, pl)
#else
#define XIN2(xa, xb, tp, t0, pl) MH xa, xb; { uvec4 w_ = U32x4View(uint64_t(pc.x)).v[plane_index(tp, int(pl) * 32 + int(hh) * 16) >> 4]; \
        [[unroll]] for (int k_ = 0; k_ < 4; ++k_) { \
            f16vec2 a_ = unpackFloat2x16(e4m3_lo(w_[k_])), b_ = unpackFloat2x16(e4m3_hi(w_[k_])); \
            xa[2 * k_] = a_.x, xa[2 * k_ + 1] = a_.y, xb[2 * k_] = b_.x, xb[2 * k_ + 1] = b_.y; } }
#if FP8QV
    // and the same bytes as e4m3 B tiles (OCP codes), as the transposed body's XIN8
#define XIN2Y(xa, xb, ya, yb, tp, pl) MH xa, xb; QB8 ya, yb; { uvec4 w_ = U32x4View(uint64_t(pc.x)).v[plane_index(tp, int(pl) * 32 + int(hh) * 16) >> 4]; \
        [[unroll]] for (int k_ = 0; k_ < 4; ++k_) { \
            f16vec2 a_ = unpackFloat2x16(e4m3_lo(w_[k_])), b_ = unpackFloat2x16(e4m3_hi(w_[k_])); \
            xa[2 * k_] = a_.x, xa[2 * k_ + 1] = a_.y, xb[2 * k_] = b_.x, xb[2 * k_ + 1] = b_.y; \
            uint o_ = ocp4(w_[k_]); \
            ya[2 * k_] = uintBitsToFloate4m3EXT(uint8_t(o_ & 0xffu)), ya[2 * k_ + 1] = uintBitsToFloate4m3EXT(uint8_t((o_ >> 8) & 0xffu)); \
            yb[2 * k_] = uintBitsToFloate4m3EXT(uint8_t((o_ >> 16) & 0xffu)), yb[2 * k_ + 1] = uintBitsToFloate4m3EXT(uint8_t(o_ >> 24)); } }
#endif
#endif
    // x^T tile names: x<own token tile><channel tile>; out accumulators c<own token tile><channel tile>
#if FP8QV
#define XIN_A(pl, a, b) XIN2Y(x0##a, x0##b, y0##a, y0##b, tpa, uint(pl))
#define XIN_B(pl, a, b) XIN2Y(x1##a, x1##b, y1##a, y1##b, tpb, uint(pl))
#else
#define XIN_A(pl, a, b) XIN2(x0##a, x0##b, tpa, ra, uint(pl))
#define XIN_B(pl, a, b) XIN2(x1##a, x1##b, tpb, rb, uint(pl))
#endif
    W2P(XIN_A) W2P(XIN_B)
#define DECL_C0(n) ACC c0##n = bias_acc_t(pc.out_b, uint(n) * 16u);
#define DECL_C1(n) ACC c1##n = c0##n;
    W2T(DECL_C0) W2T(DECL_C1)
#if DEC_UP
    // both subgroups have read U before head 0 overwrites it (a barrier inside the head loop costs the weight prefetch)
    WG_SYNC();
#endif
    for (uint hd = 0u; hd < LT_HEADS; ++hd) {
        uint wq = hd * 32u * ENC_C;
#if FP8QV
        // Q^T, V^T [dim tile][token tile] = in_a x^T, in_b x^T on e4m3 x e4m3 -> fp32, one rounding to fp16
        ACC q00, q01, q10, q11, v00, v01, v10, v11;
        {
            uint64_t wa8 = uint64_t(pc.in_a) + uint64_t(QV8_OFF), wb8 = uint64_t(pc.in_b) + uint64_t(QV8_OFF);
            QC8 fq00 = QC8(0.0), fq01 = QC8(0.0), fq10 = QC8(0.0), fq11 = QC8(0.0);
            QC8 fv00 = QC8(0.0), fv01 = QC8(0.0), fv10 = QC8(0.0), fv11 = QC8(0.0);
#define QV_K8(n, ya, yb) { QA8 a0_, a1_, b0_, b1_; \
            coopMatLoad(a0_, E4View(wa8).v, wq + (n) * 16u, ENC_C, ROW); coopMatLoad(a1_, E4View(wa8).v, wq + 16u * ENC_C + (n) * 16u, ENC_C, ROW); \
            coopMatLoad(b0_, E4View(wb8).v, wq + (n) * 16u, ENC_C, ROW); coopMatLoad(b1_, E4View(wb8).v, wq + 16u * ENC_C + (n) * 16u, ENC_C, ROW); \
            MMA(fq00, a0_, ya) MMA(fq01, a1_, ya) MMA(fq10, a0_, yb) MMA(fq11, a1_, yb) \
            MMA(fv00, b0_, ya) MMA(fv01, b1_, ya) MMA(fv10, b0_, yb) MMA(fv11, b1_, yb) }
#define QV_K8N(n) QV_K8(uint(n), y0##n, y1##n)
            W2T(QV_K8N)
            q00 = MH(fq00), q01 = MH(fq01), q10 = MH(fq10), q11 = MH(fq11);
            v00 = MH(fv00), v01 = MH(fv01), v10 = MH(fv10), v11 = MH(fv11);
        }
#define VLAY ROW
#else
#define VLAY COL
        // Q^T [dim tile][token tile] = in_a x^T, V [token tile][dim tile] = x in_b^T; K = channel tiles in order
        ACC q00 = ACC(0.0), q01 = ACC(0.0), q10 = ACC(0.0), q11 = ACC(0.0);
        ACC v00 = ACC(0.0), v01 = ACC(0.0), v10 = ACC(0.0), v11 = ACC(0.0);
#define QV_TOK(xt, qa, qb, va, vb) { MB b_; CPB(b_, xt) MMA(qa, wa0, b_) MMA(qb, wa1, b_) \
            MA a_; CPA(a_, xt) MMA(va, a_, wb0) MMA(vb, a_, wb1) }
#define QV_K(n, xa, xb) { MA wa0, wa1; MB wb0, wb1; \
            WLD(wa0, pc.in_a.v, wq + (n) * 16u, ENC_C, ROW); WLD(wa1, pc.in_a.v, wq + 16u * ENC_C + (n) * 16u, ENC_C, ROW); \
            WLD(wb0, pc.in_b.v, wq + (n) * 16u, ENC_C, COL); WLD(wb1, pc.in_b.v, wq + 16u * ENC_C + (n) * 16u, ENC_C, COL); \
            QV_TOK(xa, q00, q01, v00, v01) QV_TOK(xb, q10, q11, v10, v11) }
#define QV_KN(n) QV_K(uint(n), x0##n, x1##n)
        W2T(QV_KN)
#endif
        // the other subgroup has read the keys of the previous head before they are overwritten
        if (hd > 0u) WG_SYNC();
        // Q rows: Q^T stored column-major; V^T rows: V stored column-major (FP8QV: V^T row-major)
        coopMatStore(q00, lds, W2Q + ra * QST, QST, COL); coopMatStore(q01, lds, W2Q + ra * QST + 16u, QST, COL);
        coopMatStore(q10, lds, W2Q + rb * QST, QST, COL); coopMatStore(q11, lds, W2Q + rb * QST + 16u, QST, COL);
        coopMatStore(v00, lds, W2V + ra, VST, VLAY); coopMatStore(v01, lds, W2V + 16u * VST + ra, VST, VLAY);
        coopMatStore(v10, lds, W2V + rb, VST, VLAY); coopMatStore(v11, lds, W2V + 16u * VST + rb, VST, VLAY);
#if !W2_DEFER
        // out projection fragments of the head: [channel tile][dim tile], K = 32 hd + dims
        MA wo00, wo01, wo10, wo11, wo20, wo21, wo30, wo31;
        WLD(wo00, pc.wout.v, WT_OUT(hd * 32u), WS_OUT, ROW); WLD(wo01, pc.wout.v, WT_OUT(hd * 32u + 16u), WS_OUT, ROW);
        WLD(wo10, pc.wout.v, WT_OUT(16u * ENC_X + hd * 32u), WS_OUT, ROW); WLD(wo11, pc.wout.v, WT_OUT(16u * ENC_X + hd * 32u + 16u), WS_OUT, ROW);
        WLD(wo20, pc.wout.v, WT_OUT(32u * ENC_X + hd * 32u), WS_OUT, ROW); WLD(wo21, pc.wout.v, WT_OUT(32u * ENC_X + hd * 32u + 16u), WS_OUT, ROW);
        WLD(wo30, pc.wout.v, WT_OUT(48u * ENC_X + hd * 32u), WS_OUT, ROW); WLD(wo31, pc.wout.v, WT_OUT(48u * ENC_X + hd * 32u + 16u), WS_OUT, ROW);
#endif
        WG_SYNC();
        // own token tile: S^T [64 keys][16] = Q_keys Q_own^T + bias^T, softmax in registers (the transposed body's
        // tree), O^T = V^T P^T into o0, o1
#define ATT(r0_, qo0, qo1, o0, o1) { \
            MH bh; \
            coopMatLoad(bh, pc.bias.v, hd * 4096u + (r0_) * 64u, 64u, COL); ACC s0 = ACC(bh); \
            coopMatLoad(bh, pc.bias.v, hd * 4096u + (r0_) * 64u + 16u, 64u, COL); ACC s1 = ACC(bh); \
            coopMatLoad(bh, pc.bias.v, hd * 4096u + (r0_) * 64u + 32u, 64u, COL); ACC s2 = ACC(bh); \
            coopMatLoad(bh, pc.bias.v, hd * 4096u + (r0_) * 64u + 48u, 64u, COL); ACC s3 = ACC(bh); \
            { MB b_; MA a_; CPB(b_, qo0) \
              IFK((r0_) >> 4, 0u) { coopMatLoad(a_, lds, W2Q, QST, ROW); MMA(s0, a_, b_) } IFK((r0_) >> 4, 1u) { coopMatLoad(a_, lds, W2Q + 16u * QST, QST, ROW); MMA(s1, a_, b_) } \
              IFK((r0_) >> 4, 2u) { coopMatLoad(a_, lds, W2Q + 32u * QST, QST, ROW); MMA(s2, a_, b_) } IFK((r0_) >> 4, 3u) { coopMatLoad(a_, lds, W2Q + 48u * QST, QST, ROW); MMA(s3, a_, b_) } \
              CPB(b_, qo1) \
              IFK((r0_) >> 4, 0u) { coopMatLoad(a_, lds, W2Q + 16u, QST, ROW); MMA(s0, a_, b_) } IFK((r0_) >> 4, 1u) { coopMatLoad(a_, lds, W2Q + 16u * QST + 16u, QST, ROW); MMA(s1, a_, b_) } \
              IFK((r0_) >> 4, 2u) { coopMatLoad(a_, lds, W2Q + 32u * QST + 16u, QST, ROW); MMA(s2, a_, b_) } IFK((r0_) >> 4, 3u) { coopMatLoad(a_, lds, W2Q + 48u * QST + 16u, QST, ROW); MMA(s3, a_, b_) } } \
            MH p0 = MH(s0), p1 = MH(s1), p2 = MH(s2), p3 = MH(s3); \
            f16vec2 e2[4]; \
            [[unroll]] for (int e = 0; e < 8; e += 2) { \
                f16vec2 t0 = TK((r0_) >> 4, 0u, unpackFloat2x16(exp_bits2(f16vec2(p0[e], p0[e + 1])))); \
                f16vec2 t1 = TK((r0_) >> 4, 1u, unpackFloat2x16(exp_bits2(f16vec2(p1[e], p1[e + 1])))); \
                f16vec2 t2 = TK((r0_) >> 4, 2u, unpackFloat2x16(exp_bits2(f16vec2(p2[e], p2[e + 1])))); \
                f16vec2 t3 = TK((r0_) >> 4, 3u, unpackFloat2x16(exp_bits2(f16vec2(p3[e], p3[e + 1])))); \
                p0[e] = t0.x, p0[e + 1] = t0.y, p1[e] = t1.x, p1[e + 1] = t1.y; \
                p2[e] = t2.x, p2[e + 1] = t2.y, p3[e] = t3.x, p3[e + 1] = t3.y; \
                precise f16vec2 sm = (t0 + t1) + (t2 + t3); \
                e2[e >> 1] = sm; \
            } \
            f16vec2 pc2[4]; \
            [[unroll]] for (int i = 0; i < 4; ++i) { \
                f16vec2 o = unpackFloat2x16(subgroupShuffleXor(packFloat2x16(e2[i]), 16u)); \
                precise f16vec2 sp = e2[i] + o; \
                pc2[i] = sp; \
            } \
            precise f16vec2 b2_ = (pc2[0] + pc2[1]) + (pc2[2] + pc2[3]); \
            precise float16_t sum = b2_.x + b2_.y; \
            float16_t r = RCP_SUM(sum); \
            [[unroll]] for (int e = 0; e < 8; e += 2) { \
                precise f16vec2 u0 = f16vec2(p0[e], p0[e + 1]) * r, u1 = f16vec2(p1[e], p1[e + 1]) * r; \
                precise f16vec2 u2 = f16vec2(p2[e], p2[e + 1]) * r, u3 = f16vec2(p3[e], p3[e + 1]) * r; \
                p0[e] = u0.x, p0[e + 1] = u0.y, p1[e] = u1.x, p1[e + 1] = u1.y; \
                p2[e] = u2.x, p2[e + 1] = u2.y, p3[e] = u3.x, p3[e + 1] = u3.y; \
            } \
            { MB b_; MA a_; \
              IFK((r0_) >> 4, 0u) { CPB(b_, p0) coopMatLoad(a_, lds, W2V, VST, ROW); MMA(o0, a_, b_) coopMatLoad(a_, lds, W2V + 16u * VST, VST, ROW); MMA(o1, a_, b_) } \
              IFK((r0_) >> 4, 1u) { CPB(b_, p1) coopMatLoad(a_, lds, W2V + 16u, VST, ROW); MMA(o0, a_, b_) coopMatLoad(a_, lds, W2V + 16u * VST + 16u, VST, ROW); MMA(o1, a_, b_) } \
              IFK((r0_) >> 4, 2u) { CPB(b_, p2) coopMatLoad(a_, lds, W2V + 32u, VST, ROW); MMA(o0, a_, b_) coopMatLoad(a_, lds, W2V + 16u * VST + 32u, VST, ROW); MMA(o1, a_, b_) } \
              IFK((r0_) >> 4, 3u) { CPB(b_, p3) coopMatLoad(a_, lds, W2V + 48u, VST, ROW); MMA(o0, a_, b_) coopMatLoad(a_, lds, W2V + 16u * VST + 48u, VST, ROW); MMA(o1, a_, b_) } } }
#if W2_DEFER
        // both token tiles first, then the out projection with each fragment serving both
        ACC oa0 = ACC(0.0), oa1 = ACC(0.0), ob0 = ACC(0.0), ob1 = ACC(0.0);
        ATT(ra, q00, q01, oa0, oa1)
        ATT(rb, q10, q11, ob0, ob1)
        {
            MB ba0, ba1, bb0, bb1;
            MH oh = MH(oa0); CPB(ba0, oh) oh = MH(oa1); CPB(ba1, oh) oh = MH(ob0); CPB(bb0, oh) oh = MH(ob1); CPB(bb1, oh)
#define OUTP(n) { MA wo_; WLD(wo_, pc.wout.v, WT_OUT(uint(n) * 16u * ENC_X + hd * 32u), WS_OUT, ROW); MMA(c0##n, wo_, ba0) MMA(c1##n, wo_, bb0) \
            WLD(wo_, pc.wout.v, WT_OUT(uint(n) * 16u * ENC_X + hd * 32u + 16u), WS_OUT, ROW); MMA(c0##n, wo_, ba1) MMA(c1##n, wo_, bb1) }
#if NT == 6
            // the wout fragments of channel tile n + 1 issued before the products of tile n
#define OLD(f_, n, hl) WLD(f_, pc.wout.v, uint(n) * 16u * ENC_X + hd * 32u + (hl) * 16u, ENC_X, ROW);
#define OMM(n, f0_, f1_) MMA(c0##n, f0_, ba0) MMA(c1##n, f0_, bb0) MMA(c0##n, f1_, ba1) MMA(c1##n, f1_, bb1)
            { MA p0, p1, q0, q1;
              OLD(p0, 0, 0) OLD(p1, 0, 1) OLD(q0, 1, 0) OLD(q1, 1, 1)
              OMM(0, p0, p1) OLD(p0, 2, 0) OLD(p1, 2, 1)
              OMM(1, q0, q1) OLD(q0, 3, 0) OLD(q1, 3, 1)
              OMM(2, p0, p1) OLD(p0, 4, 0) OLD(p1, 4, 1)
              OMM(3, q0, q1) OLD(q0, 5, 0) OLD(q1, 5, 1)
              OMM(4, p0, p1) OMM(5, q0, q1) }
#else
            W2T(OUTP)
#endif
        }
#else
#define OUT4(o0, o1, ca0, ca1, ca2, ca3) { MB b_; \
            MH oh = MH(o0); CPB(b_, oh) MMA(ca0, wo00, b_) MMA(ca1, wo10, b_) MMA(ca2, wo20, b_) MMA(ca3, wo30, b_) \
            oh = MH(o1); CPB(b_, oh) MMA(ca0, wo01, b_) MMA(ca1, wo11, b_) MMA(ca2, wo21, b_) MMA(ca3, wo31, b_) }
        { ACC o0 = ACC(0.0), o1 = ACC(0.0); ATT(ra, q00, q01, o0, o1) OUT4(o0, o1, c00, c01, c02, c03) }
        { ACC o0 = ACC(0.0), o1 = ACC(0.0); ATT(rb, q10, q11, o0, o1) OUT4(o0, o1, c10, c11, c12, c13) }
#endif
    }
    // x1 = fma(O out^T + b, ls1, x)
#define RESID1(cn, xn, l) { MH a16 = MH(cn); for (int i = 0; i < a16.length(); i += 2) { \
        precise f16vec2 v = fma(f16vec2(a16[i], a16[i + 1]), f16vec2(l), f16vec2(xn[i], xn[i + 1])); xn[i] = v.x, xn[i + 1] = v.y; } }
#define RES_A1(n) RESID1(c0##n, x0##n, ls1)
#define RES_B1(n) RESID1(c1##n, x1##n, ls1)
#define RES_A2(n) RESID1(c0##n, x0##n, ls2)
#define RES_B2(n) RESID1(c1##n, x1##n, ls2)
    W2T(RES_A1) W2T(RES_B1)
#define FC2B0(n) c0##n = bias_acc_t(pc.fc2_b, uint(n) * 16u);
#define FC2B1(n) c1##n = c0##n;
    W2T(FC2B0) W2T(FC2B1)
    // MLP in 64-neuron slices: h^T = gelu(fc1 x1^T + b), m^T += fc2 h^T (K order of the fc2 chain unchanged); each
    // fragment serves both token tiles
#define GELU1(h) { MH g_ = MH(h); for (int i = 0; i < g_.length(); i += 2) { f16vec2 q = gelu2(f16vec2(g_[i], g_[i + 1])); g_[i] = q.x, g_[i + 1] = q.y; } h = ACC(g_); }
#if NT > 4
#define FC2_45(m, ba, bb) FC2_1(4, m, ba, bb) FC2_1(5, m, ba, bb)
#else
#define FC2_45(m, ba, bb)
#endif
#define FC2_1(n, m, ba, bb) { MA g_; WLD(g_, pc.fc2.v, WT_FC2(uint(n) * 16u * HID + j + (m) * 16u), WS_FC2, ROW); MMA(c0##n, g_, ba) MMA(c1##n, g_, bb) }
#define FC2_N(m, ha, hb) { MB ba_, bb_; CPB(ba_, ha) CPB(bb_, hb) \
            FC2_1(0, m, ba_, bb_) FC2_1(1, m, ba_, bb_) FC2_1(2, m, ba_, bb_) FC2_1(3, m, ba_, bb_) FC2_45(m, ba_, bb_) }
#if W2_SL == 64
    for (uint j = 0u; j < LT_HID; j += 64u) {
        ACC hb0 = bias_acc_t(pc.fc1_b, j), hb1 = bias_acc_t(pc.fc1_b, j + 16u), hb2 = bias_acc_t(pc.fc1_b, j + 32u), hb3 = bias_acc_t(pc.fc1_b, j + 48u);
        ACC h00 = hb0, h01 = hb1, h02 = hb2, h03 = hb3, h10 = hb0, h11 = hb1, h12 = hb2, h13 = hb3;
#define FC1_K(n, xa, xb) { MA f0, f1, f2, f3; MB b_; \
            WLD(f0, pc.fc1.v, WT_FC1(j * ENC_C + (n) * 16u), WS_FC1, ROW); WLD(f1, pc.fc1.v, WT_FC1((j + 16u) * ENC_C + (n) * 16u), WS_FC1, ROW); \
            WLD(f2, pc.fc1.v, WT_FC1((j + 32u) * ENC_C + (n) * 16u), WS_FC1, ROW); WLD(f3, pc.fc1.v, WT_FC1((j + 48u) * ENC_C + (n) * 16u), WS_FC1, ROW); \
            CPB(b_, xa) MMA(h00, f0, b_) MMA(h01, f1, b_) MMA(h02, f2, b_) MMA(h03, f3, b_) \
            CPB(b_, xb) MMA(h10, f0, b_) MMA(h11, f1, b_) MMA(h12, f2, b_) MMA(h13, f3, b_) }
#define FC1_KN(n) FC1_K(uint(n), x0##n, x1##n)
        W2T(FC1_KN)
        GELU1(h00) GELU1(h01) GELU1(h02) GELU1(h03) GELU1(h10) GELU1(h11) GELU1(h12) GELU1(h13)
        FC2_N(0u, h00, h10) FC2_N(1u, h01, h11) FC2_N(2u, h02, h12) FC2_N(3u, h03, h13)
    }
#else
    // 32-neuron slices: hidden tiles h0t (token tile a), h1t (b)
    for (uint j = 0u; j < LT_HID; j += 32u) {
        ACC hb0 = bias_acc_t(pc.fc1_b, j), hb1 = bias_acc_t(pc.fc1_b, j + 16u);
        ACC h00 = hb0, h01 = hb1, h10 = hb0, h11 = hb1;
#define FC1_K2(n) { MA f0, f1; MB b_; \
            WLD(f0, pc.fc1.v, WT_FC1(j * ENC_C + uint(n) * 16u), WS_FC1, ROW); WLD(f1, pc.fc1.v, WT_FC1((j + 16u) * ENC_C + uint(n) * 16u), WS_FC1, ROW); \
            CPB(b_, x0##n) MMA(h00, f0, b_) MMA(h01, f1, b_) \
            CPB(b_, x1##n) MMA(h10, f0, b_) MMA(h11, f1, b_) }
        W2T(FC1_K2)
        GELU1(h00) GELU1(h01) GELU1(h10) GELU1(h11)
        FC2_N(0u, h00, h10) FC2_N(1u, h01, h11)
    }
#endif
    W2T(RES_A2) W2T(RES_B2)
    // TensorOutput of the own tokens straight from the x^T tiles, one 16-byte store per lane and plane
#define XOUT2(xa, xb, tp, own, pl) if (own) { uvec4 w_; [[unroll]] for (int k_ = 0; k_ < 4; ++k_) \
        w_[k_] = e4m3x4(f16vec2(xa[2 * k_], xa[2 * k_ + 1]), f16vec2(xb[2 * k_], xb[2 * k_ + 1])); \
        U32x4View(uint64_t(pc.y)).v[plane_index(tp, int(pl) * 32 + int(hh) * 16) >> 4] = w_; }
#define XOUT_A(pl, a, b) XOUT2(x0##a, x0##b, tpa, towna, uint(pl))
#define XOUT_B(pl, a, b) XOUT2(x1##a, x1##b, tpb, townb, uint(pl))
    W2P(XOUT_A) W2P(XOUT_B)
#if ENC_STAGE
    // DFN head: d^T = dfn x2^T + b per token tile (3 rows used, in lane half 0); lane half 0 takes tile a, half 1
    // tile b (its 3 values moved across the halves), gains and DFNOutput per lane, the 2x2 pooling by shuffles
    {
        ACC da = bias_acc_t(pc.dfn_b, 0u), db = da;
        {
            MA dw; MB b_;
#define DFN_K(n) coopMatLoad(dw, pc.dfn_w.v, uint(n) * 16u, ENC_C, ROW); CPB(b_, x0##n) MMA(da, dw, b_) CPB(b_, x1##n) MMA(db, dw, b_)
            W2T(DFN_K)
        }
        MH ha_ = MH(da), hb_ = MH(db);
        f16vec2 r0_ = unpackFloat2x16(subgroupShuffleXor(packFloat2x16(f16vec2(hb_[0], hb_[1])), 16u));
        float16_t r1_ = unpackFloat2x16(subgroupShuffleXor(packFloat2x16(f16vec2(hb_[2], 0.0hf)), 16u)).x;
        f16vec3 dv = hh != 0u ? f16vec3(r0_, r1_) : f16vec3(ha_[0], ha_[1], ha_[2]);
        float16_t sd = uint16BitsToFloat16(uint16_t(pc.s_dfn));
        bool own;
        ivec2 p = token_pixel(int(ra + ln), win, own);
        f16vec4 o = f16vec4(0.0hf);
        if (own) {
            uint pi = uint(p.y * pc.size.x + p.x);
            f16vec4 im = F16x4Buf(uint64_t(pc.dfn_in)).v[pi];
            for (int c = 0; c < 3; ++c) {
                precise float e = exp2(float(dv[c]) * uintBitsToFloat(0xbfb8aa3bu));
                float16_t g = float16_t(SIG_RCP(e + 1.0));
                precise float16_t gs = g * sd;
                precise float16_t v = im[c] * gs;
                o[c] = min(v, 65504.0hf);
            }
            precise float16_t a = im.w * 0.0hf;
            o.w = a;
            F16x4Buf(uint64_t(pc.dfn_out)).v[pi] = o;
        }
        // DFNPooledOutput: ((p00 + p01) + (p10 + p11)) * 0.25 in fp16
        uint src = (ln & 16u) + 2u * (ln & 3u), oxy = packFloat2x16(o.xy), ozw = packFloat2x16(o.zw);
        f16vec4 o00 = f16vec4(unpackFloat2x16(subgroupShuffle(oxy, src)), unpackFloat2x16(subgroupShuffle(ozw, src)));
        f16vec4 o01 = f16vec4(unpackFloat2x16(subgroupShuffle(oxy, src + 1u)), unpackFloat2x16(subgroupShuffle(ozw, src + 1u)));
        f16vec4 o10 = f16vec4(unpackFloat2x16(subgroupShuffle(oxy, src + 8u)), unpackFloat2x16(subgroupShuffle(ozw, src + 8u)));
        f16vec4 o11 = f16vec4(unpackFloat2x16(subgroupShuffle(oxy, src + 9u)), unpackFloat2x16(subgroupShuffle(ozw, src + 9u)));
        if ((ln & 15u) < 4u) {
            ivec2 pp = token_pixel(int(ra + src), win, own);
            if (own && pp.x + 1 < pc.size.x && pp.y + 1 < pc.size.y) {
                ivec2 q = pp >> 1;
                f16vec4 po;
                for (int c = 0; c < 4; ++c) {
                    precise float16_t s0 = o00[c] + o01[c];
                    precise float16_t s1 = o10[c] + o11[c];
                    precise float16_t v = (s0 + s1) * 0.25hf;
                    po[c] = v;
                }
                F16x4Buf(uint64_t(pc.dfn_pool)).v[uint(q.y * (pc.size.x >> 1) + q.x)] = po;
            }
        }
    }
    // merge: A row r = pooled pixel (r / 4, r % 4) of the window, K = (2 ky + kx) C + c, written straight from the
    // x^T tiles into the LDS [16][4 C] (one 16-byte store per lane and channel tile); pooled (16 x Co) = A mrg^T + b,
    // output tiles split between the subgroups, then through the LDS to the e4m3 planes
    WG_SYNC();
#define MST(xn, t0, n) { MH h_ = xn; uint t_ = (t0) + (ln & 15u); \
        uint a_ = ((t_ >> 4) * 4u + ((t_ & 7u) >> 1)) * 4u * ENC_C + ((((t_ >> 3) & 1u) << 1) + (t_ & 1u)) * ENC_C + (n) * 16u + 8u * hh; \
        lds4[a_ >> 3] = uvec4(packFloat2x16(f16vec2(h_[0], h_[1])), packFloat2x16(f16vec2(h_[2], h_[3])), \
            packFloat2x16(f16vec2(h_[4], h_[5])), packFloat2x16(f16vec2(h_[6], h_[7]))); }
#define MST_A(n) MST(x0##n, ra, uint(n))
#define MST_B(n) MST(x1##n, rb, uint(n))
    W2T(MST_A) W2T(MST_B)
    WG_SYNC();
    {
        // output tiles n0 .. n0 + Co / 32 - 1
        uint n0 = (ENC_CO / 32u) * w;
        ACC m0 = bias_acc(pc.mrg_b, n0 * 16u), m1 = bias_acc(pc.mrg_b, n0 * 16u + 16u), m2 = bias_acc(pc.mrg_b, n0 * 16u + 32u);
#if ENC_CO > 96
        ACC m3 = bias_acc(pc.mrg_b, n0 * 16u + 48u);
#endif
        for (uint k = 0u; k < 4u * ENC_C; k += 16u) {
            MA a_;
            MB b_;
            coopMatLoad(a_, lds, k, 4u * ENC_C, ROW);
            WLD(b_, pc.mrg_w.v, n0 * 16u * 4u * ENC_C + k, 4u * ENC_C, COL); MMA(m0, a_, b_)
            WLD(b_, pc.mrg_w.v, (n0 * 16u + 16u) * 4u * ENC_C + k, 4u * ENC_C, COL); MMA(m1, a_, b_)
            WLD(b_, pc.mrg_w.v, (n0 * 16u + 32u) * 4u * ENC_C + k, 4u * ENC_C, COL); MMA(m2, a_, b_)
#if ENC_CO > 96
            WLD(b_, pc.mrg_w.v, (n0 * 16u + 48u) * 4u * ENC_C + k, 4u * ENC_C, COL); MMA(m3, a_, b_)
#endif
        }
        WG_SYNC();
        coopMatStore(MH(m0), lds, n0 * 16u, ENC_CO, ROW);
        coopMatStore(MH(m1), lds, n0 * 16u + 16u, ENC_CO, ROW);
        coopMatStore(MH(m2), lds, n0 * 16u + 32u, ENC_CO, ROW);
#if ENC_CO > 96
        coopMatStore(MH(m3), lds, n0 * 16u + 48u, ENC_CO, ROW);
#endif
    }
    WG_SYNC();
    // PooledOutput: e4m3 of the pooled pixels the window owns
    {
        ivec2 ps = pc.size >> 1;
        for (uint i = gl_LocalInvocationIndex; i < ENC_CO; i += 64u) {
            uint r = i / (ENC_CO / 16u), a = (i % (ENC_CO / 16u)) * 16u;
            bool own;
            ivec2 p = token_pixel(int((2u * (r >> 2)) * 8u + 2u * (r & 3u)), win, own);
            if (!own) continue;
            ivec2 q = p >> 1;
            U32x4View(uint64_t(pc.pooled)).v[(uint(a >> 5) * uint(ps.x * ps.y * 32) + uint((q.y * ps.x + q.x) * 32) + (a & 31u)) >> 4] =
                get16(r * ENC_CO, a);
        }
    }
#endif
#if DEC_DFN
    // decoder DFN: d^T = dfn x2^T + b per token tile (4 rows used, in lane half 0); lane half 0 takes tile a, half 1
    // tile b (its 4 values moved across the halves)
    {
        ACC da = bias_acc_t(pc.dfn_b, 0u), db = da;
        {
            MA dw; MB b_;
#define DFN_K(n) coopMatLoad(dw, pc.dfn_w.v, uint(n) * 16u, ENC_C, ROW); CPB(b_, x0##n) MMA(da, dw, b_) CPB(b_, x1##n) MMA(db, dw, b_)
            W2T(DFN_K)
        }
        MH ha = MH(da), hb = MH(db);
        f16vec2 r0 = unpackFloat2x16(subgroupShuffleXor(packFloat2x16(f16vec2(hb[0], hb[1])), 16u));
        f16vec2 r1 = unpackFloat2x16(subgroupShuffleXor(packFloat2x16(f16vec2(hb[2], hb[3])), 16u));
        f16vec4 dv = hh != 0u ? f16vec4(r0, r1) : f16vec4(ha[0], ha[1], ha[2], ha[3]);
        ivec2 p = hh != 0u ? tpb : tpa;
        bool own = hh != 0u ? townb : towna;
        // up = bilinear(below) * fp16(sig(d_c) s); out = min(fma(skip, 1 - sig(d3), up * sig(d3)), 65504)
        if (own && p.y < pc.out_h) {
            uint pi = uint(p.y * pc.size.x + p.x) * 4u;
            float16_t sd = uint16BitsToFloat16(uint16_t(pc.s_dfn));
            float16_t g3 = sigmoid16(dv[3]);
            precise float16_t omg = 1.0hf - g3;
            f16vec3 u = upsample_below(p);
            f16vec4 sk = F16x4Buf(uint64_t(pc.skip)).v[pi >> 2], o;
            for (int c = 0; c < 3; ++c) {
                float16_t g = sigmoid16(dv[c]);
                precise float16_t gs = g * sd;
                precise float16_t ug = u[c] * gs;
                precise float16_t tt = ug * g3;
                precise float16_t oc = fma(sk[c], omg, tt);
                o[c] = min(oc, 65504.0hf);
            }
            precise float16_t ow = sk.w * omg;
            o.w = min(ow, 65504.0hf);
            F16x4Buf(uint64_t(pc.dfn_out)).v[pi >> 2] = o;
        }
    }
#endif
}
