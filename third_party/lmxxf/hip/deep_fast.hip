// Production ViT matrix math. FP8 MLP, F16 QKV, raw FP32 accumulation.
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
#ifndef HIP_NATIVE_FP8_F
#define HIP_NATIVE_FP8_F 1
#endif
#ifndef HIP_DEC_HOIST_SCALE
#define HIP_DEC_HOIST_SCALE 0 /* 1: decoder_project2x_h16w skip scale read once per lane */
#endif
#ifndef HIP_VIT_HOIST_SCALE
#define HIP_VIT_HOIST_SCALE 0 /* 1: vit_project_frag residual scale and the QKV frag kernel's row scale read once per lane instead of per output element */
#endif
#ifndef HIP_BRANCHLESS_F
#define HIP_BRANCHLESS_F 1 /* 2026-09-17: with the two multihead files -0.12ms, bit-exact. F() as clamp+cvt+cndmask instead of a divergent branch pair per call (same values for every non-NaN input; NaN gives -448 instead of +-448) */
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

using f8=float __attribute__((ext_vector_type(8)));
using h8=_Float16 __attribute__((ext_vector_type(8)));
using i2=int __attribute__((ext_vector_type(2)));
#define WAVE KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
DEV uint rc(){return __builtin_amdgcn_workitem_id_x()&15u;}
DEV uint gr(){return (__builtin_amdgcn_workitem_id_x()>>4)&1u;}
DEV uint bid(){return __builtin_amdgcn_workgroup_id_x();}
DEV void pack(i2&x,uint e,float v){uint b=uint(__builtin_amdgcn_cvt_pk_fp8_f32(clampf(v,-448.f,448.f),0.f,0,false))&255;x[e/4]=int(uint(x[e/4])|(b<<(8*(e%4))));}
template<bool ByteInput=false>
DEV f8 dot8(const float*in,const float*w,uint first,uint row,uint channels,uint begin,uint end,f8 a){
 for(uint k=begin;k<end;k+=16){i2 x{},y{};
  for(uint e=0;e<8;e++){uint j=k+gr()*8+e;if constexpr(!ByteInput)pack(x,e,in[(first+rc())*channels+j]);
#ifndef HIP_PREPACKED_WEIGHTS
   pack(y,e,w[(row+rc())*channels+j]);
#endif
  }
if constexpr(ByteInput){const uint*pi=reinterpret_cast<const uint*>(in);uint off=((first+rc())*channels+k+gr()*8)/4;x={int(pi[off]),int(pi[off+1])};}
#ifdef HIP_PREPACKED_WEIGHTS
  const uint*pw=reinterpret_cast<const uint*>(w);uint offset=((row+rc())*channels+k+gr()*8)/4;y={int(pw[offset]),int(pw[offset+1])};
#endif
  a=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(x,y,a);
 }return a;
}
#ifdef HIP_VIT_EXPAND_PAIR
// Two adjacent output tiles reuse each input load/FP8 pack. Keep the legacy
// launch ABI: odd column tiles return; even tiles own both output regions.
// Each accumulator retains the original increasing-K WMMA order.
WAVE void vit_expand(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){
 uint tile=bid()%(outputs/16),first=bid()/(outputs/16)*16,row=tile*16;
 if(first>=tokens||(tile&1u))return;
 bool second=row+16<outputs;f8 a{},b{};
 for(uint k=0;k<inputs;k+=16){
  i2 x{},y{},z{};
  for(uint e=0;e<8;e++){
   uint j=k+gr()*8+e;pack(x,e,in[(first+rc())*inputs+j]);
#ifndef HIP_PREPACKED_WEIGHTS
   pack(y,e,w[(row+rc())*inputs+j]);
   if(second)pack(z,e,w[(row+16+rc())*inputs+j]);
#endif
  }
#ifdef HIP_PREPACKED_WEIGHTS
  const uint*pw=reinterpret_cast<const uint*>(w);
  uint offset=((row+rc())*inputs+k+gr()*8)/4;
  y={int(pw[offset]),int(pw[offset+1])};
  if(second){offset+=16*inputs/4;z={int(pw[offset]),int(pw[offset+1])};}
#endif
  a=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(x,y,a);
  if(second)b=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(x,z,b);
 }
 for(uint e=0;e<8;e++){
  float v=a[e],g=clampf(v,-4.f,4.f),poly=__builtin_fmaf(g,absf(g)*(-.055908203125f)+.447265625f,.89453125f);
  out[(first+gr()*8+e)*outputs+row+rc()]=F(v*poly);
  if(second){v=b[e];g=clampf(v,-4.f,4.f);poly=__builtin_fmaf(g,absf(g)*(-.055908203125f)+.447265625f,.89453125f);
   out[(first+gr()*8+e)*outputs+row+16+rc()]=F(v*poly);}
 }
}
#else
WAVE void vit_expand(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){uint first=bid()/(outputs/16)*16,row=bid()%(outputs/16)*16;if(first>=tokens)return;f8 a{};a=dot8(in,w,first,row,inputs,0,inputs,a);for(uint e=0;e<8;e++){float v=a[e],g=clampf(v,-4.f,4.f),poly=__builtin_fmaf(g,absf(g)*(-.055908203125f)+.447265625f,.89453125f);out[(first+gr()*8+e)*outputs+row+rc()]=F(v*poly);}}
#endif
WAVE void vit_project(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs){uint first=bid()/(outputs/16)*16,row=bid()%(outputs/16)*16;if(first>=tokens)return;f8 total{};
 if(inputs==4096){for(uint e=0;e<8;e++)total[e]=H(skip[(first+gr()*8+e)*outputs+row+rc()]*w[inputs*outputs+row+rc()]);for(uint p=0;p<4;p++){f8 a{};a=dot8(in,w,first,row,inputs,p*(inputs/4),(p+1)*(inputs/4),a);for(uint e=0;e<8;e++)total[e]+=a[e];}}
 else{for(uint p=0;p<4;p++){f8 a{};if(!p)for(uint e=0;e<8;e++)a[e]=H(skip[(first+gr()*8+e)*outputs+row+rc()]*w[inputs*outputs+row+rc()]);a=dot8(in,w,first,row,inputs,p*(inputs/4),(p+1)*(inputs/4),a);for(uint e=0;e<8;e++)total[e]=p?total[e]+a[e]:a[e];}}
 for(uint e=0;e<8;e++)out[(first+gr()*8+e)*outputs+row+rc()]=F(H(total[e]));
}
// Production fused QKV keeps F32 dot until square normalization; no two512 partition rounds.
WAVE void vit_qkv_project(const float*in,const float*w,float*out,uint tokens){uint perpart=(tokens/16)*64,part=bid()/perpart,local=bid()%perpart,first=local/64*16,row=local%64*16;if(part>=3)return;f8 a{};for(uint k=0;k<1024;k+=16){h8 x{},y{};for(uint e=0;e<8;e++){uint j=k+gr()*8+e;x[e]=(_Float16)in[(first+rc())*1024+j];y[e]=(_Float16)w[part*1048576+(row+rc())*1024+j];}a=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,y,a);}for(uint e=0;e<8;e++)out[(part*tokens+first+gr()*8+e)*1024+row+rc()]=a[e];}
WAVE void vit_qkv_normalize(const float*in,const float*w,float*out,uint tokens){uint perpart=(tokens/16)*64,part=bid()/perpart,local=bid()%perpart,first=local/64*16,row=local%64*16,head=row/32;if(part>=3)return;f8 sum{};if(part<2){for(uint k=0;k<32;k+=16){h8 x{},ones{};for(uint e=0;e<8;e++){uint j=head*32+k+gr()*8+e;float v=in[(part*tokens+first+rc())*1024+j];x[e]=(_Float16)(v*v);ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,ones,sum);}}
 for(uint e=0;e<8;e++){uint i=(part*tokens+first+gr()*8+e)*1024+row+rc();float a=in[i];if(part<2){float scale=part==0?5.65625f*w[3145728+head]:1.f;a*=__builtin_amdgcn_rsqf(maxf(sum[e],6.198883056640625e-5f))*scale;}out[i]=F(a);}
}
// Explicit f32tof16 conversion in production attention is RTZ, unlike Matrix.Cast<F16>.
#ifndef HIP_NATIVE_RTZ
#define HIP_NATIVE_RTZ 1
#endif
#if HIP_NATIVE_RTZ
DEV float Hrtz(float x){uint h;float r;asm("v_cvt_pkrtz_f16_f32 %0, %1, %1":"=v"(h):"v"(x));asm("v_cvt_f32_f16 %0, %1":"=v"(r):"v"(h));return r;}
#else
DEV float Hrtz(float x){uint b=bits(x),a=b&0x7fffffffu,s=(b>>16)&0x8000u;int e=int(a>>23)-127;if(a>=0x7f800000u)return from_half(s|0x7c00u|((a&0x7fffffu)?0x200u:0u));if(e>15)return from_half(s|0x7bffu);if(e<-24)return from_half(s);if(e<-14)return from_half(s|(((a&0x7fffffu)|0x800000u)>>uint(-e-1)));return from_half(s|((a>>13)-0x1c000u));}
#endif
// scores work=T*T*32; T%16=0; ex[query][head32][key].
WAVE void vit_attention_scores_fast(const float*in,float*out,uint tokens){uint kt= tokens/16,key=(bid()%kt)*16,head=(bid()/kt)%32,first=bid()/(kt*32)*16;if(first>=tokens)return;f8 a{};for(uint k=0;k<32;k+=16){i2 x{},y{};for(uint e=0;e<8;e++){uint j=k+gr()*8+e;pack(x,e,in[(first+rc())*1024+head*32+j]);pack(y,e,in[(tokens+key+rc())*1024+head*32+j]);}a=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(x,y,a);}for(uint e=0;e<8;e++){float af=clampf(a[e]*from_half(0x2dbb)+1.708984375f,1.439453125f,1.9775390625f);uint hb=(bits(af)>>13)-0x1c000u;out[((first+gr()*8+e)*32+head)*tokens+key+rc()]=from_half(((hb<<4)+0x4000u)&65535u);}}
// inverse SPECIAL work=T*32*16, not T*32. One wave handles16 queries for one head.
WAVE void vit_attention_inverse_fast(const float*ex,float*out,uint tokens){uint head=bid()%32,first=bid()/32*16;if(first>=tokens)return;f8 a{};for(uint k=0;k<tokens;k+=16){h8 x{},ones{};for(uint e=0;e<8;e++){uint key=k+gr()*8+e;x[e]=(_Float16)ex[((first+rc())*32+head)*tokens+key];ones[e]=(_Float16)1.f;}a=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,ones,a);}if(rc()==0)for(uint e=0;e<8;e++)out[(first+gr()*8+e)*32+head]=1.f/a[e];}
// FUSED ViT attention (host option vit_attn_fused): one wave = 16 queries x one head over every key, bid = query tile*32 + head.
// Same per-element operations, operand packing and MMA K order as vit_attention_scores_fast / vit_attention_inverse_fast /
// vit_attention_av_fast: Q packed once, scores per16-key tile through the same exp bit map, the half bit pattern those
// kernels exchange through the f32 ex tensor kept in LDS ([query16][key] uint16, exact: ex is an f32 copy of a half),
// denominator = f16 MMA of the LDS rows against ones (K16 steps in key order), PV = FP8 MMA of pack(ex) against V for
// both16-column halves of the head, epilogue F(Hrtz(acc*inv)). Requires tokens%16==0 and tokens<=640.
// LDS per wave: P as E4M3 bytes [16][MAXT+16] plus a double-buffered16x16 half tile that transposes each score tile from
// accumulator layout to the A-operand layout (keys along K) for the denominator MMA and the byte store. Three token
// buckets keep the static LDS (and so the occupancy) matched to the geometry: 256 (8.2KB), 400 (8.2KB), 640 (12KB).
// ByteInput: Q/K/V arrive as the E4M3 bytes vit_qkv_project_normalize_fused_f16compact_fp8 stores (byte_F of the same
// F() values the f32 tensor holds, so pack() of the f32 value and the stored byte are identical): Q/K rows are one
// 8-byte load per lane, V columns eight byte loads placed exactly as pack() places them.
template<uint MAXT,bool ByteInput=false,bool ByteOut=false>
DEV void vit_attention_fused_body(const float*in,float*out,uint tokens){
 uint head=bid()%32,first=bid()/32*16;if(first>=tokens)return;
 __attribute__((shared)) unsigned char p8[16*(MAXT+16)];
 __attribute__((shared)) unsigned short t16[2*16*24];
 const uint S=tokens+16;const unsigned char*in8=reinterpret_cast<const unsigned char*>(in);
 i2 q[2]{};for(uint k=0;k<32;k+=16){if constexpr(ByteInput)__builtin_memcpy(&q[k/16],in8+(first+rc())*1024+head*32+k+gr()*8,8);else for(uint e=0;e<8;e++){uint j=k+gr()*8+e;pack(q[k/16],e,in[(first+rc())*1024+head*32+j]);}}
 h8 ones{};for(uint e=0;e<8;e++)ones[e]=(_Float16)1.f;
 f8 sum{};
 for(uint key=0,tile=0;key<tokens;key+=16,tile^=1u){f8 a{};
  for(uint k=0;k<32;k+=16){i2 y{};if constexpr(ByteInput)__builtin_memcpy(&y,in8+(tokens+key+rc())*1024+head*32+k+gr()*8,8);else for(uint e=0;e<8;e++){uint j=k+gr()*8+e;pack(y,e,in[(tokens+key+rc())*1024+head*32+j]);}a=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(q[k/16],y,a);}
  unsigned short*t=t16+tile*16*24;
  for(uint e=0;e<8;e++){float af=clampf(a[e]*from_half(0x2dbb)+1.708984375f,1.439453125f,1.9775390625f);uint hb=(bits(af)>>13)-0x1c000u;t[(gr()*8+e)*24+rc()]=(unsigned short)(((hb<<4)+0x4000u)&65535u);}
  __builtin_amdgcn_fence(3,"workgroup");__builtin_amdgcn_s_barrier();__builtin_amdgcn_fence(2,"workgroup");
  h8 x{};i2 xb{};for(uint e=0;e<8;e++){unsigned short u=t[rc()*24+gr()*8+e];union{unsigned short u;_Float16 h;}cv;cv.u=u;x[e]=cv.h;pack(xb,e,from_half(u));}
  sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,ones,sum);
  __builtin_memcpy(p8+rc()*S+key+gr()*8,&xb,8);
 }
 float inv[8];for(uint e=0;e<8;e++)inv[e]=1.f/sum[e];
 __builtin_amdgcn_fence(3,"workgroup");__builtin_amdgcn_s_barrier();__builtin_amdgcn_fence(2,"workgroup");
 f8 acc[2]{};
 for(uint k=0;k<tokens;k+=16){i2 x;__builtin_memcpy(&x,p8+rc()*S+k+gr()*8,8);
  for(uint c=0;c<2;c++){i2 y{};for(uint e=0;e<8;e++){uint key=k+gr()*8+e;if constexpr(ByteInput){uint b=in8[(2*tokens+key)*1024+head*32+c*16+rc()];y[e/4]=int(uint(y[e/4])|(b<<(8*(e%4))));}else pack(y,e,in[(2*tokens+key)*1024+head*32+c*16+rc()]);}acc[c]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(x,y,acc[c]);}
 }
 for(uint c=0;c<2;c++)for(uint e=0;e<8;e++){if constexpr(ByteOut)reinterpret_cast<unsigned char*>(out)[(first+gr()*8+e)*1024+head*32+c*16+rc()]=byte_F(Hrtz(acc[c][e]*inv[e]));else out[(first+gr()*8+e)*1024+head*32+c*16+rc()]=F(Hrtz(acc[c][e]*inv[e]));}
}
// Byte-stream twins: byte Q/K/V in, attention output stored as its E4M3 byte for vit_project_bytein_bout.
WAVE void vit_attention_fused_256_bytein_bout(const float*in,float*out,uint tokens){vit_attention_fused_body<256,true,true>(in,out,tokens);}
WAVE void vit_attention_fused_400_bytein_bout(const float*in,float*out,uint tokens){vit_attention_fused_body<400,true,true>(in,out,tokens);}
WAVE void vit_attention_fused_640_bytein_bout(const float*in,float*out,uint tokens){vit_attention_fused_body<640,true,true>(in,out,tokens);}
// ViT projection on the byte stream: attention bytes as the A operand (dot8<ByteInput>), residual skip decoded from the
// contract bytes, F(H(total)) stored as f32 (block output / final gather) and, when out8 is given, as the E4M3 byte
// the next block's expand consumes directly (identical to what vit_pack_input would produce from the f32 value).
template<bool HalfSkip>
DEV void vit_project_bytein_bout_body(const float*in,const float*w,const float*skip,float*out,unsigned char*out8,uint tokens){
 uint first=bid()/64*16,row=bid()%64*16;if(first>=tokens)return;f8 total{};const unsigned char*s8=reinterpret_cast<const unsigned char*>(skip);const _Float16*s16=reinterpret_cast<const _Float16*>(skip);
 for(uint p=0;p<4;p++){f8 a{};if(!p)for(uint e=0;e<8;e++){uint i=(first+gr()*8+e)*1024+row+rc();float s;if constexpr(HalfSkip)s=(float)s16[i];else s=__builtin_amdgcn_cvt_f32_fp8(int(s8[i]),0);a[e]=H(s*w[1048576+row+rc()]);}a=dot8<true>(in,w,first,row,1024,p*256,(p+1)*256,a);for(uint e=0;e<8;e++)total[e]=p?total[e]+a[e]:a[e];}
 for(uint e=0;e<8;e++){float v=F(H(total[e]));uint i=(first+gr()*8+e)*1024+row+rc();out[i]=v;if(out8)out8[i]=static_cast<unsigned char>(__builtin_amdgcn_cvt_pk_fp8_f32(v,0.f,0,false));}
}
WAVE void vit_project_bytein_bout(const float*in,const float*w,const float*skip,float*out,unsigned char*out8,uint tokens){vit_project_bytein_bout_body<false>(in,w,skip,out,out8,tokens);}
WAVE void vit_project_bytein_bout_hskip(const float*in,const float*w,const float*skip,float*out,unsigned char*out8,uint tokens){vit_project_bytein_bout_body<true>(in,w,skip,out,out8,tokens);}
WAVE void vit_attention_fused_256(const float*in,float*out,uint tokens){vit_attention_fused_body<256>(in,out,tokens);}
WAVE void vit_attention_fused_400(const float*in,float*out,uint tokens){vit_attention_fused_body<400>(in,out,tokens);}
WAVE void vit_attention_fused_640(const float*in,float*out,uint tokens){vit_attention_fused_body<640>(in,out,tokens);}
WAVE void vit_attention_fused_256_bytein(const float*in,float*out,uint tokens){vit_attention_fused_body<256,true>(in,out,tokens);}
WAVE void vit_attention_fused_400_bytein(const float*in,float*out,uint tokens){vit_attention_fused_body<400,true>(in,out,tokens);}
WAVE void vit_attention_fused_640_bytein(const float*in,float*out,uint tokens){vit_attention_fused_body<640,true>(in,out,tokens);}
WAVE void vit_attention_av_fast(const float*in,const float*ex,const float*inv,float*out,uint tokens){uint first=bid()/64*16,head=(bid()%64)/2,col=(bid()%2)*16;if(first>=tokens)return;f8 a{};for(uint k=0;k<tokens;k+=16){i2 x{},y{};for(uint e=0;e<8;e++){uint key=k+gr()*8+e;pack(x,e,ex[((first+rc())*32+head)*tokens+key]);pack(y,e,in[(2*tokens+key)*1024+head*32+col+rc()]);}a=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(x,y,a);}for(uint e=0;e<8;e++){uint q=first+gr()*8+e;out[q*1024+head*32+col+rc()]=F(Hrtz(a[e]*inv[q*32+head]));}}
// Production decoder build leaves NATIVE_DECODER_HW_H=0: H is integer RNE.
WAVE void decoder_project2x(const float*in,const float*w,const float*skip,float*out,uint iw,uint ih,uint ow,uint oh,uint inputs,uint outputs){uint first=bid()/(outputs/16)*16,row=bid()%(outputs/16)*16;if(first>=iw*ih)return;uint parts=inputs==1024?4:1;f8 total{};for(uint p=0;p<parts;p++){f8 a{};for(uint k=p*(inputs/parts);k<(p+1)*(inputs/parts);k+=16){h8 x{},y{};for(uint e=0;e<8;e++){uint j=k+gr()*8+e;x[e]=first+rc()<iw*ih?(_Float16)in[(first+rc())*inputs+j]:(_Float16)0;y[e]=(_Float16)w[(row+rc())*inputs+j];}a=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,y,a);}for(uint e=0;e<8;e++)total[e]=p?total[e]+a[e]:a[e];}for(uint e=0;e<8;e++){uint token=first+gr()*8+e;if(token>=iw*ih)continue;float t=H(total[e]);for(uint dy=0;dy<2;dy++)for(uint dx=0;dx<2;dx++){uint x=(token%iw)*2+dx,y=(token/iw)*2+dy;if(x>=ow||y>=oh)continue;uint i=(y*ow+x)*outputs+row+rc();float merged=H(t+F(skip[i])*w[inputs*outputs+row+rc()]);out[i]=outputs==32?merged:F(merged);}}}
// Prepacked-half twin of decoder_project2x (host option decoder_h16w): the [outputs][inputs] matrix arrives as RNE halves
// (same values the (_Float16) cast produces), one 16-byte load per B fragment; the skip scale stays f32 at its offset.
WAVE void decoder_project2x_h16w(const float*in,const float*w,const float*skip,float*out,uint iw,uint ih,uint ow,uint oh,uint inputs,uint outputs){uint first=bid()/(outputs/16)*16,row=bid()%(outputs/16)*16;if(first>=iw*ih)return;uint parts=inputs==1024?4:1;f8 total{};const _Float16*w16=reinterpret_cast<const _Float16*>(w);for(uint p=0;p<parts;p++){f8 a{};for(uint k=p*(inputs/parts);k<(p+1)*(inputs/parts);k+=16){h8 x{},y;for(uint e=0;e<8;e++){uint j=k+gr()*8+e;x[e]=first+rc()<iw*ih?(_Float16)in[(first+rc())*inputs+j]:(_Float16)0;}__builtin_memcpy(&y,w16+(row+rc())*inputs+k+gr()*8,16);a=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,y,a);}for(uint e=0;e<8;e++)total[e]=p?total[e]+a[e]:a[e];}
#if HIP_DEC_HOIST_SCALE
 const float ss=w[inputs*outputs+row+rc()]; // skip scale once per lane (was re-read per output element behind the out[] stores)
 for(uint e=0;e<8;e++){uint token=first+gr()*8+e;if(token>=iw*ih)continue;float t=H(total[e]);for(uint dy=0;dy<2;dy++)for(uint dx=0;dx<2;dx++){uint x=(token%iw)*2+dx,y=(token/iw)*2+dy;if(x>=ow||y>=oh)continue;uint i=(y*ow+x)*outputs+row+rc();float merged=H(t+F(skip[i])*ss);out[i]=outputs==32?merged:F(merged);}}}
