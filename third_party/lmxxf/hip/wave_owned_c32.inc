// One complete 64-token C32 window per wave; chain FP8 input/output.
// Test20 candidate: keep normalized attention probabilities in half precision.
// E4M3 subnormal steps are coarse near the usual 1/64 probability. This changes
// network numerics; opt in at module build time, not as a bit-exact optimization.
#ifndef CW_PROB_HALF
#define CW_PROB_HALF 0
#endif
#ifndef CW_AV_HALF
#define CW_AV_HALF 0
#endif
#ifndef CW_QK_HALF
#define CW_QK_HALF 0
#endif
#ifndef CW_FEATURE_HALF
#define CW_FEATURE_HALF 0
#endif
#ifndef CW_V_HALF
#define CW_V_HALF 0
#endif
#if CW_AV_HALF && !CW_PROB_HALF
#error CW_AV_HALF requires CW_PROB_HALF
#endif
#ifndef CW_LAUNDER
#define CW_LAUNDER 0
#endif
#ifndef CW_ROLL_HIDDEN
#define CW_ROLL_HIDDEN 0
#endif
#ifndef CW_QKV_FENCE
#define CW_QKV_FENCE 0
#endif
#ifndef CW_ROLL_WINDOW
#define CW_ROLL_WINDOW 0
#endif
#ifndef CW_PREFIX_SPLIT
#define CW_PREFIX_SPLIT 0 /* 1: prefix features split across both half-waves (bit-exact; 1080 -0.03ms, results/vit-attn-c32-input-20260926) */
#endif
#ifndef CW_TAIL_QUAD
#define CW_TAIL_QUAD 0 /* 1: finish/prefix tail walks 2x2 quads once: 4 LDS reads feed both main and down (was 64+64 reads), 32-bit byte offsets for stores; arithmetic unchanged */
#endif
#ifndef CW_VEC_INPUT
#define CW_VEC_INPUT 0 /* 1: mapped/post input rows read as b128/b64 instead of per-element loads (bit-exact; results/c32-wave-phase-20260926) */
#endif
// C32 ACO audit (Development/results/c32-aco-20260927). Switches default off; the build recipe selects the accepted pair.
#ifndef CW_FMED3_CLAMP
#define CW_FMED3_CLAMP 0
#endif
#ifndef CW_DIRECT_OUT
#define CW_DIRECT_OUT 0
#endif
#ifndef CW_RTZ_PAIR
#define CW_RTZ_PAIR 0
#endif
DEV float cw_clampf(float x,float lo,float hi){
#if CW_FMED3_CLAMP
 return __builtin_amdgcn_fmed3f(x,lo,hi);
#else
 return clampf(x,lo,hi);
#endif
}
// Two independent RTZ conversions in the instruction we already use; no packed-half arithmetic,
// no MODE change, and no contraction across an arithmetic/rounding boundary.
#if CW_RTZ_PAIR
using cw_h2=_Float16 __attribute__((ext_vector_type(2)));
DEV h8 cw_rtz_half8(f8 x){h8 out;_Pragma("unroll") for(uint j=0;j<4;j++){
 uint packed;asm("v_cvt_pkrtz_f16_f32 %0, %1, %2" : "=v"(packed) : "v"(x[2*j]),"v"(x[2*j+1]));
 cw_h2 pair=__builtin_bit_cast(cw_h2,packed);out[2*j]=pair[0];out[2*j+1]=pair[1];}return out;}
