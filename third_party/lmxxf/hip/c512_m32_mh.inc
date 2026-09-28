// C512 QKV+normalize, 32 tokens per wave (opt-in DLSS5_HIP_C512_M32, module c512-m32-mh; results/c512-ffn-20260926).
// mh_qkv_normalize_frag_c512 re-reads its 64 weight columns for every 16 tokens (one B fragment per WMMA); here one wave
// owns two 16-token A tiles, so each B fragment feeds two WMMAs and weight L2 traffic halves. Per-output K order, row
// sums, inv and q8(F()) are unchanged; the byte epilogue goes through a 16x64 LDS tile and two 128-bit stores per lane
// (production: 32 global_store_b8). Output identical by construction and verified bit-exact.
using c5u4=unsigned __attribute__((ext_vector_type(4)));
FASTWAVE void mh_qkv_normalize_frag_c512_m32(const unsigned char*in8,const float*w,unsigned char*out,uint tokens){
 const uint C=512,N=1536;uint l=__builtin_amdgcn_workitem_id_x(),rc=l&15,g=l>>4,block=__builtin_amdgcn_workgroup_id_x(),first=block/24*32,col=block%24*64;if(first>=tokens)return;
 const bool two=first+16<tokens;
 const unsigned char*wb=reinterpret_cast<const unsigned char*>(w);f8 acc0[4]{},acc1[4]{};
 for(uint gs=0;gs<16;gs++){const unsigned char*t0=in8+((first/16)*16+gs)*512+rc*32+g*8,*t1=t0+16*512;
  for(uint h=0;h<2;h++){i2 a0,a1{};__builtin_memcpy(&a0,t0+h*16,8);if(two)__builtin_memcpy(&a1,t1+h*16,8);
   for(uint n=0;n<4;n++){uint n0=col+n*16+rc;i2 b;__builtin_memcpy(&b,wb+((n0/16)*16+gs)*512+((h*2+g)*16+n0%16)*8,8);
    acc0[n]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a0,b,acc0[n]);acc1[n]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a1,b,acc1[n]);}}}
 __attribute__((shared)) float raw[17*65];
 __attribute__((shared)) c5u4 tile[16*4];
 unsigned char*tb=reinterpret_cast<unsigned char*>(tile);
 uint part=col/C,head0=(col%C)/32,scale=4*C*C+(C/32)*4096;
 for(uint t=0;t<2;t++){if(t&&!two)break;const uint base=first+t*16;f8*acc=t?acc1:acc0;
  if(t){WG_FENCE(3);__builtin_amdgcn_s_barrier();WG_FENCE(2);}
  for(uint n=0;n<4;n++)for(uint e=0;e<8;e++)raw[(g*8+e)*65+n*16+rc]=acc[n][e];
  WG_FENCE(3);__builtin_amdgcn_s_barrier();WG_FENCE(2);
  {uint row=l%16,h=l/16;float inv=1.f;if(base+row<tokens&&part<2){float ss=0.f;for(uint j=0;j<32;j++){float v=raw[row*65+h*32+j];ss+=v*v;}inv=__builtin_amdgcn_rsqf(maxf(ss,6.198883056640625e-5f))*(part==0?w[scale+head0+h]:1.f);}raw[16*65+l]=inv;}
  WG_FENCE(3);__builtin_amdgcn_s_barrier();WG_FENCE(2);
  for(uint n=0;n<4;n++)for(uint e=0;e<8;e++){uint row=g*8+e;tb[row*64+n*16+rc]=static_cast<unsigned char>(q8(F(acc[n][e]*raw[16*65+(n/2)*16+row])));}
  WG_FENCE(3);__builtin_amdgcn_s_barrier();WG_FENCE(2);
  {uint row=l>>1,half=l&1,p=base+row;if(p<tokens){c5u4*d=reinterpret_cast<c5u4*>(out+size_t(p)*N+col+half*32);d[0]=tile[row*4+half*2];d[1]=tile[row*4+half*2+1];}}
 }
}