#else
 for(uint e=0;e<8;e++){uint token=first+gr()*8+e;if(token>=iw*ih)continue;float t=H(total[e]);for(uint dy=0;dy<2;dy++)for(uint dx=0;dx<2;dx++){uint x=(token%iw)*2+dx,y=(token/iw)*2+dy;if(x>=ow||y>=oh)continue;uint i=(y*ow+x)*outputs+row+rc();float merged=H(t+F(skip[i])*w[inputs*outputs+row+rc()]);out[i]=outputs==32?merged:F(merged);}}}
#endif
template<bool ByteInput=false>
DEV f8 dot16(const float*in,const float*w,uint first,uint row,uint instride,uint kcols,uint inoff,uint woff,f8 a){for(uint k=0;k<kcols;k+=16){h8 x{},y{};for(uint e=0;e<8;e++){uint j=k+gr()*8+e;if constexpr(ByteInput)x[e]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int(reinterpret_cast<const unsigned char*>(in)[(first+rc())*instride+inoff+j]),0);else x[e]=(_Float16)in[(first+rc())*instride+inoff+j];y[e]=(_Float16)w[woff+(row+rc())*kcols+j];}a=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,y,a);}return a;}
WAVE void split_mix(const float*in,const float*w,float*out,uint tokens){uint first=bid()/32*16,row=bid()%32*16;if(first>=tokens)return;f8 a{};a=dot16(in,w,first,row,512,512,0,0,a);for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+row+rc()]=F(Hrtz(a[e]));}
WAVE void split_expand(const float*in,const float*w,float*out,uint tokens){uint first=bid()/128*16,col=bid()%128*16,g=col/256,row=col%256;if(first>=tokens)return;f8 a{};a=dot16(in,w,first,row,512,64,g*64,262144+g*16384,a);for(uint e=0;e<8;e++){float v=Hrtz(a[e]),gate=clampf(v,-4.f,4.f),poly=gate*(absf(gate)*(-.055908203125f)+.447265625f)+.89453125f;out[(first+gr()*8+e)*2048+col+rc()]=F(v*poly);}}
WAVE void split_contract(const float*in,const float*w,float*out,uint tokens){uint first=bid()/32*16,col=bid()%32*16,g=col/64,row=col%64;if(first>=tokens)return;f8 a{};a=dot16(in,w,first,row,2048,256,g*256,393216+g*16384,a);for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+col+rc()]=F(Hrtz(a[e]));}
// stream0 FFWD projection build omits NATIVE_HW_H: initial and final H are integer RNE.
WAVE void split_projection(const float*in,const float*w,const float*skip,float*out,uint tokens){uint first=bid()/32*16,row=bid()%32*16;if(first>=tokens)return;f8 a{};for(uint e=0;e<8;e++)a[e]=H(skip[(first+gr()*8+e)*512+row+rc()]*w[262144+row+rc()]);a=dot8(in,w,first,row,512,0,512,a);for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+row+rc()]=F(H(a[e]));}

