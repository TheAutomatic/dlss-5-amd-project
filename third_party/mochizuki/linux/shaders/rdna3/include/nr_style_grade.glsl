// The DLL's Style grade: what cg2r_post_process_kernel does to the post block's answer when the Style is not
// Standard (DLL 310.8 and 310.9, artifacts/ptx/0112fd90.ptx, the path with the original image bound and no masks,
// base, intensity map or transfer). The host launches it only for a Style whose grade is not neutral, so Standard
// never comes here. Measured against the real DLL (RTX 5090, NGX): without it our Natural / Cinematic were
// 26 / 33 dB from NVIDIA's, with it 49 dB, the same as Standard.
//
// The grade per Style is a table in the DLL (.rdata, the same in both versions and for both models; the launch
// parameters captured on the 5090 agree): black, white, exposure, gamma, contrast, saturation, saturation power,
// two tints and five tone-band gains. Only the entries the table holds are non-zero here; every step is still
// done in the kernel's order, because the identity steps round like the kernel's do.
//
// nr_style_grade(style, base, rgb, intensity): base = the network's input (the DLL's colour), rgb = the post
// block's answer after its own Intensity compose. Returns sat(base + sat(I) * (grade(sat(rgb)) - base)).

struct NrGrade {
    float black, white, exposure, gamma, contrast, saturation, sat_power, tint1, tint2;
    float band0, band1, band2, band3, band4;
};

NrGrade nr_grade_of(uint style) {
    NrGrade g = NrGrade(0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    if (style == 1u) { g.exposure = -0.1; g.contrast = -0.25; g.saturation = -0.1; }   // Natural
    if (style == 2u) { g.saturation = -0.15; }                                         // Cinematic
    return g;
}

float nr_grade_sat(float x) { return clamp(x, 0.0, 1.0); }
float nr_grade_smooth(float t) { t = nr_grade_sat(t); return t * t * (3.0 - (t + t)); }

// A tint at the pixel's lightness (the kernel's +344 / +348): HSL q and p at saturation 1, one channel at a hue.
vec3 nr_grade_tint(vec3 c, float a, float hue_pos, float hue_neg, bool warm_first) {
    const float m = abs(a);
    if (m < 1.0e-6) return c;
    const float mx = max(max(c.r, c.g), c.b), mn = min(min(c.r, c.g), c.b);
    const float L = (mx + mn) * 0.5;
    const float hi = L < 0.5 ? L + L : (L + 1.0) - L;
    const float lo = (L + L) - hi;
    vec3 t;
    if (warm_first) {   // +344: a > 0 -> (hi, lo + (hi - lo) * 6 * h, lo); a <= 0 -> (lo, lo + (hi - lo) * 6 * h, hi)
        if (a > 0.0) t = vec3(hi, fma((hi - lo) * 6.0, hue_pos, lo), lo);
        else t = vec3(lo, fma((hi - lo) * hue_neg, 6.0, lo), hi);
    } else {            // +348: a > 0 -> (lo + (hi - lo) * 6 * h, hi, lo); a <= 0 -> (lo + (hi - lo) * 6 * h, lo, hi)
        if (a > 0.0) t = vec3(fma((hi - lo) * hue_pos, 6.0, lo), hi, lo);
        else t = vec3(fma((hi - lo) * 6.0, hue_neg, lo), lo, hi);
    }
    return clamp(fma(vec3(m), t - c, c), vec3(0.0), vec3(1.0));
}

// RGB -> HSL as the kernel does it (h in 0..1; s and h 0 for a grey).
vec3 nr_grade_hsl(vec3 c) {
    const float mx = max(max(c.r, c.g), c.b), mn = min(min(c.r, c.g), c.b);
    const float sum = mx + mn, L = sum * 0.5;
    float h = 0.0, s = 0.0;
    if (mx > mn) {
        const float d = mx - mn;
        s = L > 0.5 ? d / ((2.0 - mx) - mn) : d / sum;
        if (mx == c.r) h = ((c.g < c.b ? 6.0 : 0.0) + (c.g - c.b) / d) / 6.0;
        else if (mx == c.g) h = ((c.b - c.r) / d + 2.0) / 6.0;
        else h = ((c.r - c.g) / d + 4.0) / 6.0;
    }
    return vec3(h, s, L);
}

float nr_grade_hue(float t, float hi, float lo) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return fma(t, (hi - lo) * 6.0, lo);
    if (t < 0.5) return hi;
    if (t < 2.0 / 3.0) return fma((2.0 / 3.0 - t) * (hi - lo), 6.0, lo);
    return lo;
}

