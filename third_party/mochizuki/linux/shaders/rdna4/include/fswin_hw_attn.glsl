// NR_EXP_NOHI_HEAD: the head-split softmax and context of one head, included
// twice (NR_EXPB = the exponential with or without the upper clamp). Copied
// from fswin_body.glsl; keep in step with it.
        NR_OPB pb[NR_MF][NR_JF];
#ifdef NR_DIAG_BIAS_ONCE
        // diagnostic (wrong picture): one bias fragment loaded per window
        // and used for all sixteen blocks - bounds what the bias loads cost.
        NR_FRAG_ACC nr_b1;
        NR_LOAD_ACC_COL(nr_b1, wgt_f32, pc.b_off + uint(hh) * uint(NR_WIN * NR_WIN) + tok0 * uint(NR_WIN),
                        uint(NR_WIN));
#endif
        for (int m = 0; m < NR_MF; ++m) NR_MGA(m) {
            f16vec2 pq2[NR_JF][4];
            NR_ACCF lg[NR_JF];
#if NR_BIAS_SEED
            for (int j = 0; j < NR_JF; ++j) lg[j] = nr_swin_bias_seed(uint(hh),uint(m),uint(j),tok0);
#else
            for (int j = 0; j < NR_JF; ++j) lg[j] = NR_ACCZERO;
#endif
            // One Q fragment, NR_JF products - Q amortises here as K did.
            for (int d = 0; d < NR_DF; ++d)
                for (int j = 0; j < NR_JF; ++j) NR_MGJ(j) {
                    NR_QK_OPA kf = kreg[j][d];
                    NR_MMA(lg[j], kf, qb[m][d]);
                }
            for (int j = 0; j < NR_JF; ++j) {
                // The bias block loads ColumnMajor: it is stored [query][key]
                // and this accumulator is [key][query] - the same addresses,
                // read the other way round.
#if !NR_BIAS_SEED
#if NR_SWIN_BIAS_F16
                NR_FRAG_ACC16 bhalf;
                NR_LOAD_ACC_COL(bhalf, wgt_f16,
                                pc.b_off + uint(hh) * uint(NR_WIN * NR_WIN)
                                + (tok0 + uint(m) * 16u) * uint(NR_WIN) + uint(j) * 16u,
                                uint(NR_WIN));
#if NR_ABLATE_BIAS
                NR_FRAG_ACC bf = NR_ACC_ZERO;
#else
                NR_FRAG_ACC bf = NR_FRAG_ACC(bhalf);
#endif
#elif defined(NR_DIAG_BIAS_ONCE)
                NR_FRAG_ACC bf = nr_b1;
#else
                NR_FRAG_ACC bf;
#if NR_BIAS_TABLE
                NR_BIAS_TBL_LOAD(bf, uint(hh), tok0 + uint(m) * 16u, uint(j))
#else
                NR_LOAD_ACC_COL(bf, wgt_f32,
                                pc.b_off + NR_BFOLD(uint(hh) * uint(NR_WIN * NR_WIN)
                                + (tok0 + uint(m) * 16u) * uint(NR_WIN) + uint(j) * 16u),
                                uint(NR_WIN));
#endif
#endif
#endif
                for (int c = 0; c < 8; c += 2)
                    pq2[j][c / 2] =
#if NR_BIAS_SEED
                        nr_swin_exp2(vec2(lg[j][c],lg[j][c+1]));
#elif NR_BAKED_EXP_BIAS
                        NR_EXPB(vec2(lg[j][c],lg[j][c+1]),vec2(bf[c],bf[c+1]));
#else
                        nr_swin_exp2(vec2(lg[j][c] + bf[c], lg[j][c + 1] + bf[c + 1]));
#endif
            }
            NR_F16 sum = nr_swin_probability_sum(pq2);
            // The f32 spelling is what reaches the free converter - see the
            // r66 note in the token-split path below.
            float inv = float(NR_F16(1.0 / float(max(sum, NR_F16(NR_SUM_FLOOR)))));
            for (int j = 0; j < NR_JF; ++j)
                for (int c = 0; c < 4; ++c) {
                    float x = float(pq2[j][c].x) * inv;
                    float y = float(pq2[j][c].y) * inv;
                    // `NR_N2_P`'s NR_PKN=0/NR_R2=0 expansion is exactly the
                    // `f16vec2(NR_F16(x), NR_F16(y))` that stood here, so the
                    // shipping SPIR-V does not move; the macro is what lets the
                    // narrowing knobs reach the head-split path at all, which
                    // they did not in r75 (the P column of its ledger is a
                    // C=32 measurement and C=32 is the one width with no head
                    // split).
#if NR_PROB_BOUNDED
                    fe4m3vec2 q = fe4m3vec2(NR_N2_P(x,y));
#else
                    fe4m3vec2 q = nr_quant_pair(NR_N2_P(x, y));
#endif
                    pb[m][j][2 * c] = q.x;
                    pb[m][j][2 * c + 1] = q.y;
                }
        }
        // ctx^T = V^T . P^T; this head's output occupies channel fragments
        // [hh*NR_DF, (hh+1)*NR_DF) of the concatenated C rows.
        for (int e = 0; e < NR_DF; ++e) {
            NR_ACCF a[NR_MF];
            for (int m = 0; m < NR_MF; ++m) a[m] = NR_ACCZERO;
            for (int j = 0; j < NR_JF; ++j) NR_MGJ(j) {
                NR_PV_OPA vf;
#if NR_V_REGS
                vf = vreg[e][j];
#elif NR_HWAVES
                NR_LOAD_A_COL(vf, lds_y, NR_LXB_
                              uint(j * NR_CF + hh * NR_DF + e) * 256u
                              + NR_OPQ(192u), 16u);
#else
                NR_LOAD_A(vf, lds_v, NR_WKB NR_V_LDS_OFFSET + uint(e) * 16u * uint(NR_WIN) + uint(j) * 16u,
                          uint(NR_WIN));
#endif
                for (int m = 0; m < NR_MF; ++m) NR_MGA(m) NR_MMA(a[m], vf, pb[m][j]);
                if (NR_ACC_F16 > 0 && (j + 1) % NR_ACC_F16 == 0)
                    for (int m = 0; m < NR_MF; ++m) NR_RND(a[m])
            }
            for (int m = 0; m < NR_MF; ++m) NR_MGA(m) {
#if NR_CONTEXT_LDS || NR_HWAVES
                NR_STAGE_FRAG out_context;
#if NR_QBATCH_ON
#define NR_QV_CTX(c) NR_N2_CTX(a[m][c], a[m][(c) + 1])
                NR_QRUN8(out_context, NR_QV_CTX)
#undef NR_QV_CTX
#elif NR_QUANT_PAIRED
                for (int c = 0; c < 8; c += 2) {
                    NR_QPAIR_T qp = NR_QP_CTX(NR_N2_CTX(a[m][c], a[m][c + 1]));
                    out_context[c] = qp.x;
                    out_context[c + 1] = qp.y;
                }
#else
                for (int c = 0; c < 8; ++c)
                    out_context[c] = nr_quant_e4m3(NR_F16(a[m][c]));
#endif
#if NR_HWAVES
                // Over this wave's own V tiles for dim fragment e, which the j
                // loop above has finished with and no later e reads.
                NR_STORE_ACC_COL(out_context, lds_y, NR_LXB_
                                 uint(m * NR_CF + hh * NR_DF + e) * 256u, 16u);
#else
                NR_STORE_ACC_COL(out_context, lds_context,
                    (tok0+uint(m)*16u)*uint(NR_C)+uint(hh*NR_DF+e)*16u, uint(NR_C));
#endif
#else
#if NR_QBATCH_ON
#define NR_QV_CQ(c) NR_N2_CTX(a[m][c], a[m][(c) + 1])
                NR_QRUN8(cq[m][hh * NR_DF + e], NR_QV_CQ)
#undef NR_QV_CQ
#elif NR_QUANT_PAIRED
                for (int c = 0; c < 8; c += 2) {
                    NR_QPAIR_T qp = NR_QP_CTX(NR_N2_CTX(a[m][c], a[m][c + 1]));
                    cq[m][hh * NR_DF + e][c] = qp.x;
                    cq[m][hh * NR_DF + e][c + 1] = qp.y;
                }
#else
                for (int c = 0; c < 8; ++c)
                    cq[m][hh * NR_DF + e][c] = nr_quant_e4m3(NR_F16(a[m][c]));
#endif
#endif
            }
        }
