// Append to c32_fast_attention.hip for F and explicit legacy D3D Hrtz.
// Production fused C32 epilogue and post merge/head. block256, scalar output count.
DEV uint bf_index(){return __builtin_amdgcn_workgroup_id_x()*256u+__builtin_amdgcn_workitem_id_x();}
DEV uint bf_tile(uint x,uint y,uint width){return ((y/8)*(width/8)+x/8)*64+(y%8)*8+x%8;}
KERNEL void c32_finish_fast(const float*raw,float*main,float*down,uint w,uint h){uint i=bf_index();if(i>=w*h*32)return;uint p=i/32,c=i%32,x=p%w,y=p/w;if(main)main[i]=F(raw[bf_tile(x,y,w)*32+c]);if(down&&!(x&1)&&!(y&1)){float top=Hrtz(raw[bf_tile(x,y,w)*32+c]+raw[bf_tile(x+1,y,w)*32+c]),bottom=Hrtz(raw[bf_tile(x,y+1,w)*32+c]+raw[bf_tile(x+1,y+1,w)*32+c]);down[((y/2)*(w/2)+x/2)*32+c]=F(Hrtz(Hrtz(top+bottom)*.25f));}}
KERNEL void hip_post_merge_fast(const float*low,const float*skip,const float*weights,float*out,uint w,uint h){uint i=bf_index();if(i>=w*h*32)return;uint p=i/32,c=i%32,q=(p/w/2)*(w/2)+(p%w)/2;out[i]=Hrtz(Hrtz(low[q*32+c]*weights[c])+skip[i]*weights[32+c]);}
KERNEL void hip_post_head_fast(const float*raw,const float*base,const float*weights,float*out,uint w,uint h,uint workw,uint sx,uint sy,float scale){uint i=bf_index();if(i>=w*h*3)return;uint p=i/3,row=i%3,fb=bf_tile(p%w+sx,p/w+sy,workw)*32;float acc=0;for(uint j=0;j<32;j++)acc+=raw[fb+j]*weights[row*32+j];acc=Hrtz(acc);float centered=base[p*4+row]*.125f-.0625f,rgb=(acc*scale+centered)*8.f+.5f;out[i]=clampf(rgb,0,1);}

DEV float bf_half_read(const float*p,uint i){return float(reinterpret_cast<const _Float16*>(p)[i]);}

KERNEL void c32_finish_fast_half(const float*raw,float*main,float*down,uint w,uint h){uint i=bf_index();if(i>=w*h*32)return;uint p=i/32,c=i%32,x=p%w,y=p/w;if(main)main[i]=F(bf_half_read(raw,bf_tile(x,y,w)*32+c));if(down&&!(x&1)&&!(y&1)){float top=Hrtz(bf_half_read(raw,bf_tile(x,y,w)*32+c)+bf_half_read(raw,bf_tile(x+1,y,w)*32+c)),bottom=Hrtz(bf_half_read(raw,bf_tile(x,y+1,w)*32+c)+bf_half_read(raw,bf_tile(x+1,y+1,w)*32+c));down[((y/2)*(w/2)+x/2)*32+c]=F(Hrtz(Hrtz(top+bottom)*.25f));}}

KERNEL void hip_post_head_fast_half(const float*raw,const float*base,const float*weights,float*out,uint w,uint h,uint workw,uint sx,uint sy,float scale){uint i=bf_index();if(i>=w*h*3)return;uint p=i/3,row=i%3,fb=bf_tile(p%w+sx,p/w+sy,workw)*32;float acc=0;for(uint j=0;j<32;j++)acc+=bf_half_read(raw,fb+j)*weights[row*32+j];acc=Hrtz(acc);float centered=base[p*4+row]*.125f-.0625f,rgb=(acc*scale+centered)*8.f+.5f;out[i]=clampf(rgb,0,1);}

// Fused half raw -> cropped f32 main, retaining uncropped downsample geometry.
KERNEL void c32_finish_crop_half(const float*raw,float*main,float*down,uint ww,uint hh,uint w,uint h,uint sx,uint sy){
 uint i=bf_index(),p=i/32,c=i%32;
 if(main&&p<w*h){uint x=p%w+sx,y=p/w+sy;main[i]=F(bf_half_read(raw,bf_tile(x,y,ww)*32+c));}
 if(down&&p<ww*hh){uint x=p%ww,y=p/ww;if(!(x&1)&&!(y&1)){
  float top=Hrtz(bf_half_read(raw,bf_tile(x,y,ww)*32+c)+bf_half_read(raw,bf_tile(x+1,y,ww)*32+c));
  float bottom=Hrtz(bf_half_read(raw,bf_tile(x,y+1,ww)*32+c)+bf_half_read(raw,bf_tile(x+1,y+1,ww)*32+c));
  down[((y/2)*(ww/2)+x/2)*32+c]=F(Hrtz(Hrtz(top+bottom)*.25f));
 }}
}

KERNEL void c32_finish_fast_half_main8(const float*raw,float*main,float*down,uint w,uint h){uint i=bf_index();if(i>=w*h*32)return;uint p=i/32,c=i%32,x=p%w,y=p/w;if(main)reinterpret_cast<unsigned char*>(main)[i]=static_cast<unsigned char>(fp8(F(bf_half_read(raw,bf_tile(x,y,w)*32+c))));if(down&&!(x&1)&&!(y&1)){float top=Hrtz(bf_half_read(raw,bf_tile(x,y,w)*32+c)+bf_half_read(raw,bf_tile(x+1,y,w)*32+c)),bottom=Hrtz(bf_half_read(raw,bf_tile(x,y+1,w)*32+c)+bf_half_read(raw,bf_tile(x+1,y+1,w)*32+c));down[((y/2)*(w/2)+x/2)*32+c]=F(Hrtz(Hrtz(top+bottom)*.25f));}}
KERNEL void hip_post_merge_fast_skip8(const float*low,const float*skip,const float*weights,float*out,uint w,uint h){uint i=bf_index();if(i>=w*h*32)return;uint p=i/32,c=i%32,q=(p/w/2)*(w/2)+(p%w)/2;out[i]=Hrtz(Hrtz(low[q*32+c]*weights[c])+__builtin_amdgcn_cvt_f32_fp8(int(reinterpret_cast<const unsigned char*>(skip)[i]),0)*weights[32+c]);}
