// SPDX-License-Identifier: MIT
// Log encoding of the post/history path: v = 0.36 * log10(1 + 100 y) (no 1/2.45 here).

vec3 post_encode(vec3 y) {
    vec3 r;
    for (int i = 0; i < 3; ++i) {
        precise float t = max(fma(y[i], 100.0, 1.0), fbits(0x38d1b717u));
        precise float v = (log2(t) * fbits(0x3e9a209bu)) * fbits(0x3eb851ecu);
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
        precise float y = (exp2(t) - 1.0) * fbits(0x3c23d70au);
        r[i] = y;
    }
    return r;
}
