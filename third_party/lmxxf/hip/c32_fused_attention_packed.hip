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
DEV float F(float x){uint b=bits(x),a=b&0x7fffffffu,s=b&0x80000000u;if(a==0)return 0.f;
 if(a>=0x43e00000u)return value(s|0x43e00000u); // finite E4M3 saturation, including overflow
 if(a<0x3c800000u){float scaled=absf(x)*512.f;uint q=uint(scaled);float r=scaled-float(q);q+=uint(r>.5f||(r==.5f&&(q&1u)));float f=float(q)/512.f;return s?-f:f;}
 uint r=(a+0x7ffffu+((a>>20)&1u))&0xfff00000u;return value(s|(r>0x43e00000u?0x43e00000u:r));}

using i2=int __attribute__((ext_vector_type(2)));
using f8=float __attribute__((ext_vector_type(8)));
using h8=_Float16 __attribute__((ext_vector_type(8)));
#define WAVE KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
DEV uint rc(){return __builtin_amdgcn_workitem_id_x()&15;}
DEV uint gr(){return (__builtin_amdgcn_workitem_id_x()&31)>>4;}
DEV uint bid(){return __builtin_amdgcn_workgroup_id_x();}
// D3D explicit f32tof16 maps to RTZ here; matrix Cast<F16> remains RNE.
DEV float Hrtz(float x){uint b=bits(x),a=b&0x7fffffffu,sg=(b>>16)&0x8000u;int e=int(a>>23)-127;
 if(a>=0x7f800000u)return from_half(sg|0x7c00u|((a&0x7fffffu)?0x200u:0u));
 if(e>15)return from_half(sg|0x7bffu);
 if(e<-24)return from_half(sg);
 if(e<-14)return from_half(sg|(((a&0x7fffffu)|0x800000u)>>uint(-e-1)));
 return from_half(sg|((a>>13)-0x1c000u));
}
DEV uint fp8(float f){return uint(__builtin_amdgcn_cvt_pk_fp8_f32(clampf(f,-448.f,448.f),0.f,0,false))&255u;}
DEV void put(i2&v,uint e,float x){v[e/4]=int(uint(v[e/4])|(fp8(x)<<(8*(e%4))));}

DEV void sync_window(){__builtin_amdgcn_fence(3,"workgroup");__builtin_amdgcn_s_barrier();__builtin_amdgcn_fence(2,"workgroup");}

// Packed attention: shared raw/exp scratch 8448 bytes + normalized/prob/AV bytes 6912.
using ushort=unsigned short;
DEV void put_bits(i2&v,uint e,uint b){v[e/4]=int(uint(v[e/4])|((b&255u)<<(8*(e%4))));}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128)))
void c32_fast_attention_fused(const float*raw_input,const float*w,float*out,uint windows,uint raw_output){
 uint window=bid();if(window>=windows)return;uint tid=__builtin_amdgcn_workitem_id_x(),wave=tid/32,first=wave*16,base=window*64;
 union Scratch {float raw[64*33];_Float16 ex[64*66];};
 __attribute__((shared)) Scratch scratch;
 __attribute__((shared)) unsigned char packed[3*64*36];
 // A single raw QKV plane is reused after its norm has been packed separately.
 for(uint part=0;part<3;part++){
  for(uint col=0;col<32;col+=16){f8 acc{};
   for(uint kt=0;kt<2;kt++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put(a,e,raw_input[(base+first+rc())*32+k]);put(b,e,w[part*1024+(col+rc())*32+k]);}acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}
   for(uint e=0;e<8;e++)scratch.raw[(first+gr()*8+e)*33+col+rc()]=acc[e];
  }
  sync_window();f8 sum{};
  if(part<2)for(uint kt=0;kt<2;kt++){h8 a{},ones{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;float v=scratch.raw[(first+rc())*33+k];a[e]=(_Float16)(v*v);ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,ones,sum);}
  for(uint col=0;col<32;col+=16)for(uint e=0;e<8;e++){uint row=first+gr()*8+e;float v=scratch.raw[row*33+col+rc()];if(part<2)v*=__builtin_amdgcn_rsqf(maxf(sum[e],6.198883056640625e-5f))*(part==0?w[8192]:1.f);packed[(part*64+row)*36+col+rc()]=static_cast<unsigned char>(fp8(v));}
  sync_window();
 }
 // Q/K packed rows: 36-byte stride. Exponent values stored exactly as half bits.
 for(uint key=0;key<64;key+=16){f8 acc{};
  for(uint kt=0;kt<2;kt++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,packed[(first+rc())*36+k]);put_bits(b,e,packed[(64+key+rc())*36+k]);}acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}
  for(uint e=0;e<8;e++){uint q=first+gr()*8+e,k=key+rc();float score=acc[e]+w[4096+q*64+k],affine=clampf(score*.044921875f+1.30078125f,1.03125f,1.5693359375f);uint ah=(bits(affine)>>13)-0x1c000u;ushort hb=static_cast<ushort>(((ah<<5)+0x8000u)&65535u);scratch.ex[q*66+k]=__builtin_bit_cast(_Float16,hb);}
 }
 sync_window();
 // Scores no longer consume Q/K; their 4608 bytes become 64 probability rows of 68 bytes.
 {f8 sum{};for(uint kt=0;kt<4;kt++){h8 a{},ones{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;a[e]=scratch.ex[(first+rc())*66+k];ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,ones,sum);}
  for(uint key=0;key<64;key+=16)for(uint e=0;e<8;e++){uint row=first+gr()*8+e,k=key+rc();float v=float(scratch.ex[row*66+k])*(1.f/sum[e]);packed[row*68+k]=static_cast<unsigned char>(fp8(v));}
 }
 sync_window();
 // Both AV column fragments stay live until every wave has finished reading probability rows.
 f8 av[2];
 for(uint ci=0;ci<2;ci++){f8 acc{};uint col=ci*16;
  for(uint kt=0;kt<4;kt++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,packed[(first+rc())*68+k]);put_bits(b,e,packed[(128+k)*36+col+rc()]);}acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}av[ci]=acc;
 }
 sync_window();
 for(uint ci=0;ci<2;ci++)for(uint e=0;e<8;e++)packed[(first+gr()*8+e)*36+ci*16+rc()]=static_cast<unsigned char>(fp8(av[ci][e]));
 sync_window();
 for(uint col=0;col<32;col+=16){f8 acc{};
  for(uint kt=0;kt<2;kt++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,packed[(first+rc())*36+k]);put(b,e,w[3072+(col+rc())*32+k]);}acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}
  for(uint e=0;e<8;e++){uint i=(base+first+gr()*8+e)*32+col+rc();float prod=raw_input[i]*w[8193+col+rc()],v=Hrtz(acc[e]+prod);out[i]=raw_output?v:F(v);}
 }
}
