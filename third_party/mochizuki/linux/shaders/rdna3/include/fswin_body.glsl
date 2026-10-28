// The per-item body of fswin_t.comp, moved here unchanged so that the
// persistent kernels can instantiate it once per live-tile mask (NR_EDGE_BODIES).
// Everything it declares is local to one instantiation. NR_LIVE (set by the
// includer) is the compile-time mask of the window's in-image tiles; NR_MG,
// NR_MGA and NR_MGK read it.
// NR_LIVE15_NOOOB: with NR_EDGE_BODIES the all-live body (NR_LIVE 15) only ever
// runs windows whose four tiles are in the image (in-image tile sets are
// rectangles, so no other mask reaches it), so its out-of-image tests are constant
// false. Other bodies and non-edge-body kernels keep the runtime test.
#undef NR_TOOB
#if defined(NR_LIVE15_NOOOB) && NR_EDGE_BODIES && defined(NR_LIVE)
#if NR_LIVE15_NOOOB && NR_LIVE == 15
#define NR_TOOB(q) false
#else
#define NR_TOOB(q) nr_tile_oob(q)
#endif
#else
#define NR_TOOB(q) nr_tile_oob(q)
#endif
    const uint wbase = nr_wx * uint(NR_WIN * NR_C);
#if NR_HWAVES
    const uint tok0  = 0u;              // the wave owns every token
    const int  nrhw_h = int(wave);      // ... and exactly one head
    // **An opaque zero, to keep NIR from CSE'ing the LDS operand loads.**
    // `nr_graph` dispatches every fused Swin kernel with gridZ = 1
    // (nr_graph.cpp: `d.gz = 1`), so this is always zero - and nothing in the
    // shader lets the compiler prove it. Two loads of the same fragment whose
    // offsets differ by a distinct multiple of it cannot be merged, so a
    // fragment is re-read where it is used instead of being held live across a
    // whole stage: sixty-four CSE'd fragments were 128 VGPRs. Loads inside one
    // expand pair or one QKV pass share a constant and still merge, which is
    // the reuse those shapes exist for.
    const uint nr_opaque = gl_WorkGroupID.z;
    // 16 elements keeps the offset's alignment provable - a byte-aligned
    // dynamic term would cost the ds_load_b64 lowering.
#define NR_OPQ(kk) (nr_opaque * ((kk) * 16u))
#else
    const uint tok0  = wave * uint(NR_MTOK);
#endif
    // The accumulator's row for component c is NR_ROW(c) = 2c + lane/16: every
    // per-output-channel scalar that is combined with an accumulator component - rs,
    // ars - is indexed by it. `rbase` is something else: the first of the eight
    // *consecutive* channels a lane moves through memory (lane half h: 8h..8h+7), for
    // the vector loads and stores of the blend gathers; NR_OPLIN turns those into
    // operand components.
    const uint rbase = 8u * (lane / 16u);
    const bool nr_hi = lane >= 16u;
#if NR_ACTIVATION_LUT
    for(uint i=tid*16u;i<NR_ACTIVATION_LUT_SIZE;i+=uint(NR_THREADS)*16u) {
        fe4m3vec4 v[4];
        for(int j=0;j<4;++j) v[j]=act_lut4[(pc.rsd_off+i)/4u+uint(j)];
        for(int j=0;j<16;++j) nr_act_lds[i+uint(j)]=v[j/4][j%4];
    }
    NR_BODY_BARRIER();
#endif

#if NR_UPS_SPF
    // NR_UPS_SPF: the skip bytes of every (token tile, channel tile) the
    // blend reads, issued before the upsample projection so their DRAM latency
    // (4K: the encoder wrote them ~15 ms ago) overlaps the projection's MMAs.
    // The same addresses the blend forms; the values are only read earlier.
    fe4m3vec4 nr_spf[NR_MF][NR_CF][2];
    for (int m = 0; m < NR_MF; ++m) {
        const uint q=(tok0+uint(m)*16u)/16u;
        const uint tx=uint(clamp(2*int(nr_wx)+pc.shift+int(q&1u),0,int(pc.tiles_x)-1));
        const uint ty=uint(clamp(2*int(nr_wy)+pc.shift_y+int(q>>1u),0,int(pc.tiles_y)-1));
        const uint stw=pc.blend_stiles_x!=0u?pc.blend_stiles_x:pc.blend_tiles_x;
        const uint stile=ty*stw+tx;
        for (int k = 0; k < NR_CF; ++k) {
            const uint saddr=pc.blend_s_off+(stile*uint(NR_CF)+uint(k))*256u+(lane%16u)*16u+rbase;
            nr_spf[m][k][0]=blend_e4m3x4[saddr/4u]; nr_spf[m][k][1]=blend_e4m3x4[saddr/4u+1u];
        }
    }
#endif
#ifdef NR_FUSED_UPS_PROJECT
    // One 4x4 half-resolution patch feeds this complete 8x8 output window.
    // Gather once, perform all64 input-channel products, then replicate locally.
    NR_FRAG_B upsrc[4];
    const uint ups_slot=lane%16u;
    const uint ups_px=uint(clamp(4*int(nr_wx)+2*pc.shift+int(ups_slot%4u),0,int(pc.blend_itiles_x*4u)-1));
    const uint ups_py=uint(clamp(4*int(nr_wy)+2*pc.shift_y+int(ups_slot/4u),0,int(pc.blend_itiles_y*4u)-1));
    const uint ups_tile=(ups_py/4u)*pc.blend_itiles_x+ups_px/4u;
    const uint ups_in_slot=(ups_py%4u)*4u+ups_px%4u;
#if NR_UPS_KOUTER
    // k outermost - one input fragment and every output accumulator
    // live, instead of all four input fragments held across the n loop. Each
    // accumulator still reduces k in ascending order: the same products in
    // the same order, so the same bits. **No effect**: still 240 VGPRs at
    // C=32/64/128 - the 240 is ACO spending the registers the LDS-limited
    // occupancy (6 waves) leaves free; NR_UPS_ALIAS gets 192 / 8 waves, and
    // that measured as no time change. Not shipped.
    NR_FRAG_ACC uacc[2];
    for(int n=0;n<2;++n) uacc[n]=NR_ACC_ZERO;
    for(int k=0;k<4;++k) {
        for(int j=0;j<16;++j)
            upsrc[k][j]=act_e4m3[pc.blend_p_off+(ups_tile*4u+uint(k))*256u+ups_in_slot*16u+uint(j)];
        for(int n=0;n<2;++n) {
            NR_FRAG_A w;
            NR_LOAD_A(w,wgt_e4m3,pc.ups_weight_off+uint(n*4+k)*256u,16u);
            NR_MMA(uacc[n],w,upsrc[k]);
        }
    }
    for(int n=0;n<2;++n) {
        NR_FRAG_ACC16 half_result;
        for(int j=0;j<8;++j)half_result[j]=NR_F16(uacc[n][j]);
        NR_STORE_ACC_COL(half_result,ups_projected,uint(n)*256u,16u);
    }
#else
    for(int k=0;k<4;++k)
        for(int j=0;j<16;++j)
            upsrc[k][j]=act_e4m3[pc.blend_p_off+(ups_tile*4u+uint(k))*256u+ups_in_slot*16u+uint(j)];
    for(int n=0;n<2;++n) {
        NR_FRAG_ACC acc=NR_ACC_ZERO;
        for(int k=0;k<4;++k) {
            NR_FRAG_A w;
            NR_LOAD_A(w,wgt_e4m3,pc.ups_weight_off+uint(n*4+k)*256u,16u);
            NR_MMA(acc,w,upsrc[k]);
        }
        NR_FRAG_ACC16 half_result;
        for(int j=0;j<8;++j)half_result[j]=NR_F16(acc[j]);
        NR_STORE_ACC_COL(half_result,ups_projected,uint(n)*256u,16u);
    }
#endif
    NR_BODY_BARRIER();
#endif

#ifdef NR_WIDE_UPS_PROJECT
#if NR_PERSIST_UPS
    [[dont_flatten]] if (nr_ups_layer) {
#endif
    // Each head owns32 output channels; all2C inputs participate in projection.
    // The existing view adapter, when needed, has already transformed input.
#if NR_UPS_KOUTER && defined(NR_WIDE_UPS_VIEW)
#error "NR_UPS_KOUTER: the plain wide gather only"
#endif
    NR_FRAG_B upsrc[2*NR_CF];
    const uint ups_slot=lane%16u;
    const uint ups_px=uint(clamp(4*int(nr_wx)+2*pc.shift+int(ups_slot%4u),0,int(pc.blend_itiles_x*4u)-1));
    const uint ups_py=uint(clamp(4*int(nr_wy)+2*pc.shift_y+int(ups_slot/4u),0,int(pc.blend_itiles_y*4u)-1));
    const uint ups_tile=(ups_py/4u)*pc.blend_itiles_x+ups_px/4u;
    const uint ups_in_slot=(ups_py%4u)*4u+ups_px%4u;
#if NR_UPS_KOUTER
    // k outermost, as in the C=32 body above - one input fragment and
    // NR_DF accumulators live instead of 2*NR_CF input fragments; ascending k
    // into every accumulator, so the same bits.
    NR_FRAG_ACC uacc[NR_DF];
    for(int n=0;n<NR_DF;++n) uacc[n]=NR_ACC_ZERO;
    for(int k=0;k<2*NR_CF;++k) {
        NR_FRAG_B us;
        for(int j=0;j<16;++j)
            us[j]=act_e4m3[pc.blend_p_off+(ups_tile*uint(2*NR_CF)+uint(k))*256u+ups_in_slot*16u+uint(j)];
        for(int n=0;n<NR_DF;++n) {
            const uint nf=uint(nrhw_h*NR_DF+n);
            NR_FRAG_A w;
            NR_LOAD_A(w,wgt_e4m3,pc.ups_weight_off+(nf*uint(2*NR_CF)+uint(k))*256u,16u);
            NR_MMA(uacc[n],w,us);
        }
    }
    for(int n=0;n<NR_DF;++n) {
        const uint nf=uint(nrhw_h*NR_DF+n);
        NR_FRAG_ACC16 half_result;
        for(int j=0;j<8;++j)half_result[j]=NR_F16(uacc[n][j]);
        NR_STORE_ACC_COL(half_result,ups_projected,nf*256u,16u);
    }
#else
    for(int k=0;k<2*NR_CF;++k)
        for(int j=0;j<16;++j)
#ifdef NR_WIDE_UPS_VIEW
        {
            const uint token=ups_tile*16u+ups_in_slot;
            const uint ch=uint(k)*16u+uint(j);
            if(pc.blend_o_off==0u) {
                upsrc[k][j]=act_e4m3[pc.blend_p_off+(ups_tile*uint(2*NR_CF)+uint(k))*256u+ups_in_slot*16u+uint(j)];
            } else {
                // Exact byte view from upsample_view.comp, fused into gather.
                const uint W=pc.blend_o_off,H=pc.blend_mode;
                const uint rw=(W+3u)/4u*4u,rh=(H+3u)/4u*4u;
                const uint x=(token/16u%(rw/4u))*4u+token%4u;
                const uint y=(token/16u/(rw/4u))*4u+token%16u/4u;
                const uint p=(ch/16u)*rw*rh+y*rw+x;
                const uint sc=p/(W*H)*16u+ch%16u,pixel=p%(W*H);
                const uint sx=pixel%W,sy=pixel/W;
                const uint st=(sy/4u*(W/4u)+sx/4u)*16u+sy%4u*4u+sx%4u;
                upsrc[k][j]=NR_E4M3(0.0);
                if(y<rh && sc<uint(2*NR_C) && sx<(W/4u)*4u)
                    upsrc[k][j]=act_e4m3[pc.blend_p_off+(st/16u*uint(2*NR_CF)+sc/16u)*256u+st%16u*16u+sc%16u];
            }
        }
#else
            upsrc[k][j]=act_e4m3[pc.blend_p_off+(ups_tile*uint(2*NR_CF)+uint(k))*256u+ups_in_slot*16u+uint(j)];
#endif
    for(int n=0;n<NR_DF;++n) {
        const uint nf=uint(nrhw_h*NR_DF+n);
        NR_FRAG_ACC acc=NR_ACC_ZERO;
        for(int k=0;k<2*NR_CF;++k) {
            NR_FRAG_A w;
            NR_LOAD_A(w,wgt_e4m3,pc.ups_weight_off+(nf*uint(2*NR_CF)+uint(k))*256u,16u);
            NR_MMA(acc,w,upsrc[k]);
        }
        NR_FRAG_ACC16 half_result;
        for(int j=0;j<8;++j)half_result[j]=NR_F16(acc[j]);
        NR_STORE_ACC_COL(half_result,ups_projected,nf*256u,16u);
    }
#endif
    NR_BODY_BARRIER();
#if NR_PERSIST_UPS
    }
#endif
#endif

    // ---- stage 1: e = act(E . x) -----------------------------------------
    // x is the B operand and never leaves registers; the MLP residual reads the
    // same fragments later.
    // The tile each token fragment lives in. In window-major mode this is the
    // same arithmetic NR_TILE was doing; in image mode it is the only place the
    // window's position enters, and the output epilogue reuses it unchanged.
    uint tbase[NR_MF];
    for (int m = 0; m < NR_MF; ++m)
#if NR_IMAGE
        tbase[m] = pc.x_off + nr_tile_base((tok0 + uint(m) * 16u) / 16u);
#else
        tbase[m] = NR_TILE(pc.x_off + wbase, tok0 + uint(m) * 16u, 0u, uint(NR_C));
#endif
#if !NR_HWAVES
    NR_FRAG_B xb[NR_MF][NR_CF];
#endif
#if NR_HWAVES
// The B operand every dense stage reads. With one wave per head it is a load
// from the exchange buffer instead of a register, and the loop that needs it
// declares `xbk` just above its own MMA.
#define NR_XB(m, k) xbk[m]
#else
#define NR_XB(m, k) xb[m][k]
#endif
#ifdef NR_INPUT_F16
#if NR_XH_ACC
    NR_F16 xh[NR_MF][NR_CF][8];
#else
    NR_FRAG_B16 xh[NR_MF][NR_CF];
#endif
#endif
#ifdef NR_FUSED_IMAGE_INPUT
    NR_FRAG_B16 image_features[NR_MF];
    for(int m=0;m<NR_MF;++m)
        NR_LOAD_B(image_features[m],input_features,(tok0+uint(m)*16u)*16u,16u);
#endif
#if NR_HWAVES
    // Stage the window's x once. Wave h takes channel fragments
    // [h*NR_DF, (h+1)*NR_DF) of all four token tiles - the fragments partition
    // exactly, because NR_DF * NR_HEADS == NR_CF - and writes them at the same
    // (m*NR_CF + k)*256 the register form addressed the arena with, so the load
    // back is byte for byte the fragment xb[m][k] used to be.
#if NR_PERSIST_UPS
    // Two whole loops, each storing its own fragments - one loop with the
    // source chosen per fragment would merge two e4m3 fragments in a phi.
    [[dont_flatten]] if (!nr_ups_layer) {
        for (int m = 0; m < NR_MF; ++m)
            for (int d = 0; d < NR_DF; ++d) {
                const uint k = uint(nrhw_h * NR_DF + d);
                // The arena tile read ColumnMajor into an Accumulator is the tile the
                // ColumnMajor store below writes: no component hops through a B operand.
                NR_FRAG_E4M3 dst;
                NR_LOAD_ACC_COL(dst, act_e4m3, tbase[m] + k * 256u, 16u);
                if (NR_TOOB(uint(m)))
                    for (int j = 0; j < 8; ++j) dst[j] = NR_E4M3(0.0);
                NR_STORE_ACC_COL(dst, lds_x, NR_LXB_ (uint(m) * uint(NR_CF) + k) * 256u, 16u);
            }
    } else
#endif
    for (int m = 0; m < NR_MF; ++m)
        for (int d = 0; d < NR_DF; ++d) {
            const uint k = uint(nrhw_h * NR_DF + d);
#ifdef NR_FUSED_UPS_BLEND
            const uint q=uint(m);
            const uint tx=uint(clamp(2*int(nr_wx)+pc.shift+int(q&1u),0,int(pc.tiles_x)-1));
            const uint ty=uint(clamp(2*int(nr_wy)+pc.shift_y+int(q>>1u),0,int(pc.tiles_y)-1));
            const uint slot=lane%16u,dx=slot%4u,dy=slot/4u;
            const uint ipx=min((4u*tx+dx)/2u,pc.blend_itiles_x*4u-1u);
            const uint ipy=min((4u*ty+dy)/2u,pc.blend_itiles_y*4u-1u);
            const uint itile=(ipy/4u)*pc.blend_itiles_x+ipx/4u;
            const uint islot=(ipy%4u)*4u+ipx%4u;
            const uint stw=pc.blend_stiles_x!=0u?pc.blend_stiles_x:pc.blend_tiles_x;
            const uint stile=ty*stw+tx;
            // This lane's eight consecutive channels rbase..rbase+7 of its token, written
            // to the tile in memory order below - no fragment is involved.
            NR_E4M3 src[8];
#if NR_UPS_BLEND_PK >= 2 && defined(NR_WIDE_UPS_PROJECT)
            // NR_UPS_BLEND_PK=2: the head-split blend two channels an instruction.
            // e4m3 -> f16 is exact, and ACO already contracted sv*g + pv into one
            // v_fma_f16 a channel, which v_pk_fma_f16 rounds the same way per half.
            {
                const uint saddr=pc.blend_s_off+(stile*uint(NR_CF)+k)*256u+slot*16u+rbase;
                const fe4m3vec4 sv4[2]={blend_e4m3x4[saddr/4u], blend_e4m3x4[saddr/4u+1u]};
                const uint local_slot=((q>>1u)*2u+dy/2u)*4u+(q&1u)*2u+dx/2u;
                for(int j=0;j<8;j+=2) {
                    const uint ch=k*16u+rbase+uint(j);
                    const uint pb=k*256u+local_slot*16u+rbase+uint(j);
                    const f16vec2 pv2=f16vec2(ups_projected[pb],ups_projected[pb+1u]);
                    const f16vec2 sv2=unpackFloat2x16(packHalf2x16(vec2(
                        float(sv4[j/4][j%4]), float(sv4[j/4][j%4+1]))));
                    const f16vec2 g2=f16vec2(wgt_f16[pc.blend_g_off+ch],wgt_f16[pc.blend_g_off+ch+1u]);
                    const fe4m3vec2 qv=nr_quant_pair(fma(sv2,g2,pv2));
                    src[j]=qv.x; src[j+1]=qv.y;
                }
            }
            if(false)
#endif
            for(int j=0;j<8;++j) {
                const uint ch=k*16u+rbase+uint(j);
#ifdef NR_WIDE_UPS_PROJECT
                const uint local_slot=((q>>1u)*2u+dy/2u)*4u+(q&1u)*2u+dx/2u;
                NR_F16 pv=ups_projected[k*256u+local_slot*16u+rbase+uint(j)];
#else
                NR_F16 pv=act_f16[pc.blend_p_off+(itile*uint(NR_CF)+k)*256u+islot*16u+rbase+uint(j)];
#endif
                NR_F16 sv=NR_F16(act_e4m3[pc.blend_s_off+(stile*uint(NR_CF)+k)*256u+slot*16u+rbase+uint(j)]);
                NR_F16 sp=NR_F16(sv*wgt_f16[pc.blend_g_off+ch]);
                // Native wide path quantizes the half blend before residual/MLP.
                src[j]=nr_quant_e4m3(NR_F16(pv+sp));
            }
#if NR_IMAGE
            // The same zero fill the per-component path does at line 615.
            if (NR_TOOB(uint(m)))
                for (int j = 0; j < 8; ++j) src[j] = NR_E4M3(0.0);
#endif
            // Tile-blocked [token][16 channels]: the layout NR_STORE_ACC_COL(.., 16u) writes.
            for (int j = 0; j < 8; ++j)
                lds_x[NR_LXB_ (uint(m) * uint(NR_CF) + k) * 256u + slot * 16u + rbase + uint(j)] = src[j];
#else
            NR_FRAG_E4M3 dst;
            NR_LOAD_ACC_COL(dst, act_e4m3, tbase[m] + k * 256u, 16u);
#if NR_IMAGE
            // The same zero fill the per-component path does at line 615.
            if (NR_TOOB(uint(m)))
                for (int j = 0; j < 8; ++j) dst[j] = NR_E4M3(0.0);
#endif
            NR_STORE_ACC_COL(dst, lds_x, NR_LXB_ (uint(m) * uint(NR_CF) + k) * 256u, 16u);
#endif
        }
#if NR_SWI4
    // NR_SWI4: this wave's 32 channels (one int4 k step, 2g) of every token tile, from its own lds_x stores. The
    // k step's record slots are (token l%16, half hh = l/16): half hh holds the 16 channels of fragment 2g+hh as
    // 8 bytes. A gfx11 B fragment has all 16 channels of the lane's token in both lane halves, so each lane loads
    // both fragments of the step and packs the one its half owns: no exchange with the lane 16 away.
    {
        subgroupMemoryBarrierShared();
        const uint xq = pc.e_off / 4u;
        const uint qix = wgt_u32[xq + 1u];
        const uint hh = lane / 16u;
        for (int m = 0; m < NR_MF; ++m) NR_MSK4(NR_MG(m)) {
            NR_FRAG_B xf0, xf1;
            NR_LOAD_B(xf0, lds_x, NR_LXB_ (uint(m) * uint(NR_CF) + uint(nrhw_h * 2)) * 256u, 16u);
            NR_LOAD_B(xf1, lds_x, NR_LXB_ (uint(m) * uint(NR_CF) + uint(nrhw_h * 2 + 1)) * 256u, 16u);
            float v_[16];
            for (int j = 0; j < 16; ++j) v_[j] = hh == 0u ? float(xf0[j]) : float(xf1[j]);
            const uint qb = qix + (uint(nrhw_h * 2) + hh) * 16u;
            float lo_[8], hi_[8];
            for (int j = 0; j < 8; ++j) { lo_[j] = v_[j]; hi_[j] = v_[8 + j]; }
            const uvec2 w2 = uvec2(nr_s4pack(lo_, qb), nr_s4pack(hi_, qb + 8u));
            const uint a4 = NR_LXB_ NR_SWI4_X4 + uint(m) * uint(NR_C * 2) + uint(nrhw_h / 2) * 128u + lane * 4u + uint(nrhw_h % 2) * 2u;
            lds_y4[a4] = w2.x; lds_y4[a4 + 1u] = w2.y;
        }
    }
#endif
    NR_BODY_BARRIER();
