#ifndef HIP_MH_INPUT_DWORD
#define HIP_MH_INPUT_DWORD 1
#endif
// Production MH score/probability/AV fused; normalized f32 HWC input.
// All kernels flat grid ceil(work_items/256), block32; token count divisible64.
#pragma clang fp contract(off)
#pragma clang fp reassociate(off)
typedef unsigned uint;
#define DEV __attribute__((device)) __attribute__((always_inline))
#define KERNEL extern "C" __attribute__((global))
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
#ifndef HIP_BRANCHLESS_F
#define HIP_BRANCHLESS_F 1 /* 2026-09-17: with deep_fast and the padded file -0.12ms, bit-exact. F()/fp8() as clamp+cvt(+cndmask) instead of a divergent branch pair per call (same values for every non-NaN input; NaN gives -448 instead of +-448) */
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

using i2=int __attribute__((ext_vector_type(2)));
using f8=float __attribute__((ext_vector_type(8)));
using h8=_Float16 __attribute__((ext_vector_type(8)));
#define WAVE KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
DEV uint rc(){return __builtin_amdgcn_workitem_id_x()&15;}
DEV uint gr(){return (__builtin_amdgcn_workitem_id_x()&31)>>4;}
DEV uint bid(){return __builtin_amdgcn_workgroup_id_x();}
// D3D explicit f32tof16 maps to RTZ here; matrix Cast<F16> remains RNE.
#ifndef HIP_MH_VT
#ifndef HIP_MH_ABLATE
#define HIP_MH_ABLATE 0 /* diagnostic (c64 body only, breaks the hash): 1 skip scores+softmax, 2 skip AV, 3 skip projection */
#endif
#define HIP_MH_VT 0 /* 1: the window's V bytes are staged transposed ([col][key], 68-byte rows, in the same 2304-byte packed region) so every AV B fragment is one 8-byte LDS read instead of 8 byte gathers; same bytes */
#endif
#ifndef HIP_MH_RTZ_ISA
#define HIP_MH_RTZ_ISA 1 /* 2026-09-17: -0.02 alone, -0.16 with HIP_BRANCHLESS_Q8, bit-exact. Hrtz via v_cvt_pkrtz_f16_f32 (the C32/deep/padded files' production form) instead of the software bit path, which the ISA showed as a 4-deep nest of divergent branches per output element (32 instances per projection epilogue) */
#endif
#if HIP_MH_RTZ_ISA
DEV float Hrtz(float x){uint h;float r;asm("v_cvt_pkrtz_f16_f32 %0, %1, %1" : "=v"(h) : "v"(x));asm("v_cvt_f32_f16 %0, %1" : "=v"(r) : "v"(h));return r;}
#else
DEV float Hrtz(float x){uint b=bits(x),a=b&0x7fffffffu,sg=(b>>16)&0x8000u;int e=int(a>>23)-127;
 if(a>=0x7f800000u)return from_half(sg|0x7c00u|((a&0x7fffffu)?0x200u:0u));
 if(e>15)return from_half(sg|0x7bffu);
 if(e<-24)return from_half(sg);
 if(e<-14)return from_half(sg|(((a&0x7fffffu)|0x800000u)>>uint(-e-1)));
 return from_half(sg|((a>>13)-0x1c000u));
}
#endif
#if HIP_BRANCHLESS_F
DEV uint fp8(float f){return uint(__builtin_amdgcn_cvt_pk_fp8_f32(__builtin_fminf(__builtin_fmaxf(f,-448.f),448.f),0.f,0,false))&255u;}
#else
DEV uint fp8(float f){return uint(__builtin_amdgcn_cvt_pk_fp8_f32(clampf(f,-448.f,448.f),0.f,0,false))&255u;}
#endif
DEV void put(i2&v,uint e,float x){v[e/4]=int(uint(v[e/4])|(fp8(x)<<(8*(e%4))));}

DEV void sync_window(){WG_FENCE(3);__builtin_amdgcn_s_barrier();WG_FENCE(2);}

// Packed attention: shared raw/exp scratch 8448 bytes + normalized/prob/AV bytes 6912.
using ushort=unsigned short;
DEV void put_bits(i2&v,uint e,uint b){v[e/4]=int(uint(v[e/4])|((b&255u)<<(8*(e%4))));}
DEV uint raster(uint win,uint tok,uint width){return ((win/(width/8))*8+tok/8)*width+(win%(width/8))*8+tok%8;}
// Grid=(width/8)*(height/8)*(channels/32), block128. No QKV/norm/projection here.
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128)))
void mh_attention_fused(const float*normalized,const float*w,float*out,uint width,uint height,uint channels){
 uint heads=channels/32,windows=(width/8)*(height/8),wh=bid();
 if((channels!=64&&channels!=128&&channels!=256&&channels!=512)||width%8||height%8||wh>=windows*heads)return;
 uint win=wh/heads,head=wh%heads,tid=__builtin_amdgcn_workitem_id_x(),wave=tid/32,first=wave*16;
 __attribute__((shared)) _Float16 ex[64*66];
 __attribute__((shared)) unsigned char packed[3*64*36];
 // 6912-byte packed Q/K/V. No global intermediate writes.
 for(uint i=tid;i<3*64*32;i+=128){uint part=i/(64*32),tok=(i/32)%64,c=i%32,p=raster(win,tok,width);packed[(part*64+tok)*36+c]=static_cast<unsigned char>(fp8(normalized[(p*3+part)*channels+head*32+c]));}
 sync_window();
 for(uint key=0;key<64;key+=16){f8 acc{};
  for(uint kt=0;kt<2;kt++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,packed[(first+rc())*36+k]);put_bits(b,e,packed[(64+key+rc())*36+k]);}acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}
  for(uint e=0;e<8;e++){uint q=first+gr()*8+e,k=key+rc();float score=acc[e]+w[4*channels*channels+head*4096+q*64+k],affine=clampf(score*.044921875f+1.30078125f,1.03125f,1.5693359375f);uint ah=(bits(affine)>>13)-0x1c000u;ushort hb=static_cast<ushort>(((ah<<5)+0x8000u)&65535u);ex[q*66+k]=__builtin_bit_cast(_Float16,hb);}
 }
 sync_window();
 // MH production partial0 sums keys0..15,32..47; partial1 sums16..31,48..63.
 // Keep this distinct from C32 FAST4's sequential64-key accumulator.
 f8 sums[2];for(uint side=0;side<2;side++){f8 sum{};for(uint half=0;half<2;half++){h8 a{},ones{};for(uint e=0;e<8;e++){uint k=side*16+half*32+gr()*8+e;a[e]=ex[(first+rc())*66+k];ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,ones,sum);}sums[side]=sum;}
 // Q/K are dead; probability64x68 occupies4352 of their4608 bytes. V starts4608.
 for(uint key=0;key<64;key+=16)for(uint e=0;e<8;e++){uint row=first+gr()*8+e,k=key+rc();float inv=1.f/(sums[0][e]+sums[1][e]);packed[row*68+k]=static_cast<unsigned char>(fp8(float(ex[row*66+k])*inv));}
 sync_window();
 for(uint col=0;col<32;col+=16){f8 acc{};for(uint kt=0;kt<4;kt++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,packed[(first+rc())*68+k]);put_bits(b,e,packed[(128+k)*36+col+rc()]);}acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}for(uint e=0;e<8;e++){uint p=raster(win,first+gr()*8+e,width);out[p*channels+head*32+col+rc()]=F(acc[e]);}}
}

