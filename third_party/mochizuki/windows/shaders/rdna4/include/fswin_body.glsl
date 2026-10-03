// The per-item body of fswin_t.comp, moved here unchanged so that the
// persistent kernels can instantiate it once per live-tile mask (NR_EDGE_BODIES).
// Everything it declares is local to one instantiation. NR_LIVE (set by the
// includer) is the compile-time mask of the window's in-image tiles; NR_MG,
// NR_MGA and NR_MGK read it.
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
    // The accumulator's row for component c, from the measured map. Every
    // per-output-channel scalar here - rs, ars - is indexed by it.
    const uint rbase = 8u * (lane / 16u);
#if NR_ACTIVATION_LUT
    for(uint i=tid*16u;i<NR_ACTIVATION_LUT_SIZE;i+=uint(NR_THREADS)*16u) {
        fe4m3vec4 v[4];
        for(int j=0;j<4;++j) v[j]=act_lut4[(pc.rsd_off+i)/4u+uint(j)];
        for(int j=0;j<16;++j) nr_act_lds[i+uint(j)]=v[j/4][j%4];
    }
    barrier();
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
        for(int j=0;j<8;++j)
            upsrc[k][j]=NR_ACT_B8(pc.blend_p_off+(ups_tile*4u+uint(k))*256u+ups_in_slot*16u+rbase, j);
        for(int n=0;n<2;++n) {
            NR_FRAG_A w;
            NR_LOAD_A(w,wgt_e4m3,pc.ups_weight_off+uint(n*4+k)*256u,16u);
            NR_MMA(uacc[n],w,upsrc[k]);
        }
    }
    for(int n=0;n<2;++n) {
        NR_FRAG_ACC16 half_result;
        for(int j=0;j<8;++j)half_result[j]=NR_F16(uacc[n][j]);
        NR_UPS_STORE(half_result,uint(n)*256u);
    }
#else
    for(int k=0;k<4;++k)
        for(int j=0;j<8;++j)
            upsrc[k][j]=NR_ACT_B8(pc.blend_p_off+(ups_tile*4u+uint(k))*256u+ups_in_slot*16u+rbase, j);
    for(int n=0;n<2;++n) {
        NR_FRAG_ACC acc=NR_ACC_ZERO;
        for(int k=0;k<4;++k) {
            NR_FRAG_A w;
            NR_LOAD_A(w,wgt_e4m3,pc.ups_weight_off+uint(n*4+k)*256u,16u);
            NR_MMA(acc,w,upsrc[k]);
        }
        NR_FRAG_ACC16 half_result;
        for(int j=0;j<8;++j)half_result[j]=NR_F16(acc[j]);
        NR_UPS_STORE(half_result,uint(n)*256u);
    }
#endif
    barrier();
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
        for(int j=0;j<8;++j)
            us[j]=NR_ACT_B8(pc.blend_p_off+(ups_tile*uint(2*NR_CF)+uint(k))*256u+ups_in_slot*16u+rbase, j);
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
        NR_UPS_STORE(half_result,nf*256u);
    }
#else
    for(int k=0;k<2*NR_CF;++k)
        for(int j=0;j<8;++j)
#ifdef NR_WIDE_UPS_VIEW
        {
            const uint token=ups_tile*16u+ups_in_slot;
            const uint ch=uint(k)*16u+rbase+uint(j);
            if(pc.blend_o_off==0u) {
                upsrc[k][j]=NR_ACT_B8(pc.blend_p_off+(ups_tile*uint(2*NR_CF)+uint(k))*256u+ups_in_slot*16u+rbase, j);
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
            upsrc[k][j]=NR_ACT_B8(pc.blend_p_off+(ups_tile*uint(2*NR_CF)+uint(k))*256u+ups_in_slot*16u+rbase, j);
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
        NR_UPS_STORE(half_result,nf*256u);
    }
#endif
    barrier();
#if NR_PERSIST_UPS
    }
#endif
#endif

    // ---- stage 1: e = act(E . x) -----------------------------------------
    NR_PROF_STAGE(1)
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
    NR_FRAG_B16 xh[NR_MF][NR_CF];
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
                NR_FRAG_B src;
                NR_LOAD_B_ACT(src, tbase[m] + k * 256u, 16u);
                NR_FRAG_E4M3 dst;
                for (int j = 0; j < 8; ++j) dst[j] = src[j];
                if (nr_tile_oob(uint(m)))
                    for (int j = 0; j < 8; ++j) dst[j] = NR_E4M3(0.0);
                NR_STORE_ACC_COL(dst, lds_x, NR_LXB_ (uint(m) * uint(NR_CF) + k) * 256u, 16u);
            }
    } else
#endif
    for (int m = 0; m < NR_MF; ++m)
        for (int d = 0; d < NR_DF; ++d) {
            const uint k = uint(nrhw_h * NR_DF + d);
            NR_FRAG_B src;
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
#if NR_UPS_BLEND_PK >= 2 && defined(NR_WIDE_UPS_PROJECT)
            // NR_UPS_BLEND_PK=2: the head-split blend two channels an instruction.
            // e4m3 -> f16 is exact, and ACO already contracted sv*g + pv into one
            // v_fma_f16 a channel, which v_pk_fma_f16 rounds the same way per half.
            {
                const uint saddr=pc.blend_s_off+(stile*uint(NR_CF)+k)*256u+slot*16u+rbase;
                const fe4m3vec4 sv4[2]={NR_BLEND_X4(saddr/4u), NR_BLEND_X4(saddr/4u+1u)};
                const uint local_slot=((q>>1u)*2u+dy/2u)*4u+(q&1u)*2u+dx/2u;
                for(int j=0;j<8;j+=2) {
                    const uint ch=k*16u+rbase+uint(j);
                    const uint pb=k*256u+local_slot*16u+rbase+uint(j);
                    const f16vec2 pv2=NR_UPS_PAIR(pb);
                    const f16vec2 sv2=unpackFloat2x16(packHalf2x16(nr_e4f2_pick(vec4(sv4[j/4]),j%4)));
#if NR_BLEND_G32
                    const f16vec2 g2=unpackFloat2x16(wgt_u32[(pc.blend_g_off+ch)>>1u]);
#else
                    const f16vec2 g2=f16vec2(wgt_f16[pc.blend_g_off+ch],wgt_f16[pc.blend_g_off+ch+1u]);
#endif
                    const fe4m3vec2 qv=nr_quant_pair(fma(sv2,g2,pv2));
                    src[j]=qv.x; src[j+1]=qv.y;
                }
            }
            if(false)
#endif
            {
            NR_F16 blend_h[8];
            for(int j=0;j<8;++j) {
                const uint ch=k*16u+rbase+uint(j);
#ifdef NR_WIDE_UPS_PROJECT
                const uint local_slot=((q>>1u)*2u+dy/2u)*4u+(q&1u)*2u+dx/2u;
                NR_F16 pv=NR_UPS_ONE(k*256u+local_slot*16u+rbase+uint(j));
#else
                NR_F16 pv=act_f16[pc.blend_p_off/2u+(itile*uint(NR_CF)+k)*256u+islot*16u+rbase+uint(j)];
#endif
                NR_F16 sv=NR_F16(NR_ACT_B8(pc.blend_s_off+(stile*uint(NR_CF)+k)*256u+slot*16u+rbase, j));
                NR_F16 sp=NR_F16(sv*wgt_f16[pc.blend_g_off+ch]);
                // Native wide path quantizes the half blend before residual/MLP.
                blend_h[j]=NR_F16(pv+sp);
            }
            // Windows: quantised four at a time (nr_quant4_h), the same bytes.
            for(int j=0;j<8;j+=4) {
                const fe4m3vec4 q4=nr_quant4_h(f16vec4(blend_h[j],blend_h[j+1],blend_h[j+2],blend_h[j+3]));
                src[j]=q4.x;src[j+1]=q4.y;src[j+2]=q4.z;src[j+3]=q4.w;
            }
            }
#else
            NR_LOAD_B_ACT(src, tbase[m] + k * 256u, 16u);
#endif
            NR_FRAG_E4M3 dst;
            for (int j = 0; j < 8; ++j) dst[j] = src[j];
#if NR_IMAGE
            // The same zero fill the per-component path does at line 615.
            if (nr_tile_oob(uint(m)))
                for (int j = 0; j < 8; ++j) dst[j] = NR_E4M3(0.0);
#endif
            NR_STORE_ACC_COL(dst, lds_x, NR_LXB_ (uint(m) * uint(NR_CF) + k) * 256u, 16u);
        }
    barrier();