#else
#if defined(NR_FUSED_POST_BLEND) && NR_POST_BLEND_PK && NR_BLEND_VECTOR_LOAD && NR_POST_REUSE && !NR_BLEND_G32
    // NR_POST_REUSE: the main side is a nearest 2x upsample, so a window's
    // 8x8 outputs read 4x4 distinct main pixels, each decoded and scaled four times. In a window
    // whose four tiles and main footprint need no clamping, lane p (= lane & 15) makes main pixel
    // p's scaled half pair once a k, and each tile takes it from lane (lane & 16) | p. The same
    // f16 products reach the same FMAs; windows at the borders keep the per-tile path.
#if NR_FWAVES != 1
#error "NR_POST_REUSE: one wave a window (the tile index is the fragment index m)"
#endif
    const int nr_rx0 = 2 * int(nr_wx) + pc.shift, nr_ry0 = 2 * int(nr_wy) + pc.shift_y;
    const bool nr_reuse = nr_rx0 >= 0 && nr_ry0 >= 0 && nr_rx0 + 1 <= int(pc.tiles_x) - 1 &&
                          nr_ry0 + 1 <= int(pc.tiles_y) - 1 &&
                          2 * nr_rx0 + 3 <= int(pc.blend_itiles_x * 4u) - 1 &&
                          2 * nr_ry0 + 3 <= int(pc.blend_itiles_y * 4u) - 1;
    [[dont_flatten]] if (nr_reuse) {
#if NR_POST_GAINBC
        // lane L loads one gain pair - main (bit 3 = 0) or skip (bit 3 = 1) pair
        // 8*((L>>2)&1) + 4*(L>>4) + (L&3) of the 16; channel pair (k, j) of lane L is then at lane
        // (L & 16) | main/skip<<3 | k<<2 | j/2: a DPP row_share read, no per-lane gain loads.
        const uint nr_gl = gl_SubgroupInvocationID;
        const uint nr_gp = 8u * ((nr_gl >> 2u) & 1u) + 4u * (nr_gl >> 4u) + (nr_gl & 3u);
        const uint nr_gword = wgt_u32[(pc.blend_g_off + (((nr_gl >> 3u) & 1u) != 0u ? 0u : uint(NR_C)) + 2u * nr_gp) >> 1u];
#define NR_GAIN_MAIN(k_, j_) unpackFloat2x16(subgroupShuffle(nr_gword, (gl_SubgroupInvocationID & 16u) | (uint(k_) << 2u) | (uint(j_) >> 1u)))
#define NR_GAIN_SKIP(k_, j_) unpackFloat2x16(subgroupShuffle(nr_gword, (gl_SubgroupInvocationID & 16u) | 8u | (uint(k_) << 2u) | (uint(j_) >> 1u)))
#endif
        uint nr_mp[NR_CF][4];
        // Lane L makes main pixel (2*b0 + b1, 2*b2 + b3) of the window's 4x4 (b = L's bits), so the
        // lane a tile's destination reads is (lane & 0x1A) | tile bits: a masked swizzle, which
        // RADV's tid-function pass turns into DPP8 (a VALU move) instead of ds_bpermute.
        const uint nr_sl = gl_SubgroupInvocationID;
        const uint nr_px = uint(2 * nr_rx0) + 2u * (nr_sl & 1u) + ((nr_sl >> 1u) & 1u);
        const uint nr_py = uint(2 * nr_ry0) + 2u * ((nr_sl >> 2u) & 1u) + ((nr_sl >> 3u) & 1u);
        const uint nr_it = (nr_py / 4u) * pc.blend_itiles_x + nr_px / 4u, nr_is = (nr_py % 4u) * 4u + nr_px % 4u;
        for (int k = 0; k < NR_CF; ++k) {
            const uint paddr = pc.blend_p_off + (nr_it * uint(NR_CF) + uint(k)) * 256u + nr_is * 16u + rbase;
            const fe4m3vec4 pv4[2] = {blend_e4m3x4[paddr / 4u], blend_e4m3x4[paddr / 4u + 1u]};
            for (int j = 0; j < 8; j += 2) {
                const uint ch = uint(k) * 16u + rbase + uint(j);
                const f16vec2 pv2 = unpackFloat2x16(packHalf2x16(vec2(float(pv4[j / 4][j % 4]), float(pv4[j / 4][j % 4 + 1]))));
#if NR_POST_GAINBC
                const f16vec2 gm2 = NR_GAIN_MAIN(k, j);
#else
                const f16vec2 gm2 = f16vec2(wgt_f16[pc.blend_g_off + uint(NR_C) + ch], wgt_f16[pc.blend_g_off + uint(NR_C) + ch + 1u]);
#endif
                nr_mp[k][j / 2] = packFloat2x16(pv2 * gm2);
            }
        }
        // Every tile of the window: the skip pixel of this lane, the main product of its supplier.
        const uint nr_slot = lane % 16u;
        const uint nr_stw = pc.blend_stiles_x != 0u ? pc.blend_stiles_x : pc.blend_tiles_x;
        for (int m = 0; m < NR_MF; ++m)
            for (int k = 0; k < NR_CF; ++k) {
                const uint nr_stile = (uint(nr_ry0) + (uint(m) >> 1u)) * nr_stw + uint(nr_rx0) + (uint(m) & 1u);
                const uint saddr = pc.blend_s_off + (nr_stile * uint(NR_CF) + uint(k)) * 256u + nr_slot * 16u + rbase;
                const fe4m3vec4 sv4[2] = {blend_e4m3x4[saddr / 4u], blend_e4m3x4[saddr / 4u + 1u]};
                const uint nr_sup = (gl_SubgroupInvocationID & 0x1Au) | (uint(m) & 1u) | ((uint(m) >> 1u) << 2u);
                for (int j = 0; j < 8; j += 2) {
                    const uint ch = uint(k) * 16u + rbase + uint(j);
                    const f16vec2 sv2 = unpackFloat2x16(packHalf2x16(vec2(float(sv4[j / 4][j % 4]), float(sv4[j / 4][j % 4 + 1]))));
#if NR_POST_GAINBC
                    const f16vec2 gs2 = NR_GAIN_SKIP(k, j);
#else
                    const f16vec2 gs2 = f16vec2(wgt_f16[pc.blend_g_off + ch], wgt_f16[pc.blend_g_off + ch + 1u]);
#endif
#if NR_DIAG_NOBLEND
                    const f16vec2 xh2 = sv2;   // diagnostic (wrong picture): no main/gain math
#else
                    const f16vec2 xh2 = fma(sv2, gs2, unpackFloat2x16(subgroupShuffle(nr_mp[k][j / 2], nr_sup)));
#endif
                    NR_OPLIN(xh[m][k], j, xh2)
                }
            }
    }
#endif
    for (int m = 0; m < NR_MF; ++m)
        for (int k = 0; k < NR_CF; ++k) {
#ifdef NR_INPUT_F16
#ifdef NR_FUSED_IMAGE_INPUT
            NR_FRAG_A16 lift;
            NR_LOAD_A(lift,wgt_f16,pc.image_lift_off+uint(k)*256u,16u);
#if NR_LIFT_ACC16
            // research: the lift's 16-term product into an f16 accumulator.
            NR_FRAG_ACC16 lifted16=NR_FRAG_ACC16(0.0);NR_MMA(lifted16,lift,image_features[m]);
            for(int j=0;j<8;j+=2) NR_OPPUT(xh[m][k], j, f16vec2(lifted16[j], lifted16[j+1]))
#else
            NR_FRAG_ACC lifted=NR_ACC_ZERO;NR_MMA(lifted,lift,image_features[m]);
#endif
#if NR_LIFT_ACC16
#elif NR_LIFT_PRECISE
            // `precise` keeps the lift's native f16 rounding without the
            // tile branch NR_PRE_FULL removes (see the bit-64 note below).
            for(int j=0;j<8;j+=2) {
                precise NR_F16 nr_lh=NR_F16(lifted[j]); precise NR_F16 nr_lh1=NR_F16(lifted[j+1]);
                NR_XHPUT(xh[m][k], j, f16vec2(nr_lh, nr_lh1))
            }
#else
            for(int j=0;j<8;j+=2) NR_XHPUT(xh[m][k], j, f16vec2(NR_F16(lifted[j]), NR_F16(lifted[j+1])))
#endif
#elif defined(NR_FUSED_UPS_BLEND)
            const uint q=(tok0+uint(m)*16u)/16u;
            const uint tx=uint(clamp(2*int(nr_wx)+pc.shift+int(q&1u),0,int(pc.tiles_x)-1));
            const uint ty=uint(clamp(2*int(nr_wy)+pc.shift_y+int(q>>1u),0,int(pc.tiles_y)-1));
            const uint slot=lane%16u,dx=slot%4u,dy=slot/4u;
            const uint ipx=min((4u*tx+dx)/2u,pc.blend_itiles_x*4u-1u);
            const uint ipy=min((4u*ty+dy)/2u,pc.blend_itiles_y*4u-1u);
            const uint itile=(ipy/4u)*pc.blend_itiles_x+ipx/4u;
            const uint islot=(ipy%4u)*4u+ipx%4u;
            const uint stw=pc.blend_stiles_x!=0u?pc.blend_stiles_x:pc.blend_tiles_x;
            const uint stile=ty*stw+tx;
            // The lane's eight consecutive channels, in memory order; NR_OPLIN below turns
            // them into the sixteen components of the B operand.
            NR_F16 nr_xl[8];
#if NR_UPS_BLEND_PK
            // The scalar chain below compiles to one v_fma_f16 a channel
            // (ACO contracts sv*g into the add); v_pk_fma_f16 rounds the same
            // way per half. e4m3 -> f16 is exact, so the pair pack loses nothing.
            {
#if NR_UPS_SPF
                const fe4m3vec4 sv4[2]={nr_spf[m][k][0], nr_spf[m][k][1]};
#else
                const uint saddr=pc.blend_s_off+(stile*uint(NR_CF)+uint(k))*256u+slot*16u+rbase;
                const fe4m3vec4 sv4[2]={blend_e4m3x4[saddr/4u], blend_e4m3x4[saddr/4u+1u]};
#endif
                for(int j=0;j<8;j+=2) {
                    const uint ch=uint(k)*16u+rbase+uint(j);
#ifdef NR_FUSED_UPS_PROJECT
                    const uint local_slot=((q>>1u)*2u+dy/2u)*4u+(q&1u)*2u+dx/2u;
                    const uint pb=uint(k)*256u+local_slot*16u+rbase+uint(j);
                    const f16vec2 pv2=f16vec2(ups_projected[pb],ups_projected[pb+1u]);
#else
                    const uint pb=pc.blend_p_off+(itile*uint(NR_CF)+uint(k))*256u+islot*16u+rbase+uint(j);
                    const f16vec2 pv2=f16vec2(act_f16[pb],act_f16[pb+1u]);
#endif
                    const f16vec2 sv2=unpackFloat2x16(packHalf2x16(vec2(
                        float(sv4[j/4][j%4]), float(sv4[j/4][j%4+1]))));
#if NR_BLEND_G32
                    const f16vec2 g2=unpackFloat2x16(wgt_u32[(pc.blend_g_off+ch)>>1u]);
#else
                    const f16vec2 g2=f16vec2(wgt_f16[pc.blend_g_off+ch],wgt_f16[pc.blend_g_off+ch+1u]);
#endif
                    const f16vec2 xh2=fma(sv2,g2,pv2);
                    nr_xl[j]=xh2.x; nr_xl[j+1]=xh2.y;
                }
            }
            if(false)
#endif
            for(int j=0;j<8;++j) {
                const uint ch=uint(k)*16u+rbase+uint(j);
#ifdef NR_FUSED_UPS_PROJECT
                const uint local_slot=((q>>1u)*2u+dy/2u)*4u+(q&1u)*2u+dx/2u;
                NR_F16 pv=ups_projected[uint(k)*256u+local_slot*16u+rbase+uint(j)];
#else
                NR_F16 pv=act_f16[pc.blend_p_off+(itile*uint(NR_CF)+uint(k))*256u+islot*16u+rbase+uint(j)];
#endif
                NR_F16 sv=NR_F16(act_e4m3[pc.blend_s_off+(stile*uint(NR_CF)+uint(k))*256u+slot*16u+rbase+uint(j)]);
                NR_F16 sp=NR_F16(sv*wgt_f16[pc.blend_g_off+ch]);
                nr_xl[j]=NR_F16(pv+sp);
            }
            for(int j=0;j<8;j+=2) NR_OPLIN(xh[m][k], j, f16vec2(nr_xl[j], nr_xl[j+1]))
#elif defined(NR_FUSED_POST_BLEND)
#if NR_POST_REUSE
            [[dont_flatten]] if (!nr_reuse) {
#endif
            // Gather the same half blend as upsample_blend.comp mode 6.
            // Retain both product roundings and the sum rounding.
            const uint q=(tok0+uint(m)*16u)/16u;
            const uint tx=uint(clamp(2*int(nr_wx)+pc.shift+int(q&1u),0,int(pc.tiles_x)-1));
            const uint ty=uint(clamp(2*int(nr_wy)+pc.shift_y+int(q>>1u),0,int(pc.tiles_y)-1));
            const uint slot=lane%16u, dx=slot%4u,dy=slot/4u;
            const uint ipx=min((4u*tx+dx)/2u,pc.blend_itiles_x*4u-1u);
            const uint ipy=min((4u*ty+dy)/2u,pc.blend_itiles_y*4u-1u);
            const uint itile=(ipy/4u)*pc.blend_itiles_x+ipx/4u;
            const uint islot=(ipy%4u)*4u+ipx%4u;
            const uint stw=pc.blend_stiles_x!=0u?pc.blend_stiles_x:pc.blend_tiles_x;
            const uint stile=ty*stw+tx;
            // The lane's eight consecutive channels, in memory order; NR_OPLIN below turns
            // them into the sixteen components of the B operand.
            NR_F16 nr_xl[8];
#if NR_BLEND_VECTOR_LOAD
            // The eight channels belong to one aligned half-fragment. Fetch
            // them as two dwords instead of eight independent byte loads.
            const uint paddr=pc.blend_p_off+(itile*uint(NR_CF)+uint(k))*256u+islot*16u+rbase;
            const uint saddr=pc.blend_s_off+(stile*uint(NR_CF)+uint(k))*256u+slot*16u+rbase;
            fe4m3vec4 pv4[2], sv4[2];
            for (int v=0;v<2;++v) {
                pv4[v]=blend_e4m3x4[paddr/4u+uint(v)];
                sv4[v]=blend_e4m3x4[saddr/4u+uint(v)];
            }
#endif
#if NR_POST_BLEND_PK && NR_BLEND_VECTOR_LOAD
            // The same blend two channels an instruction. The shipped
            // code is mp = f16(pv*g_main) then fma(sv, g_skip, mp) (ACO fuses
            // the skip product into the add), and pk_mul/pk_fma round exactly
            // like v_mul_f16/v_fma_f16. e4m3 -> f16 is exact, so the
            // round-toward-zero pair pack loses nothing.
            for(int j=0;j<8;j+=2) {
                const uint ch=uint(k)*16u+rbase+uint(j);
                const f16vec2 pv2=unpackFloat2x16(packHalf2x16(vec2(
                    float(pv4[j/4][j%4]), float(pv4[j/4][j%4+1]))));
                const f16vec2 sv2=unpackFloat2x16(packHalf2x16(vec2(
                    float(sv4[j/4][j%4]), float(sv4[j/4][j%4+1]))));
#if NR_BLEND_G32
                const f16vec2 gm2=unpackFloat2x16(wgt_u32[(pc.blend_g_off+uint(NR_C)+ch)>>1u]);
                const f16vec2 gs2=unpackFloat2x16(wgt_u32[(pc.blend_g_off+ch)>>1u]);
#else
                const f16vec2 gm2=f16vec2(wgt_f16[pc.blend_g_off+uint(NR_C)+ch],
                                          wgt_f16[pc.blend_g_off+uint(NR_C)+ch+1u]);
                const f16vec2 gs2=f16vec2(wgt_f16[pc.blend_g_off+ch],
                                          wgt_f16[pc.blend_g_off+ch+1u]);
#endif
                const f16vec2 xh2=fma(sv2,gs2,pv2*gm2);
                nr_xl[j]=xh2.x; nr_xl[j+1]=xh2.y;
            }
            if(false)
#endif
            for(int j=0;j<8;++j) {
                const uint ch=uint(k)*16u+rbase+uint(j);
#if NR_BLEND_VECTOR_LOAD
                const NR_F16 pv=NR_F16(pv4[j/4][j%4]);
                const NR_F16 sv=NR_F16(sv4[j/4][j%4]);
#else
                const NR_F16 pv=NR_F16(act_e4m3[pc.blend_p_off+(itile*uint(NR_CF)+uint(k))*256u+islot*16u+rbase+uint(j)]);
                const NR_F16 sv=NR_F16(act_e4m3[pc.blend_s_off+(stile*uint(NR_CF)+uint(k))*256u+slot*16u+rbase+uint(j)]);
#endif
                NR_F16 mp=NR_F16(pv*wgt_f16[pc.blend_g_off+uint(NR_C)+ch]);
                NR_F16 sp=NR_F16(sv*wgt_f16[pc.blend_g_off+ch]);
                nr_xl[j]=NR_F16(mp+sp);
            }
            for(int j=0;j<8;j+=2) NR_OPLIN(xh[m][k], j, f16vec2(nr_xl[j], nr_xl[j+1]))
#if NR_POST_REUSE
            }
#endif
#else
            // The f16 image lies where the e4m3 tiles would, two bytes an element like them,
            // so the arena's e4m3 and f16 views share one element index.
            NR_LOAD_B(xh[m][k], act_f16, tbase[m] + uint(k) * 256u, 16u);
#endif
#if NR_OOB_BRANCH
            // The tile test is wave-uniform. Flattened, it was eight
            // `v_cndmask` a fragment (plus two on xb below) on every window;
            // as a scalar branch it costs nothing where it is never taken.
#if NR_PRE_FULL & 64
            // The tile is always in range here (NR_PRE_FULL), but this
            // branch is what keeps xh a real f16: without the merge NIR folds
            // f2f32(f2f16(lifted)) away and the lift's f16 rounding (native)
            // disappears - measured, -1.37 dB on the NVIDIA-replay controls.
            // A condition the compiler cannot prove false keeps it; never taken.
            [[dont_flatten]] if(pc.tiles_x == 0u)
#else
            [[dont_flatten]] if(NR_TOOB((tok0+uint(m)*16u)/16u))
#endif
#if NR_OOB_NOFLAT
            {
                // a store behind a condition that is never true keeps this
                // a scalar branch; flattened, it was four v_cndmask a fragment.
                for (int j=0;j<NR_XH_N;++j) xh[m][k][j]=NR_F16(0.0);
                if (gl_NumWorkGroups.x == 0xffffffffu) act_e4m3[pc.o_off] = NR_E4M3(0.0);
            }
#else
                for (int j=0;j<NR_XH_N;++j) xh[m][k][j]=NR_F16(0.0);
#endif
#endif
#if !NR_OOB_BRANCH
            if(NR_TOOB((tok0+uint(m)*16u)/16u))
                for (int j=0;j<NR_XH_N;++j) xh[m][k][j]=NR_F16(0.0);
#endif
            // Quantise the pair of components that are accumulator components j, j+1 of
            // this lane (rows 2j+h, 2j+2+h) and hand them to the operand across the halves.
            for (int j=0;j<8;j+=2) {
                const fe4m3vec2 q=NR_QP_XB(f16vec2(NR_XHK(xh[m][k],j),NR_XHK(xh[m][k],j+1)));
                NR_OPPUT(xb[m][k], j, q)
            }
#else
            NR_LOAD_B(xb[m][k], act_e4m3, tbase[m] + uint(k) * 256u, 16u);
#endif
#if NR_IMAGE
#if NR_OOB_BRANCH && defined(NR_INPUT_F16)
            // xh is already zero there, and e4m3(+0) is +0.
#elif NR_OOB_BRANCH
            [[dont_flatten]] if(NR_TOOB((tok0+uint(m)*16u)/16u))
#if NR_OOB_NOFLAT
            {
                for(int j=0;j<16;++j)xb[m][k][j]=NR_E4M3(0.0);
                if (gl_NumWorkGroups.x == 0xffffffffu) act_e4m3[pc.o_off] = NR_E4M3(0.0);
            }
#else
                for(int j=0;j<16;++j)xb[m][k][j]=NR_E4M3(0.0);
#endif
#else
            if(NR_TOOB((tok0+uint(m)*16u)/16u))
                for(int j=0;j<16;++j)xb[m][k][j]=NR_E4M3(0.0);
#endif
#endif
        }
#endif
#if NR_F16_MMA
    // The same window, as f16 B operands. Every e4m3 value is exact in f16, so
    // this is a widening and not a rounding, and the products the MMA forms
    // are the products the e4m3 MMA formed. `xb` stays alive: the stage-2
    // residual reads it exactly as it did.
    NR_FRAG_B16 xb16[NR_MF][NR_CF];
    for (int m = 0; m < NR_MF; ++m)
        for (int k = 0; k < NR_CF; ++k)
            for (int j = 0; j < 8; ++j) xb16[m][k][j] = NR_F16(xb[m][k][j]);
#define NR_XB_MMA(m, k) xb16[m][k]
#else
#define NR_XB_MMA(m, k) xb[m][k]
#endif

#if NR_HEADS > 1
    // Expand and middle are fused **per group**, because the middle is block
    // diagonal: output group g reduces only hidden group g. Computing the whole
    // hidden first would hold H/16 operand fragments live - 64 of them, 128
    // VGPRs, at C=256 - where one group at a time holds (H/heads)/16, which is
    // 8 at every width.
