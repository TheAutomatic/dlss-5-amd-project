#ifndef HIP_MH_SCALAR_DIAGONAL
#define HIP_MH_SCALAR_DIAGONAL 1
#endif
#ifndef HIP_FFN_INPUT_PACK4
#define HIP_FFN_INPUT_PACK4 1
#endif
#ifndef HIP_QKV_SPECIALIZE
#define HIP_QKV_SPECIALIZE 1
#endif
#ifndef HIP_QKV_ROW_SUM
#define HIP_QKV_ROW_SUM 1
#endif
#ifndef HIP_ISA_HALF
#define HIP_ISA_HALF 1
#endif
// Native gfx1201 FP8 WMMA multihead kernels. Original externalfloat32 ABI.
// Mirrors native_c64.hlsl non-wave math, native_c64_shift.hlsl, native_c64_ds.hlsl.
// Host validates geometry/channel contracts; all tensors/weights externally float32.
// Every exported kernel uses one-dimensional blocks of exactly512 threads.
#pragma clang fp contract(off)
#pragma clang fp reassociate(off)
typedef unsigned uint;
#define DEV __attribute__((device)) __attribute__((always_inline))
#define KERNEL extern "C" __attribute__((global))
#ifndef HIP_FFN_WAVE_NORM
#define HIP_FFN_WAVE_NORM 0 /* 2026-09-23 (NOT bit-exact, default off): parts 0/1 of the QKV are computed one head (32 columns) per
 wave so the normalisation's sum of squares is 16 in-lane terms + one ds_bpermute add, with rsqrt/scale/norm bytes in registers -- no
 storage.raw round trip, no two barriers, no 128-thread serial loop. Re-associates the 32-term sum (two 16-term partial sums) before
 rsqrt; everything else unchanged. -0.18/-0.14 ms on top of HIP_FFN_TRANSPOSED_TAIL. Enable only after image-quality judgment. */
#endif
#ifndef HIP_FFN_TRANSPOSED_TAIL
#define HIP_FFN_TRANSPOSED_TAIL 1 /* 2026-09-23: projection and QKV WMMAs take swapped operands so each lane holds one token row and
 8 consecutive columns (A/B fragment register layouts are identical). Residual input, out/qfeature bytes, storage.raw rows and norm
 bytes then go 8-consecutive per lane; the compiler merges the byte stores into b64 (c256 kernel: global stores 38->10, -212
 instructions). The serial 32-term sum of squares keeps its order: bit-exact. -0.28/-0.21 ms (1080/900), three replications. */
#endif
#ifndef HIP_LDS_FENCE
#define HIP_LDS_FENCE 1 /* 2026-09-22 fence scope: workgroup fences here only publish LDS; the default "workgroup" scope made gfx12 emit global_inv (vector L0 invalidate) and wait for all outstanding global loads at every barrier. Restricting the fence to the LDS address space removes both; bit-exact, see Development/results/fence-scope-all-20260922 */
#endif
#if HIP_LDS_FENCE
#define WG_FENCE(k) __builtin_amdgcn_fence(k,"workgroup","local")
#else
#define WG_FENCE(k) __builtin_amdgcn_fence(k,"workgroup")
#endif
DEV uint bits(float x){union{float f;uint u;}v;v.f=x;return v.u;}
DEV float value(uint x){union{float f;uint u;}v;v.u=x;return v.f;}
DEV uint lane(){return __builtin_amdgcn_workgroup_id_x()*256u+__builtin_amdgcn_workitem_id_x();}
DEV float absf(float x){return value(bits(x)&0x7fffffffu);}
DEV float maxf(float a,float b){return a>b?a:b;}
DEV float minf(float a,float b){return a<b?a:b;}
DEV float clampf(float a,float lo,float hi){return minf(maxf(a,lo),hi);}
DEV uint shift_rne(uint x,uint n){if(n>31)return 0;uint q=x>>n,mask=(1u<<n)-1u,r=x&mask,mid=1u<<(n-1);return q+uint(r>mid||(r==mid&&(q&1u)));}
DEV uint half_bits(float x){uint b=bits(x),a=b&0x7fffffffu,s=(b>>16)&0x8000u;int e=int(a>>23)-127;
 if(a>=0x7f800000u)return s|0x7c00u|((a&0x7fffffu)?0x200u:0u);
 if(e<-25)return s;
 if(e<-14)return s|shift_rne((a&0x7fffffu)|0x800000u,uint(-e-1));
 if(e>15)return s|0x7c00u;
 uint h=((a+0xfffu+((a>>13)&1u))>>13)-0x1c000u;return s|(h>=0x7c00u?0x7c00u:h);
}
DEV float from_half(uint h){uint s=(h&0x8000u)<<16,e=(h>>10)&31u,m=h&1023u;if(e==31)return value(s|0x7f800000u|(m<<13));if(!e){if(!m)return value(s);float f=float(m)*5.9604644775390625e-8f;return s?-f:f;}return value(s|((e+112)<<23)|(m<<13));}
#ifdef HIP_ISA_HALF
// Keep f32 expression evaluation and both rounding boundaries opaque to fusion.
DEV float H(float x){uint h;float result;asm("v_cvt_f16_f32 %0, %1" : "=v"(h) : "v"(x));asm("v_cvt_f32_f16 %0, %1" : "=v"(result) : "v"(h));return result;}
#else
DEV float H(float x){if((bits(x)&0x7fffffffu)>=0x7f800000u)return x;return from_half(half_bits(x));}
#endif
#ifndef HIP_NATIVE_FP8_F
#define HIP_NATIVE_FP8_F 1
#endif
#ifndef HIP_BRANCHLESS_F
#define HIP_BRANCHLESS_F 1 /* 2026-09-17: with deep_fast and the fused attention file -0.12ms, bit-exact. F() as clamp+cvt+cndmask instead of a divergent branch pair per call (same values for every non-NaN input; NaN gives -448 instead of +-448) */
#endif
#if HIP_NATIVE_FP8_F && HIP_BRANCHLESS_F
DEV float F(float x){uint a=bits(x)&0x7fffffffu;float c=__builtin_fminf(__builtin_fmaxf(x,-448.f),448.f);float r=__builtin_amdgcn_cvt_f32_fp8(__builtin_amdgcn_cvt_pk_fp8_f32(c,0.f,0,false),0);return a==0?0.f:r;}
#elif HIP_NATIVE_FP8_F
DEV float F(float x){
 uint b=bits(x),a=b&0x7fffffffu;
 if(a==0)return 0.f;
 // Preserve the existing signed saturation, including nonfinite inputs.
 if(a>=0x43e00000u)return value((b&0x80000000u)|0x43e00000u);
 int packed=__builtin_amdgcn_cvt_pk_fp8_f32(x,0.f,0,false);
 return __builtin_amdgcn_cvt_f32_fp8(packed,0);
}
#else
DEV float F(float x){uint b=bits(x),a=b&0x7fffffffu,s=b&0x80000000u;if(a==0)return 0.f;
 if(a>=0x43e00000u)return value(s|0x43e00000u); // finite E4M3 saturation, including overflow
 if(a<0x3c800000u){float scaled=absf(x)*512.f;uint q=uint(scaled);float r=scaled-float(q);q+=uint(r>.5f||(r==.5f&&(q&1u)));float f=float(q)/512.f;return s?-f:f;}
 uint r=(a+0x7ffffu+((a>>20)&1u))&0xfff00000u;return value(s|(r>0x43e00000u?0x43e00000u:r));}
#endif
DEV float half_square_pair(float lower,float upper){float a=lower*lower,b=H(upper*upper),sum=a+b;
 if((bits(sum)&0x7fffffffu)>=0x7f800000u)return H(sum);
 float large=maxf(a,b),small=minf(a,b),recovered=sum-large,error=small-recovered;
 if(sum==65520.f)return error<0?65504.f:H(sum);
 float rounded=H(sum);if(error==0||(bits(rounded)&0x7fffffffu)>=0x7f800000u||rounded==sum)return rounded;
 uint hb=half_bits(rounded),lo_bits=sum<rounded?hb-1:hb;float lo=from_half(lo_bits),hi=from_half(lo_bits+1),mid=(lo+hi)*.5f;
 return sum==mid?(error>0?hi:lo):rounded;
}
DEV float exp_approx(float x){float a=clampf(H(x*.044921875f+1.30078125f),1.03125f,1.5693359375f);return from_half(((half_bits(a)<<5)+0x8000u)&65535u);}
DEV bool valid_c(uint c){return c==64u||c==128u||c==256u;}
DEV bool valid_attn(uint c){return valid_c(c)||c==512u;}
DEV bool valid_spatial(uint c){return c==32u||valid_attn(c);}
DEV uint pixel_in_window(uint window,uint token,uint width){return ((window/(width/8))*8+token/8)*width+(window%(width/8))*8+token%8;}
DEV uint window_of_pixel(uint p,uint width){return (p/width/8)*(width/8)+(p%width)/8;}
DEV uint token_of_pixel(uint p,uint width){return ((p/width)%8)*8+(p%width)%8;}

