// Native gfx1201 F16 WMMA deep matrices. f32 ABI; all operands must be exact finite half values.
// Flat grid ceil(output_work_items/256), block32. One block = 16 tokens x 16 channels.
// Tokens and output channels must be divisible by16; no scalar GEMM fallback.
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

using f8=float __attribute__((ext_vector_type(8)));
using h8=_Float16 __attribute__((ext_vector_type(8)));
#define WAVE KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
DEV uint rc(){return __builtin_amdgcn_workitem_id_x()&15u;}
DEV uint group(){return __builtin_amdgcn_workitem_id_x()>>4;}
DEV uint bid(){return __builtin_amdgcn_workgroup_id_x();}
DEV float activation(float a){float g=clampf(a,-4.f,4.f);return F(H(a*H(g*H(absf(g)*(-.055908203125f)+.447265625f)+.89453125f)));}
// K32 group sum begins at zero; round H(previous + group) only after BOTH K16 MMAs.
DEV f8 dot(const float*a,const float*b,uint stride_a,uint stride_b,uint first,uint row,uint off_a,uint off_b,uint begin,uint end,f8 acc,uint valid_tokens=~0u){
 for(uint k=begin;k<end;k+=32){f8 sum{};
  #pragma unroll
  for(uint tile=0;tile<2;tile++){h8 av{},bv{};
   #pragma unroll
   for(uint e=0;e<8;e++){uint j=k+tile*16+group()*8+e;av[e]=first+rc()<valid_tokens?(_Float16)a[(first+rc())*stride_a+off_a+j]:(_Float16)0;bv[e]=(_Float16)b[off_b+(row+rc())*stride_b+j];}
   sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(av,bv,sum);
  }
  #pragma unroll
  for(uint e=0;e<8;e++)acc[e]=H(acc[e]+sum[e]);
 }
 return acc;
}
DEV f8 residual(const float*input,const float*weights,uint first,uint row,uint channels,uint offset){f8 a{};
 #pragma unroll
 for(uint e=0;e<8;e++)a[e]=H(input[(first+group()*8+e)*channels+row+rc()]*weights[offset+row+rc()]);return a;
}
DEV void store(float*out,f8 a,uint first,uint row,uint channels,uint activated){
 #pragma unroll
 for(uint e=0;e<8;e++)out[(first+group()*8+e)*channels+row+rc()]=activated?activation(a[e]):F(a[e]);
}
WAVE void split_mix(const float*input,const float*weights,float*output,uint tokens){uint first=bid()/32*16,row=bid()%32*16;if(first>=tokens)return;f8 a{};a=dot(input,weights,512,512,first,row,0,0,0,512,a);store(output,a,first,row,512,0);}
WAVE void split_expand(const float*input,const float*weights,float*output,uint tokens){uint first=bid()/128*16,col=bid()%128*16,g=col/256,row=col%256;if(first>=tokens)return;f8 a{};a=dot(input,weights,512,64,first,row,g*64,262144+g*16384,0,64,a);store(output,a,first,col,2048,1);}
WAVE void split_contract(const float*input,const float*weights,float*output,uint tokens){uint first=bid()/32*16,col=bid()%32*16,g=col/64,row=col%64;if(first>=tokens)return;f8 a{};a=dot(input,weights,2048,256,first,row,g*256,393216+g*16384,0,256,a);store(output,a,first,col,512,0);}
WAVE void split_projection(const float*input,const float*weights,const float*skip,float*output,uint tokens){uint first=bid()/32*16,row=bid()%32*16;if(first>=tokens)return;f8 a=residual(skip,weights,first,row,512,262144);a=dot(input,weights,512,512,first,row,0,0,0,512,a);store(output,a,first,row,512,0);}
WAVE void vit_expand(const float*input,const float*weights,float*output,uint tokens,uint inputs,uint outputs){uint tiles=outputs/16,first=bid()/tiles*16,row=bid()%tiles*16;if(first>=tokens)return;f8 a{};a=dot(input,weights,inputs,inputs,first,row,0,0,0,inputs,a);store(output,a,first,row,outputs,1);}
WAVE void vit_project(const float*input,const float*weights,const float*skip,float*output,uint tokens,uint inputs,uint outputs){uint tiles=outputs/16,first=bid()/tiles*16,row=bid()%tiles*16;if(first>=tokens)return;f8 total{};
 for(uint p=0;p<4;p++){f8 a{};if(p==0)a=residual(skip,weights,first,row,outputs,inputs*outputs);a=dot(input,weights,inputs,inputs,first,row,0,0,p*(inputs/4),(p+1)*(inputs/4),a);
  #pragma unroll
  for(uint e=0;e<8;e++)total[e]=p?H(total[e]+a[e]):a[e];
 }store(output,total,first,row,outputs,0);
}
WAVE void vit_qkv_project(const float*input,const float*weights,float*output,uint tokens){uint perpart=(tokens/16)*64,part=bid()/perpart,local=bid()%perpart,first=local/64*16,row=local%64*16;if(part>=3)return;f8 total{};
 for(uint p=0;p<2;p++){f8 a{};a=dot(input,weights,1024,1024,first,row,0,part*1048576,p*512,(p+1)*512,a);
  #pragma unroll
  for(uint e=0;e<8;e++)total[e]=p?H(total[e]+a[e]):a[e];
 }
 #pragma unroll
 for(uint e=0;e<8;e++)output[(part*tokens+first+group()*8+e)*1024+row+rc()]=total[e];
}
WAVE void decoder_project2x(const float*input,const float*weights,const float*skip,float*output,uint iw,uint ih,uint ow,uint oh,uint inputs,uint outputs){uint tiles=outputs/16,first=bid()/tiles*16,row=bid()%tiles*16;if(first>=iw*ih)return;uint partitions=inputs==1024?4:1;f8 total{};
 for(uint p=0;p<partitions;p++){f8 a{};a=dot(input,weights,inputs,inputs,first,row,0,0,p*(inputs/partitions),(p+1)*(inputs/partitions),a,iw*ih);
  #pragma unroll
  for(uint e=0;e<8;e++)total[e]=p?H(total[e]+a[e]):a[e];
 }
 #pragma unroll
 for(uint e=0;e<8;e++){uint token=first+group()*8+e;if(token>=iw*ih)continue;
  #pragma unroll
  for(uint dy=0;dy<2;dy++){
   #pragma unroll
   for(uint dx=0;dx<2;dx++){uint x=(token%iw)*2+dx,y=(token/iw)*2+dy;if(x>=ow||y>=oh)continue;uint i=(y*ow+x)*outputs+row+rc();float merged=H(total[e]+F(skip[i])*weights[inputs*outputs+row+rc()]);output[i]=outputs==32?merged:F(merged);}
  }
 }
}
