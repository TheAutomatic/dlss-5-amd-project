// The only file in this project that names a cooperative matrix or an element type.
//
// RDNA3 (gfx11, RX 7000) counterpart of linux/shaders/rdna4/include/coopmm.glsl. gfx11 has
// no FP8 conversion and no FP8 WMMA, so every e4m3 quantity of the network is held as an
// FP16 value: `NR_E4M3` is `float16_t`, `fe4m3vecN` is `f16vecN`, and the WMMA is the f16 x
// f16 -> f32 one. Every e4m3 value is exactly an FP16 value and the product of two needs 8
// significant bits, so the products are the ones the FP8 WMMA forms; only the order of the
// sixteen-term sum inside one WMMA is the hardware's.
//
// What an FP8 build does with a *conversion* (`NR_E4M3(x)`, `fe4m3vec2(v)`, `NR_FRAG_E4M3(acc)`)
// is a quantisation and is spelled `nr_quant_*` in this tree. Those constructors must not be
// used on an unquantised value here: as FP16 types they would narrow without rounding onto the
// e4m3 grid and compile without complaint.
//
// Every element index and stride in the shaders counts elements, so an e4m3 array and the FP16
// array that replaces it are indexed identically; the host allocates twice the bytes.
//
// The shape is 16x16x16 at wave32 (requiredSubgroupSize 32), the f16 configurations NAVI31 reports.
#include "coherent_act.glsl"
#extension GL_KHR_cooperative_matrix : require
#extension GL_KHR_memory_scope_semantics : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_shuffle : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require


#define NR_MMA_M 16
#define NR_MMA_N 16
#define NR_MMA_K 16

#define NR_E4M3 float16_t
#define NR_F16  float16_t
#ifndef NR_ROUND_HALF_UP
#define NR_ROUND_HALF_UP 0
#endif

// Operands are FP16 whatever the network's type: an e4m3 value is an FP16 value.
#define NR_OPERAND NR_F16
#define NR_FRAG_A   coopmat<NR_OPERAND, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA>
#define NR_FRAG_B   coopmat<NR_OPERAND, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB>

#define NR_FRAG_ACC coopmat<float,   gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>
#define NR_ACC_ZERO NR_FRAG_ACC(0.0)

// accumulator to f16 and storing it straight out is what removes the LDS round
// trip: element indexing into a fragment is implementation-defined in KHR
// cooperative matrix, so per-element epilogue work has to go through memory -
// but a *whole-fragment* store does not.
#define NR_FRAG_A16   coopmat<NR_F16, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA>
#define NR_FRAG_B16   coopmat<NR_F16, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB>
#define NR_FRAG_ACC16 coopmat<NR_F16, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>
#define NR_NARROW_ACC(acc) NR_FRAG_ACC16(acc)
// The way back.
//
// **This was documented as "an f16 rounding that survives the optimiser" and it
// does not.** Wiring it into `fswin_t.comp`'s accumulator emulation as
// `NR_ROUND_MODE=3` is 1.31x / 1.49x / **2.76x** at C=32/128/256 - and scores
// 90.8% / 73.5% / 68.4%, which are **exactly** an `NR_ACC_F16=0` build's scores
// at those widths. ACO folds `f32 -> f16 -> f32` here just as it folds the
// scalar spelling; the speedup is the rounding disappearing.
//
// A standalone f16-accumulator probe may well still show it surviving - a probe stores
// the value, which forces it to be materialised, where this feeds straight into
// the next `NR_MMA` and can be folded. **A round trip only survives if
// something downstream cannot be proved not to need it**, so the probe does not
// generalise to the use.
//
// So both routes to a cheap hardware rounding are closed: the scalar one
// (`NR_ROUND_MODE=1`) and this one. `nr_round_f16_e4m3`'s three instructions are
// the floor.
#define NR_WIDEN_ACC(acc)  NR_FRAG_ACC(acc)

// A is [M][K] row-major - the activation tile as staged in LDS.
#define NR_LOAD_A(frag, arr, off, stride) \
    coopMatLoad(frag, arr, (off), (stride), gl_CooperativeMatrixLayoutRowMajor)

