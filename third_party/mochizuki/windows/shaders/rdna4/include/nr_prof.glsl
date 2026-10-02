#ifndef NR_PROF_GLSL
#define NR_PROF_GLSL
// Diagnostic: per-wave clock records into a word region the host appends
// after the arena (env NR_PROF=<words>, it prints the offset). Build a kernel
// with -DNR_PROF_OFF=<offset> to record; without it every macro is empty and
// the SPIR-V is unchanged. Record (8 words): tag, wg.x | wg.y<<16,
// subgroup | aux<<16, realtime start, end (100 MHz), shader clock start, end,
// end hi. NR_PROF_END covers the wave's life; NR_PROF_ITEM_* one persistent item.
#ifdef NR_PROF_OFF
#extension GL_EXT_shader_realtime_clock : require
#extension GL_ARB_shader_clock : require
#extension GL_KHR_shader_subgroup_basic : require
#ifndef NR_PROF_CAP
#define NR_PROF_CAP 8000000u
#endif
layout(set = 0, binding = 0, std430) coherent buffer NrProfU { uint nr_prof_u[]; };
uvec2 nr_prof_t0, nr_prof_c0, nr_prof_tm, nr_prof_cm;
#define NR_PROF_BEGIN() { nr_prof_t0 = clockRealtime2x32EXT(); nr_prof_c0 = clock2x32ARB(); \
                          nr_prof_tm = nr_prof_t0; nr_prof_cm = nr_prof_c0; }
#define NR_PROF_REC(tag, aux, ta, ca) { \
    const uvec2 nr_t1 = clockRealtime2x32EXT(), nr_c1 = clock2x32ARB(); \
    if (subgroupElect()) { \
        const uint nr_s = atomicAdd(nr_prof_u[uint(NR_PROF_OFF)], 1u); \
        if (nr_s < NR_PROF_CAP) { \
            const uint nr_b = uint(NR_PROF_OFF) + 64u + nr_s * 8u; \
            nr_prof_u[nr_b] = uint(tag); \
            nr_prof_u[nr_b + 1u] = gl_WorkGroupID.x | (gl_WorkGroupID.y << 16u); \
            nr_prof_u[nr_b + 2u] = gl_SubgroupID | (uint(aux) << 16u); \
            nr_prof_u[nr_b + 3u] = (ta).x; \
            nr_prof_u[nr_b + 4u] = nr_t1.x; \
            nr_prof_u[nr_b + 5u] = (ca).x; \
            nr_prof_u[nr_b + 6u] = nr_c1.x; \
            nr_prof_u[nr_b + 7u] = nr_c1.y; \
        } } }
#define NR_PROF_END(tag) NR_PROF_REC(tag, gl_WorkGroupID.z, nr_prof_t0, nr_prof_c0)
#define NR_PROF_ITEM_START() { nr_prof_tm = clockRealtime2x32EXT(); nr_prof_cm = clock2x32ARB(); }
#define NR_PROF_ITEM_END(tag, item) NR_PROF_REC(tag, item, nr_prof_tm, nr_prof_cm)
// Windows diagnostic: item start -> its producers waited for (tag bit 30 also set).
#define NR_PROF_ITEM_WAITED(tag, item) NR_PROF_REC(tag, item, nr_prof_tm, nr_prof_cm)
#else
#define NR_PROF_BEGIN()
#define NR_PROF_END(tag)
#define NR_PROF_ITEM_START()
#define NR_PROF_ITEM_END(tag, item)
#define NR_PROF_ITEM_WAITED(tag, item)
#endif
#endif