#if NR_HWAVES
    {
        const int g = nrhw_h;           // the wave's own group, and only it
#else
    NR_FRAG_B mq[NR_MF][NR_CF];
    for (int g = 0; g < NR_HEADS; ++g) {
#endif
        NR_FRAG_B eqg[NR_MF][NR_HGF];
#if NR_HWAVES
#ifndef NR_EXPAND_GROUP
#define NR_EXPAND_GROUP 2
#endif
#if NR_EXPAND_GROUP != 2 && NR_EXPAND_GROUP != 4 && !(NR_SWI4 && NR_EXPAND_GROUP == 1)
#error "NR_EXPAND_GROUP must be 2 or 4 (1 on the int4 expansion, which has no paired weights)"
#endif
#if (NR_HGF % NR_EXPAND_GROUP) != 0
#error "NR_EXPAND_GROUP must divide the hidden-fragment count per head"
#endif
        // Four rows reuse each LDS fragment twice as much as the paired
        // form described below. Both remain at 192 VGPRs / 8 subgroups per
        // SIMD on Mesa 26.2.2, with no scratch; each MMA keeps its k order.
        // **Two weight rows a k step, so each B fragment feeds two MMAs.**
        // With one row at a time every one of this stage's 512 MMAs takes its
        // own `ds_load_b64` out of lds_x - the token split reads x out of
        // registers sixty-four times and this read it once per product, which
        // at 256 B of B fragment per ~8-cycle MMA is the WGP's whole 128 B/clk
        // of LDS. Processing h in pairs halves the loads: 4 loads and 8 MMAs a
        // k step where it was 4 and 4. The accumulators double to 8 (64 VGPRs).
        // Same k order into each accumulator, so the arithmetic is unchanged.
        // NR_EXPAND_KLOOP=[[dont_unroll]] keeps the k loop of this stage rolled. NIR
        // otherwise unrolls it at C=64 and C=128 (4 and 8 steps): the 16 live
        // accumulators plus the unrolled fragments then spill (pds128 1202 scratch
        // instructions, 286 rolled) and the code outgrows the instruction cache.
        // C=256 (16 steps) is rolled already.
#ifndef NR_EXPAND_KLOOP
#define NR_EXPAND_KLOOP
#endif
        for (int h = 0; h < NR_HGF; h += NR_EXPAND_GROUP) {
            NR_ACCF a[NR_EXPAND_GROUP][NR_MF];
#if NR_SWI4
            {
                // int4: X from lds_y4 (one 16-byte record a token tile and 64 channels), W from the record's
                // weights; then acc = float(int) * s + c per hidden row. One token tile at a time: an FP16 fragment
                // is twice the registers of an FP8 one here and these kernels already hold ~250, so the four tiles'
                // int accumulators (64 registers for a group of two) spilled thousands of instructions; the
                // weights are read again for every tile instead (L1).
                const uint xq = pc.e_off / 4u, w4 = wgt_u32[xq + 2u], sci = wgt_u32[xq + 3u];
#define NR_S4STEPS (NR_CF / 2)
#define NR_S4REC ((NR_S4STEPS + 1) / 2)
                // (scale, constant) as one f16 pair a row: half the live registers of two f32 (sci indexes u32).
                // Component c of a gfx11 accumulator is hidden row 2c + lane/16.
                uint sp_[NR_EXPAND_GROUP][8];
                for (int p = 0; p < NR_EXPAND_GROUP; ++p)
                    nr_s4q2(sp_[p], sci + uint(g * NR_HGF + h + p) * 16u + (lane / 16u));
                // A record's slot l holds row l%16, bytes 8*(l/16)..+7 of two k steps of 32 nibbles. A gfx11 lane needs
                // its row's k steps of 16 nibbles (8 bytes): slot r (first half of both steps) and slot r + 16 (the
                // second half), so the four k steps of a record are slot r .xy, slot r + 16 .xy, slot r .zw and
                // slot r + 16 .zw, for X and W alike (gemm1x1.comp).
                for (int m = 0; m < NR_MF; ++m) {
                    NR_FRAG_IACC ia[NR_EXPAND_GROUP];
                    for (int p = 0; p < NR_EXPAND_GROUP; ++p) ia[p] = NR_IACC_ZERO;
                    NR_MG(m) {
                        for (int r = 0; r < NR_S4REC; ++r) {
                            const uint b = NR_LXB_ NR_SWI4_X4 + uint(m) * uint(NR_C * 2) + uint(r) * 128u + (lane & 15u) * 4u;
                            const uvec4 xr = uvec4(lds_y4[b], lds_y4[b + 1u], lds_y4[b + 2u], lds_y4[b + 3u]);
                            const uvec4 xh = uvec4(lds_y4[b + 64u], lds_y4[b + 65u], lds_y4[b + 66u], lds_y4[b + 67u]);
                            for (int p = 0; p < NR_EXPAND_GROUP; ++p) {
                                const uint wb = (w4 + (uint(g * NR_HGF + h + p) * uint(NR_S4REC) + uint(r)) * 512u) / 4u + (lane & 15u) * 4u;
                                const uvec4 wr = uvec4(wgt_u32[wb], wgt_u32[wb + 1u], wgt_u32[wb + 2u], wgt_u32[wb + 3u]);
                                const uvec4 wh = uvec4(wgt_u32[wb + 64u], wgt_u32[wb + 65u], wgt_u32[wb + 66u], wgt_u32[wb + 67u]);
                                ia[p] = nr_s4mma(ia[p], NR_S4SLOTS0(wr, wh), NR_S4SLOTS0(xr, xh));
                                if (2 * r + 1 < NR_S4STEPS) ia[p] = nr_s4mma(ia[p], NR_S4SLOTS1(wr, wh), NR_S4SLOTS1(xr, xh));
                            }
                        }
                    }
                    for (int p = 0; p < NR_EXPAND_GROUP; ++p)
                        NR_MSK4(NR_MG(m)) for (int c = 0; c < 8; ++c) {
                            const vec2 sc_ = unpackHalf2x16(sp_[p][c]);
                            a[p][m][c] = fma(float(ia[p][c]), sc_.x, sc_.y);
                        }
                }
            }
#else
            for (int p = 0; p < NR_EXPAND_GROUP; ++p)
                for (int m = 0; m < NR_MF; ++m) a[p][m] = NR_ACCZERO;
            NR_EXPAND_KLOOP for (int k = 0; k < NR_CF; ++k) {
                NR_FRAG_B xbk[NR_MF];
                for (int m = 0; m < NR_MF; ++m)
                    NR_LOAD_B(xbk[m], lds_x, NR_LXB_
                              uint(m * NR_CF + k) * 256u + NR_OPQ(1u + uint(h)), 16u);
#if defined(NR_PACKED_EXPAND)
#if NR_ACC_F16 != 0 || NR_F16_MMA
#error "paired expansion weights require FP32 with FP8 operands"
#endif
                for(int p=0;p<NR_EXPAND_GROUP;p+=2) {
#if NR_DIAG_WFOLD
                    const uint q=pc.e_off/4u + NR_WFOLD(uint((g*NR_HGF+h+p)/2)*uint(NR_CF)
                        + uint(k))*128u + lane*4u;
#else
                    const uint q=pc.e_off/4u + uint((g*NR_HGF+h+p)/2)*uint(NR_CF)*128u
                        + uint(k)*128u + lane*4u;
#endif
                    const fe4m3vec4 w0=wexpand_pair4[q],w1=wexpand_pair4[q+1u];
                    const fe4m3vec4 w2=wexpand_pair4[q+2u],w3=wexpand_pair4[q+3u];
                    NR_OPA wf0,wf1;
                    for(int v=0;v<4;++v) {
                        wf0[v]=w0[v]; wf0[v+4]=w1[v];
                        wf1[v]=w2[v]; wf1[v+4]=w3[v];
                    }
                    for(int m=0;m<NR_MF;++m) NR_MG(m) NR_MMA(a[p][m],wf0,xbk[m]);
                    for(int m=0;m<NR_MF;++m) NR_MG(m) NR_MMA(a[p+1][m],wf1,xbk[m]);
#ifdef NR_FRAGDUMP_OFF
                    // diagnostic: real WMMA operand fragments of layer 0 for the
                    // power probe. Weights (tag 1) from window (NR_FD_X, NR_FD_Y),
                    // tokens (tag 2) from four windows below it; wave 0 only.
                    if (nr_layer == 0u && wave == 0u && nr_wx == uint(NR_FD_X) &&
                        nr_wy >= uint(NR_FD_Y) && nr_wy < uint(NR_FD_Y) + 4u) {
                        for (int t = 0; t < 2 + NR_MF; ++t) {
                            if (t < 2 && nr_wy != uint(NR_FD_Y)) continue;
                            if (t >= 2 && p != 0) continue;
                            uint slot = 0u;
                            if (lane == 0u) slot = atomicAdd(nr_prof_u[uint(NR_FRAGDUMP_OFF)], 1u);
                            slot = subgroupBroadcastFirst(slot);
                            const uint wb = uint(NR_FRAGDUMP_OFF) + 64u + slot * 72u;
                            if (lane == 0u) {
                                nr_prof_u[wb] = (t < 2 ? 1u : 2u) | (uint(h + p + t % 2) << 8u) | (uint(k) << 16u) | (uint(max(t - 2, 0)) << 24u);
                                nr_prof_u[wb + 1u] = nr_wx | (nr_wy << 16u);
                                nr_prof_u[wb + 2u] = uint(g);
                            }
                            for (int v = 0; v < 8; ++v)
                                act_e4m3[(wb + 8u) * 4u + lane * 8u + uint(v)] =
                                    t == 0 ? wf0[v] : t == 1 ? wf1[v] : xbk[max(t - 2, 0)][v];
                        }
                    }
#endif
                }
#else
                for (int p = 0; p < NR_EXPAND_GROUP; ++p) {
                    NR_OPA wf;
                    NR_LOAD_A(wf, NR_WARENA,
                              NR_TILE(pc.e_off, uint(g * NR_HGF + h + p) * 16u,
                                      uint(k) * 16u, uint(NR_C)), 16u);
                    for (int m = 0; m < NR_MF; ++m) NR_MMA(a[p][m], wf, xbk[m]);
                }
#endif
                if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                    for (int p = 0; p < NR_EXPAND_GROUP; ++p)
                        for (int m = 0; m < NR_MF; ++m) NR_RND(a[p][m])
            }
#endif
            for (int p = 0; p < NR_EXPAND_GROUP; ++p)
            for (int m = 0; m < NR_MF; ++m) NR_MG(m)
#if NR_ACTIVATION_LUT
                for(int c=0;c<8;c+=2) {
                    const fe4m3vec2 qp=nr_act_lookup(NR_N2_EQ(a[p][m][c],a[p][m][c+1]));
                    NR_OPPUT(eqg[m][h+p], c, qp)
                }
#elif NR_ACT_F32
                for (int c = 0; c < 8; c += 2) {
                    const fe4m3vec2 qp = nr_quant_pair32(vec2(
                        NR_ACTP(a[p][m][c], c), NR_ACTP_B(a[p][m][c + 1], c + 1)));
                    NR_OPPUT(eqg[m][h + p], c, qp)
                }
#elif NR_ACT_PACKED
#if NR_QBATCH_ON
                NR_QBLOCK8(eqg[m][h + p], a[p][m])
#else
                for (int c = 0; c < 8; c += 2) {
                    const f16vec2 v = nr_act2(NR_N2_EQ(a[p][m][c],
                                                       a[p][m][c + 1]));
#if NR_QUANT_PAIRED
                    fe4m3vec2 qp = nr_quant_pair(v);
                    NR_OPPUT(eqg[m][h + p], c, qp)
#else
                    eqg[m][h + p][c]     = nr_quant_e4m3(v.x);
                    eqg[m][h + p][c + 1] = nr_quant_e4m3(v.y);
#endif
                }
#endif
#else
#if NR_QUANT_PAIRED
                for (int c = 0; c < 8; c += 2) {
                    fe4m3vec2 qp = nr_quant_pair(f16vec2(
                        NR_F16(nr_act(a[p][m][c])),
                        NR_F16(nr_act(a[p][m][c + 1]))));
                    NR_OPPUT(eqg[m][h + p], c, qp)
                }
#else
                for (int c = 0; c < 8; ++c)
                    eqg[m][h + p][c] = nr_quant_e4m3(NR_F16(nr_act(a[p][m][c])));
#endif
#endif
        }
#else
        for (int h = 0; h < NR_HGF; ++h) {
            NR_ACCF a[NR_MF];
            for (int m = 0; m < NR_MF; ++m) a[m] = NR_ACCZERO;
            for (int k = 0; k < NR_CF; ++k) {
#if NR_HWAVES
                NR_FRAG_B xbk[NR_MF];
                for (int m = 0; m < NR_MF; ++m)
                    NR_LOAD_B(xbk[m], lds_x, NR_LXB_ uint(m * NR_CF + k) * 256u, 16u);
#endif
                // One weight fragment, NR_MF products: the point of widening M.
                NR_OPA wf;
                NR_LOAD_A(wf, NR_WARENA,
                          NR_TILE(pc.e_off, uint(g * NR_HGF + h) * 16u, uint(k) * 16u,
                                  uint(NR_C)), 16u);
                for (int m = 0; m < NR_MF; ++m) NR_MMA(a[m], wf, NR_XB(m, k));
                if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                    for (int m = 0; m < NR_MF; ++m) NR_RND(a[m])
            }
            for (int m = 0; m < NR_MF; ++m)
#if NR_ACT_F32
                // Same trade as the heads==1 expand: f32 throughout, one
                // conversion, a different rounding. See nr_quant_pair32.
                for (int c = 0; c < 8; c += 2) {
                    const fe4m3vec2 qp = nr_quant_pair32(vec2(
                        NR_ACTP(a[m][c], c), NR_ACTP_B(a[m][c + 1], c + 1)));
                    NR_OPPUT(eqg[m][h], c, qp)
                }
#elif NR_ACT_PACKED
#if NR_QBATCH_ON
                NR_QBLOCK8(eqg[m][h], a[m])
#else
                for (int c = 0; c < 8; c += 2) {
                    const f16vec2 v = nr_act2(NR_N2_EQ(a[m][c], a[m][c + 1]));
#if NR_QUANT_PAIRED
                    fe4m3vec2 qp = nr_quant_pair(v);
                    NR_OPPUT(eqg[m][h], c, qp)
#else
                    eqg[m][h][c]     = nr_quant_e4m3(v.x);
                    eqg[m][h][c + 1] = nr_quant_e4m3(v.y);
#endif
                }
#endif
#else
#if NR_QUANT_PAIRED
                for (int c = 0; c < 8; c += 2) {
                    fe4m3vec2 qp = nr_quant_pair(f16vec2(
                        NR_F16(nr_act(a[m][c])),
                        NR_F16(nr_act(a[m][c + 1]))));
                    NR_OPPUT(eqg[m][h], c, qp)
                }
#else
                for (int c = 0; c < 8; ++c)
                    eqg[m][h][c] = nr_quant_e4m3(NR_F16(nr_act(a[m][c])));
#endif
#endif
        }
#endif
#if NR_ACTIVATION_LUT && NR_HWAVES
        NR_BODY_BARRIER(); // Retire every head's lookup before middle outputs reuse lds_y.
#endif
#if (NR_PACKED_DENSE & 1) && NR_HWAVES
#if NR_DF != 2 || NR_ACC_F16 != 0 || NR_F16_MMA
#error "paired dense weights require the FP32 head-split path"
#endif
        NR_ACCF nr_mid_acc[NR_DF][NR_MF];
        for(int n=0;n<NR_DF;++n) for(int m=0;m<NR_MF;++m) nr_mid_acc[n][m]=NR_ACCZERO;
        for(int k=0;k<NR_HGF;++k) {
            NR_OPA wf0,wf1;
            NR_WEIGHT_PAIR(wf0,wf1,pc.mid_off,uint(g*NR_DF),uint(k),uint(NR_HGF))
            for(int m=0;m<NR_MF;++m) NR_MG(m) NR_MMA(nr_mid_acc[0][m],wf0,eqg[m][k]);
            for(int m=0;m<NR_MF;++m) NR_MG(m) NR_MMA(nr_mid_acc[1][m],wf1,eqg[m][k]);
        }
#endif
#if NR_SWI4
        // the e4m3 middle output fills all of lds_y, the int4 x copy included: every wave must be done
        // with its expansion first.
        NR_BODY_BARRIER();
#endif
        // This group's output channels are [g*hd, (g+1)*hd), which is NR_DF
        // fragments. No activation here - the host applies none between middle
        // and contract, only the quantisation any MMA operand needs.
        for (int n = 0; n < NR_DF; ++n) {
            const int nf = g * NR_DF + n;
#if NR_HWAVES
            NR_FRAG_E4M3 mqf[NR_MF];
#define NR_MQ(m, nf) mqf[m]
#else
#define NR_MQ(m, nf) mq[m][nf]
#endif
#if (NR_PACKED_DENSE & 1) && NR_HWAVES
            NR_ACCF a[NR_MF];
            for(int m=0;m<NR_MF;++m) a[m]=nr_mid_acc[n][m];
#else
            NR_ACCF a[NR_MF];
            for (int m = 0; m < NR_MF; ++m) a[m] = NR_ACCZERO;
            for (int k = 0; k < NR_HGF; ++k) {
                NR_OPA wf;
                NR_LOAD_A(wf, NR_WARENA,
                          NR_TILE(pc.mid_off, uint(nf) * 16u, uint(k) * 16u,
                                  uint(NR_HG)), 16u);
                for (int m = 0; m < NR_MF; ++m) NR_MMA(a[m], wf, eqg[m][k]);
                if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                    for (int m = 0; m < NR_MF; ++m) NR_RND(a[m])
            }
#endif
            for (int m = 0; m < NR_MF; ++m) NR_MG(m)
#if NR_QBATCH_ON
            {
#define NR_QV_M(c) f16vec2(NR_F16(a[m][c]), NR_F16(a[m][(c) + 1]))
                NR_QRUN8(NR_MQ(m, nf), NR_QV_M)
#undef NR_QV_M
            }
#elif NR_QUANT_PAIRED
                for (int c = 0; c < 8; c += 2) {
                    fe4m3vec2 qp = nr_quant_pair(f16vec2(
                        NR_F16(a[m][c]),
                        NR_F16(a[m][c + 1])));
                    NR_MQ(m, nf)[c] = qp.x;
                    NR_MQ(m, nf)[c + 1] = qp.y;
                }
#else
                for (int c = 0; c < 8; ++c) NR_MQ(m, nf)[c] = nr_quant_e4m3(NR_F16(a[m][c]));
#endif
#if NR_HWAVES
            for (int m = 0; m < NR_MF; ++m) NR_MG(m)
                NR_STORE_ACC_COL(mqf[m], lds_y, NR_LXB_ uint(m * NR_CF + nf) * 256u, 16u);
#endif
        }
    }
#if NR_HWAVES
    NR_BODY_BARRIER();                          // the middle output is now everyone's
#define NR_CT_SRC(m, k) ctk[m]
#else
#define NR_CT_SRC(m, k) mq[m][k]
#endif
#else
#if NR_STREAM_C32
#if NR_ACC_F16 != 0 || NR_F16_MMA || !NR_PTX_ACC || !NR_NATIVE_RESIDUAL || NR_ABLATE_RESID || NR_WPF
#error "streaming C32 MLP requires the default FP32 residual path"
#endif
    // Consume each hidden fragment immediately. Keep the contraction's FP32
    // accumulators instead of all eight quantized hidden fragments. Every
    // contraction still visits hidden fragments in ascending order.
    NR_ACCF stream_contract[NR_CF][NR_MF];
    for(int n=0;n<NR_CF;++n) for(int m=0;m<NR_MF;++m)
        for(int c=0;c<8;c+=2) {
#ifdef NR_INPUT_F16
            const f16vec2 x=f16vec2(NR_XHK(xh[m][n],c),NR_XHK(xh[m][n],c+1));
#else
            const f16vec2 x=NR_N2_XB(NR_OPK(xb[m][n],c),NR_OPK(xb[m][n],c+1));
#endif
            const uint off=pc.rs_off+uint(n)*16u+NR_ROW(c);
#if NR_RESIDUAL_F32
            const vec2 residual=vec2(x)*vec2(wgt_f32[off],wgt_f32[off+2u]);
#else
            const f16vec2 residual=x*nr_residual_scale(off);
#endif
            stream_contract[n][m][c]=float(residual.x);
            stream_contract[n][m][c+1]=float(residual.y);
        }
    NR_OPB stream_eq[NR_MF];
#define NR_EQ(m,h) stream_eq[m]
#else
    NR_OPB eq[NR_MF][NR_HF];
#define NR_EQ(m,h) eq[m][h]
#endif
#if NR_WPF
    // Flat over (h, k): NR_HF * NR_CF weight tiles of the expand.
#define NR_WPF_E_TOT (NR_HF * NR_CF)
#define NR_WPF_E_ADDR(i) NR_TILE(pc.e_off, uint((i) / NR_CF) * 16u,           \
                                 uint((i) % NR_CF) * 16u, uint(NR_C))
    NR_OPA wpe[NR_WPF_SLOTS];
    NR_WPF_PRIME(wpe, NR_WPF_E_TOT, NR_WPF_E_ADDR)
#endif
    // NR_EXPAND_HLOOP keeps the loop over the hidden fragments rolled. NIR left it rolled on
    // its own until NR_OP_KSWAP made the operand builds smaller; unrolled, the C=32 kernels
    // go from 216 to 240 VGPRs, one wave a SIMD fewer, and the smaller exchange is lost.
#ifndef NR_EXPAND_HLOOP
#if NR_STREAM_C32 && NR_OP_KSWAP
#define NR_EXPAND_HLOOP [[dont_unroll]]
#else
#define NR_EXPAND_HLOOP
#endif
#endif
    NR_EXPAND_HLOOP for (int h = 0; h < NR_HF; ++h) {
        NR_ACCF a[NR_MF];
        for (int m = 0; m < NR_MF; ++m) a[m] = NR_ACCZERO;
        for (int k = 0; k < NR_CF; ++k) {
#if NR_WPF
            NR_WPF_STEP(wpe, h * NR_CF + k, NR_WPF_E_TOT, NR_WPF_E_ADDR)
#define NR_WPF_WF_E NR_WPF_AT(wpe, h * NR_CF + k)
#else
            NR_OPA wf;
            NR_C32_WEIGHT(wf,NR_WB_E,uint(h),uint(k),uint(NR_CF));
#define NR_WPF_WF_E wf
#endif
            for (int m = 0; m < NR_MF; ++m) NR_MG(m) NR_MMA(a[m], NR_WPF_WF_E, NR_XB_MMA(m, k));
            if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                for (int m = 0; m < NR_MF; ++m) NR_RND(a[m])
        }
        for (int m = 0; m < NR_MF; ++m) NR_MG(m)
#if NR_ACTIVATION_LUT && NR_C == 32
            for(int c=0;c<8;c+=2) {
                const fe4m3vec2 qp=nr_act_lookup(NR_N2_EQ(a[m][c],a[m][c+1]));
                NR_OPPUT(NR_EQ(m,h), c, qp)
            }
#elif NR_ACT_F32
            // The accumulator is f32 and the converter takes f32, so the half
            // hop in between buys nothing but two conversions each way. This
            // evaluates the same polynomial in f32 and goes straight to e4m3.
            // It is NOT the same function: NVIDIA's activation is a packed half
            // FMA chain and rounds like one, so this changes the image. See
            // nr_quant_pair32 for why the hop costs what it does.
            for (int c = 0; c < 8; c += 2) {
                const fe4m3vec2 qp = nr_quant_pair32(vec2(
                    NR_ACTP(a[m][c], c), NR_ACTP_B(a[m][c + 1], c + 1)));
                NR_OPPUT(NR_EQ(m,h), c, qp)
            }
#elif NR_ACT_PACKED
#if NR_QBATCH_ON
            NR_QBLOCK8(NR_EQ(m,h), a[m])
#else
            for (int c = 0; c < 8; c += 2) {
                const f16vec2 v = nr_act2(NR_N2_EQ(a[m][c], a[m][c + 1]));
#if NR_QUANT_PAIRED
                NR_QPAIR_T qp = NR_QP_EQ(v);
                NR_OPPUT(NR_EQ(m,h), c, qp)
#else
                NR_EQ(m,h)[c]     = nr_quant_e4m3(v.x);
                NR_EQ(m,h)[c + 1] = nr_quant_e4m3(v.y);
#endif
            }
#endif
#else
#if NR_QUANT_PAIRED
            for (int c = 0; c < 8; c += 2) {
                fe4m3vec2 qp = nr_quant_pair(f16vec2(
                    NR_F16(nr_act(a[m][c])),
                    NR_F16(nr_act(a[m][c + 1]))));
                NR_OPPUT(NR_EQ(m,h), c, qp)
            }
#else
            for (int c = 0; c < 8; ++c)
                NR_EQ(m,h)[c] = nr_quant_e4m3(NR_F16(nr_act(a[m][c])));
#endif
#endif
#if NR_STREAM_C32
        for(int n=0;n<NR_CF;++n) {
            NR_OPA cw;
            NR_C32_WEIGHT(cw,NR_WB_CT,uint(n),uint(h),uint(NR_HF));
            for(int m=0;m<NR_MF;++m) NR_MMA(stream_contract[n][m],cw,stream_eq[m]);
        }
#endif
    }
