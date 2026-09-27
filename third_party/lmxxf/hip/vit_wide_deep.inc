// 2026-09-26 (results/m32-sweep-20260926): ViT projection with one wave owning 16 tokens x 64 columns (four 16-column B tiles).
// Production vit_project_frag (inputs = outputs = 1024) owns 16 x 16: every K16 step converts eight f32 inputs to E4M3 for one WMMA;
// here the converted A fragment feeds four. Per output: quarter p accumulates from 0 (p = 0 from H(skip * scale)) over the same
// K order, total = p ? total + acc : acc, F(H(total)) stored -- the production arithmetic (HIP_VIT_HOIST_SCALE 0) unchanged.
// This is the measured m32-sweep template text (instantiated TM=1, TN=4 only); a hand-specialised rewrite compiled to different
// code (98 vs 160 VGPR) and gave back more than half of the gain, so keep the template form.
// Host option vit_proj_n64 (DLSS5_HIP_VIT_PROJ_N64=1), module vit-wide-deep.hsaco.
template<uint TM,uint TN>
DEV void vit_project_frag_tiles(const float*in,const float*w,const float*skip,float*out,uint tokens,const uint*reuse_gate){if(reuse_gate&&reuse_gate[0])return;
 constexpr uint IN=1024,OUT=1024;
 uint first=bid()/(OUT/(16*TN))*(16*TM),row=bid()%(OUT/(16*TN))*(16*TN);if(first>=tokens)return;
 uint live=0;
#pragma unroll
 for(uint t=0;t<TM;t++)live+=first+t*16<tokens;
 const unsigned char*wf=reinterpret_cast<const unsigned char*>(w);
 f8 total[TM][TN]{};
 for(uint p=0;p<4;p++){f8 a[TM][TN]{};
  if(!p)
#pragma unroll
  for(uint t=0;t<TM;t++)for(uint j=0;j<TN;j++)if(t<live)for(uint e=0;e<8;e++)a[t][j][e]=H(skip[(first+t*16+gr()*8+e)*OUT+row+j*16+rc()]*w[IN*OUT+row+j*16+rc()]);
  for(uint k=p*(IN/4);k<(p+1)*(IN/4);k+=16){i2 x[TM];
#pragma unroll
   for(uint t=0;t<TM;t++){x[t]=i2{};if(t<live)DF_PACK8_LOOP(x[t],in[(first+t*16+rc())*IN+k+gr()*8+e]);}
#pragma unroll
   for(uint j=0;j<TN;j++){uint r0=row+j*16;i2 y;__builtin_memcpy(&y,wf+((r0/16)*(IN/32)+k/32)*512+((((k%32)/16)*2+gr())*16+rc())*8,8);
#pragma unroll
    for(uint t=0;t<TM;t++)a[t][j]=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(x[t],y,a[t][j]);}}
#pragma unroll
  for(uint t=0;t<TM;t++)
#pragma unroll
  for(uint j=0;j<TN;j++)for(uint e=0;e<8;e++)total[t][j][e]=p?total[t][j][e]+a[t][j][e]:a[t][j][e];}
#pragma unroll
 for(uint t=0;t<TM;t++){if(t>=live)continue;for(uint j=0;j<TN;j++)for(uint e=0;e<8;e++)out[(first+t*16+gr()*8+e)*OUT+row+j*16+rc()]=F(H(total[t][j][e]));}
}
WAVE void vit_project_frag_n64(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs,const uint*reuse_gate){vit_project_frag_tiles<1,4>(in,w,skip,out,tokens,reuse_gate);}