#if !__has_builtin(__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12)
#error "gfx12 FP8 WMMA compiler required; no scalar fallback"
#endif
using i2=int __attribute__((ext_vector_type(2)));
using f8=float __attribute__((ext_vector_type(8)));
#define TILE KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
DEV uint q8(float x){return uint(__builtin_amdgcn_cvt_pk_fp8_f32(x,x,0,false))&255u;}
DEV void pack(i2&v,uint e,float x){v[e/4]=int(uint(v[e/4])|(q8(x)<<(8*(e%4))));}
// Diagnostic production HLSL f32tof16 path: explicit RTZ, distinct from
// Cast<F16> / ISA_HALF RNE. Keep both exported contracts until measured.
DEV float Hrtz(float x){uint h;float out;asm("v_cvt_pkrtz_f16_f32 %0, %1, %1" : "=v"(h) : "v"(x));asm("v_cvt_f32_f16 %0, %1" : "=v"(out) : "v"(h));return out;}
//64x64 output tile,16 waves. Each thread stages one packed4-byte A row word
// and one B column word; each packed operand is reused by four waves.
DEV uint pack4(float a,float b,float c,float d){uint lo=uint(__builtin_amdgcn_cvt_pk_fp8_f32(a,b,0,false));uint hi=uint(__builtin_amdgcn_cvt_pk_fp8_f32(c,d,0,false));return (lo&65535u)|((hi&65535u)<<16);}
template<bool ByteInput=false,bool ByteOutput=false,bool Normalize=false,bool Crop=false>
DEV void fast_dense(const float* input,const float* feature,const float* weights,float* output,
 uint M,uint N,uint K,uint matrix_offset,uint skip_offset,uint epilogue,uint width=0,uint valid_width=0,uint valid_height=0,uint matrix_residual=0,uint cropw=0,uint croph=0,uint workw=0,uint sx=0,uint sy=0){
 __attribute__((shared)) uint sa[576];
 __attribute__((shared)) uint sb[576];
 uint block=__builtin_amdgcn_workgroup_id_x(),nt=(N+63)/64;
 if(!nt||block>=((M+63)/64)*nt)return;
 uint mr=block/nt*64,nc=block%nt*64,t=__builtin_amdgcn_workitem_id_x(),wave=t/32,l=t%32,rc=l&15,group=l>>4;
 uint wm=wave/4,wn=wave%4,first=mr+wm*16,col=nc+wn*16;
 f8 accum{};
 #pragma unroll
 for(uint e=0;e<8;e++){uint p=first+group*8+e,n=col+rc;if(p<M&&n<N&&skip_offset!=~0u&&!matrix_residual)accum[e]=Hrtz(feature[p*N+n]*weights[skip_offset+n]);}
 if(matrix_residual){
  uint n=col+rc;float remain=n<N?weights[skip_offset+n]:0.f;
  for(uint part=0;part<3;part++){
   float component;
   if(absf(remain)<.015625f){float x=absf(remain)*512.f;uint q=uint(x);float r=x-float(q);q+=uint(r>.5f||(r==.5f&&(q&1)));q=q>7?7:q;component=(remain<0?-1.f:1.f)*float(q)/512.f;}
   else component=F(remain);
   remain-=component;
#if HIP_MH_SCALAR_DIAGONAL
   for(uint e=0;e<8;e++){uint p=first+group*8+e;if(p<M&&n<N){float x=__builtin_amdgcn_cvt_f32_fp8(int(q8(feature[p*N+n])),0);for(uint half=0;half<2;half++)accum[e]=__builtin_fmaf(x,half==(n%32)/16?component:0.f,accum[e]);}}
#else
   #pragma unroll
   for(uint half=0;half<2;half++){i2 a{},b{};
    #pragma unroll
    for(uint e=0;e<8;e++){uint k=half*16+group*8+e,p=first+rc,g=col/32*32;float x=(p<M&&g+k<N)?feature[p*N+g+k]:0.f;pack(a,e,x);pack(b,e,(n<N&&k==n%32)?component:0.f);}
    accum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,accum);
   }
#endif
  }
 }
 for(uint chunk=0;chunk<K/32;chunk++){
  uint row=t/8,word=t%8,k=chunk*32+word*4,p=mr+row,n=nc+row;
  float a0=0,a1=0,a2=0,a3=0,b0=0,b1=0,b2=0,b3=0;
  if constexpr(!ByteInput){if(p<M&&(!valid_width||(p%width<valid_width&&p/width<valid_height))){uint off=p*K+k;a0=input[off];a1=input[off+1];a2=input[off+2];a3=input[off+3];}}
  #ifndef HIP_PREPACKED_WEIGHTS
  if(n<N){uint off=matrix_offset+n*K+k;b0=weights[off];b1=weights[off+1];b2=weights[off+2];b3=weights[off+3];}
#endif
  if constexpr(ByteInput){sa[row*9+word]=(p<M&&(!valid_width||(p%width<valid_width&&p/width<valid_height)))?reinterpret_cast<const uint*>(input)[(p*K+k)/4]:0u;}else sa[row*9+word]=pack4(a0,a1,a2,a3);
#ifdef HIP_PREPACKED_WEIGHTS
  // Matrix bytes start at the original float-region offset; nonmatrix fields stay f32.
  const uint* packed_weights=reinterpret_cast<const uint*>(weights);
  sb[row*9+word]=n<N?packed_weights[matrix_offset+(n*K+k)/4]:0u;
#else
  sb[row*9+word]=pack4(b0,b1,b2,b3);
#endif
  __builtin_amdgcn_s_barrier();
  #pragma unroll
  for(uint half=0;half<2;half++){
   uint word0=half*4+group*2;
   i2 a={int(sa[(wm*16+rc)*9+word0]),int(sa[(wm*16+rc)*9+word0+1])};
   i2 b={int(sb[(wn*16+rc)*9+word0]),int(sb[(wn*16+rc)*9+word0+1])};
   accum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,accum);
  }
  // All sixteen waves finish reading before any writer reuses this K32 tile.
  __builtin_amdgcn_s_barrier();
  // Production fast path: keep accumulator live across every K16/K32.
 }
#if HIP_QKV_ROW_SUM
 if constexpr(Normalize){
  __attribute__((shared)) float tile_qkv[64*65],norm_inv[128];
  for(uint e=0;e<8;e++)tile_qkv[(wm*16+group*8+e)*65+wn*16+rc]=accum[e];
  __builtin_amdgcn_s_barrier();
  uint channels=N/3,heads=channels/32,scale=4*channels*channels+heads*4096;
  if(t<128){uint row=t%64,h=t/64,n=nc+h*32,part=n/channels,head=(n%channels)/32;float inv=1.f;
   if(mr+row<M&&n<N&&part<2){float ss=0.f;
    for(uint j=0;j<32;j++){float v=tile_qkv[row*65+h*32+j];ss+=v*v;}
    inv=__builtin_amdgcn_rsqf(maxf(ss,6.198883056640625e-5f))*(part==0?weights[scale+head]:1.f);
   }norm_inv[h*64+row]=inv;
  }
  __builtin_amdgcn_s_barrier();
  for(uint e=0;e<8;e++){uint row=wm*16+group*8+e,p=mr+row,n=col+rc;if(p<M&&n<N)reinterpret_cast<unsigned char*>(output)[p*N+n]=static_cast<unsigned char>(q8(F(accum[e]*norm_inv[(wn/2)*64+row])));}
  return;
 }
#else
 if constexpr(Normalize){
  __attribute__((shared)) float tile_qkv[64*64];
  for(uint e=0;e<8;e++)tile_qkv[(wm*16+group*8+e)*64+wn*16+rc]=accum[e];
  __builtin_amdgcn_s_barrier();
  uint channels=N/3,heads=channels/32,scale=4*channels*channels+heads*4096;
  for(uint row0=wave/2;row0<64;row0+=8){
   uint p=mr+row0,n=nc+(wave%2)*32+l;
   if(p>=M||n>=N)continue;
   uint part=n/channels,head=(n%channels)/32;
   float v=tile_qkv[row0*64+(wave%2)*32+l],inv=1.f;
   if(part<2){float sq=v*v,ss=0.f;
    #pragma unroll
    for(uint j=0;j<32;j++)ss+=value(__builtin_amdgcn_readlane(bits(sq),j));
    inv=__builtin_amdgcn_rsqf(maxf(ss,6.198883056640625e-5f))*(part==0?weights[scale+head]:1.f);
   }
   reinterpret_cast<unsigned char*>(output)[p*N+n]=static_cast<unsigned char>(q8(F(v*inv)));
  }
  return;
 }
#endif
 #pragma unroll
 for(uint e=0;e<8;e++){uint p=first+group*8+e,n=col+rc;if(p>=M||n>=N)continue;float a=accum[e],v;
  if(epilogue==1){float gate=clampf(a,-4.f,4.f),q=absf(gate)*(-.055908203125f)+.447265625f,poly=gate*q+.89453125f;v=F(a*poly);}
  else v=epilogue==2?a:epilogue==3?Hrtz(a):epilogue==4?F(a):epilogue==5?F(Hrtz(a)):F(H(a));
  v=(valid_width&&(p%width>=valid_width||p/width>=valid_height))?0.f:v;
  uint dest=p*N+n;
  if constexpr(Crop){uint x=p%workw,y=p/workw;if(x<sx||y<sy||x>=sx+cropw||y>=sy+croph)continue;dest=((y-sy)*cropw+x-sx)*N+n;}
  if constexpr(ByteOutput)reinterpret_cast<unsigned char*>(output)[dest]=static_cast<unsigned char>(q8(v));else output[dest]=v;
 }
}
// Final build contract: fused FP8act overwritten without HW_QUANTIZE:
// expand=F(polynomial(FP32 acc)), contract=F(H(FP32 acc)).
TILE void mh_ffn_expand_fast(const float* input,const float* weights,float* hidden,uint tokens,uint channels){if(valid_c(channels))fast_dense(input,input,weights,hidden,tokens,4*channels,channels,0,~0u,1);}
TILE void mh_ffn_contract_fast(const float* hidden,const float* weights,float* middle,uint tokens,uint channels){if(valid_c(channels))fast_dense(hidden,hidden,weights,middle,tokens,channels,4*channels,4*channels*channels,~0u,5);}
// Production MAP_FEATURE+DIRECT_CAST variant. Not MATRIX_RESIDUAL.
TILE void mh_ffn_project_fast(const float* middle,const float* feature,const float* weights,float* output,uint tokens,uint channels){if(valid_c(channels))fast_dense(middle,feature,weights,output,tokens,channels,channels,8*channels*channels,9*channels*channels,4);}
// Raw FP32 QKV; do NOT feed to legacy H-reduction normalization.
TILE void mh_qkv_fast(const float* input,const float* weights,float* qkv,uint tokens,uint channels){if(valid_attn(channels))fast_dense(input,input,weights,qkv,tokens,3*channels,channels,0,~0u,2);}
// Explicit scalar-residual diagnostic variant. Does NOT implement production
// NATIVE_MATRIX_RESIDUAL (three FP8 diagonal MMAs). post:0 F(H),3 H,4 directF.
TILE void mh_attention_project_fast_scalar(const float* av,const float* feature,const float* weights,float* output,uint tokens,uint channels,uint post){if(valid_attn(channels)&&(post==0||post==3||post==4))fast_dense(av,feature,weights,output,tokens,channels,channels,3*channels*channels,4*channels*channels+(channels/32)*4096+channels/32,post==0?5:post);}

// Same input/weight/output ABI as contract_fast, only the last epilogue differs.
TILE void mh_ffn_contract_fast_pre(const float* hidden,const float* weights,float* middle,uint tokens,uint channels){if(valid_c(channels))fast_dense(hidden,hidden,weights,middle,tokens,channels,4*channels,4*channels*channels,~0u,2);}
TILE void mh_ffn_contract_fast_rtz(const float* hidden,const float* weights,float* middle,uint tokens,uint channels){if(valid_c(channels))fast_dense(hidden,hidden,weights,middle,tokens,channels,4*channels,4*channels*channels,~0u,5);}

TILE void mh_ffn_contract_fast_rne(const float* hidden,const float* weights,float* middle,uint tokens,uint channels){if(valid_c(channels))fast_dense(hidden,hidden,weights,middle,tokens,channels,4*channels,4*channels*channels,~0u,0);}
// Production NATIVE_QKV_FAST2=0: rawFP32 squares, sequentialFP32 sum,
// unrounded rsqrt*scale and direct saturated FP8 cast. Not legacy normalize.
// Input/output HWC with three parts[p][Q,K,V][C]. Block256, one thread per head/token.
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void mh_qkv_normalize_fast(const float* qkv,const float* weights,float* normalized,uint tokens,uint channels){
 uint i=lane(),heads=channels/32;if(!valid_attn(channels)||i>=tokens*heads)return;uint p=i/heads,head=i%heads,scale=4*channels*channels+heads*4096;
 for(uint part=0;part<3;part++){uint base=(p*3+part)*channels+head*32;float inv=1.f;
  if(part<2){float ss=0.f;for(uint c=0;c<32;c++){float v=qkv[base+c];float square=v*v;ss+=square;}inv=__builtin_amdgcn_rsqf(maxf(ss,6.198883056640625e-5f))*(part==0?weights[scale+head]:1.f);}
  for(uint c=0;c<32;c++)normalized[base+c]=F(qkv[base+c]*inv);
 }
}

