// `NR_COH` is the activation arena's coherence qualifier, in one place.
//
// Every buffer block in this project that aliases the activation arena is
// declared `NR_COH buffer`; every weight block is declared plain. Under
// `-DNR_COHERENT_ACT=1` that expands to `coherent`, which on RADV / Mesa 26.2.2
// / gfx1201 makes ACO emit this buffer's vector loads and stores with
// `scope:SCOPE_DEV` - they bypass L0 and go to L2, the coherence point -
// `coopMatLoad`'s included. With every load of activation data at device scope,
// the memory half of the inter-dispatch barrier is redundant and the runner's
// `--coherent-barriers` drops it, keeping the execution dependency.
//
// **It is off by default and the boundary took the job.** Two facts from
// the ISA, both on gfx1201 / Mesa 26.2.2, decided it:
//
//   1. **No shader-side spelling produces a cache invalidate.** The obvious
//      narrowing - one device-scope acquire at kernel entry, then plain loads
//      that may hit L0 - is not expressible. `memoryBarrierBuffer()`,
//      `memoryBarrier(gl_ScopeDevice, gl_StorageSemanticsBuffer,
//      gl_SemanticsAcquire)`, the `controlBarrier` form, the same two with
//      `gl_SemanticsMakeVisible` under `#pragma use_vulkan_memory_model`, and an
//      `atomicLoad` at `gl_ScopeDevice` with acquire semantics all emit the same
//      thing: `s_wait_loadcnt`/`s_wait_storecnt` and nothing else. ACO never
//      emits `global_inv` on this part. The instruction exists - `llvm-mc
//      -mcpu=gfx1201` assembles `global_inv scope:SCOPE_DEV`, and LLVM's own
//      lowering of `fence syncscope("agent") acquire` emits exactly that - but
//      no path through RADV reaches it. Coherence here is per-access scope or
//      it is a command-buffer packet; there is no third place to put it.
//   2. **Every cache below the device coherence point is write-through.** LLVM
//      emits `global_wb` only at SCOPE_SYS: `fence release` gives
//      `global_wb scope:SCOPE_SYS; s_wait_storecnt 0x0`, while
//      `fence syncscope("agent") release` after a *plain, unscoped* store gives
//      `s_wait_storecnt 0x0` alone. So an unscoped store is device-visible on
//      the same event a `scope:SCOPE_DEV` one is, and the store half of this
//      qualifier never bought anything.
//
// What is left is the read side, and one invalidate a dispatch boundary states
// it exactly: `runner.inv_barrier`, a barrier with `srcAccessMask = 0`. That is
// -1.49% at 1080p and -1.76% at 4K against this file at 1, byte-identical, and
// it is a clock win at a pinned 340 W.
//
// Everything below still builds and is still the A/B: a build with
// `NR_COHERENT_ACT=1` writes `1` into `coherent-act.txt` beside the SPVs and the
// runner switches back to the execution-only barrier on its own.
//
// Default 0, so any build that does not pass the define is byte-identical to
// the one before this file existed.
#ifndef NR_COHERENT_ACT
#define NR_COHERENT_ACT 0
#endif
#if NR_COHERENT_ACT
#define NR_COH coherent
#else
#define NR_COH
#endif

// **The driver infers what the shader cannot state.** Mesa's `nir_opt_access`
// marks a buffer variable that *this kernel* only reads (or only writes)
// non-writeable (non-readable) and then drops its COHERENT bit - the device
// scope is there for the *previous dispatch's* stores, which that pass cannot
// see. Read from the ISA on gfx1201 / Mesa 26.2.2, at `NR_COHERENT_ACT=1`:
//
//     view read and written in the kernel   scope:SCOPE_DEV        (gemmproj b1)
//     view declared `readonly`              scope:SCOPE_DEV        (imgout16 b0)
//     view only read, not declared readonly no scope               (gemmups  b1)
//     view only written                     no scope               (gemmups  b7)
//
// so 74 activation loads and 142 stores across ten of the fifty-six graph
// kernels came out unscoped, which is exactly the stale-L0 hole the memory
// barrier exists to close. The inference is stopped by giving every activation
// view one read and one write the hardware never executes: a workgroup count
// of 0xffffffff is impossible (the device limit is 65535 per dimension) and the
// compiler cannot prove it, so the block survives every pass and the branch is
// never taken. Measured cost: four instructions and one descriptor load in a
// block that never runs.
#define NR_COH_NEVER (gl_NumWorkGroups.x == 0xffffffffu)