DEV unsigned char byte_F(float x){return static_cast<unsigned char>(__builtin_amdgcn_cvt_pk_fp8_f32(F(x),0.f,0,false));}

WAVE void vit_expand_fp8(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){uint first=bid()/(outputs/16)*16,row=bid()%(outputs/16)*16;if(first>=tokens)return;f8 a{};a=dot8(in,w,first,row,inputs,0,inputs,a);for(uint e=0;e<8;e++){float v=a[e],g=clampf(v,-4.f,4.f),poly=__builtin_fmaf(g,absf(g)*(-.055908203125f)+.447265625f,.89453125f);reinterpret_cast<unsigned char*>(out)[(first+gr()*8+e)*outputs+row+rc()]=byte_F(v*poly);}}

WAVE void vit_project_fp8(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs){uint first=bid()/(outputs/16)*16,row=bid()%(outputs/16)*16;if(first>=tokens)return;f8 total{};
 if(inputs==4096){for(uint e=0;e<8;e++)total[e]=H(skip[(first+gr()*8+e)*outputs+row+rc()]*w[inputs*outputs+row+rc()]);for(uint p=0;p<4;p++){f8 a{};a=dot8<true>(in,w,first,row,inputs,p*(inputs/4),(p+1)*(inputs/4),a);for(uint e=0;e<8;e++)total[e]+=a[e];}}
 else{for(uint p=0;p<4;p++){f8 a{};if(!p)for(uint e=0;e<8;e++)a[e]=H(skip[(first+gr()*8+e)*outputs+row+rc()]*w[inputs*outputs+row+rc()]);a=dot8<true>(in,w,first,row,inputs,p*(inputs/4),(p+1)*(inputs/4),a);for(uint e=0;e<8;e++)total[e]=p?total[e]+a[e]:a[e];}}
 for(uint e=0;e<8;e++)out[(first+gr()*8+e)*outputs+row+rc()]=F(H(total[e]));
}

WAVE void split_expand_fp8(const float*in,const float*w,float*out,uint tokens){uint first=bid()/128*16,col=bid()%128*16,g=col/256,row=col%256;if(first>=tokens)return;f8 a{};a=dot16(in,w,first,row,512,64,g*64,262144+g*16384,a);for(uint e=0;e<8;e++){float v=Hrtz(a[e]),gate=clampf(v,-4.f,4.f),poly=gate*(absf(gate)*(-.055908203125f)+.447265625f)+.89453125f;reinterpret_cast<unsigned char*>(out)[(first+gr()*8+e)*2048+col+rc()]=byte_F(v*poly);}}

WAVE void split_contract_fp8(const float*in,const float*w,float*out,uint tokens){uint first=bid()/32*16,col=bid()%32*16,g=col/64,row=col%64;if(first>=tokens)return;f8 a{};a=dot16<true>(in,w,first,row,2048,256,g*256,393216+g*16384,a);for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+col+rc()]=F(Hrtz(a[e]));}