#endif
DEV float cw_output_value(float v){
#if CW_DIRECT_OUT
 // fp8(F(v)) == fp8(v), except F canonicalizes an exact -0. Preserve underflow-generated -0.
 return v+0.f;
#else
 return F(float((_Float16)v));
#endif
}
#ifndef CW_PREFIX_DIRECT_OUT
#define CW_PREFIX_DIRECT_OUT 0 /* prefix byte tail: avoid fp8(F(half-value)); retain exact-zero canonicalization */
#endif
#ifndef CW_PREFIX_FULL_TILE
#define CW_PREFIX_FULL_TILE 0 /* prefix only: host emits complete 8x8 tiles, sx=sy=0, so its per-pixel bounds tests are redundant */
#endif
#ifndef CW_POST_HEAD_VEC
#define CW_POST_HEAD_VEC 0 /* RGB head: read each contiguous eight-half group together, retain channel reduction order */
#endif
#ifndef CW_POST_FULL_TILE
#define CW_POST_FULL_TILE 0 /* specialize interior RGB output windows once per wave */
#endif
#ifndef CW_FINISH_FULL_TILE
#define CW_FINISH_FULL_TILE 0 /* specialize interior main/down output windows once per wave */
#endif
DEV uint cw_prefix_byte(float v){
#if CW_PREFIX_DIRECT_OUT
 return fp8(v+0.f);
#else
 return fp8(F(v));
#endif
}
struct cw_true {constexpr operator bool()const{return true;}};
struct cw_false {constexpr operator bool()const{return false;}};
using cw_i16=int __attribute__((ext_vector_type(16)));
// CW_PACK8 (2026-09-27, results/pack8-20260927): the same bytes as put_bits(a,e,fp8(x)) -- the identical clamp, then
// v_cvt_pk_fp8_f32 on two values at once written straight into the fragment word instead of one cvt + and/shift/or per byte.
// fp8() has no -0 select in C32, so nothing else changes.
#ifndef CW_PACK8
#define CW_PACK8 0
#endif
#if CW_PACK8
#if HIP_FP8_SAT_MODE==3
DEV float cw_z(float x){return __builtin_amdgcn_fmed3f(x,-448.f,448.f);}
#elif HIP_C32_BRANCHLESS_F
DEV float cw_z(float x){return __builtin_fminf(__builtin_fmaxf(x,-448.f),448.f);}
#else
DEV float cw_z(float x){return clampf(x,-448.f,448.f);}
#endif
#ifndef CW_PACK_CENSUS
#define CW_PACK_CENSUS 0 /* diagnostic only: per-kind, per-site pack counts / nonfinite / >448 / >1 / maximum absolute finite value */
#endif
#ifndef CW_PACK_MODE_MASK
#define CW_PACK_MODE_MASK 0 /* bit per pack site: input, hidden, feature, QKV, probability, AV, output */
#endif
#if CW_PACK_CENSUS
extern "C" __attribute__((device)) uint cw_pack_census[6*7*5]={};
template<uint Site,uint Kind> DEV void cw_census(const float*t){
 uint lane=__builtin_amdgcn_workitem_id_x(),flags=0,mx=0;
 for(uint e=0;e<8;e++){uint ab=bits(t[e])&0x7fffffffu;flags|=(ab>=0x7f800000u?1u:0u)|(ab>0x43e00000u?2u:0u)|(ab>0x3f800000u?4u:0u);if(ab<0x7f800000u&&ab>mx)mx=ab;}
 for(uint sh=16;sh;sh>>=1){uint other=uint(__builtin_amdgcn_ds_bpermute((lane^sh)*4,int(mx)));if(other>mx)mx=other;flags|=uint(__builtin_amdgcn_ds_bpermute((lane^sh)*4,int(flags)));}
 if(lane==0){uint*p=cw_pack_census+(Kind*7+Site)*5;__hip_atomic_fetch_add(p,1u,__ATOMIC_RELAXED,__HIP_MEMORY_SCOPE_AGENT);
  if(flags&1)__hip_atomic_fetch_add(p+1,1u,__ATOMIC_RELAXED,__HIP_MEMORY_SCOPE_AGENT);
  if(flags&2)__hip_atomic_fetch_add(p+2,1u,__ATOMIC_RELAXED,__HIP_MEMORY_SCOPE_AGENT);
  if(flags&4)__hip_atomic_fetch_add(p+3,1u,__ATOMIC_RELAXED,__HIP_MEMORY_SCOPE_AGENT);
  __hip_atomic_fetch_max(p+4,mx,__ATOMIC_RELAXED,__HIP_MEMORY_SCOPE_AGENT);}
}
#endif
template<uint Site,uint Kind> DEV i2 cw_pack8(const float*t){
#if CW_PACK_CENSUS
 cw_census<Site,Kind>(t);
#endif
// Keep MODE and its four conversions indivisible: LLVM can hoist the FP8 intrinsics outside s_setreg.
 if constexpr((CW_PACK_MODE_MASK&(1u<<Site))!=0){int lo,hi;
  asm volatile("s_setreg_imm32_b32 hwreg(HW_REG_MODE, 23, 1), 1\n\t"
               "v_cvt_pk_fp8_f32 %0, %2, %3\n\t"
               "v_cvt_pk_fp8_f32 %1, %6, %7\n\t"
               "v_cvt_pk_fp8_f32 %0, %4, %5 op_sel:[0,0,1]\n\t"
               "v_cvt_pk_fp8_f32 %1, %8, %9 op_sel:[0,0,1]\n\t"
               "s_setreg_imm32_b32 hwreg(HW_REG_MODE, 23, 1), 0"
               : "=&v"(lo),"=&v"(hi) : "v"(t[0]),"v"(t[1]),"v"(t[2]),"v"(t[3]),"v"(t[4]),"v"(t[5]),"v"(t[6]),"v"(t[7]) : "memory");
  return i2{lo,hi};}
 int lo=__builtin_amdgcn_cvt_pk_fp8_f32(cw_z(t[0]),cw_z(t[1]),0,false);lo=__builtin_amdgcn_cvt_pk_fp8_f32(cw_z(t[2]),cw_z(t[3]),lo,true);
 int hi=__builtin_amdgcn_cvt_pk_fp8_f32(cw_z(t[4]),cw_z(t[5]),0,false);hi=__builtin_amdgcn_cvt_pk_fp8_f32(cw_z(t[6]),cw_z(t[7]),hi,true);
 i2 a;a[0]=lo;a[1]=hi;return a;}

#define CW_Q8_DECL(A) i2 A;float A##_t[8]
#define CW_Q8_SET(A,E,X) A##_t[E]=(X)
#define CW_Q8_END_SITE(A,S) A=cw_pack8<S,cw_kind>(A##_t)
#define CW_Q8_END(A) CW_Q8_END_SITE(A,0)
#else
#define CW_Q8_DECL(A) i2 A{}
#define CW_Q8_SET(A,E,X) put_bits(A,E,fp8(X))
#define CW_Q8_END(A) (void)0
#define CW_Q8_END_SITE(A,S) (void)0
#endif

