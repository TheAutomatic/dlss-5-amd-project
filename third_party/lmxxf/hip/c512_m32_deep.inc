#ifndef C512_MIX_OCC_LDS
#define C512_MIX_OCC_LDS 0 /* bytes of dummy LDS on split_mix_blocked_h16w_m32 (occupancy cap) */
#endif
// C512 split mix (half weights), 32 tokens per wave (opt-in DLSS5_HIP_C512_M32, module c512-m32-deep): each half B
// fragment feeds two WMMAs; per-output K order and F(Hrtz()) epilogue as split_mix_blocked_h16w. results/c512-ffn-20260926.
#ifndef C512_MIX_PIPE
#define C512_MIX_PIPE 0 /* 1: split_mix_blocked_h16w_m32 K loop software-pipelined (next A rows + B fragments issued one step ahead); bit-identical. results/fill-cu-20260930 */
#endif
#ifndef C512_ZERO_PAD_900
#define C512_ZERO_PAD_900 0
#endif
#if C512_ZERO_PAD_900
using c512_u4=uint __attribute__((ext_vector_type(4)));
DEV uint c512_wave_or(uint x){uint lane=__builtin_amdgcn_workitem_id_x();for(uint d=16;d;d>>=1)x|=uint(__builtin_amdgcn_ds_bpermute((lane^d)*4,int(x)));
#if C512_ZERO_PAD_900 == 2
 return __builtin_amdgcn_readfirstlane(x);
#else
 return x;
#endif
}
DEV bool c512_zero_tile(const float*in,const _Float16*w,uint first,uint row){
 uint lane=__builtin_amdgcn_workitem_id_x(),bits_or=0;c512_u4 v;
 // Cheap rejection, then prove every scalar is +/-zero; no assumption about real image pixels.
 __builtin_memcpy(&v,in+first*512+lane*4,16);bits_or=v[0]|v[1]|v[2]|v[3];
 if(c512_wave_or(bits_or&0x7fffffffu))return false;
 _Pragma("clang loop unroll(disable)") for(uint q=32+lane;q<4096;q+=32){__builtin_memcpy(&v,in+first*512+q*4,16);bits_or|=v[0]|v[1]|v[2]|v[3];}
 if(c512_wave_or(bits_or&0x7fffffffu))return false;
 // Zero*Inf/NaN is not zero. Check the actual 64x512 half-weight tile before bypassing the WMMA.
 uint bad=0;
 _Pragma("clang loop unroll(disable)") for(uint q=lane;q<4096;q+=32){__builtin_memcpy(&v,w+row*512+q*8,16);for(uint j=0;j<4;j++)bad|=((v[j]&0x7c007c00u)+0x04000400u)&0x80008000u;}
 return c512_wave_or(bad)==0;
}
#endif
WAVE void split_mix_blocked_h16w_m32(const float*in,const float*w,float*out,uint tokens){
#if C512_MIX_OCC_LDS
 __attribute__((shared)) volatile uint occ_pad[C512_MIX_OCC_LDS/4]; /* occupancy cap only (small-wins O); branch never taken for a real token count */
 if(tokens==0xffffffffu){uint lane=__builtin_amdgcn_workitem_id_x();uint i=(bid()*131u+lane)%(C512_MIX_OCC_LDS/4);occ_pad[i]=i;__builtin_amdgcn_s_barrier();if(occ_pad[(i+1)%(C512_MIX_OCC_LDS/4)]==5u)out[0]=0.f;}
#endif

 uint first=bid()/8*32,row=bid()%8*64;if(first>=tokens)return;const bool two=first+16<tokens;f8 acc0[4]{},acc1[4]{};const _Float16*w16=reinterpret_cast<const _Float16*>(w);
#if C512_ZERO_PAD_900
 const bool suspect=(tokens==1792&&first>=1680)||(tokens==2240&&(first+32<=224||first>=1904));
 if(two&&suspect&&c512_zero_tile(in,w16,first,row)){
  for(uint t=0;t<2;t++)for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+t*16+gr()*8+e)*512+row+j*16+rc()]=0.f;
  return;
 }
