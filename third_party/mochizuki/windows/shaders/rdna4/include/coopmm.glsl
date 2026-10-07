// The only file in this project that names a cooperative matrix or an FP8 type.
//
// Decision 7, rule 1 of the architecture rules: all coopmat/FP8 use lives in one
// include with a fixed interface, so a D3D12/HLSL port rewrites this file and
// nothing else. Every other shader goes through the macros below and must not
// write `coopmat`, `floate4m3_t` or `gl_CooperativeMatrixLayout*` itself.
// A shader lint enforces that, along with the rest of decision 7's
// portability rules.
//
// The shape is fixed at 16x16x16 because that is the only shape gfx1201 has:
// all twenty configurations it reports are M=N=K=16 at subgroup scope, and the
// FP8 ones accumulate in FP32 only (queried from the device). K=32 FP8 is
// gfx1250; the K=32 shapes on gfx12 are the sparse V_SWMMAC_* variants, which
// NVIDIA's dense mma.sync does not use either.
#include "coherent_act.glsl"
#extension GL_KHR_cooperative_matrix : require
#extension GL_KHR_memory_scope_semantics : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_shuffle : require
#extension GL_EXT_float_e4m3 : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#if defined(NR_CVT4_SPLIT) && NR_CVT4_SPLIT == 3
#extension GL_EXT_spirv_intrinsics : require
#endif

#define NR_MMA_M 16
#define NR_MMA_N 16
#define NR_MMA_K 16

#define NR_E4M3 floate4m3_t
#define NR_F16  float16_t
#ifndef NR_ROUND_HALF_UP
#define NR_ROUND_HALF_UP 0
#endif

// NR_OPERAND_F16 swaps the operand element type for FP16 and changes nothing
// else. It is a *probe*, not a precision option: AMD's RDNA4 WMMA guide says an
// FP16 fragment load fills the 128-bit per-lane interface while FP8 uses only
// 64 bits of it, so building the identical kernel both ways and comparing bytes
// per second is the one measurement that says whether the FP8 operand path is
// at its own ceiling or at ours. The FP16 build computes the wrong answer, on
// purpose - it reinterprets e4m3 bytes as halves - and the host refuses to
// score it.
#ifdef NR_OPERAND_F16
#define NR_OPERAND NR_F16
#else
#define NR_OPERAND NR_E4M3
#endif
#define NR_FRAG_A   coopmat<NR_OPERAND, gl_ScopeSubgroup, 16, 16, gl_MatrixUseA>
#define NR_FRAG_B   coopmat<NR_OPERAND, gl_ScopeSubgroup, 16, 16, gl_MatrixUseB>
#define NR_FRAG_ACC coopmat<float,   gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>
#define NR_ACC_ZERO NR_FRAG_ACC(0.0)

// FP16 fragments and an FP16 accumulator, for the epilogue. Narrowing the f32
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

// An Accumulator whose components are **e4m3**. KHR restricts a fragment's
// *Use*, not its component type - `NR_FRAG_ACC16` already relies on that - so an
// epilogue can quantise in registers and store straight into the array the next
// stage loads its A operand from. No staging buffer, no read-back, no barrier:
// eight LDS instructions per fragment where the staged form needs twenty-four
// and two subgroup barriers. Measured on `fswin.comp`, where converting five
// epilogues was most of a 2x with byte-identical output.
//
// Probe it before relying on it on a new driver: glslang accepts the type and
// RADV lowers the store, but neither is required by the extension's wording.
#define NR_FRAG_E4M3 coopmat<NR_E4M3, gl_ScopeSubgroup, 16, 16, gl_MatrixUseAccumulator>