// B is held [N][K] and loaded ColumnMajor. Decision 2, and it is measured, not
// stylistic: this orientation makes K contiguous and lowers to one
// buffer_load_b64 per lane with zero permutes, where RowMajor B costs eight
// buffer_load_d16_u8 plus six v_perm_b32 per tile. It is also NVIDIA's own
// `.row.col` convention and PyTorch's [out][in], so nothing driver-specific
// enters the weight file.
#define NR_LOAD_B(frag, arr, off, stride) \
    coopMatLoad(frag, arr, (off), (stride), gl_CooperativeMatrixLayoutColumnMajor)

// B held [K][N] and loaded RowMajor. The other orientation is the usual one -
// weights are [N][K] and want ColumnMajor - but an operand that is already
// indexed [k][n] needs this one, and getting it wrong is a transpose that
// scores at chance rather than failing. In the attention kernel K wants
// ColumnMajor (it is [token][dim] and the product needs [dim][token]) while V
// wants RowMajor (it is [token][dim] and the product needs exactly that).
#define NR_LOAD_B_ROW(frag, arr, off, stride) \
    coopMatLoad(frag, arr, (off), (stride), gl_CooperativeMatrixLayoutRowMajor)

#define NR_STORE_ACC(frag, arr, off, stride) \
    coopMatStore(frag, arr, (off), (stride), gl_CooperativeMatrixLayoutRowMajor)

// Load a plain row-major [M][N] block straight into an **Accumulator**. This is
// the portable way to get an (i, j)-indexed matrix into a fragment: the driver
// puts each element in whichever component it owns, so the shader never learns
// the component-to-coordinate map that KHR leaves implementation-defined.
//
// It is what makes an additive term like an attention position bias free -
// `acc = acc + bias_frag` is element-wise on two same-use fragments and needs no
// coordinates, where indexing `bias[(i0+r)*64 + j]` per element needs them and
// therefore needs memory. NVIDIA does the same thing by making the bias their
// MMA's C operand; this reaches it without depending on our fragment order.
#define NR_LOAD_ACC(frag, arr, off, stride) \
    coopMatLoad(frag, arr, (off), (stride), gl_CooperativeMatrixLayoutRowMajor)

// The transposed orientation needs the other layout for three of these. An A
// operand read from a [n][k] array, an Accumulator stored into a [col][row]
// array, an Accumulator loaded from one - all the same trick, and none of them
// costs anything the RowMajor form does not: the addresses a lane touches are
// identical, only which index runs fastest changes.
#define NR_LOAD_A_COL(frag, arr, off, stride) \
    coopMatLoad(frag, arr, (off), (stride), gl_CooperativeMatrixLayoutColumnMajor)
#define NR_LOAD_ACC_COL(frag, arr, off, stride) \
    coopMatLoad(frag, arr, (off), (stride), gl_CooperativeMatrixLayoutColumnMajor)
#define NR_STORE_ACC_COL(frag, arr, off, stride) \
    coopMatStore(frag, arr, (off), (stride), gl_CooperativeMatrixLayoutColumnMajor)

// The e4m3 vector element types of the RDNA4 shaders are FP16 vectors here.
#define fe4m3vec2 f16vec2
#define fe4m3vec4 f16vec4

// An Accumulator holding e4m3-grid values. Here it is the FP16 Accumulator: narrowing an f32
// accumulator into it does **not** round onto the e4m3 grid, so a fragment that was an
// `NR_FRAG_E4M3(acc)` conversion on RDNA4 goes through `NR_QUANT_FRAG` instead.
#define NR_FRAG_E4M3 coopmat<NR_E4M3, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>