// Matrix-residual projection: three original FP8 diagonal terms, no seed H.
// post0 F(H_RTZ),3 H_RTZ,4 directFP8; feature must be exact finiteFP8.
TILE void mh_attention_project_fast_matrix(const float* av,const float* feature,const float* weights,float* output,uint tokens,uint channels,uint post){if(valid_attn(channels)&&(post==0||post==3||post==4))fast_dense(av,feature,weights,output,tokens,channels,channels,3*channels*channels,4*channels*channels+(channels/32)*4096+channels/32,post==0?5:post,0,0,0,1);}
#define FASTWAVE KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
using h8=_Float16 __attribute__((ext_vector_type(8)));
// normalized is raster[p][Q,K,V][C], ex/prob are[window][head][q64][k64].
FASTWAVE void mh_scores_exp_fast(const float* normalized,const float* weights,float* ex,uint width,uint height,uint channels){
 uint tile=__builtin_amdgcn_workgroup_id_x(),heads=channels/32,windows=width/8*(height/8);if(!valid_attn(channels)||width%8||height%8||tile>=windows*heads*16)return;
 uint wh=tile/16,win=wh/heads,head=wh%heads,q=tile%16/4*16,col=tile%4*16,l=__builtin_amdgcn_workitem_id_x(),rc=l&15,g=l>>4;f8 sum{};
 for(uint half=0;half<2;half++){i2 a{},b{};for(uint e=0;e<8;e++){uint c=half*16+g*8+e,qp=pixel_in_window(win,q+rc,width),kp=pixel_in_window(win,col+rc,width);pack(a,e,normalized[(qp*3)*channels+head*32+c]);pack(b,e,normalized[(kp*3+1)*channels+head*32+c]);}sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,sum);}
 for(uint e=0;e<8;e++){uint qr=q+g*8+e,k=col+rc;float score=sum[e]+weights[4*channels*channels+head*4096+qr*64+k];float a=clampf(score*.044921875f+1.30078125f,1.03125f,1.5693359375f);uint h;asm("v_cvt_pkrtz_f16_f32 %0, %1, %1" : "=v"(h) : "v"(a));ex[(wh*64+qr)*64+k]=from_half((((h&65535u)<<5)+0x8000u)&65535u);}
}
// Each wave handles16 queries, computes the two production32-key partials
// with F16 WMMA against ones, then forms probabilities with unrounded inverse.
FASTWAVE void mh_probabilities_fast(const float* ex,float* prob,uint windows,uint channels){
 uint tile=__builtin_amdgcn_workgroup_id_x(),heads=channels/32;if(!valid_attn(channels)||tile>=windows*heads*4)return;uint wh=tile/4,q=tile%4*16,l=__builtin_amdgcn_workitem_id_x(),rc=l&15,g=l>>4;f8 totals[2];
 for(uint side=0;side<2;side++){f8 sum{};for(uint half=0;half<2;half++){h8 a{},b{};for(uint e=0;e<8;e++){uint k=side*16+half*32+g*8+e;a[e]=(_Float16)ex[(wh*64+q+rc)*64+k];b[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,sum);}totals[side]=sum;}
 for(uint e=0;e<8;e++){uint qr=q+g*8+e;float inv=1.f/(totals[0][e]+totals[1][e]);for(uint k=rc;k<64;k+=16)prob[(wh*64+qr)*64+k]=F(ex[(wh*64+qr)*64+k]*inv);}
}
FASTWAVE void mh_attention_av_fast(const float* prob,const float* normalized,float* av,uint width,uint height,uint channels){
 uint tile=__builtin_amdgcn_workgroup_id_x(),heads=channels/32,windows=width/8*(height/8);if(!valid_attn(channels)||width%8||height%8||tile>=windows*heads*8)return;
 uint wh=tile/8,win=wh/heads,head=wh%heads,q=tile%8/2*16,col=tile%2*16,l=__builtin_amdgcn_workitem_id_x(),rc=l&15,g=l>>4;f8 sum{};
 for(uint half=0;half<4;half++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=half*16+g*8+e,kp=pixel_in_window(win,k,width);pack(a,e,prob[(wh*64+q+rc)*64+k]);pack(b,e,normalized[(kp*3+2)*channels+head*32+col+rc]);}sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,sum);}
 for(uint e=0;e<8;e++){uint p=pixel_in_window(win,q+g*8+e,width);av[p*channels+head*32+col+rc]=F(sum[e]);}
}
// Production pool projection is NOT the dense fast accumulation schedule.
// C>=64: F16 operands, independent K32 sum then software-H/RNE each group.
// C32: FP8 operands, a single K32 dot, explicit HLSL-half/RTZ epilogue.
FASTWAVE void mh_pool_project_production(const float* pooled,const float* weights,float* output,uint width,uint height,uint valid_width,uint valid_height,uint channels){
 uint M=width*height,N=2*channels,tile=__builtin_amdgcn_workgroup_id_x(),nt=N/16;
 if(!valid_spatial(channels)||tile>=((M+15)/16)*nt)return;uint first=tile/nt*16,col=tile%nt*16,l=__builtin_amdgcn_workitem_id_x(),rc=l&15,g=l>>4;f8 acc{};
 for(uint chunk=0;chunk<channels/32;chunk++){f8 sum{};
  for(uint half=0;half<2;half++){
   if(channels==32){i2 a{},b{};for(uint e=0;e<8;e++){uint k=half*16+g*8+e,p=first+rc;pack(a,e,p<M?pooled[p*channels+k]:0.f);pack(b,e,weights[(col+rc)*channels+k]);}sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,sum);}
   else{h8 a{},b{};for(uint e=0;e<8;e++){uint k=chunk*32+half*16+g*8+e,p=first+rc;a[e]=(_Float16)(p<M?pooled[p*channels+k]:0.f);b[e]=(_Float16)weights[(col+rc)*channels+k];}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,sum);}
  }
  for(uint e=0;e<8;e++)acc[e]=channels==32?Hrtz(acc[e]+sum[e]):H(acc[e]+sum[e]);
 }
 for(uint e=0;e<8;e++){uint p=first+g*8+e;if(p<M)output[p*N+col+rc]=(valid_width&&(p%width>=valid_width||p/width>=valid_height))?0.f:F(acc[e]);}
}
// Prepacked-weight twin (host option pool_project_h16w): weights arrive as RNE half rows (c>=64) or E4M3 bytes (c==32)
// exactly as the (_Float16)/pack() casts of the f32 kernel would produce them; one fragment load replaces eight casts.
FASTWAVE void mh_pool_project_production_h16w(const float* pooled,const float* weights,float* output,uint width,uint height,uint valid_width,uint valid_height,uint channels){
 uint M=width*height,N=2*channels,tile=__builtin_amdgcn_workgroup_id_x(),nt=N/16;
 if(!valid_spatial(channels)||tile>=((M+15)/16)*nt)return;uint first=tile/nt*16,col=tile%nt*16,l=__builtin_amdgcn_workitem_id_x(),rc=l&15,g=l>>4;f8 acc{};
 for(uint chunk=0;chunk<channels/32;chunk++){f8 sum{};
  for(uint half=0;half<2;half++){
   if(channels==32){i2 a{},b;for(uint e=0;e<8;e++){uint k=half*16+g*8+e,p=first+rc;pack(a,e,p<M?pooled[p*channels+k]:0.f);}__builtin_memcpy(&b,reinterpret_cast<const unsigned char*>(weights)+(col+rc)*32+half*16+g*8,8);sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,sum);}
   else{h8 a{},b;for(uint e=0;e<8;e++){uint k=chunk*32+half*16+g*8+e,p=first+rc;a[e]=(_Float16)(p<M?pooled[p*channels+k]:0.f);}__builtin_memcpy(&b,reinterpret_cast<const _Float16*>(weights)+(col+rc)*channels+chunk*32+half*16+g*8,16);sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,sum);}
  }
  for(uint e=0;e<8;e++)acc[e]=channels==32?Hrtz(acc[e]+sum[e]):H(acc[e]+sum[e]);
 }
 for(uint e=0;e<8;e++){uint p=first+g*8+e;if(p<M)output[p*N+col+rc]=(valid_width&&(p%width>=valid_width||p/width>=valid_height))?0.f:F(acc[e]);}
}

