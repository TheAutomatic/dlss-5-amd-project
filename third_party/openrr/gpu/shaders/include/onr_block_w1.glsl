// SPDX-License-Identifier: MIT
// One subgroup per window (W1: C = 32, fast variant), included by onr_block_fast.glsl in place of its main. The
// subgroup holds all 64 tokens: x^T as 4 token tiles x 2 channel tiles in registers. Per head, Q^T [32 dims][64 tokens]
// stays in registers: it is the B operand of the own rows and, copied element by element, the A operand of the keys
// (probe_wmma: an accumulator is the A operand of its transpose). V is computed untransposed (x in_b^T, x as the A copy
// of x^T), so its accumulator copied is the A operand of V^T for P V. Every weight fragment serves the 4 token tiles.
// No LDS, no barrier. Same products, same K order, same roundings as the transposed body.

// accumulator -> A operand of its transpose / B operand of itself
#define CPA(a, h) { for (int i_ = 0; i_ < h.length(); ++i_) a[i_] = h[i_]; }
#define CPB(b, h) { for (int i_ = 0; i_ < h.length(); ++i_) b[i_] = h[i_]; }
#define MMA(acc, a, b) acc = coopMatMulAdd(a, b, acc);

#if DEC_HEAD
// output head of one pixel from the 18 block channels it reads (GPU order), as the transposed body does
void head_px(ivec2 p, bool own, float16_t dv[18]) {
    if (!own || p.y >= pc.out_h) return;
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
    // with the post tile written here ajab is dead (only post_tile reads it), and the DFN image too unless it is
    // exported (the engine passes dfn_out 0 then)
    bool tl = uint64_t(pc.tile) != 0ul;
    if (uint64_t(pc.dfn_out) != 0ul) F16x4Buf(uint64_t(pc.dfn_out)).v[pi >> 2] = o;
    if (p.x >= pc.out_w) return;
    pi = uint(p.y * pc.out_w + p.x) * 4u;
    u8vec4 aj = u8vec4(0);
    for (int c = 0; c < 3; ++c) aj[c] = uint8_t(unorm8_code(sigmoid16(dv[4 + c])));
    if (!tl) U8x4Buf(uint64_t(pc.ajab)).v[pi >> 2] = aj;
    float16_t m[4], e[4];
    for (int c = 0; c < 4; ++c) m[c] = dv[10 + c];
    float16_t mx = max(max(m[0], m[2]), max(m[1], m[3]));
    for (int c = 0; c < 4; ++c) {
        precise float16_t d = m[c] - mx;
        precise float16_t z = d * 1.4423828125hf;
        e[c] = HEAD_RZ(exp2(float(z)));
    }
    precise float16_t s02 = e[0] + e[2], s13 = e[1] + e[3], se = s02 + s13;
    float16_t r = float16_t(SIG_RCP(float(se)));
    u8vec4 mv, hh;
    for (int c = 0; c < 4; ++c) { precise float16_t v = e[c] * r; mv[c] = uint8_t(unorm8_code(v)); }
    U8x4Buf(uint64_t(pc.mvf)).v[pi >> 2] = mv;
    for (int c = 0; c < 4; ++c) hh[c] = uint8_t(snorm8_code(HEAD_RZ(tanh_f32(float(dv[14 + c])))));
    U8x4Buf(uint64_t(pc.hidden)).v[pi >> 2] = hh;
    precise float a = float(sigmoid16(clamp(dv[7], -20.0hf, 20.0hf))) * pc.k_scale;
    precise float b = float(sigmoid16(clamp(dv[8], -20.0hf, 20.0hf))) * pc.k_scale;
    precise float tt = float(dv[9]) * pc.k_tanh;
    float th = tanh_f32(tt);
    precise float aa = a * a, bb = b * b, ab = a * b, abt = ab * th;
    // kept with the tile too: without this store the compiler schedules the head's math differently and a few
    // outputs move by an ulp
#ifdef LT_HM
    if (!tl)
#endif
    F16x4Buf(uint64_t(pc.aniso)).v[pi >> 2] = f16vec4(float16_t(aa), float16_t(bb), float16_t(abt), 0.0hf);
    if (tl) {
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

void main() {
#if ENC_EMBED
    // enc0 walks the grid backwards: it starts on the attribute rows enc0_attr wrote last (still in the MALL), and
    // enc1, walking forwards, starts on the rows enc0 wrote last
    int wid_ = pc.nwx * ((pc.size.y + pc.shift.y + 7) / 8) - 1 - int(gl_WorkGroupID.x);
#else
    int wid_ = int(gl_WorkGroupID.x);
#endif
    ivec2 win = ivec2(wid_ % pc.nwx, wid_ / pc.nwx);
#if DEC_HEAD
    if (8 * win.y - pc.shift.y >= pc.out_h) return;
#elif DEC_UP
    if (pc.live_h > 0 && 8 * win.y - pc.shift.y >= pc.live_h) return;
#endif
    uint ln = gl_SubgroupInvocationID, hh = ln >> 4;
    float16_t ls1 = uint16BitsToFloat16(uint16_t(pc.ls & 0xffffu));
    float16_t ls2 = uint16BitsToFloat16(uint16_t(pc.ls >> 16));
    // lane l: token 16 t + l % 16 of token tile t, GPU channels 8 h .. 8 h + 7 of each channel tile
    // token tiles with no written token (rows outside the grid, or from out_h on in dec0_1) are only keys: no S, P V,
    // out or MLP (dec0_0 measured slower with the test)
#if DEC_UP
    const bool lv0 = true, lv1 = true, lv2 = true, lv3 = true;
#else
    bool lv0 = tile_live(0, win), lv1 = tile_live(1, win), lv2 = tile_live(2, win), lv3 = tile_live(3, win);
#endif
    bool town0, town1, town2, town3;
    ivec2 tp0 = token_pixel(int(ln & 15u), win, town0), tp1 = token_pixel(int(16u + (ln & 15u)), win, town1);
    ivec2 tp2 = token_pixel(int(32u + (ln & 15u)), win, town2), tp3 = token_pixel(int(48u + (ln & 15u)), win, town3);
    // 16 record bytes of a 32-channel plane -> the lane's elements of its two channel tiles (as XIN)
#define REC2(xa, xb, w) [[unroll]] for (int k_ = 0; k_ < 4; ++k_) { \
            f16vec2 a_ = unpackFloat2x16(e4m3_lo(w[k_])), b_ = unpackFloat2x16(e4m3_hi(w[k_])); \
            xa[2 * k_] = a_.x, xa[2 * k_ + 1] = a_.y, xb[2 * k_] = b_.x, xb[2 * k_ + 1] = b_.y; }
#if DEC_UP
    // U^T [channels][16 low-res pixels] per PixelShuffle position = up_pos A^T + b, with A^T (lane: pixel l % 16) as
    // the B operand straight from the planes below; U goes through a wave-private LDS [4][16][C] for the per-token
    // gather (no barrier: one subgroup)
    {
        ivec2 q = (8 * win - pc.shift) / 2 + ivec2(ln & 3u, (ln >> 2) & 3u);
        q = ivec2(reflect_i(q.x, pc.lo_size.x), reflect_i(q.y, pc.lo_size.y));
        U32x4View lo = U32x4View(uint64_t(pc.xlo));
        uvec4 w0 = lo.v[plane_index_sz(q, int(hh) * 16, pc.lo_size) >> 4], w1 = lo.v[plane_index_sz(q, 32 + int(hh) * 16, pc.lo_size) >> 4];
        MB a0, a1, a2, a3;
        REC2(a0, a1, w0) REC2(a2, a3, w1)
#define UP_T(p, n) { ACC u = bias_acc_t(pc.up_b, (p) * ENC_C + (n) * 16u); MA w_; uint o_ = ((p) * ENC_C + (n) * 16u) * ENC_CP; \
            coopMatLoad(w_, pc.up_w.v, o_, ENC_CP, ROW); MMA(u, w_, a0) coopMatLoad(w_, pc.up_w.v, o_ + 16u, ENC_CP, ROW); MMA(u, w_, a1) \
            coopMatLoad(w_, pc.up_w.v, o_ + 32u, ENC_CP, ROW); MMA(u, w_, a2) coopMatLoad(w_, pc.up_w.v, o_ + 48u, ENC_CP, ROW); MMA(u, w_, a3) \
            MH h_ = MH(u); lds4[(((p) * 16u + (ln & 15u)) * ENC_C + (n) * 16u + 8u * hh) >> 3] = uvec4(packFloat2x16(f16vec2(h_[0], h_[1])), \
                packFloat2x16(f16vec2(h_[2], h_[3])), packFloat2x16(f16vec2(h_[4], h_[5])), packFloat2x16(f16vec2(h_[6], h_[7]))); }
        UP_T(0u, 0u) UP_T(0u, 1u) UP_T(1u, 0u) UP_T(1u, 1u) UP_T(2u, 0u) UP_T(2u, 1u) UP_T(3u, 0u) UP_T(3u, 1u)
    }
    SG_SYNC();
    // x^T = fma(skip, s, U) per lane: the skip's record bytes as in XIN, U of the token (position, low-res pixel)
    float16_t sc = uint16BitsToFloat16(uint16_t(pc.s_skip));
#define XIN1(xa, xb, tp, t0) MH xa, xb; { uvec4 w_ = U32x4View(uint64_t(pc.x)).v[plane_index(tp, int(hh) * 16) >> 4]; \
        uint t_ = (t0) + (ln & 15u), ua_ = (((((t_ >> 3) & 1u) << 1) + (t_ & 1u)) * 16u * ENC_C + (((t_ >> 4) << 2) + ((t_ & 7u) >> 1)) * ENC_C + 8u * hh) >> 1; \
        [[unroll]] for (int k_ = 0; k_ < 4; ++k_) { \
            f16vec2 a_ = unpackFloat2x16(e4m3_lo(w_[k_])), b_ = unpackFloat2x16(e4m3_hi(w_[k_])); \
            f16vec2 ua = unpackFloat2x16(lds1[ua_ + uint(k_)]), ubv = unpackFloat2x16(lds1[ua_ + 8u + uint(k_)]); \
            precise f16vec2 za = fma(a_, f16vec2(sc), ua), zb = fma(b_, f16vec2(sc), ubv); \
            xa[2 * k_] = za.x, xa[2 * k_ + 1] = za.y, xb[2 * k_] = zb.x, xb[2 * k_ + 1] = zb.y; } }