// NR_I4: int8 fragments whose bytes each hold two signed int4 (low nibble = even k). The SPIR-V and the driver see
// v_wmma_i32_16x16x16_iu8; the host rewrites that opcode to v_wmma_i32_16x16x16_iu4 (0xcc44 -> 0xcc45) in the
// pipeline binary (nrvk.hpp). The iu4 form reads the first two of the fragment's four operand registers, so
// one multiply is 16 x 16 x 16 int4 at twice the iu8 and f16 rate (linux/test/wmma_rate): only the first eight
// bytes of a lane's sixteen carry data (gemm1x1.comp NR_I4FRAG).
#if defined(NR_I4) || defined(NR_I4_OUT) || (defined(NR_I4_SIDE) && NR_I4_SIDE)
#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int32 : require
#define NR_FRAG_I8A  coopmat<int8_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA>
#define NR_FRAG_I8B  coopmat<int8_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB>
#define NR_FRAG_IACC coopmat<int32_t, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>
#define NR_IACC_ZERO NR_FRAG_IACC(0)
// The int8 fragment holds 32 nibbles, a gfx11 iu4 WMMA reads 16: a fragment is multiplied twice into the same accumulator
// and the second multiply is marked saturating, which the host's rewrite (nrvk.hpp rewrite_iu4) turns into "read the
// other half of the operands". Accumulations of nibble products never get near saturating.
#define NR_I4_MMA2(c, a, b) { (c) = coopMatMulAdd((a), (b), (c)); \
    (c) = coopMatMulAdd((a), (b), (c), gl_MatrixOperandsSaturatingAccumulation); }
#endif

// ---- the fragment layout this target actually uses ----------------------
//
// Measured with a layout probe on gfx1100 / RADV at wave32, a 16x16 Accumulator:
//
//     component c of lane l  ==  element (2 * c + l / 16, l % 16)
//
// (gfx12 is (8 * (l / 16) + c, l % 16).) A row is still sixteen consecutive lanes at one
// component, so a row-wise reduction is the same butterfly over the low four lane bits. A
// column is **not** eight adjacent rows of one lane any more: a lane's eight components are
// the rows of one parity, two apart. Every site that turns a component into a coordinate goes
// through `NR_ACC_ROW`; the lane half (`l / 16`) is the row's parity.
#define NR_ACC_ROW_LANES 16u
#define NR_ACC_ROW(lane, c) (2u * uint(c) + ((lane) >> 4))
#define NR_ACC_COL(lane)    ((lane) & 15u)
#define NR_ACC_ROW_REDUCE(v) { \
    (v) += subgroupShuffleXor((v), 1u); \
    (v) += subgroupShuffleXor((v), 2u); \
    (v) += subgroupShuffleXor((v), 4u); \
    (v) += subgroupShuffleXor((v), 8u); }

// has one place to look.
#define NR_WIDEN_A16(a) NR_FRAG_A16(a)

#define NR_MMA(acc, a, b) acc = coopMatMulAdd(a, b, acc)

// Compile-time unrolling, because nothing else does it. Measured on the first
// kernel written against this header: a `for (int i = 0; i < 4; ++i)` over an
// array of accumulator fragments is left as a real loop by glslang, by
// glslang's own -Os, and by ACO, so the array is dynamically indexed and lands
// in **scratch** - 98 scratch_store_b128 in a 465-instruction shader, the whole
// register-blocking scheme silently undone. Constant indices are the entire
// difference, so the indices are generated here rather than hoped for.
//
// Two independent families so a row loop can contain a column loop; a macro
// cannot nest inside itself. Bodies take the index as a literal, and an inner
// body picks up the outer index from a `const int` in the enclosing block,
// which GLSL treats as a constant expression.
#define NR_PASTE2(a, b) a##b
#define NR_PASTE(a, b) NR_PASTE2(a, b)