#else
    for (int m = 0; m < NR_MF; ++m)
        for (int k = 0; k < NR_CF; ++k) {
#ifdef NR_INPUT_F16
#ifdef NR_FUSED_IMAGE_INPUT
            NR_FRAG_A16 lift;
            NR_LOAD_A(lift,wgt_f16,pc.image_lift_off+uint(k)*256u,16u);
            NR_FRAG_ACC lifted=NR_ACC_ZERO;NR_MMA(lifted,lift,image_features[m]);
            for(uint j=0u;j<8u;++j)xh[m][k][j]=NR_F16(lifted[j]);
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
#if NR_UPS_BLEND_PK
            // The scalar chain below compiles to one v_fma_f16 a channel
            // (ACO contracts sv*g into the add); v_pk_fma_f16 rounds the same
            // way per half. e4m3 -> f16 is exact, so the pair pack loses nothing.
            {
                const uint saddr=pc.blend_s_off+(stile*uint(NR_CF)+uint(k))*256u+slot*16u+rbase;
                const fe4m3vec4 sv4[2]={NR_BLEND_X4(saddr/4u), NR_BLEND_X4(saddr/4u+1u)};
                for(int j=0;j<8;j+=2) {
                    const uint ch=uint(k)*16u+rbase+uint(j);
#ifdef NR_FUSED_UPS_PROJECT
                    const uint local_slot=((q>>1u)*2u+dy/2u)*4u+(q&1u)*2u+dx/2u;
                    const uint pb=uint(k)*256u+local_slot*16u+rbase+uint(j);
                    const f16vec2 pv2=NR_UPS_PAIR(pb);
#else
                    const uint pb=pc.blend_p_off/2u+(itile*uint(NR_CF)+uint(k))*256u+islot*16u+rbase+uint(j);
                    const f16vec2 pv2=f16vec2(act_f16[pb],act_f16[pb+1u]);
#endif
                    const f16vec2 sv2=unpackFloat2x16(packHalf2x16(nr_e4f2_pick(vec4(sv4[j/4]),j%4)));
#if NR_BLEND_G32
                    const f16vec2 g2=unpackFloat2x16(wgt_u32[(pc.blend_g_off+ch)>>1u]);
#else
                    const f16vec2 g2=f16vec2(wgt_f16[pc.blend_g_off+ch],wgt_f16[pc.blend_g_off+ch+1u]);
#endif
                    const f16vec2 xh2=fma(sv2,g2,pv2);
                    xh[m][k][j]=xh2.x; xh[m][k][j+1]=xh2.y;
                }
            }
            if(false)
#endif
            for(int j=0;j<8;++j) {
                const uint ch=uint(k)*16u+rbase+uint(j);
#ifdef NR_FUSED_UPS_PROJECT
                const uint local_slot=((q>>1u)*2u+dy/2u)*4u+(q&1u)*2u+dx/2u;
                NR_F16 pv=NR_UPS_ONE(uint(k)*256u+local_slot*16u+rbase+uint(j));
#else
                NR_F16 pv=act_f16[pc.blend_p_off/2u+(itile*uint(NR_CF)+uint(k))*256u+islot*16u+rbase+uint(j)];
#endif
                NR_F16 sv=NR_F16(NR_ACT_B8(pc.blend_s_off+(stile*uint(NR_CF)+uint(k))*256u+slot*16u+rbase, j));
                NR_F16 sp=NR_F16(sv*wgt_f16[pc.blend_g_off+ch]);
                xh[m][k][j]=NR_F16(pv+sp);
            }
#elif defined(NR_FUSED_POST_BLEND)
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
#if NR_BLEND_VECTOR_LOAD
            // The eight channels belong to one aligned half-fragment. Fetch
            // them as two dwords instead of eight independent byte loads.
            const uint paddr=pc.blend_p_off+(itile*uint(NR_CF)+uint(k))*256u+islot*16u+rbase;
            const uint saddr=pc.blend_s_off+(stile*uint(NR_CF)+uint(k))*256u+slot*16u+rbase;
            fe4m3vec4 pv4[2], sv4[2];
            for (int v=0;v<2;++v) {
                pv4[v]=NR_BLEND_X4(paddr/4u+uint(v));
                sv4[v]=NR_BLEND_X4(saddr/4u+uint(v));
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
                const f16vec2 pv2=unpackFloat2x16(packHalf2x16(nr_e4f2_pick(vec4(pv4[j/4]),j%4)));
                const f16vec2 sv2=unpackFloat2x16(packHalf2x16(nr_e4f2_pick(vec4(sv4[j/4]),j%4)));
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
                xh[m][k][j]=xh2.x; xh[m][k][j+1]=xh2.y;
            }
            if(false)
#endif
            for(int j=0;j<8;++j) {
                const uint ch=uint(k)*16u+rbase+uint(j);
#if NR_BLEND_VECTOR_LOAD
                const NR_F16 pv=NR_F16(pv4[j/4][j%4]);
                const NR_F16 sv=NR_F16(sv4[j/4][j%4]);
#else
                const NR_F16 pv=NR_F16(NR_ACT_B8(pc.blend_p_off+(itile*uint(NR_CF)+uint(k))*256u+islot*16u+rbase, j));
                const NR_F16 sv=NR_F16(NR_ACT_B8(pc.blend_s_off+(stile*uint(NR_CF)+uint(k))*256u+slot*16u+rbase, j));
#endif
                NR_F16 mp=NR_F16(pv*wgt_f16[pc.blend_g_off+uint(NR_C)+ch]);
                NR_F16 sp=NR_F16(sv*wgt_f16[pc.blend_g_off+ch]);
                xh[m][k][j]=NR_F16(mp+sp);
            }
#else
            NR_LOAD_B(xh[m][k], act_f16,
                      pc.x_off / 2u + tbase[m] - pc.x_off + uint(k) * 256u, 16u);
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
            [[dont_flatten]] if(nr_tile_oob((tok0+uint(m)*16u)/16u))
#endif
                for (int j=0;j<8;++j) xh[m][k][j]=NR_F16(0.0);
#endif
            for (int j=0;j<8;++j) {
#if !NR_OOB_BRANCH
                if(nr_tile_oob((tok0+uint(m)*16u)/16u)) xh[m][k][j]=NR_F16(0.0);
#endif
#if NR_QUANT_PAIRED
            }
            for (int j=0;j<8;j+=2) {
                fe4m3vec2 q=NR_QP_XB(f16vec2(xh[m][k][j],xh[m][k][j+1]));
                xb[m][k][j]=q.x; xb[m][k][j+1]=q.y;
#else
                xb[m][k][j]=nr_quant_e4m3(xh[m][k][j]);
#endif
            }
#else
            NR_LOAD_B_ACT(xb[m][k], tbase[m] + uint(k) * 256u, 16u);
#endif
#if NR_IMAGE
#if NR_OOB_BRANCH && defined(NR_INPUT_F16)
            // xh is already zero there, and e4m3(+0) is +0.
#elif NR_OOB_BRANCH
            [[dont_flatten]] if(nr_tile_oob((tok0+uint(m)*16u)/16u))
                for(int j=0;j<8;++j)xb[m][k][j]=NR_E4M3(0.0);
#else
            if(nr_tile_oob((tok0+uint(m)*16u)/16u))
                for(int j=0;j<8;++j)xb[m][k][j]=NR_E4M3(0.0);
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
#if NR_EXPAND_GROUP != 2 && NR_EXPAND_GROUP != 4
#error "NR_EXPAND_GROUP must be 2 or 4"
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
#if NR_ESPLIT
        // Windows (LLPC) NR_ESPLIT: the expansion over the token fragments in two halves, half the
        // accumulators live (LLVM hoists every k step's loads ahead of the first WMMA, so this was
        // the register peak of the DS runs). Each accumulator sees the same k order; the weight
        // pairs are read once a half.
        for (int h = 0; h < NR_HGF; h += NR_EXPAND_GROUP) {
        for (int mh = 0; mh < NR_MF; mh += NR_MF / 2) {
            NR_ACCF a[NR_EXPAND_GROUP][NR_MF];
            for (int p = 0; p < NR_EXPAND_GROUP; ++p)
                for (int m = mh; m < mh + NR_MF / 2; ++m) a[p][m] = NR_ACCZERO;
            for (int k = 0; k < NR_CF; ++k) {
                NR_FRAG_B xbk[NR_MF];
                for (int m = mh; m < mh + NR_MF / 2; ++m)
                    NR_LOAD_B(xbk[m], lds_x, NR_LXB_
                              uint(m * NR_CF + k) * 256u + NR_OPQ(1u + uint(h)), 16u);
#if defined(NR_PACKED_EXPAND)
#if NR_ACC_F16 != 0 || NR_F16_MMA
#error "paired expansion weights require FP32 with FP8 operands"
#endif
                for(int p=0;p<NR_EXPAND_GROUP;p+=2) {
                    const uint q=pc.e_off/4u + uint((g*NR_HGF+h+p)/2)*uint(NR_CF)*128u
                        + uint(k)*128u + lane*4u;
                    NR_OPA wf0,wf1;
                    { NR_WPAIR_FILL2S(wf0,wf1,q,1) }
                    for(int m=mh;m<mh+NR_MF/2;++m) NR_MG(m) NR_MMA(a[p][m],wf0,xbk[m]);
                    for(int m=mh;m<mh+NR_MF/2;++m) NR_MG(m) NR_MMA(a[p+1][m],wf1,xbk[m]);
                }
#else
                for (int p = 0; p < NR_EXPAND_GROUP; ++p) {
                    NR_OPA wf;
                    NR_LOAD_WA(wf,
                              NR_TILE(pc.e_off, uint(g * NR_HGF + h + p) * 16u,
                                      uint(k) * 16u, uint(NR_C)), 16u);
                    for (int m = mh; m < mh + NR_MF / 2; ++m) NR_MMA(a[p][m], wf, xbk[m]);
                }
#endif
                if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                    for (int p = 0; p < NR_EXPAND_GROUP; ++p)
                        for (int m = mh; m < mh + NR_MF / 2; ++m) NR_RND(a[p][m])
            }
            for (int p = 0; p < NR_EXPAND_GROUP; ++p)
            for (int m = mh; m < mh + NR_MF / 2; ++m) NR_MG(m)
#if NR_ACTIVATION_LUT
                for(int c=0;c<8;c+=2) {
                    const fe4m3vec2 qp=nr_act_lookup(NR_N2_EQ(a[p][m][c],a[p][m][c+1]));
                    eqg[m][h+p][c]=qp.x;eqg[m][h+p][c+1]=qp.y;
                }
#elif NR_ACT_F32
                for (int c = 0; c < 8; c += 2) {
                    const fe4m3vec2 qp = nr_quant_pair32(vec2(
                        NR_ACTP(a[p][m][c], c), NR_ACTP_B(a[p][m][c + 1], c + 1)));
                    eqg[m][h + p][c] = qp.x;
                    eqg[m][h + p][c + 1] = qp.y;
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
                    eqg[m][h + p][c] = qp.x;
                    eqg[m][h + p][c + 1] = qp.y;
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
                    eqg[m][h + p][c] = qp.x;
                    eqg[m][h + p][c + 1] = qp.y;
                }
#else
                for (int c = 0; c < 8; ++c)
                    eqg[m][h + p][c] = nr_quant_e4m3(NR_F16(nr_act(a[p][m][c])));
#endif
#endif
        }
        }
#else
        for (int h = 0; h < NR_HGF; h += NR_EXPAND_GROUP) {
            NR_ACCF a[NR_EXPAND_GROUP][NR_MF];
            for (int p = 0; p < NR_EXPAND_GROUP; ++p)
                for (int m = 0; m < NR_MF; ++m) a[p][m] = NR_ACCZERO;
            for (int k = 0; k < NR_CF; ++k) {
                NR_FRAG_B xbk[NR_MF];
                for (int m = 0; m < NR_MF; ++m)
                    NR_LOAD_B(xbk[m], lds_x, NR_LXB_
                              uint(m * NR_CF + k) * 256u + NR_OPQ(1u + uint(h)), 16u);
#if defined(NR_PACKED_EXPAND)
#if NR_ACC_F16 != 0 || NR_F16_MMA
#error "paired expansion weights require FP32 with FP8 operands"
#endif
                for(int p=0;p<NR_EXPAND_GROUP;p+=2) {
                    const uint q=pc.e_off/4u + uint((g*NR_HGF+h+p)/2)*uint(NR_CF)*128u
                        + uint(k)*128u + lane*4u;
                    NR_OPA wf0,wf1;
                    { NR_WPAIR_FILL2S(wf0,wf1,q,1) }
                    for(int m=0;m<NR_MF;++m) NR_MG(m) NR_MMA(a[p][m],wf0,xbk[m]);
                    for(int m=0;m<NR_MF;++m) NR_MG(m) NR_MMA(a[p+1][m],wf1,xbk[m]);
                }
#else
                for (int p = 0; p < NR_EXPAND_GROUP; ++p) {
                    NR_OPA wf;
                    NR_LOAD_WA(wf,
                              NR_TILE(pc.e_off, uint(g * NR_HGF + h + p) * 16u,
                                      uint(k) * 16u, uint(NR_C)), 16u);
                    for (int m = 0; m < NR_MF; ++m) NR_MMA(a[p][m], wf, xbk[m]);
                }
#endif
                if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                    for (int p = 0; p < NR_EXPAND_GROUP; ++p)
                        for (int m = 0; m < NR_MF; ++m) NR_RND(a[p][m])
            }
            for (int p = 0; p < NR_EXPAND_GROUP; ++p)
            for (int m = 0; m < NR_MF; ++m) NR_MG(m)
#if NR_ACTIVATION_LUT
                for(int c=0;c<8;c+=2) {
                    const fe4m3vec2 qp=nr_act_lookup(NR_N2_EQ(a[p][m][c],a[p][m][c+1]));
                    eqg[m][h+p][c]=qp.x;eqg[m][h+p][c+1]=qp.y;
                }
#elif NR_ACT_F32
                for (int c = 0; c < 8; c += 2) {
                    const fe4m3vec2 qp = nr_quant_pair32(vec2(
                        NR_ACTP(a[p][m][c], c), NR_ACTP_B(a[p][m][c + 1], c + 1)));
                    eqg[m][h + p][c] = qp.x;
                    eqg[m][h + p][c + 1] = qp.y;
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
                    eqg[m][h + p][c] = qp.x;
                    eqg[m][h + p][c + 1] = qp.y;
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
                    eqg[m][h + p][c] = qp.x;
                    eqg[m][h + p][c + 1] = qp.y;
                }
#else
                for (int c = 0; c < 8; ++c)
                    eqg[m][h + p][c] = nr_quant_e4m3(NR_F16(nr_act(a[p][m][c])));
#endif
#endif
        }
#endif
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
                NR_LOAD_WA(wf,
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
                    eqg[m][h][c] = qp.x;
                    eqg[m][h][c + 1] = qp.y;
                }
#elif NR_ACT_PACKED
#if NR_QBATCH_ON
                NR_QBLOCK8(eqg[m][h], a[m])
#else
                for (int c = 0; c < 8; c += 2) {
                    const f16vec2 v = nr_act2(NR_N2_EQ(a[m][c], a[m][c + 1]));
#if NR_QUANT_PAIRED
                    fe4m3vec2 qp = nr_quant_pair(v);
                    eqg[m][h][c] = qp.x;
                    eqg[m][h][c + 1] = qp.y;
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
                    eqg[m][h][c] = qp.x;
                    eqg[m][h][c + 1] = qp.y;
                }
#else
                for (int c = 0; c < 8; ++c)
                    eqg[m][h][c] = nr_quant_e4m3(NR_F16(nr_act(a[m][c])));
#endif
#endif
        }
