// Native gfx1201 FP8 WMMA C32 FFN. Host ABI remains f32; kernels are separate from the scalar reference.
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
// One wave computes 16 tokens x 16 output channels. A row and B column use lane%16;
// each half-wave supplies eight K entries, and D rows are half-wave*8+element.
KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
void c32_ffn_expand_wmma(const float* input,const float* weights,float* hidden,uint tokens){
 uint block=__builtin_amdgcn_workgroup_id_x(),first=(block/8)*16u,col=(block%8)*16u;
 if(first>=tokens||col>=128)return;
 uint lane=__builtin_amdgcn_workitem_id_x(),rc=lane&15,group=lane>>4;
 float8 sum{};
 #pragma unroll
 for(uint tile=0;tile<2;tile++){
  int2 a{},b{};
  #pragma unroll
  for(uint e=0;e<8;e++){uint k=tile*16+group*8+e;uint av=first+rc<tokens?packed_fp8(F(input[(first+rc)*32+k])):0u;uint bv=packed_fp8(weights[512+(col+rc)*32+k]);a[e/4]=int(uint(a[e/4])|(av<<(8*(e%4))));b[e/4]=int(uint(b[e/4])|(bv<<(8*(e%4))));}
  sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,sum);
 }
 #pragma unroll
 for(uint e=0;e<8;e++){uint p=first+group*8+e;if(p>=tokens)continue;float a=H(sum[e]),g=clampf(a,-4.f,4.f);float poly=H(g*H(absf(g)*(-.055908203125f)+.447265625f)+.89453125f);hidden[p*128+col+rc]=F(H(a*poly));}
}
KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
void c32_ffn_contract_wmma(const float* input,const float* hidden,const float* weights,float* output,uint tokens,uint raw_output){
 uint block=__builtin_amdgcn_workgroup_id_x(),first=(block/2)*16u,col=(block%2)*16u;
 if(first>=tokens||col>=32)return;
 uint lane=__builtin_amdgcn_workitem_id_x(),rc=lane&15,group=lane>>4;
 float8 residual{};
 #pragma unroll
 for(uint e=0;e<8;e++){uint p=first+group*8+e;residual[e]=p<tokens?H(input[p*32+col+rc]*weights[8704+col+rc]):0.f;}
 #pragma unroll
 for(uint chunk=0;chunk<4;chunk++){
  // Keep the residual outside the K16 MMA seed: sum(K32) starts from zero.
  float8 sum{};
  #pragma unroll
  for(uint tile=0;tile<2;tile++){
   int2 a{},b{};
   #pragma unroll
   for(uint e=0;e<8;e++){uint k=chunk*32+tile*16+group*8+e;uint av=first+rc<tokens?packed_fp8(hidden[(first+rc)*128+k]):0u;uint bv=packed_fp8(weights[4608+(col+rc)*128+k]);a[e/4]=int(uint(a[e/4])|(av<<(8*(e%4))));b[e/4]=int(uint(b[e/4])|(bv<<(8*(e%4))));}
   sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,sum);
  }
  #pragma unroll
  for(uint e=0;e<8;e++)residual[e]=H(residual[e]+sum[e]);
 }
 #pragma unroll
 for(uint e=0;e<8;e++){uint p=first+group*8+e;if(p<tokens)output[p*32+col+rc]=raw_output?residual[e]:F(residual[e]);}
}

DEV float half_add_midpoint(float a,float b){float sum=a+b,vb=sum-a,error=(a-(sum-vb))+(b-vb);uint u=bits(sum),m=u&0x7fffffffu;
 if(m>=0x38800000u&&m<0x47800000u&&(m&0x1fffu)==0x1000u&&error!=0)sum=value(((error>0)==(sum>0))?u+1:u-1);return H(sum);}
DEV float exp_approx(float x){float a=clampf(H(x*.044921875f+1.30078125f),1.03125f,1.5693359375f);return from_half(((half_bits(a)<<5)+0x8000u)&65535u);}