#endif
#if C512_MIX_PIPE == 2
 // Lighter pipeline: only the two A rows (raw f32) are fetched one step ahead; B fragments stay in-step as before.
 {using f4v=float __attribute__((ext_vector_type(4)));f4v p0[2],p1[2]{},q0[2],q1[2]{};
  const float*ar0=in+(first+rc())*512+gr()*8,*ar1=ar0+16*512;
  __builtin_memcpy(p0,ar0,32);if(two)__builtin_memcpy(p1,ar1,32);
  for(uint k=0;k<512;k+=16){
   if(k+16<512){__builtin_memcpy(q0,ar0+k+16,32);if(two)__builtin_memcpy(q1,ar1+k+16,32);}
   h8 a0,a1;for(uint e=0;e<8;e++){a0[e]=(_Float16)p0[e/4][e%4];a1[e]=(_Float16)p1[e/4][e%4];}
   for(uint j=0;j<4;j++){h8 b;__builtin_memcpy(&b,w16+(row+j*16+rc())*512+k+gr()*8,16);acc0[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a0,b,acc0[j]);acc1[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a1,b,acc1[j]);}
   for(uint i=0;i<2;i++){p0[i]=q0[i];p1[i]=q1[i];}}}
#elif C512_MIX_PIPE
 // Software pipeline (results/fill-cu-20260930): step k+16's raw f32 A rows and the 4 B fragments are issued before
 // step k's WMMAs, so one wave no longer waits two serial memory latencies per K step (the compiler only prefetched a0).
 // Same values, same f32->f16 conversions, same WMMA operands and K order -> bit-identical.
 {using f4v=float __attribute__((ext_vector_type(4)));f4v p0[2],p1[2]{},q0[2],q1[2]{};h8 bn[4],bq[4];
  const float*ar0=in+(first+rc())*512+gr()*8,*ar1=ar0+16*512;const _Float16*wr=w16+(row+rc())*512+gr()*8;
  __builtin_memcpy(p0,ar0,32);if(two)__builtin_memcpy(p1,ar1,32);for(uint j=0;j<4;j++)__builtin_memcpy(&bn[j],wr+j*16*512,16);
  for(uint k=0;k<512;k+=16){
   if(k+16<512){__builtin_memcpy(q0,ar0+k+16,32);if(two)__builtin_memcpy(q1,ar1+k+16,32);for(uint j=0;j<4;j++)__builtin_memcpy(&bq[j],wr+j*16*512+k+16,16);}
   h8 a0,a1;for(uint e=0;e<8;e++){a0[e]=(_Float16)p0[e/4][e%4];a1[e]=(_Float16)p1[e/4][e%4];}
   for(uint j=0;j<4;j++){acc0[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a0,bn[j],acc0[j]);acc1[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a1,bn[j],acc1[j]);}
   for(uint i=0;i<2;i++){p0[i]=q0[i];p1[i]=q1[i];}for(uint j=0;j<4;j++)bn[j]=bq[j];}}
#else
 for(uint k=0;k<512;k+=16){h8 a0{},a1{};for(uint e=0;e<8;e++)a0[e]=(_Float16)in[(first+rc())*512+k+gr()*8+e];if(two)for(uint e=0;e<8;e++)a1[e]=(_Float16)in[(first+16+rc())*512+k+gr()*8+e];
  for(uint j=0;j<4;j++){h8 b;__builtin_memcpy(&b,w16+(row+j*16+rc())*512+k+gr()*8,16);acc0[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a0,b,acc0[j]);acc1[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a1,b,acc1[j]);}}