#endif
#if NR_ACTIVATION_LUT && NR_HWAVES
        barrier(); // Retire every head's lookup before middle outputs reuse lds_y.
#endif
#if (NR_PACKED_DENSE & 1) && NR_HWAVES
#if NR_DF != 2 || NR_ACC_F16 != 0 || NR_F16_MMA
#error "paired dense weights require the FP32 head-split path"
#endif
        NR_ACCF nr_mid_acc[NR_DF][NR_MF];
        for(int n=0;n<NR_DF;++n) for(int m=0;m<NR_MF;++m) nr_mid_acc[n][m]=NR_ACCZERO;
        for(int k=0;k<NR_HGF;++k) {
            NR_OPA wf0,wf1;
            NR_WEIGHT_PAIR_S(wf0,wf1,pc.mid_off,uint(g*NR_DF),uint(k),uint(NR_HGF),2)
            for(int m=0;m<NR_MF;++m) NR_MG(m) NR_MMA(nr_mid_acc[0][m],wf0,eqg[m][k]);
            for(int m=0;m<NR_MF;++m) NR_MG(m) NR_MMA(nr_mid_acc[1][m],wf1,eqg[m][k]);
        }
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
                NR_LOAD_WA(wf,
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
    barrier();                          // the middle output is now everyone's
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
            const f16vec2 x=f16vec2(xh[m][n][c],xh[m][n][c+1]);
#else
            const f16vec2 x=NR_N2_XB(xb[m][n][c],xb[m][n][c+1]);
#endif
            const uint off=pc.rs_off+uint(n)*16u+rbase+uint(c);
            const f16vec2 residual=x*nr_residual_scale(off);
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
    for (int h = 0; h < NR_HF; ++h) {
        NR_ACCF a[NR_MF];
        for (int m = 0; m < NR_MF; ++m) a[m] = NR_ACCZERO;
        for (int k = 0; k < NR_CF; ++k) {
#if NR_WPF
            NR_WPF_STEP(wpe, h * NR_CF + k, NR_WPF_E_TOT, NR_WPF_E_ADDR)
#define NR_WPF_WF_E NR_WPF_AT(wpe, h * NR_CF + k)
#else
            NR_OPA wf;
            NR_C32_WEIGHT(wf,pc.e_off,uint(h),uint(k),uint(NR_CF));
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
                NR_EQ(m,h)[c]=qp.x;NR_EQ(m,h)[c+1]=qp.y;
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
                NR_EQ(m,h)[c] = qp.x;
                NR_EQ(m,h)[c + 1] = qp.y;
            }
#elif NR_ACT_PACKED
#if NR_QBATCH_ON
            NR_QBLOCK8(NR_EQ(m,h), a[m])
#else
            for (int c = 0; c < 8; c += 2) {
                const f16vec2 v = nr_act2(NR_N2_EQ(a[m][c], a[m][c + 1]));
#if NR_QUANT_PAIRED
                NR_QPAIR_T qp = NR_QP_EQ(v);
                NR_EQ(m,h)[c] = qp.x;
                NR_EQ(m,h)[c + 1] = qp.y;
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
                NR_EQ(m,h)[c] = qp.x;
                NR_EQ(m,h)[c + 1] = qp.y;
            }
#else
            for (int c = 0; c < 8; ++c)
                NR_EQ(m,h)[c] = nr_quant_e4m3(NR_F16(nr_act(a[m][c])));
#endif
#endif
#if NR_STREAM_C32
        for(int n=0;n<NR_CF;++n) {
            NR_OPA cw;
            NR_C32_WEIGHT(cw,pc.ct_off,uint(n),uint(h),uint(NR_HF));
            for(int m=0;m<NR_MF;++m) NR_MMA(stream_contract[n][m],cw,stream_eq[m]);
        }
#endif
    }
#define NR_CT_SRC(m, k) NR_EQ(m,k)
#endif

#if NR_ACTIVATION_LUT && NR_C == 32
    barrier(); // All lookup readers retire before K/V overwrite the table.
#endif
    // ---- stage 2: y = Ct . q(e or m) + rs * x -----------------------------
    NR_PROF_STAGE(2)
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
#define NR_YQ(m, n) yqw[m]
#else
#define NR_YQ(m, n) yq[m][NR_YN]
#endif
#if NR_HEADS > 1
    // At heads > 1 the MLP residual is requantised, so the value the attention
    // skip adds back *is* the quantised one - there is nothing wide to keep.
    // Holding it separately costs 64 VGPRs of long-lived state at C=256, which
    // is where the widest level was losing its occupancy.
#if NR_HWAVES
    // Reloaded from `lds_x` in the output projection, which is the only stage
    // that reads it; see `yqr` there.
#define NR_Y(m, n, c) float(yqr[m][c])
#else
#define NR_Y(m, n, c) float(NR_YQ(m, n)[c])
#endif
#else
#if NR_Q32_STAGE
    // Windows: kept in f32. RADV's NIR deletes the f32 -> f16 -> f32 round trip of every use,
    // so linux/ never rounded it; LLPC keeps it (two converts a value). f32 >= native precision.
    float yh[NR_MF][NR_CF][8];
#else
    NR_FRAG_ACC16 yh[NR_MF][NR_CF];     // ... and the wide one, for the skip
#endif
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
                const uint off=pc.rs_off+uint(n)*16u+rbase+uint(c);
#if NR_RESIDUAL_F32
                const vec2 residual=NR_E4F2(yr,c)*vec2(wgt_f32[off],wgt_f32[off+1u]);
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
        NR_WEIGHT_PAIR_S(wf0,wf1,pc.ct_off,uint(nrhw_h*NR_DF),uint(k),uint(NR_KCF),4)
        for(int m=0;m<NR_MF;++m) NR_MG(m) NR_MMA(nr_contract_acc[0][m],wf0,ctx[m]);
        for(int m=0;m<NR_MF;++m) NR_MG(m) NR_MMA(nr_contract_acc[1][m],wf1,ctx[m]);
    }
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
        NR_FRAG_B yqw[NR_MF];
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
            const vec2 x=vec2(xh[m][n][c],xh[m][n][c+1]);