// ---- the fragment layout this target actually uses ----------------------
//
// KHR leaves the component-to-coordinate map implementation-defined, and every
// epilogue in this project that needed a *coordinate* rather than a value has
// paid for that with a trip through LDS. The map is not secret though, only
// unspecified: a layout probe kernel writes a distinct value into every
// component, stores the fragment RowMajor and reads the whole map back in one
// dispatch. On gfx1201 / RADV at wave32, a 16x16 Accumulator:
//
//     component c of lane l  ==  element (8 * (l / 16) + c, l % 16)
//
// A row is therefore sixteen consecutive lanes at a single component, so a
// row-wise reduction is a butterfly over the low four lane bits - no memory, no
// barrier, and every lane ends holding the sums for exactly the rows its own
// components need. A column, by contrast, is eight components of one lane and
// needs nothing at all.
//
// **This is the only layout assumption in the project and it lives here**, in
// the one file a D3D12 port rewrites. Re-run the probe when the driver or the
// target changes; `NR_ACC_ROW_LANES` is wrong rather than merely slow if the
// map moves.
#define NR_ACC_ROW_LANES 16u
#define NR_ACC_ROW_REDUCE(v) { \
    (v) += subgroupShuffleXor((v), 1u); \
    (v) += subgroupShuffleXor((v), 2u); \
    (v) += subgroupShuffleXor((v), 4u); \
    (v) += subgroupShuffleXor((v), 8u); }

// The same freedom on the A side: an operand already in registers as e4m3
// becomes the f16 operand a diagonal residual MMA needs, with no memory round
// trip. `NR_FRAG_A16(a)` is the spelling; this name exists so the rule-1 lint
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