// 6 is here because the QKV projection tiles 96 rows as one N block of six
// fragments; the counts are not required to be powers of two.
#define NR_I_1(X)  X(0)
#define NR_I_2(X)  X(0) X(1)
#define NR_I_3(X)  X(0) X(1) X(2)
#define NR_I_4(X)  X(0) X(1) X(2) X(3)
#define NR_I_6(X)  X(0) X(1) X(2) X(3) X(4) X(5)
#define NR_I_8(X)  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define NR_J_1(X)  X(0)
#define NR_J_2(X)  X(0) X(1)
#define NR_J_3(X)  X(0) X(1) X(2)
#define NR_J_4(X)  X(0) X(1) X(2) X(3)
#define NR_J_6(X)  X(0) X(1) X(2) X(3) X(4) X(5)
#define NR_J_8(X)  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define NR_K_1(X)  X(0)
#define NR_K_2(X)  X(0) X(1)
#define NR_K_3(X)  X(0) X(1) X(2)
#define NR_K_4(X)  X(0) X(1) X(2) X(3)
#define NR_K_6(X)  X(0) X(1) X(2) X(3) X(4) X(5)
#define NR_K_8(X)  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define NR_K_16(X) NR_K_8(X) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
#define NR_K_32(X) NR_K_16(X) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
                              X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)

#define NR_V_1(X)  X(0)
#define NR_V_2(X)  X(0) X(1)
#define NR_V_4(X)  X(0) X(1) X(2) X(3)
#define NR_V_8(X)  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define NR_V_16(X) NR_V_8(X) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
#define NR_S_1(X)  X(0)
#define NR_S_2(X)  X(0) X(1)
#define NR_S_4(X)  X(0) X(1) X(2) X(3)
#define NR_S_8(X)  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define NR_S_16(X) NR_S_8(X) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
#define NR_T_1(X)  X(0)
#define NR_T_2(X)  X(0) X(1)
#define NR_T_4(X)  X(0) X(1) X(2) X(3)
#define NR_T_8(X)  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7)
#define NR_T_16(X) NR_T_8(X) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)

#define NR_FOR_I(n, X) NR_PASTE(NR_I_, n)(X)
#define NR_FOR_J(n, X) NR_PASTE(NR_J_, n)(X)
#define NR_FOR_K(n, X) NR_PASTE(NR_K_, n)(X)
#define NR_FOR_V(n, X) NR_PASTE(NR_V_, n)(X)
#define NR_FOR_S(n, X) NR_PASTE(NR_S_, n)(X)
#define NR_FOR_T(n, X) NR_PASTE(NR_T_, n)(X)

// How many contiguous elements one lane moves per memory instruction group.
// Eight f16 is 16 bytes, which is a full buffer_load_b128 per lane and 512 B
// per wave; eight e4m3 is one ds_write_b64. The number that matters is not the
// width but that the addresses are compile-time adjacent, so the loads issue
// together instead of the loop waiting once per element.
#define NR_VEC 8


// ---- quantisation onto the e4m3 grid --------------------------------------
//
// Round to nearest even, saturate at +-448 (infinity included), NaN stays NaN - the semantics
// of `cvt.rn.satfinite.e4m3x2.f16x2`, in integer and f32 arithmetic. Checked exhaustively
// against the host's reference over all 65536 f16 inputs. The f32 entry points round
// directly; an f16 input is already on f16's grid, so it is the same function.
#include "e4m3_emul.glsl"

f16vec2 nr_quant_pair(f16vec2 v) { return nr_e4m3_round_pair(v); }
f16vec2 nr_quant_pair32(vec2 x)  { return f16vec2(nr_quant_e4m3(x.x), nr_quant_e4m3(x.y)); }
f16vec2 nr_quant_pair_bare(f16vec2 v) { return nr_quant_pair(v); }
#define NR_HAVE_QUANT_PAIR32 1
#define NR_QUANT_PAIRED (NR_ACC_F16 == 0)

// Whole-fragment form of the above, for a conversion that was `NR_FRAG_E4M3(acc)`. Per-component
// work is coordinate-free, so it is valid whatever the layout.
#define NR_QUANT_FRAG(dst, src) { \
    for (uint nr_qi = 0u; nr_qi < uint((dst).length()); ++nr_qi) \
        (dst)[nr_qi] = nr_quant_e4m3(float((src)[nr_qi])); }