#else
            const vec2 x=NR_E4F2(NR_XB(m,n),c);
#endif
            const uint off=pc.rs_off+uint(n)*16u+rbase+uint(c);
            const vec2 residual=x*vec2(wgt_f32[off],wgt_f32[off+1u]);
#else
#ifdef NR_INPUT_F16
            const f16vec2 x=f16vec2(xh[m][n][c],xh[m][n][c+1]);
#else
            const f16vec2 x=NR_N2_XB(NR_XB(m,n)[c],NR_XB(m,n)[c+1]);
#endif
            const uint off=pc.rs_off+uint(n)*16u+rbase+uint(c);
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
            const float x=float(xh[m][n][c]);
#else
            const float x=float(NR_XB(m,n)[c]);
#endif
            a[m][c]=nr_round_f16(x*wgt_f32[pc.rs_off+uint(n)*16u+rbase+uint(c)]);
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
            NR_C32_WEIGHT(wf,pc.ct_off,uint(n),uint(k),uint(NR_KCF));
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
#if NR_Q32_STAGE
            float requantized[8];
#else
            NR_F16 requantized[8];
#endif
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
                const NR_F16 residual = NR_F16(xh[m][n][c] *
                    NR_F16(wgt_f32[pc.rs_off + uint(n)*16u + rbase + uint(c)]));
                const float v = float(NR_F16(NR_F16(a[m][c]) + residual));