// Four independent K1024 partitions of the existing K4096 contract.
WAVE void vit_contract_parts_fp8(const float*in,const float*w,float*out,uint tokens){
 uint blocks=(tokens/16)*64,part=bid()/blocks,local=bid()%blocks,first=local/64*16,row=local%64*16;
 if(part>=4)return;f8 a{};a=dot8<true>(in,w,first,row,4096,part*1024,(part+1)*1024,a);
 for(uint e=0;e<8;e++)out[(part*tokens+first+gr()*8+e)*1024+row+rc()]=a[e];
}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void vit_contract_combine(const float*partial,const float*w,const float*skip,float*out,uint tokens){
 uint i=bid()*256+__builtin_amdgcn_workitem_id_x(),count=tokens*1024;if(i>=count)return;
 float total=H(skip[i]*w[4194304+i%1024]);
 for(uint part=0;part<4;part++)total+=partial[part*count+i];
 out[i]=F(H(total));
}

template<bool Tiled> DEV i2 vit_weight_fragment(const float*w,uint row,uint channels,uint k){
 i2 b{};
 if constexpr(Tiled){const unsigned char*p=reinterpret_cast<const unsigned char*>(w);uint off=((row/16)*(channels/32)+k/32)*512+(k%32+gr()*8)*16+row%16;for(uint e=0;e<8;e++)b[e/4]=int(uint(b[e/4])|(uint(p[off+16*e])<<(8*(e%4))));}
 else{const uint*p=reinterpret_cast<const uint*>(w);uint off=(row*channels+k+gr()*8)/4;b={int(p[off]),int(p[off+1])};}
 return b;
}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void vit_pack_input(const float*in,uint*out,uint count){
 uint word=bid()*256+__builtin_amdgcn_workitem_id_x(),i=word*4;if(i>=count)return;
 uint a=uint(__builtin_amdgcn_cvt_pk_fp8_f32(clampf(in[i],-448.f,448.f),clampf(in[i+1],-448.f,448.f),0,false));
 uint b=uint(__builtin_amdgcn_cvt_pk_fp8_f32(clampf(in[i+2],-448.f,448.f),clampf(in[i+3],-448.f,448.f),0,false));
 out[word]=(a&65535u)|((b&65535u)<<16);
}
// A-operand tile layout for the ViT expand input (host option vit_input_tiled): 16 tokens x 32 channels per 512-byte
// tile, inside a tile byte ((k%32)/8)*128+(row%16)*8+k%8, so one wave's A fragment (lanes rc=row, gr=k half) is a
// contiguous 256-byte load. Same bytes as vit_pack_input, only placed differently.
DEV uint vit_tile_offset(uint row,uint k,uint channels){return ((row/16)*(channels/32)+k/32)*512+((k%32)/8)*128+(row%16)*8+k%8;}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256)))
void vit_pack_input_tiled(const float*in,uint*out,uint count){
 uint word=bid()*256+__builtin_amdgcn_workitem_id_x(),i=word*4;if(i>=count)return;uint row=i/1024,k=i%1024;
 uint a=uint(__builtin_amdgcn_cvt_pk_fp8_f32(clampf(in[i],-448.f,448.f),clampf(in[i+1],-448.f,448.f),0,false));
 uint b=uint(__builtin_amdgcn_cvt_pk_fp8_f32(clampf(in[i+2],-448.f,448.f),clampf(in[i+3],-448.f,448.f),0,false));
 out[vit_tile_offset(row,k,1024)/4]=(a&65535u)|((b&65535u)<<16);
}
// One wave produces16x64, reusing A across four output-column fragments.
DEV i2 vit_weight_fragment_native(const float*w,uint row,uint channels,uint k){i2 b;__builtin_memcpy(&b,reinterpret_cast<const unsigned char*>(w)+((row/16)*(channels/32)+k/32)*512+(((k%32)/16*2+gr())*16+row%16)*8,8);return b;}
template<bool Tiled=false,bool ByteInput=false,bool Frag=false,bool TiledInput=false>
DEV void vit_expand_blocked_body(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){
 uint tiles=outputs/64,first=bid()/tiles*16,col=bid()%tiles*64;if(first>=tokens)return;
 const uint*pw=reinterpret_cast<const uint*>(w);f8 accum[4]{};
 for(uint k=0;k<inputs;k+=16){i2 a{};
  if constexpr(TiledInput){__builtin_memcpy(&a,reinterpret_cast<const unsigned char*>(in)+vit_tile_offset(first+rc(),k+gr()*8,inputs),8);}
  else if constexpr(ByteInput){const uint*p=reinterpret_cast<const uint*>(in);uint off=((first+rc())*inputs+k+gr()*8)/4;a={int(p[off]),int(p[off+1])};}
  else for(uint e=0;e<8;e++)pack(a,e,in[(first+rc())*inputs+k+gr()*8+e]);
  for(uint n=0;n<4;n++){uint row=col+n*16+rc();i2 b;if constexpr(Frag)b=vit_weight_fragment_native(w,row,inputs,k);else b=vit_weight_fragment<Tiled>(w,row,inputs,k);accum[n]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,accum[n]);}
 }
 for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){float v=accum[n][e],g=clampf(v,-4.f,4.f),poly=__builtin_fmaf(g,absf(g)*(-.055908203125f)+.447265625f,.89453125f);reinterpret_cast<unsigned char*>(out)[(first+gr()*8+e)*outputs+col+n*16+rc()]=byte_F(v*poly);}
}

// BM token tiles per wave (host option vit_expand_m4, HLSL BLOCK_M=4 shape): every weight fragment load serves BM
// token tiles. Each accumulator keeps its own K-ascending order with the same a/b operands as the single-tile body, so
// the bytes are identical; token tiles past the end are neither loaded nor stored (wave-uniform branch).
template<bool Tiled,bool ByteInput,uint BM,bool Frag=false>
DEV void vit_expand_blocked_body_m(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){
 uint tiles=outputs/64,first=bid()/tiles*16*BM,col=bid()%tiles*64;if(first>=tokens)return;
 f8 accum[BM][4]{};
 for(uint k=0;k<inputs;k+=16){i2 a[BM];
  #pragma unroll
  for(uint m=0;m<BM;m++){a[m]={};uint row=first+m*16;if(row<tokens){if constexpr(ByteInput){const uint*p=reinterpret_cast<const uint*>(in);uint off=((row+rc())*inputs+k+gr()*8)/4;a[m]={int(p[off]),int(p[off+1])};}else for(uint e=0;e<8;e++)pack(a[m],e,in[(row+rc())*inputs+k+gr()*8+e]);}}
  i2 b[4];
  #pragma unroll
  for(uint n=0;n<4;n++){if constexpr(Frag)b[n]=vit_weight_fragment_native(w,col+n*16+rc(),inputs,k);else b[n]=vit_weight_fragment<Tiled>(w,col+n*16+rc(),inputs,k);}
  #pragma unroll
  for(uint m=0;m<BM;m++){if(first+m*16<tokens){
   #pragma unroll
   for(uint n=0;n<4;n++)accum[m][n]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a[m],b[n],accum[m][n]);}}
 }
 #pragma unroll
 for(uint m=0;m<BM;m++){uint row=first+m*16;if(row<tokens){
  #pragma unroll
  for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){float v=accum[m][n][e],g=clampf(v,-4.f,4.f),poly=__builtin_fmaf(g,absf(g)*(-.055908203125f)+.447265625f,.89453125f);reinterpret_cast<unsigned char*>(out)[(row+gr()*8+e)*outputs+col+n*16+rc()]=byte_F(v*poly);}}}
}
WAVE void vit_expand_blocked_fp8_tiled_bytein_m4(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){vit_expand_blocked_body_m<true,true,4>(in,w,out,tokens,inputs,outputs);}
WAVE void vit_expand_blocked_fp8_frag_bytein_m2(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){vit_expand_blocked_body_m<true,true,2,true>(in,w,out,tokens,inputs,outputs);}
WAVE void vit_expand_blocked_fp8_frag_bytein_m4(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){vit_expand_blocked_body_m<true,true,4,true>(in,w,out,tokens,inputs,outputs);}

