// SDKless geometry and exact head. Float tensors; all kernels block256.
#pragma clang fp contract(off)
#pragma clang fp reassociate(off)
typedef unsigned uint;
#define DEV __attribute__((device)) __attribute__((always_inline))
#define KERNEL extern "C" __attribute__((global))
DEV uint bits(float x){return __builtin_bit_cast(uint,x);}
DEV float value(uint x){return __builtin_bit_cast(float,x);}
DEV uint index1(){return __builtin_amdgcn_workgroup_id_x()*256u+__builtin_amdgcn_workitem_id_x();}
DEV float absf(float x){return value(bits(x)&0x7fffffffu);}
DEV uint rshift(uint x,uint n){if(n>31)return 0;uint q=x>>n,r=x&((1u<<n)-1u),m=1u<<(n-1);return q+uint(r>m||(r==m&&(q&1)));}
DEV float half_from(uint h){uint s=(h&0x8000u)<<16,e=(h>>10)&31u,m=h&1023;if(e==31)return value(s|0x7f800000u|(m<<13));if(!e){if(!m)return value(s);float f=float(m)*5.9604644775390625e-8f;return s?-f:f;}return value(s|((e+112)<<23)|(m<<13));}
#ifdef HIP_ISA_HALF
// Keep f32 expression evaluation and both rounding boundaries opaque to fusion.
DEV float H(float x){uint h;float result;asm("v_cvt_f16_f32 %0, %1" : "=v"(h) : "v"(x));asm("v_cvt_f32_f16 %0, %1" : "=v"(result) : "v"(h));return result;}
#else
DEV float H(float x){uint b=bits(x),a=b&0x7fffffffu,s=(b>>16)&0x8000u;int e=int(a>>23)-127;if(a>=0x7f800000u)return x;if(e<-25)return value(b&0x80000000u);if(e<-14)return half_from(s|rshift((a&0x7fffffu)|0x800000u,uint(-e-1)));if(e>15)return half_from(s|0x7c00u);uint h=((a+0xfffu+((a>>13)&1u))>>13)-0x1c000u;return half_from(s|(h>=0x7c00u?0x7c00u:h));}
#endif
DEV uint reflected(uint p,uint size){if(size<=1)return 0;uint n=p%(2*size-2);return n<size?n:2*size-2-n;}
DEV uint tile_pixel(uint x,uint y,uint width){return ((y/8)*(width/8)+x/8)*64+(y%8)*8+x%8;}
KERNEL void hip_input_reflect(const float*input,float*base,float*tiles,uint vw,uint vh,uint pw,uint ph){uint p=index1();if(p>=pw*ph)return;uint x=p%pw,y=p/pw,s=(reflected(y,vh)*vw+reflected(x,vw))*4,t=tile_pixel(x,y,pw)*4;for(uint c=0;c<4;c++){float v=input[s+c];base[p*4+c]=v;tiles[t+c]=v;}}
KERNEL void hip_c32_pack(const float*input,float*output,uint w,uint h,uint ww,uint wh,uint sx,uint sy){uint i=index1();if(i>=ww*wh*32)return;uint p=i/32,c=i%32,t=p/64,x=(t%(ww/8))*8+p%8,y=(t/(ww/8))*8+(p%64)/8;int a=int(x)-int(sx),b=int(y)-int(sy);output[i]=a>=0&&b>=0&&a<int(w)&&b<int(h)?input[(uint(b)*w+uint(a))*32+c]:0;}
KERNEL void hip_crop(const float*input,float*output,uint w,uint h,uint workw,uint sx,uint sy,uint channels,uint tiled){uint i=index1();if(i>=w*h*channels)return;uint p=i/channels,c=i%channels,x=p%w+sx,y=p/w+sy;uint src=tiled?tile_pixel(x,y,workw):y*workw+x;output[i]=input[src*channels+c];}
KERNEL void hip_post_merge(const float*low,const float*skip,const float*weights,float*output,uint w,uint h){uint i=index1();if(i>=w*h*32)return;uint p=i/32,c=i%32,q=(p/w/2)*(w/2)+(p%w)/2;output[i]=H(H(low[q*32+c]*weights[c])+skip[i]*weights[32+c]);}
DEV float aligned_half(int sum,float acc,float scale,int e){
 float scaled=acc*scale;if(scaled!=float(int(scaled))||absf(scaled)>=67108864.f)return value(0x7fc00000u);
 bool neg=sum<0,aneg=acc<0;uint m=neg?0u-uint(sum):uint(sum),other=uint(absf(scaled));
 if(neg==aneg)m+=other;else if(m>=other)m-=other;else{m=other-m;neg=aneg;}if(!m)return 0;
 int quantum=e-27,top=31-__builtin_clz(m),step=(top+quantum>-14?top+quantum:-14)-10,drop=step-quantum;uint r=m;
 if(drop>32)r=0;else if(drop==32)r=m>0x80000000u?1:0;else if(drop>0)r=rshift(m,uint(drop));else step=quantum;
 float result=float(r)*value(uint(step+127)<<23);if(result>=65520.f)result=value(0x7f800000u);return neg?-result:result;
}
KERNEL void hip_post_head_exact(const float*raw,const float*base,const float*weights,float*output,uint w,uint h,uint workw,uint sx,uint sy,float input_scale){
 uint i=index1();if(i>=w*h*3)return;uint p=i/3,row=i%3,fb=tile_pixel(p%w+sx,p/w+sy,workw)*32;float acc=0;
 for(uint part=0;part<2;part++){
  float products[16];int e=acc==0?-1000:int((bits(acc)>>23)&255u)-125;
  for(uint j=0;j<16;j++){float a=raw[fb+part*16+j],b=weights[row*32+part*16+j];products[j]=a*b;if(products[j]!=0){int k=int((bits(a)>>23)&255u)+int((bits(b)>>23)&255u)-252;e=e>k?e:k;}}
  if(e!=-1000){float scale=value(uint(27-e+127)<<23);int sum=0;for(uint j=0;j<16;j++)sum+=int(products[j]*scale);acc=aligned_half(sum,acc,scale,e);}
 }
 float centered=base[p*4+row]*.125f-.0625f,encoded=acc*input_scale+centered,rgb=encoded*8.f+.5f;output[i]=rgb<0?0:rgb>1?1:rgb;
}
