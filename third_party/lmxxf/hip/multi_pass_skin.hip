// Independent colour-heuristic implementation: original network x is sRGB-like after HDR shoulder.
// The YCbCr ellipse parameters are publicly described by OptiShade's SkinColourWeight.
#pragma clang fp contract(off)
#pragma clang fp reassociate(off)
typedef unsigned uint;
#define KERNEL extern "C" __attribute__((global))
#define DEV __attribute__((device)) __attribute__((always_inline))
DEV float cap(float a){return __builtin_fminf(__builtin_fmaxf(a,0.f),1.f);}
DEV float ease(float lo,float hi,float v){float t=cap((v-lo)/(hi-lo));return t*t*(3.f-2.f*t);}
DEV float skin(const float*x,uint p){float r=cap(x[p*4]),g=cap(x[p*4+1]),b=cap(x[p*4+2]);float y=.299f*r+.587f*g+.114f*b;float u=((b-y)*.564f+.5f-.405f)/.090f,v=((r-y)*.713f+.5f-.600f)/.110f;float distance=__builtin_amdgcn_sqrtf(u*u+v*v),chroma=__builtin_fmaxf(r,__builtin_fmaxf(g,b))-__builtin_fminf(r,__builtin_fminf(g,b));return (1.f-ease(.95f,1.45f,distance))*ease(.02f,.06f,chroma);}
KERNEL void mp_skin_mask(const float*x,float*mask,uint w,uint h){uint p=__builtin_amdgcn_workgroup_id_x()*256+__builtin_amdgcn_workitem_id_x();if(p>=w*h)return;uint px=p%w,py=p/w;float base=skin(x,p),sum=0;for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){uint sx=uint(__builtin_fminf(__builtin_fmaxf(float(int(px)+dx),0.f),float(w-1))),sy=uint(__builtin_fminf(__builtin_fmaxf(float(int(py)+dy),0.f),float(h-1)));sum+=skin(x,sy*w+sx);}mask[p]=__builtin_fmaxf(base,sum/9.f);}
KERNEL void mp_skin_blend(const float*one,const float*many,const float*mask,float*out,uint n){uint p=__builtin_amdgcn_workgroup_id_x()*256+__builtin_amdgcn_workitem_id_x();if(p>=n)return;float m=mask[p];for(uint c=0;c<3;c++){uint i=p*3+c;out[i]=m>=1.f?one[i]:m<=0.f?many[i]:one[i]*m+many[i]*(1.f-m);}}

KERNEL void mp_skin_blend_stride(const float*one,const float*many,const float*mask,float*out,uint n,uint first_stride){uint p=__builtin_amdgcn_workgroup_id_x()*256+__builtin_amdgcn_workitem_id_x();if(p>=n)return;float m=mask[p];for(uint c=0;c<3;c++){uint i=p*3+c;out[i]=m>=1.f?one[p*first_stride+c]:m<=0.f?many[i]:one[p*first_stride+c]*m+many[i]*(1.f-m);}}