// ---- the requant path has no shorter spelling on gfx1201 -------------------
//
// Settled by **disassembling the encoding space**, not by reading a manual or
// by trying mnemonics: sweep every opcode of a class with a fixed operand
// pattern through `llvm-mc -mcpu=gfx1201` and see which ones decode.  VOP3P is
// 7 bits at [22:16] under `0xcc`, VOP3 is 10 bits at [25:16] under `0xd4`,
// VOP1 is 8 bits at [16:9] under `0x7e`.
//
//   * **VOP3P has 56 valid opcodes** and the complete f16 arithmetic set is
//     `pk_fma / pk_add / pk_mul / pk_min_num / pk_max_num / pk_minimum /
//     pk_maximum` plus `fma_mix{,lo,hi}`, `dot2_f32_f16` and the WMMA block.
//     There is **no packed fused min-max in any spelling** - no
//     `v_pk_maxmin_num_f16`, no `v_pk_minmax_num_f16`, no `v_pk_med3_f16`.
//     NVIDIA's `HMNMX2` has no counterpart here, so a `clamp` on an `f16vec2`
//     is two instructions and stays two.  The fused form that *does* exist is
//     VOP3 `v_minmax_num_f16` (0x26a) / `v_maxmin_num_f16` (0x26b), which is
//     one f16 per instruction - the same two per pair - and ACO already emits
//     it where the clamp is scalar (80 of them in `attn`).
//   * **There is no `v_cvt_pk_f32_f16`** anywhere in VOP1, VOP3 or VOP3P.  The
//     widen back to f32 for the fp8 converter is one instruction per value and
//     cannot be halved.
//   * The only packed narrowing is `v_cvt_pk_rtz_f16_f32` (VOP2 0x2f / VOP3
//     0x12f), i.e. **round-toward-zero only** - that is `NR_PKN`, priced and
//     default 0.  There is no packed RNE narrowing.
//   * The only e4m3 producers are `v_cvt_pk_fp8_f32` (0x369) and
//     `v_cvt_sr_fp8_f32` (0x36b); both take f32.  `v_cvt_pk_fp8_f16` and
//     `v_cvt_f16_fp8` do not exist (re-confirmed against the full table).
//     f32 is the only bridge into e4m3 on this part.
//
// **And the packed min/max in this family is not the requant clamp at all.**
// The shipping C=32 kernels are `NR_QUANT_MODE=4`, whose saturation is the
// MODE register, not a clamp.  Ablating the two clamps that are left accounts
// for every matched pair of the 516 `v_pk_min/max_num_f16` in `g_fswin32`:
// 256 are `nr_swin_act2`'s `clamp(x, +-4)` and 128 are `nr_swin_exp2`'s
// `clamp(y, 1.03125, 1.5693359375)` - both of which *define* the functions
// NVIDIA's PTX computes - and the remaining 132 are `v_pk_max_num_f16` with no
// `min` beside them.  The requant tax at C=32 is the **1489 conversions**,
// 39.8% of VALU, not the 53% a min/max-inclusive count gives.
//
// The quantiser to match is the shipping kernels' `cvt.rn.satfinite.e4m3x2.f16x2`:
// from f16, round-to-nearest-even, saturating rather than overflowing.
//
// Measurement 4 of the architecture notes is why the guard is not optional. The
// driver's implicit conversion turns every finite f16 above 448 into NaN -
// 14,718 of the 65,536 bit patterns - and one NaN destroys the whole
// accumulator tile of the next MMA. Clamping without the NaN branch is worse
// than it looks: clamp is min(max(v, lo), hi), so a NaN comes out as -448, with
// the sign wrong as well as the value. Guarded is 65536/65536 exact.
// Three ways to reach e4m3, because the guard and the f16 detour around it are
// **54% of every VALU instruction** in `fswin_t.comp` while the conversion
// itself - `v_cvt_pk_fp8_f32`, which takes f32 directly and two at a time - is
// under a third of that. Measured on the transposed kernel: 608 conversions
// against 640 `v_cvt_f16_f32`/`v_cvt_f32_f16`, 525 for the NaN branch and 320
// for a clamp done in the narrower type for no reason.
//
//   2  guarded, via f16 - the original, and what every scored result until now
//      was produced with
//   1  via f16, no NaN branch
//   0  straight from f32, one rounding instead of two
//
// Which is correct is a question about the *inputs*, not about the arithmetic:
// the guard exists because the driver's implicit conversion turns finite f16
// above 448 into NaN, and a clamp in f32 handles that case without a branch.
// Default 1. The NaN branch is **provably dead on this corpus**: every one of
// the four kernels scores bit-identically with and without it - fswin_t on all
// nine input variants, attn 99.91%, ffwd3 94.17%, gemm1x1's three families -
// and it costs 22% of `fswin_t.comp`. What the guard was for is finite f16
// above 448, and mode 1 still clamps those; only a NaN *input*, which no real
// weight produces, now comes out as -448 rather than propagating. Set 2 to put
// it back.
#ifndef NR_QUANT_MODE
#define NR_QUANT_MODE 1
#endif
#if NR_QUANT_MODE == 2
NR_E4M3 nr_quant_e4m3(NR_F16 v) {
    if (isnan(v)) return NR_E4M3(v);
    return NR_E4M3(clamp(v, NR_F16(-448.0), NR_F16(448.0)));
}
#elif NR_QUANT_MODE == 1
NR_E4M3 nr_quant_e4m3(NR_F16 v) {
    return NR_E4M3(clamp(v, NR_F16(-448.0), NR_F16(448.0)));
}
#elif NR_QUANT_MODE == 4 || NR_QUANT_MODE == 5
// Defined below, after the f32 entry point it delegates to.
#elif NR_QUANT_MODE == 3
// No clamp at all - prices it. **Not safe to ship**: the driver's implicit
// conversion turns a finite f16 above 448 into NaN, and one NaN destroys the
// whole accumulator tile of the next MMA.
NR_E4M3 nr_quant_e4m3(NR_F16 v) { return NR_E4M3(v); }
#else
NR_E4M3 nr_quant_e4m3(NR_F16 v) {
    return NR_E4M3(clamp(float(v), -448.0, 448.0));
}
#endif
#if NR_QUANT_MODE == 0
// The f32 entry point the mode exists for: no narrowing at all on the way in.
NR_E4M3 nr_quant_e4m3(float v) {
    return NR_E4M3(clamp(v, -448.0, 448.0));
}
#endif