DEV i2 cw_input(const unsigned char*in,uint win,uint qt,uint ci,uint width,uint height,uint sx,uint sy,uint prevw,uint px,uint py,uint r,uint g){
 uint ww=width+2*sx;int x=int((win%(ww/8))*8+(qt*16+r)%8)-int(sx),y=int((win/(ww/8))*8+(qt*16+r)/8)-int(sy);i2 a{};
 if(x>=0&&y>=0&&x<int(width)&&y<int(height)){uint xx=uint(x)+px,yy=uint(y)+py,tile=(yy/8)*(prevw/8)+xx/8,tok=(yy%8)*8+xx%8;__builtin_memcpy(&a,in+(tile*64+tok)*32+ci*16+g*8,8);}return a;
}
template<bool Chain,bool Finish=false,bool DownCrop=false,bool Post=false,bool Prefix=false>
DEV void cw_body(const unsigned char*in,const float*fw,const float*w,unsigned char*out,uint windows,uint mode,uint raw,uint width,uint height,uint sx,uint sy,uint prevw,uint px,uint py,float*main=nullptr,float*down=nullptr,const unsigned char*skip=nullptr,const float*scales=nullptr,const float*color=nullptr,const float*headw=nullptr,float*rgb=nullptr,float rgbscale=0.f,const float*history=nullptr,uint seed=0,uint temporal=0){fp8_sat_mode();
 constexpr uint cw_kind=Prefix?5:Post?4:Finish?(DownCrop?3:2):Chain?0:1;
 uint win=bid();if(win>=windows)return;
 __attribute__((shared)) _Float16 residual[4*2*32*8];
 cw_i16 keys{},values{},queries{};
#if CW_QK_HALF
 h8 half_keys[8],half_queries[8];
#endif
#if CW_V_HALF
 h8 half_values[8];
#endif
#if CW_ROLL_WINDOW
 _Pragma("clang loop unroll(disable)")
#else
 _Pragma("unroll 4")
#endif
 for(uint qt=0;qt<4;qt++){
  uint lane=__builtin_amdgcn_workitem_id_x();
#if CW_LAUNDER
  __asm__ volatile("" : "+v"(lane));lane&=31u;
#endif
  uint r=lane&15u,g=lane>>4;
  i2 input[2];f8 ffn[2]{};
  if constexpr(Chain){for(uint ci=0;ci<2;ci++)input[ci]=cw_input(in,win,qt,ci,width,height,sx,sy,prevw,px,py,r,g);}
  else if constexpr(Prefix){
#if CW_PREFIX_SPLIT
   // B1 prefix split: fused_prefix_values ran on the g==0 half only (16 lanes idle) and then shipped values 8..15 with eight
   // bpermutes. Here both halves run the same instruction stream on the same token r: the half-wave selects which Box-Muller
   // pair it evaluates (g==1: a,c -> g0; g==0: b,d -> g1,g2), and g0 is the only value exchanged (one bpermute). Each value's
   // operation sequence is identical to fused_prefix_values, so the features, and everything downstream, are bit-identical.
   h8 a{};
   {const float*rgba=reinterpret_cast<const float*>(in);uint p=win*64+qt*16+r,tile=p/64,x=(tile%(width/8))*8+p%8,y=(tile/(width/8))*8+(p%64)/8,rgbp=y*width+x;
    uint h=production_pcg((x*0x8da6b343u)^(y*0xd8163841u)^(seed*0x9e3779b9u)^0x243f6a88u);
    float u1=production_uniform(g?h*0xcaa5b80du+0x21dd796bu:h*0x2c9277b5u+0xac564b05u),u2=production_uniform(g?h*0x83232c31u+0x3463e0acu:h*0xfa6dc5f9u+0x4712a88eu);
    float rr=__builtin_amdgcn_sqrtf(-2.f*(__builtin_amdgcn_logf(u1)*.6931471805599453f)),tt=(6.283185482025146f*u2)*.15915494309189535f;
    float gc=Hrtz(rr*__builtin_amdgcn_cosf(tt)),gs=Hrtz(rr*__builtin_amdgcn_sinf(tt));
    float g0=__builtin_bit_cast(float,__builtin_amdgcn_ds_bpermute((r+16)*4,__builtin_bit_cast(int,gc)));
    float red=Hrtz(Hrtz(Hrtz(rgba[rgbp*4])-.5f)*.125f),green=Hrtz(Hrtz(Hrtz(rgba[rgbp*4+1])-.5f)*.125f),blue=Hrtz(Hrtz(Hrtz(rgba[rgbp*4+2])-.5f)*.125f);
    float hr=red,hg=green,hb=blue;
    if(temporal){uint q=(y*width+x)*4;hr=Hrtz(Hrtz(Hrtz(history[q])-.5f)*.125f);hg=Hrtz(Hrtz(Hrtz(history[q+1])-.5f)*.125f);hb=Hrtz(Hrtz(Hrtz(history[q+2])-.5f)*.125f);}
    const float lo[8]={gc,gs,hg,hb,g0,1,.0078125f,1},hi[8]={red,green,1,1,blue,hr,1,0};
    for(uint e=0;e<8;e++)a[e]=(_Float16)(g?hi[e]:lo[e]);}
#else
   float pv[16]{};if(g==0)fused_prefix_values(reinterpret_cast<const float*>(in),history,win*64+qt*16+r,width,seed,temporal,pv);
   h8 a{};for(uint e=0;e<8;e++){float hi=__builtin_bit_cast(float,__builtin_amdgcn_ds_bpermute(r*4,__builtin_bit_cast(int,pv[8+e])));a[e]=(_Float16)(g?hi:pv[e]);}
#endif
   for(uint ci=0;ci<2;ci++){
    h8 b{};for(uint e=0;e<8;e++)b[e]=(_Float16)fw[(ci*16+r)*16+g*8+e];
    f8 pre=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(b,a,f8{});h8 zero{};
    pre=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(zero,zero,pre);
    CW_Q8_DECL(packed);for(uint e=0;e<8;e++){float v=Hrtz(pre[e]);CW_Q8_SET(packed,e,v);ffn[ci][e]=cw_clampf(v,-448.f,448.f)*fw[8704+ci*16+g*8+e];}CW_Q8_END(packed);input[ci]=packed;
   }
  }
  else{
   uint ww=width+2*sx;int x=int((win%(ww/8))*8+(qt*16+r)%8)-int(sx),y=int((win/(ww/8))*8+(qt*16+r)/8)-int(sy);
   bool valid=x>=0&&y>=0&&x<int(width)&&y<int(height);
#if CW_VEC_INPUT
   // The lane's 8 channels c=ci*16+g*8..+7 are contiguous in every source (f32 features, FP8 skip bytes, scales, FFN residual
   // scale), 32-byte aligned (hipMalloc bases, channel offsets multiple of 8). Read them as whole rows (2x b128 / 1x b64)
   // instead of 8 per-element loads each; the per-element arithmetic and its order are unchanged.
   for(uint ci=0;ci<2;ci++){uint c0=ci*16+g*8;float src[8]{};
    if(valid){
     if constexpr(Post){uint p=uint(y)*width+uint(x),low=(uint(y)/2)*(width/2)+uint(x)/2;
      float lo[8],s0[8],s1[8];unsigned long long sk;
      __builtin_memcpy(lo,__builtin_assume_aligned(reinterpret_cast<const float*>(in)+low*32+c0,32),32);
      __builtin_memcpy(s0,__builtin_assume_aligned(scales+c0,32),32);__builtin_memcpy(s1,__builtin_assume_aligned(scales+32+c0,32),32);
      __builtin_memcpy(&sk,__builtin_assume_aligned(skip+p*32+c0,8),8);
      for(uint e=0;e<8;e++){float high=__builtin_amdgcn_cvt_f32_fp8(int((sk>>(8*e))&255u),0);src[e]=Hrtz(Hrtz(lo[e]*s0[e])+high*s1[e]);}
     }else __builtin_memcpy(src,__builtin_assume_aligned(reinterpret_cast<const float*>(in)+(uint(y)*width+uint(x))*32+c0,32),32);
    }
    float res[8];__builtin_memcpy(res,__builtin_assume_aligned(fw+8704+c0,32),32);
    CW_Q8_DECL(a);for(uint e=0;e<8;e++){float v=src[e];CW_Q8_SET(a,e,v);ffn[ci][e]=cw_clampf(float((_Float16)v),-448.f,448.f)*res[e];}CW_Q8_END(a);input[ci]=a;}
#else
   for(uint ci=0;ci<2;ci++){CW_Q8_DECL(a);for(uint e=0;e<8;e++){uint c=ci*16+g*8+e;float v=0.f;if(valid){if constexpr(Post){uint p=uint(y)*width+uint(x),low=(uint(y)/2)*(width/2)+uint(x)/2;float high=__builtin_amdgcn_cvt_f32_fp8(int(skip[p*32+c]),0);v=Hrtz(Hrtz(reinterpret_cast<const float*>(in)[low*32+c]*scales[c])+high*scales[32+c]);}else v=reinterpret_cast<const float*>(in)[(uint(y)*width+uint(x))*32+c];}CW_Q8_SET(a,e,v);ffn[ci][e]=cw_clampf(float((_Float16)v),-448.f,448.f)*fw[8704+c];}CW_Q8_END(a);input[ci]=a;}
#endif
  }
  if constexpr(Chain)for(uint part=0;part<3;part++)for(uint ci=0;ci<2;ci++)for(uint kt=0;kt<2;kt++){
   i2 b=matrix8(fw,34944+((part*2+ci)*2+kt)*512+(g*16+r)*8);
   ffn[ci]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b,input[kt],ffn[ci]);
  }
#if CW_ROLL_HIDDEN
  _Pragma("clang loop unroll(disable)")
#endif
  for(uint ht=0;ht<8;ht++){
   f8 expanded{};for(uint kt=0;kt<2;kt++){i2 b=matrix8(fw+512,(ht*16+r)*32+kt*16+g*8);expanded=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b,input[kt],expanded);}
   CW_Q8_DECL(a);for(uint e=0;e<8;e++){float v=expanded[e],g=cw_clampf(v,-4.f,4.f),q=absf(g)*(-.055908203125f)+.447265625f,p=g*q+.89453125f;CW_Q8_SET(a,e,v*p);}CW_Q8_END_SITE(a,1);
   for(uint ci=0;ci<2;ci++){i2 b=matrix8(fw+4608,(ci*16+r)*128+ht*16+g*8);ffn[ci]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b,a,ffn[ci]);}
  }
  i2 feature[2];