#elif ENC_EMBED
    // rr2_enc0 embed, transposed: the dithered attributes of a token tile are the B operands straight from memory (lane:
    // token l % 16, attributes 16 kt + 8 h .. + 7); h^T = gelu(fc1 A^T + b), x^T = fc2 h^T + b. K = 48: attributes
    // 43..47 are +0
    MA ew00, ew01, ew02, ew10, ew11, ew12, ew20, ew21, ew22, ew30, ew31, ew32;   // fc1 [neuron tile][K tile]
    coopMatLoad(ew00, pc.mrg_w.v, 0u, 64u, ROW); coopMatLoad(ew01, pc.mrg_w.v, 16u, 64u, ROW); coopMatLoad(ew02, pc.mrg_w.v, 32u, 64u, ROW);
    coopMatLoad(ew10, pc.mrg_w.v, 1024u, 64u, ROW); coopMatLoad(ew11, pc.mrg_w.v, 1040u, 64u, ROW); coopMatLoad(ew12, pc.mrg_w.v, 1056u, 64u, ROW);
    coopMatLoad(ew20, pc.mrg_w.v, 2048u, 64u, ROW); coopMatLoad(ew21, pc.mrg_w.v, 2064u, 64u, ROW); coopMatLoad(ew22, pc.mrg_w.v, 2080u, 64u, ROW);
    coopMatLoad(ew30, pc.mrg_w.v, 3072u, 64u, ROW); coopMatLoad(ew31, pc.mrg_w.v, 3088u, 64u, ROW); coopMatLoad(ew32, pc.mrg_w.v, 3104u, 64u, ROW);
    MA fw00, fw01, fw02, fw03, fw10, fw11, fw12, fw13;   // fc2 [channel tile][neuron tile]
    coopMatLoad(fw00, pc.mrg_w.v, 4096u, 64u, ROW); coopMatLoad(fw01, pc.mrg_w.v, 4112u, 64u, ROW);
    coopMatLoad(fw02, pc.mrg_w.v, 4128u, 64u, ROW); coopMatLoad(fw03, pc.mrg_w.v, 4144u, 64u, ROW);
    coopMatLoad(fw10, pc.mrg_w.v, 5120u, 64u, ROW); coopMatLoad(fw11, pc.mrg_w.v, 5136u, 64u, ROW);
    coopMatLoad(fw12, pc.mrg_w.v, 5152u, 64u, ROW); coopMatLoad(fw13, pc.mrg_w.v, 5168u, 64u, ROW);
    ACC eb0 = bias_acc_t(pc.mrg_b, 0u), eb1 = bias_acc_t(pc.mrg_b, 16u), eb2 = bias_acc_t(pc.mrg_b, 32u), eb3 = bias_acc_t(pc.mrg_b, 48u);
    ACC fb0 = bias_acc_t(pc.mrg_b, 64u), fb1 = bias_acc_t(pc.mrg_b, 80u);
    U32x4View av = U32x4View(uint64_t(pc.attr));
    // the attributes come dithered from enc0_attr_f (ATTR64): the token's 48-byte record, the lane half's 16 bytes and the shared 16;
    // per pair of block j the two high bytes and their bit 7 from the plane dword
