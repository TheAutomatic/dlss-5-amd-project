// SPDX-License-Identifier: MIT
// Log encoding of the post/history path: v = 0.36 * log10(1 + 100 y) (no 1/2.45 here).

#ifdef LT_POST_LOG
// log2 of a positive fp32 by its exponent and a cubic on the mantissa (at most about 1.4e-3 off)
float lt_log2(float x) {
    uint b = floatBitsToUint(x);
    float e = float(int(b >> 23) - 127), m = uintBitsToFloat((b & 0x7fffffu) | 0x3f800000u) - 1.0;
    return e + m * (1.42086 + m * (-0.57725 + m * 0.15639));
}
// 2^x, floor plus a cubic (at most about 1e-4 relative)
float lt_exp2f(float x) {
    float i = floor(x), f = x - i;
    float p = 1.0 + f * (0.69589 + f * (0.22464 + f * 0.07947));
    return uintBitsToFloat(floatBitsToUint(p) + (uint(int(i)) << 23));
}
#define LOG2_(x) lt_log2(x)
#define EXP2_(x) lt_exp2f(x)
#else
#define LOG2_(x) log2(x)
#define EXP2_(x) exp2(x)
#endif

vec3 post_encode(vec3 y) {
    vec3 r;
    for (int i = 0; i < 3; ++i) {
        precise float t = max(fma(y[i], 100.0, 1.0), fbits(0x38d1b717u));
        precise float v = (LOG2_(t) * fbits(0x3e9a209bu)) * fbits(0x3eb851ecu);
        r[i] = v;
    }
    return r;
}

// unorm8 code -> fp16 of code / 255 with one rounding. fp16(fp32(c * fp32(1/255))) gives exactly that for every
// code (checked on the CPU for all 256 against the exact rational), so no fp64 is needed.
float16_t unorm8_half(uint c) {
    precise float v = float(c) * (1.0 / 255.0);
    return float16_t(v);
}

vec3 post_decode(vec3 v) {
    vec3 r;
    for (int i = 0; i < 3; ++i) {
        float x = v[i] > fbits(0x38d1b717u) ? min(v[i], 3.0) : fbits(0x38d1b717u);
        precise float t = (x * fbits(0x4031c71cu)) * fbits(0x40549a78u);
        precise float y = (EXP2_(t) - 1.0) * fbits(0x3c23d70au);
        r[i] = y;
    }
    return r;
}