// ByteStream: the block input arrives as the E4M3 bytes the previous vit_project_bytein_bout stored (or vit_pack_input
// produced), the residual skip is decoded from them, and the F(H(total)) result is stored as its byte (byte_F: exact on
// the F() lattice). Same MMA operands, accumulation order and rounding as the f32 body.
template<bool Partial,bool Tiled=false,bool ByteStream=false,bool HalfOut=false,bool Frag=false>
DEV void vit_contract_blocked_body(const float*in,const float*w,const float*skip,float*out,uint tokens){
 uint blocks=tokens,part0=Partial?bid()/blocks:0,local=Partial?bid()%blocks:bid(),first=local/16*16,col=local%16*64;
 if(first>=tokens||part0>=4)return;const uint*ip=reinterpret_cast<const uint*>(in),*wp=reinterpret_cast<const uint*>(w);f8 total[4]{};
 if constexpr(!Partial)for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){uint c=col+n*16+rc();float s;if constexpr(ByteStream)s=__builtin_amdgcn_cvt_f32_fp8(int(reinterpret_cast<const unsigned char*>(skip)[(first+gr()*8+e)*1024+c]),0);else s=skip[(first+gr()*8+e)*1024+c];total[n][e]=H(s*w[4194304+c]);}
 for(uint part=part0;part<(Partial?part0+1:4);part++){f8 acc[4]{};
  for(uint k=part*1024;k<(part+1)*1024;k+=16){uint off=((first+rc())*4096+k+gr()*8)/4;i2 a={int(ip[off]),int(ip[off+1])};
   for(uint n=0;n<4;n++){uint row=col+n*16+rc();i2 b;if constexpr(Frag)b=vit_weight_fragment_native(w,row,4096,k);else b=vit_weight_fragment<Tiled>(w,row,4096,k);acc[n]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc[n]);}
  }
  for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){if constexpr(Partial)out[(part*tokens+first+gr()*8+e)*1024+col+n*16+rc()]=acc[n][e];else total[n][e]+=acc[n][e];}
 }
 if constexpr(!Partial)for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){uint i=(first+gr()*8+e)*1024+col+n*16+rc();if constexpr(HalfOut)reinterpret_cast<_Float16*>(out)[i]=(_Float16)F(H(total[n][e]));else if constexpr(ByteStream)reinterpret_cast<unsigned char*>(out)[i]=byte_F(H(total[n][e]));else out[i]=F(H(total[n][e]));}
}
// Fragment-weight twin (host option vit_contract_frag): B from the FragmentPackedMatrix tiles (one contiguous 8-byte load) as the frag expand does.
WAVE void vit_contract_blocked_fp8_frag(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs){if(inputs==4096&&outputs==1024)vit_contract_blocked_body<false,false,false,false,true>(in,w,skip,out,tokens);}
WAVE void vit_contract_blocked_fp8_bstream(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs){if(inputs==4096&&outputs==1024)vit_contract_blocked_body<false,false,true>(in,w,skip,out,tokens);}
// Half-stream contract: byte skip in, F(H(total)) stored as F16 (exact on the E4M3 lattice) so the QKV projection loads
// its A fragments directly (vit_qkv_project_normalize_fused_f16compact_fp8_h16in*), the way the HLSL fused QKV does.
WAVE void vit_contract_blocked_fp8_hstream(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs){if(inputs==4096&&outputs==1024)vit_contract_blocked_body<false,false,true,true>(in,w,skip,out,tokens);}
WAVE void vit_contract_blocked_fp8_tiled_hstream(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs){if(inputs==4096&&outputs==1024)vit_contract_blocked_body<false,true,true,true>(in,w,skip,out,tokens);}
WAVE void vit_contract_blocked_fp8_tiled_bstream(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs){if(inputs==4096&&outputs==1024)vit_contract_blocked_body<false,true,true>(in,w,skip,out,tokens);}
// Fused ViT FFN (host option vit_ffn_fused): 16 waves own 16 tokens; hidden is produced in four 1024-column quarters
// (each wave 64 hidden columns, full K=1024 expand chain, activation + byte_F as vit_expand_blocked_body) into LDS,
// and each quarter's contract partial (each wave 64 output columns, K=1024 chain in ascending order) is added to the
// running total exactly as vit_contract_blocked_body adds its four parts (total starts at H(skip*scale)). Two launches
// become one and the 4096-byte hidden rows never leave the CU. Expand weights fragment-native, contract weights row-major.
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void vit_ffn_fused(const float*in,const float*we,const float*wc,const float*skip,float*out,uint tokens){
 uint tid=__builtin_amdgcn_workitem_id_x(),wave=tid/32,first=bid()*16;if(first>=tokens)return;
 __attribute__((shared)) unsigned char hid[16*1040];
 const uint*ip=reinterpret_cast<const uint*>(in),*cp=reinterpret_cast<const uint*>(wc);
 f8 total[4];for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){uint c=wave*64+n*16+rc();total[n][e]=H(skip[(first+gr()*8+e)*1024+c]*wc[4194304+c]);}
 for(uint q=0;q<4;q++){
  f8 accum[4]{};
  for(uint k=0;k<1024;k+=16){uint off=((first+rc())*1024+k+gr()*8)/4;i2 a={int(ip[off]),int(ip[off+1])};
   for(uint n=0;n<4;n++){uint row=q*1024+wave*64+n*16+rc();i2 b=vit_weight_fragment_native(we,row,1024,k);accum[n]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,accum[n]);}
  }
  if(q)__builtin_amdgcn_s_barrier();
  for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){float v=accum[n][e],g=clampf(v,-4.f,4.f),poly=__builtin_fmaf(g,absf(g)*(-.055908203125f)+.447265625f,.89453125f);hid[(gr()*8+e)*1040+wave*64+n*16+rc()]=byte_F(v*poly);}
  __builtin_amdgcn_s_barrier();
  f8 acc[4]{};
  for(uint k=0;k<1024;k+=16){i2 a;__builtin_memcpy(&a,hid+rc()*1040+k+gr()*8,8);
   for(uint n=0;n<4;n++){uint row=wave*64+n*16+rc(),wo=(row*4096+q*1024+k+gr()*8)/4;i2 b={int(cp[wo]),int(cp[wo+1])};acc[n]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc[n]);}
  }
  for(uint n=0;n<4;n++)for(uint e=0;e<8;e++)total[n][e]+=acc[n][e];
 }
 for(uint n=0;n<4;n++)for(uint e=0;e<8;e++)out[(first+gr()*8+e)*1024+wave*64+n*16+rc()]=F(H(total[n][e]));
}
WAVE void vit_contract_blocked_fp8(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs){if(inputs==4096&&outputs==1024)vit_contract_blocked_body<false>(in,w,skip,out,tokens);}
WAVE void vit_contract_parts_blocked_fp8(const float*in,const float*w,float*out,uint tokens){vit_contract_blocked_body<true>(in,w,nullptr,out,tokens);}

WAVE void vit_expand_blocked_fp8(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){vit_expand_blocked_body<false,false>(in,w,out,tokens,inputs,outputs);}

WAVE void vit_expand_blocked_fp8_tiled(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){vit_expand_blocked_body<true,false>(in,w,out,tokens,inputs,outputs);}

WAVE void vit_expand_blocked_fp8_bytein(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){vit_expand_blocked_body<false,true>(in,w,out,tokens,inputs,outputs);}

WAVE void vit_expand_blocked_fp8_tiled_bytein(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){vit_expand_blocked_body<true,true>(in,w,out,tokens,inputs,outputs);}
// Fragment-native weight layout (FragmentPackedMatrix): one 8-byte load per B fragment instead of eight byte gathers.
WAVE void vit_expand_blocked_fp8_frag_bytein(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){vit_expand_blocked_body<true,true,true>(in,w,out,tokens,inputs,outputs);}
// Tile-layout input (vit_pack_input_tiled) + fragment-native weights: both operands are contiguous 8-byte lane loads.
WAVE void vit_expand_blocked_fp8_frag_tiledin(const float*in,const float*w,float*out,uint tokens,uint inputs,uint outputs){vit_expand_blocked_body<true,true,true,true>(in,w,out,tokens,inputs,outputs);}

WAVE void vit_contract_blocked_fp8_tiled(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs){if(inputs==4096&&outputs==1024)vit_contract_blocked_body<false,true>(in,w,skip,out,tokens);}
WAVE void vit_contract_parts_blocked_fp8_tiled(const float*in,const float*w,float*out,uint tokens){vit_contract_blocked_body<true,true>(in,w,nullptr,out,tokens);}

KERNEL __attribute__((amdgpu_flat_work_group_size(128,128)))
void split_ffn_fused(const float*in,const float*w,float*out,uint tokens){
 uint block=bid(),first=(block/8)*16,g=block%8,tid=__builtin_amdgcn_workitem_id_x(),wave=tid/32;
 if(first>=tokens)return;
 __attribute__((shared)) unsigned char hidden[16*260];
 f8 expanded[4]{};
 for(uint j=0;j<4;j++)expanded[j]=dot16(in,w,first,wave*64+j*16,512,64,g*64,262144+g*16384,expanded[j]);
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++){float v=Hrtz(expanded[j][e]),gate=clampf(v,-4.f,4.f),poly=gate*(absf(gate)*(-.055908203125f)+.447265625f)+.89453125f;hidden[(gr()*8+e)*260+wave*64+j*16+rc()]=byte_F(v*poly);}
 __builtin_amdgcn_s_barrier();
 f8 result{};
 for(uint k=0;k<256;k+=16){h8 a{},b{};for(uint e=0;e<8;e++){uint kk=k+gr()*8+e;a[e]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int(hidden[rc()*260+kk]),0);b[e]=(_Float16)w[393216+g*16384+(wave*16+rc())*256+kk];}result=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,result);}
 for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+g*64+wave*16+rc()]=F(Hrtz(result[e]));
}

WAVE void split_mix_blocked(const float*in,const float*w,float*out,uint tokens){
 uint first=bid()/8*16,row=bid()%8*64;if(first>=tokens)return;f8 acc[4]{};
 for(uint k=0;k<512;k+=16){h8 a{};for(uint e=0;e<8;e++)a[e]=(_Float16)in[(first+rc())*512+k+gr()*8+e];for(uint j=0;j<4;j++){h8 b{};for(uint e=0;e<8;e++)b[e]=(_Float16)w[(row+j*16+rc())*512+k+gr()*8+e];acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,acc[j]);}}
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+row+j*16+rc()]=F(Hrtz(acc[j][e]));
}

