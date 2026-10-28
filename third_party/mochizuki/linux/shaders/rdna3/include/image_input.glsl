// Specialized native input lift for the FP32 pre block. The host supplies
// complete, unshifted 8x8 groups and default colour gain/layout/UV controls.
// Style, tone, masks, seed, noise and the constant feature remain live.
// The feature preparation below is per *pixel* of the 8x8 group, not per
// thread, so it runs at any thread count that divides 64 - the loop is the only
// thing the wave count touches. One wave is what makes `NR_K_REGS` legal, which
// is the 2048 B that takes this kernel's window from 6144 B to 4096 and its
// one-wave occupancy from five subgroups a SIMD to eight. Four waves would put
// NR_MF at 1 and is refused for the same reason the rest of the family refuses
// it.
//
// **Measured, byte-identical at both extents, and it is small.**
//
//   config                    LDS  VGPR  wv  ins/win  cyc/win   us 1080p  us 4K
//   ship            fw2      6144   144  10     6650     8498      701.4   2886
//   fw1 + NR_K_REGS          4096   192   8     6425     8266      691.3   2837
//   fw1 alone                6144   256   5     6359     8172       -       -
//
// -1.4% at 1080p and -1.7% at 4K on the dispatch, twelve samples each with no
// overlap, against a modelled -2.7%; in the frame that is -0.15% of energy at
// both extents, which is at the resolution limit of the harness. It also
// confirms the measured knee on a third kernel: ten subgroups to eight costs nothing,
// and five would have cost everything. Default stays at two waves.
#if NR_FWAVES > 2 || NR_C != 32 || NR_ACC_F16 != 0 || !defined(NR_INPUT_F16) || !defined(NR_POOL_F16)
#error Fused image input requires the FP32 one- or two-wave C32 pre pooling kernel
#endif
#ifndef NR_TEMPORAL_HPASS
#define NR_TEMPORAL_HPASS 0
#endif
#ifdef NR_EXTERNAL_CONTROL_MASK
#define NR_MASK_BINDING 6
#define NR_MASK_SAMPLER_BINDING 8
#include "control_mask.glsl"
layout(set=0,binding=7) uniform sampler2D nr_tex;
#elif defined(NR_TEMPORAL)
// The temporal variant follows the external-mask variant's shape exactly: the
// extra scalars travel in a buffer rather than in push constants, so
// PushPreImage keeps its size and the host's existing offsets into `d.push`
// stay valid. Production binaries are built without this define and are
// byte-identical to the ones this project has already validated.
//
//   [0] = (history and motion are usable, mv scale x, mv scale y, unused)
//   [1] = (history width, history height, depth present, depth inverted)
//   [2] = (uv -> motion texture: the region's share of the allocation x, y; its base x, y)
//
// Element 0's first component is the whole of the original's gate, resolved on the host:
// the first-frame latch, DLSSNR.Reset and whether a motion source exists at all.
// The shader does not re-derive it; there is one place that decision is made.
layout(set=0,binding=6,std430) readonly buffer NrTemporal { vec4 nr_temporal[3]; };
layout(set=0,binding=7) uniform sampler2D nr_tex;
layout(set=0,binding=8) uniform sampler2D nr_motion;
layout(set=0,binding=9) uniform sampler2D nr_history;
layout(set=0,binding=10) uniform sampler2D nr_depth;
#if NR_TEMPORAL_HPASS
// the post block's history, reconstructed here once. The post samples the
// history at the pixel's own vector; this block samples it at the depth-chosen
// tap's, which is the pixel's own everywhere but at silhouettes - there it
// reconstructs the post's a second time. The post loads the f32 value back.
layout(set=0,binding=11,rgba32f) uniform writeonly image2D nr_hpass;
#endif
#include "temporal_history.glsl"
#else
layout(set=0,binding=6) uniform sampler2D nr_tex;
#endif
shared NR_F16 input_features[4*256];