// ---- mode 4: let the hardware saturate ------------------------------------
//
// The clamp to +-448 is **768 `v_maxmin_num_f32` per lane-window at C=32**,
// three times the WMMA count in a kernel whose instruction count is its time.
// (That once read "448, four times the WMMA count"; a fixed ISA histogram
// counts 768 against 256 `v_wmma` in the mode-0
// build - RADV's disassembler desynchronised on the 8-byte converts and lost
// 13.7% of C=32's instructions, so no opcode count taken with it is real.)
// `SPV_EXT_float8` has a saturating conversion for exactly this, and RDNA4 has
// a MODE-register bit (`FP16_OVFL`) that does it with no VALU at all.
//
// Reaching the free path is not straightforward. ACO lowers a plain
// `f2e4m3fn_sat` to `v_minimummaximum_f32` + `v_cvt_pk_fp8_f32` - the same cost
// as the hand-written clamp, because the ISA documentation for FP16_OVFL is
// wrong about Inf. There is a second opcode, `f2e4m3fn_satfn` ("saturate
// finite"), which lowers to a bare `v_cvt_pk_fp8_f32` with the mode bit set,
// and `nir_opt_algebraic` only produces it from one pattern - written for
// vkd3d-proton - which is `f2e4m3fn_sat(bcsel(feq(fabs(x), inf), NaN, x))`.
// Hence the `isinf` spelling below: it is not defensive, it is the shape the
// optimiser matches.
//
// This is our finite-input fast path, NOT full NVIDIA `cvt.rn.satfinite`
// semantics: finite overflow saturates to 448, but Inf becomes NaN. NVIDIA's
// satfinite conversion instead maps Inf to sign-preserved 448 and preserves
// NaN. The original C128 SASS and actual GPU probes confirm this boundary
// mismatch. Whether these nonfinite
// inputs occur in the complete graph has not yet been established.
//
// **Whether the free path exists on the installed driver is a measurement, not
// a reading.** Count `v_maxmin_num_f32` with an ISA histogram (not
// with `grep` on RADV's listing): if it does not fall to **zero** the pattern
// did not match and this mode is the clamp with extra steps. Measured on
// Mesa 26.2.2, C=32: mode 0 has 768 and no `s_setreg`, mode 4 has **0
// `v_maxmin_num_f32` and 343 `s_setreg_imm32_b32`** for its 384
// `v_cvt_pk_fp8_f32`, mode 5 has neither.
// NR_ABLATE_CVT (diagnostic, wrong output, fswin_t pipelines only - they enable int8): the byte
// taken from the value's top bits instead of a conversion, so no MODE write, no convert and no
// clamp. With NR_ABLATE_QUANT (clamp dropped, convert kept) it prices the quantiser on LLPC.
#ifndef NR_ABLATE_CVT
#define NR_ABLATE_CVT 0
#endif
// NR_CVT4_SPLIT (Windows): a four-wide e4m3 conversion as two pair conversions. The driver's
// LLPC (true16) converts the second pair into the high half of a *copy* of the first pair's
// result and merges the two with v_bfi_b32: two extra VALU a dword (fswin32: 568 + 514), the same
// for cooperative-matrix conversions. Same conversions, same bytes; only the IR shape changes.
// 1: two fe4m3vec2 put together - shift + and_or instead, no gain. 3: each pair bit-cast to an
// f16 and the two built as an f16 pair - about one v_perm a dword, but every pair now has its own
// MODE write (fswin32 621 -> 1237 s_setreg): faster only in the two-wave image blocks
// (fswinimagepost32 4K -0.046 ms, preds32 -0.008), slower in fswin32 and the persistent runs.
// (An int16 spelling, pack16 + u16vec2, was a shift/perm mess.)
#ifndef NR_CVT4_SPLIT
#define NR_CVT4_SPLIT 0
#endif
#if NR_CVT4_SPLIT == 3
// Each half bit-cast to an f16 (OpBitcast), the two put side by side as an f16 pair, cast back.
spirv_instruction(id = 124) float16_t nr_bc_e2_h(fe4m3vec2 v);
spirv_instruction(id = 124) fe4m3vec4 nr_bc_h2_e4(f16vec2 v);
fe4m3vec4 nr_cvt4_split(vec4 x) {
    return nr_bc_h2_e4(f16vec2(nr_bc_e2_h(fe4m3vec2(x.xy)), nr_bc_e2_h(fe4m3vec2(x.zw))));
}
fe4m3vec4 nr_cvt4_split(f16vec4 x) {
    return nr_bc_h2_e4(f16vec2(nr_bc_e2_h(fe4m3vec2(x.xy)), nr_bc_e2_h(fe4m3vec2(x.zw))));
}
#elif NR_CVT4_SPLIT
fe4m3vec4 nr_cvt4_split(vec4 x) {
    const fe4m3vec2 p = fe4m3vec2(x.xy), q = fe4m3vec2(x.zw);
    return fe4m3vec4(p.x, p.y, q.x, q.y);
}
fe4m3vec4 nr_cvt4_split(f16vec4 x) {
    const fe4m3vec2 p = fe4m3vec2(x.xy), q = fe4m3vec2(x.zw);
    return fe4m3vec4(p.x, p.y, q.x, q.y);
}
#endif
#if NR_CVT4_SPLIT
#define NR_CVT4(x) nr_cvt4_split(x)
#else
#define NR_CVT4(x) fe4m3vec4(x)
#endif
#if NR_ABLATE_CVT
fe4m3vec4 nr_fake4(vec4 x) {
    const uvec4 u = floatBitsToUint(x) >> 24u;
    return fe4m3vec4(uintBitsToFloate4m3EXT(uint8_t(u.x)), uintBitsToFloate4m3EXT(uint8_t(u.y)),
                     uintBitsToFloate4m3EXT(uint8_t(u.z)), uintBitsToFloate4m3EXT(uint8_t(u.w)));
}
#endif
#if NR_QUANT_MODE == 4
NR_E4M3 nr_quant_e4m3(float v) {
#if defined(NR_QUANT_EXPLICIT) && NR_QUANT_EXPLICIT
    // See nr_e4m3_range below: clamp and inf/NaN -> NaN in f32, then a plain conversion.
    return NR_E4M3(clamp(v, -448.0, 448.0) + v * 0.0);
#else
    NR_E4M3 r;
    saturatedConvertEXT(r, isinf(v) ? uintBitsToFloat(0x7FC00000u) : v);
    return r;
#endif
}
NR_E4M3 nr_quant_e4m3(NR_F16 v) { return nr_quant_e4m3(float(v)); }
#endif

