// C512 split mix (half weights), 32 tokens per wave (opt-in DLSS5_HIP_C512_M32, module c512-m32-deep): each half B
// fragment feeds two WMMAs; per-output K order and F(Hrtz()) epilogue as split_mix_blocked_h16w. results/c512-ffn-20260926.
#ifndef C512_ZERO_PAD_900
#define C512_ZERO_PAD_900 0
#endif
#if C512_ZERO_PAD_900
using c512_u4=uint __attribute__((ext_vector_type(4)));
DEV uint c512_wave_or(uint x){uint lane=__builtin_amdgcn_workitem_id_x();for(uint d=16;d;d>>=1)x|=uint(__builtin_amdgcn_ds_bpermute((lane^d)*4,int(x)));
#if C512_ZERO_PAD_900 == 2
 return __builtin_amdgcn_readfirstlane(x);
#else
 return x;
#endif
}
DEV bool c512_zero_tile(const float*in,const _Float16*w,uint first,uint row){
 uint lane=__builtin_amdgcn_workitem_id_x(),bits_or=0;c512_u4 v;
 // Cheap rejection, then prove every scalar is +/-zero; no assumption about real image pixels.
 __builtin_memcpy(&v,in+first*512+lane*4,16);bits_or=v[0]|v[1]|v[2]|v[3];
 if(c512_wave_or(bits_or&0x7fffffffu))return false;
 _Pragma("clang loop unroll(disable)") for(uint q=32+lane;q<4096;q+=32){__builtin_memcpy(&v,in+first*512+q*4,16);bits_or|=v[0]|v[1]|v[2]|v[3];}
 if(c512_wave_or(bits_or&0x7fffffffu))return false;
 // Zero*Inf/NaN is not zero. Check the actual 64x512 half-weight tile before bypassing the WMMA.
 uint bad=0;
 _Pragma("clang loop unroll(disable)") for(uint q=lane;q<4096;q+=32){__builtin_memcpy(&v,w+row*512+q*8,16);for(uint j=0;j<4;j++)bad|=((v[j]&0x7c007c00u)+0x04000400u)&0x80008000u;}
 return c512_wave_or(bad)==0;
}
#endif
WAVE void split_mix_blocked_h16w_m32(const float*in,const float*w,float*out,uint tokens){
 uint first=bid()/8*32,row=bid()%8*64;if(first>=tokens)return;const bool two=first+16<tokens;f8 acc0[4]{},acc1[4]{};const _Float16*w16=reinterpret_cast<const _Float16*>(w);
#if C512_ZERO_PAD_900
 const bool suspect=(tokens==1792&&first>=1680)||(tokens==2240&&(first+32<=224||first>=1904));
 if(two&&suspect&&c512_zero_tile(in,w16,first,row)){
  for(uint t=0;t<2;t++)for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+t*16+gr()*8+e)*512+row+j*16+rc()]=0.f;
  return;
 }
#endif
 for(uint k=0;k<512;k+=16){h8 a0{},a1{};for(uint e=0;e<8;e++)a0[e]=(_Float16)in[(first+rc())*512+k+gr()*8+e];if(two)for(uint e=0;e<8;e++)a1[e]=(_Float16)in[(first+16+rc())*512+k+gr()*8+e];
  for(uint j=0;j<4;j++){h8 b;__builtin_memcpy(&b,w16+(row+j*16+rc())*512+k+gr()*8,16);acc0[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a0,b,acc0[j]);acc1[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a1,b,acc1[j]);}}
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+row+j*16+rc()]=F(Hrtz(acc0[j][e]));
 if(two)for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+16+gr()*8+e)*512+row+j*16+rc()]=F(Hrtz(acc1[j][e]));
}
