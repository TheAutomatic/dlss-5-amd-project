// C512 QKV/normalization/attention fusion. Appended to multihead_fast_padded.hip.
// The host supplies the same tiled FFN feature and fragment-packed QKV weights
// as mh_qkv_normalize_frag_c512_m32; AV remains raster FP8 for the old projection.
#ifndef C512_FUSED_QKV_ATTN
#define C512_FUSED_QKV_ATTN 0
#endif
#ifndef C512_FUSED_WAVES
#define C512_FUSED_WAVES 2
#endif
#ifndef C512_FUSED_DEBUG
#define C512_FUSED_DEBUG 0
#endif
#if C512_FUSED_QKV_ATTN
#if C512_FUSED_WAVES != 2 && C512_FUSED_WAVES != 4
#error C512_FUSED_WAVES must be 2 or 4
#endif
DEV uint c5f_pixel(uint win,uint tok,uint width){return ((win/(width/8))*8+tok/8)*width+(win%(width/8))*8+tok%8;}
DEV float c5f_shuffle(float v,uint src){return __builtin_bit_cast(float,__builtin_amdgcn_ds_bpermute(src*4,__builtin_bit_cast(int,v)));}
DEV float c5f_serial_norm(f8 lo,f8 hi){
 // Channel order is 0..7,8..15,16..23,24..31, as in the production raw-LDS loop.
 // Both half-waves execute, but only the correct prefix is carried to the next half.
 uint row=__builtin_amdgcn_workitem_id_x()&15u;float s=0.f;
 _Pragma("unroll 8") for(uint e=0;e<8;e++)s+=lo[e]*lo[e];
 s=c5f_shuffle(s,row);
 _Pragma("unroll 8") for(uint e=0;e<8;e++)s+=lo[e]*lo[e];
 s=c5f_shuffle(s,row+16);
 _Pragma("unroll 8") for(uint e=0;e<8;e++)s+=hi[e]*hi[e];
 s=c5f_shuffle(s,row);
 _Pragma("unroll 8") for(uint e=0;e<8;e++)s+=hi[e]*hi[e];
 return c5f_shuffle(s,row+16);
}
// Keep C512 attention's own clamp/F/byte semantics (not the C256 bounded reciprocal).
DEV float c5f_clamp(float a,float lo,float hi){float b=a>lo?a:lo;return b<hi?b:hi;}
DEV uint c5f_fp8(float x){return uint(__builtin_amdgcn_cvt_pk_fp8_f32(__builtin_fminf(__builtin_fmaxf(x,-448.f),448.f),0.f,0,false))&255u;}
DEV float c5f_F(float x){uint a=bits(x)&0x7fffffffu;float c=__builtin_fminf(__builtin_fmaxf(x,-448.f),448.f);float r=__builtin_amdgcn_cvt_f32_fp8(__builtin_amdgcn_cvt_pk_fp8_f32(c,0.f,0,false),0);return a==0?0.f:r;}
DEV void c5f_bits(i2&v,uint e,uint b){v[e/4]=int(uint(v[e/4])|((b&255u)<<(8*(e%4))));}
DEV i2 c5f_load(const unsigned char*p,uint row,uint col){i2 a;__builtin_memcpy(&a,p+row*32+col,8);return a;}