// split_mix_blocked with the mix weights prepacked as RNE half (PackedSplitFfnWeightMixHalf): one 16-byte load per B
// fragment instead of eight f32 loads and casts; identical operands, so identical bits.
// split_ffn_fused_fp8 with a second E4M3 tiled store of F(Hrtz(result)) for split_projection_frag (host option c512_proj_tiles).
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128)))
void split_ffn_fused_fp8_t8(const float*in,const float*w,float*out,unsigned char*out8,uint tokens){
 uint block=bid(),first=(block/8)*16,g=block%8,tid=__builtin_amdgcn_workitem_id_x(),wave=tid/32;
 if(first>=tokens)return;
 __attribute__((shared)) unsigned char hidden[16*260];
 f8 expanded[4]{};
 for(uint j=0;j<4;j++)for(uint k=0;k<64;k+=16){h8 a{},b{};for(uint e=0;e<8;e++)a[e]=(_Float16)in[(first+rc())*512+g*64+k+gr()*8+e];__builtin_memcpy(&b,reinterpret_cast<const _Float16*>(w+262144)+g*16384+(wave*64+j*16+rc())*64+k+gr()*8,16);expanded[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,expanded[j]);}
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++){float v=Hrtz(expanded[j][e]),gate=clampf(v,-4.f,4.f),poly=gate*(absf(gate)*(-.055908203125f)+.447265625f)+.89453125f;hidden[(gr()*8+e)*260+wave*64+j*16+rc()]=byte_F(v*poly);}
 __builtin_amdgcn_s_barrier();
 f8 result{};
 const uint*pw=reinterpret_cast<const uint*>(w);
 for(uint k=0;k<256;k+=16){i2 a{};__builtin_memcpy(&a,hidden+rc()*260+k+gr()*8,8);uint off=393216+g*4096+((wave*16+rc())*256+k+gr()*8)/4;i2 b={int(pw[off]),int(pw[off+1])};result=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,result);}
 for(uint e=0;e<8;e++){uint r=first+gr()*8+e,c=g*64+wave*16+rc();float v=F(Hrtz(result[e]));out[r*512+c]=v;out8[((r/16)*16+c/32)*512+(r%16)*32+c%32]=static_cast<unsigned char>(__builtin_amdgcn_cvt_pk_fp8_f32(v,0.f,0,false));}
}
// split_projection_blocked in the QKV-frag shape: A from the contract's E4M3 tiles, B from fragment-tiled E4M3 projection
// weights (PackedSplitProjectionFrag), no LDS; residual H(skip*scale) and F(H(acc)) as split_projection_blocked. Also
// stores the tiled E4M3 copy for mh_qkv_normalize_frag_c512 (replaces split_projection_blocked_t8).
WAVE void split_projection_frag(const unsigned char*in8,const float*w,const float*skip,float*out,unsigned char*out8,uint tokens){
 uint first=bid()/8*16,row=bid()%8*64;if(first>=tokens)return;f8 acc[4]{};
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)acc[j][e]=H(skip[(first+gr()*8+e)*512+row+j*16+rc()]*w[262144+row+j*16+rc()]);
 const unsigned char*wb=reinterpret_cast<const unsigned char*>(w);const unsigned char*arow=in8+((first/16)*16)*512+rc()*32+gr()*8;
 for(uint gs=0;gs<16;gs++)for(uint h=0;h<2;h++){i2 a;__builtin_memcpy(&a,arow+gs*512+h*16,8);
  for(uint j=0;j<4;j++){uint n0=row+j*16+rc();i2 b;__builtin_memcpy(&b,wb+((n0/16)*16+gs)*512+((h*2+gr())*16+n0%16)*8,8);acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc[j]);}}
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++){uint r=first+gr()*8+e,c=row+j*16+rc();float v=F(H(acc[j][e]));out[r*512+c]=v;out8[((r/16)*16+c/32)*512+(r%16)*32+c%32]=static_cast<unsigned char>(__builtin_amdgcn_cvt_pk_fp8_f32(v,0.f,0,false));}
}
WAVE void split_mix_blocked_h16w(const float*in,const float*w,float*out,uint tokens){
 uint first=bid()/8*16,row=bid()%8*64;if(first>=tokens)return;f8 acc[4]{};const _Float16*w16=reinterpret_cast<const _Float16*>(w);
 for(uint k=0;k<512;k+=16){h8 a{};for(uint e=0;e<8;e++)a[e]=(_Float16)in[(first+rc())*512+k+gr()*8+e];for(uint j=0;j<4;j++){h8 b;__builtin_memcpy(&b,w16+(row+j*16+rc())*512+k+gr()*8,16);acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,acc[j]);}}
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+row+j*16+rc()]=F(Hrtz(acc[j][e]));
}
WAVE void split_projection_blocked(const float*in,const float*w,const float*skip,float*out,uint tokens){
 uint first=bid()/8*16,row=bid()%8*64;if(first>=tokens)return;f8 acc[4]{};
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)acc[j][e]=H(skip[(first+gr()*8+e)*512+row+j*16+rc()]*w[262144+row+j*16+rc()]);
 for(uint k=0;k<512;k+=16){i2 a{};for(uint e=0;e<8;e++)pack(a,e,in[(first+rc())*512+k+gr()*8+e]);for(uint j=0;j<4;j++){i2 b{};
#ifdef HIP_PREPACKED_WEIGHTS
  const uint*pw=reinterpret_cast<const uint*>(w);uint off=((row+j*16+rc())*512+k+gr()*8)/4;b={int(pw[off]),int(pw[off+1])};
#else
  for(uint e=0;e<8;e++)pack(b,e,w[(row+j*16+rc())*512+k+gr()*8+e]);
#endif
  acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc[j]);}}
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+row+j*16+rc()]=F(H(acc[j][e]));
}

// split_projection_blocked with a second, E4M3 tiled store (host option c512_qkv_frag): the F(H(acc)) values are also
// written as 16-token x 32-channel row-major 512-byte tiles (HLSL stream8 layout) for mh_qkv_normalize_frag_c512.
WAVE void split_projection_blocked_t8(const float*in,const float*w,const float*skip,float*out,unsigned char*out8,uint tokens){
 uint first=bid()/8*16,row=bid()%8*64;if(first>=tokens)return;f8 acc[4]{};
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)acc[j][e]=H(skip[(first+gr()*8+e)*512+row+j*16+rc()]*w[262144+row+j*16+rc()]);
 for(uint k=0;k<512;k+=16){i2 a{};for(uint e=0;e<8;e++)pack(a,e,in[(first+rc())*512+k+gr()*8+e]);for(uint j=0;j<4;j++){i2 b{};
#ifdef HIP_PREPACKED_WEIGHTS
  const uint*pw=reinterpret_cast<const uint*>(w);uint off=((row+j*16+rc())*512+k+gr()*8)/4;b={int(pw[off]),int(pw[off+1])};
#else
  for(uint e=0;e<8;e++)pack(b,e,w[(row+j*16+rc())*512+k+gr()*8+e]);
#endif
  acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,acc[j]);}}
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++){uint r=first+gr()*8+e,c=row+j*16+rc();float v=F(H(acc[j][e]));out[r*512+c]=v;out8[((r/16)*16+c/32)*512+(r%16)*32+c%32]=static_cast<unsigned char>(__builtin_amdgcn_cvt_pk_fp8_f32(v,0.f,0,false));}
}
WAVE void vit_qkv_project_blocked(const float*in,const float*w,float*out,uint tokens){
 uint perpart=(tokens/16)*32,part=bid()/perpart,local=bid()%perpart,first=local/32*16,row=local%32*32;if(part>=3)return;f8 acc[2]{};
 for(uint k=0;k<1024;k+=16){h8 a{};for(uint e=0;e<8;e++)a[e]=(_Float16)in[(first+rc())*1024+k+gr()*8+e];for(uint j=0;j<2;j++){h8 b{};for(uint e=0;e<8;e++)b[e]=(_Float16)w[part*1048576+(row+j*16+rc())*1024+k+gr()*8+e];acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,acc[j]);}}
 for(uint j=0;j<2;j++)for(uint e=0;e<8;e++)out[(part*tokens+first+gr()*8+e)*1024+row+j*16+rc()]=acc[j][e];
}

WAVE void vit_qkv_project_normalize_fused(const float*in,const float*w,float*out,uint tokens){
 uint perpart=(tokens/16)*32,part=bid()/perpart,local=bid()%perpart,first=local/32*16,row=local%32*32;if(part>=3)return;f8 acc[2]{};
 for(uint k=0;k<1024;k+=16){h8 a{};for(uint e=0;e<8;e++)a[e]=(_Float16)in[(first+rc())*1024+k+gr()*8+e];for(uint j=0;j<2;j++){h8 b{};for(uint e=0;e<8;e++)b[e]=(_Float16)w[part*1048576+(row+j*16+rc())*1024+k+gr()*8+e];acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,acc[j]);}}
 __attribute__((shared)) float raw[16*33];
 f8 sum{};
 if(part<2){
  for(uint j=0;j<2;j++)for(uint e=0;e<8;e++)raw[(gr()*8+e)*33+j*16+rc()]=acc[j][e];
  __builtin_amdgcn_s_barrier();
  for(uint k=0;k<32;k+=16){h8 x{},ones{};for(uint e=0;e<8;e++){float v=raw[rc()*33+k+gr()*8+e];x[e]=(_Float16)(v*v);ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,ones,sum);}
 }
 for(uint j=0;j<2;j++)for(uint e=0;e<8;e++){float v=acc[j][e];if(part<2){float scale=part==0?5.65625f*w[3145728+row/32]:1.f;v*=__builtin_amdgcn_rsqf(maxf(sum[e],6.198883056640625e-5f))*scale;}out[(part*tokens+first+gr()*8+e)*1024+row+j*16+rc()]=F(v);}
}

