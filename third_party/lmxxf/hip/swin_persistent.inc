#ifndef HIP_SWIN_PERSISTENT_KERNELS
#define HIP_SWIN_PERSISTENT_KERNELS 0
#endif
#if HIP_SWIN_PERSISTENT_KERNELS
// State: head, tail, abort, timeout-count; counters[total], queue[total],
// per-workgroup task broadcast[total]. All storage is device memory.
DEV uint sp_load(const uint *p){return __hip_atomic_load(p,__ATOMIC_ACQUIRE,__HIP_MEMORY_SCOPE_AGENT);}
DEV uint sp_poll(const uint *p){return __hip_atomic_load(p,__ATOMIC_RELAXED,__HIP_MEMORY_SCOPE_AGENT);}
DEV void sp_store(uint*p,uint x){__hip_atomic_store(p,x,__ATOMIC_RELEASE,__HIP_MEMORY_SCOPE_AGENT);}
DEV void sp_fail(SpParams p,uint code){
 uint old=__hip_atomic_exchange(p.state+2,code,__ATOMIC_ACQ_REL,__HIP_MEMORY_SCOPE_AGENT);
 if(!old)__hip_atomic_fetch_add(p.error_host,1u,__ATOMIC_RELEASE,__HIP_MEMORY_SCOPE_SYSTEM);
}
DEV void sp_global_group_sync(){
 __builtin_amdgcn_fence(__ATOMIC_RELEASE,"agent");
 __builtin_amdgcn_s_barrier();
 __builtin_amdgcn_fence(__ATOMIC_ACQUIRE,"agent");
}
KERNEL void sp_init(SpParams p){
 uint i=__builtin_amdgcn_workgroup_id_x()*256+__builtin_amdgcn_workitem_id_x();
 if(i<3*p.total)p.state[4+i]=0;
 if(i==0){p.state[0]=p.base;p.state[1]=p.base+p.first;p.state[2]=0;p.state[3]=0;}
}
// SP_INIT_PAIR (2026-10-01 bitexact-pm): one launch initializes this stage's queue state AND the other C256 stage's
// (encoder -> decoder): the decoder's state is a separate buffer whose last user is the previous frame, so its init has
// no data dependency on anything this frame and the decoder's own sp_init dispatch goes away. Same values as two sp_init.
#ifndef SP_INIT_PAIR
#define SP_INIT_PAIR 0
#endif
#if SP_INIT_PAIR
KERNEL void sp_init_pair(SpParams p,uint*state2,uint total2,uint base2,uint first2){
 uint i=__builtin_amdgcn_workgroup_id_x()*256+__builtin_amdgcn_workitem_id_x();
 if(i<3*p.total)p.state[4+i]=0;
 if(i<3*total2)state2[4+i]=0;
 if(i==0){p.state[0]=p.base;p.state[1]=p.base+p.first;p.state[2]=0;p.state[3]=0;state2[0]=base2;state2[1]=base2+first2;state2[2]=0;state2[3]=0;}
}
#endif
template<uint C,bool W16=false> DEV void sp_run_body(SpParams p){
 const uint tid=__builtin_amdgcn_workitem_id_x(),group=__builtin_amdgcn_workgroup_id_x();
 uint *counts=p.state+4,*queue=counts+p.total,*broadcast=queue+p.total;
 if(tid==0){
  uint selected=0;
  if(!sp_poll(p.state+2)){
   uint ticket=__hip_atomic_fetch_add(p.state,1u,__ATOMIC_RELAXED,__HIP_MEMORY_SCOPE_AGENT);
   uint slot=ticket-p.base;
   if(slot>=p.total)sp_fail(p,2);
   else if(slot<p.first)selected=slot+1;
   else {
    selected=sp_poll(queue+slot);
    if(!selected){
     unsigned long long start=__builtin_readsteadycounter();
     do {
      if(sp_poll(p.state+2))break;
      __builtin_amdgcn_s_sleep(2);
      selected=sp_poll(queue+slot);
      if(!selected&&__builtin_readsteadycounter()-start>=p.timeout_ticks){
       __hip_atomic_fetch_add(p.state+3,1u,__ATOMIC_RELAXED,__HIP_MEMORY_SCOPE_AGENT);
       sp_fail(p,1);break;
      }
     }while(!selected);
    }
    // A relaxed load followed by an acquire fence consumes the queue slot's
    // release publication without flushing caches on every unsuccessful poll.
    if(selected)__builtin_amdgcn_fence(__ATOMIC_ACQUIRE,"agent");
   }
  }
  sp_store(broadcast+group,selected);
 }
 sp_global_group_sync();
 uint selected=__builtin_amdgcn_readfirstlane(sp_load(broadcast+group));
 if(!selected)return; // Uniform across the complete workgroup.
 uint job=selected-1;
 if(job>=p.total){if(tid==0)sp_fail(p,2);return;}
 uint layer_index=__builtin_amdgcn_readfirstlane(p.nodes[job].layer);
 uint window=__builtin_amdgcn_readfirstlane(p.nodes[job].window);
 if(layer_index>=p.layers_count){if(tid==0)sp_fail(p,2);return;}
 SpLayer layer=p.layers[layer_index];
 // This function is generated verbatim from the production body except for
 // supplying the window ID rather than reading workgroup_id_x.
 swin_wave2_body<C,true,true,false,true,W16>(static_cast<const float*>(layer.in),
   static_cast<const float*>(layer.fw),static_cast<const float*>(layer.aw),
   layer.out,layer.w,layer.h,layer.ww,layer.hh,layer.sx,layer.sy,layer.post,nullptr,nullptr,window);
 // Every wave publishes all its output before the leader publishes readiness.
 sp_global_group_sync();
 if(tid==0&&!sp_load(p.state+2)){
  const SpNode &node=p.nodes[job];
  // Fault injection deliberately strands the next layer; used only in lab.
  if(p.fault==1&&node.layer==0)return;
  for(uint i=0;i<node.nchild;i++){
   if(i>=4||node.child[i]>=p.total){sp_fail(p,2);break;}
   uint child=node.child[i];
   uint old=__hip_atomic_fetch_add(counts+child,1u,__ATOMIC_ACQ_REL,__HIP_MEMORY_SCOPE_AGENT);
   uint need=p.nodes[child].need;
   if(old+1>need){sp_fail(p,2);break;}
   if(old+1==need){
    uint ticket=__hip_atomic_fetch_add(p.state+1,1u,__ATOMIC_RELAXED,__HIP_MEMORY_SCOPE_AGENT);
    uint slot=ticket-p.base;
    if(slot>=p.total){sp_fail(p,2);break;}
    sp_store(queue+slot,child+1);
   }
  }
 }
}
KERNEL __attribute__((amdgpu_flat_work_group_size(64,64))) void sp_run64(SpParams p){sp_run_body<64>(p);}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void sp_run128(SpParams p){sp_run_body<128>(p);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void sp_run256(SpParams p){sp_run_body<256>(p);}
#if W2_FFN_W16
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void sp_run256_w16(SpParams p){sp_run_body<256,true>(p);}
#endif

// One workgroup, no polling and no inter-workgroup dependency. Normally it
// exits immediately. After an aborted queue it replays layer-major from the
// untouched input, preserving every operation in the production window body.
template<uint C,bool W16=false> DEV void sp_recover_body(SpParams p){
 if(!sp_load(p.state+2))return;
 for(uint li=0;li<p.layers_count;li++){
  SpLayer layer=p.layers[li];uint windows=(layer.ww/8)*(layer.hh/8);
  for(uint win=0;win<windows;win++){
   swin_wave2_body<C,true,true,false,true,W16>(static_cast<const float*>(layer.in),
    static_cast<const float*>(layer.fw),static_cast<const float*>(layer.aw),
    layer.out,layer.w,layer.h,layer.ww,layer.hh,layer.sx,layer.sy,layer.post,nullptr,nullptr,win);
   sp_global_group_sync();
  }
 }
 if(__builtin_amdgcn_workitem_id_x()==0)__hip_atomic_fetch_add(p.error_host+1,1u,__ATOMIC_RELEASE,__HIP_MEMORY_SCOPE_SYSTEM);
}
KERNEL __attribute__((amdgpu_flat_work_group_size(64,64))) void sp_recover64(SpParams p){sp_recover_body<64>(p);}
KERNEL __attribute__((amdgpu_flat_work_group_size(128,128))) void sp_recover128(SpParams p){sp_recover_body<128>(p);}
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void sp_recover256(SpParams p){sp_recover_body<256>(p);}
#if W2_FFN_W16
KERNEL __attribute__((amdgpu_flat_work_group_size(256,256))) void sp_recover256_w16(SpParams p){sp_recover_body<256,true>(p);}
#endif

#endif