#define NR_CT_SRC(m, k) NR_EQ(m,k)
#endif

#if NR_ACTIVATION_LUT && NR_C == 32
    NR_BODY_BARRIER(); // All lookup readers retire before K/V overwrite the table.
#endif
    // ---- stage 2: y = Ct . q(e or m) + rs * x -----------------------------
#if NR_HWAVES
    // **No `yq` array at all.** The rows this wave owns go into `lds_x` at the
    // end of stage 2 and the output projection reloads them from there, so the
    // only copy alive is `yqw`, born and dead inside one iteration. (Holding
    // even the four fragments of this wave's rows - indexed by the *local* row
    // `nn`, because a coopmat array indexed by anything the compiler cannot
    // fold goes to scratch - was 16 VGPRs live from stage 2 to stage 6, and
    // those were the 15 ACO spilled.)
#define NR_YN nn
#define NR_QKVA(m, r) qkv[m][r]
#else
    NR_OPB        yq[NR_MF][NR_CF];     // the quantised value, for QKV
#if NR_V_SWAP
    NR_OPA        yqa[NR_MF][NR_CF];    // The same bytes as V's A operand
#endif
#define NR_YN n
#define NR_QKV_SRC(m, k) yq[m][k]
#define NR_QKVA(m, r) qkv[r]
#endif
#if NR_HWAVES
// yqw is an Accumulator-typed fragment that is only stored (to lds_x) and read back as a
// B operand there, so it is written in accumulator order, component for component.
#define NR_YQ(m, n) yqw[m]
#define NR_YQPUT(m, n, c, q) { NR_YQ(m, n)[c] = (q).x; NR_YQ(m, n)[(c) + 1] = (q).y; }
#else
#define NR_YQ(m, n) yq[m][NR_YN]
#define NR_YQPUT(m, n, c, q) NR_OPPUT(NR_YQ(m, n), c, q)
#endif
#if NR_HEADS > 1
    // At heads > 1 the MLP residual is requantised, so the value the attention
    // skip adds back *is* the quantised one - there is nothing wide to keep.
    // Holding it separately costs 64 VGPRs of long-lived state at C=256, which
    // is where the widest level was losing its occupancy.
#if NR_HWAVES
    // Reloaded from `lds_x` in the output projection, which is the only stage
    // that reads it; see `yqr` there.
#define NR_Y(m, n, c) float(NR_OPK(yqr[m], c))
#else
#define NR_Y(m, n, c) float(NR_OPK_R(NR_YQ(m, n), c))
#endif
#else
    NR_FRAG_ACC16 yh[NR_MF][NR_CF];     // ... and the wide one, for the skip
#define NR_Y(m, n, c) float(yh[m][n][c])
#endif
#if NR_WPF
    // Flat over (n, k): NR_CF * NR_KCF weight tiles of the contract.
#define NR_WPF_CT_TOT (NR_CF * NR_KCF)
#define NR_WPF_CT_ADDR(i) NR_TILE(pc.ct_off, uint((i) / NR_KCF) * 16u,        \
                                  uint((i) % NR_KCF) * 16u, uint(NR_KCF * 16))
    NR_OPA wpct[NR_WPF_SLOTS];
    NR_WPF_PRIME(wpct, NR_WPF_CT_TOT, NR_WPF_CT_ADDR)
#endif
#if (NR_PACKED_DENSE & 2) && NR_HWAVES
#if NR_DF != 2 || NR_ACC_F16 != 0 || !NR_NATIVE_RESIDUAL || !NR_PTX_ACC || NR_WPF || NR_ABLATE_RESID
#error "paired dense weights require the default FP32 head-split path"
#endif
    NR_ACCF nr_contract_acc[NR_DF][NR_MF];
    for(int p=0;p<NR_DF;++p) {
        const int n=nrhw_h*NR_DF+p;
        for(int m=0;m<NR_MF;++m) NR_MG(m) {
            NR_FRAG_B yr;
            NR_LOAD_B(yr,lds_x, NR_LXB_ uint(m*NR_CF+n)*256u+NR_OPQ(80u),16u);
            for(int c=0;c<8;c+=2) {
                const uint off=pc.rs_off+uint(n)*16u+NR_ROW(c);
#if NR_RESIDUAL_F32
                const vec2 residual=vec2(yr[c],yr[c+1])*vec2(wgt_f32[off],wgt_f32[off+2u]);
#else
                const f16vec2 scale=nr_residual_scale(off);
                const f16vec2 residual=f16vec2(yr[c],yr[c+1])*scale;
#endif
                nr_contract_acc[p][m][c]=float(residual.x);
                nr_contract_acc[p][m][c+1]=float(residual.y);
            }
        }
    }
    for(int k=0;k<NR_KCF;++k) {
        NR_FRAG_B ctx[NR_MF];
        for(int m=0;m<NR_MF;++m)
            NR_LOAD_B(ctx[m],lds_y, NR_LXB_ uint(m*NR_CF+k)*256u+NR_OPQ(64u),16u);
        NR_OPA wf0,wf1;
        NR_WEIGHT_PAIR(wf0,wf1,pc.ct_off,uint(nrhw_h*NR_DF),uint(k),uint(NR_KCF))
        for(int m=0;m<NR_MF;++m) NR_MG(m) NR_MMA(nr_contract_acc[0][m],wf0,ctx[m]);
        for(int m=0;m<NR_MF;++m) NR_MG(m) NR_MMA(nr_contract_acc[1][m],wf1,ctx[m]);
    }
#endif
#if NR_SWQ4
    // NR_SWQ4: every head is done reading the
    // middle output in lds_y before the int4 y copy overwrites [32C, 64C).
    NR_BODY_BARRIER();
