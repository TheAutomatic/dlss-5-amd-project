// SDKless multihead C64/C128/C256 scalar GPU reference, NOT a performance backend.
// Mirrors native_c64.hlsl non-wave math, native_c64_shift.hlsl, native_c64_ds.hlsl.
// Host validates geometry/channel contracts; all tensors/weights externally float32.
// Every kernel uses one-dimensional blocks of exactly 256 threads.
#pragma clang fp contract(off)
#pragma clang fp reassociate(off)
typedef unsigned uint;
#define DEV __attribute__((device)) __attribute__((always_inline))
#define KERNEL extern "C" __attribute__((global))
DEV uint bits(float x){union{float f;uint u;}v;v.f=x;return v.u;}
DEV float value(uint x){union{float f;uint u;}v;v.u=x;return v.f;}
DEV uint lane(){return __builtin_amdgcn_workgroup_id_x()*256u+__builtin_amdgcn_workitem_id_x();}
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
DEV float H(float x){if((bits(x)&0x7fffffffu)>=0x7f800000u)return x;return from_half(half_bits(x));}
#endif
DEV float F(float x){uint b=bits(x),a=b&0x7fffffffu,s=b&0x80000000u;if(a==0)return 0.f;
 if(a>=0x43e00000u)return value(s|0x43e00000u); // finite E4M3 saturation, including overflow
 if(a<0x3c800000u){float scaled=absf(x)*512.f;uint q=uint(scaled);float r=scaled-float(q);q+=uint(r>.5f||(r==.5f&&(q&1u)));float f=float(q)/512.f;return s?-f:f;}
 uint r=(a+0x7ffffu+((a>>20)&1u))&0xfff00000u;return value(s|(r>0x43e00000u?0x43e00000u:r));}
