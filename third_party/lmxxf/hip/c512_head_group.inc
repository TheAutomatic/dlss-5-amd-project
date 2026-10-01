// Append only to multihead-fast-padded-wave-packed experimental source.
// Reuse existing production pool/group projection math; no new arithmetic implementation.
#ifndef C512_HEAD_GROUP
#define C512_HEAD_GROUP 0
#endif
#if C512_HEAD_GROUP
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void mh_pool_project_group_c512(const float*raw,const float*w,float*output,uint width,uint height,uint source_width,uint valid_width,uint valid_height){
 mh_pool_project_group_body<512>(raw,w,output,width,height,source_width,valid_width,valid_height);
}
#ifndef HIP_VIT_GATHER_FOLD
#define HIP_VIT_GATHER_FOLD 0
#endif
#if HIP_VIT_GATHER_FOLD
// Head Down storing in ViT gather order (replaces Down + forward vit_gather; host requires ow*oh % 16 == 0).
KERNEL __attribute__((amdgpu_flat_work_group_size(512,512)))
void mh_pool_project_group_c512_gout(const float*raw,const float*w,float*output,uint width,uint height,uint source_width,uint valid_width,uint valid_height){
 mh_pool_project_group_body<512,true>(raw,w,output,width,height,source_width,valid_width,valid_height);
}
#endif
#endif