#endif
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+row+j*16+rc()]=F(c512_hq(acc0[j][e]));
 if(two)for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+16+gr()*8+e)*512+row+j*16+rc()]=F(c512_hq(acc1[j][e]));
}
#ifndef C512_MIX_LDS_G
#define C512_MIX_LDS_G 0 /* >0: add export split_mix_blocked_h16w_m32_lds (32*G threads = G waves on one 64-column row block, each wave the same 32-token tile work as split_mix_blocked_h16w_m32; half-weight 64x64 chunks staged once per group through LDS, double buffered). Same per-wave WMMA chains/K order/epilogue. grid = 8*ceil(ceil(tokens/32)/G). results/c512-ffn-lds-20260930. */
#endif
#ifndef C512_MIX_LDS_U
#define C512_MIX_LDS_U 4 /* unroll of the 4 k16 steps inside a staged chunk */
#endif
#if C512_MIX_LDS_G
static_assert(C512_MIX_LDS_G==4||C512_MIX_LDS_G==2,"4 waves x 4 pieces or 2 waves x 8 pieces = one 8KB chunk");
KERNEL __attribute__((amdgpu_flat_work_group_size(32*C512_MIX_LDS_G,32*C512_MIX_LDS_G))) void split_mix_blocked_h16w_m32_lds(const float*in,const float*w,float*out,uint tokens){
 constexpr uint CH=4;/* chunk = 4 k16 steps x 4 column tiles x 32 lanes x 16B = 8KB = 512 pieces; thread t moves pieces t+i*128 (column tile i) */
 __attribute__((shared)) _Float16 lds[2*4*CH*256];
 const uint t=__builtin_amdgcn_workitem_id_x(),wv=t>>5,lane=t&31,first=(bid()/8*C512_MIX_LDS_G+wv)*32,row=bid()%8*64;
 const bool live=first<tokens,two=first+16<tokens;const _Float16*w16=reinterpret_cast<const _Float16*>(w);
 h8 s0,s1,s2,s3,s4,s5,s6,s7;constexpr uint TS=32*C512_MIX_LDS_G;f8 acc0[4]{},acc1[4]{};
 const _Float16*src=w16+(row+(t&15))*512+((t>>5)&3)*16+((t>>4)&1)*8;/* p=t+i*TS: j=p>>7, kk=(p>>5)&3; for TS=128 + i*16 columns, TS=64 + (i>>1)*16 columns + (i&1)*2 k16 steps */
#define MIXL_OFF(i) (TS==128?(i)*16*512:((i)>>1)*16*512+((i)&1)*32)
#define MIXL_FETCH(kc_) {const _Float16*q=src+(kc_)*CH*16;__builtin_memcpy(&s0,q+MIXL_OFF(0),16);__builtin_memcpy(&s1,q+MIXL_OFF(1),16);__builtin_memcpy(&s2,q+MIXL_OFF(2),16);__builtin_memcpy(&s3,q+MIXL_OFF(3),16);if(TS==64){__builtin_memcpy(&s4,q+MIXL_OFF(4),16);__builtin_memcpy(&s5,q+MIXL_OFF(5),16);__builtin_memcpy(&s6,q+MIXL_OFF(6),16);__builtin_memcpy(&s7,q+MIXL_OFF(7),16);}}
#define MIXL_STASH(sb_) {_Float16*d=lds+(sb_)*(4*CH*256)+t*8;__builtin_memcpy(d,&s0,16);__builtin_memcpy(d+TS*8,&s1,16);__builtin_memcpy(d+2*TS*8,&s2,16);__builtin_memcpy(d+3*TS*8,&s3,16);if(TS==64){__builtin_memcpy(d+4*TS*8,&s4,16);__builtin_memcpy(d+5*TS*8,&s5,16);__builtin_memcpy(d+6*TS*8,&s6,16);__builtin_memcpy(d+7*TS*8,&s7,16);}}
 MIXL_FETCH(0)
#pragma unroll 1
 for(uint kc=0;kc<512/(16*CH);kc++){MIXL_STASH(kc&1)__builtin_amdgcn_s_barrier();if(kc+1<512/(16*CH)){MIXL_FETCH(kc+1)}
  if(live)
#pragma unroll C512_MIX_LDS_U
  for(uint kk=0;kk<CH;kk++){const uint k=(kc*CH+kk)*16;h8 a0{},a1{};for(uint e=0;e<8;e++)a0[e]=(_Float16)in[(first+rc())*512+k+gr()*8+e];if(two)for(uint e=0;e<8;e++)a1[e]=(_Float16)in[(first+16+rc())*512+k+gr()*8+e];
#pragma unroll
   for(uint j=0;j<4;j++){h8 b;__builtin_memcpy(&b,lds+(kc&1)*(4*CH*256)+(j*128+kk*32+lane)*8,16);acc0[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a0,b,acc0[j]);acc1[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a1,b,acc1[j]);}}}
#undef MIXL_FETCH
#undef MIXL_STASH
#undef MIXL_OFF
 if(!live)return;
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+row+j*16+rc()]=F(c512_hq(acc0[j][e]));
 if(two)for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+16+gr()*8+e)*512+row+j*16+rc()]=F(c512_hq(acc1[j][e]));
}
#endif
#ifndef C512_SPLIT_N
#define C512_SPLIT_N 0 /* 1: add exports split_mix_blocked_h16w_m32_n2 (block 64 = 2 waves, same grid) and _n2g (block 32, grid x2): each wave 32 tokens x 32 columns instead of 64, per-output K order/operands/epilogue unchanged. results/fill-cu-20260930. */
#endif
#if C512_SPLIT_N
template<bool WG64> DEV void split_mix_n2_body(const float*in,const float*w,float*out,uint tokens){
 const uint half=WG64?(__builtin_amdgcn_workitem_id_x()>>5):(bid()&1u),blk=WG64?bid():(bid()>>1);
 uint first=blk/8*32,row=blk%8*64+half*32;if(first>=tokens)return;const bool two=first+16<tokens;f8 acc0[2]{},acc1[2]{};const _Float16*w16=reinterpret_cast<const _Float16*>(w);
 for(uint k=0;k<512;k+=16){h8 a0{},a1{};for(uint e=0;e<8;e++)a0[e]=(_Float16)in[(first+rc())*512+k+gr()*8+e];if(two)for(uint e=0;e<8;e++)a1[e]=(_Float16)in[(first+16+rc())*512+k+gr()*8+e];
  for(uint j=0;j<2;j++){h8 b;__builtin_memcpy(&b,w16+(row+j*16+rc())*512+k+gr()*8,16);acc0[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a0,b,acc0[j]);acc1[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a1,b,acc1[j]);}}
 for(uint j=0;j<2;j++)for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+row+j*16+rc()]=F(c512_hq(acc0[j][e]));
 if(two)for(uint j=0;j<2;j++)for(uint e=0;e<8;e++)out[(first+16+gr()*8+e)*512+row+j*16+rc()]=F(c512_hq(acc1[j][e]));
}
KERNEL __attribute__((amdgpu_flat_work_group_size(64,64))) void split_mix_blocked_h16w_m32_n2(const float*in,const float*w,float*out,uint tokens){split_mix_n2_body<true>(in,w,out,tokens);}
WAVE void split_mix_blocked_h16w_m32_n2g(const float*in,const float*w,float*out,uint tokens){split_mix_n2_body<false>(in,w,out,tokens);}
#endif

