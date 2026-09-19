// Append after c32_reference.hip: uses its SDKless DEV/KERNEL, H(), bits/value helpers.
// Exact noise-table prefix from preblock_input_mix.hlsl, LIVE_PROFILE=1, NATIVE_FAST_PREFIX=0.
// block256; pixels are 8x8 tile-major RGBA. History is processing-grid raster RGBA.
DEV uint prefix_pcg(uint s){uint w=((s>>((s>>28)+4))^s)*0x108ef2d9u;return (w>>22)^w;}
DEV uint prefix_uniform_index(uint s){uint w=((s>>((s>>28)+4))^s)*0x108ef2d9u;return (w>>30)^(w>>8);}
DEV uint prefix_round_shift(uint m,int shift){
 if(shift<=0)return m<<uint(-shift);
 if(shift>=32)return shift==32&&m>0x80000000u?1u:0u;
 uint q=m>>shift,rem=m&((1u<<shift)-1u),half=1u<<(shift-1);
 return q+uint(rem>half||(rem==half&&(q&1u)));
}
DEV float prefix_scaled_half(int n,int exponent){
 uint sign=n<0?0x8000u:0u,m=n<0?0u-uint(n):uint(n);if(!m)return 0;
 int lead=31-__builtin_clz(m),e=lead+exponent;uint b;
 if(e < -14)b=prefix_round_shift(m,-exponent-24);
 else{uint q=prefix_round_shift(m,lead-10);if(q==2048u){q=1024;e++;}b=e>15?0x7c00u:(uint(e+15)<<10)|(q-1024u);}
 return from_half(sign|b);
}
KERNEL void dlss5_prefix_reference(const float*rgba_tiles,const float*history_raster,const float*noise,const float*weights,float*prefix,float*debug_features,uint width,uint height,uint seed,uint temporal){
 uint p=lane();if(p>=width*height)return;
 uint tile=p/64,x=(tile%(width/8))*8+p%8,y=(tile/(width/8))*8+(p%64)/8;
 uint h=prefix_pcg((x*0x8da6b343u)^(y*0xd8163841u)^(seed*0x9e3779b9u)^0x243f6a88u);
 uint ia=prefix_uniform_index(h*0xcaa5b80du+0x21dd796bu),ib=prefix_uniform_index(h*0x2c9277b5u+0xac564b05u);
 uint ic=prefix_uniform_index(h*0x83232c31u+0x3463e0acu),id=prefix_uniform_index(h*0xfa6dc5f9u+0x4712a88eu);
 float g0=H(noise[ia*3]*noise[ic*3+1]),g1=H(noise[ib*3]*noise[id*3+1]),g2=H(noise[ib*3]*noise[id*3+2]);
 float r=H(H(H(rgba_tiles[p*4])-.5f)*.125f),g=H(H(H(rgba_tiles[p*4+1])-.5f)*.125f),b=H(H(H(rgba_tiles[p*4+2])-.5f)*.125f);
 float f[16]={g1,g2,g,b,g0,1,.0078125f,1,r,g,1,1,b,r,1,0};
 if(temporal){uint q=(y*width+x)*4;f[13]=H(H(H(history_raster[q])-.5f)*.125f);f[2]=H(H(H(history_raster[q+1])-.5f)*.125f);f[3]=H(H(H(history_raster[q+2])-.5f)*.125f);}
 if(debug_features)for(uint i=0;i<32;i++)debug_features[p*32+i]=i<16?f[i]:0;
 for(uint c=0;c<32;c++){
  float products[16];int largest=-1000;
  for(uint i=0;i<16;i++){float a=f[i],b=weights[c*16+i];products[i]=a*b;if(products[i]!=0){int e=int((bits(a)>>23)&255u)-126+int((bits(b)>>23)&255u)-126;largest=largest>e?largest:e;}}
  if(largest==-1000){prefix[p*32+c]=0;continue;}
  float scale=value(uint(27-largest+127)<<23);int sum=0;for(uint i=0;i<16;i++)sum+=int(products[i]*scale);
  prefix[p*32+c]=prefix_scaled_half(sum,largest-27);
 }
}