// Which modes have a paired `nr_quant_pair`. Mode 4 reaches the MODE-register
// saturating convert; mode 5 clamps the half pair instead and needs no MODE
// register at all. Everything else falls back to the scalar path.
#define NR_QUANT_PAIRED ((NR_QUANT_MODE == 4 || NR_QUANT_MODE == 5) && NR_ACC_F16 == 0)

#if NR_QUANT_MODE == 5
// **The clamp in packed half, and no MODE register.**
//
// Mode 4 spells the clamp as the hardware's saturating conversion, which this
// driver lowers to `s_setreg(MODE.FP16_OVFL)` around every convert - **343 MODE
// writes for 384 converts at C=32** (this said "163 for 182", which is the
// old disassembler undercounting by half), i.e. ACO is not merging them,
// and batching the conversions was confirmed not to make it. An
// `s_setreg` drains the SIMD pipeline and is the one stall occupancy cannot
// hide.
//
// Mode 0 avoids the MODE register by clamping, but clamps in **f32 scalar** -
// four to five instructions a pair - which is why it measures slower despite
// having no MODE writes. The value here is already an `f16vec2` and gfx1201
// has `v_pk_max_f16`/`v_pk_min_f16`, so the same clamp is **two** packed
// instructions a pair and needs no mode switch.
//
// **That last sentence is the design and not the codegen.** The mode-5
// build at C=32 emits **708 `v_pk_max_num_f16` + 576 `v_pk_min_num_f16` for
// 381 pairs** - 3.4 packed instructions a pair, not two - plus 908
// `v_cvt_f32_f16` and 741 `v_cvt_f16_f32` against mode 4's 524 and 549,
// because the value reaches this function as f32 and has to be narrowed for
// the packed clamp and widened again for `v_cvt_pk_fp8_f32`. Total 5977
// instructions against mode 4's 4752: **+1225**, where an earlier record says
// +614. It is a fidelity lever.
//
// 448 is e4m3's largest finite value and is exact in f16, so the clamp is
// lossless for everything in range. Against mode 4 the difference is the
// nonfinite classes: mode 4 maps Inf to NaN deliberately, this maps it to
// sign-preserved 448 - which is what NVIDIA's own `cvt.rn.satfinite` does.
fe4m3vec2 nr_quant_pair(f16vec2 v) {
    return fe4m3vec2(clamp(v, f16vec2(-448.0hf), f16vec2(448.0hf)));
}
NR_E4M3 nr_quant_e4m3(NR_F16 v) {
    return NR_E4M3(clamp(v, NR_F16(-448.0), NR_F16(448.0)));
}
NR_E4M3 nr_quant_e4m3(float v) { return nr_quant_e4m3(NR_F16(v)); }
fe4m3vec2 nr_quant_pair32(vec2 x) {
    return nr_quant_pair(f16vec2(clamp(x, vec2(-448.0), vec2(448.0))));
}
#define NR_HAVE_QUANT_PAIR32 1
#endif