// `cc_vit_1d_ffn_expand_fp8`, where removing it takes the gold score from
// 98.35% to 0.06%.
float nr_mp_cubic_silu(float x) {
    const float t = clamp(x, -4.0, 4.0);
    return x * (-0.055908203125 * abs(t) * t + 0.447265625 * t + 0.89453125);
}

// Round an f32 to f16 precision and keep it in f32, round-to-nearest-even.
//
// Written on the bits because **the natural spellings do not survive this
// toolchain**. `float(float16_t(x))` is deleted outright, and so is
// `float(uint16BitsToFloat16(float16BitsToUint16(float16_t(x))))` - the
// bitcasts are no-ops in NIR, so ACO still sees f2f32(f2f16(x)) and folds it.
// A cooperative matrix conversion down and back is folded too, which is worth
// knowing because it looks like it should survive - an f16-accumulator probe
// reports it identical to the unrounded accumulator in 256 of 256 components.
// This is ours, not the hardware's: ROCm's clang compiled the same round trip
// for gfx1201 as `s_cvt_f16_f32` followed by `s_cvt_f32_f16`, both present.
// Measured, not suspected: two builds of gemm1x1 differing only in three such
// round trips produced **byte-identical ISA and byte-identical output**. A
// reference model that leans on an intermediate rounding therefore stops being
// that model without anything in the source changing.
//
// This is validation machinery. Our own paths deliberately carry f32 to the
// single narrowing the trained graph asks for; this exists to reproduce
// NVIDIA's f16 accumulator when the question is what *they* computed - the
// split-K f16 atomics and the attention softmax will both want it.
//
// Overflow past f16's range goes to infinity rather than saturating, which is
// what an f16 rounding does; the e4m3 quantiser saturates separately.
// The same rounding in four instructions, for the one place that can prove it
// does not need the other twenty-six: an FP8 matmul accumulator on its way to
// an e4m3 output.
//
// Adding 0x0FFF plus the surviving bit's own lsb and then clearing the low
// thirteen bits is exact round-to-nearest-even onto f16's ten mantissa bits,
// including the carry into the exponent. What it does not do is renormalise
// into f16's denormals or overflow to infinity, and here neither can be
// observed:
//
//   denormals  f16's smallest normal is 6.1e-05 and e4m3's smallest denormal is
//              1.95e-03, so everything this path keeps more precisely than f16
//              would is quantised to an e4m3 zero either way.
//   overflow   f16 stops at 65504 and e4m3 saturates at 448, so an infinity and
//              a large finite value both leave as 448.
//
// So this is not an approximation of nr_round_f16 in this context - it is the
// same function on the reachable domain. Anywhere the result is not immediately
// quantised to e4m3, use nr_round_f16 instead.
float nr_round_f16_e4m3(float x) {
    const uint u = floatBitsToUint(x);
#if NR_ROUND_HALF_UP
    // Two instructions instead of three: drop the `(u >> 13) & 1` term, which
    // exists only to break exact ties toward even. Round-half-away-from-zero
    // and round-half-even differ on precisely the inputs whose low thirteen
    // bits are 0x1000 and nowhere else - about one accumulator component in
    // 8192 for a sum with no particular alignment. The claim that this is free
    // is a claim about a score, so it is measured rather than argued.
    //
    // **It is measured, and it is not free. Default 0 and leave it there.**
    // The tie-break is biased - it always rounds away from zero - so unlike an
    // unbiased error it accumulates along a reduction instead of cancelling,
    // and the cost therefore grows with how much summing a kernel does:
    //
    //   fswin_t C=32   kernel/gold 91.3% -> 91.4%   (2 k-steps a rounding)
    //   fswin_t C=64               83.9% -> 83.8%
    //   fswin_t C=128              75.0% -> 74.6%
    //   ffwd3   C=512   per group  95.1/92.4/95.0/95.6/91.0/94.8/94.5/95.1
    //                          ->  93.2/89.1/92.8/93.2/87.0/92.8/92.4/93.2
    //                              and over-4-ulp components 602 -> 1327
    //
    // Two points a group on the three-stage chain is not a rounding detail.
    // What makes it tempting is that it is worth **1.88x on fswin_t at C=256**
    // (309.8 us -> 164.5 at the 1080p window count) and 1.25x at C=64, because
    // that kernel is dominated by this function. But C=256 is the one width
    // with no whole-block gold in the corpus, so the width where the win is
    // largest is the width where the error cannot be checked - and the trend
    // above says a longer reduction is where it gets worse, not better.
    // **Revisit only once an NVIDIA capture has a C=256 fused-Swin reference.**
    return uintBitsToFloat((u + 0x1000u) & 0xFFFFE000u);
#else
    return uintBitsToFloat((u + 0x0FFFu + ((u >> 13) & 1u)) & 0xFFFFE000u);
#endif
}

