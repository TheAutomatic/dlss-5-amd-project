// Native gfx1201 FP8 WMMA multihead kernels. Original externalfloat32 ABI.
// Mirrors native_c64.hlsl non-wave math, native_c64_shift.hlsl, native_c64_ds.hlsl.
// Host validates geometry/channel contracts; all tensors/weights externally float32.
// Every exported kernel uses one-dimensional blocks of exactly32 threads.
#pragma clang fp contract(off)
#pragma clang fp reassociate(off)
typedef unsigned uint;
#define DEV __attribute__((device)) __attribute__((always_inline))
#define KERNEL extern "C" __attribute__((global))
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
DEV float F(float x){uint b=bits(x),a=b&0x7fffffffu,s=b&0x80000000u;if(a==0)return 0.f;
 if(a>=0x43e00000u)return value(s|0x43e00000u); // finite E4M3 saturation, including overflow
 if(a<0x3c800000u){float scaled=absf(x)*512.f;uint q=uint(scaled);float r=scaled-float(q);q+=uint(r>.5f||(r==.5f&&(q&1u)));float f=float(q)/512.f;return s?-f:f;}
 uint r=(a+0x7ffffu+((a>>20)&1u))&0xfff00000u;return value(s|(r>0x43e00000u?0x43e00000u:r));}
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
#define WAVE KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
DEV uint q8(float x){return uint(__builtin_amdgcn_cvt_pk_fp8_f32(x,x,0,false))&255u;}
DEV void pack(i2&v,uint e,float x){v[e/4]=int(uint(v[e/4])|(q8(x)<<(8*(e%4))));}
// Dense row-major A[M,K] times transposed stored weight[N,K]. A/weight must
// already be exact finite E4M3 lattice; residual feature/scales remainfloat32.
// epilogue0=F,1=gate+F,2=rawH; skip_offset=~0u disables residual.
DEV void dense(const float* input,const float* feature,const float* weights,float* output,
 uint M,uint N,uint K,uint matrix_offset,uint skip_offset,uint epilogue,
 uint width=0,uint valid_width=0,uint valid_height=0){
 uint tile=__builtin_amdgcn_workgroup_id_x(),nt=N/16;
 if(!nt||tile>=((M+15)/16)*nt)return;
 uint first=(tile/nt)*16,col=(tile%nt)*16,l=__builtin_amdgcn_workitem_id_x(),rc=l&15,group=l>>4;
 f8 accum{};
 #pragma unroll
 for(uint e=0;e<8;e++){uint p=first+group*8+e;if(p<M&&skip_offset!=~0u)accum[e]=H(feature[p*N+col+rc]*weights[skip_offset+col+rc]);}
 for(uint chunk=0;chunk<K/32;chunk++){
  f8 sum{};
  #pragma unroll
  for(uint half=0;half<2;half++){
   i2 a{},b{};
   #pragma unroll
   for(uint e=0;e<8;e++){uint k=chunk*32+half*16+group*8+e,p=first+rc;float x=0;
    if(p<M&&(!valid_width||(p%width<valid_width&&p/width<valid_height)))x=input[p*K+k];
    pack(a,e,x);pack(b,e,weights[matrix_offset+(col+rc)*K+k]);}
   sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,sum);
  }
  // K32 sum starts from ZERO. Previous H/residual must NOT seed the WMMA.
  #pragma unroll
  for(uint e=0;e<8;e++)accum[e]=H(accum[e]+sum[e]);
 }
 #pragma unroll
 for(uint e=0;e<8;e++){uint p=first+group*8+e;if(p>=M)continue;float a=accum[e],v;
  if(epilogue==1){float gate=clampf(a,-4.f,4.f),poly=H(gate*H(absf(gate)*(-.055908203125f)+.447265625f)+.89453125f);v=F(H(a*poly));}
  else v=epilogue==2?a:F(a);
  output[p*N+col+rc]=(valid_width&&(p%width>=valid_width||p/width>=valid_height))?0.f:v;
 }
}
WAVE void mh_ffn_expand_wmma(const float* input,const float* weights,float* hidden,uint tokens,uint channels){if(valid_c(channels))dense(input,input,weights,hidden,tokens,4*channels,channels,0,~0u,1);}
WAVE void mh_ffn_contract_wmma(const float* hidden,const float* weights,float* middle,uint tokens,uint channels){if(valid_c(channels))dense(hidden,hidden,weights,middle,tokens,channels,4*channels,4*channels*channels,~0u,0);}
WAVE void mh_ffn_project_wmma(const float* middle,const float* feature,const float* weights,float* output,uint tokens,uint channels){if(valid_c(channels))dense(middle,feature,weights,output,tokens,channels,channels,8*channels*channels,9*channels*channels,0);}
WAVE void mh_qkv_wmma(const float* input,const float* weights,float* qkv,uint tokens,uint channels){if(valid_attn(channels))dense(input,input,weights,qkv,tokens,3*channels,channels,0,~0u,2);}
WAVE void mh_attention_project_wmma(const float* av,const float* feature,const float* weights,float* output,uint tokens,uint channels,uint raw_output){if(valid_attn(channels))dense(av,feature,weights,output,tokens,channels,channels,3*channels*channels,4*channels*channels+(channels/32)*4096+channels/32,raw_output?2:0);}
WAVE void mh_split_project_wmma(const float* input,const float* feature,const float* weights,float* output,uint tokens,uint channels){if(valid_attn(channels))dense(input,feature,weights,output,tokens,channels,channels,0,channels*channels,0);}
WAVE void mh_pool_project_wmma(const float* pooled,const float* weights,float* output,uint width,uint height,uint valid_width,uint valid_height,uint channels){if(valid_spatial(channels))dense(pooled,pooled,weights,output,width*height,2*channels,channels,0,~0u,0,width,valid_width,valid_height);}
// Each window/head has16 tiles (4query blocks x4key blocks).
WAVE void mh_scores_exp_wmma(const float* qk,const float* weights,float* ex,uint width,uint height,uint channels){
 uint tile=__builtin_amdgcn_workgroup_id_x(),heads=channels/32,windows=(width/8)*(height/8);
 if(!valid_attn(channels)||width%8||height%8||tile>=windows*heads*16)return;
 uint wh=tile/16,win=wh/heads,head=wh%heads,q=(tile%16)/4*16,kcol=(tile%4)*16;
 uint l=__builtin_amdgcn_workitem_id_x(),rc=l&15,group=l>>4;f8 sum{};
 #pragma unroll
 for(uint half=0;half<2;half++){i2 a{},b{};
  #pragma unroll
  for(uint e=0;e<8;e++){uint c=half*16+group*8+e,qp=pixel_in_window(win,q+rc,width),kp=pixel_in_window(win,kcol+rc,width);pack(a,e,qk[(qp*2)*channels+head*32+c]);pack(b,e,qk[(kp*2+1)*channels+head*32+c]);}
  sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,sum);
 }
 #pragma unroll
 for(uint e=0;e<8;e++){uint qr=q+group*8+e,k=kcol+rc;ex[(wh*64+qr)*64+k]=exp_approx(H(sum[e]+weights[4*channels*channels+head*4096+qr*64+k]));}
}
// Each window/head has8 tiles (4query blocks x2channel blocks).
WAVE void mh_attention_av_wmma(const float* prob,const float* qkv,float* av,uint width,uint height,uint channels){
 uint tile=__builtin_amdgcn_workgroup_id_x(),heads=channels/32,windows=(width/8)*(height/8);
 if(!valid_attn(channels)||width%8||height%8||tile>=windows*heads*8)return;
 uint wh=tile/8,win=wh/heads,head=wh%heads,q=(tile%8)/2*16,col=(tile%2)*16;
 uint l=__builtin_amdgcn_workitem_id_x(),rc=l&15,group=l>>4;f8 accum{};
 #pragma unroll
 for(uint chunk=0;chunk<2;chunk++){f8 sum{};
  #pragma unroll
  for(uint half=0;half<2;half++){i2 a{},b{};
   #pragma unroll
   for(uint e=0;e<8;e++){uint key=chunk*32+half*16+group*8+e,kp=pixel_in_window(win,key,width);pack(a,e,prob[(wh*64+q+rc)*64+key]);pack(b,e,F(qkv[(kp*3+2)*channels+head*32+col+rc]));}
   sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,sum);
  }
  #pragma unroll
  for(uint e=0;e<8;e++)accum[e]=H(accum[e]+sum[e]);
 }
 #pragma unroll
 for(uint e=0;e<8;e++){uint p=pixel_in_window(win,q+group*8+e,width);av[p*channels+head*32+col+rc]=F(accum[e]);}
}