KERNEL __attribute__((amdgpu_flat_work_group_size(128,128)))
// Keep expansion F16: the FP8 expansion probe differs on block46 wide-range input.
void split_ffn_fused_fp8(const float*in,const float*w,float*out,uint tokens){
 uint block=bid(),first=(block/8)*16,g=block%8,tid=__builtin_amdgcn_workitem_id_x(),wave=tid/32;
 if(first>=tokens)return;
 __attribute__((shared)) unsigned char hidden[16*260];
 f8 expanded[4]{};
 for(uint j=0;j<4;j++)for(uint k=0;k<64;k+=16){h8 a{},b{};for(uint e=0;e<8;e++)a[e]=(_Float16)in[(first+rc())*512+g*64+k+gr()*8+e];__builtin_memcpy(&b,reinterpret_cast<const _Float16*>(w+262144)+g*16384+(wave*64+j*16+rc())*64+k+gr()*8,16);expanded[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,expanded[j]);}
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++){float v=Hrtz(expanded[j][e]),gate=clampf(v,-4.f,4.f),poly=gate*(absf(gate)*(-.055908203125f)+.447265625f)+.89453125f;hidden[(gr()*8+e)*260+wave*64+j*16+rc()]=byte_F(v*poly);}
 __builtin_amdgcn_s_barrier();
 f8 result{};
 const uint*pw=reinterpret_cast<const uint*>(w);
 for(uint k=0;k<256;k+=16){i2 a{};__builtin_memcpy(&a,hidden+rc()*260+k+gr()*8,8);uint off=393216+g*4096+((wave*16+rc())*256+k+gr()*8)/4;i2 b={int(pw[off]),int(pw[off+1])};result=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,result);}
 for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+g*64+wave*16+rc()]=F(Hrtz(result[e]));
}

// Mix fused into the C512 FFN (host option split_mix_fused, HLSL ffwd_parallel shape): the four waves of a group each
// compute 16 of the group's 64 mixed columns over K=512 (same F16 MMA chain and F(Hrtz()) as split_mix_blocked), park
// them as exact halves in LDS, and the expand reads its A fragments from there. One launch instead of two; the mixed
// tensor never leaves the CU. Weight buffer = PackedSplitFfnWeight (mix f32 at 0, expand F16 at 262144, contract E4M3 at 393216).
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128)))
void split_ffn_fused_fp8_mix(const float*in,const float*w,float*out,uint tokens){
 uint block=bid(),first=(block/8)*16,g=block%8,tid=__builtin_amdgcn_workitem_id_x(),wave=tid/32;
 if(first>=tokens)return;
 __attribute__((shared)) unsigned char hidden[16*260];
 __attribute__((shared)) _Float16 mixed16[16*72];
 {f8 acc{};uint row=g*64+wave*16;
  for(uint k=0;k<512;k+=16){h8 a{},b{};for(uint e=0;e<8;e++){a[e]=(_Float16)in[(first+rc())*512+k+gr()*8+e];b[e]=(_Float16)w[(row+rc())*512+k+gr()*8+e];}acc=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,acc);}
  for(uint e=0;e<8;e++)mixed16[(gr()*8+e)*72+wave*16+rc()]=(_Float16)F(Hrtz(acc[e]));
 }
 __builtin_amdgcn_s_barrier();
 f8 expanded[4]{};
 for(uint j=0;j<4;j++)for(uint k=0;k<64;k+=16){h8 a{},b{};for(uint e=0;e<8;e++)a[e]=mixed16[rc()*72+k+gr()*8+e];__builtin_memcpy(&b,reinterpret_cast<const _Float16*>(w+262144)+g*16384+(wave*64+j*16+rc())*64+k+gr()*8,16);expanded[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,expanded[j]);}
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++){float v=Hrtz(expanded[j][e]),gate=clampf(v,-4.f,4.f),poly=gate*(absf(gate)*(-.055908203125f)+.447265625f)+.89453125f;hidden[(gr()*8+e)*260+wave*64+j*16+rc()]=byte_F(v*poly);}
 __builtin_amdgcn_s_barrier();
 f8 result{};
 const uint*pw=reinterpret_cast<const uint*>(w);
 for(uint k=0;k<256;k+=16){i2 a{};__builtin_memcpy(&a,hidden+rc()*260+k+gr()*8,8);uint off=393216+g*4096+((wave*16+rc())*256+k+gr()*8)/4;i2 b={int(pw[off]),int(pw[off+1])};result=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,result);}
 for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+g*64+wave*16+rc()]=F(Hrtz(result[e]));
}

WAVE void vit_qkv_project_normalize_fused_f16weight(const float*in,const float*w,float*out,uint tokens){
 uint perpart=(tokens/16)*32,part=bid()/perpart,local=bid()%perpart,first=local/32*16,row=local%32*32;if(part>=3)return;f8 acc[2]{};
 for(uint k=0;k<1024;k+=16){h8 a{};for(uint e=0;e<8;e++)a[e]=(_Float16)in[(first+rc())*1024+k+gr()*8+e];for(uint j=0;j<2;j++){h8 b{};__builtin_memcpy(&b,reinterpret_cast<const _Float16*>(w)+part*1048576+(row+j*16+rc())*1024+k+gr()*8,16);acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,acc[j]);}}
 __attribute__((shared)) float raw[16*33];
 f8 sum{};
 if(part<2){
  for(uint j=0;j<2;j++)for(uint e=0;e<8;e++)raw[(gr()*8+e)*33+j*16+rc()]=acc[j][e];
  __builtin_amdgcn_s_barrier();
  for(uint k=0;k<32;k+=16){h8 x{},ones{};for(uint e=0;e<8;e++){float v=raw[rc()*33+k+gr()*8+e];x[e]=(_Float16)(v*v);ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,ones,sum);}
 }
 for(uint j=0;j<2;j++)for(uint e=0;e<8;e++){float v=acc[j][e];if(part<2){float scale=part==0?5.65625f*w[3145728+row/32]:1.f;v*=__builtin_amdgcn_rsqf(maxf(sum[e],6.198883056640625e-5f))*scale;}out[(part*tokens+first+gr()*8+e)*1024+row+j*16+rc()]=F(v);}
}

// Byte-output twin of vit_qkv_project_normalize_fused_f16compact: identical projection/normalize, the F() result stored
// as its E4M3 byte (byte_F: exact on the F() lattice, same byte pack() would produce from the f32 value).
WAVE void vit_qkv_project_normalize_fused_f16compact_fp8(const float*in,const float*w,unsigned char*out,uint tokens){
 uint perpart=(tokens/16)*32,part=bid()/perpart,local=bid()%perpart,first=local/32*16,row=local%32*32;if(part>=3)return;f8 acc[2]{};
 for(uint k=0;k<1024;k+=16){h8 a{};for(uint e=0;e<8;e++)a[e]=(_Float16)in[(first+rc())*1024+k+gr()*8+e];for(uint j=0;j<2;j++){h8 b{};__builtin_memcpy(&b,reinterpret_cast<const _Float16*>(w)+part*1048576+(row+j*16+rc())*1024+k+gr()*8,16);acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,acc[j]);}}
 __attribute__((shared)) float raw[16*33];
 f8 sum{};
 if(part<2){
  for(uint j=0;j<2;j++)for(uint e=0;e<8;e++)raw[(gr()*8+e)*33+j*16+rc()]=acc[j][e];
  __builtin_amdgcn_s_barrier();
  for(uint k=0;k<32;k+=16){h8 x{},ones{};for(uint e=0;e<8;e++){float v=raw[rc()*33+k+gr()*8+e];x[e]=(_Float16)(v*v);ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,ones,sum);}
 }
 for(uint j=0;j<2;j++)for(uint e=0;e<8;e++){float v=acc[j][e];if(part<2){float scale=part==0?5.65625f*w[1572864+row/32]:1.f;v*=__builtin_amdgcn_rsqf(maxf(sum[e],6.198883056640625e-5f))*scale;}out[(part*tokens+first+gr()*8+e)*1024+row+j*16+rc()]=byte_F(v);}
}
// Fragment-weight twins (host options vit_proj_frag / vit_qkv_frag): identical operands and accumulation order to
// vit_project / vit_qkv_project_normalize_fused_f16compact_fp8, but every B fragment is one contiguous load from the
// prepacked tile layout (FragmentPackedMatrix for the E4M3 projection, 16x16 f16 tiles for the QKV halves) instead of
// 16 lanes touching 16 different weight rows.
DEV f8 dot8_frag(const float*in,const unsigned char*wf,uint first,uint row,uint channels,uint begin,uint end,f8 a){
 for(uint k=begin;k<end;k+=16){i2 x{},y{};
  for(uint e=0;e<8;e++)pack(x,e,in[(first+rc())*channels+k+gr()*8+e]);
  __builtin_memcpy(&y,wf+((row/16)*(channels/32)+k/32)*512+((((k%32)/16)*2+gr())*16+rc())*8,8);
  a=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(x,y,a);
 }return a;
}
WAVE void vit_project_frag(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs){uint first=bid()/(outputs/16)*16,row=bid()%(outputs/16)*16;if(first>=tokens)return;f8 total{};
 const unsigned char*wf=reinterpret_cast<const unsigned char*>(w);
#if HIP_VIT_HOIST_SCALE
 const float rs=w[inputs*outputs+row+rc()]; // residual scale once per lane (was re-read per element: 7 identical loads in the ISA)
 if(inputs==4096){for(uint e=0;e<8;e++)total[e]=H(skip[(first+gr()*8+e)*outputs+row+rc()]*rs);for(uint p=0;p<4;p++){f8 a{};a=dot8_frag(in,wf,first,row,inputs,p*(inputs/4),(p+1)*(inputs/4),a);for(uint e=0;e<8;e++)total[e]+=a[e];}}
 else{for(uint p=0;p<4;p++){f8 a{};if(!p)for(uint e=0;e<8;e++)a[e]=H(skip[(first+gr()*8+e)*outputs+row+rc()]*rs);a=dot8_frag(in,wf,first,row,inputs,p*(inputs/4),(p+1)*(inputs/4),a);for(uint e=0;e<8;e++)total[e]=p?total[e]+a[e]:a[e];}}
#else
 if(inputs==4096){for(uint e=0;e<8;e++)total[e]=H(skip[(first+gr()*8+e)*outputs+row+rc()]*w[inputs*outputs+row+rc()]);for(uint p=0;p<4;p++){f8 a{};a=dot8_frag(in,wf,first,row,inputs,p*(inputs/4),(p+1)*(inputs/4),a);for(uint e=0;e<8;e++)total[e]+=a[e];}}
 else{for(uint p=0;p<4;p++){f8 a{};if(!p)for(uint e=0;e<8;e++)a[e]=H(skip[(first+gr()*8+e)*outputs+row+rc()]*w[inputs*outputs+row+rc()]);a=dot8_frag(in,wf,first,row,inputs,p*(inputs/4),(p+1)*(inputs/4),a);for(uint e=0;e<8;e++)total[e]=p?total[e]+a[e]:a[e];}}
#endif
 for(uint e=0;e<8;e++)out[(first+gr()*8+e)*outputs+row+rc()]=F(H(total[e]));
}
WAVE void vit_qkv_project_normalize_fused_f16compact_fp8_frag(const float*in,const float*w,unsigned char*out,uint tokens){
 uint perpart=(tokens/16)*32,part=bid()/perpart,local=bid()%perpart,first=local/32*16,row=local%32*32;if(part>=3)return;f8 acc[2]{};
 const _Float16*w16=reinterpret_cast<const _Float16*>(w)+part*1048576;
 for(uint k=0;k<1024;k+=16){h8 a{};for(uint e=0;e<8;e++)a[e]=(_Float16)in[(first+rc())*1024+k+gr()*8+e];for(uint j=0;j<2;j++){h8 b{};__builtin_memcpy(&b,w16+(((row+j*16)/16)*64+k/16)*256+(gr()*16+rc())*8,16);acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,acc[j]);}}
 __attribute__((shared)) float raw[16*33];
 f8 sum{};
 if(part<2){
  for(uint j=0;j<2;j++)for(uint e=0;e<8;e++)raw[(gr()*8+e)*33+j*16+rc()]=acc[j][e];
  __builtin_amdgcn_s_barrier();
  for(uint k=0;k<32;k+=16){h8 x{},ones{};for(uint e=0;e<8;e++){float v=raw[rc()*33+k+gr()*8+e];x[e]=(_Float16)(v*v);ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,ones,sum);}
 }
