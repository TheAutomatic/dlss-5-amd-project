#ifndef NR_OCC_PAD_GLSL
#define NR_OCC_PAD_GLSL
// NR_OCC_LDS=<bytes>: an LDS allocation nobody uses, to cap how many
// workgroups a WGP holds. At 1080p the C=512 GEMMs launch 1088 waves on 128
// SIMDs - 8 or 9 a SIMD, and the SIMDs that get 9 also start ~1 us later. With
// the cap the leftover workgroups wait and start on the first SIMD that frees a
// slot. gemmprojw (ViT FFN contraction, 1440p/4K) runs faster with the cap at 4
// waves a SIMD too (4K 544 waves = 4.25 a SIMD). Output unchanged; NR_OCC_USE
// keeps the array alive (never taken). attn (C=512 attention, 2-wave
// workgroups) takes 3 KB more: 6 waves a SIMD, 4K -14 us.
#ifdef NR_OCC_LDS
shared uint nr_occ_pad[(NR_OCC_LDS) / 4];
#define NR_OCC_USE(cond, sink) if (cond) { \
    const uint nr_oi = (gl_WorkGroupID.x * 131u + gl_LocalInvocationIndex) % uint((NR_OCC_LDS) / 4); \
    nr_occ_pad[nr_oi] = nr_oi; barrier(); \
    if (nr_occ_pad[(nr_oi + 1u) % uint((NR_OCC_LDS) / 4)] == 5u) sink; }
#else
#define NR_OCC_USE(cond, sink)
#endif
#endif
