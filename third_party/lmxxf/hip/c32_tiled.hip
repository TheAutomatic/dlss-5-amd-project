// Experimental C32 LDS-tiled WMMA. Separate module; existing graph/ABI are untouched.
#define HIP_ISA_HALF 1
#pragma clang fp contract(off)
#pragma clang fp reassociate(off)
typedef unsigned uint;
#define DEV __attribute__((device)) __attribute__((always_inline))
#define KERNEL extern "C" __attribute__((global))
DEV uint bits(float x){union{float f;uint u;}v;v.f=x;return v.u;}
DEV float value(uint x){union{float f;uint u;}v;v.u=x;return v.f;}
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
DEV float H(float x){return from_half(half_bits(x));}
#endif
DEV float F(float x){uint b=bits(x),a=b&0x7fffffffu,s=b&0x80000000u;if(a==0)return 0.f;
 if(a>=0x43e00000u)return value(s|0x43e00000u); // finite E4M3 saturation, including overflow
 if(a<0x3c800000u){float scaled=absf(x)*512.f;uint q=uint(scaled);float r=scaled-float(q);q+=uint(r>.5f||(r==.5f&&(q&1u)));float f=float(q)/512.f;return s?-f:f;}
 uint r=(a+0x7ffffu+((a>>20)&1u))&0xfff00000u;return value(s|(r>0x43e00000u?0x43e00000u:r));}

using int2=int __attribute__((ext_vector_type(2)));
using float8=float __attribute__((ext_vector_type(8)));
DEV uint packed_fp8(float x){x=__builtin_amdgcn_fmed3f(x,-448.f,448.f);return uint(__builtin_amdgcn_cvt_pk_fp8_f32(x,x,0,false))&255u;}
DEV float half_add_midpoint(float a,float b){float sum=a+b,vb=sum-a,error=(a-(sum-vb))+(b-vb);uint u=bits(sum),m=u&0x7fffffffu;
 if(m>=0x38800000u&&m<0x47800000u&&(m&0x1fffu)==0x1000u&&error!=0)sum=value(((error>0)==(sum>0))?u+1:u-1);return H(sum);}

DEV uint pack4(const float*p){
 float x0=__builtin_amdgcn_fmed3f(p[0],-448.f,448.f),x1=__builtin_amdgcn_fmed3f(p[1],-448.f,448.f);
 float x2=__builtin_amdgcn_fmed3f(p[2],-448.f,448.f),x3=__builtin_amdgcn_fmed3f(p[3],-448.f,448.f);
 int lo=__builtin_amdgcn_cvt_pk_fp8_f32(x0,x1,0,false);
 return uint(__builtin_amdgcn_cvt_pk_fp8_f32(x2,x3,lo,true));
}
DEV void sync_block(){__builtin_amdgcn_fence(3,"workgroup");__builtin_amdgcn_s_barrier();__builtin_amdgcn_fence(2,"workgroup");}
// Mode 0 expand, 1 contract, 2 QKV, 3 projection. LDS rows hold eight packed K32 words.
template<uint Mode> DEV void tiled(const float*input,const float*operand,const float*weights,float*output,uint tokens,uint raw_output){
 constexpr uint N=Mode==0?64:32,Threads=Mode==0?512:256,K=Mode==1?128:32;
 __attribute__((shared)) uint a8[64*8];
 __attribute__((shared)) uint b8[N*8];
 uint id=__builtin_amdgcn_workgroup_id_x(),blocks=(tokens+63)/64,part=0,first=0,col=0;
 if(Mode==0){first=(id/2)*64;col=(id%2)*64;}else if(Mode==2){part=id/blocks;first=(id%blocks)*64;if(part>=3)return;}else first=id*64;
 if(first>=tokens)return;
 uint tid=__builtin_amdgcn_workitem_id_x(),wave=tid/32,lane=tid%32,rc=lane&15,half=lane>>4;
 uint wave_row=(wave/(N/16))*16,wave_col=(wave%(N/16))*16;
 float8 acc{};
 if(Mode==1){
  #pragma unroll
  for(uint e=0;e<8;e++){uint p=first+wave_row+half*8+e;acc[e]=p<tokens?H(input[p*32+wave_col+rc]*weights[8704+wave_col+rc]):0.f;}
 }
 #pragma unroll
 for(uint chunk=0;chunk<K/32;chunk++){
  // Coalesced f32 loads and exactly one conversion per input/weight value per block.
  for(uint i=tid;i<64*8;i+=Threads){uint row=i/8,k=(i%8)*4+chunk*32;const float*src=Mode==1||Mode==3?operand:input;uint stride=Mode==1?128:32;a8[i]=first+row<tokens?pack4(src+(first+row)*stride+k):0u;}
  for(uint i=tid;i<N*8;i+=Threads){uint row=col+i/8,k=(i%8)*4+chunk*32;uint offset=Mode==0?512:Mode==1?4608:Mode==2?part*1024:3072;b8[i]=pack4(weights+offset+row*K+k);}
  sync_block();float8 sum{};
  #pragma unroll
  for(uint tile=0;tile<2;tile++){uint ai=(wave_row+rc)*8+tile*4+half*2,bi=(wave_col+rc)*8+tile*4+half*2;
   int2 a={int(a8[ai]),int(a8[ai+1])},b={int(b8[bi]),int(b8[bi+1])};
   sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,sum);
  }
  if(Mode==1){
   #pragma unroll
   for(uint e=0;e<8;e++)acc[e]=H(acc[e]+sum[e]);
  }else acc=sum;
  sync_block();
 }
 #pragma unroll
 for(uint e=0;e<8;e++){uint p=first+wave_row+half*8+e,c=col+wave_col+rc;if(p>=tokens)continue;float v=acc[e];
  if(Mode==0){float a=H(v),g=clampf(a,-4.f,4.f),poly=H(g*H(absf(g)*(-.055908203125f)+.447265625f)+.89453125f);output[p*128+c]=F(H(a*poly));}
  else if(Mode==1)output[p*32+c]=raw_output?v:F(v);
  else if(Mode==2){v=H(v);output[(part*tokens+p)*32+c]=part==2?F(v):v;}
  else{v=half_add_midpoint(v,H(input[p*32+c]*weights[8193+c]));output[p*32+c]=raw_output?v:F(v);}
 }
}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void c32_ffn_expand_tiled(const float*input,const float*weights,float*hidden,uint tokens){tiled<0>(input,input,weights,hidden,tokens,1);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void c32_ffn_contract_tiled(const float*input,const float*hidden,const float*weights,float*output,uint tokens,uint raw_output){tiled<1>(input,hidden,weights,output,tokens,raw_output);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void c32_attn_qkv_tiled(const float*input,const float*weights,float*qkv,uint tokens){tiled<2>(input,input,weights,qkv,tokens,1);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void c32_attn_project_tiled(const float*input,const float*av,const float*weights,float*output,uint tokens,uint raw_output){tiled<3>(input,av,weights,output,tokens,raw_output);}