#if NR_QUANT_MODE == 4
// Keep both scalar half values and mode-4 saturation semantics, but present
// them together so ACO can use both inputs of v_cvt_pk_fp8_f32. SQTT
// identified scalar conversions with a zero second operand; full graph
// validation and actual Mesa 26.2.2 code generation were checked.
// NR_ABLATE_QUANT is diagnostic only and produces wrong output: it drops the
// saturation the network needs (NVIDIA's own 1080p capture has 2.38 M bytes at
// exactly 0x7E). It exists to price the conversion in instructions - see the
// ablation block at the top of swin_math.glsl.
#ifndef NR_ABLATE_QUANT
#define NR_ABLATE_QUANT 0
#endif
// NR_QUANT_EXPLICIT=1 spells the same conversion without the saturating form: clamp to the
// e4m3 range and send inf/NaN to NaN in f32, then a plain (in-range) conversion. Identical
// bytes. LLPC (AMD's Windows driver) writes the MODE register around every saturating
// convert; the plain one needs no mode change.
#ifndef NR_QUANT_EXPLICIT
#define NR_QUANT_EXPLICIT 0
#endif
#if NR_QUANT_EXPLICIT
// clamp(x) + x*0: x*0 is a signed zero for finite x (the clamp passes unchanged, -0 stays -0)
// and NaN for inf or NaN, so inf/NaN become NaN without a compare-and-select. LLPC's
// fast-math flags carry no `ninf`, so x*0 cannot be folded to 0.
vec2 nr_e4m3_range(vec2 x) {
    return clamp(x, vec2(-448.0), vec2(448.0)) + x * 0.0;
}
#endif
fe4m3vec2 nr_quant_pair(f16vec2 v) {
#if NR_ABLATE_QUANT
    return fe4m3vec2(v);
#elif NR_QUANT_EXPLICIT
    return fe4m3vec2(nr_e4m3_range(vec2(v)));
#else
    vec2 x = vec2(v);
    fe4m3vec2 r;
    saturatedConvertEXT(r, mix(x, vec2(uintBitsToFloat(0x7FC00000u)), isinf(x)));
    return r;
#endif
}