#else
                const float v = a[m][c]
                    + wgt_f32[pc.rs_off + uint(n) * 16u + rbase + uint(c)]
                      * float(NR_XB(m,n)[c]);
#endif
#if NR_QUANT_PAIRED && NR_Q32_STAGE
                requantized[c] = v;
#elif NR_QUANT_PAIRED
                requantized[c] = NR_F16(v);
#else
                NR_YQ(m, n)[c] = nr_quant_e4m3(NR_F16(v));
#endif
#if NR_HEADS == 1 && NR_Q32_STAGE
                yh[m][n][c] = v;
#elif NR_HEADS == 1
                yh[m][n][c] = NR_F16(v);
#endif
            }
#endif
#if NR_QUANT_PAIRED
            for (int c=0;c<8;c+=2) {
#if NR_Q32_STAGE
                NR_QPAIR_T q=nr_quant_pair32(vec2(requantized[c],requantized[c+1]));
#else
                NR_QPAIR_T q=NR_QP_YQ(f16vec2(requantized[c],requantized[c+1]));
#endif
                NR_YQ(m, n)[c]=q.x; NR_YQ(m, n)[c+1]=q.y;
#if NR_V_SWAP && !NR_HWAVES
                yqa[m][NR_YN][c]=q.x; yqa[m][NR_YN][c+1]=q.y;
#endif
            }
        }
#else
#endif
#if NR_HWAVES
        // Back into lds_x, NR_LXB_ in place: this wave is the only writer of rows n,
        // and every wave finished reading x before the barrier above.
        for (int m = 0; m < NR_MF; ++m) NR_MG(m) {
            NR_FRAG_E4M3 yf;
            for (int c = 0; c < 8; ++c) yf[c] = NR_YQ(m, n)[c];
            NR_STORE_ACC_COL(yf, lds_x, NR_LXB_ uint(m * NR_CF + n) * 256u, 16u);
        }
#endif
    }
#if NR_HWAVES
    barrier();                          // y is now everyone's; lds_y is free
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
        NR_STORE_ACT_COL(d, pc.o_off + wbase
                         + (tok0 + uint(m) * 16u) * uint(NR_C) + uint(n) * 16u, uint(NR_C));
    }
    return;
#endif

    // ---- stages 3 to 5: QKV, attention and context, one head at a time ---
    NR_PROF_STAGE(3)
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
                    NR_WEIGHT_PAIR_S(wf0,wf1,pc.qkv_off,uint(hh*3*NR_DF+0+r),uint(k),uint(NR_CF),8)
                    for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_MMA(acc[m][r],wf0,yk[m]);
                    for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_MMA(acc[m][r+1],wf1,yk[m]);
                }
#else
                for (int r = 0; r < NR_QK_ROWS; ++r) {
                    NR_OPA wf;
                    NR_C32_WEIGHT(wf,pc.qkv_off,uint(hh*3*NR_DF+r),uint(k),uint(NR_CF));
                    for (int m = 0; m < NR_MF; ++m) NR_MMA(acc[m][r], wf, yk[m]);
                }
#endif
                if (NR_ACC_F16 > 0 && (k + 1) % NR_ACC_F16 == 0)
                    for (int m = 0; m < NR_MF; ++m)
                        for (int r = 0; r < NR_QK_ROWS; ++r) NR_RND(acc[m][r])
                NR_KFENCE(8)
            }
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
                        qb[m][d][c] = qp.x;
                        qb[m][d][c + 1] = qp.y;
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
                    NR_LOAD_WA(wf,
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
                    NR_FRAG_E4M3 kf;
                    for (int c = 0; c < 8; c += 2) {
#if NR_QK_SCALE_FAST
                        fe4m3vec2 qp = nr_kscale_fast(vec2(acc[m][d + NR_QK_TOGETHER * NR_DF][c],
                            acc[m][d + NR_QK_TOGETHER * NR_DF][c+1]),nk);
#else
                        f16vec2 kv = NR_N2_K(acc[m][d + NR_QK_TOGETHER * NR_DF][c],
                                             acc[m][d + NR_QK_TOGETHER * NR_DF][c + 1]);
                        fe4m3vec2 qp = nr_quant_pair(kv * f16vec2(nk));
#endif
                        kf[c] = qp.x;
                        kf[c + 1] = qp.y;
                    }
                    // The ColumnMajor store was the transpose; the component
                    // copy is the same transpose with no memory behind it.
                    for (int c = 0; c < 8; ++c) kreg[m][d][c] = kf[c];
                }
            }
            // ---- pass V: rows [hh*3*NR_DF + 2*NR_DF, +NR_DF) ----
