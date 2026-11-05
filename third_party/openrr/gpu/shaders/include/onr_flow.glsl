// SPDX-License-Identifier: MIT
// FLOW (the _fl kernels, enc2 .. dec2_1): consecutive kernels of the small levels run without a barrier between them.
// Each window of kernel FLOW_SLOT counts itself done in its window row; a window of the next kernel (FLOW_WAIT) first
// waits for the rows of the previous kernel its inputs come from (one row of margin each side, the whole kernel when its
// band touches the last row). Correct because the windows of a dispatch are all launched before those of the next one
// (same queue, in order): a waiting window never holds back the windows it waits for. The flags come in pc.mask (unused
// by the fast bodies), zeroed by the engine every frame: per slot FLOW_WORDS words, [0, FLOW_ROWS) windows done per row,
// [FLOW_ROWS, 2 FLOW_ROWS) row done, then windows done and all done. The engine keeps the barriers when a level has more
// window rows than FLOW_ROWS.
#extension GL_KHR_memory_scope_semantics : require
#define FLOW_ROWS 240u
#define FLOW_WORDS 512u
layout(buffer_reference, std430, buffer_reference_align = 4) buffer FlowFlags { uint v[]; };
FlowFlags flow_flags() { return FlowFlags(uint64_t(pc.mask)); }
#if FLOW_WAIT
// producer pixel rows [ya, yb] at producer height hp
void flow_wait_rows(int ya, int yb, int hp) {
    if (gl_LocalInvocationIndex == 0u) {
        FlowFlags f = flow_flags();
        uint b = uint(FLOW_SLOT - 1) * FLOW_WORDS;
        int lo = max(ya / 8 - 1, 0), hi = yb / 8 + 1;
        if (hi >= (hp - 1) / 8) {
            while (atomicLoad(f.v[b + 2u * FLOW_ROWS + 1u], gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsAcquire) == 0u) {}
        } else {
            for (int r = lo; r <= hi; ++r)
                while (atomicLoad(f.v[b + FLOW_ROWS + uint(r)], gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsAcquire) == 0u) {}
        }
    }
    barrier();
    memoryBarrier(gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsAcquire);
}
void flow_wait(ivec2 win) {
    int y0 = 8 * win.y - pc.shift.y, y1 = y0 + 7;
    y0 = max(y0, 0), y1 = min(max(y1, 7), pc.size.y - 1);
#if ENC_STAGE
    flow_wait_rows(2 * y0, 2 * y1 + 1, 2 * pc.size.y);   // the previous level's pooled output, at 2x
#elif DEC_UP
    flow_wait_rows(max(y0 / 2 - 1, 0), y1 / 2 + 1, pc.lo_size.y);   // the level below, at 1/2
#else
    flow_wait_rows(y0, y1, pc.size.y);   // dec_dfn: the dec_up of the level
#endif
}
#else
void flow_wait(ivec2 win) {}
#endif
void flow_signal(ivec2 win) {
    memoryBarrierBuffer();
    barrier();
    if (gl_LocalInvocationIndex == 0u) {
        FlowFlags f = flow_flags();
        uint b = uint(FLOW_SLOT) * FLOW_WORDS;
        uint rows = uint((pc.size.y + pc.shift.y + 7) / 8);
        uint n = atomicAdd(f.v[b + uint(win.y)], 1u, gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsAcquireRelease);
        if (n + 1u == uint(pc.nwx)) atomicStore(f.v[b + FLOW_ROWS + uint(win.y)], 1u, gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelease);
        uint t = atomicAdd(f.v[b + 2u * FLOW_ROWS], 1u, gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsAcquireRelease);
        if (t + 1u == uint(pc.nwx) * rows) atomicStore(f.v[b + 2u * FLOW_ROWS + 1u], 1u, gl_ScopeDevice, gl_StorageSemanticsBuffer, gl_SemanticsRelease);
    }
}