#include "image_noise.glsl"
void nr_prepare_features() {
    // One pass a pixel of the 8x8 group, whatever the wave count. The trip
    // count is a *compile-time* 64/NR_THREADS and not a `< 64u` test, so at two
    // waves this unrolls back to the single straight-line pass it has always
    // been - written as a dynamic loop it rolled, and the two-wave build went
    // 6650 -> 7008 instructions a window for nothing.
    for(uint nr_i=0u;nr_i<64u/uint(NR_THREADS);++nr_i) {
    const uint nr_pf=gl_LocalInvocationIndex+nr_i*uint(NR_THREADS);
    const uint x=gl_WorkGroupID.x*8u+nr_pf%8u;
    const uint y=gl_WorkGroupID.y*8u+nr_pf/8u;
    const uint sw=pc.image_source_W!=0u?pc.image_source_W:pc.tiles_x*4u;
    const uint sh=pc.image_source_H!=0u?pc.image_source_H:pc.tiles_y*4u;
    const int sx=x<sw?int(x):2*int(sw)-int(x)-2;
    const int sy=y<sh?int(y):2*int(sh)-int(y)-2;
    const vec2 uv=vec2((float(sx)+0.5)/float(sw),(float(sy)+0.5)/float(sh));
    const vec4 rgba=textureLod(nr_tex,uv,0.0);
    const f16vec3 centered=f16vec3(f16vec3(rgba.rgb)-f16vec3(0.5));
    const vec3 cn=vec3(f16vec3(centered*NR_F16(0.125)));
    float f[16];
#if NR_NOISE_FIELD
    // The noise features are a fixed function of (x, y, seed) and the noise
    // gain, so the host computes them once at build (noise_field.comp, the same
    // code below) into the weight arena, [row][pixel] f16 x4 over the padded
    // working grid, and every frame reads them back: two dwords a pixel instead
    // of a 32-bit hash with four quarter-rate multiplies and four
    // transcendentals (1080p 19 us, 4K 87 us a frame). Offset 0: compute here.
    [[dont_flatten]] if (pc.image_noise_off != 0u) {
        const uint nr_ni = pc.image_noise_off + (y * (gl_NumWorkGroups.x * 8u) + x) * 2u;
        const vec2 nr_n01 = unpackHalf2x16(wgt_u32[nr_ni]);
        const vec2 nr_n2 = unpackHalf2x16(wgt_u32[nr_ni + 1u]);
        f[0]=nr_n01.x;f[1]=nr_n01.y;f[2]=nr_n2.x;
    } else
#endif
    {
#if NR_DIAG_NONOISE
    nr_g0=nr_g1=nr_g2=0.0;  // diagnostic only: prices the noise stream
#else
    nr_gauss3(x,y,pc.image_seed);
#endif
    f[0]=pc.image_noise*nr_g0;f[1]=pc.image_noise*nr_g1;f[2]=pc.image_noise*nr_g2;
    }
    f[3]=pc.image_constant;
    f[4]=cn.x;f[5]=cn.y;f[6]=cn.z;
    // Slots 7..9 are the history colour. The original seeds them with the
    // current colour and only overwrites them inside the branch it takes when
    // BOTH a history and a motion handle are present - so this default is not a
    // stand-in for the real thing, it is what the original does with no motion.
    f[7]=cn.x;f[8]=cn.y;f[9]=cn.z;
#ifdef NR_TEMPORAL
    if (nr_temporal[0].x != 0.0) {
        // Depth picks *where* the motion vector is read, and never becomes a
        // feature of its own: the original samples depth at the centre and four
        // diagonals and reads motion at whichever of them is nearest the camera
        // (the pre PTX at :339-378). On a
        // silhouette that is what stops the background's vector being used for
        // a foreground pixel.
        vec2 muv = uv;
#if NR_TEMPORAL_HPASS
        bool moved = false;
#endif
        if (nr_temporal[1].z != 0.0) {
            const vec2 step = 1.0/vec2(sw,sh);
            const bool inverted = nr_temporal[1].w != 0.0;
            float best = textureLod(nr_depth,uv,0.0).x;
            for (int dy=-1; dy<=1; dy+=2) for (int dx=-1; dx<=1; dx+=2) {
                const vec2 at = uv+vec2(dx,dy)*step;
                const float d = textureLod(nr_depth,at,0.0).x;
#if NR_TEMPORAL_HPASS
                if (inverted ? d>best : d<best) { best=d; muv=at; moved=true; }
#else
                if (inverted ? d>best : d<best) { best=d; muv=at; }
#endif
            }
        }
        // The game's vectors in place at full resolution (the region's share of
        // the allocation and its base in [2]), or the estimator's field ([2] = 1, 1, 0, 0).
        const vec2 mv = textureLod(nr_motion,muv*nr_temporal[2].xy+nr_temporal[2].zw,0.0).xy*nr_temporal[0].yz;
        const vec2 extent = nr_temporal[1].xy;
        const vec3 hist = nr_history_5tap(nr_history,(uv+mv)*extent,vec2(0.5),
                                          extent-0.5,1.0/extent);
#if NR_TEMPORAL_HPASS
        vec3 hpost = hist;
        if (moved) {
            const vec2 mv0 = textureLod(nr_motion,uv*nr_temporal[2].xy+nr_temporal[2].zw,0.0).xy*nr_temporal[0].yz;
            hpost = nr_history_5tap(nr_history,(uv+mv0)*extent,vec2(0.5),extent-0.5,1.0/extent);
        }
        if (x < sw && y < sh) imageStore(nr_hpass, ivec2(x, y), vec4(hpost, 0.0));
#endif
        const f16vec3 hcentered = f16vec3(f16vec3(hist)-f16vec3(0.5));
        const vec3 hn = vec3(f16vec3(hcentered*NR_F16(0.125)));
        f[7]=hn.x;f[8]=hn.y;f[9]=hn.z;
    }
#endif
    f[10]=pc.image_style;f[11]=pc.image_tone;f[12]=pc.image_structure;
    f[13]=pc.image_skin;f[14]=pc.image_other;f[15]=0.0;
#ifdef NR_EXTERNAL_CONTROL_MASK
    vec4 mask=nr_control_mask(uv);
    f[11]=pc.image_tone*mask.g;f[12]=pc.image_structure*mask.b;
    f[13]=-1.0;f[14]=-1.0;
#endif
    // One feature calculation per pixel. Only the 16 input features need LDS;
    // the 32 projected features pass directly into the Swin B fragments.
    const uint token=((y%8u)/4u*2u+(x%8u)/4u)*16u+(y%4u)*4u+x%4u;
    for(uint j=0u;j<16u;++j)input_features[token*16u+j]=NR_F16(f[j]);
    }
    barrier();
}