#if NR_V_SWAP && NR_VPASS_SPLIT
            // Windows (LLPC) NR_VPASS_SPLIT: the V pass over the token fragments in two halves,
            // four accumulators live instead of eight (LLVM hoists every k step's fragment loads
            // ahead of the first WMMA, so the pass was the register peak). Each accumulator sees
            // the same k order; the weight pairs are read once a half.
            for (int mh = 0; mh < NR_MF; mh += NR_MF / 2) {
                for (int m = mh; m < mh + NR_MF / 2; ++m)
                    for (int d = 0; d < NR_DF; ++d) acc[m][d] = NR_ACCZERO;
                for (int k = 0; k < NR_CF; ++k) {
                    NR_FRAG_A yka[NR_MF];
                    for (int m = mh; m < mh + NR_MF / 2; ++m)
                        NR_LOAD_A(yka[m], lds_x, NR_LXB_
                                  uint(m * NR_CF + k) * 256u + NR_OPQ(130u), 16u);
                    for(int r=0;r<NR_DF;r+=2) {
                        NR_FRAG_B wb0,wb1;
                        NR_WEIGHT_PAIR_S(wb0,wb1,pc.qkv_off,uint(hh*3*NR_DF+2*NR_DF+r),uint(k),uint(NR_CF),16)
                        for(int m=mh;m<mh+NR_MF/2;++m) NR_MGA(m) NR_MMA(acc[m][r],yka[m],wb0);
                        for(int m=mh;m<mh+NR_MF/2;++m) NR_MGA(m) NR_MMA(acc[m][r+1],yka[m],wb1);
                    }
                }
                for (int m = mh; m < mh + NR_MF / 2; ++m) NR_MGK(m)
                    for (int d = 0; d < NR_DF; ++d)
                        for (int c = 0; c < 8; c += 2) {
                            fe4m3vec2 qp = nr_quant_pair(NR_N2_V(
                                acc[m][d][c],
                                acc[m][d][c + 1]));
                            vreg[d][m][c] = qp.x;
                            vreg[d][m][c + 1] = qp.y;
                        }
            }
#else
            for (int m = 0; m < NR_MF; ++m)
                for (int d = 0; d < NR_DF; ++d) acc[m][d] = NR_ACCZERO;
            for (int k = 0; k < NR_CF; ++k) {
#if NR_V_SWAP
#if !(NR_HWAVES && NR_ACC_F16 == 0 && defined(NR_PACKED_QKV) && NR_QUANT_PAIRED && NR_MF == NR_JF)
#error "NR_V_SWAP: head-split FP32 packed-QKV body with one wave per window"
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
                    NR_WEIGHT_PAIR_S(wb0,wb1,pc.qkv_off,uint(hh*3*NR_DF+2*NR_DF+r),uint(k),uint(NR_CF),16)
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
                    NR_WEIGHT_PAIR_S(wf0,wf1,pc.qkv_off,uint(hh*3*NR_DF+2*NR_DF+r),uint(k),uint(NR_CF),16)
                    for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_MMA(acc[m][r],wf0,yk[m]);
                    for(int m=0;m<NR_MF;++m) NR_MGA(m) NR_MMA(acc[m][r+1],wf1,yk[m]);
                }
#else
                for (int r = 0; r < NR_DF; ++r) {
                    NR_OPA wf;
                    NR_LOAD_WA(wf,
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
                NR_KFENCE(16)
            }
#if NR_V_SWAP
            for (int m = 0; m < NR_MF; ++m) NR_MGK(m)
                for (int d = 0; d < NR_DF; ++d)
                    for (int c = 0; c < 8; c += 2) {
                        fe4m3vec2 qp = nr_quant_pair(NR_N2_V(
                            acc[m][d][c],
                            acc[m][d][c + 1]));
                        vreg[d][m][c] = qp.x;
                        vreg[d][m][c + 1] = qp.y;
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
                    NR_V_LDS_STORE(vf, lds_y, NR_LXB_
                                     uint(m * NR_CF + hh * NR_DF + d) * 256u, 16u);
                }
#endif
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
                        NR_C32_WEIGHT(wfb,pc.qkv_off,uint(hh*3*NR_DF+r),uint(k),uint(NR_CF));
                        NR_MMA(qkv[r], yqa[m][k], wfb);
                        continue;
                    }
#endif
                    NR_OPA wf;
                    NR_C32_WEIGHT(wf,pc.qkv_off,uint(hh*3*NR_DF+r),uint(k),uint(NR_CF));
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
                    qb[m][d][c] = qp.x;
                    qb[m][d][c + 1] = qp.y;
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
                    kf[c] = qp.x;
                    kf[c + 1] = qp.y;
                }
#else
                for (int c = 0; c < 8; ++c)
                    kf[c] = nr_quant_e4m3(NR_F16(NR_F16(NR_QKVA(m, NR_DF + d)[c]) * nk));
#endif
                // K wants [token][dim]: store ColumnMajor, the accumulator
                // being [dim][token]. V wants [dim][token], which is the
                // accumulator's own orientation - a plain RowMajor store.
#if NR_K_REGS
                // The ColumnMajor store was the transpose; the component copy
                // is the same transpose with no memory behind it.
                for (int c = 0; c < 8; ++c) kreg[m][d][c] = kf[c];
#else
                NR_STORE_ACC_COL(kf, lds_k, t0 * uint(NR_HD) + uint(d) * 16u, uint(NR_HD));
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
                    vreg[d][m][c] = qp.x;
                    vreg[d][m][c + 1] = qp.y;
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
                NR_V_LDS_STORE(vf, lds_y, NR_LXB_
                                 uint(m * NR_CF + hh * NR_DF + d) * 256u, 16u);
#elif NR_V_SWAP && !NR_V_REGS
                NR_STORE_ACC_COL(vf, lds_v, NR_V_LDS_OFFSET + uint(d) * 16u * uint(NR_WIN) + t0, uint(NR_WIN));
#elif !NR_V_SWAP
                NR_STORE_ACC(vf, lds_v, NR_V_LDS_OFFSET + uint(d) * 16u * uint(NR_WIN) + t0, uint(NR_WIN));
#endif
            }
        }
#endif
#if !NR_HWAVES
        barrier();
#endif

        NR_PROF_STAGE_LAST(4)
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
        NR_OPB pb[NR_MF][NR_JF];
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
#else
                NR_FRAG_ACC bf;
                NR_LOAD_ACC_COL(bf, wgt_f32,
                                pc.b_off + uint(hh) * uint(NR_WIN * NR_WIN)
                                + (tok0 + uint(m) * 16u) * uint(NR_WIN) + uint(j) * 16u,
                                uint(NR_WIN));
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
#if NR_PROB_BOUNDED && NR_PROB_PK == 2
                    fe4m3vec2 q = fe4m3vec2(vec2(pq2[j][c]) * inv);
#elif NR_PROB_BOUNDED && NR_PROB_PK
                    fe4m3vec2 q = fe4m3vec2(pq2[j][c] * f16vec2(NR_F16(inv)));
#elif NR_PROB_BOUNDED
                    fe4m3vec2 q = fe4m3vec2(NR_N2_P(x,y));
#else
                    fe4m3vec2 q = nr_quant_pair(NR_N2_P(x, y));
#endif
                    pb[m][j][2 * c] = q.x;
                    pb[m][j][2 * c + 1] = q.y;
                }
        }
#else
        NR_PV_OPB pb[NR_MF][NR_JF];
#if NR_QUANT_PAIRED
        f16vec2 pq2[NR_MF][NR_JF][4];
