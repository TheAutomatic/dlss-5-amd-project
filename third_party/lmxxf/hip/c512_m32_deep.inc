// C512 split mix (half weights), 32 tokens per wave (opt-in DLSS5_HIP_C512_M32, module c512-m32-deep): each half B
// fragment feeds two WMMAs; per-output K order and F(Hrtz()) epilogue as split_mix_blocked_h16w. results/c512-ffn-20260926.
WAVE void split_mix_blocked_h16w_m32(const float*in,const float*w,float*out,uint tokens){
 uint first=bid()/8*32,row=bid()%8*64;if(first>=tokens)return;const bool two=first+16<tokens;f8 acc0[4]{},acc1[4]{};const _Float16*w16=reinterpret_cast<const _Float16*>(w);
 for(uint k=0;k<512;k+=16){h8 a0{},a1{};for(uint e=0;e<8;e++)a0[e]=(_Float16)in[(first+rc())*512+k+gr()*8+e];if(two)for(uint e=0;e<8;e++)a1[e]=(_Float16)in[(first+16+rc())*512+k+gr()*8+e];
  for(uint j=0;j<4;j++){h8 b;__builtin_memcpy(&b,w16+(row+j*16+rc())*512+k+gr()*8,16);acc0[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a0,b,acc0[j]);acc1[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a1,b,acc1[j]);}}
 for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+gr()*8+e)*512+row+j*16+rc()]=F(Hrtz(acc0[j][e]));
 if(two)for(uint j=0;j<4;j++)for(uint e=0;e<8;e++)out[(first+16+gr()*8+e)*512+row+j*16+rc()]=F(Hrtz(acc1[j][e]));
}