#define UNREC(b, d0, d1, pl, j) [[unroll]] for (int k_ = 0; k_ < 4; ++k_) { \
            uint d_ = k_ < 2 ? d0 : d1; \
            uint w_ = ((k_ & 1) == 0 ? ((d_ << 8) & 0x0000ff00u) | ((d_ << 16) & 0xff000000u) : ((d_ >> 8) & 0x0000ff00u) | (d_ & 0xff000000u)) \
                | (((pl) >> uint(4 * (j) + k_) << 7) & 0x00800080u); \
            f16vec2 e_ = unpackFloat2x16(w_); b[2 * k_] = e_.x, b[2 * k_ + 1] = e_.y; }
#define GELUB(e, g) MB g; { MH h_ = MH(e); for (int i = 0; i < h_.length(); i += 2) { f16vec2 q = gelu2(f16vec2(h_[i], h_[i + 1])); h_[i] = q.x, h_[i + 1] = q.y; } CPB(g, h_) }
#ifdef ATTR_WIDE
// the 64-byte record (enc0_attr_f_w: attributes 32 and 33 in use), the lane half's 32 bytes
#define EMB_REC(tp) \
        uint ab_ = uint(tp.y * pc.size.x + tp.x) * 4u + hh * 2u; \
        uvec4 r0_ = av.v[ab_], s_ = av.v[ab_ + 1u]; \
        uint d4_ = s_.x, d5_ = s_.y, pl_ = s_.z;
