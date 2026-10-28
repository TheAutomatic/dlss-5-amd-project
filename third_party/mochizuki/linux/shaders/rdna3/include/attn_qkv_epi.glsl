// attn.comp's shipping QKV epilogue (fused QK norm, QKSWAP), moved here
// so the per-tile-mask copies of the projection (NR_ATTN_EDGE) can each carry
// their own: the accumulators then die inside the branch that made them.
            // A lane owns token lane%16 and dims 2*c+lane/16, c = 0..7, of Q and
            // K (fragments 0,1 = Q dims 0-15,16-31; 2,3 = K). The old tree, per
            // token: s_d = q_d^2 + q_{d+16}^2 (and K alongside), then the
            // butterfly 8,4,2,1 over d, i.e. d bit 3, then 2, then 1, then 0. Its result is
            // one value every lane shares. On this layout d = 2*c + lane/16, so d bits 3, 2,
            // 1 are the bits 2, 1, 0 of c and d bit 0 is the lane half: the same tree is
            // a_c = s_c + s_{c+4}, b = (a_0 + a_2, a_1 + a_3), e = b_0 + b_1, and last the
            // half exchange. Each pair add is commutative bit for bit, so this is that value.
            for (int i = 0; i < NR_AM; ++i) {
                NR_FRAG_E4M3 vfr0, vfr1, qf0, qf1, kf0, kf1;
#if NR_ATTN_EDGE
                [[dont_flatten]] if (NR_QDEAD(i) || nr_tile_oob(mtok0 / 16u + uint(i))) {
#else
                [[dont_flatten]] if (nr_tile_oob(mtok0 / 16u + uint(i))) {
#endif
                    const f16vec2 zscale = f16vec2(NR_F16(wgt_f32[pc.s_off + h]), NR_F16(1.0));
                    precise f16vec2 zs = f16vec2(0.0hf) * zscale;
                    const fe4m3vec2 zq = nr_attn_quant(zs), zv = nr_attn_quant(f16vec2(0.0hf));
                    for (int cc = 0; cc < 8; ++cc) {
                        qf0[cc] = zq.x; qf1[cc] = zq.x; kf0[cc] = zq.y; kf1[cc] = zq.y;
                        vfr0[cc] = zv.x; vfr1[cc] = zv.y;
                    }
                } else {
                    for (int cc = 0; cc < 8; ++cc) {
                        const fe4m3vec2 v = nr_attn_quant(f16vec2(qacc[i][4][cc], qacc[i][5][cc]));
                        vfr0[cc] = v.x; vfr1[cc] = v.y;
                    }
                    f16vec2 qk0[8], qk1[8];
                    precise f16vec2 t[8];
                    for (int c = 0; c < 8; ++c) {
                        qk0[c] = f16vec2(NR_F16(qacc[i][0][c]), NR_F16(qacc[i][2][c]));
                        qk1[c] = f16vec2(NR_F16(qacc[i][1][c]), NR_F16(qacc[i][3][c]));
                        precise f16vec2 sq0 = qk0[c] * qk0[c], sq1 = qk1[c] * qk1[c];
                        t[c] = sq0 + sq1;
                    }
                    precise f16vec2 u0 = t[0] + t[4], u1 = t[1] + t[5], u2 = t[2] + t[6], u3 = t[3] + t[7];
                    precise f16vec2 v0 = u0 + u2, v1 = u1 + u3;
                    precise f16vec2 e = v0 + v1;
                    precise f16vec2 w = e + unpackFloat2x16(subgroupShuffleXor(packFloat2x16(e), 16u));
                    const f16vec2 norm = f16vec2(
                        NR_F16(inversesqrt(float(max(w.x, NR_F16(0.000062))))),
                        NR_F16(inversesqrt(float(max(w.y, NR_F16(0.000062))))));
                    const f16vec2 scale = f16vec2(NR_F16(wgt_f32[pc.s_off + h]), NR_F16(1.0));
                    for (int c = 0; c < 8; ++c) {
                        precise f16vec2 n0 = qk0[c] * norm, n1 = qk1[c] * norm;
                        precise f16vec2 scaled0 = n0 * scale, scaled1 = n1 * scale;
                        const fe4m3vec2 q0 = nr_attn_quant(scaled0), q1 = nr_attn_quant(scaled1);
                        qf0[c] = q0.x; kf0[c] = q0.y; qf1[c] = q1.x; kf1[c] = q1.y;
                    }
                }
                // Q/K: element (dim row, token column) at token*NR_QS + dim.
                const uint tb = mtok0 + uint(i) * 16u;
                NR_STORE_ACC_COL(qf0, lds_q, qb + tb * uint(NR_QS), uint(NR_QS));
                NR_STORE_ACC_COL(qf1, lds_q, qb + tb * uint(NR_QS) + 16u, uint(NR_QS));
                NR_STORE_ACC_COL(kf0, lds_k, qb + tb * uint(NR_QS), uint(NR_QS));
                NR_STORE_ACC_COL(kf1, lds_k, qb + tb * uint(NR_QS) + 16u, uint(NR_QS));
                NR_STORE_ACC_COL(vfr0, lds_v, vb + NR_VINDEX(tb, 0u), uint(NR_VS));
                NR_STORE_ACC_COL(vfr1, lds_v, vb + NR_VINDEX(tb, 16u), uint(NR_VS));
            }
