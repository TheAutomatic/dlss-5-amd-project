// Same RGB patch gain for all channels. Two real passes, predict only pass three.
#pragma clang fp contract(off)
#pragma clang fp reassociate(off)
typedef unsigned uint;
#define KERNEL extern "C" __attribute__((global))
#define DEV __attribute__((device)) __attribute__((always_inline))
DEV float clip(float v,float lo,float hi){return __builtin_fminf(__builtin_fmaxf(v,lo),hi);}
// bpermute returns uint bits; retain float reinterpretation.
DEV float sum32(float v){for(uint s=16;s;s>>=1){union{float f;uint u;}a,b;a.f=v;b.u=__builtin_amdgcn_ds_bpermute(((__builtin_amdgcn_workitem_id_x()+s)&31)*4,a.u);v+=b.f;}return v;}
KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
void mp_predict_gain(const float*x,const float*y1,const float*y2,float*gain,uint w,uint h){
 uint lane=__builtin_amdgcn_workitem_id_x(),tile=__builtin_amdgcn_workgroup_id_x(),cols=(w+15)/16;
 float aa=0,bb=0,ab=0,count=0;
 for(uint k=lane;k<256;k+=32){uint px=(tile%cols)*16+k%16,py=(tile/cols)*16+k/16;if(px>=w||py>=h)continue;uint p=py*w+px;count+=1;
 for(uint c=0;c<3;c++){float a=y1[p*3+c]-x[p*4+c],b=y2[p*3+c]-y1[p*3+c];aa+=a*a;bb+=b*b;ab+=a*b;}}
 aa=sum32(aa);bb=sum32(bb);ab=sum32(ab);count=sum32(count);
 if(!lane){float eps=count*1.e-7f;float r=0;if(aa>=eps&&ab>0&&ab*ab>=.25f*aa*bb)r=clip(ab/(aa+eps),0,1);gain[tile]=r;}
}
KERNEL void mp_predict_apply(const float*y1,const float*y2,const float*gain,float*out,uint w,uint h){
 uint p=__builtin_amdgcn_workgroup_id_x()*256+__builtin_amdgcn_workitem_id_x();if(p>=w*h)return;
 uint cols=(w+15)/16,rows=(h+15)/16;float tx=clip((float(p%w)+.5f)/16.f-.5f,0,float(cols-1)),ty=clip((float(p/w)+.5f)/16.f-.5f,0,float(rows-1));uint ix=uint(tx),iy=uint(ty),jx=ix+1<cols?ix+1:ix,jy=iy+1<rows?iy+1:iy;float fx=tx-ix,fy=ty-iy;
 float top=gain[iy*cols+ix]*(1-fx)+gain[iy*cols+jx]*fx,bot=gain[jy*cols+ix]*(1-fx)+gain[jy*cols+jx]*fx,r=top*(1-fy)+bot*fy;
 for(uint c=0;c<3;c++){uint i=p*3+c;out[i]=clip(y2[i]+r*(y2[i]-y1[i]),0,1);}
}

// Direct RGBA first pass, remaining inputs/output keep RGB.
KERNEL __attribute__((amdgpu_flat_work_group_size(32,32)))
void mp_predict_gain_stride(const float*x,const float*y1,const float*y2,float*gain,uint w,uint h,uint first_stride){
 uint lane=__builtin_amdgcn_workitem_id_x(),tile=__builtin_amdgcn_workgroup_id_x(),cols=(w+15)/16;
 float aa=0,bb=0,ab=0,count=0;
 for(uint k=lane;k<256;k+=32){uint px=(tile%cols)*16+k%16,py=(tile/cols)*16+k/16;if(px>=w||py>=h)continue;uint p=py*w+px;count+=1;
 for(uint c=0;c<3;c++){float a=y1[p*first_stride+c]-x[p*4+c],b=y2[p*3+c]-y1[p*first_stride+c];aa+=a*a;bb+=b*b;ab+=a*b;}}
 aa=sum32(aa);bb=sum32(bb);ab=sum32(ab);count=sum32(count);
 if(!lane){float eps=count*1.e-7f;float r=0;if(aa>=eps&&ab>0&&ab*ab>=.25f*aa*bb)r=clip(ab/(aa+eps),0,1);gain[tile]=r;}
}
KERNEL void mp_predict_apply_stride(const float*y1,const float*y2,const float*gain,float*out,uint w,uint h,uint first_stride){
 uint p=__builtin_amdgcn_workgroup_id_x()*256+__builtin_amdgcn_workitem_id_x();if(p>=w*h)return;
 uint cols=(w+15)/16,rows=(h+15)/16;float tx=clip((float(p%w)+.5f)/16.f-.5f,0,float(cols-1)),ty=clip((float(p/w)+.5f)/16.f-.5f,0,float(rows-1));uint ix=uint(tx),iy=uint(ty),jx=ix+1<cols?ix+1:ix,jy=iy+1<rows?iy+1:iy;float fx=tx-ix,fy=ty-iy;
 float top=gain[iy*cols+ix]*(1-fx)+gain[iy*cols+jx]*fx,bot=gain[jy*cols+ix]*(1-fx)+gain[jy*cols+jx]*fx,r=top*(1-fy)+bot*fy;
 for(uint c=0;c<3;c++){uint i=p*3+c;out[i]=clip(y2[i]+r*(y2[i]-y1[p*first_stride+c]),0,1);}
}