#else
#define EMB_REC(tp) \
        uint ab_ = uint(tp.y * pc.size.x + tp.x) * 3u; \
        uvec4 r0_ = av.v[ab_ + hh], s_ = av.v[ab_ + 2u]; \
        uint d4_ = hh == 0u ? s_.x & 0xffff0000u : s_.w & 0x00ffffffu, d5_ = hh == 0u ? s_.y : 0u; \
        uint pl_ = hh == 0u ? s_.z : (s_.x & 0xfffu) | ((s_.w >> 8) & 0xff0000u) | ((s_.x & 0x1000u) << 12);
#endif
#define EMB_TOK(xa, xb, tp) MH xa, xb; { \
        EMB_REC(tp) \
        MB b0_, b1_, b2_; \
        UNREC(b0_, r0_.x, r0_.y, pl_, 0) UNREC(b1_, r0_.z, r0_.w, pl_, 1) UNREC(b2_, d4_, d5_, pl_, 2) \
        ACC e0 = eb0, e1 = eb1, e2 = eb2, e3 = eb3; \
        MMA(e0, ew00, b0_) MMA(e1, ew10, b0_) MMA(e2, ew20, b0_) MMA(e3, ew30, b0_) \
        MMA(e0, ew01, b1_) MMA(e1, ew11, b1_) MMA(e2, ew21, b1_) MMA(e3, ew31, b1_) \
        MMA(e0, ew02, b2_) MMA(e1, ew12, b2_) MMA(e2, ew22, b2_) MMA(e3, ew32, b2_) \
        GELUB(e0, g0_) GELUB(e1, g1_) GELUB(e2, g2_) GELUB(e3, g3_) \
        ACC f0 = fb0, f1 = fb1; \
        MMA(f0, fw00, g0_) MMA(f1, fw10, g0_) MMA(f0, fw01, g1_) MMA(f1, fw11, g1_) \
        MMA(f0, fw02, g2_) MMA(f1, fw12, g2_) MMA(f0, fw03, g3_) MMA(f1, fw13, g3_) \
        xa = MH(f0), xb = MH(f1); }
    EMB_TOK(x00, x01, tp0) EMB_TOK(x10, x11, tp1) EMB_TOK(x20, x21, tp2) EMB_TOK(x30, x31, tp3)
#define XIN1(xa, xb, tp, t0)
#else
#define XIN1(xa, xb, tp, t0) MH xa, xb; { uvec4 w_ = U32x4View(uint64_t(pc.x)).v[plane_index(tp, int(hh) * 16) >> 4]; REC2(xa, xb, w_) }
#endif
    XIN1(x00, x01, tp0, 0u) XIN1(x10, x11, tp1, 16u) XIN1(x20, x21, tp2, 32u) XIN1(x30, x31, tp3, 48u)
    // x^T tile names: x<token tile><channel tile>; out accumulators c<token tile><channel tile>
    ACC c00 = bias_acc_t(pc.out_b, 0u), c01 = bias_acc_t(pc.out_b, 16u);
    ACC c10 = c00, c11 = c01, c20 = c00, c21 = c01, c30 = c00, c31 = c01;
    for (uint hd = 0u; hd < LT_HEADS; ++hd) {
        uint wq = hd * 32u * ENC_C;
        // Q^T [dim tile][token tile] = in_a x^T, V [token tile][dim tile] = x in_b^T; K = channel tiles in order
        ACC q00 = ACC(0.0), q01 = ACC(0.0), q10 = ACC(0.0), q11 = ACC(0.0), q20 = ACC(0.0), q21 = ACC(0.0), q30 = ACC(0.0), q31 = ACC(0.0);
        ACC v00 = ACC(0.0), v01 = ACC(0.0), v10 = ACC(0.0), v11 = ACC(0.0), v20 = ACC(0.0), v21 = ACC(0.0), v30 = ACC(0.0), v31 = ACC(0.0);
#define QV_TOK(xt, qa, qb, va, vb) { MB b_; CPB(b_, xt) MMA(qa, wa0, b_) Q2_IF { MMA(qb, wa1, b_) } \
            MA a_; CPA(a_, xt) MMA(va, a_, wb0) MMA(vb, a_, wb1) }
