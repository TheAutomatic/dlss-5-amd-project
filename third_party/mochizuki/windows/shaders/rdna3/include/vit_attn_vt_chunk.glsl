// The key-block loop of vit_attn.comp's NR_VTRANS path, included twice.
// NR_VT_GUARD 1: a chunk that may run past `pc.tokens` (tiles beyond it read
// zero K and contribute no probability to P.V). NR_VT_GUARD 0: a full chunk,
// where both tests are constant and the zero-or-load phi they created - which
// ACO copied with four byte-inserting `v_perm_b32` a dword - does not exist.
        for (uint kb = 0u; kb < uint(NR_KC); kb += 16u) {
            // K as the A operand: the same addresses its ColumnMajor B load read.
            NR_FRAG_A kfr[2];
            for (uint d = 0u; d < 2u; ++d) {
#if NR_VKMAN && NR_VT_GUARD == 0
                {
                    const uint nrk = kc4 + kb * (X3 >> 2) + d * 64u;
                    // Row `sl % 16` of the tile, all sixteen k values (both lane halves).
                    const fe4m3vec4 r0 = act_x4[nrk], r1 = act_x4[nrk + 1u],
                                    r2 = act_x4[nrk + 2u], r3 = act_x4[nrk + 3u];
                    for (int v = 0; v < 4; ++v) {
                        kfr[d][v] = r0[v]; kfr[d][v + 4] = r1[v];
                        kfr[d][v + 8] = r2[v]; kfr[d][v + 12] = r3[v];
                    }
                }
#elif NR_VADDR && NR_VT_GUARD == 0
                NR_LOAD_A(kfr[d], act_e4m3, kcb + kb * X3 + d * 256u, 16u);
#else
                if (NR_VT_GUARD == 0 || j0 + kb < pc.tokens)
                    NR_LOAD_A(kfr[d], act_e4m3,
                              nr_at16(pc.x_off, j0 + kb, kbase + d * 16u, X3), 16u);
                else kfr[d] = NR_FRAG_A(0.0);
#endif
            }
            // V^T as the A operand, hoisted out of the query blocks. Clamped to
            // the last live key tile for the reason the RowMajor form was: the
            // probabilities that multiply a padding tile are already zero, and a
            // load that is always in bounds is always the same basic block.
            NR_FRAG_A vf[2];
            for (uint n = 0u; n < 2u; ++n)
#if NR_VADDR && NR_VT_GUARD == 0
                NR_LOAD_A_COL(vf[n], act_e4m3, vcb + kb * X3 + n * 256u, 16u);
#else
                NR_LOAD_A_COL(vf[n], act_e4m3,
                              nr_at16(pc.x_off, min(j0 + kb, jlast),
                                      vbase + n * 16u, X3), 16u);
#endif
            for (uint b = 0u; b < uint(NR_QB); ++b) {
                NR_FRAG_ACC lg = NR_ACC_ZERO;
                for (uint d = 0u; d < 2u; ++d) NR_MMA(lg, kfr[d], qfr[b][d]);
                // The probabilities, already rounded onto the e4m3 grid.
                NR_FRAG_ACC16 ph;
                f16vec2 pv[4];
                for (int c = 0; c < lg.length(); c += 2) {
#if NR_ACC_F16 > 0
                    lg[c]   = nr_round_f16(float(lg[c]));
                    lg[c+1] = nr_round_f16(float(lg[c+1]));
#endif
                    vec2 a = vec2(float(lg[c]), float(lg[c+1]));
#if !NR_VPRENORMALIZED
                    // The query scale is this lane's and the key reciprocal is
                    // this component's: the two have traded places with the axes.
                    a *= vec2(qs[b]);
                    a *= vec2(lds_ki[kb + NR_ACC_ROW(sl, c)],
                              lds_ki[kb + NR_ACC_ROW(sl, c + 1)]);
#endif
                    f16vec2 pp = nr_vit_exp2(a);
#if NR_VQP
                    pp = f16vec2(nr_quant_e4m3(pp.x * NR_F16(ps)),
                                 nr_quant_e4m3(pp.y * NR_F16(ps)));
#endif
                    pv[c >> 1] = pp;
                    // The e4m3 rounding of the probability; the denominator keeps `pp`.
                    const f16vec2 pq = nr_quant_pair(pp);
                    ph[c] = pq.x; ph[c+1] = pq.y;
                }
                // The PTX reduction, in lane. On gfx11 component `c` of a lane is key
                // `2*c + l/16`, so a lane holds the keys of one parity and the pair
                // eight keys apart (`k`, `k+8`) is components `c` and `c+4` of the
                // same lane: the other half wave holds the other parity, which is
                // the original `s.x` (even keys) and `s.y` (odd keys). `pv[0]+pv[2]`
                // is the pairs for `i` = 0,1 and `pv[1]+pv[3]` for `i` = 2,3, each
                // accumulated over the chunk in f16 exactly as before.
#if NR_VLATE_SHUFFLE
                // Swin's NR_PROB_LATE_SHUFFLE here - the lane's own values
                // are summed first and the other half wave's total is added once
                // per chunk. Every term stays, f16 throughout; only the order of
                // the half adds changes.
                for (int c = 0; c < lg.length(); c += 2) {
                    const f16vec2 pv2 = pv[c >> 1];
                    part[b][c >> 1] = (kb == 0u) ? pv2 : part[b][c >> 1] + pv2;
                }
                if (kb + 16u == uint(NR_KC)) {
                    f16vec2 s = ((part[b][0] + part[b][1]) + part[b][2]) + part[b][3];
                    s = s + unpackFloat2x16(subgroupShuffleXor(packFloat2x16(s), 16u));
                    den[b] = float(NR_F16(NR_F16(den[b]) + NR_F16(s.x + s.y)));
                }
#else
                {
                    const f16vec2 pa = pv[0] + pv[2], pz = pv[1] + pv[3];
                    part[b][0] = (kb == 0u) ? pa : part[b][0] + pa;
                    part[b][1] = (kb == 0u) ? pz : part[b][1] + pz;
                }
                if (kb + 16u == uint(NR_KC)) {
                    // ((X0+X1)+X2)+X3 of this lane's parity, then the other parity's total.
                    const f16vec2 xa = part[b][0], xz = part[b][1];
                    const NR_F16 mine = ((xa.x + xa.y) + xz.x) + xz.y;
                    const NR_F16 theirs = unpackFloat2x16(
                        subgroupShuffleXor(packFloat2x16(f16vec2(mine, NR_F16(0.0))), 16u)).x;
                    den[b] = float(NR_F16(NR_F16(den[b]) + NR_F16(mine + theirs)));
                }
#endif
                // **P^T is the B operand.** The accumulator is [key][query] and holds
                // keys of one parity per lane half (`2*c + l/16`), while a gfx11 B
                // fragment is the query's sixteen keys in order in *both* halves, so
                // each lane takes the other parity from lane `l ^ 16` (same query):
                // component `c` of this lane is key `2c + l/16`, the partner's is
                // `2c + 1 - l/16`. A padding key tile is killed here exactly as
                // it was before the transpose.
                if (NR_VT_GUARD != 0 && j0 + kb >= pc.tokens) ph = NR_FRAG_ACC16(0.0);
                NR_FRAG_B pf;
                {
                    const bool lo = sl < 16u;
                    for (int c = 0; c < 8; c += 2) {
                        const f16vec2 mine = f16vec2(ph[c], ph[c + 1]);
                        const f16vec2 theirs = unpackFloat2x16(
                            subgroupShuffleXor(packFloat2x16(mine), 16u));
                        pf[2 * c]     = lo ? mine.x : theirs.x;
                        pf[2 * c + 1] = lo ? theirs.x : mine.x;
                        pf[2 * c + 2] = lo ? mine.y : theirs.y;
                        pf[2 * c + 3] = lo ? theirs.y : mine.y;
                    }
                }
                for (uint n = 0u; n < 2u; ++n) {
                    NR_MMA(ctx[b][n], vf[n], pf);
#if NR_ACC_F16 > 0
                    if ((kb/16u+1u)%uint(NR_ACC_F16)==0u)
                        for (int z=0; z<ctx[b][n].length(); ++z)
                            ctx[b][n][z]=nr_round_f16(float(ctx[b][n][z]));
#endif
                }
            }
        }