// Fused 2x2 pool + channel-doubling projection (host option pool_project_fused, C=64/128/256): one wave produces 16
// pooled tokens x 64 output columns; the pooled A fragments are computed once from raw (same H/F chain as mh_pool) and
// reused across the four column tiles, weights arrive prepacked as F16 rows (one 16-byte load per B fragment). The
// per-32-chunk f32 sum, H(acc+sum) chaining and F(acc) store follow mh_pool_project_production exactly.
template<uint C>
DEV void mh_pool_project_fused_body(const float*raw,const float*w,float*output,uint width,uint height,uint source_width,uint valid_width,uint valid_height){
 const uint M=width*height,N=2*C,ncol=N/64;uint tile=__builtin_amdgcn_workgroup_id_x();if(tile>=((M+15)/16)*ncol)return;
 uint first=tile/ncol*16,colg=tile%ncol*64,l=__builtin_amdgcn_workitem_id_x(),rc=l&15,g=l>>4;
 const _Float16*w16=reinterpret_cast<const _Float16*>(w);
 h8 a[C/16];uint p=first+rc;bool live=p<M&&!(valid_width&&(p%width>=valid_width||p/width>=valid_height));uint base=live?((p/width)*2*source_width+(p%width)*2)*C:0;
 for(uint sidx=0;sidx<C/16;sidx++)for(uint e=0;e<8;e++){uint c=sidx*16+g*8+e;float v=0.f;if(live){float top=H(raw[base+c]+raw[base+C+c]),bottom=H(raw[base+source_width*C+c]+raw[base+(source_width+1)*C+c]);v=F(H(H(top+bottom)*.25f));}a[sidx][e]=(_Float16)v;}
 f8 acc[4]{};
 for(uint chunk=0;chunk<C/32;chunk++){f8 sum[4]{};
  for(uint half=0;half<2;half++){uint sidx=chunk*2+half;for(uint j=0;j<4;j++){h8 b;__builtin_memcpy(&b,w16+(colg+j*16+rc)*C+sidx*16+g*8,16);sum[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a[sidx],b,sum[j]);}}
  for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)acc[j][e]=H(acc[j][e]+sum[j][e]);
 }
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++){uint q=first+g*8+e;if(q<M)output[q*N+colg+j*16+rc]=(valid_width&&(q%width>=valid_width||q/width>=valid_height))?0.f:F(acc[j][e]);}
}
// Group-shared pool + projection (host option pool_project_group, C=64/128/256): one workgroup of 2C/64 waves owns 16
// pooled tokens; the pooling (same H/F chain as mh_pool) is done once per group into LDS f16 rows (exact: values are
// F()-quantized), then each wave produces its 64 output columns with A fragments from LDS and B fragments from the
// prepacked 16x16 f16 tile layout (PackedDsWeightFrag: one contiguous 512-byte load per fragment). Same per-32-chunk
// sum, H(acc+sum) chaining and F(acc) store as mh_pool_project_production.
template<uint C>
DEV void mh_pool_project_group_body(const float*raw,const float*wf,float*output,uint width,uint height,uint source_width,uint valid_width,uint valid_height){
 const uint M=width*height,N=2*C,S=C+8;uint tile=__builtin_amdgcn_workgroup_id_x();if(tile>=(M+15)/16)return;
 uint first=tile*16,t=__builtin_amdgcn_workitem_id_x(),wave=t/32,l=t%32,rc=l&15,g=l>>4,colg=wave*64;
 __attribute__((shared)) _Float16 pooled[16*S];
 for(uint k=0;k<16;k++){uint p=first+k,c=t;float v=0.f;if(p<M&&!(valid_width&&(p%width>=valid_width||p/width>=valid_height))){uint base=((p/width)*2*source_width+(p%width)*2)*C;float top=H(raw[base+c]+raw[base+C+c]),bottom=H(raw[base+source_width*C+c]+raw[base+(source_width+1)*C+c]);v=F(H(H(top+bottom)*.25f));}pooled[k*S+c]=(_Float16)v;}
 WG_FENCE(3);__builtin_amdgcn_s_barrier();WG_FENCE(2);
 const _Float16*w16=reinterpret_cast<const _Float16*>(wf);
 f8 acc[4]{};
 for(uint chunk=0;chunk<C/32;chunk++){f8 sum[4]{};
  for(uint half=0;half<2;half++){uint sidx=chunk*2+half;h8 a;__builtin_memcpy(&a,pooled+rc*S+sidx*16+g*8,16);
   for(uint j=0;j<4;j++){h8 b;uint nt=colg/16+j;__builtin_memcpy(&b,w16+(nt*(C/16)+sidx)*256+(g*16+rc)*8,16);sum[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,sum[j]);}}
  for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)acc[j][e]=H(acc[j][e]+sum[j][e]);
 }
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++){uint q=first+g*8+e;if(q<M)output[q*N+colg+j*16+rc]=(valid_width&&(q%width>=valid_width||q/width>=valid_height))?0.f:F(acc[j][e]);}
}
KERNEL __attribute__((amdgpu_flat_work_group_size(64,64))) void mh_pool_project_group_c64(const float*raw,const float*w,float*output,uint width,uint height,uint source_width,uint valid_width,uint valid_height){mh_pool_project_group_body<64>(raw,w,output,width,height,source_width,valid_width,valid_height);}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_pool_project_group_c128(const float*raw,const float*w,float*output,uint width,uint height,uint source_width,uint valid_width,uint valid_height){mh_pool_project_group_body<128>(raw,w,output,width,height,source_width,valid_width,valid_height);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_pool_project_group_c256(const float*raw,const float*w,float*output,uint width,uint height,uint source_width,uint valid_width,uint valid_height){mh_pool_project_group_body<256>(raw,w,output,width,height,source_width,valid_width,valid_height);}
FASTWAVE void mh_pool_project_fused_c64(const float*raw,const float*w,float*output,uint width,uint height,uint source_width,uint valid_width,uint valid_height){mh_pool_project_fused_body<64>(raw,w,output,width,height,source_width,valid_width,valid_height);}
FASTWAVE void mh_pool_project_fused_c128(const float*raw,const float*w,float*output,uint width,uint height,uint source_width,uint valid_width,uint valid_height){mh_pool_project_fused_body<128>(raw,w,output,width,height,source_width,valid_width,valid_height);}
FASTWAVE void mh_pool_project_fused_c256(const float*raw,const float*w,float*output,uint width,uint height,uint source_width,uint valid_width,uint valid_height){mh_pool_project_fused_body<256>(raw,w,output,width,height,source_width,valid_width,valid_height);}

// C512 QKV+normalize without the 64x64 LDS-staged tile (host option qkv_norm_wave_c512): a 128-thread group packs the
// 16-token A rows (pack4, as fast_dense) into LDS once, then each of its four waves owns 64 output columns (two heads)
// and runs the K=512 FP8 MMA chain straight from LDS/A and the prepacked [N][K] weight bytes/B. Row sums stay the
// sequential j=0..31 order over a per-wave LDS tile; the store is q8(F(acc*inv)) exactly as fast_dense<Normalize>.
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128)))
void mh_qkv_normalize_wave_c512(const float*in,const float*w,float*out,uint tokens){
 const uint C=512,N=1536,K=512;uint t=__builtin_amdgcn_workitem_id_x(),wave=t/32,l=t%32,rc=l&15,g=l>>4;
 uint block=__builtin_amdgcn_workgroup_id_x(),first=block/6*16,colbase=block%6*256;if(first>=tokens)return;
 union Shared{uint a[16*130];float raw[4*17*65];};__attribute__((shared)) Shared sh;
 for(uint i=t;i<16*128;i+=128){uint row=i/128,word=i%128,p=first+row,k=word*4;uint v=0;if(p<tokens){uint off=p*K+k;v=pack4(in[off],in[off+1],in[off+2],in[off+3]);}sh.a[row*130+word]=v;}
 __builtin_amdgcn_s_barrier();
 const uint*pw=reinterpret_cast<const uint*>(w);uint col=colbase+wave*64;f8 acc[4]{};
 for(uint k=0;k<K;k+=16){uint word0=(k+g*8)/4;i2 a={int(sh.a[rc*130+word0]),int(sh.a[rc*130+word0+1])};
  for(uint j=0;j<4;j++){uint n=col+j*16+rc,off=(n*K+k+g*8)/4;i2 b={int(pw[off]),int(pw[off+1])};acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc[j]);}
 }
 __builtin_amdgcn_s_barrier();
 float*raw=sh.raw+wave*17*65;
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)raw[(g*8+e)*65+j*16+rc]=acc[j][e];
 __builtin_amdgcn_s_barrier();
 uint part=col/C,head0=(col%C)/32,scale=4*C*C+(C/32)*4096;
 float inv=1.f;{uint row=l%16,h=l/16;if(first+row<tokens&&part<2){float ss=0.f;for(uint j=0;j<32;j++){float v=raw[row*65+h*32+j];ss+=v*v;}inv=__builtin_amdgcn_rsqf(maxf(ss,6.198883056640625e-5f))*(part==0?w[scale+head0+h]:1.f);}}
 // inv lives in lane (h*16+row); the store needs inv[row=g*8+e][h=j/2]: read it back through the same LDS tile row 16..
 __builtin_amdgcn_s_barrier();
 raw[16*65+l]=inv;
 __builtin_amdgcn_s_barrier();
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++){uint row=g*8+e,p=first+row,n=col+j*16+rc;if(p<tokens)reinterpret_cast<unsigned char*>(out)[p*N+n]=static_cast<unsigned char>(q8(F(acc[j][e]*raw[16*65+(j/2)*16+row])));}
}

// C512 QKV+normalize in the HLSL native_wave_qkv_normalize shape (host option c512_qkv_frag): one wave = 16 tokens x
// 64 output columns (two heads of one part); A comes straight from the projection's 512-byte [16 token][32 k] E4M3
// tiles, B from fragment-tiled E4M3 QKV weights (FragmentPackedMatrix: 512-byte tile per (column tile, k32), 8
// contiguous bytes per lane) -- no LDS and no barrier in the K loop. Row sums keep the sequential j=0..31 order in a
// wave-private LDS tile; the store is q8(F(acc*inv)) exactly as fast_dense<Normalize>.
FASTWAVE void mh_qkv_normalize_frag_c512(const unsigned char*in8,const float*w,unsigned char*out,uint tokens){
 const uint C=512,N=1536;uint l=__builtin_amdgcn_workitem_id_x(),rc=l&15,g=l>>4,block=__builtin_amdgcn_workgroup_id_x(),first=block/24*16,col=block%24*64;if(first>=tokens)return;
 const unsigned char*wb=reinterpret_cast<const unsigned char*>(w);f8 acc[4]{};
 for(uint gs=0;gs<16;gs++){const unsigned char*atile=in8+((first/16)*16+gs)*512+rc*32+g*8;
  for(uint h=0;h<2;h++){i2 a;__builtin_memcpy(&a,atile+h*16,8);
   for(uint n=0;n<4;n++){uint n0=col+n*16+rc;i2 b;__builtin_memcpy(&b,wb+((n0/16)*16+gs)*512+((h*2+g)*16+n0%16)*8,8);acc[n]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc[n]);}}}
 __attribute__((shared)) float raw[17*65];
 for(uint n=0;n<4;n++)for(uint e=0;e<8;e++)raw[(g*8+e)*65+n*16+rc]=acc[n][e];
 WG_FENCE(3);__builtin_amdgcn_s_barrier();WG_FENCE(2);
 uint part=col/C,head0=(col%C)/32,scale=4*C*C+(C/32)*4096;
 {uint row=l%16,h=l/16;float inv=1.f;if(first+row<tokens&&part<2){float ss=0.f;for(uint j=0;j<32;j++){float v=raw[row*65+h*32+j];ss+=v*v;}inv=__builtin_amdgcn_rsqf(maxf(ss,6.198883056640625e-5f))*(part==0?w[scale+head0+h]:1.f);}raw[16*65+l]=inv;}
 WG_FENCE(3);__builtin_amdgcn_s_barrier();WG_FENCE(2);
 for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){uint row=g*8+e,p=first+row,c=col+n*16+rc;if(p<tokens)out[p*N+c]=static_cast<unsigned char>(q8(F(acc[n][e]*raw[16*65+(n/2)*16+row])));}
}

// C512 attention projection in the same shape (host option c512_proj_frag): one wave = 16 tokens x 64 output columns,
// A = the attention's E4M3 AV bytes (8 contiguous bytes per lane per K16), B = fragment-tiled E4M3 projection weights
// (PackedMhWeightQkvFrag, region after the QKV rows), no LDS. Residual, epilogue (post 0 -> F(Hrtz), 3 -> Hrtz, 4 -> F)
// and the crop mapping follow fast_dense<ByteInput,Crop> with matrix_residual=0 exactly.
FASTWAVE void mh_attention_project_frag_c512(const unsigned char*av8,const float*feature,const float*w,float*out,uint tokens,uint post,uint cropw,uint croph,uint workw,uint sx,uint sy){
 const uint C=512;uint l=__builtin_amdgcn_workitem_id_x(),rc=l&15,g=l>>4,block=__builtin_amdgcn_workgroup_id_x(),first=block/8*16,col=block%8*64;if(first>=tokens)return;
 const unsigned char*wb=reinterpret_cast<const unsigned char*>(w)+size_t(3)*C*C*4;const uint skip=4*C*C+(C/32)*4096+C/32;
 f8 acc[4];
#if HIP_C512_HOIST_RES
 // Residual init with the loads outside the per-element condition: the 4 column scales once per lane, the 32 feature
 // values with a clamped row (row `first` is always < tokens), then the select. ISA of the conditional form: every
 // element re-read w[skip+c] and waited (32 global_load + s_wait_loadcnt 0 pairs per wave).
 {float ws[4];for(uint n=0;n<4;n++)ws[n]=w[skip+col+n*16+rc];
  float fv[4][8];for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){uint p=first+g*8+e;uint pp=p<tokens?p:first;fv[n][e]=feature[pp*C+col+n*16+rc];}
  for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){uint p=first+g*8+e;acc[n][e]=p<tokens?Hrtz(fv[n][e]*ws[n]):0.f;}}