#define QV_K(n, xa, xb, xc, xd) { MA wa0, wa1; MB wb0, wb1; \
            coopMatLoad(wa0, pc.in_a.v, QA_LR(wq + (n) * 16u), ENC_C, ROW); Q2_IF { coopMatLoad(wa1, pc.in_a.v, wq + 16u * ENC_C + (n) * 16u, ENC_C, ROW); } \
            coopMatLoad(wb0, pc.in_b.v, wq + (n) * 16u, ENC_C, COL); coopMatLoad(wb1, pc.in_b.v, wq + 16u * ENC_C + (n) * 16u, ENC_C, COL); \
            QV_TOK(xa, q00, q01, v00, v01) QV_TOK(xb, q10, q11, v10, v11) QV_TOK(xc, q20, q21, v20, v21) QV_TOK(xd, q30, q31, v30, v31) }
        QV_K(0u, x00, x10, x20, x30)
        QV_K(1u, x01, x11, x21, x31)
        // out projection fragments of the head: [channel tile][dim tile], K = 32 hd + dims
        MA wo00, wo01, wo10, wo11;
        coopMatLoad(wo00, pc.wout.v, WT_OUT(hd * 32u), WS_OUT, ROW);
        coopMatLoad(wo01, pc.wout.v, WT_OUT(hd * 32u + 16u), WS_OUT, ROW);
        coopMatLoad(wo10, pc.wout.v, WT_OUT(16u * ENC_X + hd * 32u), WS_OUT, ROW);
        coopMatLoad(wo11, pc.wout.v, WT_OUT(16u * ENC_X + hd * 32u + 16u), WS_OUT, ROW);
        // own token tile: S^T [64 keys][16] = Q_keys Q_own^T + bias^T, softmax in registers (the transposed body's
        // tree), O^T = V^T P^T, then the out projection into its accumulators
#define ATT(r0_, qo0, qo1, ca0, ca1) { \
            MH bh; \
            coopMatLoad(bh, pc.bias.v, hd * 4096u + (r0_) * 64u, 64u, COL); ACC s0 = ACC(bh); \
            coopMatLoad(bh, pc.bias.v, hd * 4096u + (r0_) * 64u + 16u, 64u, COL); ACC s1 = ACC(bh); \
            coopMatLoad(bh, pc.bias.v, hd * 4096u + (r0_) * 64u + 32u, 64u, COL); ACC s2 = ACC(bh); \
            coopMatLoad(bh, pc.bias.v, hd * 4096u + (r0_) * 64u + 48u, 64u, COL); ACC s3 = ACC(bh); \
            { MB b_; MA a_; CPB(b_, qo0) \
              IFK((r0_) >> 4, 0u) { CPA(a_, q00) MMA(s0, a_, b_) } IFK((r0_) >> 4, 1u) { CPA(a_, q10) MMA(s1, a_, b_) } \
              IFK((r0_) >> 4, 2u) { CPA(a_, q20) MMA(s2, a_, b_) } IFK((r0_) >> 4, 3u) { CPA(a_, q30) MMA(s3, a_, b_) } \
              Q2_IF { CPB(b_, qo1) \
              IFK((r0_) >> 4, 0u) { CPA(a_, q01) MMA(s0, a_, b_) } IFK((r0_) >> 4, 1u) { CPA(a_, q11) MMA(s1, a_, b_) } \
              IFK((r0_) >> 4, 2u) { CPA(a_, q21) MMA(s2, a_, b_) } IFK((r0_) >> 4, 3u) { CPA(a_, q31) MMA(s3, a_, b_) } } } \
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
            ACC o0 = ACC(0.0), o1 = ACC(0.0); \
            { MB b_; MA a_; \
              IFK((r0_) >> 4, 0u) { CPB(b_, p0) CPA(a_, v00) MMA(o0, a_, b_) CPA(a_, v01) MMA(o1, a_, b_) } \
              IFK((r0_) >> 4, 1u) { CPB(b_, p1) CPA(a_, v10) MMA(o0, a_, b_) CPA(a_, v11) MMA(o1, a_, b_) } \
              IFK((r0_) >> 4, 2u) { CPB(b_, p2) CPA(a_, v20) MMA(o0, a_, b_) CPA(a_, v21) MMA(o1, a_, b_) } \
              IFK((r0_) >> 4, 3u) { CPB(b_, p3) CPA(a_, v30) MMA(o0, a_, b_) CPA(a_, v31) MMA(o1, a_, b_) } \
              MH oh = MH(o0); CPB(b_, oh) MMA(ca0, wo00, b_) MMA(ca1, wo10, b_) \
              oh = MH(o1); CPB(b_, oh) MMA(ca0, wo01, b_) MMA(ca1, wo11, b_) } }
        if (lv0) ATT(0u, q00, q01, c00, c01)
        if (lv1) ATT(16u, q10, q11, c10, c11)
        if (lv2) ATT(32u, q20, q21, c20, c21)
        if (lv3) ATT(48u, q30, q31, c30, c31)
    }
    // x1 = fma(O out^T + b, ls1, x)