// Byte-input ABI paired with mh_qkv_normalize_fast_wave_fp8.
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128)))
void mh_attention_fused_fp8(const unsigned char*normalized,const float*w,float*out,uint width,uint height,uint channels){
 uint heads=channels/32,windows=(width/8)*(height/8),wh=bid();
 if((channels!=64&&channels!=128&&channels!=256&&channels!=512)||width%8||height%8||wh>=windows*heads)return;
 uint win=wh/heads,head=wh%heads,tid=__builtin_amdgcn_workitem_id_x(),wave=tid/32,first=wave*16;
 __attribute__((shared)) _Float16 ex[64*66];
 __attribute__((shared)) unsigned char packed[3*64*36];
 // 6912-byte packed Q/K/V. No global intermediate writes.
#if HIP_MH_INPUT_DWORD
 for(uint i=tid;i<3*64*8;i+=128){uint part=i/(64*8),tok=(i/8)%64,c=(i%8)*4,p=raster(win,tok,width),word;__builtin_memcpy(&word,normalized+(p*3+part)*channels+head*32+c,4);__builtin_memcpy(packed+(part*64+tok)*36+c,&word,4);}
#else
 for(uint i=tid;i<3*64*32;i+=128){uint part=i/(64*32),tok=(i/32)%64,c=i%32,p=raster(win,tok,width);packed[(part*64+tok)*36+c]=normalized[(p*3+part)*channels+head*32+c];}
#endif
 sync_window();
 for(uint key=0;key<64;key+=16){f8 acc{};
  for(uint kt=0;kt<2;kt++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,packed[(first+rc())*36+k]);put_bits(b,e,packed[(64+key+rc())*36+k]);}acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}
  for(uint e=0;e<8;e++){uint q=first+gr()*8+e,k=key+rc();float score=acc[e]+w[4*channels*channels+head*4096+q*64+k],affine=clampf(score*.044921875f+1.30078125f,1.03125f,1.5693359375f);uint ah=(bits(affine)>>13)-0x1c000u;ushort hb=static_cast<ushort>(((ah<<5)+0x8000u)&65535u);ex[q*66+k]=__builtin_bit_cast(_Float16,hb);}
 }
 sync_window();
 // MH production partial0 sums keys0..15,32..47; partial1 sums16..31,48..63.
 // Keep this distinct from C32 FAST4's sequential64-key accumulator.
 f8 sums[2];for(uint side=0;side<2;side++){f8 sum{};for(uint half=0;half<2;half++){h8 a{},ones{};for(uint e=0;e<8;e++){uint k=side*16+half*32+gr()*8+e;a[e]=ex[(first+rc())*66+k];ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,ones,sum);}sums[side]=sum;}
 // Q/K are dead; probability64x68 occupies4352 of their4608 bytes. V starts4608.
 for(uint key=0;key<64;key+=16)for(uint e=0;e<8;e++){uint row=first+gr()*8+e,k=key+rc();float inv=1.f/(sums[0][e]+sums[1][e]);packed[row*68+k]=static_cast<unsigned char>(fp8(float(ex[row*66+k])*inv));}
 sync_window();
 for(uint col=0;col<32;col+=16){f8 acc{};for(uint kt=0;kt<4;kt++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,packed[(first+rc())*68+k]);put_bits(b,e,packed[(128+k)*36+col+rc()]);}acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}for(uint e=0;e<8;e++){uint p=raster(win,first+gr()*8+e,width);out[p*channels+head*32+col+rc()]=F(acc[e]);}}
}