// The same rounding the hardware already has, in two instructions.
//
// gfx1201 has `v_cvt_f16_f32` and `v_cvt_f32_f16`, both round-to-nearest-even
// and both handling denormals and overflow exactly as f16 defines them - which
// is `nr_round_f16`'s entire 26-instruction body. The reason this project has
// a hand-written one at all is that **ACO deletes the round trip**: an f32 ->
// f16 -> f32 sequence is a no-op to an optimiser that is not told the narrowing
// is semantically required, and two builds of `gemm1x1` differing only in three
// such round trips came out byte-identical in ISA.
//
// `OpQuantizeToF16` is the SPIR-V operation that means exactly "round this f32
// to f16 precision and keep it an f32", and it is defined to be non-removable.
// glslang emits it for `unpackHalf2x16(packHalf2x16(v)).x`... but `packHalf2x16`
// on this part is `v_cvt_pkrtz_f16_f32`, round-toward-**zero**, so that spelling
// is the wrong function. The one that works is a round trip through a variable
// the optimiser cannot see through.
//
// Whether any spelling survives is a codegen question, not a language one, so
// NR_ROUND_MODE selects it and the fswin benchmark measures the result:
//   0  nr_round_f16_e4m3, the 3-instruction bit twiddle (default, measured)
//   1  the hardware converter via float16_t
//   2  nr_round_f16, the exact 26-instruction reference
//   3  a round trip through the **f16 Accumulator fragment type**, which is
//      packed two components to a VGPR - so if ACO emits it at all it is four
//      instructions for eight components rather than forty. Mode 1 showed the
//      scalar round trip gets deleted; a coopmat type conversion is a different
//      thing and may not be. NR_RND_FRAG below is the whole-fragment form,
//      because this one cannot be expressed per component.
#ifndef NR_ROUND_MODE
#define NR_ROUND_MODE 0
#endif
float nr_round_f16_hw(float x) {
    return float(float16_t(x));
}

float nr_round_f16(float x) {
    const uint u = floatBitsToUint(x);
    const uint sgn = u & 0x80000000u;
    const int  e   = int((u >> 23) & 0xFFu) - 127;
    if (e >= 128) return x;                                   // inf or NaN
    if (e > 15)   return uintBitsToFloat(sgn | 0x7F800000u);  // out of f16 range
    const int shift = (e < -14) ? (-1 - e) : 13;              // 13 + (-14 - e)
    if (shift > 24) return uintBitsToFloat(sgn);              // rounds to zero
    const uint m24  = (u & 0x7FFFFFu) | 0x800000u;
    const uint keep = m24 >> shift;
    const uint rest = m24 & ((1u << uint(shift)) - 1u);
    const uint tie = 1u << uint(shift - 1);
    const uint r = keep + ((rest > tie || (rest == tie && (keep & 1u) != 0u)) ? 1u : 0u);
    // Rounding a finite value can carry into the half infinity encoding.
    if (e == 15 && r == 2048u) return uintBitsToFloat(sgn | 0x7F800000u);
    // r has at most 12 bits and ldexp by a small exponent is exact, so the
    // reconstruction introduces no rounding of its own. A carry out of the
    // significand needs no renormalisation: the value it denotes is unchanged.
    const float v = ldexp(float(r), e - 23 + shift);
    return sgn != 0u ? -v : v;
}