// C512_FFN_ONE (2026-10-01, results/mochizuki-gap-20261001): mix -> expand -> activation -> contract in one wave, in
// registers, like mochizuki's ffwd3_t.comp. A wave owns one 64-channel group g and 32 tokens (grid = mix m32 grid).
// Mix and expand run with swapped WMMA operands (weights as A), so the accumulator comes out token-per-lane, which is
// exactly the next stage's operand layout: no LDS, no barrier, no f32 mix tensor. Per output: same operands, same K
// order, same F(c512_hq()) / Hrtz-act-byte_F / F(c512_hq()) epilogues as split_mix_blocked_h16w_m32 +
// split_ffn_fused_fp8_t8; only the E4M3 tiles are written (the f32 contract copy has no reader). 0 = no export.
#ifndef C512_FFN_ONE
#define C512_FFN_ONE 0
#endif
#if C512_FFN_ONE
#ifndef C512_FFN_GRIDQ
#define C512_FFN_GRIDQ 0 /* DIAGNOSTIC: mix quantizes its f32 block input to E4M3 (F) first; 19 SAME => every C512 block input is on the E4M3 lattice */
#endif
#if C512_FFN_GRIDQ==2
#define C5D_GRIDQ(v) ((v)*1.001f) /* control: must DIFF */
#elif C512_FFN_GRIDQ
#define C5D_GRIDQ(v) F(v)
#else
#define C5D_GRIDQ(v) (v)
#endif
#ifndef C512_FFN_FP8MIX
#define C512_FFN_FP8MIX 0 /* 2026-10-01 probe: mix WMMA as fp8 x fp8 (inputs and f16 mix weights are all on the E4M3 lattice, converted in-kernel) */
#endif
DEV i2 c5d_pack8(h8 v){i2 r;for(uint i=0;i<2;i++){int w=__builtin_amdgcn_cvt_pk_fp8_f32(float(v[4*i]),float(v[4*i+1]),0,false);w=__builtin_amdgcn_cvt_pk_fp8_f32(float(v[4*i+2]),float(v[4*i+3]),w,true);r[i]=w;}return r;}
template<uint NT> DEV void c512_ffn_one_body(const float*in,const float*w,unsigned char*out8,uint tokens,uint first,uint g){
 const bool two=NT==2&&first+16<tokens;
 const _Float16*w16=reinterpret_cast<const _Float16*>(w);const _Float16*we=reinterpret_cast<const _Float16*>(w+262144)+g*16384;
 const uint*pw=reinterpret_cast<const uint*>(w)+393216+g*4096;
 f8 m[NT][4]{};
#ifndef C512_FFN_ABLATE
#define C512_FFN_ABLATE 0 /* DIAGNOSTIC ONLY, wrong output: bit0 = no mix loop, bit1 = no expand/contract loop */
#endif
#ifndef C512_FFN_PIPE
#define C512_FFN_PIPE 0 /* 2026-10-01 (results/c512-qkv-pipeline-20261001): NT==1 (split_ffn_one_w2) mix and expand/contract loops
 fully unrolled with a D-deep register ring (mix D=C512_FFN_PIPE steps, expand 2 hidden blocks) and sched_barrier per step;
 same loads, same WMMA order per accumulator -> bit-identical. 0 = off. */
#endif
#if C512_FFN_PIPE
 if constexpr(NT==1&&!(C512_FFN_ABLATE&1)){
  constexpr uint D=C512_FFN_PIPE;
  const float*ar=in+(first+rc())*512+gr()*8;const _Float16*wr=w16+(g*64+rc())*512+gr()*8;
  h8 ra[D][2],rb[D][4];
  auto ld=[&](uint s){const uint q=s%D;__builtin_memcpy(&ra[q][0],ar+s*16,16);__builtin_memcpy(&ra[q][1],ar+s*16+4,16);
   _Pragma("unroll") for(uint j=0;j<4;j++)__builtin_memcpy(&rb[q][j],wr+j*16*512+s*16,16);};
  _Pragma("unroll") for(uint s=0;s<D;s++)ld(s);
  _Pragma("unroll") for(uint s=0;s<32;s++){
   const uint q=s%D;float fa[8];__builtin_memcpy(fa,&ra[q][0],32);h8 cb[4];_Pragma("unroll") for(uint j=0;j<4;j++)cb[j]=rb[q][j];
   if(s+D<32)ld(s+D);
   __builtin_amdgcn_sched_barrier(0);
   h8 a;_Pragma("unroll") for(uint e=0;e<8;e++)a[e]=(_Float16)fa[e];
   _Pragma("unroll") for(uint j=0;j<4;j++)m[0][j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(cb[j],a,m[0][j]);
   __builtin_amdgcn_sched_barrier(0);
  }
 }else
#endif
 if constexpr(!(C512_FFN_ABLATE&1))
 for(uint k=0;k<((C512_FFN_ABLATE&4)?256u:512u);k+=16){h8 a[NT];for(uint t=0;t<NT;t++){a[t]=h8{};if(t==0||two)for(uint e=0;e<8;e++)a[t][e]=(_Float16)C5D_GRIDQ(in[(first+t*16+rc())*512+k+gr()*8+e]);}
#if C512_FFN_FP8MIX
  i2 a8[NT];for(uint t=0;t<NT;t++)a8[t]=c5d_pack8(a[t]);
  for(uint j=0;j<4;j++){h8 b;__builtin_memcpy(&b,w16+(g*64+j*16+rc())*512+k+gr()*8,16);i2 b8=c5d_pack8(b);for(uint t=0;t<NT;t++)m[t][j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b8,a8[t],m[t][j]);}}
#else
  for(uint j=0;j<4;j++){h8 b;__builtin_memcpy(&b,w16+(g*64+j*16+rc())*512+k+gr()*8,16);for(uint t=0;t<NT;t++)m[t][j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(b,a[t],m[t][j]);}}
#endif
 h8 x[NT][4];
 for(uint t=0;t<NT;t++)for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)x[t][j][e]=(_Float16)F(c512_hq(m[t][j][e]));
#if C512_FFN_FP8MIX>=2
 i2 x8[NT][4];for(uint t=0;t<NT;t++)for(uint j=0;j<4;j++)x8[t][j]=c5d_pack8(x[t][j]);
#endif
 f8 r[NT][4]{};
#if C512_FFN_PIPE
 if constexpr(NT==1&&!(C512_FFN_ABLATE&2)){
  h8 eb[2][4];i2 cbw[2][4];
  auto ldh=[&](uint hb){const uint q=hb&1;_Pragma("unroll") for(uint ks=0;ks<4;ks++)__builtin_memcpy(&eb[q][ks],we+(hb*16+rc())*64+ks*16+gr()*8,16);
   _Pragma("unroll") for(uint ob=0;ob<4;ob++){uint off=((ob*16+rc())*256+hb*16+gr()*8)/4;cbw[q][ob]=i2{int(pw[off]),int(pw[off+1])};}};
  ldh(0);ldh(1);
  _Pragma("unroll") for(uint hb=0;hb<16;hb++){
   const uint q=hb&1;h8 b4[4];i2 c4[4];_Pragma("unroll") for(uint j=0;j<4;j++){b4[j]=eb[q][j];c4[j]=cbw[q][j];}
   if(hb+2<16)ldh(hb+2);
   __builtin_amdgcn_sched_barrier(0);
   f8 ex{};_Pragma("unroll") for(uint ks=0;ks<4;ks++)ex=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(b4[ks],x[0][ks],ex);
   i2 a{};_Pragma("unroll") for(uint e=0;e<8;e++){float v=Hrtz(ex[e]),gate=clampf(v,-4.f,4.f),poly=__builtin_fmaf(gate,__builtin_fmaf(absf(gate),-.055908203125f,.447265625f),.89453125f);a[e/4]=int(uint(a[e/4])|(uint(byte_F(v*poly))<<(8*(e%4))));}
   _Pragma("unroll") for(uint ob=0;ob<4;ob++)r[0][ob]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,c4[ob],r[0][ob]);
   __builtin_amdgcn_sched_barrier(0);
  }
 }else
