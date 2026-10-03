#ifndef NR_CHAIN_GLSL
#define NR_CHAIN_GLSL
// NR_CHAIN: the barrier between two dispatches of a chained run is
// replaced by a completion counter, so the next dispatch's workgroups launch
// while the previous one drains and are resident when it finishes.
//
// Semantics are the barrier's: a workgroup does nothing before its predecessor
// dispatch has completed entirely, and since that dispatch did the same, every
// earlier chained dispatch is complete too. Only the launch ramp and the drain
// overlap. The GPU launches a dispatch's workgroups after all of the previous
// dispatch's, so a waiting workgroup never holds a slot its producer needs.
//
// Counters live in the activation arena (binding 0 in every kernel) and are
// never reset: a dispatch of W workgroups adds W per frame, and frame f waits
// for need * f, f being the epoch word of the run before the chain (the C=256
// persistent run's, which counts frames from the zero-filled arena).
// Push tail (after the kernel's own fields): wait counter (u32 index, or
// ~0 for none), need (workgroups of the waited dispatch), own counter, epoch.
#ifndef NR_CHAIN
#define NR_CHAIN 0
#endif
#if NR_CHAIN
#extension GL_KHR_memory_scope_semantics : require
#define NR_CHAIN_FIELDS uint chain_wait, chain_need, chain_sig, chain_epoch;
layout(set = 0, binding = 0, std430) coherent buffer NrChainU { uint nr_chain_u[]; };
// Macros, not functions: they expand inside main, after the push block.
// A dispatch's block is 16 counter replicas NR_CHAIN_STRIDE words apart (word
// 1 of the block is the wait-timeout error word): every workgroup of the next
// dispatch polls, and one hot L2 line for all of them stalls the channel the
// running dispatch's own loads share. Producers add to all 16 (one lane each);
// a consumer polls replica (workgroup id % 16) with a SALU backoff between polls.
#ifndef NR_CHAIN_STRIDE
#define NR_CHAIN_STRIDE 256u
#endif
#ifndef NR_CHAIN_BACKOFF
#define NR_CHAIN_BACKOFF 64
#endif
#define nr_chain_wait() { \
    if (pc.chain_wait != 0xFFFFFFFFu) { \
        if (gl_LocalInvocationIndex == 0u) { \
            const uint nr_ce = atomicLoad(nr_chain_u[pc.chain_epoch], gl_ScopeDevice, \
                                          gl_StorageSemanticsBuffer, gl_SemanticsAcquire); \
            const uint nr_ct = pc.chain_need * nr_ce; \
            const uint nr_ca = pc.chain_wait + ((gl_WorkGroupID.x + gl_WorkGroupID.y * 5u + gl_WorkGroupID.z * 11u) & 15u) * NR_CHAIN_STRIDE; \
            uint nr_cb = 1u << 20, nr_cd = nr_ct; \
            while (atomicLoad(nr_chain_u[nr_ca], gl_ScopeDevice, gl_StorageSemanticsBuffer, \
                              gl_SemanticsAcquire) < nr_ct && nr_cb != 0u) { \
                --nr_cb; \
                for (int nr_k = 0; nr_k < NR_CHAIN_BACKOFF; ++nr_k) nr_cd = nr_cd * 1103515245u + 12345u; \
                if (nr_cd == 0x9e3779b9u) nr_cb = 1u; \
            } \
            if (nr_cb == 0u) atomicMax(nr_chain_u[pc.chain_sig + 1u], 1u); \
        } \
        barrier(); \
        memoryBarrier(gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsAcquire); \
    } }
#define nr_chain_signal() { \
    if (pc.chain_sig != 0xFFFFFFFFu) { \
        memoryBarrier(gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelease); \
        barrier(); \
        if (gl_LocalInvocationIndex < 16u) \
            atomicAdd(nr_chain_u[pc.chain_sig + gl_LocalInvocationIndex * NR_CHAIN_STRIDE], 1u, \
                      gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelease); \
    } }
#else
#define NR_CHAIN_FIELDS
#define nr_chain_wait()
#define nr_chain_signal()
#endif
#endif