#else
        float pq[NR_MF][NR_JF][8];
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
                NR_LOAD_A(kf, lds_k, uint(j) * 16u * uint(NR_HD) + uint(d) * 16u,
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
#else
                NR_FRAG_ACC bf;
                NR_LOAD_ACC_COL(bf, wgt_f32,
                                pc.b_off + uint(hh) * uint(NR_WIN * NR_WIN)
                                + (tok0 + uint(m) * 16u) * uint(NR_WIN) + uint(j) * 16u,
                                uint(NR_WIN));
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
#if NR_PROB_BOUNDED && NR_PROB_PK == 2
                NR_PV_QT q=fe4m3vec2(vec2(pq2[m][j][c]) * inv);
#elif NR_PROB_BOUNDED && NR_PROB_PK
                NR_PV_QT q=fe4m3vec2(pq2[m][j][c] * f16vec2(NR_F16(inv)));
#elif NR_PROB_BOUNDED
                NR_PV_QT q=fe4m3vec2(NR_N2_P(x,y));
#else
                NR_PV_QT q=NR_QP_P(NR_N2_P(x,y));
#endif
#endif
                pb[m][j][2*c]=q.x;
                pb[m][j][2*c+1]=q.y;
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
        NR_PROF_STAGE_LAST(5)
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
                NR_V_LDS_LOAD(vf, lds_y, NR_LXB_
                              uint(j * NR_CF + hh * NR_DF + e) * 256u
                              + NR_OPQ(192u), 16u);
#else
                NR_LOAD_A(vf, lds_v, NR_V_LDS_OFFSET + uint(e) * 16u * uint(NR_WIN) + uint(j) * 16u,
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
#if NR_HEADS > 1
        // The next head reuses lds_k and lds_v; at one head there is no next.
        barrier();
#endif
    }

#if NR_CONTEXT_LDS
    // Reload constant-index FP8 fragments after the final head barrier.
    for (int m=0;m<NR_MF;++m) for (int k=0;k<NR_CF;++k)
        NR_LOAD_B(cq[m][k], lds_context,
                  (tok0+uint(m)*16u)*uint(NR_C)+uint(k)*16u, uint(NR_C));
#endif
    // ---- stage 6: the output projection and the attention skip ------------
    NR_PROF_STAGE(6)
#if NR_IMGOUT_REGS
    // One RGB triple per token *pair* of fragments, summed across the n loop in
    // the original's channel order. `nr_io_lo` picks which of the pair a lane
    // owns; the token it lands on is `tok0 + 2p*16 + lane` for both halves.
    const bool nr_io_lo = lane < 16u;
    const uint nr_io_wp = NR_IMG_WOFF >> 1u;
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
                const uint off=pc.ars_off+uint(n)*16u+rbase+uint(c);
#if NR_RESIDUAL_F32
                const vec2 residual=NR_E4F2(yr,c)*vec2(wgt_f32[off],wgt_f32[off+1u]);
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
        NR_WEIGHT_PAIR_S(wf0,wf1,pc.op_off,uint(nrhw_h*NR_DF),uint(k),uint(NR_CF),32)
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
            const uint off=pc.ars_off+uint(n)*16u+rbase+uint(c);
#if NR_RESIDUAL_F32
            const vec2 residual=vec2(NR_Y(m,n,c),NR_Y(m,n,c+1))*vec2(wgt_f32[off],wgt_f32[off+1u]);
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
            a[m][c]=nr_round_f16(wgt_f32[pc.ars_off+uint(n)*16u+rbase+uint(c)]*NR_Y(m,n,c));
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
            NR_C32_WEIGHT(wf,pc.op_off,uint(n),uint(k),uint(NR_CF));
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
                    + wgt_f32[pc.ars_off + uint(n) * 16u + rbase + uint(c)]
                      * NR_Y(m, n, c)
#endif
                    );
#ifdef NR_FUSED_IMAGE_OUTPUT
#if NR_IMGOUT_REGS
            if ((m & 1) == 0) { nr_io_even = of; }
            else {
                // A lane projects the even fragment's token in the low half-wave
                // and the odd fragment's in the high half, so what it is missing
                // is the *other* half of the channel range of **its own**
                // fragment - which lives in lane ^ 16. The shuffle therefore
                // carries the complementary select: lanes >= 16 offer the even
                // fragment (which the low half wants) and lanes < 16 offer the
                // odd one. Shuffling the own-value instead reads the wrong
                // fragment on both halves, which is a whole-picture error and
                // not a rounding one.
                //
                // The channel sequence is the original's, 0..31 in order, so the
                // low eight of this fragment are summed before the high eight:
                // only `hi` is carried between the two passes.
                uint hi[4];
                for (int c = 0; c < 8; c += 2) {
                    const uint ev = packFloat2x16(f16vec2(nr_io_even[c],
                                                          nr_io_even[c+1]));
                    const uint od = packFloat2x16(f16vec2(of[c], of[c+1]));
                    const uint xv = subgroupShuffleXor(nr_io_lo ? od : ev, 16u);
                    hi[c / 2] = nr_io_lo ? xv : od;
                    NR_IO_ACC(m / 2, uint(n) * 8u + uint(c) / 2u,      // ch n*16+c
                              nr_io_lo ? ev : xv)
                }
                for (int c = 0; c < 8; c += 2)
                    NR_IO_ACC(m / 2, uint(n) * 8u + 4u + uint(c) / 2u, // ch n*16+8+c
                              hi[c / 2])
            }
#else
            NR_STORE_ACC_COL(of,image_features,(tok0+uint(m)*16u)*32u+uint(n)*16u,32u);
#endif
#else
            if (!nr_tile_oob((tok0 + uint(m) * 16u) / 16u))
                NR_STORE_ACC_COL(of, act_f16,
                    pc.o_off / 2u + nr_tile_base((tok0 + uint(m) * 16u) / 16u)
                    + uint(n) * 256u, 16u);
#endif
#else
            NR_FRAG_E4M3 of;
            NR_F16 wide[8];
#if NR_Q32_STAGE
            float widef[8];   // the pre-narrowing values, for the quantiser (see yh)
#endif
#if NR_Q32_STAGE && NR_Q32_POOL
            // The pool from them too: -5..-7% instructions in the DS runs, no time gained on
            // Windows and 48.1 vs 48.6 dB to Linux (2026-09-28); off.
#define NR_WIDE_F(c) widef[c]
#else
#define NR_WIDE_F(c) float(wide[c])
#endif
#if NR_POOL_PACKED == 3 || NR_POOL_DPP16 >= 6
            // The pre-narrowing f32 values. The shipped scalar pool reads
            // float(wide[c]), which ACO folds back to exactly these.
            float wf[8];
#endif
            for (int c = 0; c < 8; ++c) {
#if NR_POOL_PACKED == 3 || NR_POOL_DPP16 >= 6
                wf[c] = a[m][c]
#if !NR_PTX_ACC
                    + wgt_f32[pc.ars_off + uint(n) * 16u + rbase + uint(c)]
                      * NR_Y(m, n, c)
#endif
                    ;
                wide[c] = NR_F16(wf[c]);
#if NR_Q32_STAGE
                widef[c] = wf[c];
#endif
#else
#if NR_Q32_STAGE
                widef[c] = a[m][c]
#if !NR_PTX_ACC
                    + wgt_f32[pc.ars_off + uint(n) * 16u + rbase + uint(c)]
                      * NR_Y(m, n, c)
#endif
                    ;
                wide[c] = NR_F16(widef[c]);
#else
                wide[c] = NR_F16(a[m][c]
#if !NR_PTX_ACC
                    + wgt_f32[pc.ars_off + uint(n) * 16u + rbase + uint(c)]
                      * NR_Y(m, n, c)
#endif
                    );
#endif
#endif
#if NR_QUANT_PAIRED
            }
            for (int c=0;c<8;c+=2) {
#if NR_Q32_STAGE
                fe4m3vec2 q=nr_quant_pair32(vec2(widef[c],widef[c+1]));
#else
                fe4m3vec2 q=nr_quant_pair(f16vec2(wide[c],wide[c+1]));
#endif
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
            // Each active lane owns eight consecutive
            // channels, starting at an eight-byte-aligned rbase (0 or 8).
            // Both four-byte vectors remain inside this lane's channel group.
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
                const float a0 = NR_WIDE_F(c), a1 = NR_WIDE_F(c+1);
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
                const float v0 = NR_WIDE_F(c), v1 = NR_WIDE_F(c+1);
                const float s0 = v0 + subgroupShuffleXor(v0,1u), s1 = v1 + subgroupShuffleXor(v1,1u);
                const f16vec2 pr = f16vec2(NR_F16(s0), NR_F16(s1));
                const f16vec2 sm = pr + f16vec2(NR_F16(subgroupShuffleXor(s0,4u)),
                                                NR_F16(subgroupShuffleXor(s1,4u)));
                meansf[c] = float(sm.x) * 0.25;
                meansf[c+1] = float(sm.y) * 0.25;
            }