#if CW_FEATURE_HALF
  h8 half_feature[2];
#endif
  for(uint ci=0;ci<2;ci++){CW_Q8_DECL(a);
#if CW_RTZ_PAIR
   h8 rounded=cw_rtz_half8(ffn[ci]);__builtin_memcpy(residual+((qt*2+ci)*32+(g*16+r))*8,&rounded,16);
   for(uint e=0;e<8;e++)CW_Q8_SET(a,e,float(rounded[e]));
#else
   for(uint e=0;e<8;e++){float v=Hrtz(ffn[ci][e]);residual[((qt*2+ci)*32+(g*16+r))*8+e]=(_Float16)v;CW_Q8_SET(a,e,v);}
#endif
   CW_Q8_END_SITE(a,2);feature[ci]=a;
#if CW_FEATURE_HALF
   for(uint e=0;e<8;e++)half_feature[ci][e]=(_Float16)cw_clampf(float(residual[((qt*2+ci)*32+(g*16+r))*8+e]),-448.f,448.f);
#endif
  }
  for(uint part=0;part<3;part++){
   uint qlane=__builtin_amdgcn_workitem_id_x();
#if CW_QKV_FENCE
   __asm__ volatile("" : "+v"(qlane));qlane&=31u;
#endif
   uint qr=qlane&15u,qg=qlane>>4;
   f8 q[2]{};for(uint ci=0;ci<2;ci++)for(uint kt=0;kt<2;kt++){i2 b=matrix8(w,part*1024+(ci*16+qr)*32+kt*16+qg*8);
#if CW_FEATURE_HALF
    h8 bh{};for(uint e=0;e<8;e++)bh[e]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int((uint(b[e/4])>>(8*(e%4)))&255u),0);
    q[ci]=part<2?__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(bh,half_feature[kt],q[ci]):__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(half_feature[kt],bh,q[ci]);
#else
    q[ci]=part<2?__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b,feature[kt],q[ci]):__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(feature[kt],b,q[ci]);
