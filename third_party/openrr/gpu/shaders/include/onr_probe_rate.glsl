// SPDX-License-Identifier: MIT
// WMMA throughput probe body (probe_rate_*.comp): each subgroup runs 8 independent accumulator chains of n
// steps; the host times the dispatch. RATE_F32: fp32 accumulators; RATE_FP8: e4m3 operands, K = RATE_K.
#extension GL_KHR_cooperative_matrix : require
#extension GL_KHR_memory_scope_semantics : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_EXT_float_e4m3 : require
#include "onr_common.glsl"
layout(local_size_x = 128) in;
layout(push_constant, scalar) uniform P { F16Buf src; F16Buf dst; uint n; } pc;
#define ROW gl_CooperativeMatrixLayoutRowMajor
#if RATE_FP8
#define TA floate4m3_t
#define KK RATE_K
#else
#define TA float16_t
#define KK 16
#endif
#if RATE_F32
#define TC float
#else
#define TC float16_t
#endif
#define MA coopmat<TA, gl_ScopeSubgroup, 16, KK, gl_MatrixUseA>
#define MB coopmat<TA, gl_ScopeSubgroup, KK, 16, gl_MatrixUseB>
#define MC coopmat<TC, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>
#define MH coopmat<float16_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>

void main() {
    coopmat<float16_t, gl_ScopeSubgroup, 16, KK, gl_MatrixUseA> a16;
    coopmat<float16_t, gl_ScopeSubgroup, KK, 16, gl_MatrixUseB> b16;
    coopMatLoad(a16, pc.src.v, 0u, KK, ROW);
    coopMatLoad(b16, pc.src.v, 0u, 16u, ROW);
    MA a = MA(a16);
    MB b = MB(b16);
    MC c0 = MC(0.0), c1 = MC(0.0), c2 = MC(0.0), c3 = MC(0.0), c4 = MC(0.0), c5 = MC(0.0), c6 = MC(0.0), c7 = MC(0.0);
    for (uint i = 0u; i < pc.n; ++i) {
        c0 = coopMatMulAdd(a, b, c0);
        c1 = coopMatMulAdd(a, b, c1);
        c2 = coopMatMulAdd(a, b, c2);
        c3 = coopMatMulAdd(a, b, c3);
        c4 = coopMatMulAdd(a, b, c4);
        c5 = coopMatMulAdd(a, b, c5);
        c6 = coopMatMulAdd(a, b, c6);
        c7 = coopMatMulAdd(a, b, c7);
    }
    MC s = c0 + c1 + c2 + c3 + c4 + c5 + c6 + c7;
    coopMatStore(MH(s), pc.dst.v, gl_WorkGroupID.x * 1024u + gl_SubgroupID * 256u, 16u, ROW);
}