#endif
#if NR_HWAVES
    for (int nn = 0; nn < NR_DF; ++nn) {
        const int n = nrhw_h * NR_DF + nn;
        // x for this row, from the exchange buffer: the same fragment, and the
        // same components, the register form indexed as xb[m][n].
        NR_FRAG_B xbk[NR_MF];
        for (int m = 0; m < NR_MF; ++m)
            NR_LOAD_B(xbk[m], lds_x, NR_LXB_
                      uint(m * NR_CF + n) * 256u + NR_OPQ(64u), 16u);
        // This iteration's y only. It is written into `lds_x` below and read
        // back by the output projection; holding it in registers from here to
        // there was the live range ACO spilled.
        NR_FRAG_E4M3 yqw[NR_MF];
#else
    for (int n = 0; n < NR_CF; ++n) {
#endif
#if NR_STREAM_C32
        NR_ACCF a[NR_MF];
        for(int m=0;m<NR_MF;++m) a[m]=stream_contract[n][m];
#elif (NR_PACKED_DENSE & 2) && NR_HWAVES
        NR_ACCF a[NR_MF];
        for(int m=0;m<NR_MF;++m) a[m]=nr_contract_acc[nn][m];
#else
        NR_ACCF a[NR_MF];
        for (int m = 0; m < NR_MF; ++m) a[m] = NR_ACCZERO;
#if NR_PTX_ACC
        // PTX uses the rounded scaled residual as the MMA C operand.
#if NR_NATIVE_RESIDUAL
        for (int m=0;m<NR_MF;++m) for(int c=0;c<8;c+=2) {
#if NR_RESIDUAL_F32
#ifdef NR_INPUT_F16
            const vec2 x=vec2(NR_XHK(xh[m][n],c),NR_XHK(xh[m][n],c+1));
#else
            const vec2 x=vec2(NR_OPK(NR_XB(m,n),c),NR_OPK(NR_XB(m,n),c+1));
#endif
            const uint off=pc.rs_off+uint(n)*16u+NR_ROW(c);
            const vec2 residual=x*vec2(wgt_f32[off],wgt_f32[off+2u]);
#else
#ifdef NR_INPUT_F16
            const f16vec2 x=f16vec2(NR_XHK(xh[m][n],c),NR_XHK(xh[m][n],c+1));
#else
            const f16vec2 x=NR_N2_XB(NR_OPK(NR_XB(m,n),c),NR_OPK(NR_XB(m,n),c+1));
#endif
            const uint off=pc.rs_off+uint(n)*16u+NR_ROW(c);
#if NR_ABLATE_RESID
            const f16vec2 scale=f16vec2(0.0hf);
#else
            const f16vec2 scale=nr_residual_scale(off);
#endif
            const f16vec2 residual=x*scale;
#endif
#if NR_F16_MMA
            a[m][c]=residual.x;a[m][c+1]=residual.y;
#else
            a[m][c]=float(residual.x);a[m][c+1]=float(residual.y);
#endif
        }
#else
        for (int m=0;m<NR_MF;++m) for(int c=0;c<8;++c) {
#ifdef NR_INPUT_F16
            const float x=float(NR_XHK(xh[m][n],c));
#else
            const float x=float(NR_OPK(NR_XB(m,n),c));
#endif
            a[m][c]=nr_round_f16(x*wgt_f32[pc.rs_off+uint(n)*16u+NR_ROW(c)]);
        }
#endif
#endif
        for (int k = 0; k < NR_KCF; ++k) {
#if NR_HWAVES
            NR_FRAG_B ctk[NR_MF];
            for (int m = 0; m < NR_MF; ++m)
                NR_LOAD_B(ctk[m], lds_y, NR_LXB_
                          uint(m * NR_CF + k) * 256u + NR_OPQ(1u + uint(nn)), 16u);
#endif
#if NR_WPF
            NR_WPF_STEP(wpct, n * NR_KCF + k, NR_WPF_CT_TOT, NR_WPF_CT_ADDR)
#define NR_WPF_WF_CT NR_WPF_AT(wpct, n * NR_KCF + k)
#else
            NR_OPA wf;
            NR_C32_WEIGHT(wf,NR_WB_CT,uint(n),uint(k),uint(NR_KCF));
#define NR_WPF_WF_CT wf
#endif
            for (int m = 0; m < NR_MF; ++m) NR_MG(m) NR_MMA(a[m], NR_WPF_WF_CT, NR_CT_SRC(m, k));
            if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                for (int m = 0; m < NR_MF; ++m) NR_RND(a[m])
        }
#endif
        // The residual is element-wise: `rs` is indexed by the accumulator's
        // row, which is its component, and `x` is already in registers with the
        // accumulator's own map. At heads > 1 the sum is requantised, and then
        // the *quantised* value is what the attention skip adds back.
#if NR_QUANT_PAIRED
        for (int m = 0; m < NR_MF; ++m) NR_MG(m) {
            NR_F16 requantized[8];
#else
        for (int m = 0; m < NR_MF; ++m)
#endif
#if ((NR_PKN) & NR_PKN_YQ) && NR_PTX_ACC && NR_QUANT_PAIRED
            // **The one site in this shader whose narrowing was never paired.**
            // `v` is the accumulator component itself at NR_PTX_ACC, so the
            // eight `NR_F16(v)` are eight `v_cvt_f16_f32` per (m, n) - 64 a
            // window at C=32 - and the pairing below then costs nothing
            // because `requantized` is already f16. Doing the narrowing two
            // components at a time is 32 instructions instead of 64, and the
            // pair is exactly the one the quantiser wants.
            for (int c = 0; c < 8; c += 2) {
                const f16vec2 pr = NR_RTZ2(a[m][c], a[m][c + 1]);
                requantized[c] = pr.x; requantized[c + 1] = pr.y;
#if NR_HEADS == 1
                yh[m][n][c] = pr.x; yh[m][n][c + 1] = pr.y;
#endif
            }
#else
            for (int c = 0; c < 8; ++c) {
#if NR_PTX_ACC
                const NR_ACC_SCALAR v = a[m][c];
#elif defined(NR_INPUT_F16)
                // The adapter retains its FP16 lift/blend for the residual.
                // PTX mul.f16x2 rounds before add.f16x2; only the MMA input is FP8.
                const NR_F16 residual = NR_F16(NR_XHK(xh[m][n],c) *
                    NR_F16(wgt_f32[pc.rs_off + uint(n)*16u + NR_ROW(c)]));
                const float v = float(NR_F16(NR_F16(a[m][c]) + residual));
#else
                const float v = a[m][c]
                    + wgt_f32[pc.rs_off + uint(n) * 16u + NR_ROW(c)]
                      * float(NR_OPK(NR_XB(m,n),c));
#endif
#if NR_QUANT_PAIRED
                requantized[c] = NR_F16(v);
#else
                NR_YQ(m, n)[c] = nr_quant_e4m3(NR_F16(v));
#endif
#if NR_HEADS == 1
                yh[m][n][c] = NR_F16(v);
#endif
            }
#endif
#if NR_QUANT_PAIRED
            for (int c=0;c<8;c+=2) {
                NR_QPAIR_T q=NR_QP_YQ(f16vec2(requantized[c],requantized[c+1]));
                NR_YQPUT(m, n, c, q)
#if NR_V_SWAP && !NR_HWAVES
                NR_OPPUT_X(yqa[m][NR_YN], c, q)   // an A operand against a weight tile read as B
#endif
            }
        }
#else
#endif
#if NR_HWAVES
        // Back into lds_x, NR_LXB_ in place: this wave is the only writer of rows n,
        // and every wave finished reading x before the barrier above.
        for (int m = 0; m < NR_MF; ++m) NR_MG(m) {
            NR_STORE_ACC_COL(NR_YQ(m, n), lds_x, NR_LXB_ uint(m * NR_CF + n) * 256u, 16u);
        }
#endif
    }
#if NR_HWAVES
    NR_BODY_BARRIER();                          // y is now everyone's; lds_y is free
#if NR_SWQ4
    // NR_SWQ4: the int4 copy of y, for the Q, K and V products. It is built here, once every wave is out of the loop
    // above, and not inside it: that loop's contraction still reads the middle output out of lds_y, and the copy
    // lives in lds_y. Fragment n = this wave's 16 channels of token tile m are read back from lds_x, where the loop
    // stored them: a gfx11 B fragment has all 16 channels of the lane's token in both lane halves, so half hh
    // packs channels 8hh..8hh+7 (one u32), the word the NR_I4_PAIR record keeps at (token l%16, half nn4 of the
    // step), word 2(h%2) + hh: record h/2 with h = n/2.
    {
        const uint hh4 = lane / 16u;
        for (int nn = 0; nn < NR_DF; ++nn) {
            const uint r4 = uint(nrhw_h * NR_DF + nn);
            const uint st4 = r4 / 2u, nn4 = r4 % 2u;
            uint yq4[8]; nr_s4q(yq4, wgt_u32[NR_SWI4_REC + 10u] + r4 * 16u + 8u * hh4);
            for (int m = 0; m < NR_MF; ++m) NR_MSK4(NR_MGQ(m)) {
                NR_FRAG_B yf;
                NR_LOAD_B(yf, lds_x, NR_LXB_ (uint(m) * uint(NR_CF) + r4) * 256u, 16u);
                float v_[8];
                for (int c = 0; c < 8; ++c) v_[c] = hh4 == 0u ? float(yf[c]) : float(yf[8 + c]);
                lds_y4[NR_SWI4_X4 + uint(m) * uint(2 * NR_C) + (st4 / 2u) * 128u
                       + 4u * ((lane % 16u) + 16u * nn4) + 2u * (st4 % 2u) + hh4] = nr_s4pk(v_, yq4);
            }
        }
        NR_BODY_BARRIER();                      // every wave's piece of the copy is in before any product reads it
    }
#endif
#endif

#if NR_DUMP == 1
    // The post-MLP-residual value, so a wrong MLP and a wrong attention cannot
    // be confused. Stored ColumnMajor: the array is [token][channel].
    for (int m = 0; m < NR_MF; ++m) for (int n = 0; n < NR_CF; ++n) {
        NR_FRAG_E4M3 d;
#if NR_QUANT_PAIRED
        for (int c = 0; c < 8; c += 2) {
            fe4m3vec2 qp = nr_quant_pair(f16vec2(
                NR_F16(NR_Y(m, n, c)),
                NR_F16(NR_Y(m, n, (c + 1)))));
            d[c] = qp.x;
            d[c + 1] = qp.y;
        }
#else
        for (int c = 0; c < 8; ++c) d[c] = nr_quant_e4m3(NR_F16(NR_Y(m, n, c)));
#endif
        NR_STORE_ACC_COL(d, act_e4m3, pc.o_off + wbase
                         + (tok0 + uint(m) * 16u) * uint(NR_C) + uint(n) * 16u, uint(NR_C));
    }
    return;
#endif

    // ---- stages 3 to 5: QKV, attention and context, one head at a time ---
    // The QKV rows are grouped **per head**, [Q|K|V] each, not [all Q][all K]
    // [all V]: the host reduces from `g * 3 * hd`. The two orders coincide at
    // one head, which is why C=32 could not tell them apart and C=64 scored
    // 42.5% until this was found.
#if NR_HWAVES
    {
        const int hh = nrhw_h;
#else
    NR_OPB cq[NR_MF][NR_CF];
    for (int hh = 0; hh < NR_HEADS; ++hh) {
#endif
#if NR_K_REGS
        NR_QK_OPA kreg[NR_MF][NR_DF];
#endif
#if NR_V_REGS
        NR_PV_OPA vreg[NR_DF][NR_MF];
#endif
        const NR_F16 hscale = NR_F16(wgt_f32[pc.s_off + uint(hh)]);
        NR_QK_OPB qb[NR_MF][NR_DF];
#if NR_HWAVES
#if !NR_PACKED_SWIN_MATH || !NR_QUANT_PAIRED || NR_ABLATE_NORM || NR_NORM_LATE_SHUFFLE
#error "NR_HWAVES's QKV implements the shipping paired-quant path only; NR_ABLATE_NORM and NR_NORM_LATE_SHUFFLE are diagnostics it does not carry"
#endif
        // **Three passes over k, eight accumulators each, where one pass held
        // twenty-four.** `qkv[NR_MF][3*NR_DF]` is 24 accumulators - 192 VGPRs
        // live across the whole projection, and with `qb`, `kreg` and the norm
        // temporaries on top of it that was the register wall the mode hit.
        // Q, K and V are independent reductions over the same k, so running
        // them one after another holds 8 accumulators (64 VGPRs) plus the 16
        // of `qb` and 16 of `kreg` that survive each pass.
        //
        // Every pass keeps the k-outer / m-inner nest for the same reason the
        // fused one did: the weight address does not depend on m, so with the
        // token blocks outermost NIR hoists all the loop-invariant weight
        // loads out of the unrolled m loop.
        //
        // **The arithmetic is untouched**: the same MMAs accumulate in the same
        // k order into the same zeroed accumulator, and the norm below is the
        // NR_NORM_F32 reduction's own (c, d) loop with q and k separated. That
        // separation is exact per component - the cross-lane exchange adds
        // `NR_F16(sum)` to the partner's `NR_F16(sum)` componentwise, so two
        // one-component shuffles give each component the value the packed pair
        // gave it. The cost is the reload: `lds_x` is read three times.
        {
#ifndef NR_QK_TOGETHER
#define NR_QK_TOGETHER 0
#endif
#if NR_QK_TOGETHER != 0 && NR_QK_TOGETHER != 1
#error "NR_QK_TOGETHER must be 0 or 1"
#endif
            // Q and K share the first traversal; V remains separate.
            // Sixteen accumulator fragments fit without the register spill
            // caused by keeping all twenty-four Q/K/V fragments live.
#define NR_QK_ROWS ((1 + NR_QK_TOGETHER) * NR_DF)
            NR_ACCF acc[NR_MF][NR_QK_ROWS];
            // ---- pass Q: rows [hh*3*NR_DF, +NR_DF) ----
#if NR_SWQ4
            // Q and K on int4 from the y copy (lds_y4 at 32C): A = the head's QKV rows (int4 records,
            // record word 11), B = y; acc = float(int) * s + c per row (f16 pairs, word 12), zero for skipped
            // tiles (no affine term on a tile whose MMAs did not run).
            {
                const uint xq = NR_SWI4_REC, qw4 = wgt_u32[xq + 11u], qsc = wgt_u32[xq + 12u];
                NR_FRAG_IACC iq[NR_MF][NR_QK_ROWS];
                for (int m = 0; m < NR_MF; ++m) for (int d = 0; d < NR_QK_ROWS; ++d) iq[m][d] = NR_IACC_ZERO;
                // two record slots a lane, two fragments a record (see the expansion)
                for (int r_ = 0; r_ < NR_Q4REC; ++r_) {
                    uvec4 wr[NR_QK_ROWS], wh[NR_QK_ROWS];
                    for (int d = 0; d < NR_QK_ROWS; ++d) {
                        const uint wb = (qw4 + (uint(hh * 3 * NR_DF + d) * uint(NR_Q4REC) + uint(r_)) * 512u) / 4u + (lane & 15u) * 4u;
                        wr[d] = uvec4(wgt_u32[wb], wgt_u32[wb + 1u], wgt_u32[wb + 2u], wgt_u32[wb + 3u]);
                        wh[d] = uvec4(wgt_u32[wb + 64u], wgt_u32[wb + 65u], wgt_u32[wb + 66u], wgt_u32[wb + 67u]);
                    }
                    for (int m = 0; m < NR_MF; ++m) NR_MGQ(m) {
                        const uint b = NR_SWI4_X4 + uint(m) * uint(2 * NR_C) + uint(r_) * 128u + (lane & 15u) * 4u;
                        const uvec4 xr = uvec4(lds_y4[b], lds_y4[b + 1u], lds_y4[b + 2u], lds_y4[b + 3u]);
                        const uvec4 xh = uvec4(lds_y4[b + 64u], lds_y4[b + 65u], lds_y4[b + 66u], lds_y4[b + 67u]);
                        for (int d = 0; d < NR_QK_ROWS; ++d) {
                            iq[m][d] = nr_s4mma(iq[m][d], NR_S4SLOTS0(wr[d], wh[d]), NR_S4SLOTS0(xr, xh));
                            if (2 * r_ + 1 < NR_Q4STEPS) iq[m][d] = nr_s4mma(iq[m][d], NR_S4SLOTS1(wr[d], wh[d]), NR_S4SLOTS1(xr, xh));
                        }
                    }
                }
                for (int d = 0; d < NR_QK_ROWS; ++d) {
                    uint sp_[8]; nr_s4q2(sp_, qsc + uint(hh * 3 * NR_DF + d) * 16u + (lane / 16u));
                    for (int m = 0; m < NR_MF; ++m) {
                        acc[m][d] = NR_ACCZERO;
                        NR_MGQ(m) for (int c = 0; c < 8; ++c) { const vec2 sc_ = unpackHalf2x16(sp_[c]); acc[m][d][c] = fma(float(iq[m][d][c]), sc_.x, sc_.y); }
                    }
                }
            }
#else
            for (int m = 0; m < NR_MF; ++m)
                for (int d = 0; d < NR_QK_ROWS; ++d) acc[m][d] = NR_ACCZERO;
            for (int k = 0; k < NR_CF; ++k) {
                NR_FRAG_B yk[NR_MF];
                for (int m = 0; m < NR_MF; ++m)
                    NR_LOAD_B(yk[m], lds_x, NR_LXB_
                              uint(m * NR_CF + k) * 256u + NR_OPQ(128u), 16u);
#if defined(NR_PACKED_QKV)
#if NR_ACC_F16 != 0 || NR_QK_TOGETHER != 1 || NR_DF != 2
#error "paired QKV weights require the FP32 Q/K-together head-split path"
#endif
                for(int r=0;r<NR_QK_ROWS;r+=2) {
                    NR_OPA wf0,wf1;
                    NR_WEIGHT_PAIR(wf0,wf1,pc.qkv_off,uint(hh*3*NR_DF+0+r),uint(k),uint(NR_CF))
                    for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_MMA(acc[m][r],wf0,yk[m]);
                    for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_MMA(acc[m][r+1],wf1,yk[m]);
                }
#else
                for (int r = 0; r < NR_QK_ROWS; ++r) {
                    NR_OPA wf;
                    NR_C32_WEIGHT(wf,NR_WB_QKV,uint(hh*3*NR_DF+r),uint(k),uint(NR_CF));
                    for (int m = 0; m < NR_MF; ++m) NR_MMA(acc[m][r], wf, yk[m]);
                }
#endif
                if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                    for (int m = 0; m < NR_MF; ++m)
                        for (int r = 0; r < NR_QK_ROWS; ++r) NR_RND(acc[m][r])
            }
#endif
            for (int m = 0; m < NR_MF; ++m) NR_MGA(m) {
#if NR_NORM_F32
                float sqp[4];
                for (int i = 0; i < 4; ++i) sqp[i] = 0.0;
                for (int c = 0; c < 8; ++c)
                    for (int d = 0; d < NR_DF; ++d) {
                        const float q = acc[m][d][c];
                        sqp[c & 3] = fma(q, q, sqp[c & 3]);
                    }
                // The same narrowing and the same single exchange, on a pair
                // whose second component is unused: `packFloat2x16` moves 32
                // bits either way and the add is componentwise.
#if NR_QK_SCALE_FAST == 3
                float sq = (sqp[0]+sqp[1])+(sqp[2]+sqp[3]);
                sq += subgroupShuffleXor(sq,16u);
                const float nq = inversesqrt(max(sq,0.000062));
#else
                f16vec2 sq = f16vec2(vec2((sqp[0] + sqp[1]) + (sqp[2] + sqp[3]), 0.0));
                sq = sq + unpackFloat2x16(subgroupShuffleXor(packFloat2x16(sq), 16u));
                const NR_F16 nq =
                    nr_norm_rsq(sq.x);
#endif
#else
                // **The shipping f16 reduction, the q half of it.** The packed
                // form below the `#else` of NR_PACKED_SWIN_MATH carries q and
                // k in one `f16vec2` because both accumulators are live there;
                // in the three-pass split they are not. A packed f16 op rounds
                // its halves independently, so the pair is two independent
                // scalar-f16 chains and taking one of them here is exact: the
                // same square, the same d order, the same eight partial sums,
                // the same 4/2/1 tree.
                NR_F16 sq[8];
                for (int c = 0; c < 8; ++c) {
                    sq[c] = NR_F16(0.0);
                    for (int d = 0; d < NR_DF; ++d) {
                        // Preserve the packed path's contraction permission -
                        // its comment says `precise` here changes the frame.
                        const NR_F16 q = NR_F16(acc[m][d][c]);
                        sq[c] = NR_F16(sq[c] + NR_F16(q * q));
                    }
                }
                // **Four exchanges, not eight.** The packed form's shuffle
                // carried one c of q and the same c of k; here two c's of the
                // *same* chain share the 32 bits. The add after the shuffle is
                // still componentwise f16, so each c gets exactly the partner
                // lane's own sum for that c - the same value, half the moves.
                for (int c = 0; c < 8; c += 2) {
                    const f16vec2 o = unpackFloat2x16(subgroupShuffleXor(
                        packFloat2x16(f16vec2(sq[c], sq[c + 1])), 16u));
                    sq[c] = NR_F16(sq[c] + o.x);
                    sq[c + 1] = NR_F16(sq[c + 1] + o.y);
                }
                for (int stride = 4; stride > 0; stride /= 2)
                    for (int c = 0; c < stride; ++c)
                        sq[c] = NR_F16(sq[c] + sq[c + stride]);
                const NR_F16 nq =
                    nr_norm_rsq(sq[0]);
#endif
                for (int d = 0; d < NR_DF; ++d)
                    for (int c = 0; c < 8; c += 2) {
#if NR_QK_SCALE_FAST
                        fe4m3vec2 qp = nr_qscale_fast(vec2(acc[m][d][c],acc[m][d][c+1]),nq,hscale);
#else
                        f16vec2 qv = NR_N2_Q(acc[m][d][c], acc[m][d][c + 1]);
                        qv = (qv * f16vec2(nq)) * f16vec2(hscale);
                        fe4m3vec2 qp = nr_quant_pair(qv);
#endif
                        NR_OPPUT(qb[m][d], c, qp)
                    }
            }
#if !NR_QK_TOGETHER
            // ---- pass K: rows [hh*3*NR_DF + NR_DF, +NR_DF) ----
            for (int m = 0; m < NR_MF; ++m)
                for (int d = 0; d < NR_DF; ++d) acc[m][d] = NR_ACCZERO;
            for (int k = 0; k < NR_CF; ++k) {
                NR_FRAG_B yk[NR_MF];
                for (int m = 0; m < NR_MF; ++m)
                    NR_LOAD_B(yk[m], lds_x, NR_LXB_
                              uint(m * NR_CF + k) * 256u + NR_OPQ(129u), 16u);
                for (int r = 0; r < NR_DF; ++r) {
                    NR_OPA wf;
                    NR_LOAD_A(wf, NR_WARENA,
                              NR_TILE(pc.qkv_off,
                                      uint(hh * 3 * NR_DF + NR_DF + r) * 16u,
                                      uint(k) * 16u, uint(NR_C)), 16u);
                    for (int m = 0; m < NR_MF; ++m) NR_MMA(acc[m][r], wf, yk[m]);
                }
                if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                    for (int m = 0; m < NR_MF; ++m)
                        for (int r = 0; r < NR_DF; ++r) NR_RND(acc[m][r])
            }
#endif
            for (int m = 0; m < NR_MF; ++m) NR_MGK(m) {
#if NR_NORM_F32
                float skp[4];
                for (int i = 0; i < 4; ++i) skp[i] = 0.0;
                for (int c = 0; c < 8; ++c)
                    for (int d = 0; d < NR_DF; ++d) {
                        const float kk = acc[m][d + NR_QK_TOGETHER * NR_DF][c];
                        skp[c & 3] = fma(kk, kk, skp[c & 3]);
                    }
#if NR_QK_SCALE_FAST == 3
                float sk = (skp[0]+skp[1])+(skp[2]+skp[3]);
                sk += subgroupShuffleXor(sk,16u);
                const float nk = inversesqrt(max(sk,0.000062));
#else
                f16vec2 sk = f16vec2(vec2((skp[0] + skp[1]) + (skp[2] + skp[3]), 0.0));
                sk = sk + unpackFloat2x16(subgroupShuffleXor(packFloat2x16(sk), 16u));
                const NR_F16 nk =
                    nr_norm_rsq(sk.x);
#endif
#else
                // The k half of the same reduction; see the q pass above.
                NR_F16 sk[8];
                for (int c = 0; c < 8; ++c) {
                    sk[c] = NR_F16(0.0);
                    for (int d = 0; d < NR_DF; ++d) {
                        const NR_F16 kk = NR_F16(acc[m][d + NR_QK_TOGETHER * NR_DF][c]);
                        sk[c] = NR_F16(sk[c] + NR_F16(kk * kk));
                    }
                }
                for (int c = 0; c < 8; c += 2) {
                    const f16vec2 o = unpackFloat2x16(subgroupShuffleXor(
                        packFloat2x16(f16vec2(sk[c], sk[c + 1])), 16u));
                    sk[c] = NR_F16(sk[c] + o.x);
                    sk[c + 1] = NR_F16(sk[c + 1] + o.y);
                }
                for (int stride = 4; stride > 0; stride /= 2)
                    for (int c = 0; c < stride; ++c)
                        sk[c] = NR_F16(sk[c] + sk[c + stride]);
                const NR_F16 nk =
                    nr_norm_rsq(sk[0]);
#endif
                for (int d = 0; d < NR_DF; ++d) {
                    for (int c = 0; c < 8; c += 2) {
#if NR_QK_SCALE_FAST
                        fe4m3vec2 qp = nr_kscale_fast(vec2(acc[m][d + NR_QK_TOGETHER * NR_DF][c],
                            acc[m][d + NR_QK_TOGETHER * NR_DF][c+1]),nk);
#else
                        f16vec2 kv = NR_N2_K(acc[m][d + NR_QK_TOGETHER * NR_DF][c],
                                             acc[m][d + NR_QK_TOGETHER * NR_DF][c + 1]);
                        fe4m3vec2 qp = nr_quant_pair(kv * f16vec2(nk));
#endif
                        // The ColumnMajor store was the transpose: the accumulator is
                        // [dim][token] and the A operand wants [token][dim], which is
                        // the accumulator's rows as the operand's components - across
                        // the lane halves on gfx11.
                        NR_OPPUT(kreg[m][d], c, qp)
                    }
                }
            }
            // ---- pass V: rows [hh*3*NR_DF + 2*NR_DF, +NR_DF) ----
#if NR_SWQ4
#if !NR_V_SWAP
#error "NR_SWQ4: the swapped V (register-resident) body"
#endif
            // V on int4: A = y (lds_y4), B = the head's V rows; the accumulator is [token][dim], so a lane holds
            // one channel (l%16) of fragment d: one (s, c) a lane. One token tile at a time: two int accumulators
            // live instead of eight (Q and K are live here; eight cost 40 VGPRs and three waves a SIMD).
            {
                // A shared-memory barrier between the passes: without it NIR reuses the Q/K pass's y-record loads
                // (same addresses, no store between) and keeps all sixteen live across the Q/K epilogue (+43 VGPRs).
                memoryBarrierShared();
                const uint xq = NR_SWI4_REC, qw4 = wgt_u32[xq + 11u], qsc = wgt_u32[xq + 12u];
                vec2 vsc_[NR_DF];
                for (int d = 0; d < NR_DF; ++d)
                    vsc_[d] = unpackHalf2x16(floatBitsToUint(wgt_f32[qsc + uint(hh * 3 * NR_DF + 2 * NR_DF + d) * 16u + (lane & 15u)]));
                for (int m = 0; m < NR_MF; ++m) {
                    for (int d = 0; d < NR_DF; ++d) acc[m][d] = NR_ACCZERO;
                    NR_MGQ(m) {
                        NR_FRAG_IACC iv[NR_DF];
                        for (int d = 0; d < NR_DF; ++d) iv[d] = NR_IACC_ZERO;
                        for (int r_ = 0; r_ < NR_Q4REC; ++r_) {
                            const uint b = NR_SWI4_X4 + uint(m) * uint(2 * NR_C) + uint(r_) * 128u + (lane & 15u) * 4u;
                            const uvec4 xr = uvec4(lds_y4[b], lds_y4[b + 1u], lds_y4[b + 2u], lds_y4[b + 3u]);
                            const uvec4 xh = uvec4(lds_y4[b + 64u], lds_y4[b + 65u], lds_y4[b + 66u], lds_y4[b + 67u]);
                            for (int d = 0; d < NR_DF; ++d) {
                                const uint wb = (qw4 + (uint(hh * 3 * NR_DF + 2 * NR_DF + d) * uint(NR_Q4REC) + uint(r_)) * 512u) / 4u + (lane & 15u) * 4u;
                                const uvec4 wr = uvec4(wgt_u32[wb], wgt_u32[wb + 1u], wgt_u32[wb + 2u], wgt_u32[wb + 3u]);
                                const uvec4 wh = uvec4(wgt_u32[wb + 64u], wgt_u32[wb + 65u], wgt_u32[wb + 66u], wgt_u32[wb + 67u]);
                                iv[d] = nr_s4mma(iv[d], NR_S4SLOTS0(xr, xh), NR_S4SLOTS0(wr, wh));
                                if (2 * r_ + 1 < NR_Q4STEPS) iv[d] = nr_s4mma(iv[d], NR_S4SLOTS1(xr, xh), NR_S4SLOTS1(wr, wh));
                            }
                        }
                        for (int d = 0; d < NR_DF; ++d) for (int c = 0; c < 8; ++c) acc[m][d][c] = fma(float(iv[d][c]), vsc_[d].x, vsc_[d].y);
                    }
                }
            }
#else
            for (int m = 0; m < NR_MF; ++m)
                for (int d = 0; d < NR_DF; ++d) acc[m][d] = NR_ACCZERO;
            for (int k = 0; k < NR_CF; ++k) {
#if NR_V_SWAP
#if !(NR_HWAVES && NR_ACC_F16 == 0 && NR_QUANT_PAIRED && NR_MF == NR_JF)
#error "NR_V_SWAP: head-split FP32 body with one wave per window"
#endif
                // V = X . Wv^T, rows tokens - the same bytes read as the
                // other operand (A RowMajor and B ColumnMajor touch the same
                // addresses), so the accumulator comes out [token][dim], which
                // is the context product's V^T A operand component for component.
                NR_FRAG_A yka[NR_MF];
                for (int m = 0; m < NR_MF; ++m)
                    NR_LOAD_A(yka[m], lds_x, NR_LXB_
                              uint(m * NR_CF + k) * 256u + NR_OPQ(130u), 16u);
                for(int r=0;r<NR_DF;r+=2) {
                    NR_FRAG_B wb0,wb1;
                    NR_C32_WEIGHT_B(wb0,pc.qkv_off,uint(hh*3*NR_DF+2*NR_DF+r),uint(k),uint(NR_CF));
                    NR_C32_WEIGHT_B(wb1,pc.qkv_off,uint(hh*3*NR_DF+2*NR_DF+r+1),uint(k),uint(NR_CF));
                    for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_MMA(acc[m][r],yka[m],wb0);
                    for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_MMA(acc[m][r+1],yka[m],wb1);
                }
#else
                NR_FRAG_B yk[NR_MF];
                for (int m = 0; m < NR_MF; ++m)
                    NR_LOAD_B(yk[m], lds_x, NR_LXB_
                              uint(m * NR_CF + k) * 256u + NR_OPQ(130u), 16u);
#if defined(NR_PACKED_QKV)
#if NR_ACC_F16 != 0 || NR_QK_TOGETHER != 1 || NR_DF != 2
#error "paired QKV weights require the FP32 Q/K-together head-split path"
#endif
                for(int r=0;r<NR_DF;r+=2) {
                    NR_OPA wf0,wf1;
                    NR_WEIGHT_PAIR(wf0,wf1,pc.qkv_off,uint(hh*3*NR_DF+2*NR_DF+r),uint(k),uint(NR_CF))
                    for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_MMA(acc[m][r],wf0,yk[m]);
                    for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_MMA(acc[m][r+1],wf1,yk[m]);
                }
#else
                for (int r = 0; r < NR_DF; ++r) {
                    NR_OPA wf;
                    NR_LOAD_A(wf, NR_WARENA,
                              NR_TILE(pc.qkv_off,
                                      uint(hh * 3 * NR_DF + 2 * NR_DF + r) * 16u,
                                      uint(k) * 16u, uint(NR_C)), 16u);
                    for (int m = 0; m < NR_MF; ++m) NR_MMA(acc[m][r], wf, yk[m]);
                }
#endif
#endif
                if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                    for (int m = 0; m < NR_MF; ++m)
                        for (int r = 0; r < NR_DF; ++r) NR_RND(acc[m][r])
            }
#endif
#if NR_V_SWAP
            for (int m = 0; m < NR_MF; ++m) NR_MGK(m)
                for (int d = 0; d < NR_DF; ++d)
                    for (int c = 0; c < 8; c += 2) {
                        fe4m3vec2 qp = nr_quant_pair(NR_N2_V(
                            acc[m][d][c],
                            acc[m][d][c + 1]));
                        NR_OPPUT(vreg[d][m], c, qp)
                    }
#else
            for (int m = 0; m < NR_MF; ++m) NR_MGK(m)
                for (int d = 0; d < NR_DF; ++d) {
                    NR_FRAG_E4M3 vf;
                    for (int c = 0; c < 8; c += 2) {
                        fe4m3vec2 qp = nr_quant_pair(NR_N2_V(
                            acc[m][d][c],
                            acc[m][d][c + 1]));
                        vf[c] = qp.x;
                        vf[c + 1] = qp.y;
                    }
                    // V into this wave's own tiles of lds_y - [dim][token]
                    // stored ColumnMajor at stride 16 is read back ColumnMajor
                    // as an A operand that is [dim][token], which is what the
                    // context product wants. Nothing crosses a wave.
                    NR_STORE_ACC_COL(vf, lds_y, NR_LXB_
                                     uint(m * NR_CF + hh * NR_DF + d) * 256u, 16u);
                }
#endif
        }
#else
        for (int m = 0; m < NR_MF; ++m) {
            NR_ACCF qkv[3 * NR_DF];
            for (int r = 0; r < 3 * NR_DF; ++r) qkv[r] = NR_ACCZERO;
#if NR_WPF
            // Flat over (k, r): NR_CF * 3 * NR_DF weight tiles of the
            // projection, restarted per token block because `m` is outermost
            // here and the same tiles are read again.
#define NR_WPF_Q_TOT (NR_CF * 3 * NR_DF)
#define NR_WPF_Q_ADDR(i) NR_TILE(pc.qkv_off,                                  \
                                 uint(hh * 3 * NR_DF + (i) % (3 * NR_DF)) * 16u, \
                                 uint((i) / (3 * NR_DF)) * 16u, uint(NR_C))
            NR_OPA wpq[NR_WPF_SLOTS];
            NR_WPF_PRIME(wpq, NR_WPF_Q_TOT, NR_WPF_Q_ADDR)
#endif
            NR_MGA(m)
            for (int k = 0; k < NR_CF; ++k) {
                for (int r = 0; r < 3 * NR_DF; ++r) {
#if NR_WPF
                    NR_WPF_STEP(wpq, k * 3 * NR_DF + r, NR_WPF_Q_TOT, NR_WPF_Q_ADDR)
#define NR_WPF_WF_Q NR_WPF_AT(wpq, k * 3 * NR_DF + r)
#else
#if NR_V_SWAP
#if NR_HWAVES || NR_WPF || !(NR_ACC_F16 == 0 && NR_QUANT_PAIRED)
#error "NR_V_SWAP (window body): FP32, paired quantiser, no NR_WPF"
#endif
                    if (r >= 2 * NR_DF) {
                        // V = X . Wv^T, rows tokens (see the head-split pass V).
                        NR_OPB wfb;
                        NR_C32_WEIGHT_B(wfb,NR_WB_QKV,uint(hh*3*NR_DF+r),uint(k),uint(NR_CF));
                        NR_MMA(qkv[r], yqa[m][k], wfb);
                        continue;
                    }
#endif
                    NR_OPA wf;
                    NR_C32_WEIGHT(wf,NR_WB_QKV,uint(hh*3*NR_DF+r),uint(k),uint(NR_CF));
#define NR_WPF_WF_Q wf
#endif
                    NR_MMA(qkv[r], NR_WPF_WF_Q, NR_QKV_SRC(m, k));
                }
                if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                    for (int r = 0; r < 3 * NR_DF; ++r) NR_RND(qkv[r])
            }
            // A column of the accumulator is one token, so the L2 norm is a
            // within-lane sum over the head's NR_DF fragments plus one shuffle:
            // no LDS, no barrier. The norms are taken on the f16-narrowed
            // values, which is what the staged kernel normalised because its
            // staging array was f16.
            // The shipping normalization squares and reduces in FP16.
            // Its overflow-to-zero reciprocal norm is observable on real tensors.
#if NR_PACKED_SWIN_MATH
#if NR_NORM_F32
            // **Square and reduce in f32, straight out of the accumulator.**
            //
            // The packed-half form below builds `f16vec2(qkv[d][c],
            // qkv[NR_DF+d][c])` for all 48 components of the six QKV
            // accumulators - two scalar `v_cvt_f16_f32` each - and then throws
            // the halves away, because the scaling that follows re-reads the
            // same f32 components. Touching an accumulator component by
            // component also forces ACO to materialise it out of its packed
            // WMMA form for the whole loop, which is why the measured cost of
            // this reduction is 712 instructions where its arithmetic is ~170.
            //
            // Here the square is one `v_fma_f32` per value and there is no
            // conversion at all; only the two finished sums cross lanes.
            //
            // **This is not the same function.** The shipping reduction squares
            // and accumulates in f16, and its overflow-to-zero reciprocal norm
            // is observable on real tensors. An f32 sum does not overflow where
            // an f16 one does, so a tensor that relied on that will differ.
            // **Four independent partial sums, not one chain.** A single
            // accumulator makes this a 16-deep serial `v_fma_f32` dependency
            // where the packed-half form had eight independent chains, one per
            // component. At C=32's twelve waves a SIMD that is hidden; at
            // C=128's five it is not, and the first version of this was 10%
            // *fewer* instructions and 9% slower there. Four partials cost
            // three extra adds and bound the chain at four.
            float sqp[4], skp[4];
            for (int i=0;i<4;++i) { sqp[i]=0.0; skp[i]=0.0; }
            for (int c=0;c<8;++c)
                for (int d=0;d<NR_DF;++d) {
                    const float q = NR_QKVA(m, d)[c], k = NR_QKVA(m, NR_DF + d)[c];
                    sqp[c&3] = fma(q, q, sqp[c&3]);
                    skp[c&3] = fma(k, k, skp[c&3]);
                }
            // **One cross-lane exchange, on the pair.** Two f32 shuffles sit
            // on the critical path with nothing left to overlap them - the
            // packed-half form interleaved its eight with the squares, which
            // is why the first f32 version was 10% fewer instructions and 11%
            // slower at C=128, where only five waves a SIMD are there to hide
            // the stall. Narrowing the two sums to a half pair first makes it
            // one exchange, and the pair is the type the rest of this wants.
            // **Reproduce the half form's overflow, because it is behaviour
            // and not precision.** The shipping reduction accumulates in f16,
            // so a sum past 65504 becomes infinity, `inversesqrt` of it is
            // zero, and that token's Q or K is zeroed - the source comment
            // calls this "observable on real tensors" and it is. An f32 sum
            // never overflows, so without this the network does something
            // different on exactly the tensors the note is about. Narrowing
            // the f32 sum to f16 does the same thing at the same threshold.
#if NR_QK_SCALE_FAST == 3
            vec2 sqk[1];
            sqk[0] = vec2((sqp[0]+sqp[1])+(sqp[2]+sqp[3]),
                          (skp[0]+skp[1])+(skp[2]+skp[3]));
            sqk[0] += subgroupShuffleXor(sqk[0],16u);
#else
            f16vec2 sqk[1];
            sqk[0] = NR_N2_NORM((sqp[0]+sqp[1])+(sqp[2]+sqp[3]),
                                (skp[0]+skp[1])+(skp[2]+skp[3]));
            sqk[0] = sqk[0] + unpackFloat2x16(
                subgroupShuffleXor(packFloat2x16(sqk[0]), 16u));
#endif
#else
            f16vec2 sqk[8];
            for (int c=0;c<8;++c) {
                sqk[c]=f16vec2(0.0hf);
                for (int d=0;d<NR_DF;++d) {
                    f16vec2 qk=NR_N2_NSQ(NR_QKVA(m, d)[c],NR_QKVA(m, NR_DF+d)[c]);
                    // Preserve the original scalar path's contraction permission.
                    // Adding precise changes the full-frame output on this driver.
                    f16vec2 square=qk*qk;
                    sqk[c]=sqk[c]+square;
                }
#if !NR_NORM_LATE_SHUFFLE
                // `packFloat2x16`, not `packHalf2x16(vec2(...))`: the value is
                // already an f16vec2 and the shuffle only needs its 32 bits in
                // another lane, not a widen and a narrow.
                uint other=subgroupShuffleXor(packFloat2x16(sqk[c]),16u);
                sqk[c]=sqk[c]+unpackFloat2x16(other);
#endif
            }
            for(int stride=4;stride>0;stride/=2)for(int c=0;c<stride;++c)
                sqk[c]=sqk[c]+sqk[c+stride];
#if NR_NORM_LATE_SHUFFLE
            // **One cross-lane exchange instead of eight.** The eight rows a
            // lane owns are reduced in registers first and only the total
            // crosses the half-wave boundary. A lane's own eight rows and its
            // partner's eight are still each summed before being added
            // together, so this is not the same summation order - check the
            // gold before believing it is free.
            {
                uint other=subgroupShuffleXor(packFloat2x16(sqk[0]),16u);
                sqk[0]=sqk[0]+unpackFloat2x16(other);
            }
#endif
#endif
#if NR_QK_SCALE_FAST == 3
            float nq=inversesqrt(max(sqk[0].x,0.000062));
            float nk=inversesqrt(max(sqk[0].y,0.000062));
#elif NR_ABLATE_NORM == 1
            // Deletes the reduction *and* the per-value scaling, because a
            // unit factor lets ACO fold the multiplies away too.
            NR_F16 nq=NR_F16(1.0), nk=NR_F16(1.0);
#elif NR_ABLATE_NORM == 2
            // Deletes only the reduction: `hscale` is a runtime value ACO
            // cannot fold, so every scaling multiply and its conversions stay.
            // The difference between 1 and 2 is the norm *computation* alone.
            NR_F16 nq=hscale, nk=hscale;
#else
            NR_F16 nq=nr_norm_rsq(sqk[0].x);
            NR_F16 nk=nr_norm_rsq(sqk[0].y);
#endif
#else
            NR_F16 sq[8], sk[8];
            for(int c=0;c<8;++c) {
                sq[c]=NR_F16(0.0);sk[c]=NR_F16(0.0);
                for(int d=0;d<NR_DF;++d) {
                    NR_F16 q=NR_F16(NR_QKVA(m, d)[c]),k=NR_F16(NR_QKVA(m, NR_DF+d)[c]);
                    sq[c]=NR_F16(sq[c]+NR_F16(q*q));sk[c]=NR_F16(sk[c]+NR_F16(k*k));
                }
                sq[c]=NR_F16(sq[c]+NR_F16(subgroupShuffleXor(float(sq[c]),16u)));
                sk[c]=NR_F16(sk[c]+NR_F16(subgroupShuffleXor(float(sk[c]),16u)));
            }
            for(int stride=4;stride>0;stride/=2)for(int c=0;c<stride;++c) {
                sq[c]=NR_F16(sq[c]+sq[c+stride]);sk[c]=NR_F16(sk[c]+sk[c+stride]);
            }
            NR_F16 nq=nr_norm_rsq(sq[0]);
            NR_F16 nk=nr_norm_rsq(sk[0]);
#endif
            const uint t0 = tok0 + uint(m) * 16u;
            for (int d = 0; d < NR_DF; ++d) {
#if NR_QBATCH_ON
#define NR_QV_Q(c) (((NR_N2_Q(NR_QKVA(m, d)[c], NR_QKVA(m, d)[(c) + 1]))      \
                     * f16vec2(nq)) * f16vec2(hscale))
                NR_QRUN8(qb[m][d], NR_QV_Q)
#undef NR_QV_Q
#elif NR_QUANT_PAIRED
                for (int c = 0; c < 8; c += 2) {
                    // Packed, and bit-identical: `v_pk_mul_f16` rounds each
                    // half to f16 with round-to-nearest-even, which is exactly
                    // what `NR_F16(NR_F16(x) * nq)` asks for. The two scalar
                    // chains were four multiplies and six roundings a pair
                    // where this is one narrowing and two packed multiplies.
                    // The two multiplies stay separate: `nq * hscale` folded
                    // into one constant would drop a rounding step of NVIDIA's.
#if NR_QK_SCALE_FAST
                    NR_QK_QT qp = nr_qscale_fast(vec2(NR_QKVA(m,d)[c],NR_QKVA(m,d)[c+1]),nq,hscale);
#else
                    f16vec2 qv = NR_N2_Q(NR_QKVA(m, d)[c], NR_QKVA(m, d)[c + 1]);
                    qv = (qv * f16vec2(nq)) * f16vec2(hscale);
                    NR_QK_QT qp = NR_QP_Q(qv);
#endif
                    NR_OPPUT(qb[m][d], c, qp)
                }
#else
                for (int c = 0; c < 8; ++c)
                    qb[m][d][c] = nr_quant_e4m3(NR_F16(NR_F16(NR_F16(NR_QKVA(m, d)[c]) * nq) * hscale));
#endif
                NR_QK_STAGE_FRAG kf;
#if NR_QBATCH_ON
#define NR_QV_K(c) (NR_N2_K(NR_QKVA(m, NR_DF + d)[c],                         \
                            NR_QKVA(m, NR_DF + d)[(c) + 1]) * f16vec2(nk))
                NR_QRUN8(kf, NR_QV_K)
