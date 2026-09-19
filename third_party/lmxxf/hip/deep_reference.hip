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
// Split FFN: per-output kernels with explicit mixed/hidden scratch.
DEV float activation(float a){float g=clampf(a,-4.f,4.f);return F(H(a*H(g*H(absf(g)*(-.055908203125f)+.447265625f)+.89453125f)));}
KERNEL void split_mix(const float* input,const float* weights,float* mixed,uint tokens){uint i=lane();if(i>=tokens*512u)return;uint p=i/512,row=i%512;float a=0;for(uint k=0;k<512;k+=32){float s=0;for(uint j=0;j<32;j++)s+=input[p*512+k+j]*weights[row*512+k+j];a=H(a+s);}mixed[i]=F(a);}
KERNEL void split_expand(const float* mixed,const float* weights,float* hidden,uint tokens){uint i=lane();if(i>=tokens*2048u)return;uint p=i/2048,g=(i%2048)/256,row=i%256;float a=0;for(uint k=0;k<64;k+=32){float s=0;for(uint j=0;j<32;j++)s+=mixed[p*512+g*64+k+j]*weights[262144+g*16384+row*64+k+j];a=H(a+s);}hidden[i]=activation(a);}
KERNEL void split_contract(const float* hidden,const float* weights,float* output,uint tokens){uint i=lane();if(i>=tokens*512u)return;uint p=i/512,g=(i%512)/64,row=i%64;float a=0;for(uint k=0;k<256;k+=32){float s=0;for(uint j=0;j<32;j++)s+=hidden[p*2048+g*256+k+j]*weights[393216+g*16384+row*256+k+j];a=H(a+s);}output[i]=F(a);}
KERNEL void split_projection(const float* input,const float* weights,const float* residual,float* output,uint tokens){uint i=lane();if(i>=tokens*512u)return;uint p=i/512,row=i%512;float a=H(residual[i]*weights[262144+row]);for(uint k=0;k<512;k+=32){float s=0;for(uint j=0;j<32;j++)s+=input[p*512+k+j]*weights[row*512+k+j];a=H(a+s);}output[i]=F(a);}
KERNEL void vit_qkv_project(const float* input,const float* weights,float* output,uint tokens){uint i=lane();if(i>=tokens*3072u)return;uint part=i/(tokens*1024),token=(i/1024)%tokens,row=i%1024;float total=0;for(uint group=0;group<2;group++){float a=0;for(uint k=group*512;k<(group+1)*512;k+=32){float s=0;for(uint j=0;j<32;j++)s+=input[token*1024+k+j]*weights[part*1048576+row*1024+k+j];a=H(a+s);}total=group?H(total+a):a;}output[i]=total;}
DEV uint tensor_channel(uint c){return ((c&1)<<1)|((c&2)>>1)|((c&4)<<2)|((c&8)>>1)|((c&16)>>1);}
KERNEL void vit_qkv_normalize(const float* input,const float* weights,float* output,uint tokens){uint i=lane();if(i>=tokens*96u)return;uint part=i/(tokens*32),token=(i/32)%tokens,head=i%32,base=(part*tokens+token)*1024+head*32;if(part==2){for(uint c=0;c<32;c++)output[base+c]=F(input[base+c]);return;}float v[32],s[16];for(uint c=0;c<32;c++)v[tensor_channel(c)]=input[base+c];for(uint c=0;c<16;c++)s[c]=half_square_pair(v[c],v[c+16]);for(uint c=0;c<8;c++)s[c]=H(s[c*2]+s[c*2+1]);for(uint w=4;w;w/=2)for(uint c=0;c<w;c++)s[c]=H(s[c]+s[c+w]);float inverse=H(__builtin_amdgcn_rsqf(maxf(s[0],6.198883056640625e-5f)));for(uint c=0;c<32;c++){float a=H(input[base+c]*inverse);if(part==0)a=H(H(a*5.65625f)*H(weights[3145728+head]));output[base+c]=F(a);}}
// Scores [query][head32][key]. Attention tokens must be a multiple of 64.
KERNEL void vit_attention_scores(const float* qkv,float* ex,uint tokens){uint i=lane();if(i>=tokens*32u*tokens)return;uint key=i%tokens,head=(i/tokens)%32,query=i/(tokens*32);float score=0;for(uint c=0;c<32;c++)score+=qkv[query*1024+head*32+c]*qkv[(tokens+key)*1024+head*32+c];float affine=clampf(H(H(score)*from_half(0x2dbb)+1.708984375f),1.439453125f,1.9775390625f);ex[i]=from_half(((half_bits(affine)<<4)+0x4000u)&65535u);}
DEV uint key_index(uint c){return ((c&16)>>4)|((c&1)<<1)|((c&2)<<1)|(c&8)|((c&4)<<2)|(c&32);}
KERNEL void vit_attention_inverse(const float* ex,float* inverse,uint tokens){uint i=lane();if(i>=tokens*32u)return;uint row=i*tokens;float denominator=0;for(uint chunk=0;chunk<tokens;chunk+=64){float sums[2];for(uint parity=0;parity<2;parity++){float accumulated=0;for(uint group=0;group<4;group++){uint base=parity+(group&1)*2+(group>>1)*8;float a=H(ex[row+chunk+key_index(base)]+ex[row+chunk+key_index(base+16)]);a=H(a+H(ex[row+chunk+key_index(base+4)]+ex[row+chunk+key_index(base+20)]));a=H(a+H(ex[row+chunk+key_index(base+32)]+ex[row+chunk+key_index(base+48)]));a=H(a+H(ex[row+chunk+key_index(base+36)]+ex[row+chunk+key_index(base+52)]));accumulated=group?H(accumulated+a):a;}sums[parity]=accumulated;}denominator=H(denominator+H(sums[0]+sums[1]));}inverse[i]=H(1.f/denominator);}
KERNEL void vit_attention_av(const float* qkv,const float* ex,const float* inverse,float* output,uint tokens){uint i=lane();if(i>=tokens*1024u)return;uint query=i/1024,head=(i%1024)/32,c=i%32,row=(query*32+head)*tokens;float accumulated=0;for(uint part=0;part<tokens/32;part++){float a=0;for(uint j=0;j<32;j++){uint key=part*32+j;a+=F(ex[row+key])*qkv[(2*tokens+key)*1024+head*32+c];}accumulated=H(accumulated+a);}output[i]=F(H(accumulated*inverse[query*32+head]));}
KERNEL void vit_expand(const float* input,const float* weights,float* output,uint tokens,uint input_channels,uint output_channels){uint i=lane();if(i>=tokens*output_channels)return;uint token=i/output_channels,row=i%output_channels;float a=0;for(uint k=0;k<input_channels;k+=32){float s=0;for(uint j=0;j<32;j++)s+=input[token*input_channels+k+j]*weights[row*input_channels+k+j];a=H(a+s);}output[i]=activation(a);}
KERNEL void vit_project(const float* input,const float* weights,const float* residual,float* output,uint tokens,uint input_channels,uint output_channels){uint i=lane();if(i>=tokens*output_channels)return;uint token=i/output_channels,row=i%output_channels;float total=0;for(uint part=0;part<4;part++){float a=part==0?H(residual[i]*weights[input_channels*output_channels+row]):0;for(uint k=part*(input_channels/4);k<(part+1)*(input_channels/4);k+=32){float s=0;for(uint j=0;j<32;j++)s+=input[token*input_channels+k+j]*weights[row*input_channels+k+j];a=H(a+s);}total=part==0?a:H(total+a);}output[i]=F(total);}
// Project once per source pixel/channel then emit its clipped nearest 2x footprint.
KERNEL void decoder_project2x(const float* input,const float* weights,const float* residual,float* output,uint input_width,uint input_height,uint output_width,uint output_height,uint input_channels,uint output_channels){uint i=lane();if(i>=input_width*input_height*output_channels)return;uint token=i/output_channels,row=i%output_channels,partitions=input_channels==1024?4:1;float total=0;for(uint part=0;part<partitions;part++){float a=0;for(uint k=part*(input_channels/partitions);k<(part+1)*(input_channels/partitions);k+=32){float s=0;for(uint j=0;j<32;j++)s+=input[token*input_channels+k+j]*weights[row*input_channels+k+j];a=H(a+s);}total=part==0?a:H(total+a);}for(uint dy=0;dy<2;dy++)for(uint dx=0;dx<2;dx++){uint x=token%input_width*2+dx,y=token/input_width*2+dy;if(x>=output_width||y>=output_height)continue;uint idx=(y*output_width+x)*output_channels+row;float merged=H(total+F(residual[idx])*weights[input_channels*output_channels+row]);output[idx]=output_channels==32?merged:F(merged);}}
KERNEL void vit_gather(const float* input,const uint* indices,float* output,uint count){uint i=lane();if(i<count)output[i]=input[indices[i]];}
// Production FP8 fast path has a different accumulation contract from legacy above.
// Scalar sum order may differ from hardware WMMA; no inserted half rounding.
KERNEL void vit_attention_scores_fast(const float* qkv,float* ex,uint tokens){uint i=lane();if(i>=tokens*32u*tokens)return;uint key=i%tokens,head=(i/tokens)%32,query=i/(tokens*32);float score=0;for(uint c=0;c<32;c++)score+=qkv[query*1024+head*32+c]*qkv[(tokens+key)*1024+head*32+c];float affine=clampf(score*from_half(0x2dbb)+1.708984375f,1.439453125f,1.9775390625f);ex[i]=from_half(((half_bits(affine)<<4)+0x4000u)&65535u);}
KERNEL void vit_attention_inverse_fast(const float* ex,float* inverse,uint tokens){uint i=lane();if(i>=tokens*32u)return;float denominator=0;for(uint key=0;key<tokens;key++)denominator+=ex[i*tokens+key];inverse[i]=1.f/denominator;}
KERNEL void vit_attention_av_fast(const float* qkv,const float* ex,const float* inverse,float* output,uint tokens){uint i=lane();if(i>=tokens*1024u)return;uint query=i/1024,head=(i%1024)/32,c=i%32,row=(query*32+head)*tokens;float accumulated=0;for(uint key=0;key<tokens;key++)accumulated+=F(ex[row+key])*qkv[(2*tokens+key)*1024+head*32+c];output[i]=F(H(accumulated*inverse[query*32+head]));}