#define RESID1(cn, xn, l) { MH a16 = MH(cn); for (int i = 0; i < a16.length(); i += 2) { \
        precise f16vec2 v = fma(f16vec2(a16[i], a16[i + 1]), f16vec2(l), f16vec2(xn[i], xn[i + 1])); xn[i] = v.x, xn[i + 1] = v.y; } }
    RESID1(c00, x00, ls1) RESID1(c01, x01, ls1) RESID1(c10, x10, ls1) RESID1(c11, x11, ls1)
    RESID1(c20, x20, ls1) RESID1(c21, x21, ls1) RESID1(c30, x30, ls1) RESID1(c31, x31, ls1)
    c00 = bias_acc_t(pc.fc2_b, 0u), c01 = bias_acc_t(pc.fc2_b, 16u);
    c10 = c00, c11 = c01, c20 = c00, c21 = c01, c30 = c00, c31 = c01;
    // MLP in 64-neuron slices: h^T = gelu(fc1 x1^T + b), m^T += fc2 h^T (K order of the fc2 chain unchanged)
    for (uint j = 0u; j < LT_HID; j += 64u) {
        MA f00, f01, f10, f11, f20, f21, f30, f31;   // fc1 [neuron tile][channel tile]
        coopMatLoad(f00, pc.fc1.v, WT_FC1(j * ENC_C), WS_FC1, ROW); coopMatLoad(f01, pc.fc1.v, WT_FC1(j * ENC_C + 16u), WS_FC1, ROW);
        coopMatLoad(f10, pc.fc1.v, WT_FC1((j + 16u) * ENC_C), WS_FC1, ROW); coopMatLoad(f11, pc.fc1.v, WT_FC1((j + 16u) * ENC_C + 16u), WS_FC1, ROW);
        coopMatLoad(f20, pc.fc1.v, WT_FC1((j + 32u) * ENC_C), WS_FC1, ROW); coopMatLoad(f21, pc.fc1.v, WT_FC1((j + 32u) * ENC_C + 16u), WS_FC1, ROW);
        coopMatLoad(f30, pc.fc1.v, WT_FC1((j + 48u) * ENC_C), WS_FC1, ROW); coopMatLoad(f31, pc.fc1.v, WT_FC1((j + 48u) * ENC_C + 16u), WS_FC1, ROW);
        MA g00, g01, g02, g03, g10, g11, g12, g13;   // fc2 [channel tile][neuron tile]
        coopMatLoad(g00, pc.fc2.v, WT_FC2(j), WS_FC2, ROW); coopMatLoad(g01, pc.fc2.v, WT_FC2(j + 16u), WS_FC2, ROW);
        coopMatLoad(g02, pc.fc2.v, WT_FC2(j + 32u), WS_FC2, ROW); coopMatLoad(g03, pc.fc2.v, WT_FC2(j + 48u), WS_FC2, ROW);
        coopMatLoad(g10, pc.fc2.v, WT_FC2(16u * HID + j), WS_FC2, ROW); coopMatLoad(g11, pc.fc2.v, WT_FC2(16u * HID + j + 16u), WS_FC2, ROW);
        coopMatLoad(g12, pc.fc2.v, WT_FC2(16u * HID + j + 32u), WS_FC2, ROW); coopMatLoad(g13, pc.fc2.v, WT_FC2(16u * HID + j + 48u), WS_FC2, ROW);
        ACC hb0 = bias_acc_t(pc.fc1_b, j), hb1 = bias_acc_t(pc.fc1_b, j + 16u), hb2 = bias_acc_t(pc.fc1_b, j + 32u), hb3 = bias_acc_t(pc.fc1_b, j + 48u);
#define GELU1(h) { MH g_ = MH(h); for (int i = 0; i < g_.length(); i += 2) { f16vec2 q = gelu2(f16vec2(g_[i], g_[i + 1])); g_[i] = q.x, g_[i + 1] = q.y; } h = ACC(g_); }
#define MLP_TOK(xa, xb, ca, cb) { ACC h0 = hb0, h1 = hb1, h2 = hb2, h3 = hb3; MB b_; \
            CPB(b_, xa) MMA(h0, f00, b_) MMA(h1, f10, b_) MMA(h2, f20, b_) MMA(h3, f30, b_) \
            CPB(b_, xb) MMA(h0, f01, b_) MMA(h1, f11, b_) MMA(h2, f21, b_) MMA(h3, f31, b_) \
            GELU1(h0) GELU1(h1) GELU1(h2) GELU1(h3) \
            CPB(b_, h0) MMA(ca, g00, b_) MMA(cb, g10, b_) \
            CPB(b_, h1) MMA(ca, g01, b_) MMA(cb, g11, b_) \
            CPB(b_, h2) MMA(ca, g02, b_) MMA(cb, g12, b_) \
            CPB(b_, h3) MMA(ca, g03, b_) MMA(cb, g13, b_) }
        if (lv0) MLP_TOK(x00, x01, c00, c01) if (lv1) MLP_TOK(x10, x11, c10, c11) if (lv2) MLP_TOK(x20, x21, c20, c21) if (lv3) MLP_TOK(x30, x31, c30, c31)
    }
    RESID1(c00, x00, ls2) RESID1(c01, x01, ls2) RESID1(c10, x10, ls2) RESID1(c11, x11, ls2)
    RESID1(c20, x20, ls2) RESID1(c21, x21, ls2) RESID1(c30, x30, ls2) RESID1(c31, x31, ls2)