#undef NR_QV_K
#elif NR_QUANT_PAIRED
                for (int c = 0; c < 8; c += 2) {
#if NR_QK_SCALE_FAST
                    NR_QK_QT qp = nr_kscale_fast(vec2(NR_QKVA(m,NR_DF+d)[c],NR_QKVA(m,NR_DF+d)[c+1]),nk);
#else
                    f16vec2 kv = NR_N2_K(NR_QKVA(m, NR_DF + d)[c],
                                         NR_QKVA(m, NR_DF + d)[c + 1]);
                    NR_QK_QT qp = NR_QP_K(kv * f16vec2(nk));
#endif
#if NR_K_REGS
                    // The ColumnMajor store below is the transpose; the A operand takes the
                    // accumulator's rows as its components, across the lane halves.
                    NR_OPPUT(kreg[m][d], c, qp)
#else
                    kf[c] = qp.x;
                    kf[c + 1] = qp.y;
#endif
                }
#else
                for (int c = 0; c < 8; ++c)
                    kf[c] = nr_quant_e4m3(NR_F16(NR_F16(NR_QKVA(m, NR_DF + d)[c]) * nk));
#endif
                // K wants [token][dim]: store ColumnMajor, the accumulator
                // being [dim][token]. V wants [dim][token], which is the
                // accumulator's own orientation - a plain RowMajor store.
#if !NR_K_REGS
#if NR_KV_LDS_SWAP
                // The same store component by component, with the dims of the odd tokens (the
                // rows the upper lane half reads as the logits' A operand) pair-swapped, so K
                // agrees with the k-pair-swapped Q^T of NR_OPPUT. Component c is (dim 2c + h,
                // token lane % 16); 2c has no bit 0, so the XOR is a per-lane constant on the
                // lane's base address and the component offset stays an immediate.
                {
                    const uint nr_kb = NR_WKB (t0 + (lane & 15u)) * uint(NR_HD) + uint(d) * 16u
                                       + ((lane >> 4u) ^ (lane & 1u));
                    for (int c = 0; c < 8; ++c) lds_k[nr_kb + 2u * uint(c)] = kf[c];
                }
#else
                NR_STORE_ACC_COL(kf, lds_k, NR_WKB t0 * uint(NR_HD) + uint(d) * 16u, uint(NR_HD));
#endif
#endif
                NR_PV_STAGE_FRAG vf;
#if NR_QBATCH_ON
#define NR_QV_V(c) NR_N2_V(NR_QKVA(m, 2 * NR_DF + d)[c],                      \
                           NR_QKVA(m, 2 * NR_DF + d)[(c) + 1])
                NR_QRUN8(vf, NR_QV_V)
#undef NR_QV_V
#elif NR_QUANT_PAIRED
                for (int c = 0; c < 8; c += 2) {
                    NR_PV_QT qp = NR_QP_V(NR_N2_V(
                        NR_QKVA(m, 2 * NR_DF + d)[c],
                        NR_QKVA(m, 2 * NR_DF + d)[c + 1]));
#if NR_V_REGS && !NR_HWAVES
                    NR_OPPUT(vreg[d][m], c, qp)
#else
                    vf[c] = qp.x;
                    vf[c + 1] = qp.y;
#endif
                }
#else
                for (int c = 0; c < 8; ++c)
                    vf[c] = nr_quant_e4m3(NR_F16(NR_QKVA(m, 2 * NR_DF + d)[c]));
#endif
#if NR_HWAVES
                // V into this wave's own tiles of lds_y - [dim][token] stored
                // ColumnMajor at stride 16 is read back ColumnMajor as an A
                // operand that is [dim][token], which is what the context
                // product wants. Nothing crosses a wave, so no barrier.
                NR_STORE_ACC_COL(vf, lds_y, NR_LXB_
                                 uint(m * NR_CF + hh * NR_DF + d) * 256u, 16u);
#elif NR_V_SWAP && !NR_V_REGS
#if NR_KV_LDS_SWAP
                // V^T is the context product's A operand: the odd dims (its upper-half rows)
                // take their tokens pair-swapped to agree with the k-pair-swapped P^T. The
                // accumulator is [token][dim] here: component c is (token 2c + h, dim lane % 16).
                {
                    const uint nr_vb = NR_WKB NR_V_LDS_OFFSET + (uint(d) * 16u + (lane & 15u)) * uint(NR_WIN) + t0
                                       + ((lane >> 4u) ^ (lane & 1u));
                    for (int c = 0; c < 8; ++c) lds_v[nr_vb + 2u * uint(c)] = vf[c];
                }
#else
                NR_STORE_ACC_COL(vf, lds_v, NR_WKB NR_V_LDS_OFFSET + uint(d) * 16u * uint(NR_WIN) + t0, uint(NR_WIN));
#endif
#elif !NR_V_SWAP
                NR_STORE_ACC(vf, lds_v, NR_WKB NR_V_LDS_OFFSET + uint(d) * 16u * uint(NR_WIN) + t0, uint(NR_WIN));
#endif
            }
        }
#endif
#if !NR_HWAVES
        NR_BODY_BARRIER();
#endif
#if NR_SWQ4
        // every head is done reading the int4 y copy before the context overwrites lds_y.
        NR_BODY_BARRIER();
#endif

        // S^T = K . Q^T, so K is the A operand straight out of LDS and Q^T is
        // the B operand straight out of the QKV accumulator. The accumulator's
        // rows are the *key* tokens, which is what makes the softmax
        // denominator a within-lane sum: one shuffle, no memory.
#if NR_HWAVES
        // **One query fragment at a time.** The token split runs the key
        // fragment outermost and keeps `pq2[NR_MF][NR_JF][4]` - 64 VGPRs of
        // exponentials - live until every j has been computed, plus
        // `pb[NR_MF][NR_JF]` (32) and `lg[NR_MF]` (32) on top of `qb` and
        // `kreg`. With the query fragment outermost only `pb` crosses an
        // iteration: the logits, the exponentials and the denominator of one
        // query block are born and die inside it.
        //
        // Arithmetic unchanged: each logit accumulates over the same d in the
        // same order with the same A operand, the bias and the exponential are
        // the same calls on the same values, and `nr_swin_probability_sum`
        // reduces the same [NR_JF][4] array it was handed as `pq2[m]`.
#if NR_EXP_NOHI_HEAD
        // heads whose weights keep every exponent input under the upper clamp
        // (host audit, record word 22 bit h) take the copy without it.
        [[dont_flatten]] if (((subgroupBroadcastFirst(nr_hnohi) >> uint(hh)) & 1u) != 0u) {
#define NR_EXPB nr_swin_exp_baked_nohi
#include "fswin_hw_attn.glsl"
#undef NR_EXPB
        } else {
#define NR_EXPB nr_swin_exp_baked
#include "fswin_hw_attn.glsl"
#undef NR_EXPB
        }
#else
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
                        nr_swin_exp_baked(vec2(lg[j][c],lg[j][c+1]),vec2(bf[c],bf[c+1]));
#else
                        nr_swin_exp2(vec2(lg[j][c] + bf[c], lg[j][c + 1] + bf[c + 1]));
#endif
            }
            NR_F16 sum = nr_swin_probability_sum(pq2);
            // The f32 spelling is what reaches the free converter - see the
            // packed-multiply note in the token-split path below.
            float inv = float(NR_F16(1.0 / float(max(sum, NR_F16(NR_SUM_FLOOR)))));
            for (int j = 0; j < NR_JF; ++j)
                for (int c = 0; c < 4; ++c) {
                    float x = float(pq2[j][c].x) * inv;
                    float y = float(pq2[j][c].y) * inv;
                    // `NR_N2_P`'s NR_PKN=0/NR_R2=0 expansion is exactly the
                    // `f16vec2(NR_F16(x), NR_F16(y))` that stood here, so the
                    // shipping SPIR-V does not move; the macro is what lets the
                    // narrowing knobs reach the head-split path at all, which
                    // they did not before (the P column of the ledger is a
                    // C=32 measurement and C=32 is the one width with no head
                    // split).
                    // On an FP8 part NR_PROB_BOUNDED took the bare convert, which for these bounded
                    // probabilities is the same rounding; here it is the quantiser either way.
                    fe4m3vec2 q = nr_quant_pair(NR_N2_P(x, y));
                    NR_OPPUT(pb[m][j], 2 * c, q)
                }
        }
#endif
#else
        NR_PV_OPB pb[NR_MF][NR_JF];
#if NR_QUANT_PAIRED
        f16vec2 pq2[NR_MF][NR_JF][4];
#else
        float pq[NR_MF][NR_JF][8];
#endif
#ifdef NR_DIAG_BIAS_ONCE
        NR_FRAG_ACC nr_b1;
        NR_LOAD_ACC_COL(nr_b1, wgt_f32, pc.b_off + uint(hh) * uint(NR_WIN * NR_WIN) + tok0 * uint(NR_WIN),
                        uint(NR_WIN));
#endif
        for (int j = 0; j < NR_JF; ++j) {
            NR_ACCF lg[NR_MF];
#if NR_BIAS_SEED
            for (int m = 0; m < NR_MF; ++m) lg[m] = nr_swin_bias_seed(uint(hh),uint(m),uint(j),tok0);
#else
            for (int m = 0; m < NR_MF; ++m) lg[m] = NR_ACCZERO;
#endif
            for (int d = 0; d < NR_DF; ++d) {
                // One K fragment, NR_MF products - K amortises like a weight.
#if NR_K_REGS
                NR_QK_OPA kf = kreg[j][d];
#else
                NR_QK_OPA kf;
                NR_LOAD_A(kf, lds_k, NR_WKB uint(j) * 16u * uint(NR_HD) + uint(d) * 16u,
                          uint(NR_HD));
#endif
                for (int m = 0; m < NR_MF; ++m) NR_MGA(m) NR_MMA(lg[m], kf, qb[m][d]);
            }
            // The bias block loads ColumnMajor: it is stored [query][key] and
            // this accumulator is [key][query] - the same addresses, read the
            // other way round.
            for (int m = 0; m < NR_MF; ++m) NR_MGA(m) {
#if !NR_BIAS_SEED
#if NR_SWIN_BIAS_F16
                NR_FRAG_ACC16 bhalf;
                NR_LOAD_ACC_COL(bhalf, wgt_f16,
                                pc.b_off + uint(hh) * uint(NR_WIN * NR_WIN)
                                + (tok0 + uint(m) * 16u) * uint(NR_WIN) + uint(j) * 16u,
                                uint(NR_WIN));
#if NR_ABLATE_BIAS
                NR_ACCF bf = NR_ACCZERO;
#elif NR_F16_MMA
                // The fragment the frame ships is already the type the f16
                // accumulator wants: no widening at all.
                NR_ACCF bf = bhalf;
#else
                NR_FRAG_ACC bf = NR_FRAG_ACC(bhalf);
#endif
#elif NR_F16_MMA
                // The standalone harness supplies an f32 bias, so this build
                // narrows the fragment once per component. It is a property of
                // the harness's weight contract, not of the kernel.
                NR_FRAG_ACC bwide;
                NR_LOAD_ACC_COL(bwide, wgt_f32,
                                pc.b_off + uint(hh) * uint(NR_WIN * NR_WIN)
                                + (tok0 + uint(m) * 16u) * uint(NR_WIN) + uint(j) * 16u,
                                uint(NR_WIN));
                NR_ACCF bf = NR_ACCF(bwide);
#elif defined(NR_DIAG_BIAS_ONCE)
                NR_FRAG_ACC bf = nr_b1;
#else
                NR_FRAG_ACC bf;
#if NR_BIAS_TABLE
                NR_BIAS_TBL_LOAD(bf, uint(hh), tok0 + uint(m) * 16u, uint(j))
#elif NR_WSHARE > 1 && (NR_WSHARE_PARTS & 2)
                for (int c = 0; c < 8; ++c)
                    bf[c] = nr_bl[(((tok0 / 16u + uint(m)) * 4u + uint(j)) * 4u + uint(c / 2)) * 64u + lane * 2u + uint(c & 1)];
#else
                NR_LOAD_ACC_COL(bf, wgt_f32,
                                pc.b_off + NR_BFOLD(uint(hh) * uint(NR_WIN * NR_WIN)
                                + (tok0 + uint(m) * 16u) * uint(NR_WIN) + uint(j) * 16u),
                                uint(NR_WIN));
#endif
#endif
#endif
#if NR_PACKED_SWIN_MATH
                for (int c = 0; c < 8; c += 2) {
#if NR_BIAS_SEED
                    f16vec2 e = nr_swin_exp2(vec2(lg[m][c],lg[m][c+1]));
#elif NR_BAKED_EXP_BIAS
                    f16vec2 e = nr_swin_exp_baked(vec2(lg[m][c],lg[m][c+1]),vec2(bf[c],bf[c+1]));
#else
                    f16vec2 e = nr_swin_exp2(vec2(lg[m][c] + bf[c], lg[m][c+1] + bf[c+1]));
#endif
#if NR_QUANT_PAIRED
                    pq2[m][j][c/2] = e;
#else
                    pq[m][j][c] = float(e.x);
                    pq[m][j][c+1] = float(e.y);
#endif
                }
#else
#if NR_QUANT_PAIRED
                for (int c = 0; c < 8; c += 2)
                    pq2[m][j][c/2] = f16vec2(nr_fast_exp(lg[m][c]+bf[c]),nr_fast_exp(lg[m][c+1]+bf[c+1]));
#else
                for (int c = 0; c < 8; ++c)
                    pq[m][j][c] = nr_fast_exp(lg[m][c] + bf[c]);
#endif
#endif
            }
        }
        for (int m = 0; m < NR_MF; ++m) NR_MGA(m) {
#if NR_QUANT_PAIRED
            NR_F16 sum=nr_swin_probability_sum(pq2[m]);
            // **Priced and rejected: the packed multiply that Q and K use
            // costs 63 instructions here.** `pq2` is already an `f16vec2` and
            // `inv` is an exact f16 held in an f32, so
            // `pq2[m][j][c] * f16vec2(NR_F16(inv))` is the *same function* -
            // the product of two f16 values needs 22 mantissa bits and is
            // therefore exact in f32, so rounding it to f16 and multiplying in
            // f16 round-to-nearest-even give the same number - and it is one
            // `v_pk_mul_f16` where the two lines below are two widenings and
            // two scalar multiplies. It measured **+63 instructions: +56
            // `v_cvt_f32_f16`, +32 packed min/max, +26 `s_setreg`**.
            //
            // The reason is downstream, not here. Handing `nr_quant_pair` a
            // value whose provenance is a packed f16 multiply loses NIR's
            // `f2e4m3fn_satfn` match for these 32 pairs and drops them onto the
            // `v_minimummaximum_f32` clamping path the mode-4 comment in
            // coopmm.glsl warns about - the extra packed min/max and the extra
            // MODE writes are that fallback. **The f32 spelling is what reaches
            // the free converter.** Do not re-try it.
            float inv=float(NR_F16(1.0/float(max(sum,NR_F16(NR_SUM_FLOOR)))));
            for (int j=0;j<NR_JF;++j) for (int c=0;c<4;++c) {
#if NR_ATT_F16_PV
                // **The packed multiply rejected above, for the reason given there.**
                // It cost 63 instructions there only because it broke NIR's
                // `f2e4m3fn_satfn` match and dropped these pairs onto the slow
                // clamping path *of the converter that follows*. Under
                // NR_ATT_F16_PV there is no converter following: the value is
                // the operand. The multiply is the same number either way - the
                // product of two f16 values is exact in f32, so rounding it to
                // f16 and multiplying in f16 RNE agree - and this is one
                // `v_pk_mul_f16` where the f32 spelling is two widenings, two
                // multiplies and two narrowings.
                NR_PV_QT q = pq2[m][j][c] * f16vec2(NR_F16(inv));
#else
                float x=float(pq2[m][j][c].x)*inv;
                float y=float(pq2[m][j][c].y)*inv;
                NR_PV_QT q=NR_QP_P(NR_N2_P(x,y));
#endif
                NR_OPPUT(pb[m][j], 2 * c, q)
#else
            // Sum the eight key groups at each column in the packed PTX order.
            NR_F16 part[8];
            for (int c = 0; c < 8; ++c) {
                part[c] = NR_F16(0.0);
                for (int j = 0; j < NR_JF; ++j) {
                    NR_F16 pair = NR_F16(pq[m][j][c]
                        + subgroupShuffleXor(pq[m][j][c], 16u));
                    part[c] = NR_F16(part[c] + pair);
                }
#endif
            }
#if NR_QUANT_PAIRED
#else
            NR_F16 even = NR_F16(NR_F16(NR_F16(part[0]+part[2])+part[4])+part[6]);
            NR_F16 odd = NR_F16(NR_F16(NR_F16(part[1]+part[3])+part[5])+part[7]);
            NR_F16 sum = NR_F16(even + odd);
            const float inv = float(NR_F16(1.0 / float(max(sum, NR_F16(NR_SUM_FLOOR)))));
            for (int j = 0; j < NR_JF; ++j)
#if NR_QBATCH_ON
            {
#define NR_QV_PB(c) f16vec2(NR_F16(pq[m][j][c] * inv),                        \
                            NR_F16(pq[m][j][(c) + 1] * inv))
                NR_QRUN8(pb[m][j], NR_QV_PB)
#undef NR_QV_PB
            }
#elif NR_QUANT_PAIRED
                for (int c = 0; c < 8; c += 2) {
                    fe4m3vec2 qp = nr_quant_pair(f16vec2(
                        NR_F16(pq[m][j][c] * inv),
                        NR_F16(pq[m][j][c + 1] * inv)));
                    pb[m][j][c] = qp.x;
                    pb[m][j][c + 1] = qp.y;
                }
#else
                for (int c = 0; c < 8; ++c)
                    pb[m][j][c] = nr_quant_e4m3(NR_F16(pq[m][j][c] * inv));
#endif
#endif
        }