template<bool DebugNorm>
DEV void c5f_body(const unsigned char*in8,const float*w,unsigned char*out,uint width,uint height){
 constexpr uint C=512,NW=C512_FUSED_WAVES,QT=4/NW;
 uint tid=__builtin_amdgcn_workitem_id_x(),lane=tid&31u,row=lane&15u,half=lane>>4,wave=tid/32;
 uint wh=__builtin_amdgcn_workgroup_id_x(),windows=(width/8)*(height/8),win=wh/16,head=wh%16;
 if(width%8||height%8||wh>=windows*16)return;
 __attribute__((shared)) unsigned char packed[3*64*32];
 const unsigned char*wb=reinterpret_cast<const unsigned char*>(w);
 // Each wave owns QT independent 16-token tiles, sharing B-weight fragments.
 // Swapping WMMA operands transposes the result: lane row is now the token,
 // allowing serial channel normalization without a float scratch tile.
 _Pragma("clang loop unroll(disable)") for(uint part=0;part<3;part++){
  f8 acc[QT][2]{};
  for(uint gs=0;gs<16;gs++)for(uint h=0;h<2;h++){
   i2 a[QT];
   _Pragma("unroll") for(uint t=0;t<QT;t++){
    uint p=c5f_pixel(win,(wave*QT+t)*16+row,width);
    __builtin_memcpy(&a[t],in8+((p/16)*16+gs)*512+(p%16)*32+half*8+h*16,8);
   }
   _Pragma("unroll 2") for(uint ci=0;ci<2;ci++){
    uint n0=part*C+head*32+ci*16+row;i2 b;
    __builtin_memcpy(&b,wb+((n0/16)*16+gs)*512+((h*2+half)*16+n0%16)*8,8);
    _Pragma("unroll") for(uint t=0;t<QT;t++)acc[t][ci]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(b,a[t],acc[t][ci]);
   }
  }
  _Pragma("unroll") for(uint t=0;t<QT;t++){
   float inv=1.f;
   if(part<2){float ss=c5f_serial_norm(acc[t][0],acc[t][1]);inv=__builtin_amdgcn_rsqf(maxf(ss,6.198883056640625e-5f))*(part==0?w[4*C*C+16*4096+head]:1.f);}
   _Pragma("unroll 2") for(uint ci=0;ci<2;ci++){
    i2 a{};_Pragma("unroll 8") for(uint e=0;e<8;e++)c5f_bits(a,e,q8(F(acc[t][ci][e]*inv)));
    uint tok=(wave*QT+t)*16+row;__builtin_memcpy(packed+(part*64+tok)*32+ci*16+half*8,&a,8);
   }
  }
 }
 WG_FENCE(3);__builtin_amdgcn_s_barrier();WG_FENCE(2);
 if constexpr(DebugNorm){
  for(uint i=tid;i<3*64*32;i+=NW*32){uint part=i/(64*32),tok=(i/32)%64,col=i%32,p=c5f_pixel(win,tok,width);out[(p*3+part)*512+head*32+col]=packed[i];}
  return;
 }
 // Packed Q/K/V is now read-only. Each wave evaluates its own query tiles,
 // retaining ex/prob fragments in registers; no second workgroup barrier.
 _Pragma("clang loop unroll(disable)") for(uint t=0;t<QT;t++){
  uint first=(wave*QT+t)*16;
  h8 exponent[4];
  _Pragma("unroll 4") for(uint key=0;key<4;key++){
   f8 scores{};
   _Pragma("unroll 2") for(uint kt=0;kt<2;kt++){
    i2 q=c5f_load(packed,first+row,kt*16+half*8);
    i2 k=c5f_load(packed,64+key*16+row,kt*16+half*8);
    scores=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(k,q,scores);
   }
   h8 ex{};
   _Pragma("unroll 8") for(uint e=0;e<8;e++){
    uint q=first+row,k=key*16+half*8+e;
    float score=scores[e]+w[4*C*C+head*4096+q*64+k];
    float affine=c5f_clamp(score*.044921875f+1.30078125f,1.03125f,1.5693359375f);
    uint ah=(bits(affine)>>13)-0x1c000u;
    unsigned short hb=static_cast<unsigned short>(((ah<<5)+0x8000u)&65535u);ex[e]=__builtin_bit_cast(_Float16,hb);
   }
   exponent[key]=ex;
  }
  h8 ones{};_Pragma("unroll 8") for(uint e=0;e<8;e++)ones[e]=(_Float16)1.f;
  f8 sums[2]{};
  _Pragma("unroll 2") for(uint side=0;side<2;side++){
   _Pragma("unroll 2") for(uint h=0;h<2;h++)sums[side]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(ones,exponent[side+2*h],sums[side]);
  }
  i2 prob[4];
  _Pragma("unroll 4") for(uint key=0;key<4;key++){
   i2 a{};_Pragma("unroll 8") for(uint e=0;e<8;e++){float inv=1.f/(sums[0][e]+sums[1][e]);c5f_bits(a,e,c5f_fp8(float(exponent[key][e])*inv));}prob[key]=a;
  }
  _Pragma("unroll 2") for(uint ci=0;ci<2;ci++){
   f8 av{};
   _Pragma("unroll 4") for(uint kt=0;kt<4;kt++){
    i2 v{};_Pragma("unroll 8") for(uint e=0;e<8;e++)c5f_bits(v,e,packed[(128+kt*16+half*8+e)*32+ci*16+row]);
    av=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(v,prob[kt],av);
   }
   i2 bytes{};_Pragma("unroll 8") for(uint e=0;e<8;e++)c5f_bits(bytes,e,c5f_fp8(c5f_F(av[e])));
   uint p=c5f_pixel(win,first+row,width);__builtin_memcpy(out+p*512+head*32+ci*16+half*8,&bytes,8);
  }
 }
}
KERNEL __attribute__((amdgpu_flat_work_group_size(C512_FUSED_WAVES*32,C512_FUSED_WAVES*32)))
void c512_qkv_attention_fused(const unsigned char*in8,const float*w,unsigned char*out,uint width,uint height){c5f_body<false>(in8,w,out,width,height);}
#if C512_FUSED_DEBUG
KERNEL __attribute__((amdgpu_flat_work_group_size(C512_FUSED_WAVES*32,C512_FUSED_WAVES*32)))
void c512_qkv_fused_debug(const unsigned char*in8,const float*w,unsigned char*out,uint width,uint height){c5f_body<true>(in8,w,out,width,height);}
#endif
#endif
