// Compile after c32_reference.hip for exact H/F and midpoint helpers.
// 256 threads = eight wave32 rows. Host dispatches ceil(row_count/8) blocks.
// Shared arithmetic order is the scalar reference order, not a generic reduction.
DEV uint wave_lane(){return __builtin_amdgcn_workitem_id_x()&31u;}
DEV uint wave_row(){return __builtin_amdgcn_workgroup_id_x()*8u+__builtin_amdgcn_workitem_id_x()/32u;}
DEV float broadcast(float x,uint source){return value(__builtin_amdgcn_ds_bpermute((source&31u)*4u,bits(x)));}
DEV float norm_inverse(float x){
 uint l=wave_lane();float pair=half_square_pair(x,broadcast(x,l+16));
 float sum=H(broadcast(pair,l*2)+broadcast(pair,l*2+1));
 sum=H(sum+broadcast(sum,l+4));sum=H(sum+broadcast(sum,l+2));sum=H(sum+broadcast(sum,l+1));
 return H(__builtin_amdgcn_rsqf(maxf(broadcast(sum,0),6.198883056640625e-5f)));
}
KERNEL void c32_attn_normalize_wave(const float*qkv,const float*weights,float*norm,uint tokens){
 uint p=wave_row(),l=wave_lane();if(p>=tokens)return;
 for(uint part=0;part<2;part++){uint i=(part*tokens+p)*32+l;float x=qkv[i];float v=H(x*norm_inverse(x));norm[i]=F(part? v:H(v*H(weights[8192])));}
}
KERNEL void mh_normalize_wave(const float*qkv,const float*weights,float*norm,uint tokens,uint channels){
 uint p=wave_row(),l=wave_lane(),heads=channels/32;if(p>=tokens*heads)return;uint pixel=p/heads,head=p%heads;
 for(uint part=0;part<2;part++){float x=qkv[(pixel*3+part)*channels+head*32+l];float v=H(x*norm_inverse(x));norm[(pixel*2+part)*channels+head*32+l]=F(part?v:H(v*H(weights[4*channels*channels+heads*4096+head])));}
}
DEV void probability_row(const float*ex,float*prob,uint rows){
 uint p=wave_row(),l=wave_lane();if(p>=rows)return;uint row=p*64,base=(l&3u)+((l>>2)&1u)*8;
 float partial=H(ex[row+base]+ex[row+base+16]);
 partial=H(partial+H(ex[row+base+4]+ex[row+base+20]));
 partial=H(partial+H(ex[row+base+32]+ex[row+base+48]));
 partial=H(partial+H(ex[row+base+36]+ex[row+base+52]));
 uint odd=l&1u;float total=broadcast(partial,odd);
 total=H(total+broadcast(partial,odd+2));total=H(total+broadcast(partial,odd+4));total=H(total+broadcast(partial,odd+6));
 float inv=H(1.f/H(broadcast(total,0)+broadcast(total,1)));
 prob[row+l]=F(H(ex[row+l]*inv));prob[row+l+32]=F(H(ex[row+l+32]*inv));
}
KERNEL void c32_attn_probabilities_wave(const float*ex,float*prob,uint windows){probability_row(ex,prob,windows*64);}
KERNEL void mh_probabilities_wave(const float*ex,float*prob,uint windows,uint channels){probability_row(ex,prob,windows*(channels/32)*64);}
