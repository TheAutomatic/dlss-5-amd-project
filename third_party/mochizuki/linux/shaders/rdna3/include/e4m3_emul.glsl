// e4m3 on a part with no FP8: the value set is kept, the storage is FP16.
//
// RDNA3 (gfx11) has no FP8 conversion and no FP8 WMMA. Every e4m3 value is an
// FP16 value (3 mantissa bits into 10, exponent range inside FP16's), so a
// network that is e4m3 at every operation boundary can hold those values in
// float16_t, multiply them in an FP16 WMMA with an FP32 accumulator, and get
// the same products the FP8 WMMA gets. Only the *narrowing* needs work, and it
// is done here in integer/f32 arithmetic with the semantics of
// `cvt.rn.satfinite.e4m3x2.f16x2`: round to nearest even, saturate at +-448
// (infinity included), NaN stays NaN.
//
// Checked on gfx1100 against tin::f_to_e4m3 / tin::e4m3_to_f over all 65536 f16 inputs and all
// 256 codes (linux/test/e4m3_emul_test), and against the bit-level reference over every f32 bit
// pattern and every pair of f16 patterns (linux/test/e4m3_round_test). A NaN input is not
// preserved: it comes out as +-448 or 0, which no activation of the network ever reaches.
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require

// A code byte (e4m3, 0x7F/0xFF are NaN) to the f16 with the same value. The
// exponent field is moved into f16's and the 2^8 bias difference is one exact
// multiply; subnormals come out right because f16's subnormal grid is finer.
// 0x7F/0xFF would land on a finite f16 (480), so the NaN is selected explicitly.
float16_t nr_e4m3_decode(uint code) {
    const uint h = ((code & 0x80u) << 8) | ((code & 0x7Fu) << 7);
    const float16_t v = unpackFloat2x16(h)[0] * float16_t(256.0);
    return (code & 0x7Fu) == 0x7Fu ? unpackFloat2x16(0x7E00u)[0] : v;
}

// NR_QUANT_F16_ONLY=1 (diagnostic, off by default): keep the clamp to +-448 and stop rounding onto the e4m3 grid, so a
// value leaves at the precision of the f16 it is stored in. The picture changes; this measures what the rounding costs.
#ifndef NR_QUANT_F16_ONLY
#define NR_QUANT_F16_ONLY 0
#endif

// Rounding onto the e4m3 grid by adding and subtracting a constant whose ulp is the grid step.
//
// Adding M = 1.5 * 2^(E+20) to x, where 2^E <= |x| < 2^(E+1), leaves a sum whose ulp is 2^(E-3):
// exactly three mantissa bits of x survive, rounded to nearest even by the adder, and subtracting
// M gives that value back. M is positive whatever the sign of x: x + M lies within 2^(E+1) of
// 1.5 * 2^(E+20), inside M's binade either way, so no sign has to be copied into it (one
// v_and_or_b32 a value). Bit-identical to the sign-matched constant over every f32 and f16 input.
// Below 2^-6 the grid is the fixed step 2^-9 of e4m3's subnormals, so the exponent is clamped
// to -6. |x| is first limited to 448, the largest finite e4m3 value, which is the saturation.
// Infinity becomes 448. `precise` keeps the compiler from cancelling the pair.
float nr_e4m3_round(float x) {
#if NR_QUANT_F16_ONLY
    return float(float16_t(clamp(x, -448.0, 448.0)));
#endif
    precise float c = clamp(x, -448.0, 448.0);
    const uint u = floatBitsToUint(c);
    const uint magic = max(u & 0x7F800000u, 0x3C800000u) + 0x0A400000u;
    precise float sum = c + uintBitsToFloat(magic);
    precise float r = sum - uintBitsToFloat(magic);
    return r;
}

// The same for two f16 values at once, in packed 16-bit arithmetic: ACO emits v_pk_max_f16 /
// v_pk_min_f16 for the limit, v_pk_max_u16 / v_pk_add_u16 for the constant and two v_pk_add_f16
// for the rounding, about four instructions a value. M = 1.5 * 2^(E+7), the f16 form of the above.
// Valid for any f16 pair; the two halves never interact.
f16vec2 nr_e4m3_round_pair(f16vec2 x) {
#if NR_QUANT_F16_ONLY
    return clamp(x, f16vec2(-448.0hf), f16vec2(448.0hf));
#endif
    precise f16vec2 c = clamp(x, f16vec2(-448.0hf), f16vec2(448.0hf));
    const u16vec2 u = float16BitsToUint16(c);
    const f16vec2 magic = uint16BitsToFloat16(max(u & u16vec2(0x7C00us), u16vec2(0x2400us)) + u16vec2(0x1E00us));
    precise f16vec2 sum = c + magic;
    precise f16vec2 r = sum - magic;
    return r;
}

// An f16 input is already on f16's grid, so both entry points are the same direct rounding;
// an f32 input is rounded once, straight onto the e4m3 grid.
float16_t nr_quant_e4m3(float16_t v) { return nr_e4m3_round_pair(f16vec2(v)).x; }
float16_t nr_quant_e4m3(float v)     { return float16_t(nr_e4m3_round(v)); }
