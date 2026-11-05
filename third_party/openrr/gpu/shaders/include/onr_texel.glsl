// SPDX-License-Identifier: MIT
// Texels of a D3D12 placed footprint (row pitch in bytes, rows packed from byte 0) read and written as vec4.
// Format codes are the runtime's (runtime/src/formats.hpp), not DXGI's.
#define TF_RGBA16F 1u
#define TF_RGBA32F 2u
#define TF_R11G11B10F 3u
#define TF_RGB10A2 4u
#define TF_RGBA8 5u
#define TF_BGRA8 6u
#define TF_RG16F 7u
#define TF_RG32F 8u
#define TF_R16F 9u
#define TF_R32F 10u
#define TF_R8 11u
#define TF_R16 12u
#define TF_D24 13u
#define TF_D32S8 14u
#define TF_RGBA16 15u
#define TF_RGBA8_SNORM 16u
#define TF_RGBA16_SNORM 17u
#define TF_RG8 18u
#define TF_RG16 19u
#define TF_RGBA8_SRGB 20u
#define TF_BGRA8_SRGB 21u
#define TF_RG16_SNORM 22u
#define TF_RGB9E5 23u

uint tf_bytes(uint f) {
    switch (f) {
    case TF_RGBA32F: return 16u;
    case TF_RGBA16F: case TF_RG32F: case TF_D32S8: case TF_RGBA16: case TF_RGBA16_SNORM: return 8u;
    case TF_R16F: case TF_R16: case TF_RG8: return 2u;
    case TF_R8: return 1u;
    default: return 4u;
    }
}

float uf11(uint v) { return unpackHalf2x16(v << 4).x; }
float uf10(uint v) { return unpackHalf2x16(v << 5).x; }
float srgb_to_linear(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float linear_to_srgb(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float sn8(uint b) { return max(float(int(b << 24) >> 24) / 127.0, -1.0); }
float sn16(uint b) { return max(float(int(b << 16) >> 16) / 32767.0, -1.0); }
// R9G9B9E5_SHAREDEXP: mantissa * 2^(e - 24), no implied one; every value is exact in fp32 (and in fp16)
vec4 rgb9e5(uint u) {
    float s = uintBitsToFloat(((u >> 27) + 103u) << 23);
    return vec4(float(u & 511u) * s, float((u >> 9) & 511u) * s, float((u >> 18) & 511u) * s, 1.0);
}

vec4 tf_read(U32Buf b, uint pitch, uint f, ivec2 p) {
    uint a = uint(p.y) * pitch + uint(p.x) * tf_bytes(f), w = a >> 2;
    uint u0 = b.v[w];
    switch (f) {
    case TF_RGBA16F: return vec4(unpackHalf2x16(u0), unpackHalf2x16(b.v[w + 1u]));
    case TF_RGBA32F: return vec4(uintBitsToFloat(u0), uintBitsToFloat(b.v[w + 1u]), uintBitsToFloat(b.v[w + 2u]), uintBitsToFloat(b.v[w + 3u]));
    case TF_R11G11B10F: return vec4(uf11(u0 & 0x7ffu), uf11((u0 >> 11) & 0x7ffu), uf10(u0 >> 22), 1.0);
    case TF_RGB10A2: return vec4(float(u0 & 1023u), float((u0 >> 10) & 1023u), float((u0 >> 20) & 1023u), float(u0 >> 30) * 341.0) / 1023.0;
    case TF_RGBA8: return unpackUnorm4x8(u0);
    case TF_BGRA8: return unpackUnorm4x8(u0).bgra;
    case TF_RGBA8_SRGB: { vec4 c = unpackUnorm4x8(u0); return vec4(srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b), c.a); }
    case TF_BGRA8_SRGB: { vec4 c = unpackUnorm4x8(u0).bgra; return vec4(srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b), c.a); }
    case TF_RG16F: return vec4(unpackHalf2x16(u0), 0.0, 1.0);
    case TF_RG32F: return vec4(uintBitsToFloat(u0), uintBitsToFloat(b.v[w + 1u]), 0.0, 1.0);
    case TF_R16F: return vec4(unpackHalf2x16(u0 >> ((a & 2u) * 8u)).x, 0.0, 0.0, 1.0);
    case TF_R32F: case TF_D32S8: return vec4(uintBitsToFloat(u0), 0.0, 0.0, 1.0);
    case TF_R8: return vec4(float((u0 >> ((a & 3u) * 8u)) & 255u) / 255.0, 0.0, 0.0, 1.0);
    case TF_R16: return vec4(float((u0 >> ((a & 2u) * 8u)) & 65535u) / 65535.0, 0.0, 0.0, 1.0);
    case TF_RG8: { uint h = (u0 >> ((a & 2u) * 8u)) & 65535u; return vec4(float(h & 255u) / 255.0, float(h >> 8) / 255.0, 0.0, 1.0); }
    case TF_D24: return vec4(float(u0 & 0xffffffu) / 16777215.0, 0.0, 0.0, 1.0);
    case TF_RGBA16: return vec4(unpackUnorm2x16(u0), unpackUnorm2x16(b.v[w + 1u]));
    case TF_RGBA8_SNORM: return vec4(sn8(u0), sn8(u0 >> 8), sn8(u0 >> 16), sn8(u0 >> 24));
    case TF_RGBA16_SNORM: { uint u1 = b.v[w + 1u]; return vec4(sn16(u0), sn16(u0 >> 16), sn16(u1), sn16(u1 >> 16)); }
    case TF_RG16: return vec4(unpackUnorm2x16(u0), 0.0, 1.0);
    case TF_RG16_SNORM: return vec4(sn16(u0), sn16(u0 >> 16), 0.0, 1.0);
    case TF_RGB9E5: return rgb9e5(u0);
    }
    return vec4(0.0);
}