// Bases already select the wave lane's A row and B column. K stride is in floats.
DEV float8 matrix32(const float*a,const float*b,uint abase,uint bbase,uint astep,uint bstep){
 uint group=__builtin_amdgcn_workitem_id_x()>>4;float8 sum{};
 #pragma unroll
 for(uint tile=0;tile<2;tile++){int2 pa{},pb{};
  #pragma unroll
  for(uint e=0;e<8;e++){uint k=tile*16+group*8+e;uint av=packed_fp8(a[abase+k*astep]),bv=packed_fp8(b[bbase+k*bstep]);pa[e/4]=int(uint(pa[e/4])|(av<<(8*(e%4))));pb[e/4]=int(uint(pb[e/4])|(bv<<(8*(e%4))));}
  sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(pa,pb,sum);
 }return sum;
}
KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
void c32_attn_qkv_wmma(const float*input,const float*weights,float*qkv,uint tokens){
 uint block=__builtin_amdgcn_workgroup_id_x(),perpart=tokens/8,part=block/perpart,local=block%perpart,first=(local/2)*16,col=(local%2)*16;
 if(part>=3)return;uint lane=__builtin_amdgcn_workitem_id_x(),rc=lane&15,group=lane>>4;
 float8 sum=matrix32(input,weights,(first+rc)*32,part*1024+(col+rc)*32,1,1);
 #pragma unroll
 for(uint e=0;e<8;e++){uint p=first+group*8+e;float v=H(sum[e]);qkv[(part*tokens+p)*32+col+rc]=part==2?F(v):v;}
}
KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
void c32_attn_scores_wmma(const float*normalized,const float*weights,float*ex,uint windows){
 uint block=__builtin_amdgcn_workgroup_id_x(),w=block/16,qr=((block%16)/4)*16,kr=(block%4)*16;
 if(w>=windows)return;uint lane=__builtin_amdgcn_workitem_id_x(),rc=lane&15,group=lane>>4,tokens=windows*64;
 float8 sum=matrix32(normalized,normalized,(w*64+qr+rc)*32,(tokens+w*64+kr+rc)*32,1,1);
 #pragma unroll
 for(uint e=0;e<8;e++){uint q=qr+group*8+e,k=kr+rc;ex[(w*64+q)*64+k]=exp_approx(H(sum[e]+weights[4096+q*64+k]));}
}
KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
void c32_attn_av_wmma(const float*prob,const float*qkv,float*av,uint windows){
 uint block=__builtin_amdgcn_workgroup_id_x(),w=block/8,qr=((block%8)/2)*16,col=(block%2)*16;
 if(w>=windows)return;uint lane=__builtin_amdgcn_workitem_id_x(),rc=lane&15,group=lane>>4,tokens=windows*64;float8 acc{};
 #pragma unroll
 for(uint chunk=0;chunk<2;chunk++){float8 sum=matrix32(prob,qkv,(w*64+qr+rc)*64+chunk*32,(2*tokens+w*64+chunk*32)*32+col+rc,1,32);
  #pragma unroll
  for(uint e=0;e<8;e++)acc[e]=H(acc[e]+sum[e]);
 }
 #pragma unroll
 for(uint e=0;e<8;e++)av[(w*64+qr+group*8+e)*32+col+rc]=F(acc[e]);
}
KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
void c32_attn_project_wmma(const float*input,const float*av,const float*weights,float*output,uint tokens,uint raw_output){
 uint block=__builtin_amdgcn_workgroup_id_x(),first=(block/2)*16,col=(block%2)*16;
 if(first>=tokens)return;uint lane=__builtin_amdgcn_workitem_id_x(),rc=lane&15,group=lane>>4;
 float8 sum=matrix32(av,weights,(first+rc)*32,3072+(col+rc)*32,1,1);
 #pragma unroll
 for(uint e=0;e<8;e++){uint p=first+group*8+e;float result=half_add_midpoint(sum[e],H(input[p*32+col+rc]*weights[8193+col+rc]));output[p*32+col+rc]=raw_output?result:F(result);}
}
