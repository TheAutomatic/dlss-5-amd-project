// SPDX-License-Identifier: MIT
// Shared declarations for the OpenNR-Denoiser kernels. Every buffer is reached by device address
// from the push constants; arithmetic that must match the reference bit for bit is `precise`.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_8bit_storage : require

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer U32Buf { uint v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer F32Buf { float v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 2) buffer F16Buf { float16_t v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 2) buffer U16Buf { uint16_t v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 8) buffer F16x4Buf { f16vec4 v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer F16x2Buf { f16vec2 v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) buffer U8x4Buf { u8vec4 v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 1) buffer U8Buf { uint8_t v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 8) buffer U64Buf { uint64_t v[]; };

// element i of a buffer through a 32-bit byte offset from its (uniform) base: global accesses with an SGPR base and a
// 32-bit VGPR offset instead of 64-bit address arithmetic per access
#define AT32(T, b, i, sz) T(uint64_t(b) + uint64_t(uint(i) * (sz)))

float fbits(uint b) { return uintBitsToFloat(b); }
float16_t hbits(uint b) { return uint16BitsToFloat16(uint16_t(b)); }

// Correctly rounded 1/x (one Newton step on the hardware reciprocal; the reference uses 1/x in fp32).
float rcp_rn(float x) {
    precise float r = 1.0 / x;
    precise float e = fma(-x, r, 1.0);
    return fma(e, r, r);
}

// fp32 -> fp16 rounded toward zero (the formatted surface stores of the reference GPU do this).
float16_t half_rz(float x) {
    float16_t h = float16_t(x);
    if (abs(float(h)) > abs(x)) h = uint16BitsToFloat16(uint16_t(float16BitsToUint16(h) - uint16_t(1)));
    return h;
}

// Exact fp32 value hi+lo (lo = rounding error of hi) -> fp16 with one round-to-nearest-even.
float16_t half_rn_ff(float hi, float lo) {
    float16_t h = float16_t(hi);
    precise float d = hi - float(h);                 // exact
    if (lo != 0.0 && d != 0.0) {
        // hi sits exactly on a tie of the fp16 grid: lo decides the side
        float16_t o = float16_t(hi + d);             // the other neighbour
        precise float d2 = float(o) - hi;
        if (abs(d) == abs(d2) && sign(lo) == sign(d2)) h = o;
    }
    return h;
}

// R10G10B10A2 UNORM store rule of the reference: truncate to 14 fractional bits, then nearest code.
uint unorm10(float v) {
    float c = clamp(v, 0.0, 1.0);
    uint t = uint(floor(c * 16384.0));
    return (t * 1023u + 8192u) >> 14;
}

uint pack1010102(vec3 v) {
    return unorm10(v.x) | (unorm10(v.y) << 10) | (unorm10(v.z) << 20) | (3u << 30);
}

// 10-bit code -> c/1023 correctly rounded (double division, then one rounding to fp32).
float unorm10_value(uint c) { return float(double(c) / 1023.0lf); }