#else
 for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){uint p=first+g*8+e,c=col+n*16+rc;acc[n][e]=p<tokens?Hrtz(feature[p*C+c]*w[skip+c]):0.f;}
#endif
 bool live=first+rc<tokens;const unsigned char*arow=av8+(first+rc)*C+g*8;
 for(uint k=0;k<C;k+=32){uint gs=k/32;
  for(uint h=0;h<2;h++){i2 a{};if(live)__builtin_memcpy(&a,arow+k+h*16,8);
   for(uint n=0;n<4;n++){uint n0=col+n*16+rc;i2 b;__builtin_memcpy(&b,wb+((n0/16)*16+gs)*512+((h*2+g)*16+n0%16)*8,8);acc[n]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc[n]);}}}
 for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){uint p=first+g*8+e,c=col+n*16+rc;if(p>=tokens)continue;float a=acc[n][e],v=post==3?Hrtz(a):post==4?F(a):F(Hrtz(a));uint dest=p*C+c;
  if(cropw){uint x=p%workw,y=p/workw;if(x<sx||y<sy||x>=sx+cropw||y>=sy+croph)continue;dest=((y-sy)*cropw+x-sx)*C+c;}out[dest]=v;}
}

// One wave per pixel/head. Read every channel coalesced, but preserve the
// production sequential FP32 addition order with scalar-lane extraction.
// ABI matches normalize_fast; block256, grid ceil(tokens*heads/8).
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void mh_qkv_normalize_fast_wave(const float* qkv,const float* weights,float* normalized,uint tokens,uint channels){
 uint t=__builtin_amdgcn_workitem_id_x(),row=__builtin_amdgcn_workgroup_id_x()*8+t/32,c=t%32,heads=channels/32;
 if(!valid_attn(channels)||row>=tokens*heads)return;
 uint p=row/heads,head=row%heads,scale=4*channels*channels+heads*4096;
 for(uint part=0;part<3;part++){
  uint offset=(p*3+part)*channels+head*32+c;float v=qkv[offset],inv=1.f;
  if(part<2){float sq=v*v,ss=0.f;
   #pragma unroll
   for(uint j=0;j<32;j++)ss+=value(__builtin_amdgcn_readlane(bits(sq),j));
   inv=__builtin_amdgcn_rsqf(maxf(ss,6.198883056640625e-5f))*(part==0?weights[scale+head]:1.f);
  }
  normalized[offset]=F(v*inv);
 }
}

// Byte-output ABI: same arithmetic, one byte per normalized element.
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void mh_qkv_normalize_fast_wave_fp8(const float* qkv,const float* weights,unsigned char* normalized,uint tokens,uint channels){
 uint t=__builtin_amdgcn_workitem_id_x(),row=__builtin_amdgcn_workgroup_id_x()*8+t/32,c=t%32,heads=channels/32;
 if(!valid_attn(channels)||row>=tokens*heads)return;
 uint p=row/heads,head=row%heads,scale=4*channels*channels+heads*4096;
 for(uint part=0;part<3;part++){
  uint offset=(p*3+part)*channels+head*32+c;float v=qkv[offset],inv=1.f;
  if(part<2){float sq=v*v,ss=0.f;
   #pragma unroll
   for(uint j=0;j<32;j++)ss+=value(__builtin_amdgcn_readlane(bits(sq),j));
   inv=__builtin_amdgcn_rsqf(maxf(ss,6.198883056640625e-5f))*(part==0?weights[scale+head]:1.f);
  }
  normalized[offset]=static_cast<unsigned char>(q8(F(v*inv)));
 }
}

TILE void mh_ffn_expand_fast_fp8(const float* input,const float* weights,float* hidden,uint tokens,uint channels){if(valid_c(channels))fast_dense<false,true>(input,input,weights,hidden,tokens,4*channels,channels,0,~0u,1);}

TILE void mh_ffn_contract_fast_fp8(const float* hidden,const float* weights,float* middle,uint tokens,uint channels){if(valid_c(channels))fast_dense<true,false>(hidden,hidden,weights,middle,tokens,channels,4*channels,4*channels*channels,~0u,5);}

TILE void mh_attention_project_fast_scalar_fp8(const float* av,const float* feature,const float* weights,float* output,uint tokens,uint channels,uint post){if(valid_attn(channels)&&(post==0||post==3||post==4))fast_dense<true,false>(av,feature,weights,output,tokens,channels,channels,3*channels*channels,4*channels*channels+(channels/32)*4096+channels/32,post==0?5:post);}

TILE void mh_attention_project_fast_matrix_fp8(const float* av,const float* feature,const float* weights,float* output,uint tokens,uint channels,uint post){if(valid_attn(channels)&&(post==0||post==3||post==4))fast_dense<true,false>(av,feature,weights,output,tokens,channels,channels,3*channels*channels,4*channels*channels+(channels/32)*4096+channels/32,post==0?5:post,0,0,0,1);}

TILE void mh_ffn_contract_fast_fp8_out(const float* hidden,const float* weights,float* middle,uint tokens,uint channels){if(valid_c(channels))fast_dense<true,true>(hidden,hidden,weights,middle,tokens,channels,4*channels,4*channels*channels,~0u,5);}

TILE void mh_ffn_project_fast_fp8(const float* middle,const float* feature,const float* weights,float* output,uint tokens,uint channels){if(valid_c(channels))fast_dense<true,false>(middle,feature,weights,output,tokens,channels,channels,8*channels*channels,9*channels*channels,4);}

// Full FP32 QKV accumulator is normalized on chip, preserving sequential sums.
TILE void mh_qkv_normalize_fused(const float*in,const float*w,float*out,uint tokens,uint channels){
#if HIP_QKV_SPECIALIZE
 switch(channels){
 case 64:fast_dense<false,false,true>(in,in,w,out,tokens,192,64,0,~0u,2);break;
 case 128:fast_dense<false,false,true>(in,in,w,out,tokens,384,128,0,~0u,2);break;
 case 256:fast_dense<false,false,true>(in,in,w,out,tokens,768,256,0,~0u,2);break;
 case 512:fast_dense<false,false,true>(in,in,w,out,tokens,1536,512,0,~0u,2);break;
 }
#else
 if(valid_attn(channels))fast_dense<false,false,true>(in,in,w,out,tokens,3*channels,channels,0,~0u,2);
#endif
}

#ifndef HIP_FFN_COOP_INPUT
#define HIP_FFN_COOP_INPUT 1
#endif
// Fused MH FFN:16 tokens/group, C/16 waves, hidden FP8 stays in LDS.
// External input is f32 carrying FP8 values; weights use existing packed regions.
template<uint C,bool Mapped> DEV float ffn_input(const float*in,uint p,uint c,uint width,uint height,uint workw,uint sx,uint sy){if constexpr(Mapped){int x=int(p%workw)-int(sx),y=int(p/workw)-int(sy);return x>=0&&y>=0&&x<int(width)&&y<int(height)?in[(uint(y)*width+uint(x))*C+c]:0.f;}return in[p*C+c];}
#ifndef HIP_FFN_PK_ACT
#define HIP_FFN_PK_ACT 0 /* expand activation epilogue: 1 exact byte pairing per cvt_pk, 2 packed-f16 polynomial (relaxed, PSNR-gated) */
#endif
using h2=_Float16 __attribute__((ext_vector_type(2)));using f2=float __attribute__((ext_vector_type(2)));
#ifndef HIP_FFN_ABLATE_ACT
#define HIP_FFN_ABLATE_ACT 0 /* timing-only: skip the expand activation polynomial */
#endif
#ifndef HIP_FFN_HOIST_RES
#define HIP_FFN_HOIST_RES 2 /* 2026-09-17: level 1 -0.03, level 2 -0.14, bit-exact. 1: the FFN+QKV kernels' 8 residual input reads are issued branch-free (clamped index, select after) and the scale once; ISA of the conditional form: 8 x (global_load -> s_wait_loadcnt 0) per lane */
#endif
// Branch-free twins: the load is unconditional at a clamped (in-window) index, the window test selects the value.
template<uint C,bool Mapped> DEV float ffn_input_nb(const float*in,uint p,uint c,uint width,uint height,uint workw,uint sx,uint sy){if constexpr(Mapped){int x=int(p%workw)-int(sx),y=int(p/workw)-int(sy);bool ok=x>=0&&y>=0&&x<int(width)&&y<int(height);uint xi=ok?uint(x):0u,yi=ok?uint(y):0u;float v=in[(yi*width+xi)*C+c];return ok?v:0.f;}return in[p*C+c];}
template<uint C,bool Mapped> DEV float ffn_input8_nb(const unsigned char*in,uint p,uint c,uint width,uint height,uint workw,uint sx,uint sy){if constexpr(Mapped){int x=int(p%workw)-int(sx),y=int(p/workw)-int(sy);bool ok=x>=0&&y>=0&&x<int(width)&&y<int(height);uint xi=ok?uint(x):0u,yi=ok?uint(y):0u;float v=__builtin_amdgcn_cvt_f32_fp8(int(in[(yi*width+xi)*C+c]),0);return ok?v:0.f;}return __builtin_amdgcn_cvt_f32_fp8(int(in[p*C+c]),0);}
template<uint C,bool Tiled=false,bool Project=false,bool Mapped=false,bool Grouped=false>
DEV void mh_ffn_fused_body(const float*in,const float*w,unsigned char*out,uint tokens,uint width=0,uint height=0,uint workw=0,uint sx=0,uint sy=0){
 uint tid=__builtin_amdgcn_workitem_id_x(),wave=tid/32,l=tid%32,rc=l&15,group=l>>4;
 uint first=__builtin_amdgcn_workgroup_id_x()*16;
 if(first>=tokens)return;
 __attribute__((shared)) unsigned char hidden[16*(4*C+4)];
 const uint*pw=reinterpret_cast<const uint*>(w);
#if HIP_FFN_COOP_INPUT
#if HIP_FFN_INPUT_PACK4
 for(uint i=tid;i<16*(C/4+1);i+=2*C){uint row=i/(C/4+1),k=(i%(C/4+1))*4,word=0;if(k<C)word=pack4(ffn_input<C,Mapped>(in,first+row,k,width,height,workw,sx,sy),ffn_input<C,Mapped>(in,first+row,k+1,width,height,workw,sx,sy),ffn_input<C,Mapped>(in,first+row,k+2,width,height,workw,sx,sy),ffn_input<C,Mapped>(in,first+row,k+3,width,height,workw,sx,sy));__builtin_memcpy(hidden+i*4,&word,4);}
#else
 for(uint i=tid;i<16*(C+4);i+=2*C){uint row=i/(C+4),k=i%(C+4);hidden[i]=k<C?static_cast<unsigned char>(q8(ffn_input<C,Mapped>(in,first+row,k,width,height,workw,sx,sy))):0;}
#endif
 __builtin_amdgcn_s_barrier();
#endif
 f8 expand[4]{};
 for(uint k=0;k<C;k+=16){i2 a{};
#if HIP_FFN_COOP_INPUT
  __builtin_memcpy(&a,hidden+rc*(C+4)+k+group*8,8);
#else
  for(uint e=0;e<8;e++)pack(a,e,ffn_input<C,Mapped>(in,first+rc,k+group*8+e,width,height,workw,sx,sy));
#endif
  for(uint tile=0;tile<4;tile++){uint row=wave*64+tile*16+rc,offset=(row*C+k+group*8)/4;i2 b{};if constexpr(Tiled){const unsigned char*wb=reinterpret_cast<const unsigned char*>(w);uint off=((row/16)*(C/32)+k/32)*512+(k%32+group*8)*16+row%16;for(uint e=0;e<8;e++)b[e/4]=int(uint(b[e/4])|(uint(wb[off+e*16])<<(8*(e%4))));}else b={int(pw[offset]),int(pw[offset+1])};expand[tile]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,expand[tile]);}
 }