// Byte output paired with byte-input projection.
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128)))
void mh_attention_fused_fp8_out(const unsigned char*normalized,const float*w,unsigned char*out,uint width,uint height,uint channels){
 uint heads=channels/32,windows=(width/8)*(height/8),wh=bid();
 if((channels!=64&&channels!=128&&channels!=256&&channels!=512)||width%8||height%8||wh>=windows*heads)return;
 uint win=wh/heads,head=wh%heads,tid=__builtin_amdgcn_workitem_id_x(),wave=tid/32,first=wave*16;
 __attribute__((shared)) _Float16 ex[64*66];
 __attribute__((shared)) unsigned char packed[3*64*36];
 // 6912-byte packed Q/K/V. No global intermediate writes.
#if HIP_MH_INPUT_DWORD
 for(uint i=tid;i<3*64*8;i+=128){uint part=i/(64*8),tok=(i/8)%64,c=(i%8)*4,p=raster(win,tok,width),word;__builtin_memcpy(&word,normalized+(p*3+part)*channels+head*32+c,4);__builtin_memcpy(packed+(part*64+tok)*36+c,&word,4);}
#else
 for(uint i=tid;i<3*64*32;i+=128){uint part=i/(64*32),tok=(i/32)%64,c=i%32,p=raster(win,tok,width);packed[(part*64+tok)*36+c]=normalized[(p*3+part)*channels+head*32+c];}
#endif
 sync_window();
 for(uint key=0;key<64;key+=16){f8 acc{};
  for(uint kt=0;kt<2;kt++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,packed[(first+rc())*36+k]);put_bits(b,e,packed[(64+key+rc())*36+k]);}acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}
  for(uint e=0;e<8;e++){uint q=first+gr()*8+e,k=key+rc();float score=acc[e]+w[4*channels*channels+head*4096+q*64+k],affine=clampf(score*.044921875f+1.30078125f,1.03125f,1.5693359375f);uint ah=(bits(affine)>>13)-0x1c000u;ushort hb=static_cast<ushort>(((ah<<5)+0x8000u)&65535u);ex[q*66+k]=__builtin_bit_cast(_Float16,hb);}
 }
 sync_window();
 // MH production partial0 sums keys0..15,32..47; partial1 sums16..31,48..63.
 // Keep this distinct from C32 FAST4's sequential64-key accumulator.
 f8 sums[2];for(uint side=0;side<2;side++){f8 sum{};for(uint half=0;half<2;half++){h8 a{},ones{};for(uint e=0;e<8;e++){uint k=side*16+half*32+gr()*8+e;a[e]=ex[(first+rc())*66+k];ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,ones,sum);}sums[side]=sum;}
 // Q/K are dead; probability64x68 occupies4352 of their4608 bytes. V starts4608.
 for(uint key=0;key<64;key+=16)for(uint e=0;e<8;e++){uint row=first+gr()*8+e,k=key+rc();float inv=1.f/(sums[0][e]+sums[1][e]);packed[row*68+k]=static_cast<unsigned char>(fp8(float(ex[row*66+k])*inv));}
 sync_window();
 for(uint col=0;col<32;col+=16){f8 acc{};for(uint kt=0;kt<4;kt++){i2 a{},b{};for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,packed[(first+rc())*68+k]);put_bits(b,e,packed[(128+k)*36+col+rc()]);}acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}for(uint e=0;e<8;e++){uint p=raster(win,first+gr()*8+e,width);out[p*channels+head*32+col+rc()]=static_cast<unsigned char>(fp8(F(acc[e])));}}
}

// ByteFeature: the FFN feature arrives as E4M3 bytes (the f32 path re-encodes the f32 copy of the same byte, so the decoded
// value is identical). ByteOut: the block output F(...) (post 0/4, lattice-exact) is stored as its byte instead of f32;
// post 3 (Hrtz, chain tail) is never byte. Host option mh_byte_stream selects the variants per chain position.
template<bool ByteFeature,bool ByteOut,bool Diag=false>
DEV void c64_attention_project_body(const unsigned char*normalized,const float*w,const void*feature_any,void*out_any,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){
 // Reuse exact half exponent values; unroll fixed indices to avoid lane-selection expansion.
 h8 saved_ex[4];
 const float*feature=static_cast<const float*>(feature_any);const unsigned char*feature8=static_cast<const unsigned char*>(feature_any);float*out=static_cast<float*>(out_any);unsigned char*out8=static_cast<unsigned char*>(out_any);
 const uint channels=64;
 uint windows=(width/8)*(height/8),win=bid();if(width%8||height%8||win>=windows)return;
 uint alltid=__builtin_amdgcn_workitem_id_x(),head=alltid/128,tid=alltid%128,wave=tid/32,first=wave*16;
 __attribute__((shared)) _Float16 all_ex[2*64*66];
 __attribute__((shared)) unsigned char all_packed[2*64*36];
 _Float16*ex=all_ex+head*64*66;unsigned char*packed=all_packed+head*64*36;
 unsigned char*avbytes=reinterpret_cast<unsigned char*>(all_ex);
 for(uint i=tid;i<64*8;i+=128){uint tok=i/8,c=(i%8)*4,p=raster(win,tok,width),word;__builtin_memcpy(&word,normalized+(p*3+2)*channels+head*32+c,4);
#if HIP_MH_VT
  for(uint q=0;q<4;q++)packed[(c+q)*68+tok]=static_cast<unsigned char>((word>>(8*q))&255u);
#else
  __builtin_memcpy(packed+tok*36+c,&word,4);
#endif
 }
 sync_window();
#if HIP_MH_ABLATE==1
 for(uint key=0;key<64;key+=16)for(uint e=0;e<8;e++){ex[(first+gr()*8+e)*66+key+rc()]=(_Float16)1.f;saved_ex[key/16][e]=(_Float16)1.f;}
#else
 _Pragma("unroll 4") for(uint key=0;key<64;key+=16){f8 acc{};
  for(uint kt=0;kt<2;kt++){i2 a{},b{};uint qp=raster(win,first+rc(),width),kp=raster(win,key+rc(),width),off=kt*16+gr()*8;__builtin_memcpy(&a,normalized+qp*3*channels+head*32+off,8);__builtin_memcpy(&b,normalized+(kp*3+1)*channels+head*32+off,8);acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}
  _Pragma("unroll 8") for(uint e=0;e<8;e++){uint q=first+gr()*8+e,k=key+rc();float score=acc[e]+w[4*channels*channels+head*4096+q*64+k],affine=clampf(score*.044921875f+1.30078125f,1.03125f,1.5693359375f);uint ah=(bits(affine)>>13)-0x1c000u;ushort hb=static_cast<ushort>(((ah<<5)+0x8000u)&65535u);ex[q*66+k]=__builtin_bit_cast(_Float16,hb);saved_ex[key/16][e]=__builtin_bit_cast(_Float16,hb);}
 }
#endif
 sync_window();
 // MH production partial0 sums keys0..15,32..47; partial1 sums16..31,48..63.
 // Keep this distinct from C32 FAST4's sequential64-key accumulator.
 f8 sums[2];for(uint side=0;side<2;side++){f8 sum{};for(uint half=0;half<2;half++){h8 a{},ones{};for(uint e=0;e<8;e++){uint k=side*16+half*32+gr()*8+e;a[e]=ex[(first+rc())*66+k];ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,ones,sum);}sums[side]=sum;}
 uint pv[8]{};
 _Pragma("unroll 4") for(uint key=0;key<64;key+=16) _Pragma("unroll 8") for(uint e=0;e<8;e++){uint row=first+gr()*8+e,k=key+rc();float inv=1.f/(sums[0][e]+sums[1][e]);pv[e]|=fp8(float(saved_ex[key/16][e])*inv)<<(key/16*8);}
 sync_window();
 unsigned char*prob=reinterpret_cast<unsigned char*>(ex);
 #pragma unroll
 for(uint key=0;key<64;key+=16)for(uint e=0;e<8;e++)prob[(first+gr()*8+e)*68+key+rc()]=static_cast<unsigned char>(pv[e]>>(key/16*8));
 sync_window();
 f8 av[2]{};
#if HIP_MH_ABLATE==2
 for(uint col=0;col<32;col+=16)av[col/16]=f8{};
#else
 for(uint col=0;col<32;col+=16){f8 acc{};for(uint kt=0;kt<4;kt++){i2 a{},b{};
#if HIP_MH_VT
   __builtin_memcpy(&a,prob+(first+rc())*68+kt*16+gr()*8,8);__builtin_memcpy(&b,packed+(col+rc())*68+kt*16+gr()*8,8);
#else
   for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,prob[(first+rc())*68+k]);put_bits(b,e,packed[k*36+col+rc()]);}
#endif
   acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}av[col/16]=acc;}