#endif
 if constexpr(!(C512_FFN_ABLATE&2))
 for(uint hb=0;hb<16;hb++){
  f8 ex[NT]{};
#if C512_FFN_FP8MIX>=2
  for(uint ks=0;ks<4;ks++){h8 b;__builtin_memcpy(&b,we+(hb*16+rc())*64+ks*16+gr()*8,16);i2 b8=c5d_pack8(b);for(uint t=0;t<NT;t++)ex[t]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b8,x8[t][ks],ex[t]);}
#else
  for(uint ks=0;ks<4;ks++){h8 b;__builtin_memcpy(&b,we+(hb*16+rc())*64+ks*16+gr()*8,16);for(uint t=0;t<NT;t++)ex[t]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(b,x[t][ks],ex[t]);}
#endif
  i2 a[NT];
  for(uint t=0;t<NT;t++){a[t]=i2{};for(uint e=0;e<8;e++){float v=Hrtz(ex[t][e]),gate=clampf(v,-4.f,4.f),poly=__builtin_fmaf(gate,__builtin_fmaf(absf(gate),-.055908203125f,.447265625f),.89453125f);a[t][e/4]=int(uint(a[t][e/4])|(uint(byte_F(v*poly))<<(8*(e%4))));}}
  for(uint ob=0;ob<4;ob++){uint off=((ob*16+rc())*256+hb*16+gr()*8)/4;i2 b={int(pw[off]),int(pw[off+1])};for(uint t=0;t<NT;t++)r[t][ob]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a[t],b,r[t][ob]);}
 }
 if constexpr((C512_FFN_ABLATE&2)!=0)for(uint t=0;t<NT;t++)for(uint ob=0;ob<4;ob++)for(uint e=0;e<8;e++)r[t][ob][e]=float(x[t][ob][e]);
 for(uint t=0;t<NT;t++){if(t&&!two)break;for(uint ob=0;ob<4;ob++)for(uint e=0;e<8;e++){uint rr=first+t*16+gr()*8+e,c=g*64+ob*16+rc();float v=F(c512_hq(r[t][ob][e]));out8[((rr/16)*16+c/32)*512+(rr%16)*32+c%32]=static_cast<unsigned char>(__builtin_amdgcn_cvt_pk_fp8_f32(v,0.f,0,false));}}
}
WAVE void split_ffn_one_m32(const float*in,const float*w,unsigned char*out8,uint tokens){
 const uint first=bid()/8*32,g=bid()%8;if(first>=tokens)return;c512_ffn_one_body<2>(in,w,out8,tokens,first,g);}