// The same conversion for a value that is already f32, so the caller does not
// pay the narrowing and the widening back.
//
// **This is a real hardware gap, not a missed optimisation.** NVIDIA's ISA has
// `cvt.rn.satfinite.e4m3x2.f16x2` - one instruction from a half pair - and
// their accumulator is f16 already, so a value goes accumulator -> activation
// -> e4m3 without a single format conversion. gfx1201 has only
// `v_cvt_pk_fp8_f32`: checked against the driver's own opcode table, there is
// no f16 source at any level. So every value we route through f16 costs two
// `v_cvt_f32_f16` to get back, and at C=32 that is 384 instructions - 11.7% of
// the shader - on top of the narrowing that put it in f16 in the first place.
//
// Going through f16 is therefore only worth it where NVIDIA's *arithmetic*
// needs it - the packed activation and the exponential's half bit-transform
// are theirs and must round the way theirs does. Where the half hop exists
// only as a way to reach the converter, this is the cheaper spelling.
fe4m3vec2 nr_quant_pair32(vec2 x) {
#if NR_QUANT_EXPLICIT
    return fe4m3vec2(nr_e4m3_range(x));
#else
    fe4m3vec2 r;
    saturatedConvertEXT(r, mix(x, vec2(uintBitsToFloat(0x7FC00000u)), isinf(x)));
    return r;
#endif
}
#define NR_HAVE_QUANT_PAIR32 1
#if NR_QUANT_EXPLICIT && !NR_ABLATE_QUANT
// Two pairs as one four-wide conversion, element for element nr_quant_pair / nr_quant_pair32:
// LLPC writes MODE once and fills one dword with two converts (op_sel), where two pairs cost
// two MODE writes and a shift + and-or to pack. windows/build/quad_quant_glsl.py rewrites the
// pair loops to these.
fe4m3vec4 nr_quant_quad(f16vec2 a, f16vec2 b) {
#if NR_ABLATE_CVT
    return nr_fake4(vec4(vec2(a), vec2(b)));
#endif
    const vec4 x = vec4(vec2(a), vec2(b));
    return NR_CVT4(clamp(x, vec4(-448.0), vec4(448.0)) + x * 0.0);
}
fe4m3vec4 nr_quant_quad32(vec2 a, vec2 b) {
#if NR_ABLATE_CVT
    return nr_fake4(vec4(a, b));
#endif
    const vec4 x = vec4(a, b);
    return NR_CVT4(clamp(x, vec4(-448.0), vec4(448.0)) + x * 0.0);
}
#elif NR_QUANT_EXPLICIT && NR_ABLATE_QUANT
// Diagnostic: the quad forms without the range step (wrong output; prices the clamp).
fe4m3vec4 nr_quant_quad(f16vec2 a, f16vec2 b) { return fe4m3vec4(vec4(vec2(a), vec2(b))); }
fe4m3vec4 nr_quant_quad32(vec2 a, vec2 b) { return fe4m3vec4(vec4(a, b)); }
#endif
#endif

// Two plain (in-range) conversions as one four-wide one; windows/build/quad_quant_glsl.py.
#if NR_ABLATE_CVT
fe4m3vec4 nr_convert_quad(f16vec2 a, f16vec2 b) { return nr_fake4(vec4(vec2(a), vec2(b))); }
fe4m3vec4 nr_convert_quad(vec2 a, vec2 b) { return nr_fake4(vec4(a, b)); }
#else
fe4m3vec4 nr_convert_quad(f16vec2 a, f16vec2 b) { return NR_CVT4(f16vec4(a, b)); }
fe4m3vec4 nr_convert_quad(vec2 a, vec2 b) { return NR_CVT4(vec4(a, b)); }
#endif

// The same conversion with the **saturation** removed and nothing else: no
// `isinf` select, no MODE-register write around the convert, just the
// narrowing the hardware forces. Diagnostic only - it drops the clamp the
// network needs, exactly as `NR_ABLATE_QUANT` does - and it exists so that
// `fswin_t.comp`'s `NR_ABLATE_QSITE` can price one quantisation site at a time
// without moving any operand's type. The conversion itself cannot be ablated at
// a site whose product has a weight on the other side, because an e4m3 value
// still has to be produced; that half of the ledger is measured by
// `NR_ATT_F16` instead, where the type really does change.
fe4m3vec2 nr_quant_pair_bare(f16vec2 v) { return fe4m3vec2(v); }