#if ENC_STAGE
    // DFN head: d^T = dfn x2^T + b per token tile (3 rows used); then two token tiles at once (lane half 0: tile a,
    // half 1: tile b, its d moved across the halves), gains and DFNOutput per lane, the 2x2 pooling by shuffles
    {
        MA dw0, dw1;
        coopMatLoad(dw0, pc.dfn_w.v, 0u, ENC_C, ROW);
        coopMatLoad(dw1, pc.dfn_w.v, 16u, ENC_C, ROW);
        ACC dbs = bias_acc_t(pc.dfn_b, 0u);
#define W1_DFN(xa, xb, d) ACC d = dbs; { MB b_; CPB(b_, xa) MMA(d, dw0, b_) CPB(b_, xb) MMA(d, dw1, b_) }
        W1_DFN(x00, x01, d0) W1_DFN(x10, x11, d1) W1_DFN(x20, x21, d2) W1_DFN(x30, x31, d3)
        float16_t sd = uint16BitsToFloat16(uint16_t(pc.s_dfn));
#define DFN2(da, dbt, t0) { \
            MH ha_ = MH(da), hb_ = MH(dbt); \
            f16vec2 r0_ = unpackFloat2x16(subgroupShuffleXor(packFloat2x16(f16vec2(hb_[0], hb_[1])), 16u)); \
            float16_t r1_ = unpackFloat2x16(subgroupShuffleXor(packFloat2x16(f16vec2(hb_[2], 0.0hf)), 16u)).x; \
            f16vec3 dv = hh != 0u ? f16vec3(r0_, r1_) : f16vec3(ha_[0], ha_[1], ha_[2]); \
            bool own; \
            ivec2 p = token_pixel(int((t0) + ln), win, own); \
            f16vec4 o = f16vec4(0.0hf); \
            if (own) { \
                uint pi = uint(p.y * pc.size.x + p.x); \
                f16vec4 im = F16x4Buf(uint64_t(pc.dfn_in)).v[pi]; \
                for (int c = 0; c < 3; ++c) { \
                    precise float e = exp2(float(dv[c]) * uintBitsToFloat(0xbfb8aa3bu)); \
                    float16_t g = float16_t(SIG_RCP(e + 1.0)); \
                    precise float16_t gs = g * sd; \
                    precise float16_t v = im[c] * gs; \
                    o[c] = min(v, 65504.0hf); \
                } \
                precise float16_t a = im.w * 0.0hf; \
                o.w = a; \
                F16x4Buf(uint64_t(pc.dfn_out)).v[pi] = o; \
            } \
            uint src = (ln & 16u) + 2u * (ln & 3u), oxy = packFloat2x16(o.xy), ozw = packFloat2x16(o.zw); \
            f16vec4 o00 = f16vec4(unpackFloat2x16(subgroupShuffle(oxy, src)), unpackFloat2x16(subgroupShuffle(ozw, src))); \
            f16vec4 o01 = f16vec4(unpackFloat2x16(subgroupShuffle(oxy, src + 1u)), unpackFloat2x16(subgroupShuffle(ozw, src + 1u))); \
            f16vec4 o10 = f16vec4(unpackFloat2x16(subgroupShuffle(oxy, src + 8u)), unpackFloat2x16(subgroupShuffle(ozw, src + 8u))); \
            f16vec4 o11 = f16vec4(unpackFloat2x16(subgroupShuffle(oxy, src + 9u)), unpackFloat2x16(subgroupShuffle(ozw, src + 9u))); \
            if ((ln & 15u) < 4u) { \
                ivec2 pp = token_pixel(int((t0) + src), win, own); \
                if (own && pp.x + 1 < pc.size.x && pp.y + 1 < pc.size.y) { \
                    ivec2 q = pp >> 1; \
                    f16vec4 po; \
                    for (int c = 0; c < 4; ++c) { \
                        precise float16_t q00 = o00[c] * 0.25hf, q01 = o01[c] * 0.25hf; \
                        precise float16_t q10 = o10[c] * 0.25hf, q11 = o11[c] * 0.25hf; \
                        precise float16_t v = (q00 + q01) + (q10 + q11); \
                        po[c] = min(v, 65504.0hf); \
                    } \
                    F16x4Buf(uint64_t(pc.dfn_pool)).v[uint(q.y * (pc.size.x >> 1) + q.x)] = po; \
                } \
            } }
        DFN2(d0, d1, 0u)
        DFN2(d2, d3, 32u)
    }