#if HIP_VIT_HOIST_SCALE
 // The per-row scale read once (uniform); inside the loop it was re-read after every out[] store (aliasing), 16 global_load + wait pairs.
 const float scale=part==0?5.65625f*w[1572864+row/32]:1.f;
 for(uint j=0;j<2;j++)for(uint e=0;e<8;e++){float v=acc[j][e];if(part<2){v*=__builtin_amdgcn_rsqf(maxf(sum[e],6.198883056640625e-5f))*scale;}out[(part*tokens+first+gr()*8+e)*1024+row+j*16+rc()]=byte_F(v);}
#else
 for(uint j=0;j<2;j++)for(uint e=0;e<8;e++){float v=acc[j][e];if(part<2){float scale=part==0?5.65625f*w[1572864+row/32]:1.f;v*=__builtin_amdgcn_rsqf(maxf(sum[e],6.198883056640625e-5f))*scale;}out[(part*tokens+first+gr()*8+e)*1024+row+j*16+rc()]=byte_F(v);}
#endif
}
// Byte-input twin: the contract output arrives as E4M3 bytes (vit_contract_blocked_fp8*_bstream); each A fragment is two
// dword loads decoded to the same half values (E4M3 is exact in F16) the f32 kernel converts from.
WAVE void vit_qkv_project_normalize_fused_f16compact_fp8_bytein(const float*in,const float*w,unsigned char*out,uint tokens){
 uint perpart=(tokens/16)*32,part=bid()/perpart,local=bid()%perpart,first=local/32*16,row=local%32*32;if(part>=3)return;f8 acc[2]{};
 const uint*ip=reinterpret_cast<const uint*>(in);
 for(uint k=0;k<1024;k+=16){h8 a{};uint off=((first+rc())*1024+k+gr()*8)/4,w0=ip[off],w1=ip[off+1];
  a[0]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int(w0),0);a[1]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int(w0),1);a[2]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int(w0),2);a[3]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int(w0),3);
  a[4]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int(w1),0);a[5]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int(w1),1);a[6]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int(w1),2);a[7]=(_Float16)__builtin_amdgcn_cvt_f32_fp8(int(w1),3);
  for(uint j=0;j<2;j++){h8 b{};__builtin_memcpy(&b,reinterpret_cast<const _Float16*>(w)+part*1048576+(row+j*16+rc())*1024+k+gr()*8,16);acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,acc[j]);}}
 __attribute__((shared)) float raw[16*33];
 f8 sum{};
 if(part<2){
  for(uint j=0;j<2;j++)for(uint e=0;e<8;e++)raw[(gr()*8+e)*33+j*16+rc()]=acc[j][e];
  __builtin_amdgcn_s_barrier();
  for(uint k=0;k<32;k+=16){h8 x{},ones{};for(uint e=0;e<8;e++){float v=raw[rc()*33+k+gr()*8+e];x[e]=(_Float16)(v*v);ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,ones,sum);}
 }
 for(uint j=0;j<2;j++)for(uint e=0;e<8;e++){float v=acc[j][e];if(part<2){float scale=part==0?5.65625f*w[1572864+row/32]:1.f;v*=__builtin_amdgcn_rsqf(maxf(sum[e],6.198883056640625e-5f))*scale;}out[(part*tokens+first+gr()*8+e)*1024+row+j*16+rc()]=byte_F(v);}
}
// Half-input QKV: the contract output arrives as F16 (vit_contract_blocked_fp8*_hstream); one 16-byte load per A
// fragment, no conversion. N column fragments per wave (N=4: 64 columns = two heads, two square-sum MMAs). Same MMA
// operands, accumulation order, normalization and byte store as the f32-input kernel.
template<uint N>
DEV void vit_qkv_h16in_body(const float*in,const float*w,unsigned char*out,uint tokens){
 const uint cols=N*16,groups=1024/cols;uint perpart=(tokens/16)*groups,part=bid()/perpart,local=bid()%perpart,first=local/groups*16,row=local%groups*cols;if(part>=3)return;f8 acc[N]{};
 for(uint k=0;k<1024;k+=16){h8 a{};__builtin_memcpy(&a,reinterpret_cast<const _Float16*>(in)+(first+rc())*1024+k+gr()*8,16);for(uint j=0;j<N;j++){h8 b{};__builtin_memcpy(&b,reinterpret_cast<const _Float16*>(w)+part*1048576+(row+j*16+rc())*1024+k+gr()*8,16);acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,acc[j]);}}
 __attribute__((shared)) float raw[16*(N*16+1)];
 f8 sum[N/2]{};
 if(part<2){
  for(uint j=0;j<N;j++)for(uint e=0;e<8;e++)raw[(gr()*8+e)*(N*16+1)+j*16+rc()]=acc[j][e];
  __builtin_amdgcn_s_barrier();
  for(uint h=0;h<N/2;h++)for(uint k=0;k<32;k+=16){h8 x{},ones{};for(uint e=0;e<8;e++){float v=raw[rc()*(N*16+1)+h*32+k+gr()*8+e];x[e]=(_Float16)(v*v);ones[e]=(_Float16)1.f;}sum[h]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,ones,sum[h]);}
 }
 for(uint j=0;j<N;j++)for(uint e=0;e<8;e++){float v=acc[j][e];if(part<2){uint h=j/2;float scale=part==0?5.65625f*w[1572864+row/32+h]:1.f;v*=__builtin_amdgcn_rsqf(maxf(sum[h][e],6.198883056640625e-5f))*scale;}out[(part*tokens+first+gr()*8+e)*1024+row+j*16+rc()]=byte_F(v);}
}
WAVE void vit_qkv_project_normalize_fused_f16compact_fp8_h16in(const float*in,const float*w,unsigned char*out,uint tokens){vit_qkv_h16in_body<2>(in,w,out,tokens);}
WAVE void vit_qkv_project_normalize_fused_f16compact_fp8_h16in_n4(const float*in,const float*w,unsigned char*out,uint tokens){vit_qkv_h16in_body<4>(in,w,out,tokens);}
WAVE void vit_qkv_project_normalize_fused_f16compact(const float*in,const float*w,float*out,uint tokens){
 uint perpart=(tokens/16)*32,part=bid()/perpart,local=bid()%perpart,first=local/32*16,row=local%32*32;if(part>=3)return;f8 acc[2]{};
 for(uint k=0;k<1024;k+=16){h8 a{};for(uint e=0;e<8;e++)a[e]=(_Float16)in[(first+rc())*1024+k+gr()*8+e];for(uint j=0;j<2;j++){h8 b{};__builtin_memcpy(&b,reinterpret_cast<const _Float16*>(w)+part*1048576+(row+j*16+rc())*1024+k+gr()*8,16);acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,acc[j]);}}
 __attribute__((shared)) float raw[16*33];
 f8 sum{};
 if(part<2){
  for(uint j=0;j<2;j++)for(uint e=0;e<8;e++)raw[(gr()*8+e)*33+j*16+rc()]=acc[j][e];
  __builtin_amdgcn_s_barrier();
  for(uint k=0;k<32;k+=16){h8 x{},ones{};for(uint e=0;e<8;e++){float v=raw[rc()*33+k+gr()*8+e];x[e]=(_Float16)(v*v);ones[e]=(_Float16)1.f;}sum=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(x,ones,sum);}
 }
 for(uint j=0;j<2;j++)for(uint e=0;e<8;e++){float v=acc[j][e];if(part<2){float scale=part==0?5.65625f*w[1572864+row/32]:1.f;v*=__builtin_amdgcn_rsqf(maxf(sum[e],6.198883056640625e-5f))*scale;}out[(part*tokens+first+gr()*8+e)*1024+row+j*16+rc()]=F(v);}
}