// HSL -> RGB at saturation s (already saturated by the caller); s <= 0 gives the grey at L.
vec3 nr_grade_rgb(float h, float s, float L) {
    if (s <= 0.0) return vec3(L);
    const float hi = L < 0.5 ? L * (s + 1.0) : (L + s) - L * s;
    const float lo = (L + L) - hi;
    return vec3(nr_grade_hue(h + 1.0 / 3.0, hi, lo), nr_grade_hue(h, hi, lo), nr_grade_hue(h - 1.0 / 3.0, hi, lo));
}

float nr_grade_contrast(float x, float k) {
    x = nr_grade_sat(x);
    return fma(k, x * x * (3.0 - (x + x)) - x, x);
}

// The five tone bands: smoothstep windows centred on 0, .25, .5, .75 and 1; their weighted gain is an exponent.
float nr_grade_bands(float y, NrGrade g) {
    const float x = nr_grade_sat(y);
    const float w0 = nr_grade_smooth((x - 0.25) / -0.25);
    const float w1 = nr_grade_smooth(x / 0.25) * nr_grade_smooth((x - 0.5) / -0.25);
    const float w2 = nr_grade_smooth((x - 0.25) / 0.25) * nr_grade_smooth((x - 0.75) / -0.25);
    const float w3 = nr_grade_smooth((x - 0.5) / 0.25) * nr_grade_smooth((x - 1.0) / -0.25);
    const float w4 = nr_grade_smooth((x - 0.75) / 0.25);
    const float tot = fma(w4, g.band4, fma(w3, g.band3, fma(w2, g.band2, fma(w0, g.band0, w1 * g.band1))));
    return exp2(log2(x) * exp2(-tot));
}

vec3 nr_style_grade(uint style, vec3 base, vec3 rgb, float intensity) {
    const NrGrade g = nr_grade_of(style);
    vec3 c = clamp(rgb, vec3(0.0), vec3(1.0));
    c = clamp((c - g.black) / ((g.white - g.black) + 1.0e-10), vec3(0.0), vec3(1.0));
    c = nr_grade_tint(c, g.tint1, uintBitsToFloat(0x3D7A4E7Bu), uintBitsToFloat(0x3DD82E18u), true);
    c = nr_grade_tint(c, g.tint2, uintBitsToFloat(0x3CB60DC0u), uintBitsToFloat(0x3E13E8F8u), false);
    c *= exp2(g.exposure);
    c = vec3(nr_grade_contrast(c.r, g.contrast), nr_grade_contrast(c.g, g.contrast), nr_grade_contrast(c.b, g.contrast));
    c = vec3(nr_grade_bands(c.r, g), nr_grade_bands(c.g, g), nr_grade_bands(c.b, g));
    const float gp = exp2(-g.gamma);
    c = exp2(gp * log2(max(c, vec3(0.0))));
    vec3 hsl = nr_grade_hsl(c);
    c = nr_grade_rgb(hsl.x, nr_grade_sat(hsl.y * (g.saturation + 1.0)), hsl.z);
    hsl = nr_grade_hsl(c);
    c = nr_grade_rgb(hsl.x, nr_grade_sat(exp2(exp2(-g.sat_power) * log2(hsl.y))), hsl.z);
    c = clamp(c, vec3(0.0), vec3(1.0));
    return clamp(fma(vec3(nr_grade_sat(intensity)), c - base, base), vec3(0.0), vec3(1.0));
}