DEV float half_square_pair(float lower,float upper){float a=lower*lower,b=H(upper*upper),sum=a+b;
 if((bits(sum)&0x7fffffffu)>=0x7f800000u)return H(sum);
 float large=maxf(a,b),small=minf(a,b),recovered=sum-large,error=small-recovered;
 if(sum==65520.f)return error<0?65504.f:H(sum);
 float rounded=H(sum);if(error==0||(bits(rounded)&0x7fffffffu)>=0x7f800000u||rounded==sum)return rounded;
 uint hb=half_bits(rounded),lo_bits=sum<rounded?hb-1:hb;float lo=from_half(lo_bits),hi=from_half(lo_bits+1),mid=(lo+hi)*.5f;
 return sum==mid?(error>0?hi:lo):rounded;
}
DEV float exp_approx(float x){float a=clampf(H(x*.044921875f+1.30078125f),1.03125f,1.5693359375f);return from_half(((half_bits(a)<<5)+0x8000u)&65535u);}
DEV bool valid_c(uint c){return c==64u||c==128u||c==256u;}
DEV bool valid_attn(uint c){return valid_c(c)||c==512u;}
DEV bool valid_spatial(uint c){return c==32u||valid_attn(c);}
DEV uint pixel_in_window(uint window,uint token,uint width){return ((window/(width/8))*8+token/8)*width+(window%(width/8))*8+token%8;}
DEV uint window_of_pixel(uint p,uint width){return (p/width/8)*(width/8)+(p%width)/8;}
DEV uint token_of_pixel(uint p,uint width){return ((p/width)%8)*8+(p%width)%8;}
// Weights unchanged: expand0, contract4*C*C,project8*C*C,skip9*C*C.
KERNEL void mh_ffn_expand(const float* input,const float* weights,float* hidden,uint tokens,uint channels){
 uint i=lane();if(!valid_c(channels)||i>=tokens*4*channels)return;uint p=i/(4*channels),row=i%(4*channels);float a=0;
 for(uint g=0;g<channels/32;g++){float s=0;for(uint j=0;j<32;j++)s+=input[p*channels+g*32+j]*weights[row*channels+g*32+j];a=H(a+s);}
 float gate=clampf(a,-4.f,4.f),poly=H(gate*H(absf(gate)*(-.055908203125f)+.447265625f)+.89453125f);hidden[i]=F(H(a*poly));
}
KERNEL void mh_ffn_contract(const float* hidden,const float* weights,float* middle,uint tokens,uint channels){
 uint i=lane();if(!valid_c(channels)||i>=tokens*channels)return;uint p=i/channels,row=i%channels;float a=0;
 for(uint g=0;g<4*channels/32;g++){float s=0;for(uint j=0;j<32;j++)s+=hidden[p*4*channels+g*32+j]*weights[4*channels*channels+row*4*channels+g*32+j];a=H(a+s);}middle[i]=F(a);
}
DEV void project(const float* input,const float* feature,const float* weights,float* output,uint tokens,uint channels,uint matrix_offset,uint skip_offset,uint raw){
 uint i=lane();if(!valid_attn(channels)||i>=tokens*channels)return;uint p=i/channels,row=i%channels;float a=H(feature[i]*weights[skip_offset+row]);
 for(uint g=0;g<channels/32;g++){float s=0;for(uint j=0;j<32;j++)s+=input[p*channels+g*32+j]*weights[matrix_offset+row*channels+g*32+j];a=H(a+s);}output[i]=raw?a:F(a);
}
KERNEL void mh_ffn_project(const float* middle,const float* feature,const float* weights,float* output,uint tokens,uint channels){if(valid_c(channels))project(middle,feature,weights,output,tokens,channels,8*channels*channels,9*channels*channels,0);}
// Match original precomputed-QKV storage: [pixel][part=Q,K,V][channel], all H values.
KERNEL void mh_qkv(const float* input,const float* weights,float* qkv,uint tokens,uint channels){
 uint i=lane();if(!valid_attn(channels)||i>=tokens*3*channels)return;uint p=i/(3*channels),part=(i/channels)%3,row=i%channels;float a=0;
 for(uint g=0;g<channels/32;g++){float s=0;for(uint j=0;j<32;j++)s+=input[p*channels+g*32+j]*weights[part*channels*channels+row*channels+g*32+j];a=H(a+s);}qkv[i]=a;
}
// One thread per pixel/head; qk is [pixel][Q,K][channel]. Preserve square/reduction tree.
KERNEL void mh_normalize(const float* qkv,const float* weights,float* qk,uint tokens,uint channels){
 uint i=lane(),heads=channels/32;if(!valid_attn(channels)||i>=tokens*heads)return;uint p=i/heads,head=i%heads;uint scale=4*channels*channels+heads*4096;
 for(uint part=0;part<2;part++){uint base=(p*3+part)*channels+head*32;float sums[16];
  for(uint j=0;j<16;j++)sums[j]=half_square_pair(qkv[base+j],qkv[base+j+16]);
  for(uint j=0;j<8;j++)sums[j]=H(sums[j*2]+sums[j*2+1]);
  for(uint step=4;step>0;step/=2)for(uint j=0;j<step;j++)sums[j]=H(sums[j]+sums[j+step]);
  float inv=H(__builtin_amdgcn_rsqf(maxf(sums[0],6.198883056640625e-5f)));
  for(uint j=0;j<32;j++){float v=H(qkv[base+j]*inv);qk[(p*2+part)*channels+head*32+j]=F(part==0?H(v*H(weights[scale+head])):v);}
 }
}
// ex/prob are [raster_window][head][query64][key64]. There is no exp()/softmax().
KERNEL void mh_scores_exp(const float* qk,const float* weights,float* ex,uint width,uint height,uint channels){
 uint i=lane(),heads=channels/32,windows=(width/8)*(height/8);if(!valid_attn(channels)||width%8||height%8||i>=windows*heads*4096)return;
 uint wh=i/4096,window=wh/heads,head=wh%heads,q=(i/64)%64,k=i%64,qp=pixel_in_window(window,q,width),kp=pixel_in_window(window,k,width);float s=0;
 for(uint j=0;j<32;j++)s+=qk[(qp*2)*channels+head*32+j]*qk[(kp*2+1)*channels+head*32+j];
 float score=H(s+weights[4*channels*channels+head*4096+q*64+k]);ex[i]=exp_approx(score);
}
KERNEL void mh_probabilities(const float* ex,float* prob,uint windows,uint channels){
 uint p=lane();if(!valid_attn(channels)||p>=windows*(channels/32)*64)return;uint row=p*64;float parity[2];
 for(uint odd=0;odd<2;odd++){float total=0;for(uint l=0;l<4;l++){uint base=odd+(l%2)*2+(l/2)*8;float partial=H(ex[row+base]+ex[row+base+16]);partial=H(partial+H(ex[row+base+4]+ex[row+base+20]));partial=H(partial+H(ex[row+base+32]+ex[row+base+48]));partial=H(partial+H(ex[row+base+36]+ex[row+base+52]));total=l==0?partial:H(total+partial);}parity[odd]=total;}
 float inv=H(1.f/H(parity[0]+parity[1]));for(uint k=0;k<64;k++)prob[row+k]=F(H(ex[row+k]*inv));
}
KERNEL void mh_attention_av(const float* prob,const float* qkv,float* av,uint width,uint height,uint channels){
 uint i=lane();if(!valid_attn(channels)||width%8||height%8||i>=width*height*channels)return;uint p=i/channels,c=i%channels,head=c/32,window=window_of_pixel(p,width),q=token_of_pixel(p,width),heads=channels/32;float a=0;
 for(uint g=0;g<2;g++){float s=0;for(uint key=0;key<32;key++){uint k=g*32+key,kp=pixel_in_window(window,k,width);s+=prob[((window*heads+head)*64+q)*64+k]*F(qkv[(kp*3+2)*channels+c]);}a=H(a+s);}av[i]=F(a);
}
KERNEL void mh_attention_project(const float* av,const float* feature,const float* weights,float* output,uint tokens,uint channels,uint raw_output){
 uint heads=channels/32;project(av,feature,weights,output,tokens,channels,3*channels*channels,4*channels*channels+heads*4096+heads,raw_output);
}
// Generic separate projection layout used by split_project:matrix0, skipC*C.
KERNEL void mh_split_project(const float* input,const float* feature,const float* weights,float* output,uint tokens,uint channels){project(input,feature,weights,output,tokens,channels,0,channels*channels,0);}
// Shift/pad is zero padding, NOT cyclic shift. The optional C256/height4 wrap
// matches PLAIN_SHORT_Y only; it must never wrap x or other channel/height cases.
KERNEL void mh_shift_pack(const float* input,float* output,uint width,uint height,uint work_width,uint work_height,uint pad_x,uint pad_y,uint channels,uint plain_short_y){
 uint i=lane();if(!valid_spatial(channels)||i>=work_width*work_height*channels)return;uint p=i/channels,c=i%channels;int x=int(p%work_width)-int(pad_x),y=int(p/work_width)-int(pad_y);
 if(plain_short_y&&channels==256&&height==4)y=(y%4+4)%4;
 if(x<0||y<0||x>=int(width)||y>=int(height)){output[i]=0;return;}output[i]=input[(uint(y)*width+uint(x))*channels+c];
}
KERNEL void mh_shift_crop(const float* input,float* output,uint width,uint height,uint work_width,uint pad_x,uint pad_y,uint channels){
 uint i=lane();if(!valid_spatial(channels)||i>=width*height*channels)return;uint p=i/channels,c=i%channels;output[i]=input[((p/width+pad_y)*work_width+p%width+pad_x)*channels+c];
}
// native_c64_ds split into pool then projection so each boundary can be compared.
// width/height here are OUTPUT pooled dimensions, source_width is input pitch.
KERNEL void mh_pool(const float* raw,float* pooled,uint width,uint height,uint source_width,uint valid_width,uint valid_height,uint channels){
 uint i=lane();if(!valid_spatial(channels)||i>=width*height*channels)return;uint p=i/channels,c=i%channels;
 if(valid_width&&(p%width>=valid_width||p/width>=valid_height)){pooled[i]=0;return;}
 uint base=((p/width)*2*source_width+(p%width)*2)*channels;
 float top=H(raw[base+c]+raw[base+channels+c]),bottom=H(raw[base+source_width*channels+c]+raw[base+(source_width+1)*channels+c]);pooled[i]=F(H(H(top+bottom)*.25f));
}
KERNEL void mh_pool_project(const float* pooled,const float* weights,float* output,uint width,uint height,uint valid_width,uint valid_height,uint channels){
 uint i=lane();if(!valid_spatial(channels)||i>=width*height*2*channels)return;uint p=i/(2*channels),row=i%(2*channels);
 if(valid_width&&(p%width>=valid_width||p/width>=valid_height)){output[i]=0;return;}
 float a=0;for(uint g=0;g<channels/32;g++){float s=0;for(uint j=0;j<32;j++)s+=pooled[p*channels+g*32+j]*weights[row*channels+g*32+j];a=H(a+s);}output[i]=F(a);
}