uint f11_of(float x) { uint h = packHalf2x16(vec2(clamp(x, 0.0, 65000.0), 0.0)) & 0x7fffu; return min((h + 8u) >> 4, 0x7bfu); }
uint f10_of(float x) { uint h = packHalf2x16(vec2(clamp(x, 0.0, 65000.0), 0.0)) & 0x7fffu; return min((h + 16u) >> 5, 0x3dfu); }

// float -> R9G9B9E5_SHAREDEXP with round-to-zero, the D3D rule for float -> float conversions (D3D11.3 functional spec
// 3.2.2) and what the GPU's own typed store of the format gives: each channel clamped to [0, 65408] (NaN to 0), the
// shared exponent max(-16, floor(log2(largest))) + 16, the mantissas c / 2^(e - 24) truncated. Exact for any fp32.
uint e5_mant(float x, uint e) {
    uint b = floatBitsToUint(x), be = b >> 23, sh = 126u + e - be;   // x / 2^(e - 24) = (b's mantissa | 1 << 23) >> sh
    return be == 0u || sh > 31u ? 0u : ((b & 0x7fffffu) | 0x800000u) >> sh;
}
uint rgb9e5_of(vec3 c) {
    c = vec3(c.x > 0.0 ? min(c.x, 65408.0) : 0.0, c.y > 0.0 ? min(c.y, 65408.0) : 0.0, c.z > 0.0 ? min(c.z, 65408.0) : 0.0);
    uint e = max(floatBitsToUint(max(c.x, max(c.y, c.z))) >> 23, 111u) - 111u;
    return e5_mant(c.x, e) | e5_mant(c.y, e) << 9 | e5_mant(c.z, e) << 18 | e << 27;
}

// formats with at least 4 bytes per texel only; every output but an rgba16f one with a UAV is written this way and
// copied into its texture (R9G9B9E5 included)
void tf_write(U32Buf b, uint pitch, uint f, ivec2 p, vec4 c) {
    uint w = (uint(p.y) * pitch + uint(p.x) * tf_bytes(f)) >> 2;
    switch (f) {
    case TF_RGBA16F: b.v[w] = packHalf2x16(c.xy); b.v[w + 1u] = packHalf2x16(c.zw); break;
    case TF_RGBA32F: b.v[w] = floatBitsToUint(c.x); b.v[w + 1u] = floatBitsToUint(c.y); b.v[w + 2u] = floatBitsToUint(c.z); b.v[w + 3u] = floatBitsToUint(c.w); break;
    case TF_R11G11B10F: b.v[w] = f11_of(c.x) | f11_of(c.y) << 11 | f10_of(c.z) << 22; break;
    case TF_RGB10A2: { uvec4 q = uvec4(round(clamp(c, 0.0, 1.0) * vec4(1023.0, 1023.0, 1023.0, 3.0))); b.v[w] = q.x | q.y << 10 | q.z << 20 | q.w << 30; break; }
    case TF_RGBA8: b.v[w] = packUnorm4x8(c); break;
    case TF_BGRA8: b.v[w] = packUnorm4x8(c.bgra); break;
    case TF_RGBA8_SRGB: b.v[w] = packUnorm4x8(vec4(linear_to_srgb(clamp(c.r, 0.0, 1.0)), linear_to_srgb(clamp(c.g, 0.0, 1.0)), linear_to_srgb(clamp(c.b, 0.0, 1.0)), c.a)); break;
    case TF_BGRA8_SRGB: b.v[w] = packUnorm4x8(vec4(linear_to_srgb(clamp(c.b, 0.0, 1.0)), linear_to_srgb(clamp(c.g, 0.0, 1.0)), linear_to_srgb(clamp(c.r, 0.0, 1.0)), c.a)); break;
    case TF_RG16F: b.v[w] = packHalf2x16(c.xy); break;
    case TF_R32F: b.v[w] = floatBitsToUint(c.x); break;
    case TF_RG32F: b.v[w] = floatBitsToUint(c.x); b.v[w + 1u] = floatBitsToUint(c.y); break;
    case TF_RGBA16: b.v[w] = packUnorm2x16(c.xy); b.v[w + 1u] = packUnorm2x16(c.zw); break;
    case TF_RGB9E5: b.v[w] = rgb9e5_of(c.rgb); break;
    }
}