#endif
#if !(NR_HWAVES && NR_EXP_NOHI_HEAD)
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
                    NR_OPPUT(cq[m][hh * NR_DF + e], c, qp)
                }
#else
                for (int c = 0; c < 8; ++c)
                    cq[m][hh * NR_DF + e][c] = nr_quant_e4m3(NR_F16(a[m][c]));
#endif
#endif
            }
        }
#endif
#if NR_HEADS > 1
        // The next head reuses lds_k and lds_v; at one head there is no next.
        NR_BODY_BARRIER();
#elif defined(NR_DS_PROJECT) && NR_FWAVES > 1
        // The pooled pixels are staged in lds_v, which the other wave may still be
        // reading as V.
        NR_BODY_BARRIER();
#endif
    }

#if NR_CONTEXT_LDS
    // Reload constant-index FP8 fragments after the final head barrier.
    for (int m=0;m<NR_MF;++m) for (int k=0;k<NR_CF;++k)
        NR_LOAD_B(cq[m][k], lds_context,
                  (tok0+uint(m)*16u)*uint(NR_C)+uint(k)*16u, uint(NR_C));
#endif
    // ---- stage 6: the output projection and the attention skip ------------
#if NR_IMGOUT_REGS
    // One RGB triple per token *pair* of fragments, summed across the n loop in
    // the original's channel order. `nr_io_lo` picks which of the pair a lane
    // owns; the token it lands on is `tok0 + 2p*16 + lane` for both halves.
    const bool nr_io_lo = lane < 16u;
    const uint nr_io_wp = NR_IMG_WOFF >> 1u;
#if NR_IO_F32FMA
    // x * 1.0 is exact; the 1.0 is one the compiler cannot see (no dispatch has 2^32-1
    // workgroups deep), so the widening stays an instruction of its own and the FMAs below
    // read an f32 register - v_fmac_f32, dual-issuable - instead of folding back into v_fma_mix.
    const float nr_io_one = gl_NumWorkGroups.z == 0xFFFFFFFFu ? 2.0 : 1.0;
#define nr_io_widen(h_) (float(h_) * nr_io_one)
#endif
    vec3 nr_io_o[NR_MF / 2];
    for (int p = 0; p < NR_MF / 2; ++p) nr_io_o[p] = vec3(0.0);
#ifdef NR_TEMPORAL_HISTORY
    float nr_io_ow[NR_MF / 2];
    for (int p = 0; p < NR_MF / 2; ++p) nr_io_ow[p] = 0.0;
#endif
// Two channels a step, exactly `image_w_off`'s uint pairing, so the weights and
// the summation sequence are the ones the LDS form used.
#ifdef NR_TEMPORAL_HISTORY
#if NR_IO_DOT2
#define NR_IO_W3(p_, ci_) { \
        const f16vec2 ww=unpackFloat2x16(wgt_u32[nr_io_wp+48u+(ci_)]); \
        nr_io_ow[p_]=nr_fdot2mix(av, ww, nr_io_ow[p_]); }
#elif NR_IO_F32FMA
#define NR_IO_W3(p_, ci_) { \
        const vec2 ww=vec2(unpackFloat2x16(wgt_u32[nr_io_wp+48u+(ci_)]))*nr_io_one; \
        nr_io_ow[p_]=fma(nr_io_a,ww.x,nr_io_ow[p_]); nr_io_ow[p_]=fma(nr_io_a1,ww.y,nr_io_ow[p_]); }
#else
#define NR_IO_W3(p_, ci_) { \
        const f16vec2 ww=unpackFloat2x16(wgt_u32[nr_io_wp+48u+(ci_)]); \
        nr_io_ow[p_]+=nr_io_a*float(ww.x); nr_io_ow[p_]+=nr_io_a1*float(ww.y); }
#endif
#else
#define NR_IO_W3(p_, ci_) {}
#endif
// The pair travels as a packed uint - two f16 in one VGPR, which is how the
// accumulator already holds them - so the select and the cross-lane exchange
// are one instruction for two channels, and the multiply still reads
// `float(f16) * float(f16)` exactly as the LDS form did. That shape matters:
// widening the activation before the select breaks the f16-source pattern ACO
// contracts, and the picture moves by one code on 0.4% of channel samples.
#if NR_IO_DOT2
#define NR_IO_ACC(p_, ci_, u_) { \
        const f16vec2 av=unpackFloat2x16(u_); \
        const f16vec2 wx=unpackFloat2x16(wgt_u32[nr_io_wp+(ci_)]); \
        const f16vec2 wy=unpackFloat2x16(wgt_u32[nr_io_wp+16u+(ci_)]); \
        const f16vec2 wz=unpackFloat2x16(wgt_u32[nr_io_wp+32u+(ci_)]); \
        nr_io_o[p_].x=nr_fdot2mix(av, wx, nr_io_o[p_].x); \
        nr_io_o[p_].y=nr_fdot2mix(av, wy, nr_io_o[p_].y); \
        nr_io_o[p_].z=nr_fdot2mix(av, wz, nr_io_o[p_].z); \
        NR_IO_W3(p_, ci_) }
#elif NR_DIAG_NOIOPROJ
// diagnostic (wrong picture): the projection's FMAs gone, one add keeps the inputs live.
#define NR_IO_ACC(p_, ci_, u_) { const f16vec2 av=unpackFloat2x16(u_); nr_io_o[p_].x+=float(av.x)+float(av.y); }
#elif NR_IO_F32FMA
// the same single-rounding f32 FMAs as ACO's v_fma_mix, spelled as fma() on the exactly
// widened operands: the activation widened once for all rows, the (uniform) weights widened on
// the scalar unit, so the products can issue as dual (VOPD) v_fmac_f32. Same order, same bits.
#define NR_IO_ACC(p_, ci_, u_) { \
        const f16vec2 av=unpackFloat2x16(u_); \
        const float nr_io_a=nr_io_widen(av.x), nr_io_a1=nr_io_widen(av.y); \
        const vec2 wx=vec2(unpackFloat2x16(wgt_u32[nr_io_wp+(ci_)]))*nr_io_one; \
        const vec2 wy=vec2(unpackFloat2x16(wgt_u32[nr_io_wp+16u+(ci_)]))*nr_io_one; \
        const vec2 wz=vec2(unpackFloat2x16(wgt_u32[nr_io_wp+32u+(ci_)]))*nr_io_one; \
        nr_io_o[p_].x=fma(nr_io_a,wx.x,nr_io_o[p_].x); nr_io_o[p_].x=fma(nr_io_a1,wx.y,nr_io_o[p_].x); \
        nr_io_o[p_].y=fma(nr_io_a,wy.x,nr_io_o[p_].y); nr_io_o[p_].y=fma(nr_io_a1,wy.y,nr_io_o[p_].y); \
        nr_io_o[p_].z=fma(nr_io_a,wz.x,nr_io_o[p_].z); nr_io_o[p_].z=fma(nr_io_a1,wz.y,nr_io_o[p_].z); \
        NR_IO_W3(p_, ci_) }
#else
#define NR_IO_ACC(p_, ci_, u_) { \
        const f16vec2 av=unpackFloat2x16(u_); \
        const float nr_io_a=float(av.x), nr_io_a1=float(av.y); \
        const f16vec2 wx=unpackFloat2x16(wgt_u32[nr_io_wp+(ci_)]); \
        const f16vec2 wy=unpackFloat2x16(wgt_u32[nr_io_wp+16u+(ci_)]); \
        const f16vec2 wz=unpackFloat2x16(wgt_u32[nr_io_wp+32u+(ci_)]); \
        nr_io_o[p_].x+=nr_io_a*float(wx.x); nr_io_o[p_].x+=nr_io_a1*float(wx.y); \
        nr_io_o[p_].y+=nr_io_a*float(wy.x); nr_io_o[p_].y+=nr_io_a1*float(wy.y); \
        nr_io_o[p_].z+=nr_io_a*float(wz.x); nr_io_o[p_].z+=nr_io_a1*float(wz.y); \
        NR_IO_W3(p_, ci_) }
#endif
#endif
#if NR_WPF
    // Flat over (n, k): NR_CF * NR_CF weight tiles of the output projection.
#define NR_WPF_OP_TOT (NR_CF * NR_CF)
#define NR_WPF_OP_ADDR(i) NR_TILE(pc.op_off, uint((i) / NR_CF) * 16u,         \
                                  uint((i) % NR_CF) * 16u, uint(NR_C))
    NR_OPA wpop[NR_WPF_SLOTS];
    NR_WPF_PRIME(wpop, NR_WPF_OP_TOT, NR_WPF_OP_ADDR)
#endif
#if (NR_PACKED_DENSE & 4) && NR_HWAVES
#if NR_DF != 2 || NR_ACC_F16 != 0 || !NR_NATIVE_RESIDUAL || !NR_PTX_ACC || NR_WPF || NR_ABLATE_RESID
#error "paired dense weights require the default FP32 head-split path"
#endif
    NR_ACCF nr_project_acc[NR_DF][NR_MF];
    for(int p=0;p<NR_DF;++p) {
        const int n=nrhw_h*NR_DF+p;
        for(int m=0;m<NR_MF;++m) NR_MGA(m) {
            NR_FRAG_B yr;
            NR_LOAD_B(yr,lds_x, NR_LXB_ uint(m*NR_CF+n)*256u+NR_OPQ(80u),16u);
            for(int c=0;c<8;c+=2) {
                const uint off=pc.ars_off+uint(n)*16u+NR_ROW(c);
#if NR_RESIDUAL_F32
                const vec2 residual=vec2(yr[c],yr[c+1])*vec2(wgt_f32[off],wgt_f32[off+2u]);
#else
                const f16vec2 scale=nr_residual_scale(off);
                const f16vec2 residual=f16vec2(yr[c],yr[c+1])*scale;
#endif
                nr_project_acc[p][m][c]=float(residual.x);
                nr_project_acc[p][m][c+1]=float(residual.y);
            }
        }
    }
    for(int k=0;k<NR_CF;++k) {
        NR_FRAG_B ctx[NR_MF];
        for(int m=0;m<NR_MF;++m)
            NR_LOAD_B(ctx[m],lds_y, NR_LXB_ uint(m*NR_CF+k)*256u+NR_OPQ(64u),16u);
        NR_OPA wf0,wf1;
        NR_WEIGHT_PAIR(wf0,wf1,pc.op_off,uint(nrhw_h*NR_DF),uint(k),uint(NR_CF))
        for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_MMA(nr_project_acc[0][m],wf0,ctx[m]);
        for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_MMA(nr_project_acc[1][m],wf1,ctx[m]);
    }