#if HIP_FFN_COOP_INPUT
 // Every wave must finish reading input before its storage becomes hidden.
 __builtin_amdgcn_s_barrier();
#endif
 for(uint tile=0;tile<4;tile++)for(uint e=0;e<8;e++){float a=expand[tile][e],g=clampf(a,-4.f,4.f),q=absf(g)*(-.055908203125f)+.447265625f,poly=g*q+.89453125f;hidden[(group*8+e)*(4*C+4)+wave*64+tile*16+rc]=static_cast<unsigned char>(q8(F(a*poly)));}
 __builtin_amdgcn_s_barrier();
 f8 accum{};
 const uint first_k=Grouped?(wave/2)*128:0,last_k=Grouped?first_k+128:4*C;
 for(uint k=first_k;k<last_k;k+=16){i2 a;__builtin_memcpy(&a,hidden+rc*(4*C+4)+k+group*8,8);uint offset=4*C*C+((wave*16+rc)*(4*C)+k+group*8)/4;i2 b{};if constexpr(Tiled){const unsigned char*wb=reinterpret_cast<const unsigned char*>(w+4*C*C);uint row=wave*16+rc,off=((row/16)*(4*C/32)+k/32)*512+(k%32+group*8)*16+row%16;for(uint e=0;e<8;e++)b[e/4]=int(uint(b[e/4])|(uint(wb[off+e*16])<<(8*(e%4))));}else b={int(pw[offset]),int(pw[offset+1])};accum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,accum);}
 if constexpr(Project){
  __builtin_amdgcn_s_barrier();
  for(uint e=0;e<8;e++)hidden[(group*8+e)*(C+4)+wave*16+rc]=static_cast<unsigned char>(q8(F(Hrtz(accum[e]))));
  __builtin_amdgcn_s_barrier();
  f8 result{};for(uint e=0;e<8;e++)result[e]=Hrtz(ffn_input<C,Mapped>(in,first+group*8+e,wave*16+rc,width,height,workw,sx,sy)*w[9*C*C+wave*16+rc]);
  for(uint k=0;k<C;k+=16){i2 a,b;__builtin_memcpy(&a,hidden+rc*(C+4)+k+group*8,8);uint off=8*C*C+((wave*16+rc)*C+k+group*8)/4;b={int(pw[off]),int(pw[off+1])};result=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,result);}
  for(uint e=0;e<8;e++)reinterpret_cast<float*>(out)[(first+group*8+e)*C+wave*16+rc]=F(result[e]);
 }else{
 for(uint e=0;e<8;e++)out[(first+group*8+e)*C+wave*16+rc]=static_cast<unsigned char>(q8(F(Hrtz(accum[e]))));
 }
}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64(const float*in,const float*w,unsigned char*out,uint tokens){mh_ffn_fused_body<64>(in,w,out,tokens);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128(const float*in,const float*w,unsigned char*out,uint tokens){mh_ffn_fused_body<128>(in,w,out,tokens);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256(const float*in,const float*w,unsigned char*out,uint tokens){mh_ffn_fused_body<256>(in,w,out,tokens);}

KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_tiled(const float*in,const float*w,unsigned char*out,uint tokens){mh_ffn_fused_body<64,true>(in,w,out,tokens);}

KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_tiled(const float*in,const float*w,unsigned char*out,uint tokens){mh_ffn_fused_body<128,true>(in,w,out,tokens);}

KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_tiled(const float*in,const float*w,unsigned char*out,uint tokens){mh_ffn_fused_body<256,true>(in,w,out,tokens);}

KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_project(const float*in,const float*w,unsigned char*out,uint tokens){mh_ffn_fused_body<64,false,true>(in,w,out,tokens);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_project(const float*in,const float*w,unsigned char*out,uint tokens){mh_ffn_fused_body<128,false,true>(in,w,out,tokens);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_tiled_project(const float*in,const float*w,unsigned char*out,uint tokens){mh_ffn_fused_body<256,true,true>(in,w,out,tokens);}

TILE void mh_attention_crop(const float*av,const float*feature,const float*weights,float*output,uint tokens,uint post,uint cropw,uint croph,uint workw,uint sx,uint sy,uint channels){if(valid_attn(channels)&&(post==0||post==3||post==4))fast_dense<true,false,false,true>(av,feature,weights,output,tokens,channels,channels,3*channels*channels,4*channels*channels+(channels/32)*4096+channels/32,post==0?5:post,0,0,0,channels==512?0:1,cropw,croph,workw,sx,sy);}

KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_project_mapped(const float*in,const float*w,unsigned char*out,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_fused_body<64,false,true,true>(in,w,out,tokens,width,height,workw,sx,sy);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_project_mapped(const float*in,const float*w,unsigned char*out,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_fused_body<128,false,true,true>(in,w,out,tokens,width,height,workw,sx,sy);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_tiled_project_mapped(const float*in,const float*w,unsigned char*out,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_fused_body<256,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy);}

KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_project_g128(const float*in,const float*w,unsigned char*out,uint tokens){mh_ffn_fused_body<64,false,true,false,true>(in,w,out,tokens);}

KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_project_mapped_g128(const float*in,const float*w,unsigned char*out,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_fused_body<64,false,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy);}

KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_project_g128(const float*in,const float*w,unsigned char*out,uint tokens){mh_ffn_fused_body<128,false,true,false,true>(in,w,out,tokens);}

KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_project_mapped_g128(const float*in,const float*w,unsigned char*out,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_fused_body<128,false,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy);}

KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_tiled_project_g128(const float*in,const float*w,unsigned char*out,uint tokens){mh_ffn_fused_body<256,true,true,false,true>(in,w,out,tokens);}

KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_tiled_project_mapped_g128(const float*in,const float*w,unsigned char*out,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_fused_body<256,true,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy);}

// Byte result of q8(F(x)), preserving canonical zero and signed saturation.
#ifndef HIP_C512_HOIST_RES
#define HIP_C512_HOIST_RES 0 /* 1: mh_attention_project_frag_c512 residual init loads hoisted out of the p<tokens condition */
#endif
#ifndef HIP_BRANCHLESS_Q8
#define HIP_BRANCHLESS_Q8 1 /* 2026-09-17: with HIP_MH_RTZ_ISA -0.16ms, bit-exact. q8_fused_round as clamp+cvt+select (ISA: every call was a divergent branch pair, 64 per FFN+QKV kernel); same byte for every non-NaN input */
#endif
#if HIP_BRANCHLESS_Q8
DEV uint q8_fused_round(float x){uint a=bits(x)&0x7fffffffu;uint q=q8(__builtin_fminf(__builtin_fmaxf(x,-448.f),448.f));return a==0?0u:q;}
#else
DEV uint q8_fused_round(float x){uint b=bits(x),a=b&0x7fffffffu;if(!a)return 0;if(a>=0x43e00000u)return ((b>>24)&128u)|126u;return q8(x);}
#endif
// Byte residual stream (host option mh_byte_stream): the block input arrives as the E4M3 bytes the previous block's
// attention-project stored; every input value the f32 path would read is exactly cvt_f32_fp8(byte), so decoding here and
// feeding the unchanged f32 code is bit for bit the same computation. Padding outside the mapped window stays +0.
template<uint C,bool Mapped> DEV float ffn_input8(const unsigned char*in,uint p,uint c,uint width,uint height,uint workw,uint sx,uint sy){if constexpr(Mapped){int x=int(p%workw)-int(sx),y=int(p/workw)-int(sy);return x>=0&&y>=0&&x<int(width)&&y<int(height)?__builtin_amdgcn_cvt_f32_fp8(int(in[(uint(y)*width+uint(x))*C+c]),0):0.f;}return __builtin_amdgcn_cvt_f32_fp8(int(in[p*C+c]),0);}
// Preserve encoded E4M3 input (including signed zero) when staging four channels.
// Producers emit finite FP8 values; invalid mapped positions must still become positive zero.
template<uint C,bool Mapped> DEV uint ffn_input8_word(const unsigned char*in,uint p,uint c,uint width,uint height,uint workw,uint sx,uint sy){
 uint index=p*C+c;bool ok=true;
 if constexpr(Mapped){int x=int(p%workw)-int(sx),y=int(p/workw)-int(sy);ok=x>=0&&y>=0&&x<int(width)&&y<int(height);uint xi=ok?uint(x):0u,yi=ok?uint(y):0u;index=(yi*width+xi)*C+c;}
 uint word;__builtin_memcpy(&word,in+index,4);return ok?word:0u;
}
// Byte inputs can feed WMMA fragments directly; mapped padding remains positive zero.
template<uint C,bool Mapped> DEV i2 ffn_input8_fragment(const unsigned char*in,uint p,uint c,uint width,uint height,uint workw,uint sx,uint sy){
 uint index=p*C+c;bool ok=true;
 if constexpr(Mapped){int x=int(p%workw)-int(sx),y=int(p/workw)-int(sy);ok=x>=0&&y>=0&&x<int(width)&&y<int(height);uint xi=ok?uint(x):0u,yi=ok?uint(y):0u;index=(yi*width+xi)*C+c;}
 i2 v;__builtin_memcpy(&v,in+index,8);return ok?v:i2{};
}
// BatchNorm: the three QKV projections are computed first, both normalized parts' raw tiles go to LDS in one round and
// their row sums run on 2*16*(C/32) threads behind a single barrier (same sequential 32-term order per row, so the
// same bytes); two barriers and one LDS round per block fewer than the part-by-part loop. C64/C128 only (LDS budget).
// Frag: every B fragment is one contiguous 8-byte load from FragmentPackedMatrix tiles (PackedFusedMhWeightFrag for the
// FFN/project regions, PackedMhWeightQkvFragOnly for the QKV rows) instead of 16 lanes touching 16 weight rows.
DEV i2 frag_b(const unsigned char*base,uint row,uint K,uint k,uint group){i2 b;__builtin_memcpy(&b,base+((row/16)*(K/32)+k/32)*512+((((k%32)/16)*2+group)*16+row%16)*8,8);return b;}
template<uint C,bool Tiled=false,bool Project=false,bool Mapped=false,bool Grouped=false,bool ByteIn=false,bool ByteFeature=false,bool BatchNorm=false,bool Frag=false>
DEV void mh_ffn_qkv_body(const float*in,const float*w,unsigned char*out,uint tokens,uint width=0,uint height=0,uint workw=0,uint sx=0,uint sy=0,const float*aw=nullptr,unsigned char*norm=nullptr){
 const unsigned char*in8=reinterpret_cast<const unsigned char*>(in);
 auto load_in=[&](uint row,uint k)->float{if constexpr(ByteIn)return ffn_input8<C,Mapped>(in8,row,k,width,height,workw,sx,sy);else return ffn_input<C,Mapped>(in,row,k,width,height,workw,sx,sy);};
 auto load_in_nb=[&](uint row,uint k)->float{if constexpr(ByteIn)return ffn_input8_nb<C,Mapped>(in8,row,k,width,height,workw,sx,sy);else return ffn_input_nb<C,Mapped>(in,row,k,width,height,workw,sx,sy);};
 uint tid=__builtin_amdgcn_workitem_id_x(),wave=tid/32,l=tid%32,rc=l&15,group=l>>4;
 uint first=__builtin_amdgcn_workgroup_id_x()*16;
 if(first>=tokens)return;
 // A complete16-row tile in top/bottom mapped padding has exactly zero
 // bias-free FFN and normalized QKV outputs. The test is workgroup-uniform.
 if constexpr((C==128||C==256)&&Mapped){
  if(first+15<sy*workw||first>=(sy+height)*workw){
   for(uint i=tid;i<16*C;i+=2*C){if constexpr(ByteFeature)out[first*C+i]=0;else reinterpret_cast<float*>(out)[first*C+i]=0.f;}
   for(uint i=tid;i<16*3*C;i+=2*C)norm[first*3*C+i]=0;
   return;
  }
 }

 static_assert(Project&&Grouped,"FFN/QKV requires grouped project");
 union Storage {unsigned char bytes[16*(4*C+4)];float raw[(BatchNorm?2:1)*16*(C/32)*33];};
 __attribute__((shared)) Storage storage;unsigned char*hidden=storage.bytes;
 __attribute__((shared)) unsigned char qfeature[16*(C+4)];
 __attribute__((shared)) float inverse[(BatchNorm?2:1)*16*(C/32)];
 const uint*pw=reinterpret_cast<const uint*>(w);
#if HIP_FFN_COOP_INPUT
 if constexpr(!ByteIn){
#if HIP_FFN_INPUT_PACK4
#if HIP_FFN_HOIST_RES>=2
 // Level 2: the cooperative input staging also loads branch-free (4 window tests per word, loads overlap). Same bytes.
 for(uint i=tid;i<16*(C/4+1);i+=2*C){uint row=i/(C/4+1),k=(i%(C/4+1))*4,word=0;if(k<C){if constexpr(ByteIn)word=ffn_input8_word<C,Mapped>(in8,first+row,k,width,height,workw,sx,sy);else word=pack4(load_in_nb(first+row,k),load_in_nb(first+row,k+1),load_in_nb(first+row,k+2),load_in_nb(first+row,k+3));}__builtin_memcpy(hidden+i*4,&word,4);}
#else
 for(uint i=tid;i<16*(C/4+1);i+=2*C){uint row=i/(C/4+1),k=(i%(C/4+1))*4,word=0;if(k<C){if constexpr(ByteIn)word=ffn_input8_word<C,Mapped>(in8,first+row,k,width,height,workw,sx,sy);else word=pack4(load_in(first+row,k),load_in(first+row,k+1),load_in(first+row,k+2),load_in(first+row,k+3));}__builtin_memcpy(hidden+i*4,&word,4);}
#endif
#else
 for(uint i=tid;i<16*(C+4);i+=2*C){uint row=i/(C+4),k=i%(C+4);hidden[i]=k<C?static_cast<unsigned char>(q8(load_in(first+row,k))):0;}
#endif
 __builtin_amdgcn_s_barrier();
 }
#endif
 f8 expand[4]{};
 for(uint k=0;k<C;k+=16){i2 a{};
#if HIP_FFN_COOP_INPUT
  if constexpr(ByteIn)a=ffn_input8_fragment<C,Mapped>(in8,first+rc,k+group*8,width,height,workw,sx,sy);else __builtin_memcpy(&a,hidden+rc*(C+4)+k+group*8,8);
#else
  for(uint e=0;e<8;e++)pack(a,e,load_in(first+rc,k+group*8+e));
#endif
  for(uint tile=0;tile<4;tile++){uint row=wave*64+tile*16+rc,offset=(row*C+k+group*8)/4;i2 b{};if constexpr(Frag)b=frag_b(reinterpret_cast<const unsigned char*>(w),row,C,k,group);else if constexpr(Tiled){const unsigned char*wb=reinterpret_cast<const unsigned char*>(w);uint off=((row/16)*(C/32)+k/32)*512+(k%32+group*8)*16+row%16;for(uint e=0;e<8;e++)b[e/4]=int(uint(b[e/4])|(uint(wb[off+e*16])<<(8*(e%4))));}else b={int(pw[offset]),int(pw[offset+1])};expand[tile]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,expand[tile]);}
 }
#if HIP_FFN_COOP_INPUT
 // ByteIn reads global memory; only the f32 staging aliases the hidden buffer.
 if constexpr(!ByteIn)__builtin_amdgcn_s_barrier();
#endif
#if HIP_FFN_ABLATE_ACT
 for(uint tile=0;tile<4;tile++)for(uint e=0;e<8;e++)hidden[(group*8+e)*(4*C+4)+wave*64+tile*16+rc]=static_cast<unsigned char>(q8(expand[tile][e])); // timing only
#elif HIP_FFN_PK_ACT==1
 // Exact pairing: two activated values per v_cvt_pk_fp8_f32 (same clamp/RNE per element), the canonical-zero select on the bytes.
 for(uint tile=0;tile<4;tile++)for(uint e=0;e<8;e+=2){float x[2];for(uint j=0;j<2;j++){float a=expand[tile][e+j],g=clampf(a,-4.f,4.f),q=absf(g)*(-.055908203125f)+.447265625f,poly=g*q+.89453125f;x[j]=a*poly;}
  uint two=uint(__builtin_amdgcn_cvt_pk_fp8_f32(__builtin_fminf(__builtin_fmaxf(x[0],-448.f),448.f),__builtin_fminf(__builtin_fmaxf(x[1],-448.f),448.f),0,false));
  for(uint j=0;j<2;j++)hidden[(group*8+e+j)*(4*C+4)+wave*64+tile*16+rc]=static_cast<unsigned char>((bits(x[j])&0x7fffffffu)?(two>>(8*j))&255u:0u);}
#elif HIP_FFN_PK_ACT==2
 // Relaxed (PSNR-gated): the activation polynomial in packed f16 (v_pk_*), two lanes of the 8-vector per instruction;
 // the product a*poly is formed in f16 before the E4M3 cast (f32 in the exact chain), so the byte can differ by one ulp.
 for(uint tile=0;tile<4;tile++)for(uint e=0;e<8;e+=2){h2 a=__builtin_convertvector(f2{expand[tile][e],expand[tile][e+1]},h2);h2 g=__builtin_elementwise_min(__builtin_elementwise_max(a,h2{-4.f,-4.f}),h2{4.f,4.f});h2 q=__builtin_elementwise_abs(g)*h2{-.055908203125f,-.055908203125f}+h2{.447265625f,.447265625f};h2 o=a*(g*q+h2{.89453125f,.89453125f});f2 x=__builtin_convertvector(o,f2);
  uint two=uint(__builtin_amdgcn_cvt_pk_fp8_f32(__builtin_fminf(__builtin_fmaxf(x[0],-448.f),448.f),__builtin_fminf(__builtin_fmaxf(x[1],-448.f),448.f),0,false));
  for(uint j=0;j<2;j++)hidden[(group*8+e+j)*(4*C+4)+wave*64+tile*16+rc]=static_cast<unsigned char>((bits(x[j])&0x7fffffffu)?(two>>(8*j))&255u:0u);}
#else
 for(uint tile=0;tile<4;tile++)for(uint e=0;e<8;e++){float a=expand[tile][e],g=clampf(a,-4.f,4.f),q=absf(g)*(-.055908203125f)+.447265625f,poly=g*q+.89453125f;hidden[(group*8+e)*(4*C+4)+wave*64+tile*16+rc]=static_cast<unsigned char>(q8_fused_round(a*poly));}
#endif
 __builtin_amdgcn_s_barrier();
 f8 accum{};
 const uint first_k=Grouped?(wave/2)*128:0,last_k=Grouped?first_k+128:4*C;
 for(uint k=first_k;k<last_k;k+=16){i2 a;__builtin_memcpy(&a,hidden+rc*(4*C+4)+k+group*8,8);uint offset=4*C*C+((wave*16+rc)*(4*C)+k+group*8)/4;i2 b{};if constexpr(Frag)b=frag_b(reinterpret_cast<const unsigned char*>(w+4*C*C),wave*16+rc,4*C,k,group);else if constexpr(Tiled){const unsigned char*wb=reinterpret_cast<const unsigned char*>(w+4*C*C);uint row=wave*16+rc,off=((row/16)*(4*C/32)+k/32)*512+(k%32+group*8)*16+row%16;for(uint e=0;e<8;e++)b[e/4]=int(uint(b[e/4])|(uint(wb[off+e*16])<<(8*(e%4))));}else b={int(pw[offset]),int(pw[offset+1])};accum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,accum);}
 if constexpr(Project){
  __builtin_amdgcn_s_barrier();
  for(uint e=0;e<8;e++)hidden[(group*8+e)*(C+4)+wave*16+rc]=static_cast<unsigned char>(q8_fused_round(Hrtz(accum[e])));
  __builtin_amdgcn_s_barrier();
#if HIP_FFN_HOIST_RES
#if HIP_FFN_TRANSPOSED_TAIL
  f8 result{};{float rv[8],rs[8];
   for(uint e=0;e<8;e++){uint c=wave*16+group*8+e;rs[e]=w[9*C*C+c];if constexpr(ByteIn)rv[e]=ffn_input8_nb<C,Mapped>(in8,first+rc,c,width,height,workw,sx,sy);else rv[e]=ffn_input_nb<C,Mapped>(in,first+rc,c,width,height,workw,sx,sy);}
   for(uint e=0;e<8;e++)result[e]=Hrtz(rv[e]*rs[e]);}
#else
  f8 result{};{const float rs=w[9*C*C+wave*16+rc];float rv[8];
   for(uint e=0;e<8;e++){if constexpr(ByteIn)rv[e]=ffn_input8_nb<C,Mapped>(in8,first+group*8+e,wave*16+rc,width,height,workw,sx,sy);else rv[e]=ffn_input_nb<C,Mapped>(in,first+group*8+e,wave*16+rc,width,height,workw,sx,sy);}
   for(uint e=0;e<8;e++)result[e]=Hrtz(rv[e]*rs);}
#endif
#else
#if HIP_FFN_TRANSPOSED_TAIL
  f8 result{};for(uint e=0;e<8;e++)result[e]=Hrtz(load_in(first+rc,wave*16+group*8+e)*w[9*C*C+wave*16+group*8+e]);
#else
  f8 result{};for(uint e=0;e<8;e++)result[e]=Hrtz(load_in(first+group*8+e,wave*16+rc)*w[9*C*C+wave*16+rc]);
#endif
#endif
#if HIP_FFN_TRANSPOSED_TAIL
  for(uint k=0;k<C;k+=16){i2 a,b;__builtin_memcpy(&a,hidden+rc*(C+4)+k+group*8,8);uint off=8*C*C+((wave*16+rc)*C+k+group*8)/4;if constexpr(Frag)b=frag_b(reinterpret_cast<const unsigned char*>(w+8*C*C),wave*16+rc,C,k,group);else b={int(pw[off]),int(pw[off+1])};result=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b,a,result);}
#else
  for(uint k=0;k<C;k+=16){i2 a,b;__builtin_memcpy(&a,hidden+rc*(C+4)+k+group*8,8);uint off=8*C*C+((wave*16+rc)*C+k+group*8)/4;if constexpr(Frag)b=frag_b(reinterpret_cast<const unsigned char*>(w+8*C*C),wave*16+rc,C,k,group);else b={int(pw[off]),int(pw[off+1])};result=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,result);}
