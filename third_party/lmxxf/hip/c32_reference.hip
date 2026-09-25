// SDKless scalar GPU reference, not a WMMA performance backend.
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
DEV float H(float x){return from_half(half_bits(x));}
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
DEV float half_add_midpoint(float a,float b){float sum=a+b,vb=sum-a,error=(a-(sum-vb))+(b-vb);uint u=bits(sum),m=u&0x7fffffffu;
 if(m>=0x38800000u&&m<0x47800000u&&(m&0x1fffu)==0x1000u&&error!=0)sum=value(((error>0)==(sum>0))?u+1:u-1);return H(sum);}
DEV float exp_approx(float x){float a=clampf(H(x*.044921875f+1.30078125f),1.03125f,1.5693359375f);return from_half(((half_bits(a)<<5)+0x8000u)&65535u);}
// All input/output feature tensors up through projection are tile-major [window][64][32].
KERNEL void c32_ffn_expand(const float* input,const float* weights,float* hidden,uint tokens){uint i=lane();if(i>=tokens*128u)return;uint p=i/128u,row=i%128u;float sum=0;
 for(uint j=0;j<32;j++)sum+=F(input[p*32+j])*weights[512+row*32+j];
 float a=H(sum),g=clampf(a,-4.f,4.f);float poly=H(g*H(absf(g)*(-.055908203125f)+.447265625f)+.89453125f);hidden[i]=F(H(a*poly));}
KERNEL void c32_ffn_contract(const float* input,const float* hidden,const float* weights,float* output,uint tokens,uint raw_output){uint i=lane();if(i>=tokens*32u)return;uint p=i/32u,c=i%32u;float a=H(input[i]*weights[8704+c]);
 for(uint group=0;group<4;group++){float s=0;for(uint j=0;j<32;j++)s+=hidden[p*128+group*32+j]*weights[4608+c*128+group*32+j];a=H(a+s);}output[i]=raw_output?a:F(a);}
// qkv layout [part=Q,K,V][tokens][32]; Q/K stay H values, V is F(H).
KERNEL void c32_attn_qkv(const float* input,const float* weights,float* qkv,uint tokens){uint i=lane();if(i>=tokens*96u)return;uint part=i/(tokens*32u),pc=i%(tokens*32u),p=pc/32u,c=pc%32u;float sum=0;
 for(uint j=0;j<32;j++)sum+=F(input[p*32+j])*weights[part*1024+c*32+j];qkv[i]=part==2?F(H(sum)):H(sum);}
// One GPU thread per token, 16-value reductions reproduce the original half reduction tree.
KERNEL void c32_attn_normalize(const float* qkv,const float* weights,float* normalized,uint tokens){uint p=lane();if(p>=tokens)return;
 for(uint part=0;part<2;part++){float sums[16];uint base=(part*tokens+p)*32;
  for(uint i=0;i<16;i++)sums[i]=half_square_pair(qkv[base+i],qkv[base+i+16]);
  for(uint i=0;i<8;i++)sums[i]=H(sums[i*2]+sums[i*2+1]);
  for(uint step=4;step>0;step/=2)for(uint i=0;i<step;i++)sums[i]=H(sums[i]+sums[i+step]);
  float inv=H(__builtin_amdgcn_rsqf(maxf(sums[0],6.198883056640625e-5f)));
  for(uint c=0;c<32;c++){float x=H(qkv[base+c]*inv);normalized[base+c]=F(part==0?H(x*H(weights[8192])):x);}
 }
}
// scores/ex/prob layout [window][query64][key64], no standard softmax substitution.
KERNEL void c32_attn_scores(const float* normalized,const float* weights,float* ex,uint windows){uint i=lane();if(i>=windows*4096u)return;uint w=i/4096u,q=(i/64u)%64u,k=i%64u,tokens=windows*64u;float dot=0;
 for(uint c=0;c<32;c++)dot+=normalized[(w*64+q)*32+c]*normalized[(tokens+w*64+k)*32+c];ex[i]=exp_approx(H(dot+weights[4096+q*64+k]));}
KERNEL void c32_attn_probabilities(const float* ex,float* prob,uint windows){uint p=lane();if(p>=windows*64u)return;uint row=p*64u;float parity[2];
 for(uint odd=0;odd<2;odd++){float total=0;for(uint l=0;l<4;l++){uint base=odd+(l%2)*2+(l/2)*8;float partial=H(ex[row+base]+ex[row+base+16]);partial=H(partial+H(ex[row+base+4]+ex[row+base+20]));partial=H(partial+H(ex[row+base+32]+ex[row+base+48]));partial=H(partial+H(ex[row+base+36]+ex[row+base+52]));total=l==0?partial:H(total+partial);}parity[odd]=total;}
 float inv=H(1.f/H(parity[0]+parity[1]));for(uint k=0;k<64;k++)prob[row+k]=F(H(ex[row+k]*inv));}
KERNEL void c32_attn_av(const float* prob,const float* qkv,float* av,uint windows){uint i=lane(),tokens=windows*64u;if(i>=tokens*32u)return;uint p=i/32u,c=i%32u,w=p/64u,q=p%64u;float a=0;
 for(uint group=0;group<2;group++){float s=0;for(uint key=0;key<32;key++){uint k=group*32+key;s+=prob[(w*64+q)*64+k]*qkv[(2*tokens+w*64+k)*32+c];}a=H(a+s);}av[i]=F(a);}
KERNEL void c32_attn_project(const float* input,const float* av,const float* weights,float* output,uint tokens,uint raw_output){uint i=lane();if(i>=tokens*32u)return;uint p=i/32u,c=i%32u;float a=0;
 for(uint j=0;j<32;j++)a+=av[p*32+j]*weights[3072+c*32+j];float result=half_add_midpoint(a,H(input[i]*weights[8193+c]));output[i]=raw_output?result:F(result);}
DEV uint tile_index(uint x,uint y,uint c,uint width){uint tile=(y/8)*(width/8)+x/8;return (tile*64+(y%8)*8+x%8)*32+c;}
// main and down are raster HWC. Pool reads raw values before F(), exactly as preblock_finish.
KERNEL void c32_finish(const float* raw,float* main,float* down,uint width,uint height){uint i=lane();if(i>=width*height*32u)return;uint p=i/32u,c=i%32u,x=p%width,y=p/width;main[i]=F(raw[tile_index(x,y,c,width)]);
 if(!(x&1u)&&!(y&1u)){float top=H(raw[tile_index(x,y,c,width)]+raw[tile_index(x+1,y,c,width)]),bottom=H(raw[tile_index(x,y+1,c,width)]+raw[tile_index(x+1,y+1,c,width)]);down[((y/2)*(width/2)+x/2)*32+c]=F(H(H(top+bottom)*.25f));}}
