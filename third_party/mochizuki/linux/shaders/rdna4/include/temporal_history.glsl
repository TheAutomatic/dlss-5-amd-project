// Five-tap Catmull-Rom history reconstruction, transcribed instruction by
// instruction from the original pre block's PTX
// (`cc_tinlayout_fused_pre_block_swin_1h_32_1_ds_fp8.ptx`, the weight block at
// :402-440 and the five `tex.2d` at :484-488).
//
// Why five and not sixteen: the centre 2x2 of the 4x4 Catmull-Rom footprint is
// collapsed into one bilinear fetch at the weighted offset `w2/wc`, leaving a
// cross of five taps. **That merge is only valid under linear filtering.** The
// history texture must carry its own LINEAR sampler; `nrvk.hpp` creates NEAREST,
// MIRRORED_REPEAT images because that is what the shipping kernel's addressing
// is, and that is not a file to change for this.
//
// `pos`, `lo` and `hi` are in texel units with the centre convention: texel i
// has its centre at i + 0.5. The original clamps each tap to [0.5, subrect max]
// independently per axis, which is a clamp and not the mirror its colour path
// uses - the two addressing rules are genuinely different and both were read.

vec3 nr_history_5tap(sampler2D tex, vec2 pos, vec2 lo, vec2 hi, vec2 inv_size) {
    const vec2 c = floor(pos - 0.5) + 0.5;
    const vec2 t = clamp(pos - c, vec2(0.0), vec2(1.0));
    const vec2 t2 = t * t;
    const vec2 t3 = t2 * t;
    // The original forms w0 as (t + t^3) * -0.5 + t^2; kept in that order.
    const vec2 w0 = (t + t3) * -0.5 + t2;
    const vec2 w1 = 1.5 * t3 - 2.5 * t2 + 1.0;
    const vec2 w3 = 0.5 * (t3 - t2);
    const vec2 w2 = 1.0 - w0 - w1 - w3;
    const vec2 wc = w1 + w2;               // the merged centre weight per axis

    const vec2 p0 = clamp(c - 1.0, lo, hi);
    const vec2 pm = clamp(c + w2 / wc, lo, hi);
    const vec2 p2 = clamp(c + 2.0, lo, hi);

    const vec3 left   = texture(tex, vec2(p0.x, pm.y) * inv_size).rgb;
    const vec3 up     = texture(tex, vec2(pm.x, p0.y) * inv_size).rgb;
    const vec3 centre = texture(tex, vec2(pm.x, pm.y) * inv_size).rgb;
    const vec3 down   = texture(tex, vec2(pm.x, p2.y) * inv_size).rgb;
    const vec3 right  = texture(tex, vec2(p2.x, pm.y) * inv_size).rgb;

    const float k_left   = w0.x * wc.y;
    const float k_up     = w0.y * wc.x;
    const float k_centre = wc.x * wc.y;
    const float k_down   = w3.y * wc.x;
    const float k_right  = w3.x * wc.y;

    const vec3 sum = k_left * left + k_up * up + k_centre * centre
                   + k_down * down + k_right * right;
    // The original divides by the weight sum rather than assuming it is one;
    // after the per-axis clamps above it generally is not.
    return sum / (k_left + k_up + k_centre + k_down + k_right);
}