#endif
#if HIP_FFN_TRANSPOSED_TAIL
  for(uint e=0;e<8;e++){uint byte=q8_fused_round(result[e]);float v=__builtin_amdgcn_cvt_f32_fp8(int(byte),0);uint c=wave*16+group*8+e;if constexpr(ByteFeature)out[(first+rc)*C+c]=static_cast<unsigned char>(byte);else reinterpret_cast<float*>(out)[(first+rc)*C+c]=v;qfeature[rc*(C+4)+c]=static_cast<unsigned char>(byte);}
#else
  for(uint e=0;e<8;e++){uint byte=q8_fused_round(result[e]);float v=__builtin_amdgcn_cvt_f32_fp8(int(byte),0);if constexpr(ByteFeature)out[(first+group*8+e)*C+wave*16+rc]=static_cast<unsigned char>(byte);else reinterpret_cast<float*>(out)[(first+group*8+e)*C+wave*16+rc]=v;qfeature[(group*8+e)*(C+4)+wave*16+rc]=static_cast<unsigned char>(byte);}
#endif
  __builtin_amdgcn_s_barrier();
  const uint*aq=reinterpret_cast<const uint*>(aw);
  if constexpr(BatchNorm){
   f8 q[3]{};
   for(uint part=0;part<3;part++)for(uint k=0;k<C;k+=16){i2 a,b;__builtin_memcpy(&a,qfeature+rc*(C+4)+k+group*8,8);uint off=((part*C+wave*16+rc)*C+k+group*8)/4;if constexpr(Frag)b=frag_b(reinterpret_cast<const unsigned char*>(aw),part*C+wave*16+rc,C,k,group);else b={int(aq[off]),int(aq[off+1])};q[part]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,q[part]);}
   for(uint part=0;part<2;part++)for(uint e=0;e<8;e++)storage.raw[part*16*(C/32)*33+((wave/2)*16+group*8+e)*33+(wave%2)*16+rc]=q[part][e];
   __builtin_amdgcn_s_barrier();
   if(tid<2*16*(C/32)){uint part=tid/(16*(C/32)),t=tid%(16*(C/32)),head=t/16,row=t%16;float ss=0;for(uint k=0;k<32;k++){float v=storage.raw[part*16*(C/32)*33+(head*16+row)*33+k];ss+=v*v;}inverse[tid]=__builtin_amdgcn_rsqf(maxf(ss,6.198883056640625e-5f))*(part==0?aw[4*C*C+(C/32)*4096+head]:1.f);}
   __builtin_amdgcn_s_barrier();
   for(uint part=0;part<3;part++)for(uint e=0;e<8;e++){uint row=group*8+e;float inv=part<2?inverse[part*16*(C/32)+(wave/2)*16+row]:1.f;norm[((first+row)*3+part)*C+wave*16+rc]=static_cast<unsigned char>(q8_fused_round(q[part][e]*inv));}
  }else