#else
            for (int c=0;c<8;++c) {
                float v = NR_WIDE_F(c);
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
                const uint cb = rbase + 4u*(lane&1u) + 2u*((lane>>2u)&1u);
#if NR_HWAVES
                lds_x[NR_LXB_ uint(n)*256u + lp*16u + cb] = q0.x;
                lds_x[NR_LXB_ uint(n)*256u + lp*16u + cb + 1u] = q0.y;
#else
                lds_v[lp*uint(NR_C) + uint(n)*16u + cb] = q0.x;
                lds_v[lp*uint(NR_C) + uint(n)*16u + cb + 1u] = q0.y;
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
                const uint cb = rbase + 4u*(lane&1u);
#if NR_HWAVES
                for (int c=0;c<4;++c)
                    lds_x[NR_LXB_ uint(n)*256u + lp*16u + cb + uint(c)] = pooled[0][c];
#else
                for (int c=0;c<4;++c)
                    lds_v[lp*uint(NR_C) + uint(n)*16u + cb + uint(c)] = pooled[0][c];
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
                    lds_x[NR_LXB_ uint(n)*256u + lp*16u + rbase + uint(c)] = pooled[c/4][c%4];
#else
                for (int c=0;c<8;++c)
                    lds_v[lp*uint(NR_C) + uint(n)*16u + rbase + uint(c)] = pooled[c/4][c%4];
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
                             +(lslot*16u+rbase);
#else
                uint tile=uint(py/4)*pc.pool_tiles_x+uint(px/4);
                uint slot=uint(py%4)*4u+uint(px%4);
                uint address=pc.pool_off+(tile*uint(NR_CF)+uint(n))*256u+slot*16u+rbase;
#endif
#if NR_POOL_DPP16 == 7
                {
                    const fe4m3vec2 q7=NR_POOL_Q(0);
                    const uint a7=address+4u*(lane&1u)+2u*((lane>>2u)&1u);
                    act_e4m3[a7]=q7.x; act_e4m3[a7+1u]=q7.y;
                }
#elif NR_POOL_DPP16 == 6
                act_e4m3x4[address/4u+(lane&1u)]=pooled[0];
#else
                act_e4m3x4[address/4u]=pooled[0];act_e4m3x4[address/4u+1u]=pooled[1];
#endif
            }
#else
            for (int c=0;c<8;++c) {
                float v = NR_WIDE_F(c);
                NR_F16 pair = NR_F16(v + subgroupShuffleXor(v,1u));
                NR_F16 sum = NR_F16(pair + NR_F16(subgroupShuffleXor(float(pair),4u)));
                NR_F16 mean = NR_F16(sum * NR_F16(0.25));
                if ((lane&5u)==0u && px>=0 && py>=0 &&
                    px<int(pc.pool_tiles_x*4u) && py<int(pc.pool_tiles_y*4u)) {
                    uint tile = uint(py/4)*pc.pool_tiles_x+uint(px/4);
                    uint slot = uint(py%4)*4u+uint(px%4);
                    uint ch = uint(n)*16u+rbase+uint(c);
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
            if (!nr_tile_oob((tok0 + uint(m) * 16u) / 16u))
            NR_STORE_ACT_COL(of,
                             pc.o_off + nr_tile_base((tok0 + uint(m) * 16u) / 16u)
                             + uint(n) * 256u, 16u);
#else
            NR_STORE_ACT_COL(of, pc.o_off + wbase
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
    barrier();
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
        const uint col0 = rbase;
        const uint writer_rows = pc.ds_writer_rows == 0u ? pc.ds_rows : pc.ds_writer_rows;
#if NR_DS_IDENTITY_FAST
        const bool ds_identity = pc.ds_mode != 2u && pc.ds_raster == pc.ds_crow && writer_rows == pc.ds_rows;
#endif
#if NR_HWAVES
        // Each head-wave owns 2*NR_DF of the 2C output fragments.
        for (int ol = 0; ol < 2 * NR_DF; ++ol) {
            const int of = nrhw_h * 2 * NR_DF + ol;
#else
        for (int of = 0; of < 2 * NR_CF; ++of) {
#endif
            NR_ACCF acc = NR_ACCZERO;
            for (int kf = 0; kf < NR_CF; ++kf) {
                NR_OPA wf;
                NR_LOAD_WA(wf, pc.ds_w_off + uint(of * NR_CF + kf) * 256u, 16u);
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
            if (ds_identity) {
                dst_group = sg; sdx = spx; sdy = spy;
            } else {
                const uint linear = pc.ds_mode == 2u ? (sg * pc.ds_rows + spy) * pc.ds_crow + spx
                    : (sg * writer_rows + spy) * pc.ds_raster + spx;
                dst_group = linear / (pc.ds_crow * pc.ds_rows);
                const uint pixel = linear % (pc.ds_crow * pc.ds_rows);
                sdx = pixel % pc.ds_crow; sdy = pixel / pc.ds_crow;
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
            const uint ob = (pc.ds_o_off
                             + (((sdy / 4u) * pc.ds_otx + sdx / 4u) * (pc.ds_n / 16u) + dst_group) * 256u
                             + (4u * (sdy % 4u) + sdx % 4u) * 16u + col0) / 4u;
            if (in_grid && !s_oob) {
#if NR_DS_QPAIR
                // The same per-component mode-4 conversion, two at a time
                // (one v_cvt_pk_fp8_f32 a pair instead of one a value + perms).
                // Windows: four at a time (nr_quant4_h), the same bytes.
#if NR_WIN_ST8
                act_v8[ob / 2u] = uvec2(nr_e4m3x4_bits(nr_quant4_h(f16vec4(outv[0], outv[1], outv[2], outv[3]))),
                                        nr_e4m3x4_bits(nr_quant4_h(f16vec4(outv[4], outv[5], outv[6], outv[7]))));
#else
                act_e4m3x4[ob]      = nr_quant4_h(f16vec4(outv[0], outv[1], outv[2], outv[3]));
                act_e4m3x4[ob + 1u] = nr_quant4_h(f16vec4(outv[4], outv[5], outv[6], outv[7]));
#endif
#else
                act_e4m3x4[ob]      = fe4m3vec4(nr_quant_e4m3(outv[0]), nr_quant_e4m3(outv[1]),
                                                nr_quant_e4m3(outv[2]), nr_quant_e4m3(outv[3]));
                act_e4m3x4[ob + 1u] = fe4m3vec4(nr_quant_e4m3(outv[4]), nr_quant_e4m3(outv[5]),
                                                nr_quant_e4m3(outv[6]), nr_quant_e4m3(outv[7]));
#endif
            }
        }
    }
#if NR_PERSIST_DS
    }
#endif
#endif
