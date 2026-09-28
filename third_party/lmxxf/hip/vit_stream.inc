// Explicit typed ViT edges, same reductions and rounding as the production fragment kernels.
#ifndef HIP_VIT_STREAM_KERNELS
#define HIP_VIT_STREAM_KERNELS 0
#endif
#ifndef HIP_VIT_STREAM_QKV_UNROLL
#define HIP_VIT_STREAM_QKV_UNROLL 0
#endif
#if HIP_VIT_STREAM_KERNELS
template<uint TM,uint TN,bool ByteAv,bool HalfSkip>
DEV void vit_stream_project_tiles(const float*in,const float*w,const float*skip,float*out,uint tokens,const uint*reuse_gate){if(reuse_gate&&reuse_gate[0])return;
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
  for(uint t=0;t<TM;t++)for(uint j=0;j<TN;j++)if(t<live)for(uint e=0;e<8;e++)a[t][j][e]=H((HalfSkip?float(reinterpret_cast<const _Float16*>(skip)[(first+t*16+gr()*8+e)*OUT+row+j*16+rc()]):skip[(first+t*16+gr()*8+e)*OUT+row+j*16+rc()])*w[IN*OUT+row+j*16+rc()]);
  for(uint k=p*(IN/4);k<(p+1)*(IN/4);k+=16){i2 x[TM];
#pragma unroll
   for(uint t=0;t<TM;t++){x[t]=i2{};if(t<live){if constexpr(ByteAv)__builtin_memcpy(&x[t],reinterpret_cast<const unsigned char*>(in)+(first+t*16+rc())*IN+k+gr()*8,8);else {DF_PACK8_LOOP(x[t],in[(first+t*16+rc())*IN+k+gr()*8+e]);}}}
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
WAVE void vit_stream_project_n64_b(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs,const uint*reuse_gate){vit_stream_project_tiles<1,4,true,false>(in,w,skip,out,tokens,reuse_gate);}
WAVE void vit_stream_project_n64_h(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs,const uint*reuse_gate){vit_stream_project_tiles<1,4,false,true>(in,w,skip,out,tokens,reuse_gate);}
WAVE void vit_stream_project_n64_bh(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs,const uint*reuse_gate){vit_stream_project_tiles<1,4,true,true>(in,w,skip,out,tokens,reuse_gate);}
WAVE void vit_stream_qkv_frag_hin(const float*in,const float*w,unsigned char*out,uint tokens,const uint*reuse_gate){if(reuse_gate&&reuse_gate[0])return;
 uint perpart=(tokens/16)*32,part=bid()/perpart,local=bid()%perpart,first=local/32*16,row=local%32*32;if(part>=3)return;f8 acc[2]{};
 const _Float16*w16=reinterpret_cast<const _Float16*>(w)+part*1048576;
#if HIP_VIT_STREAM_QKV_UNROLL == 2
 _Pragma("unroll 2")
#endif
 for(uint k=0;k<1024;k+=16){h8 a{};__builtin_memcpy(&a,reinterpret_cast<const _Float16*>(in)+(first+rc())*1024+k+gr()*8,16);for(uint j=0;j<2;j++){h8 b{};__builtin_memcpy(&b,w16+(((row+j*16)/16)*64+k/16)*256+(gr()*16+rc())*8,16);acc[j]=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a,b,acc[j]);}}
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

WAVE void vit_stream_contract_frag_hout(const float*in,const float*w,const float*skip,float*out,uint tokens,uint inputs,uint outputs,const uint*reuse_gate){if(reuse_gate&&reuse_gate[0])return;if(inputs==4096&&outputs==1024)vit_contract_blocked_body<false,false,false,true,true>(in,w,skip,out,tokens);}
#endif