#if HIP_FFN_WAVE_NORM
  {const uint heads=C/32;uint hp=wave/heads,head=wave%heads;f8 q[2]{};
   for(uint j=0;j<2;j++)for(uint k=0;k<C;k+=16){i2 a,b;__builtin_memcpy(&a,qfeature+rc*(C+4)+k+group*8,8);uint col=head*32+j*16+rc,off=((hp*C+col)*C+k+group*8)/4;if constexpr(Frag)b=frag_b(reinterpret_cast<const unsigned char*>(aw),hp*C+col,C,k,group);else b={int(aq[off]),int(aq[off+1])};q[j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b,a,q[j]);}
   float ss=0.f;for(uint j=0;j<2;j++)for(uint e=0;e<8;e++)ss+=q[j][e]*q[j][e];
   ss+=__builtin_bit_cast(float,__builtin_amdgcn_ds_bpermute(int((l^16)*4),__builtin_bit_cast(int,ss)));
   float inv=__builtin_amdgcn_rsqf(maxf(ss,6.198883056640625e-5f))*(hp==0?aw[4*C*C+(C/32)*4096+head]:1.f);
   for(uint j=0;j<2;j++)for(uint e=0;e<8;e++)norm[((first+rc)*3+hp)*C+head*32+j*16+group*8+e]=static_cast<unsigned char>(q8_fused_round(q[j][e]*inv));}
  {f8 q{};
   for(uint k=0;k<C;k+=16){i2 a,b;__builtin_memcpy(&a,qfeature+rc*(C+4)+k+group*8,8);uint off=((2*C+wave*16+rc)*C+k+group*8)/4;if constexpr(Frag)b=frag_b(reinterpret_cast<const unsigned char*>(aw),2*C+wave*16+rc,C,k,group);else b={int(aq[off]),int(aq[off+1])};q=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b,a,q);}
   for(uint e=0;e<8;e++)norm[((first+rc)*3+2)*C+wave*16+group*8+e]=static_cast<unsigned char>(q8_fused_round(q[e]*1.f));}
#else
  for(uint part=0;part<3;part++){f8 q{};
#if HIP_FFN_TRANSPOSED_TAIL
   for(uint k=0;k<C;k+=16){i2 a,b;__builtin_memcpy(&a,qfeature+rc*(C+4)+k+group*8,8);uint off=((part*C+wave*16+rc)*C+k+group*8)/4;if constexpr(Frag)b=frag_b(reinterpret_cast<const unsigned char*>(aw),part*C+wave*16+rc,C,k,group);else b={int(aq[off]),int(aq[off+1])};q=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b,a,q);}
#else
   for(uint k=0;k<C;k+=16){i2 a,b;__builtin_memcpy(&a,qfeature+rc*(C+4)+k+group*8,8);uint off=((part*C+wave*16+rc)*C+k+group*8)/4;if constexpr(Frag)b=frag_b(reinterpret_cast<const unsigned char*>(aw),part*C+wave*16+rc,C,k,group);else b={int(aq[off]),int(aq[off+1])};q=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,q);}
#endif
#if HIP_FFN_TRANSPOSED_TAIL
   if(part<2){for(uint e=0;e<8;e++)storage.raw[((wave/2)*16+rc)*33+(wave%2)*16+group*8+e]=q[e];
#else
   if(part<2){for(uint e=0;e<8;e++)storage.raw[((wave/2)*16+group*8+e)*33+(wave%2)*16+rc]=q[e];
#endif
    __builtin_amdgcn_s_barrier();
    if(tid<16*(C/32)){uint head=tid/16,row=tid%16;float ss=0;for(uint k=0;k<32;k++){float v=storage.raw[(head*16+row)*33+k];ss+=v*v;}inverse[tid]=__builtin_amdgcn_rsqf(maxf(ss,6.198883056640625e-5f))*(part==0?aw[4*C*C+(C/32)*4096+head]:1.f);}
    __builtin_amdgcn_s_barrier();
   }
#if HIP_FFN_TRANSPOSED_TAIL
   {float inv=part<2?inverse[(wave/2)*16+rc]:1.f;for(uint e=0;e<8;e++)norm[((first+rc)*3+part)*C+wave*16+group*8+e]=static_cast<unsigned char>(q8_fused_round(q[e]*inv));}
#else
   for(uint e=0;e<8;e++){uint row=group*8+e;float inv=part<2?inverse[(wave/2)*16+row]:1.f;norm[((first+row)*3+part)*C+wave*16+rc]=static_cast<unsigned char>(q8_fused_round(q[e]*inv));}
#endif
  }
#endif
 }else{
 for(uint e=0;e<8;e++)out[(first+group*8+e)*C+wave*16+rc]=static_cast<unsigned char>(q8_fused_round(Hrtz(accum[e])));
 }
}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_project_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<64,false,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_project_mapped_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<64,false,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_project_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<128,false,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_project_mapped_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<128,false,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
// Fragment-weight twins for C64/C128 (host option mh_ffn_frag).
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_frag_project_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<64,false,true,false,true,false,false,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_frag_project_mapped_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<64,false,true,true,true,false,false,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_frag_project_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<128,false,true,false,true,false,false,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_frag_project_mapped_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<128,false,true,true,true,false,false,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
// Tiled-weight twins for C64/C128 (host option tiled_ffn_small): same body as the C256 production kernel.
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_tiled_project_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<64,true,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_tiled_project_mapped_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<64,true,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_tiled_project_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<128,true,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_tiled_project_mapped_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<128,true,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_tiled_project_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<256,true,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_tiled_project_mapped_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<256,true,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
// Batched QKV normalization (host option ffn_qkv_batched_norm), C64/C128.
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_project_g128_qkv_bn(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<64,false,true,false,true,false,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_project_mapped_g128_qkv_bn(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<64,false,true,true,true,false,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_project_g128_qkv_bn(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<128,false,true,false,true,false,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_project_mapped_g128_qkv_bn(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<128,false,true,true,true,false,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
// Byte residual stream variants: _fb = feature output stored as E4M3 bytes (attention-project reads them), _bytein_fb = block
// input also read as bytes. Same body, same arithmetic; only the storage format of lattice-exact values changes.
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_project_g128_qkv_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<64,false,true,false,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_project_mapped_g128_qkv_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<64,false,true,true,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_project_g128_qkv_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<128,false,true,false,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_project_mapped_g128_qkv_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<128,false,true,true,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_tiled_project_g128_qkv_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<256,true,true,false,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_tiled_project_mapped_g128_qkv_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<256,true,true,true,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_project_g128_qkv_bytein_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<64,false,true,false,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void mh_ffn_fused_c64_project_mapped_g128_qkv_bytein_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<64,false,true,true,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_project_g128_qkv_bytein_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<128,false,true,false,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void mh_ffn_fused_c128_project_mapped_g128_qkv_bytein_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<128,false,true,true,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_tiled_project_g128_qkv_bytein_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<256,true,true,false,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_tiled_project_mapped_g128_qkv_bytein_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<256,true,true,true,true,true,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}

// C256 contiguous weight fragments; paired with PackedFusedMhWeightFrag and PackedMhWeightQkvFragOnly.
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_frag_project_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<256,false,true,false,true,false,false,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_frag_project_mapped_g128_qkv(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<256,false,true,true,true,false,false,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_frag_project_g128_qkv_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<256,false,true,false,true,false,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_frag_project_mapped_g128_qkv_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<256,false,true,true,true,false,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_frag_project_g128_qkv_bytein_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<256,false,true,false,true,true,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512))) void mh_ffn_fused_c256_frag_project_mapped_g128_qkv_bytein_fb(const float*in,const float*w,const float*aw,unsigned char*out,unsigned char*norm,uint tokens,uint width,uint height,uint workw,uint sx,uint sy){mh_ffn_qkv_body<256,false,true,true,true,true,true,false,true>(in,w,out,tokens,width,height,workw,sx,sy,aw,norm);}
