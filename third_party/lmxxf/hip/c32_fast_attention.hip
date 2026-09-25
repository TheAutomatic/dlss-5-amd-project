// Production C32 FAST4 attention precision, separate from exact reference.
// All kernels flat grid ceil(work_items/256), block32; token count divisible64.
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
#ifndef HIP_NATIVE_FP8_F
#define HIP_NATIVE_FP8_F 1
#endif
#if HIP_NATIVE_FP8_F
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

using i2=int __attribute__((ext_vector_type(2)));
using f8=float __attribute__((ext_vector_type(8)));
using h8=_Float16 __attribute__((ext_vector_type(8)));
#define WAVE KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
DEV uint rc(){return __builtin_amdgcn_workitem_id_x()&15;}
DEV uint gr(){return __builtin_amdgcn_workitem_id_x()>>4;}
DEV uint bid(){return __builtin_amdgcn_workgroup_id_x();}
// D3D explicit f32tof16 maps to RTZ here; matrix Cast<F16> remains RNE.
#ifndef HIP_NATIVE_RTZ
#define HIP_NATIVE_RTZ 1
#endif
#if HIP_NATIVE_RTZ
DEV float Hrtz(float x){uint h;float r;asm("v_cvt_pkrtz_f16_f32 %0, %1, %1":"=v"(h):"v"(x));asm("v_cvt_f32_f16 %0, %1":"=v"(r):"v"(h));return r;}
#else
DEV float Hrtz(float x){uint b=bits(x),a=b&0x7fffffffu,sg=(b>>16)&0x8000u;int e=int(a>>23)-127;
 if(a>=0x7f800000u)return from_half(sg|0x7c00u|((a&0x7fffffu)?0x200u:0u));
 if(e>15)return from_half(sg|0x7bffu);
 if(e<-24)return from_half(sg);
 if(e<-14)return from_half(sg|(((a&0x7fffffu)|0x800000u)>>uint(-e-1)));
 return from_half(sg|((a>>13)-0x1c000u));
}
#endif
DEV uint fp8(float f){return uint(__builtin_amdgcn_cvt_pk_fp8_f32(clampf(f,-448.f,448.f),0.f,0,false))&255u;}
DEV void put(i2&v,uint e,float x){v[e/4]=int(uint(v[e/4])|(fp8(x)<<(8*(e%4))));}
// QKV output [part][tokens][32], raw F32 dot; V quantizes only in normalize.
WAVE void c32_fast_qkv(const float*in,const float*w,float*out,uint tokens){uint first=bid()/6*16,part=(bid()%6)/2,col=(bid()%2)*16;if(first>=tokens)return;f8 a{};
 for(uint tile=0;tile<2;tile++){i2 x{},y{};for(uint e=0;e<8;e++){uint k=tile*16+gr()*8+e;put(x,e,in[(first+rc())*32+k]);put(y,e,w[part*1024+(col+rc())*32+k]);}a=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(x,y,a);}
 for(uint e=0;e<8;e++)out[(part*tokens+first+gr()*8+e)*32+col+rc()]=a[e];
}
// work_items=tokens*96. Each output16-channel tile independently computes its row norm.
WAVE void c32_fast_normalize(const float*in,const float*w,float*out,uint tokens){uint first=bid()/6*16,part=(bid()%6)/2,col=(bid()%2)*16;if(first>=tokens)return;f8 sum{};
 if(part<2){for(uint tile=0;tile<2;tile++){h8 x{},ones{};for(uint e=0;e<8;e++){uint k=tile*16+gr()*8+e;float v=in[(part*tokens+first+rc())*32+k];x[e]=(_Float16)(v*v);ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,ones,sum);}}
 for(uint e=0;e<8;e++){uint i=(part*tokens+first+gr()*8+e)*32+col+rc();float v=in[i];if(part<2)v*=__builtin_amdgcn_rsqf(maxf(sum[e],6.198883056640625e-5f))*(part==0?w[8192]:1.f);out[i]=F(v);}
}
// ex [window][query64][key64], work_items=windows*4096.
WAVE void c32_fast_scores(const float*in,const float*w,float*out,uint windows){uint tile=bid()%16,window=bid()/16,qfirst=window*64+(tile/4)*16,key=(tile%4)*16,tokens=windows*64;if(window>=windows)return;f8 a{};
 for(uint kt=0;kt<2;kt++){i2 x{},y{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put(x,e,in[(qfirst+rc())*32+k]);put(y,e,in[(tokens+window*64+key+rc())*32+k]);}a=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(x,y,a);}
 for(uint e=0;e<8;e++){uint q=(tile/4)*16+gr()*8+e,k=key+rc();float score=a[e]+w[4096+q*64+k];float affine=clampf(score*.044921875f+1.30078125f,1.03125f,1.5693359375f);// Production f32tof16 here is RTZ; affine is positive normal in [1.03125,1.5693359375].
 uint ah=(bits(affine)>>13)-0x1c000u;out[(window*64+q)*64+k]=from_half(((ah<<5)+0x8000u)&65535u);}
}
// Sum exp half values via4 K16 F16 MMAs, no intermediate H; no ordinary softmax.
WAVE void c32_fast_probabilities(const float*ex,float*out,uint windows){uint tile=bid()%16,window=bid()/16,q=(tile/4)*16,kout=(tile%4)*16;if(window>=windows)return;f8 sum{};
 for(uint kt=0;kt<4;kt++){h8 x{},ones{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;x[e]=(_Float16)ex[(window*64+q+rc())*64+k];ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,ones,sum);}
 for(uint e=0;e<8;e++){uint i=(window*64+q+gr()*8+e)*64+kout+rc();out[i]=F(ex[i]*(1.f/sum[e]));}
}
WAVE void c32_fast_av(const float*prob,const float*qkv,float*out,uint windows){uint first=bid()/2*16,col=bid()%2*16,tokens=windows*64;if(first>=tokens)return;uint window=first/64,q=first%64;f8 sum{};
 for(uint kt=0;kt<4;kt++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put(a,e,prob[(window*64+q+rc())*64+k]);put(b,e,qkv[(2*tokens+window*64+k)*32+col+rc()]);}sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,sum);}
 for(uint e=0;e<8;e++)out[(first+gr()*8+e)*32+col+rc()]=F(sum[e]);
}
WAVE void c32_fast_project(const float*av,const float*raw_input,const float*w,float*out,uint tokens,uint raw_output){uint first=bid()/2*16,col=bid()%2*16;if(first>=tokens)return;f8 sum{};
 for(uint kt=0;kt<2;kt++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put(a,e,av[(first+rc())*32+k]);put(b,e,w[3072+(col+rc())*32+k]);}sum=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,sum);}
 for(uint e=0;e<8;e++){uint i=(first+gr()*8+e)*32+col+rc();float prod=raw_input[i]*w[8193+col+rc()];float v=Hrtz(sum[e]+prod);out[i]=raw_output?v:F(v);}
}