#endif
#if !DEC_HEAD
    // TensorOutput of the own tokens straight from the x^T tiles, one 16-byte store per lane
#define XOUT1(xa, xb, tp, own) if (own) { uvec4 w_; [[unroll]] for (int k_ = 0; k_ < 4; ++k_) \
        w_[k_] = e4m3x4(f16vec2(xa[2 * k_], xa[2 * k_ + 1]), f16vec2(xb[2 * k_], xb[2 * k_ + 1])); \
        U32x4View(uint64_t(pc.y)).v[plane_index(tp, int(hh) * 16) >> 4] = w_; }
    XOUT1(x00, x01, tp0, town0) XOUT1(x10, x11, tp1, town1) XOUT1(x20, x21, tp2, town2) XOUT1(x30, x31, tp3, town3)
#endif
#if DEC_HEAD
    // head on two token tiles at once: lane half 0 takes tile a, half 1 tile b. A token's channels 0..7 and 16, 17
    // sit in the lane of its half 0, channels 8..15 in its half 1: one exchange of 5 words across the halves
#define HEAD2(xa0, xa1, xb0, xb1, tpa, owna, tpb, ownb) { \
        bool hi_ = hh != 0u; \
        uvec4 sw_; \
        [[unroll]] for (int k_ = 0; k_ < 4; ++k_) \
            sw_[k_] = hi_ ? packFloat2x16(f16vec2(xa0[2 * k_], xa0[2 * k_ + 1])) : packFloat2x16(f16vec2(xb0[2 * k_], xb0[2 * k_ + 1])); \
        uint s4_ = packFloat2x16(f16vec2(xb1[0], xb1[1])); \
        uvec4 rw_ = uvec4(subgroupShuffleXor(sw_.x, 16u), subgroupShuffleXor(sw_.y, 16u), subgroupShuffleXor(sw_.z, 16u), subgroupShuffleXor(sw_.w, 16u)); \
        f16vec2 r4_ = unpackFloat2x16(subgroupShuffleXor(s4_, 16u)); \
        float16_t dv[18]; \
        [[unroll]] for (int k_ = 0; k_ < 4; ++k_) { \
            f16vec2 r_ = unpackFloat2x16(rw_[k_]); \
            dv[2 * k_] = hi_ ? r_.x : xa0[2 * k_]; dv[2 * k_ + 1] = hi_ ? r_.y : xa0[2 * k_ + 1]; \
            dv[8 + 2 * k_] = hi_ ? xb0[2 * k_] : r_.x; dv[9 + 2 * k_] = hi_ ? xb0[2 * k_ + 1] : r_.y; } \
        dv[16] = hi_ ? r4_.x : xa1[0]; dv[17] = hi_ ? r4_.y : xa1[1]; \
        head_px(hi_ ? tpb : tpa, hi_ ? ownb : owna, dv); }
    HEAD2(x00, x01, x10, x11, tp0, town0, tp1, town1)
    HEAD2(x20, x21, x30, x31, tp2, town2, tp3, town3)
#endif
}
