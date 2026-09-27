// Input is production [raster pixel][Q,K,V][channel] FP8, feature is FP8.
// Keep original FFN/QKV and PDL handoff. One wave owns one complete head.
#ifndef W2_DIRECT_FEATURE
#define W2_DIRECT_FEATURE 0
#endif
template<uint C,bool ByteOut>
DEV void attention_wave_body(const unsigned char*normalized,const float*aw,const unsigned char*feature,void*out_any,
 uint width,uint height,uint workw,uint workh,uint sx,uint sy,uint post,const uint*fflags,uint fepoch){
 static_assert(!W2_DEFER_Q,"attention-only requires stored production Q");
 const uint win=__builtin_amdgcn_workgroup_id_x(),tid=__builtin_amdgcn_workitem_id_x(),head=tid/32;
 const uint rc=w2_rc(),gr=w2_gr();
 if(win>=(workw/8)*(workh/8))return;
 if(fflags){if(tid<64){uint p=w2_pixel(win,tid,workw);while(__hip_atomic_load(fflags+p/16,__ATOMIC_RELAXED,__HIP_MEMORY_SCOPE_AGENT)<fepoch)__builtin_amdgcn_s_sleep(1);}__builtin_amdgcn_s_barrier();}
 __attribute__((shared)) unsigned char plane1[64*C];
#if !W2_DIRECT_FEATURE
 __attribute__((shared)) unsigned char plane0[64*C];
#endif
 // V transpose uses the AV plane before attention; feature stays in plane0.
 for(uint qt=0;qt<4;qt++)for(uint ci=0;ci<2;ci++){
  uint p=w2_pixel(win,qt*16+rc,workw),col=head*32+ci*16+gr*8;i2 f,v;
#if !W2_DIRECT_FEATURE
  __builtin_memcpy(&f,feature+p*C+col,8);w2_store<C>(plane0,qt,head*2+ci,f);
#endif
  __builtin_memcpy(&v,normalized+(p*3+2)*C+col,8);w2_store<C>(plane1,qt,head*2+ci,v);
 }
 w2_sync();
 i2 qkv[3][4][2];
#if W2_ROLL_QUERY
 w2_i16 query_words{};
#endif
 _Pragma("unroll 4") for(uint qt=0;qt<4;qt++){
  _Pragma("unroll 2") for(uint ci=0;ci<2;ci++){
   uint p=w2_pixel(win,qt*16+rc,workw),col=head*32+ci*16+gr*8;i2 q,k,v{};
   __builtin_memcpy(&q,normalized+p*3*C+col,8);__builtin_memcpy(&k,normalized+(p*3+1)*C+col,8);
   for(uint e=0;e<8;e++){
    uint token=gr*8+e,channel=ci*16+rc;
    uint lane=token+(channel%16/8)*16,ct=head*2+channel/16;
    w2_bits(v,e,plane1[((qt*(C/16)+ct)*32+lane)*8+channel%8]);
   }
#if W2_ROLL_QUERY
   query_words[qt*4+ci*2]=q[0];query_words[qt*4+ci*2+1]=q[1];
#else
   qkv[0][qt][ci]=q;
#endif
   qkv[1][qt][ci]=k;qkv[2][qt][ci]=v;
  }
  w2_schedule();
 }
 w2_sync(); // Every head has consumed V before the plane becomes AV output.