#endif
 sync_window();
 for(uint col=0;col<32;col+=16)for(uint e=0;e<8;e++)avbytes[(first+gr()*8+e)*68+head*32+col+rc()]=static_cast<unsigned char>(fp8(F(av[col/16][e])));
 sync_window();
 f8 result[2]{};
 if constexpr(Diag){const unsigned char*diag=reinterpret_cast<const unsigned char*>(w)+4*(4*64*64+2*4096+2+64);uint p=raster(win,first+rc(),width);
  for(uint j=0;j<2;j++){uint ct=head*2+j;i2 a{};if constexpr(ByteFeature)__builtin_memcpy(&a,feature8+p*64+ct*16+gr()*8,8);else for(uint e=0;e<8;e++)put_bits(a,e,fp8(feature[p*64+ct*16+gr()*8+e]));
   for(uint part=0;part<3;part++){i2 b{};__builtin_memcpy(&b,diag+(part*4+ct)*512+(gr()*16+rc())*8,8);result[j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,result[j]);}}}
 else for(uint j=0;j<2;j++){uint col=head*32+j*16+rc();float remain=w[4*64*64+2*4096+2+col];for(uint part=0;part<3;part++){float component;if(absf(remain)<.015625f){float x=absf(remain)*512.f;uint q=uint(x);float r=x-float(q);q+=uint(r>.5f||(r==.5f&&(q&1)));q=q>7?7:q;component=(remain<0?-1.f:1.f)*float(q)/512.f;}else component=F(remain);remain-=component;for(uint e=0;e<8;e++){uint p=raster(win,first+gr()*8+e,width);float x=ByteFeature?__builtin_amdgcn_cvt_f32_fp8(int(feature8[p*64+col]),0):__builtin_amdgcn_cvt_f32_fp8(int(fp8(feature[p*64+col])),0);for(uint half=0;half<2;half++)result[j][e]=__builtin_fmaf(x,half==(col%32)/16?component:0.f,result[j][e]);}}}
 const uint*pw=reinterpret_cast<const uint*>(w);
#if HIP_MH_ABLATE!=3
 for(uint k=0;k<64;k+=16){i2 a{};__builtin_memcpy(&a,avbytes+(first+rc())*68+k+gr()*8,8);for(uint j=0;j<2;j++){uint col=head*32+j*16+rc(),off=3*64*64+(col*64+k+gr()*8)/4;i2 b={int(pw[off]),int(pw[off+1])};result[j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,result[j]);}}