// Every mode has a paired f32 entry point, so a caller never has to know which
// quantiser it was built with. Modes 4 and 5 have a native one; the rest get
// the scalar path twice. Without this, `NR_ACT_F32` compiled under mode 4 and
// failed the round-fp16 build, which forces mode 0.
#ifndef NR_HAVE_QUANT_PAIR32
fe4m3vec2 nr_quant_pair32(vec2 x) {
    // The f16 overload, because it is the one every mode defines - only 0, 4
    // and 5 have an `nr_quant_e4m3(float)`, and `upsample_blend.comp` includes
    // this header at the default mode.
    return fe4m3vec2(nr_quant_e4m3(NR_F16(x.x)), nr_quant_e4m3(NR_F16(x.y)));
}
#endif

// Four (two) nr_quant_e4m3(NR_F16) as one conversion, the same bytes in every mode. LLPC makes
// each scalar conversion its own MODE write + convert + byte packing; four at once is one MODE
// write and two converts into one dword (windows/build/quad_quant_glsl.py has the why).
fe4m3vec4 nr_quant4_h(f16vec4 v) {
#if NR_ABLATE_CVT
    return nr_fake4(vec4(v));
#endif
#if NR_QUANT_MODE == 1 || NR_QUANT_MODE == 5
    return NR_CVT4(clamp(v, f16vec4(-448.0), f16vec4(448.0)));
#elif NR_QUANT_MODE == 4 && defined(NR_QUANT_EXPLICIT) && NR_QUANT_EXPLICIT && !NR_ABLATE_QUANT
    const vec4 x = vec4(v);
    return NR_CVT4(clamp(x, vec4(-448.0), vec4(448.0)) + x * 0.0);
#else
    return fe4m3vec4(nr_quant_e4m3(v.x), nr_quant_e4m3(v.y), nr_quant_e4m3(v.z), nr_quant_e4m3(v.w));
#endif
}
fe4m3vec2 nr_quant2_h(f16vec2 v) {
#if NR_QUANT_MODE == 1 || NR_QUANT_MODE == 5
    return fe4m3vec2(clamp(v, f16vec2(-448.0), f16vec2(448.0)));
#else
    return fe4m3vec2(nr_quant_e4m3(v.x), nr_quant_e4m3(v.y));
#endif
}

// MpCubicSiluActivation, recovered exactly from 1936 call sites across five
// modules. Verified inside the fused Swin block, and again on
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
// This is validation machinery. Our own paths carry f32 to a single
// narrowing; this exists to reproduce
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

// ---- NR_F16_MMA: quantise to the e4m3 grid, continue as f16 ---------------
//
// The f16xf16->f16 WMMA path (`NR_F16_MMA=1` in `windows/shaders/rdna4/fswin_t.comp`) keeps
// the network's arithmetic - every operand is still a value on the e4m3 grid -
// but never materialises the byte. Every e4m3 value is exactly representable
// in f16 and the product of two of them needs 8 significant bits, so an f16
// operand pair computes the same products; only the accumulation rounds
// differently.
//
// This is the correct-by-construction spelling: the hardware's own e4m3
// rounding, then the exact widening back. It is a round trip
// (`v_cvt_pk_fp8_f32` + `v_cvt_pk_f32_fp8` + a narrowing) where an integer-bit
// form would be cheaper; the instruction count of this version is what the
// round measures, and the bit form is a later question.
#if defined(NR_F16_MMA) && NR_F16_MMA
f16vec2 nr_requant_pair(f16vec2 v) {
    const fe4m3vec2 q = nr_quant_pair(v);
    return f16vec2(NR_F16(q.x), NR_F16(q.y));
}
#endif