#endif
   }
   f8 sum{};if(part<2)for(uint ci=0;ci<2;ci++){h8 squares{},ones{};for(uint e=0;e<8;e++){squares[e]=(_Float16)(q[ci][e]*q[ci][e]);ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(ones,squares,sum);}
   for(uint ci=0;ci<2;ci++){CW_Q8_DECL(a);for(uint e=0;e<8;e++){float v=q[ci][e];if(part<2)v*=__builtin_amdgcn_rsqf(maxf(sum[e],6.198883056640625e-5f))*(part==0?w[8192]:1.f);
#if CW_QK_HALF
    if(part==0)half_queries[qt*2+ci][e]=(_Float16)cw_clampf(v,-448.f,448.f);
    if(part==1)half_keys[qt*2+ci][e]=(_Float16)cw_clampf(v,-448.f,448.f);
#endif
#if CW_V_HALF
    if(part==2)half_values[qt*2+ci][e]=(_Float16)cw_clampf(v,-448.f,448.f);
#endif
    CW_Q8_SET(a,e,v);}CW_Q8_END_SITE(a,3);
    if(part==0){queries[qt*4+ci*2]=a[0];queries[qt*4+ci*2+1]=a[1];}else if(part==1){keys[qt*4+ci*2]=a[0];keys[qt*4+ci*2+1]=a[1];}else{values[qt*4+ci*2]=a[0];values[qt*4+ci*2+1]=a[1];}
   }
#if CW_QKV_FENCE
   __builtin_amdgcn_sched_barrier(0);
#endif
  }
  __builtin_amdgcn_sched_barrier(0);
 }
 _Pragma("clang loop unroll(disable)") for(uint qt=0;qt<4;qt++){
  uint lane=__builtin_amdgcn_workitem_id_x();
#if CW_LAUNDER
  __asm__ volatile("" : "+v"(lane));lane&=31u;
#endif
  uint r=lane&15u,g=lane>>4;
  i2 q[2];for(uint ci=0;ci<2;ci++)q[ci]=i2{queries[qt*4+ci*2],queries[qt*4+ci*2+1]};
  h8 ex[4];
  _Pragma("unroll 4") for(uint kt=0;kt<4;kt++){
   f8 acc{};for(uint ci=0;ci<2;ci++){
#if CW_QK_HALF
    acc=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(half_keys[kt*2+ci],half_queries[qt*2+ci],acc);
#else
    acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12((i2{keys[kt*4+ci*2],keys[kt*4+ci*2+1]}),q[ci],acc);
#endif
   }
   h8 a{};for(uint e=0;e<8;e++){uint row=qt*16+r,k=kt*16+g*8+e;float score=acc[e]+w[4096+row*64+k],affine=cw_clampf(score*.044921875f+1.30078125f,1.03125f,1.5693359375f);uint ah=(bits(affine)>>13)-0x1c000u;ushort hb=static_cast<ushort>(((ah<<5)+0x8000u)&65535u);a[e]=__builtin_bit_cast(_Float16,hb);}ex[kt]=a;
  }
  f8 sums{};h8 ones{};for(uint e=0;e<8;e++)ones[e]=(_Float16)1.f;
  _Pragma("unroll 4") for(uint kt=0;kt<4;kt++)sums=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(ones,ex[kt],sums);
#if CW_PROB_HALF
  h8 prob[4];
  _Pragma("unroll 4") for(uint kt=0;kt<4;kt++){
   h8 a{};
   for(uint e=0;e<8;e++)a[e]=(_Float16)(float(ex[kt][e])*norm_inverse(sums[e]));
   prob[kt]=a;
  }
#if CW_AV_HALF
  h8 av[2];
#else
  i2 av[2];
#endif
  for(uint ci=0;ci<2;ci++){
   f8 acc{};
   _Pragma("unroll 4") for(uint kt=0;kt<4;kt++){
    // Preserve the existing transposed V fragment layout and FP8 V values.
    // Every finite E4M3 value is exactly representable as a half.
    h8 v{};
#if CW_V_HALF
    v=half_values[kt*2+ci];
#else
    for(uint e=0;e<8;e++){
     uint word=uint(values[kt*4+ci*2+e/4]);
     v[e]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int((word>>(8*(e%4)))&255u),0);
    }
#endif
    acc=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(v,prob[kt],acc);
   }
#if CW_AV_HALF
   for(uint e=0;e<8;e++)av[ci][e]=(_Float16)cw_clampf(acc[e],-448.f,448.f);
#else
   CW_Q8_DECL(a);
   for(uint e=0;e<8;e++)CW_Q8_SET(a,e,acc[e]);
   CW_Q8_END_SITE(a,5);av[ci]=a;
#endif
  }