#endif
 for(uint j=0;j<2;j++)for(uint e=0;e<8;e++){uint p=raster(win,first+gr()*8+e,width),col=head*32+j*16+rc(),dest=p*64+col;if(cropw){uint x=p%width,y=p/width;if(x<sx||y<sy||x>=sx+cropw||y>=sy+croph)continue;dest=((y-sy)*cropw+x-sx)*64+col;}float v=result[j][e],o=post==3?Hrtz(v):post==0?F(Hrtz(v)):F(v);if constexpr(ByteOut)out8[dest]=static_cast<unsigned char>(fp8(o));else out[dest]=o;}
}
// 16-wave window (host option mh_attn_w16, HLSL attention_direct shape): per head, four query tiles x two key sides; a
// wave scores its 16 queries against its side's two key tiles (keys side*16..+16 and side*16+32..+16, the same two
// partial-sum groups the 8-wave body forms), the two sides' row sums meet in LDS in the same p0+p1 order, and each wave
// then owns one 16-column half of the head's AV and of the projection. Same operands and accumulation order per output,
// so identical bits; every wave issues half the MMAs of the 8-wave body.
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void c64_attention_project_w16(const unsigned char*normalized,const float*w,const float*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){
 const uint channels=64;
 uint windows=(width/8)*(height/8),win=bid();if(width%8||height%8||win>=windows)return;
 uint alltid=__builtin_amdgcn_workitem_id_x(),head=alltid/256,t=alltid%256,qr=t/64,side=(t/32)&1,first=qr*16;
 __attribute__((shared)) _Float16 all_ex[2*64*66];
 __attribute__((shared)) unsigned char all_packed[2*64*36];
 __attribute__((shared)) float part_sum[2*2*64];
 _Float16*ex=all_ex+head*64*66;unsigned char*packed=all_packed+head*64*36;float*psum=part_sum+head*128;
 unsigned char*avbytes=reinterpret_cast<unsigned char*>(all_ex);
 for(uint i=t;i<64*8;i+=256){uint tok=i/8,c=(i%8)*4,p=raster(win,tok,width),word;__builtin_memcpy(&word,normalized+(p*3+2)*channels+head*32+c,4);
#if HIP_MH_VT
  for(uint q=0;q<4;q++)packed[(c+q)*68+tok]=static_cast<unsigned char>((word>>(8*q))&255u);
#else
  __builtin_memcpy(packed+tok*36+c,&word,4);
#endif
 }
 sync_window();
 for(uint j=0;j<2;j++){uint key=(side+j*2)*16;f8 acc{};
  for(uint kt=0;kt<2;kt++){i2 a{},b{};uint qp=raster(win,first+rc(),width),kp=raster(win,key+rc(),width),off=kt*16+gr()*8;__builtin_memcpy(&a,normalized+qp*3*channels+head*32+off,8);__builtin_memcpy(&b,normalized+(kp*3+1)*channels+head*32+off,8);acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}
  for(uint e=0;e<8;e++){uint q=first+gr()*8+e,k=key+rc();float score=acc[e]+w[4*channels*channels+head*4096+q*64+k],affine=clampf(score*.044921875f+1.30078125f,1.03125f,1.5693359375f);uint ah=(bits(affine)>>13)-0x1c000u;ushort hb=static_cast<ushort>(((ah<<5)+0x8000u)&65535u);ex[q*66+k]=__builtin_bit_cast(_Float16,hb);}
 }
 sync_window();
 {f8 sum{};for(uint half=0;half<2;half++){h8 a{},ones{};for(uint e=0;e<8;e++){uint k=side*16+half*32+gr()*8+e;a[e]=ex[(first+rc())*66+k];ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,ones,sum);}
  for(uint e=0;e<8;e++)psum[side*64+first+gr()*8+e]=sum[e];}
 sync_window();
 uint pv[8]{};
 #pragma unroll
 for(uint j=0;j<2;j++){uint key=(side+j*2)*16;for(uint e=0;e<8;e++){uint row=first+gr()*8+e,k=key+rc();float inv=1.f/(psum[row]+psum[64+row]);pv[e]|=fp8(float(ex[row*66+k])*inv)<<(j*8);}}
 sync_window();
 unsigned char*prob=reinterpret_cast<unsigned char*>(ex);
 #pragma unroll
 for(uint j=0;j<2;j++){uint key=(side+j*2)*16;for(uint e=0;e<8;e++)prob[(first+gr()*8+e)*68+key+rc()]=static_cast<unsigned char>(pv[e]>>(j*8));}
 sync_window();
 f8 av{};{uint col=side*16;for(uint kt=0;kt<4;kt++){i2 a{},b{};
#if HIP_MH_VT
  __builtin_memcpy(&a,prob+(first+rc())*68+kt*16+gr()*8,8);__builtin_memcpy(&b,packed+(col+rc())*68+kt*16+gr()*8,8);
#else
  for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,prob[(first+rc())*68+k]);put_bits(b,e,packed[k*36+col+rc()]);}
#endif
  av=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,av);}}
 sync_window();
 for(uint e=0;e<8;e++)avbytes[(first+gr()*8+e)*68+head*32+side*16+rc()]=static_cast<unsigned char>(fp8(F(av[e])));
 sync_window();
 f8 result{};
 {uint col=head*32+side*16+rc();float remain=w[4*64*64+2*4096+2+col];for(uint part=0;part<3;part++){float component;if(absf(remain)<.015625f){float x=absf(remain)*512.f;uint q=uint(x);float r=x-float(q);q+=uint(r>.5f||(r==.5f&&(q&1)));q=q>7?7:q;component=(remain<0?-1.f:1.f)*float(q)/512.f;}else component=F(remain);remain-=component;for(uint e=0;e<8;e++){uint p=raster(win,first+gr()*8+e,width);float x=__builtin_amdgcn_cvt_f32_fp8(int(fp8(feature[p*64+col])),0);for(uint half=0;half<2;half++)result[e]=__builtin_fmaf(x,half==(col%32)/16?component:0.f,result[e]);}}}
 const uint*pw=reinterpret_cast<const uint*>(w);
 for(uint k=0;k<64;k+=16){i2 a{};__builtin_memcpy(&a,avbytes+(first+rc())*68+k+gr()*8,8);uint col=head*32+side*16+rc(),off=3*64*64+(col*64+k+gr()*8)/4;i2 b={int(pw[off]),int(pw[off+1])};result=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,result);}
 for(uint e=0;e<8;e++){uint p=raster(win,first+gr()*8+e,width),col=head*32+side*16+rc(),dest=p*64+col;if(cropw){uint x=p%width,y=p/width;if(x<sx||y<sy||x>=sx+cropw||y>=sy+croph)continue;dest=((y-sy)*cropw+x-sx)*64+col;}float v=result[e],o=post==3?Hrtz(v):post==0?F(Hrtz(v)):F(v);out[dest]=o;}
}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void c64_attention_project(const unsigned char*normalized,const float*w,const float*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c64_attention_project_body<false,false>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
// Residual via prepacked diagonal fragment tiles (host option mh_proj_diag, PackedMhWeightDiag): three MMAs replace the scalar three-piece FMAs.
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void c64_attention_project_diag(const unsigned char*normalized,const float*w,const float*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c64_attention_project_body<false,false,true>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void c64_attention_project_fb(const unsigned char*normalized,const float*w,const unsigned char*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c64_attention_project_body<true,false>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
/* 2026-09-17: byte-feature twin with the residual scales as diagonal fragment B tiles (host option mh_proj_diag_fb): the scalar residual path compiled to per-element rounding branches and v_movrel register indexing. */
void c64_attention_project_fb_diag(const unsigned char*normalized,const float*w,const unsigned char*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c64_attention_project_body<true,false,true>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void c64_attention_project_fb_bout(const unsigned char*normalized,const float*w,const unsigned char*feature,unsigned char*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c64_attention_project_body<true,true>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}

template<bool ByteFeature,bool ByteOut,bool Diag=false>
DEV void c128_attention_project_body(const unsigned char*normalized,const float*w,const void*feature_any,void*out_any,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){
 // Reuse exact half exponent values; unroll fixed indices to avoid lane-selection expansion.
 h8 saved_ex[4];
 const float*feature=static_cast<const float*>(feature_any);const unsigned char*feature8=static_cast<const unsigned char*>(feature_any);float*out=static_cast<float*>(out_any);unsigned char*out8=static_cast<unsigned char*>(out_any);
 const uint channels=128;
 uint windows=(width/8)*(height/8),win=bid();if(width%8||height%8||win>=windows)return;
 uint alltid=__builtin_amdgcn_workitem_id_x(),head=alltid/128,tid=alltid%128,wave=tid/32,first=wave*16;
 __attribute__((shared)) _Float16 all_ex[4*64*66];
 __attribute__((shared)) unsigned char all_packed[4*64*36];
 _Float16*ex=all_ex+head*64*66;unsigned char*packed=all_packed+head*64*36;
 unsigned char*avbytes=reinterpret_cast<unsigned char*>(all_ex);
 for(uint i=tid;i<64*8;i+=128){uint tok=i/8,c=(i%8)*4,p=raster(win,tok,width),word;__builtin_memcpy(&word,normalized+(p*3+2)*channels+head*32+c,4);
#if HIP_MH_VT
  for(uint q=0;q<4;q++)packed[(c+q)*68+tok]=static_cast<unsigned char>((word>>(8*q))&255u);
#else
  __builtin_memcpy(packed+tok*36+c,&word,4);
#endif
 }
 sync_window();
 _Pragma("unroll 4") for(uint key=0;key<64;key+=16){f8 acc{};
  for(uint kt=0;kt<2;kt++){i2 a{},b{};uint qp=raster(win,first+rc(),width),kp=raster(win,key+rc(),width),off=kt*16+gr()*8;__builtin_memcpy(&a,normalized+qp*3*channels+head*32+off,8);__builtin_memcpy(&b,normalized+(kp*3+1)*channels+head*32+off,8);acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}
  _Pragma("unroll 8") for(uint e=0;e<8;e++){uint q=first+gr()*8+e,k=key+rc();float score=acc[e]+w[4*channels*channels+head*4096+q*64+k],affine=clampf(score*.044921875f+1.30078125f,1.03125f,1.5693359375f);uint ah=(bits(affine)>>13)-0x1c000u;ushort hb=static_cast<ushort>(((ah<<5)+0x8000u)&65535u);ex[q*66+k]=__builtin_bit_cast(_Float16,hb);saved_ex[key/16][e]=__builtin_bit_cast(_Float16,hb);}
 }
 sync_window();
 // MH production partial0 sums keys0..15,32..47; partial1 sums16..31,48..63.
 // Keep this distinct from C32 FAST4's sequential64-key accumulator.
 f8 sums[2];for(uint side=0;side<2;side++){f8 sum{};for(uint half=0;half<2;half++){h8 a{},ones{};for(uint e=0;e<8;e++){uint k=side*16+half*32+gr()*8+e;a[e]=ex[(first+rc())*66+k];ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,ones,sum);}sums[side]=sum;}
 uint pv[8]{};
 _Pragma("unroll 4") for(uint key=0;key<64;key+=16) _Pragma("unroll 8") for(uint e=0;e<8;e++){uint row=first+gr()*8+e,k=key+rc();float inv=1.f/(sums[0][e]+sums[1][e]);pv[e]|=fp8(float(saved_ex[key/16][e])*inv)<<(key/16*8);}
 sync_window();
 unsigned char*prob=reinterpret_cast<unsigned char*>(ex);
 #pragma unroll
 for(uint key=0;key<64;key+=16)for(uint e=0;e<8;e++)prob[(first+gr()*8+e)*68+key+rc()]=static_cast<unsigned char>(pv[e]>>(key/16*8));
 sync_window();
 f8 av[2]{};
 for(uint col=0;col<32;col+=16){f8 acc{};for(uint kt=0;kt<4;kt++){i2 a{},b{};
#if HIP_MH_VT
   __builtin_memcpy(&a,prob+(first+rc())*68+kt*16+gr()*8,8);__builtin_memcpy(&b,packed+(col+rc())*68+kt*16+gr()*8,8);
#else
   for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,prob[(first+rc())*68+k]);put_bits(b,e,packed[k*36+col+rc()]);}
#endif
   acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}av[col/16]=acc;}
 sync_window();
 for(uint col=0;col<32;col+=16)for(uint e=0;e<8;e++)avbytes[(first+gr()*8+e)*132+head*32+col+rc()]=static_cast<unsigned char>(fp8(F(av[col/16][e])));
 sync_window();
 f8 result[2]{};
 if constexpr(Diag){const unsigned char*diag=reinterpret_cast<const unsigned char*>(w)+4*(4*128*128+4*4096+4+128);uint p=raster(win,first+rc(),width);
  for(uint j=0;j<2;j++){uint ct=head*2+j;i2 a{};if constexpr(ByteFeature)__builtin_memcpy(&a,feature8+p*128+ct*16+gr()*8,8);else for(uint e=0;e<8;e++)put_bits(a,e,fp8(feature[p*128+ct*16+gr()*8+e]));
   for(uint part=0;part<3;part++){i2 b{};__builtin_memcpy(&b,diag+(part*8+ct)*512+(gr()*16+rc())*8,8);result[j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,result[j]);}}}
 else for(uint j=0;j<2;j++){uint col=head*32+j*16+rc();float remain=w[4*128*128+4*4096+4+col];for(uint part=0;part<3;part++){float component;if(absf(remain)<.015625f){float x=absf(remain)*512.f;uint q=uint(x);float r=x-float(q);q+=uint(r>.5f||(r==.5f&&(q&1)));q=q>7?7:q;component=(remain<0?-1.f:1.f)*float(q)/512.f;}else component=F(remain);remain-=component;for(uint e=0;e<8;e++){uint p=raster(win,first+gr()*8+e,width);float x=ByteFeature?__builtin_amdgcn_cvt_f32_fp8(int(feature8[p*128+col]),0):__builtin_amdgcn_cvt_f32_fp8(int(fp8(feature[p*128+col])),0);for(uint half=0;half<2;half++)result[j][e]=__builtin_fmaf(x,half==(col%32)/16?component:0.f,result[j][e]);}}}
 const uint*pw=reinterpret_cast<const uint*>(w);
 for(uint k=0;k<128;k+=16){i2 a{};__builtin_memcpy(&a,avbytes+(first+rc())*132+k+gr()*8,8);for(uint j=0;j<2;j++){uint col=head*32+j*16+rc(),off=3*128*128+(col*128+k+gr()*8)/4;i2 b={int(pw[off]),int(pw[off+1])};result[j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,result[j]);}}
 for(uint j=0;j<2;j++)for(uint e=0;e<8;e++){uint p=raster(win,first+gr()*8+e,width),col=head*32+j*16+rc(),dest=p*128+col;if(cropw){uint x=p%width,y=p/width;if(x<sx||y<sy||x>=sx+cropw||y>=sy+croph)continue;dest=((y-sy)*cropw+x-sx)*128+col;}float v=result[j][e],o=post==3?Hrtz(v):post==0?F(Hrtz(v)):F(v);if constexpr(ByteOut)out8[dest]=static_cast<unsigned char>(fp8(o));else out[dest]=o;}
}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void c128_attention_project(const unsigned char*normalized,const float*w,const float*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c128_attention_project_body<false,false>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
// Residual via prepacked diagonal fragment tiles (host option mh_proj_diag, PackedMhWeightDiag): three MMAs replace the scalar three-piece FMAs.
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void c128_attention_project_diag(const unsigned char*normalized,const float*w,const float*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c128_attention_project_body<false,false,true>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void c128_attention_project_fb(const unsigned char*normalized,const float*w,const unsigned char*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c128_attention_project_body<true,false>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
/* 2026-09-17: byte-feature twin with the residual scales as diagonal fragment B tiles (host option mh_proj_diag_fb): the scalar residual path compiled to per-element rounding branches and v_movrel register indexing. */
void c128_attention_project_fb_diag(const unsigned char*normalized,const float*w,const unsigned char*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c128_attention_project_body<true,false,true>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void c128_attention_project_fb_bout(const unsigned char*normalized,const float*w,const unsigned char*feature,unsigned char*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c128_attention_project_body<true,true>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}

template<bool ByteFeature,bool ByteOut,bool Diag=false>
DEV void c256_attention_project_body(const unsigned char*normalized,const float*w,const void*feature_any,void*out_any,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){
 // Reuse exact half exponent values; unroll fixed indices to avoid lane-selection expansion.
 h8 saved_ex[4];
 const float*feature=static_cast<const float*>(feature_any);const unsigned char*feature8=static_cast<const unsigned char*>(feature_any);float*out=static_cast<float*>(out_any);unsigned char*out8=static_cast<unsigned char*>(out_any);
 const uint channels=256;
 uint windows=(width/8)*(height/8),win=bid();if(width%8||height%8||win>=windows)return;
 uint alltid=__builtin_amdgcn_workitem_id_x(),localhead=alltid/128,tid=alltid%128,wave=tid/32,first=wave*16;
 __attribute__((shared)) _Float16 all_ex[4*64*66];
 __attribute__((shared)) unsigned char all_packed[4*64*36];
 _Float16*ex=all_ex+localhead*64*66;unsigned char*packed=all_packed+localhead*64*36;
 __attribute__((shared)) unsigned char avbytes[64*260];
 for(uint batch=0;batch<2;batch++){uint head=localhead+batch*4;
 for(uint i=tid;i<64*8;i+=128){uint tok=i/8,c=(i%8)*4,p=raster(win,tok,width),word;__builtin_memcpy(&word,normalized+(p*3+2)*channels+head*32+c,4);
#if HIP_MH_VT
  for(uint q=0;q<4;q++)packed[(c+q)*68+tok]=static_cast<unsigned char>((word>>(8*q))&255u);
#else
  __builtin_memcpy(packed+tok*36+c,&word,4);
#endif
 }
 sync_window();
 _Pragma("unroll 4") for(uint key=0;key<64;key+=16){f8 acc{};
  for(uint kt=0;kt<2;kt++){i2 a{},b{};uint qp=raster(win,first+rc(),width),kp=raster(win,key+rc(),width),off=kt*16+gr()*8;__builtin_memcpy(&a,normalized+qp*3*channels+head*32+off,8);__builtin_memcpy(&b,normalized+(kp*3+1)*channels+head*32+off,8);acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}
  _Pragma("unroll 8") for(uint e=0;e<8;e++){uint q=first+gr()*8+e,k=key+rc();float score=acc[e]+w[4*channels*channels+head*4096+q*64+k],affine=clampf(score*.044921875f+1.30078125f,1.03125f,1.5693359375f);uint ah=(bits(affine)>>13)-0x1c000u;ushort hb=static_cast<ushort>(((ah<<5)+0x8000u)&65535u);ex[q*66+k]=__builtin_bit_cast(_Float16,hb);saved_ex[key/16][e]=__builtin_bit_cast(_Float16,hb);}
 }
 sync_window();
 // MH production partial0 sums keys0..15,32..47; partial1 sums16..31,48..63.
 // Keep this distinct from C32 FAST4's sequential64-key accumulator.
 f8 sums[2];for(uint side=0;side<2;side++){f8 sum{};for(uint half=0;half<2;half++){h8 a{},ones{};for(uint e=0;e<8;e++){uint k=side*16+half*32+gr()*8+e;a[e]=ex[(first+rc())*66+k];ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,ones,sum);}sums[side]=sum;}
 uint pv[8]{};
 _Pragma("unroll 4") for(uint key=0;key<64;key+=16) _Pragma("unroll 8") for(uint e=0;e<8;e++){uint row=first+gr()*8+e,k=key+rc();float inv=1.f/(sums[0][e]+sums[1][e]);pv[e]|=fp8(float(saved_ex[key/16][e])*inv)<<(key/16*8);}
 sync_window();
 unsigned char*prob=reinterpret_cast<unsigned char*>(ex);
 #pragma unroll
 for(uint key=0;key<64;key+=16)for(uint e=0;e<8;e++)prob[(first+gr()*8+e)*68+key+rc()]=static_cast<unsigned char>(pv[e]>>(key/16*8));
 sync_window();
 f8 av[2]{};
 for(uint col=0;col<32;col+=16){f8 acc{};for(uint kt=0;kt<4;kt++){i2 a{},b{};
#if HIP_MH_VT
   __builtin_memcpy(&a,prob+(first+rc())*68+kt*16+gr()*8,8);__builtin_memcpy(&b,packed+(col+rc())*68+kt*16+gr()*8,8);
#else
   for(uint e=0;e<8;e++){uint k=kt*16+gr()*8+e;put_bits(a,e,prob[(first+rc())*68+k]);put_bits(b,e,packed[k*36+col+rc()]);}
#endif
   acc=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc);}av[col/16]=acc;}
 sync_window();
 for(uint col=0;col<32;col+=16)for(uint e=0;e<8;e++)avbytes[(first+gr()*8+e)*260+head*32+col+rc()]=static_cast<unsigned char>(fp8(F(av[col/16][e])));
 sync_window();
 } // Complete both batches before projection.
 for(uint pair=0;pair<2;pair++){
 f8 result[2]{};
 if constexpr(Diag){const unsigned char*diag=reinterpret_cast<const unsigned char*>(w)+4*(4*256*256+8*4096+8+256);uint p=raster(win,first+rc(),width);
  for(uint j=0;j<2;j++){uint ct=localhead*4+pair*2+j;i2 a{};if constexpr(ByteFeature)__builtin_memcpy(&a,feature8+p*256+ct*16+gr()*8,8);else for(uint e=0;e<8;e++)put_bits(a,e,fp8(feature[p*256+ct*16+gr()*8+e]));
   for(uint part=0;part<3;part++){i2 b{};__builtin_memcpy(&b,diag+(part*16+ct)*512+(gr()*16+rc())*8,8);result[j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,result[j]);}}}
 else for(uint j=0;j<2;j++){uint col=localhead*64+pair*32+j*16+rc();float remain=w[4*256*256+8*4096+8+col];for(uint part=0;part<3;part++){float component;if(absf(remain)<.015625f){float x=absf(remain)*512.f;uint q=uint(x);float r=x-float(q);q+=uint(r>.5f||(r==.5f&&(q&1)));q=q>7?7:q;component=(remain<0?-1.f:1.f)*float(q)/512.f;}else component=F(remain);remain-=component;for(uint e=0;e<8;e++){uint p=raster(win,first+gr()*8+e,width);float x=ByteFeature?__builtin_amdgcn_cvt_f32_fp8(int(feature8[p*256+col]),0):__builtin_amdgcn_cvt_f32_fp8(int(fp8(feature[p*256+col])),0);for(uint half=0;half<2;half++)result[j][e]=__builtin_fmaf(x,half==(col%32)/16?component:0.f,result[j][e]);}}}
 const uint*pw=reinterpret_cast<const uint*>(w);
 for(uint k=0;k<256;k+=16){i2 a{};__builtin_memcpy(&a,avbytes+(first+rc())*260+k+gr()*8,8);for(uint j=0;j<2;j++){uint col=localhead*64+pair*32+j*16+rc(),off=3*256*256+(col*256+k+gr()*8)/4;i2 b={int(pw[off]),int(pw[off+1])};result[j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,result[j]);}}
 for(uint j=0;j<2;j++)for(uint e=0;e<8;e++){uint p=raster(win,first+gr()*8+e,width),col=localhead*64+pair*32+j*16+rc(),dest=p*256+col;if(cropw){uint x=p%width,y=p/width;if(x<sx||y<sy||x>=sx+cropw||y>=sy+croph)continue;dest=((y-sy)*cropw+x-sx)*256+col;}float v=result[j][e],o=post==3?Hrtz(v):post==0?F(Hrtz(v)):F(v);if constexpr(ByteOut)out8[dest]=static_cast<unsigned char>(fp8(o));else out[dest]=o;}

 }
}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void c256_attention_project(const unsigned char*normalized,const float*w,const float*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c256_attention_project_body<false,false>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
// Residual via prepacked diagonal fragment tiles (host option mh_proj_diag, PackedMhWeightDiag): three MMAs replace the scalar three-piece FMAs.
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void c256_attention_project_diag(const unsigned char*normalized,const float*w,const float*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c256_attention_project_body<false,false,true>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void c256_attention_project_fb(const unsigned char*normalized,const float*w,const unsigned char*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c256_attention_project_body<true,false>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
/* 2026-09-17: byte-feature twin with the residual scales as diagonal fragment B tiles (host option mh_proj_diag_fb): the scalar residual path compiled to per-element rounding branches and v_movrel register indexing. */
void c256_attention_project_fb_diag(const unsigned char*normalized,const float*w,const unsigned char*feature,float*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c256_attention_project_body<true,false,true>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void c256_attention_project_fb_bout(const unsigned char*normalized,const float*w,const unsigned char*feature,unsigned char*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c256_attention_project_body<true,true>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}

KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void c64_attention_project_fb_bout_diag(const unsigned char*normalized,const float*w,const unsigned char*feature,unsigned char*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c64_attention_project_body<true,true,true>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}

KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void c128_attention_project_fb_bout_diag(const unsigned char*normalized,const float*w,const unsigned char*feature,unsigned char*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c128_attention_project_body<true,true,true>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}

KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void c256_attention_project_fb_bout_diag(const unsigned char*normalized,const float*w,const unsigned char*feature,unsigned char*out,uint width,uint height,uint post,uint cropw,uint croph,uint sx,uint sy){c256_attention_project_body<true,true,true>(normalized,w,feature,out,width,height,post,cropw,croph,sx,sy);}
