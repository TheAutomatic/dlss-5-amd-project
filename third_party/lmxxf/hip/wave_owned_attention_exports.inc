#define AW_KERNEL(NAME,BO) KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void NAME(const unsigned char*n,const float*w,const unsigned char*f,void*out,uint cw,uint ch,uint ww,uint hh,uint sx,uint sy,uint post,const uint*ff,uint fe,uint*af){attention_wave_body<256,BO>(n,w,f,out,cw,ch,ww,hh,sx,sy,post,ff,fe);if(af){__builtin_amdgcn_fence(__ATOMIC_RELEASE,"agent");if(w2_lane()==0)__hip_atomic_fetch_add(af+__builtin_amdgcn_workgroup_id_x(),1u,__ATOMIC_RELAXED,__HIP_MEMORY_SCOPE_AGENT);}}
AW_KERNEL(c256_attn_wave,false)
AW_KERNEL(c256_attn_wave_bo,true)