#else
  i2 prob[4];_Pragma("unroll 4") for(uint kt=0;kt<4;kt++){CW_Q8_DECL(a);for(uint e=0;e<8;e++)CW_Q8_SET(a,e,float(ex[kt][e])*norm_inverse(sums[e]));CW_Q8_END_SITE(a,4);prob[kt]=a;}
  i2 av[2];for(uint ci=0;ci<2;ci++){f8 acc{};_Pragma("unroll 4") for(uint kt=0;kt<4;kt++)acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12((i2{values[kt*4+ci*2],values[kt*4+ci*2+1]}),prob[kt],acc);CW_Q8_DECL(a);for(uint e=0;e<8;e++)CW_Q8_SET(a,e,acc[e]);CW_Q8_END_SITE(a,5);av[ci]=a;}
#endif
  for(uint ci=0;ci<2;ci++){
   f8 acc{};for(uint kt=0;kt<2;kt++){i2 b=matrix8(w,3072+(ci*16+r)*32+kt*16+g*8);
#if CW_AV_HALF
    h8 bh{};for(uint e=0;e<8;e++)bh[e]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int((uint(b[e/4])>>(8*(e%4)))&255u),0);
    acc=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(bh,av[kt],acc);
#else
    acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b,av[kt],acc);
#endif
   }
   CW_Q8_DECL(a);
#if CW_RTZ_PAIR
   f8 summed;for(uint e=0;e<8;e++){float res=float(residual[((qt*2+ci)*32+(g*16+r))*8+e]),prod=res*w[8193+ci*16+g*8+e];summed[e]=acc[e]+prod;}
   h8 rounded=cw_rtz_half8(summed);
   if constexpr(Finish||Post)__builtin_memcpy(residual+((qt*2+ci)*32+(g*16+r))*8,&rounded,16);
   else for(uint e=0;e<8;e++)CW_Q8_SET(a,e,cw_output_value(float(rounded[e])));
#else
   for(uint e=0;e<8;e++){float res=float(residual[((qt*2+ci)*32+(g*16+r))*8+e]),prod=res*w[8193+ci*16+g*8+e],v=Hrtz(acc[e]+prod);if constexpr(Finish||Post)residual[((qt*2+ci)*32+(g*16+r))*8+e]=(_Float16)v;else CW_Q8_SET(a,e,cw_output_value(v));}
#endif
   if constexpr(!Finish&&!Post)CW_Q8_END_SITE(a,6);
   if constexpr(!Finish&&!Post)__builtin_memcpy(out+(win*64+qt*16+r)*32+ci*16+g*8,&a,8);
  }
  __builtin_amdgcn_sched_barrier(0);
 }

#if CW_POST_HEAD_VEC || CW_POST_FULL_TILE
 if constexpr(Post){
  WG_FENCE(3);WG_FENCE(2);
  uint lane=__builtin_amdgcn_workitem_id_x(),ww=width+2*sx,tx=(win%(ww/8))*8,ty=(win/(ww/8))*8;
  auto emit=[&](auto full){
  for(uint t=lane;t<64;t+=32){int x=int(tx+t%8)-int(sx),y=int(ty+t/8)-int(sy);if(full||(x>=0&&y>=0&&x<int(width)&&y<int(height))){
   uint p=uint(y)*width+uint(x);float sums[3]{};
#if CW_POST_HEAD_VEC
   for(uint cg=0;cg<4;cg++){h8 fs;float hr[8],hg[8],hb[8];uint c=cg*8;
    __builtin_memcpy(&fs,residual+((t/16*2+c/16)*32+(c%16/8*16+t%16))*8,16);
    __builtin_memcpy(hr,headw+c,32);__builtin_memcpy(hg,headw+32+c,32);__builtin_memcpy(hb,headw+64+c,32);
    for(uint e=0;e<8;e++){float f=float(fs[e]);sums[0]+=f*hr[e];sums[1]+=f*hg[e];sums[2]+=f*hb[e];}}
#else
   for(uint c=0;c<32;c++){float f=float(residual[((t/16*2+c/16)*32+(c%16/8*16+t%16))*8+c%8]);sums[0]+=f*headw[c];sums[1]+=f*headw[32+c];sums[2]+=f*headw[64+c];}
#endif
   for(uint c=0;c<3;c++){float acc=Hrtz(sums[c]),centered=color[p*4+c]*.125f-.0625f,v=(acc*rgbscale+centered)*8.f+.5f;rgb[p*3+c]=cw_clampf(v,0.f,1.f);}
  }}};
#if CW_POST_FULL_TILE
  if(tx>=sx&&ty>=sy&&tx+8<=width+sx&&ty+8<=height+sy)emit(cw_true{});else emit(cw_false{});
#else
  emit(cw_false{});
#endif
 }