// C512_FFN_ONE 2: also export split_ffn_one_w2 (same grid, 2 waves per workgroup, 16 tokens per wave): twice the waves
// in flight for the same work; per-output operations unchanged. Host launches it with 64 threads when present.
#if C512_FFN_ONE >= 2
// C512_FFN_F8W (2026-10-01, results/c512-qkv-pipeline-20261001): split_ffn_one_w2f8 = split_ffn_one_w2 with the mix and expand
// weights packed as E4M3 by the host (they are exactly E4M3 values; host packer @split-mix-fp8) and both stages on fp8 WMMA.
// Block inputs and the expand operand are on the E4M3 lattice too, so every product/operand value equals the f16 path's;
// measured bit-identical (results §8). Mix A: f32 -> fp8 (exact). 0 = no export.
#ifndef C512_FFN_F8W
#define C512_FFN_F8W 0
#endif
#if C512_FFN_F8W
DEV i2 c5d_pack8f(const float*v){i2 r;for(uint i=0;i<2;i++){int w=__builtin_amdgcn_cvt_pk_fp8_f32(v[4*i],v[4*i+1],0,false);w=__builtin_amdgcn_cvt_pk_fp8_f32(v[4*i+2],v[4*i+3],w,true);r[i]=w;}return r;}
KERNEL __attribute__((amdgpu_flat_work_group_size(64,64))) void split_ffn_one_w2f8(const float*in,const float*w,unsigned char*out8,uint tokens){
 const uint wave=__builtin_amdgcn_workitem_id_x()/32,first=bid()/8*32+wave*16,g=bid()%8;if(first>=tokens)return;
 const unsigned char*wm=reinterpret_cast<const unsigned char*>(w);const unsigned char*we=wm+262144*4+g*16384;
 const uint*pw=reinterpret_cast<const uint*>(w)+393216+g*4096;
 f8 m[4]{};
 const float*ar=in+(first+rc())*512+gr()*8;
 for(uint k=0;k<512;k+=16){float fa[8];__builtin_memcpy(fa,ar+k,32);i2 a=c5d_pack8f(fa);
  for(uint j=0;j<4;j++){i2 b;__builtin_memcpy(&b,wm+(g*64+j*16+rc())*512+k+gr()*8,8);m[j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b,a,m[j]);}}
 i2 x8[4];
 for(uint j=0;j<4;j++){float xv[8];for(uint e=0;e<8;e++)xv[e]=F(c512_hq(m[j][e]));x8[j]=c5d_pack8f(xv);}
 f8 r[4]{};
 for(uint hb=0;hb<16;hb++){
  f8 ex{};
  for(uint ks=0;ks<4;ks++){i2 b;__builtin_memcpy(&b,we+(hb*16+rc())*64+ks*16+gr()*8,8);ex=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b,x8[ks],ex);}
  i2 a{};for(uint e=0;e<8;e++){float v=Hrtz(ex[e]),gate=clampf(v,-4.f,4.f),poly=__builtin_fmaf(gate,__builtin_fmaf(absf(gate),-.055908203125f,.447265625f),.89453125f);a[e/4]=int(uint(a[e/4])|(uint(byte_F(v*poly))<<(8*(e%4))));}
  for(uint ob=0;ob<4;ob++){uint off=((ob*16+rc())*256+hb*16+gr()*8)/4;i2 b={int(pw[off]),int(pw[off+1])};r[ob]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,r[ob]);}
 }
 for(uint ob=0;ob<4;ob++)for(uint e=0;e<8;e++){uint rr=first+gr()*8+e,c=g*64+ob*16+rc();float v=F(c512_hq(r[ob][e]));out8[((rr/16)*16+c/32)*512+(rr%16)*32+c%32]=static_cast<unsigned char>(__builtin_amdgcn_cvt_pk_fp8_f32(v,0.f,0,false));}
}
#endif
KERNEL __attribute__((amdgpu_flat_work_group_size(64,64))) void split_ffn_one_w2(const float*in,const float*w,unsigned char*out8,uint tokens){
 const uint wave=__builtin_amdgcn_workitem_id_x()/32,first=bid()/8*32+wave*16,g=bid()%8;if(first>=tokens)return;c512_ffn_one_body<1>(in,w,out8,tokens,first,g);}
#endif
#endif