#endif
#if NR_HWAVES
    for (int nn = 0; nn < NR_DF; ++nn) {
        const int n = nrhw_h * NR_DF + nn;
        // The attention skip's y, read back out of the rows this wave wrote in
        // stage 2 - nothing writes `lds_x` after that, and the store was
        // NR_STORE_ACC_COL of the same components NR_LOAD_B hands back, which
        // is the round trip stage 1's staging already relies on. Same e4m3
        // bytes, same `float()` widening, no register held across the
        // attention.
        NR_FRAG_B yqr[NR_MF];
        for (int m = 0; m < NR_MF; ++m)
            NR_LOAD_B(yqr[m], lds_x, NR_LXB_
                      uint(m * NR_CF + n) * 256u + NR_OPQ(80u), 16u);
#else
#if NR_IO_WMMA
    // NR_IO_WMMA: the 32 -> RGB(+history weight) projection as f16 WMMA with
    // f32 accumulation. The weights are [row][32] f16 (x, y, z, w3 rows 32 apart),
    // so a RowMajor A load of 16 rows gives rows 0-3 = the projection; rows 4-15
    // read whatever follows and land only in output rows nobody reads.
    NR_FRAG_A16 nr_io_wa[NR_CF];
    for (int n = 0; n < NR_CF; ++n) NR_LOAD_A(nr_io_wa[n], wgt_f16, NR_IMG_WOFF + uint(n) * 16u, 32u);
    NR_FRAG_ACC nr_io_rgb[NR_MF];
    for (int m = 0; m < NR_MF; ++m) nr_io_rgb[m] = NR_FRAG_ACC(0.0);
#endif
    for (int n = 0; n < NR_CF; ++n) {
#endif
#if (NR_PACKED_DENSE & 4) && NR_HWAVES
        NR_ACCF a[NR_MF];
        for(int m=0;m<NR_MF;++m) a[m]=nr_project_acc[nn][m];
#else
        NR_ACCF a[NR_MF];
        for (int m = 0; m < NR_MF; ++m) a[m] = NR_ACCZERO;
#if NR_PTX_ACC
#if NR_NATIVE_RESIDUAL
        for(int m=0;m<NR_MF;++m) for(int c=0;c<8;c+=2) {
            const uint off=pc.ars_off+uint(n)*16u+NR_ROW(c);
#if NR_RESIDUAL_F32
            const vec2 residual=vec2(NR_Y(m,n,c),NR_Y(m,n,c+1))*vec2(wgt_f32[off],wgt_f32[off+2u]);
#else
            const f16vec2 scale=nr_residual_scale(off);
            const f16vec2 y=f16vec2(NR_Y(m,n,c),NR_Y(m,n,c+1));
            const f16vec2 residual=y*scale;
#endif
#if NR_F16_MMA
            a[m][c]=residual.x;a[m][c+1]=residual.y;
#else
            a[m][c]=float(residual.x);a[m][c+1]=float(residual.y);
#endif
        }
#else
        for(int m=0;m<NR_MF;++m) for(int c=0;c<8;++c)
            a[m][c]=nr_round_f16(wgt_f32[pc.ars_off+uint(n)*16u+NR_ROW(c)]*NR_Y(m,n,c));
#endif
#endif
        for (int k = 0; k < NR_CF; ++k) {
#if NR_HWAVES
            NR_FRAG_B cqk[NR_MF];
            for (int m = 0; m < NR_MF; ++m)
                NR_LOAD_B(cqk[m], lds_y, NR_LXB_
                          uint(m * NR_CF + k) * 256u + NR_OPQ(64u + uint(nn)), 16u);
#define NR_CQ(m, k) cqk[m]
#else
#define NR_CQ(m, k) cq[m][k]
#endif
#if NR_WPF
            NR_WPF_STEP(wpop, n * NR_CF + k, NR_WPF_OP_TOT, NR_WPF_OP_ADDR)
#define NR_WPF_WF_OP NR_WPF_AT(wpop, n * NR_CF + k)
#else
            NR_OPA wf;
            NR_C32_WEIGHT(wf,NR_WB_OP,uint(n),uint(k),uint(NR_CF));
#define NR_WPF_WF_OP wf
#endif
            for (int m = 0; m < NR_MF; ++m) NR_MGA(m) NR_MMA(a[m], NR_WPF_WF_OP, NR_CQ(m, k));
            if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                for (int m = 0; m < NR_MF; ++m) NR_RND(a[m])
        }
#endif
        // Again element-wise: `ars` is per accumulator row and `y` has been in
        // registers since stage 2. The block's output leaves ColumnMajor,
        // because the destination is [token][channel].
#if NR_IMGOUT_REGS
        // The even fragment of each pair, held one iteration until its odd
        // partner arrives. One fragment, not the whole 64x32 block.
        NR_FRAG_ACC16 nr_io_even;
#endif
        for (int m = 0; m < NR_MF; ++m) NR_MGA(m) {
#ifdef NR_FINAL_F16
            // The fused post block feeds this FP16 value directly to its RGB
            // projection; an FP8 store here would add a conversion absent in PTX.
            NR_FRAG_ACC16 of;
            for (int c = 0; c < 8; ++c)
                of[c] = NR_F16(a[m][c]
#if !NR_PTX_ACC
                    + wgt_f32[pc.ars_off + uint(n) * 16u + NR_ROW(c)]
                      * NR_Y(m, n, c)
#endif
                    );
#ifdef NR_FUSED_IMAGE_OUTPUT
#if NR_IO_WMMA
            {
                NR_FRAG_B16 nr_fb;
                for (int c = 0; c < 8; ++c) nr_fb[c] = of[c];
                nr_io_rgb[m] = coopMatMulAdd(nr_io_wa[n], nr_fb, nr_io_rgb[m]);
            }
#elif NR_IMGOUT_REGS
            if ((m & 1) == 0) { nr_io_even = of; }
            else {
                // A lane projects the even fragment's token in the low half-wave and the
                // odd fragment's in the high half. The token is the column, l % 16, in
                // both. On gfx11 the two lanes l and l ^ 16 of a column hold the even and
                // the odd channel of every channel pair of a fragment - component c of
                // lane l is channel 2c + l/16 - so the pair (2c, 2c+1), which is what the
                // projection consumes, is the component c of both lanes. A lane needs the
                // other half's component of *its own* fragment: the low lane (even
                // fragment) takes it from the high lane's even fragment, the high lane
                // (odd fragment) from the low lane's odd one. So lanes >= 16 offer the
                // even fragment and lanes < 16 the odd one; shuffling the own value
                // instead reads the wrong fragment on both halves.
                //
                // The channel sequence is the original's, 0..15 of this fragment in
                // pair order, so each f32 sum is the same sequence of the same products.
                for (int c = 0; c < 8; c += 2) {
                    const uint ev = packFloat2x16(f16vec2(nr_io_even[c], nr_io_even[c+1]));
                    const uint od = packFloat2x16(f16vec2(of[c], of[c+1]));
                    const f16vec2 xv = unpackFloat2x16(subgroupShuffleXor(nr_io_lo ? od : ev, 16u));
                    const f16vec2 own = unpackFloat2x16(nr_io_lo ? ev : od);
                    // component c, then c+1: (own, partner) on the low lane and
                    // (partner, own) on the high lane, channel order within the pair.
                    NR_IO_ACC(m / 2, uint(n) * 8u + uint(c),
                              packFloat2x16(nr_io_lo ? f16vec2(own.x, xv.x) : f16vec2(xv.x, own.x)))
                    NR_IO_ACC(m / 2, uint(n) * 8u + uint(c) + 1u,
                              packFloat2x16(nr_io_lo ? f16vec2(own.y, xv.y) : f16vec2(xv.y, own.y)))
                }
            }
#else
            NR_STORE_ACC_COL(of,image_features,(tok0+uint(m)*16u)*32u+uint(n)*16u,32u);
#endif
#else
            if (!NR_TOOB((tok0 + uint(m) * 16u) / 16u))
                NR_STORE_ACC_COL(of, act_f16,
                    pc.o_off / 2u + nr_tile_base((tok0 + uint(m) * 16u) / 16u)
                    + uint(n) * 256u, 16u);
#endif
#else
            NR_FRAG_E4M3 of;
            NR_F16 wide[8];
#if NR_POOL_PACKED == 3 || NR_POOL_DPP16 >= 6
            // The pre-narrowing f32 values. The shipped scalar pool reads
            // float(wide[c]), which ACO folds back to exactly these.
            float wf[8];
#endif
            for (int c = 0; c < 8; ++c) {
#if NR_POOL_PACKED == 3 || NR_POOL_DPP16 >= 6
                wf[c] = a[m][c]
#if !NR_PTX_ACC
                    + wgt_f32[pc.ars_off + uint(n) * 16u + NR_ROW(c)]
                      * NR_Y(m, n, c)
#endif
                    ;
                wide[c] = NR_F16(wf[c]);
#else
                wide[c] = NR_F16(a[m][c]
#if !NR_PTX_ACC
                    + wgt_f32[pc.ars_off + uint(n) * 16u + NR_ROW(c)]
                      * NR_Y(m, n, c)
#endif
                    );
#endif
#if NR_QUANT_PAIRED
            }
            for (int c=0;c<8;c+=2) {
                fe4m3vec2 q=nr_quant_pair(f16vec2(wide[c],wide[c+1]));
                of[c]=q.x; of[c+1]=q.y;
#else
                of[c] = nr_quant_e4m3(wide[c]);
#endif
            }

#ifdef NR_POOL_F16
            // The original DS epilogue reduces its half registers BEFORE the
            // independent FP8 skip store. No extra wide arena is needed.
#if NR_PERSIST_DS
            [[dont_flatten]] if (nr_ds_layer) {
#endif
            const uint q = (tok0 + uint(m)*16u)/16u;
            const int tx = 2*int(nr_wx)+pc.shift+int(q&1u);
            const int ty = 2*int(nr_wy)+pc.shift_y+int(q>>1u);
            const int px = tx*2 + int((lane%4u)/2u);
            const int py = ty*2 + int((lane%16u)/8u);
#if NR_POOL_VECTOR_STORE
            // Preserve scalar half arithmetic; group conversions and stores.
            // Each active lane owns the eight channels of one row parity, NR_ROW(c) = 2c + l/16,
            // so they are stored one element at a time rather than as vectors.
            fe4m3vec4 pooled[2];
#if NR_QUANT_PAIRED
            NR_F16 means[8];
#else
#endif
#if NR_POOL_DPP16
            float meansf[8];
#define NR_POOL_Q(c) nr_quant_pair32(vec2(meansf[c], meansf[(c)+1]))
#else
#define NR_POOL_Q(c) nr_quant_pair(f16vec2(means[c], means[(c)+1]))
#endif
#if NR_POOL_ADDR_UNIFORM
            // px = 2*tx + lx and py = 2*ty + ly with lx, ly in {0, 1},
            // so every bound and the 4x4 tile index are functions of the
            // wave-uniform tx/ty alone: px>=0 <=> tx>=0, px<4W <=> tx<2W,
            // px/4 = tx>>1, px%4 = 2*(tx&1) + lx. The same store, addressed in
            // SALU plus one lane constant instead of ~14 VALU per fragment.
#if NR_PRE_FULL & 4
            const bool store_pool=(lane&5u)==0u;
#else
            const bool store_pool=(lane&5u)==0u && tx>=0 && ty>=0 &&
                tx<int(pc.pool_tiles_x*2u) && ty<int(pc.pool_tiles_y*2u);
#endif
#if NR_POOL_DPP16 >= 6
#if NR_POOL_DPP16 == 7
#define NR_P6MASK true
#else
#define NR_P6MASK ((lane&4u)==0u)
#endif
#if NR_PRE_FULL & 4
            const bool store_pool6=NR_P6MASK;
#else
            const bool store_pool6=NR_P6MASK && tx>=0 && ty>=0 &&
                tx<int(pc.pool_tiles_x*2u) && ty<int(pc.pool_tiles_y*2u);
#endif
#endif
#else
            const bool store_pool=(lane&5u)==0u && px>=0 && py>=0 &&
                px<int(pc.pool_tiles_x*4u) && py<int(pc.pool_tiles_y*4u);
#endif
#if NR_POOL_PACKED && NR_QUANT_PAIRED
            // The same 2x2 mean on channel pairs. Each half add is the
            // exact sum rounded once to f16, and the x0.25 is exact, so this is
            // the scalar chain two channels per instruction.
            for (int c=0;c<8;c+=2) {
#if NR_POOL_PACKED == 3
                // `precise`: without it NIR narrows f2f16(a+b) into a half add of
                // narrowed operands, which is a different rounding.
                precise float s0 = wf[c] + subgroupShuffleXor(wf[c], 1u);
                precise float s1 = wf[c+1] + subgroupShuffleXor(wf[c+1], 1u);
                const f16vec2 pr = f16vec2(NR_F16(s0), NR_F16(s1));
#elif NR_POOL_PACKED == 2
                // Exact form: the first level stays the scalar f32 sum of
                // the same float(wide[]) values the scalar chain adds (ACO keeps
                // those unrounded), narrowed once per pair; the remaining
                // half-precision steps are the scalar chain's own roundings.
                const float a0 = float(wide[c]), a1 = float(wide[c+1]);
                const f16vec2 pr = f16vec2(a0 + subgroupShuffleXor(a0, 1u),
                                           a1 + subgroupShuffleXor(a1, 1u));
#else
                const f16vec2 v2 = f16vec2(wide[c], wide[c+1]);
                const f16vec2 pr = v2 + unpackFloat2x16(subgroupShuffleXor(packFloat2x16(v2), 1u));
#endif
                const f16vec2 sm = pr + unpackFloat2x16(subgroupShuffleXor(packFloat2x16(pr), 4u));
                const f16vec2 mn = sm * f16vec2(0.25hf);
                means[c]=mn.x; means[c+1]=mn.y;
            }
#elif NR_POOL_DPP16 >= 6
            // Mode 6, reduce-scatter: the x pair (lane ^ 1) splits the eight
            // channels - even x keeps rbase+0..3, odd x rbase+4..7 - so each
            // lane adds its four channels to the partner's four, the same two
            // f32 operands the scalar chain added; the y pair (lane ^ 4) has
            // the same split and finishes as mode 5. Lanes of even y store four
            // bytes each, where the scalar chain had one lane in four store eight.
            {
                const bool xo = (lane & 1u) != 0u;
                float pf[4];
                for (int c=0;c<4;++c) {
                    const float own = xo ? wf[c+4] : wf[c];
                    const float snd = xo ? wf[c] : wf[c+4];
                    pf[c] = own + subgroupShuffleXor(snd,1u);
                }
#if NR_POOL_DPP16 == 7
                // Mode 7: the y pair splits again - even y keeps two of the
                // four channels, odd y the other two - so every lane finishes
                // two means of its own and all 32 lanes store two bytes.
                {
                    const bool yo = (lane & 4u) != 0u;
                    const float o0 = yo ? pf[2] : pf[0], o1 = yo ? pf[3] : pf[1];
                    const float s0 = yo ? pf[0] : pf[2], s1 = yo ? pf[1] : pf[3];
                    const f16vec2 pr = f16vec2(NR_F16(o0), NR_F16(o1));
                    const f16vec2 sm = pr + f16vec2(NR_F16(subgroupShuffleXor(s0,4u)),
                                                    NR_F16(subgroupShuffleXor(s1,4u)));
                    meansf[0] = float(sm.x) * 0.25;
                    meansf[1] = float(sm.y) * 0.25;
                }
                if (false)
#endif
                for (int c=0;c<4;c+=2) {
                    const f16vec2 pr = f16vec2(NR_F16(pf[c]), NR_F16(pf[c+1]));
                    const f16vec2 sm = pr + f16vec2(NR_F16(subgroupShuffleXor(pf[c],4u)),
                                                    NR_F16(subgroupShuffleXor(pf[c+1],4u)));
                    meansf[c] = float(sm.x) * 0.25;
                    meansf[c+1] = float(sm.y) * 0.25;
                }
            }
#elif NR_POOL_DPP16 == 5
            // Mode 5: the scalar chain two channels at a time - each first-level
            // f32 sum narrowed once, then one packed half add with the pixel pair
            // four lanes away. The neighbour's f32 sum is narrowed here, as the
            // scalar chain does: with a single use ACO fuses f2f16(a + b) into
            // v_fma_mixlo (one rounding where the chain has two).
            for (int c=0;c<8;c+=2) {
                const float v0 = float(wide[c]), v1 = float(wide[c+1]);
                const float s0 = v0 + subgroupShuffleXor(v0,1u), s1 = v1 + subgroupShuffleXor(v1,1u);
                const f16vec2 pr = f16vec2(NR_F16(s0), NR_F16(s1));
                const f16vec2 sm = pr + f16vec2(NR_F16(subgroupShuffleXor(s0,4u)),
                                                NR_F16(subgroupShuffleXor(s1,4u)));
                meansf[c] = float(sm.x) * 0.25;
                meansf[c+1] = float(sm.y) * 0.25;
            }
#else
            for (int c=0;c<8;++c) {
                float v = float(wide[c]);
                NR_F16 pair = NR_F16(v + subgroupShuffleXor(v,1u));
#if NR_POOL_DPP16 & 1
                // Diagnostic: shuffling the half itself saves one instruction a
                // channel, but ACO then fuses the first level's f2f16(a+b) into
                // one `v_fma_mixlo_f16` (single rounding where this chain rounds
                // twice) even under `precise` - not byte-identical.
                NR_F16 sum = pair + subgroupShuffleXor(pair,4u);
#else
                NR_F16 sum = NR_F16(pair + NR_F16(subgroupShuffleXor(float(pair),4u)));
#endif
#if NR_POOL_DPP16
                // x0.25 taken in f32 on the way to the converter, one
                // `v_fma_mix_f32` instead of `v_mul_f16` + `v_cvt_f32_f16`. The
                // product is exact in f32; the f16 product differs only below
                // 2^-14, which e4m3 sends to the same signed zero.
                meansf[c] = float(sum) * 0.25;
#else
                NR_F16 mean = NR_F16(sum * NR_F16(0.25));
#endif
#if NR_POOL_DPP16
#elif NR_QUANT_PAIRED
                means[c]=mean;
#else
                if(store_pool)pooled[c/4][c%4]=nr_quant_e4m3(mean);
#endif
            }
#endif
#ifdef NR_DS_PROJECT
            // Every top-left lane of a 2x2 block publishes its pixel, in or out
            // of the pooled grid; out-of-grid pixels are dropped at the store.
#if NR_POOL_DPP16 == 7
            // Mode 7: every lane publishes its two channels.
            {
                const fe4m3vec2 q0=NR_POOL_Q(0);
                const uint lp = uint(2*int(q>>1u) + int((lane%16u)/8u)) * 4u
                              + uint(2*int(q&1u) + int((lane%4u)/2u));
                const uint cc = 4u*(lane&1u) + 2u*((lane>>2u)&1u);   // first component of the pair
#if NR_HWAVES
                lds_x[NR_LXB_ uint(n)*256u + lp*16u + NR_ROW(cc)] = q0.x;
                lds_x[NR_LXB_ uint(n)*256u + lp*16u + NR_ROW(cc + 1u)] = q0.y;
#else
                lds_v[lp*uint(NR_C) + uint(n)*16u + NR_ROW(cc)] = q0.x;
                lds_v[lp*uint(NR_C) + uint(n)*16u + NR_ROW(cc + 1u)] = q0.y;
#endif
            }
#elif NR_POOL_DPP16 == 6
            // Mode 6: both x lanes of a block's top row publish their four channels.
            if ((lane&4u)==0u) {
                {
                    const fe4m3vec2 q0=NR_POOL_Q(0), q1=NR_POOL_Q(2);
                    pooled[0]=fe4m3vec4(q0.x,q0.y,q1.x,q1.y);
                }
                const uint lp = uint(2*int(q>>1u) + int((lane%16u)/8u)) * 4u
                              + uint(2*int(q&1u) + int((lane%4u)/2u));
                const uint cc = 4u*(lane&1u);                        // first component of the four
#if NR_HWAVES
                for (int c=0;c<4;++c)
                    lds_x[NR_LXB_ uint(n)*256u + lp*16u + NR_ROW(cc + uint(c))] = pooled[0][c];
#else
                for (int c=0;c<4;++c)
                    lds_v[lp*uint(NR_C) + uint(n)*16u + NR_ROW(cc + uint(c))] = pooled[0][c];
#endif
            }
#else
            if ((lane&5u)==0u) {
                for (int c=0;c<8;c+=2) {
                    fe4m3vec2 q=NR_POOL_Q(c);
                    pooled[c/4][c%4]=q.x;pooled[c/4][c%4+1]=q.y;
                }
                const uint lp = uint(2*int(q>>1u) + int((lane%16u)/8u)) * 4u
                              + uint(2*int(q&1u) + int((lane%4u)/2u));
#if NR_HWAVES
                for (int c=0;c<8;++c)
                    lds_x[NR_LXB_ uint(n)*256u + lp*16u + NR_ROW(c)] = pooled[c/4][c%4];
#else
                for (int c=0;c<8;++c)
                    lds_v[lp*uint(NR_C) + uint(n)*16u + NR_ROW(c)] = pooled[c/4][c%4];
#endif
            }
#endif
            if(false) {
#else
#if NR_POOL_DPP16 == 7
            if(store_pool6) {
#elif NR_POOL_DPP16 == 6
            if(store_pool6) {
                {
                    const fe4m3vec2 q0=NR_POOL_Q(0), q1=NR_POOL_Q(2);
                    pooled[0]=fe4m3vec4(q0.x,q0.y,q1.x,q1.y);
                }
#else
            if(store_pool) {
#endif
#endif
#if NR_QUANT_PAIRED && NR_POOL_DPP16 < 6
                for (int c=0;c<8;c+=2) {
                    fe4m3vec2 q=NR_POOL_Q(c);
                    pooled[c/4][c%4]=q.x;pooled[c/4][c%4+1]=q.y;
                }
#else
#endif
#if NR_POOL_ADDR_UNIFORM
                const uint tile=uint(ty>>1)*pc.pool_tiles_x+uint(tx>>1);
                const uint uslot=uint(ty&1)*8u+uint(tx&1)*2u;
                const uint lslot=((lane%16u)/8u)*4u+(lane%4u)/2u;
                uint address=(pc.pool_off+(tile*uint(NR_CF)+uint(n))*256u+uslot*16u)
                             +lslot*16u;
#else
                uint tile=uint(py/4)*pc.pool_tiles_x+uint(px/4);
                uint slot=uint(py%4)*4u+uint(px%4);
                uint address=pc.pool_off+(tile*uint(NR_CF)+uint(n))*256u+slot*16u;
#endif
#if NR_POOL_DPP16 == 7
                {
                    const fe4m3vec2 q7=NR_POOL_Q(0);
                    const uint cc = 4u*(lane&1u) + 2u*((lane>>2u)&1u);
                    act_e4m3[address+NR_ROW(cc)]=q7.x; act_e4m3[address+NR_ROW(cc+1u)]=q7.y;
                }
#elif NR_POOL_DPP16 == 6
                for (int c=0;c<4;++c)
                    act_e4m3[address+NR_ROW(4u*(lane&1u)+uint(c))]=pooled[0][c];
#else
                for (int c=0;c<8;++c)
                    act_e4m3[address+NR_ROW(c)]=pooled[c/4][c%4];
#endif
            }
#else
            for (int c=0;c<8;++c) {
                float v = float(wide[c]);
                NR_F16 pair = NR_F16(v + subgroupShuffleXor(v,1u));
                NR_F16 sum = NR_F16(pair + NR_F16(subgroupShuffleXor(float(pair),4u)));
                NR_F16 mean = NR_F16(sum * NR_F16(0.25));
                if ((lane&5u)==0u && px>=0 && py>=0 &&
                    px<int(pc.pool_tiles_x*4u) && py<int(pc.pool_tiles_y*4u)) {
                    uint tile = uint(py/4)*pc.pool_tiles_x+uint(px/4);
                    uint slot = uint(py%4)*4u+uint(px%4);
                    uint ch = uint(n)*16u+NR_ROW(c);
                    act_e4m3[pc.pool_off+(tile*uint(NR_CF)+ch/16u)*256u+slot*16u+ch%16u]
                        = nr_quant_e4m3(mean);
                }
            }
#endif
#if NR_PERSIST_DS
            }
#endif
#endif
            // Window-major mode writes canonical [token][channel], which is what
            // every gold in the corpus is scored against after a host transform.
            // Image mode writes **the same tile-blocked form it read**, so the
            // next layer's NR_LOAD_B consumes it with no transform at all - the
            // property that makes a graph possible. Same fragment, same store,
            // only the base and the stride differ: ColumnMajor with stride 16
            // puts element (channel i, token j) at `block + j*16 + i`, which is
            // `tile_blocked`'s `(r%16)*16 + (k%16)`.
#if NR_IMAGE
            if (!NR_TOOB((tok0 + uint(m) * 16u) / 16u))
            NR_STORE_ACC_COL(of, act_e4m3,
                             pc.o_off + nr_tile_base((tok0 + uint(m) * 16u) / 16u)
                             + uint(n) * 256u, 16u);
#else
            NR_STORE_ACC_COL(of, act_e4m3, pc.o_off + wbase
                             + (tok0 + uint(m) * 16u) * uint(NR_C) + uint(n) * 16u,
                             uint(NR_C));
#endif
#endif
        }
    }
#ifdef NR_DS_PROJECT
    // The downsample's learned projection on the window's 16 pooled
    // pixels, out^T[n][px] = W[n][k] . pooled^T[k][px] - the same e4m3 inputs,
    // the same ascending k order and the same f16-then-e4m3 epilogue as the
    // gemmds dispatch it replaces, stored through gemmds's own shear map.
#if NR_PERSIST_DS
    [[dont_flatten]] if (nr_ds_layer) {
#endif
    NR_BODY_BARRIER();
    {
#if !NR_HWAVES
        NR_FRAG_B dsb[NR_CF];
        for (int kf = 0; kf < NR_CF; ++kf)
            NR_LOAD_B(dsb[kf], lds_v, uint(kf) * 16u, uint(NR_C));
#endif
        const int px0 = (2*int(nr_wx)+pc.shift)*2, py0 = (2*int(nr_wy)+pc.shift_y)*2;
        const int spx_i = px0 + int((lane % 16u) % 4u), spy_i = py0 + int((lane % 16u) / 4u);
        const bool in_grid = spx_i >= 0 && spy_i >= 0 &&
            spx_i < int(pc.pool_tiles_x * 4u) && spy_i < int(pc.pool_tiles_y * 4u);
        const uint spx = uint(max(spx_i, 0)), spy = uint(max(spy_i, 0));
        const uint writer_rows = pc.ds_writer_rows == 0u ? pc.ds_rows : pc.ds_writer_rows;
#if NR_DS_IDENTITY_FAST
        const bool ds_identity = pc.ds_mode != 2u && pc.ds_raster == pc.ds_crow && writer_rows == pc.ds_rows;
#endif
#if NR_HWAVES
        // Each head-wave owns 2*NR_DF of the 2C output fragments.
        for (int ol = 0; ol < 2 * NR_DF; ++ol) {
            const int of = nrhw_h * 2 * NR_DF + ol;
#else
        // Two waves share the 2C output fragments; both read the whole pooled block.
        for (int ofl = 0; ofl < 2 * NR_CF / NR_FWAVES; ++ofl) {
            const int of = int(wave) * (2 * NR_CF / NR_FWAVES) + ofl;
#endif
            NR_ACCF acc = NR_ACCZERO;
            for (int kf = 0; kf < NR_CF; ++kf) {
                NR_OPA wf;
                NR_LOAD_A(wf, NR_WARENA, pc.ds_w_off + uint(of * NR_CF + kf) * 256u, 16u);
#if NR_HWAVES
                NR_FRAG_B dsbk;
                NR_LOAD_B(dsbk, lds_x, NR_LXB_ uint(kf) * 256u + NR_OPQ(96u), 16u);
                NR_MMA(acc, wf, dsbk);
#else
                NR_MMA(acc, wf, dsb[kf]);
#endif
            }
            NR_F16 outv[8];
            for (int c = 0; c < 8; ++c) outv[c] = NR_F16(acc[c]);
            const uint sg = uint(of);
#if NR_DS_IDENTITY_FAST
            // With raster == crow and writer_rows == rows (every C>=64 host-
            // boundary layer, and C32 at 1080p/4K) the shear map is the identity
            // for in-range pixels: linear = (sg*rows + spy)*crow + spx with
            // spx < crow and spy < rows. Out-of-range pixels are suppressed by
            // s_oob either way, so only in-range values matter.
            uint dst_group, sdx, sdy;
#if NR_DS_ID_NOFLAT
            // the general shear behind a real branch (a never-true store keeps
            // ACO from flattening it); flattened, every window paid its three
            // integer divisions and the selects.
            [[dont_flatten]] if (ds_identity) {
#else
            if (ds_identity) {
#endif
                dst_group = sg; sdx = spx; sdy = spy;
            } else {
                const uint linear = pc.ds_mode == 2u ? (sg * pc.ds_rows + spy) * pc.ds_crow + spx
                    : (sg * writer_rows + spy) * pc.ds_raster + spx;
                dst_group = linear / (pc.ds_crow * pc.ds_rows);
                const uint pixel = linear % (pc.ds_crow * pc.ds_rows);
                sdx = pixel % pc.ds_crow; sdy = pixel / pc.ds_crow;
#if NR_DS_ID_NOFLAT
                if (gl_NumWorkGroups.x == 0xffffffffu) act_e4m3[pc.o_off] = NR_E4M3(0.0);
#endif
            }
#else
            const uint linear = pc.ds_mode == 2u ? (sg * pc.ds_rows + spy) * pc.ds_crow + spx
                : (sg * writer_rows + spy) * pc.ds_raster + spx;
            const uint dst_group = linear / (pc.ds_crow * pc.ds_rows);
            const uint pixel = linear % (pc.ds_crow * pc.ds_rows);
            const uint sdx = pixel % pc.ds_crow, sdy = pixel / pc.ds_crow;
#endif
            const bool s_oob = (pc.ds_mode != 2u && (spy >= writer_rows || spx >= pc.ds_raster)) ||
                sdx >= pc.ds_otx * 4u || dst_group >= pc.ds_n / 16u;
            if (pc.ds_writer_rows != 0u && (sdy >= pc.ds_writer_rows || sdx >= pc.ds_raster))
                for (int c = 0; c < 8; ++c) outv[c] = NR_F16(0.0);
            // The padding fix (gemm1x1.comp clear_x/clear_y) - native
            // writes the view's padded tokens outside the real pooled extent as 0.
            if (pc.ds_clear_x != 0u && (sdx >= pc.ds_clear_x || sdy >= pc.ds_clear_y))
                for (int c = 0; c < 8; ++c) outv[c] = NR_F16(0.0);
            // The element index of this lane's token (channel 0) in the output tile; the
            // channel of component c is NR_ROW(c).
            const uint ob = pc.ds_o_off
                            + (((sdy / 4u) * pc.ds_otx + sdx / 4u) * (pc.ds_n / 16u) + dst_group) * 256u
                            + (4u * (sdy % 4u) + sdx % 4u) * 16u;
            if (in_grid && !s_oob) {
#if NR_DS_QPAIR
                // The same per-component mode-4 conversion, two at a time.
                const fe4m3vec2 q0 = nr_quant_pair(f16vec2(outv[0], outv[1]));
                const fe4m3vec2 q1 = nr_quant_pair(f16vec2(outv[2], outv[3]));
                const fe4m3vec2 q2 = nr_quant_pair(f16vec2(outv[4], outv[5]));
                const fe4m3vec2 q3 = nr_quant_pair(f16vec2(outv[6], outv[7]));
                act_e4m3[ob + NR_ROW(0)] = q0.x; act_e4m3[ob + NR_ROW(1)] = q0.y;
                act_e4m3[ob + NR_ROW(2)] = q1.x; act_e4m3[ob + NR_ROW(3)] = q1.y;
                act_e4m3[ob + NR_ROW(4)] = q2.x; act_e4m3[ob + NR_ROW(5)] = q2.y;
                act_e4m3[ob + NR_ROW(6)] = q3.x; act_e4m3[ob + NR_ROW(7)] = q3.y;
#else
                for (int c = 0; c < 8; ++c) act_e4m3[ob + NR_ROW(c)] = nr_quant_e4m3(outv[c]);
#endif
            }
        }
    }
#if NR_PERSIST_DS
    }
#endif
#endif