#else
 if constexpr(Post){
  WG_FENCE(3);WG_FENCE(2);
  uint lane=__builtin_amdgcn_workitem_id_x(),ww=width+2*sx,tx=(win%(ww/8))*8,ty=(win/(ww/8))*8;
  for(uint t=lane;t<64;t+=32){int x=int(tx+t%8)-int(sx),y=int(ty+t/8)-int(sy);if(x>=0&&y>=0&&x<int(width)&&y<int(height)){
   uint p=uint(y)*width+uint(x);float sums[3]{};
   for(uint c=0;c<32;c++){float f=float(residual[((t/16*2+c/16)*32+(c%16/8*16+t%16))*8+c%8]);sums[0]+=f*headw[c];sums[1]+=f*headw[32+c];sums[2]+=f*headw[64+c];}
   for(uint c=0;c<3;c++){float acc=Hrtz(sums[c]),centered=color[p*4+c]*.125f-.0625f,v=(acc*rgbscale+centered)*8.f+.5f;rgb[p*3+c]=cw_clampf(v,0.f,1.f);}
  }}
 }
#endif
 if constexpr(CW_FINISH_FULL_TILE&&!Prefix){
 if constexpr(Finish){
  WG_FENCE(3);WG_FENCE(2);
  uint c=__builtin_amdgcn_workitem_id_x(),ww=width+2*sx,tx=(win%(ww/8))*8,ty=(win/(ww/8))*8;
  auto load=[&](uint t)->float{return float(residual[((t/16*2+c/16)*32+(c%16/8*16+t%16))*8+c%8]);};
#if CW_TAIL_QUAD
  auto put_main=[&](uint t,float raw_v){int x=int(tx+t%8)-int(sx),y=int(ty+t/8)-int(sy);if(x>=0&&y>=0&&x<int(width)&&y<int(height)){uint dest=(uint(y)*width+uint(x))*32+c;float v=F(raw_v);
   if constexpr(Prefix)*reinterpret_cast<unsigned char*>(reinterpret_cast<char*>(main)+dest)=static_cast<unsigned char>(fp8(v));else *reinterpret_cast<float*>(reinterpret_cast<char*>(main)+dest*4u)=v;}};
  for(uint k=0;k<16;k++){uint t=(k/4)*16+(k%4)*2;float a0=load(t),a1=load(t+1),b0=load(t+8),b1=load(t+9);
   if(main){put_main(t,a0);put_main(t+1,a1);put_main(t+8,b0);put_main(t+9,b1);}
   if(down){float top=Hrtz(a0+a1),bottom=Hrtz(b0+b1);uint x=tx+t%8,y=ty+t/8;
    if constexpr(DownCrop){int cx=int(x/2)-int(sx/2),cy=int(y/2)-int(sy/2);if(cx>=0&&cy>=0&&cx<int(width/2)&&cy<int(height/2))*reinterpret_cast<float*>(reinterpret_cast<char*>(down)+((uint(cy)*(width/2)+uint(cx))*32+c)*4u)=F(Hrtz(Hrtz(top+bottom)*.25f));}
    else *reinterpret_cast<float*>(reinterpret_cast<char*>(down)+(((y/2)*(ww/2)+x/2)*32+c)*4u)=F(Hrtz(Hrtz(top+bottom)*.25f));}
  }
  // The quad route above emits both outputs.
#endif
  auto emit=[&](auto full){if(main)for(uint t=0;t<64;t++){int x=int(tx+t%8)-int(sx),y=int(ty+t/8)-int(sy);if(full||(Prefix&&CW_PREFIX_FULL_TILE)||(x>=0&&y>=0&&x<int(width)&&y<int(height))){uint dest=(uint(y)*width+uint(x))*32+c;float v=load(t);if constexpr(Prefix)reinterpret_cast<unsigned char*>(main)[dest]=static_cast<unsigned char>(cw_prefix_byte(v));else main[dest]=F(v);}}
  if(!CW_TAIL_QUAD&&down)for(uint k=0;k<16;k++){uint t=(k/4)*16+(k%4)*2;float top=Hrtz(load(t)+load(t+1)),bottom=Hrtz(load(t+8)+load(t+9));uint x=tx+t%8,y=ty+t/8;
   if constexpr(DownCrop){int cx=int(x/2)-int(sx/2),cy=int(y/2)-int(sy/2);if(full||(cx>=0&&cy>=0&&cx<int(width/2)&&cy<int(height/2)))down[(uint(cy)*(width/2)+uint(cx))*32+c]=F(Hrtz(Hrtz(top+bottom)*.25f));}
   else down[((y/2)*(ww/2)+x/2)*32+c]=F(Hrtz(Hrtz(top+bottom)*.25f));
  }
  };
#if !CW_TAIL_QUAD
  // DownCrop uses sx/2,sy/2; the production shifts are even. Keep the generic edge path for odd shifts.
  if(tx>=sx&&ty>=sy&&tx+8<=width+sx&&ty+8<=height+sy&&(!DownCrop||((sx|sy)&1u)==0))emit(cw_true{});else emit(cw_false{});
#endif
 }
 }else{
 if constexpr(Finish){
  WG_FENCE(3);WG_FENCE(2);
  uint c=__builtin_amdgcn_workitem_id_x(),ww=width+2*sx,tx=(win%(ww/8))*8,ty=(win/(ww/8))*8;
  auto load=[&](uint t)->float{return float(residual[((t/16*2+c/16)*32+(c%16/8*16+t%16))*8+c%8]);};
#if CW_TAIL_QUAD
  auto put_main=[&](uint t,float raw_v){int x=int(tx+t%8)-int(sx),y=int(ty+t/8)-int(sy);if(x>=0&&y>=0&&x<int(width)&&y<int(height)){uint dest=(uint(y)*width+uint(x))*32+c;float v=F(raw_v);
   if constexpr(Prefix)*reinterpret_cast<unsigned char*>(reinterpret_cast<char*>(main)+dest)=static_cast<unsigned char>(fp8(v));else *reinterpret_cast<float*>(reinterpret_cast<char*>(main)+dest*4u)=v;}};
  for(uint k=0;k<16;k++){uint t=(k/4)*16+(k%4)*2;float a0=load(t),a1=load(t+1),b0=load(t+8),b1=load(t+9);
   if(main){put_main(t,a0);put_main(t+1,a1);put_main(t+8,b0);put_main(t+9,b1);}
   if(down){float top=Hrtz(a0+a1),bottom=Hrtz(b0+b1);uint x=tx+t%8,y=ty+t/8;
    if constexpr(DownCrop){int cx=int(x/2)-int(sx/2),cy=int(y/2)-int(sy/2);if(cx>=0&&cy>=0&&cx<int(width/2)&&cy<int(height/2))*reinterpret_cast<float*>(reinterpret_cast<char*>(down)+((uint(cy)*(width/2)+uint(cx))*32+c)*4u)=F(Hrtz(Hrtz(top+bottom)*.25f));}
    else *reinterpret_cast<float*>(reinterpret_cast<char*>(down)+(((y/2)*(ww/2)+x/2)*32+c)*4u)=F(Hrtz(Hrtz(top+bottom)*.25f));}
  }
  if(false)
#endif
  if(main)for(uint t=0;t<64;t++){int x=int(tx+t%8)-int(sx),y=int(ty+t/8)-int(sy);if((Prefix&&CW_PREFIX_FULL_TILE)||(x>=0&&y>=0&&x<int(width)&&y<int(height))){uint dest=(uint(y)*width+uint(x))*32+c;float v=load(t);if constexpr(Prefix)reinterpret_cast<unsigned char*>(main)[dest]=static_cast<unsigned char>(cw_prefix_byte(v));else main[dest]=F(v);}}
  if(!CW_TAIL_QUAD&&down)for(uint k=0;k<16;k++){uint t=(k/4)*16+(k%4)*2;float top=Hrtz(load(t)+load(t+1)),bottom=Hrtz(load(t+8)+load(t+9));uint x=tx+t%8,y=ty+t/8;
   if constexpr(DownCrop){int cx=int(x/2)-int(sx/2),cy=int(y/2)-int(sy/2);if(cx>=0&&cy>=0&&cx<int(width/2)&&cy<int(height/2))down[(uint(cy)*(width/2)+uint(cx))*32+c]=F(Hrtz(Hrtz(top+bottom)*.25f));}
   else down[((y/2)*(ww/2)+x/2)*32+c]=F(Hrtz(Hrtz(top+bottom)*.25f));
  }
 }
 }
}
#define CW_ENTRY KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
CW_ENTRY void c32_wave1_chain(const unsigned char*in,const float*fw,const float*w,unsigned char*out,uint windows,uint mode,uint raw,uint width,uint height,uint sx,uint sy,uint prevw,uint px,uint py){cw_body<true>(in,fw,w,out,windows,mode,raw,width,height,sx,sy,prevw,px,py);}
CW_ENTRY void c32_wave1_mapped(const unsigned char*in,const float*fw,const float*w,unsigned char*out,uint windows,uint mode,uint raw,uint width,uint height,uint sx,uint sy){cw_body<false>(in,fw,w,out,windows,mode,raw,width,height,sx,sy,0,0,0);}
#define CW_FINISH(NAME,DC) CW_ENTRY void NAME(const unsigned char*in,const float*fw,const float*w,float*main,float*down,uint windows,uint mode,uint raw,uint width,uint height,uint sx,uint sy,uint prevw,uint px,uint py){cw_body<true,true,DC>(in,fw,w,nullptr,windows,mode,raw,width,height,sx,sy,prevw,px,py,main,down);}
CW_FINISH(c32_wave1_finish,false)
CW_FINISH(c32_wave1_finish_dcrop,true)
CW_ENTRY void c32_wave1_post(const unsigned char*low,const unsigned char*skip,const float*scales,const float*fw,const float*w,const float*color,const float*headw,float*rgb,uint windows,uint width,uint height,uint sx,uint sy,float scale){cw_body<false,false,false,true>(low,fw,w,nullptr,windows,0,1,width,height,sx,sy,0,0,0,nullptr,nullptr,skip,scales,color,headw,rgb,scale);}
CW_ENTRY void c32_wave1_prefix(const unsigned char*rgba,const float*history,const float*fw,const float*w,float*main,float*down,uint windows,uint mode,uint raw,uint width,uint height,uint seed,uint temporal){cw_body<false,true,false,false,true>(rgba,fw,w,nullptr,windows,mode,raw,width,height,0,0,0,0,0,main,down,nullptr,nullptr,nullptr,nullptr,nullptr,0.f,history,seed,temporal);}
