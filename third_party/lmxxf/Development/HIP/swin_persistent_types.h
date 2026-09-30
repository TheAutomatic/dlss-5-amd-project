#pragma once
// Identical host/device ABI. No HIP SDK headers required.
struct SpLayer {
 const void *in,*fw,*aw; void *out;
 unsigned w,h,ww,hh,sx,sy,post,pad;
};
struct SpNode { unsigned layer,window,need,nchild,child[4]; };
struct SpParams {
 SpLayer layers[8]; const SpNode *nodes; unsigned *state,*error_host;
 unsigned total,first,base,fault,layers_count,pad;
 unsigned long long timeout_ticks;
};
static_assert(sizeof(SpLayer)==64,"SpLayer ABI");
static_assert(sizeof(SpNode)==32,"SpNode ABI");
static_assert(sizeof(SpParams)==568,"SpParams ABI");
