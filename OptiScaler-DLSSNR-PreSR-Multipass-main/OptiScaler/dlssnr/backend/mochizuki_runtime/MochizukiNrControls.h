#pragma once

/* MochizukiNrRuntime.dll's own C exports, beside LmxxfNrGetApi (LmxxfNrApi.h, which this header leaves untouched, so
 * the lmxxf runtime and hosts that know only that ABI are unaffected). A host resolves each export with
 * GetProcAddress; a missing one means an older runtime.
 *
 * Versioning: every struct starts with struct_size, which the caller sets to the size of the struct it knows. A
 * reader uses a field only when struct_size >= offsetof(field) + sizeof(field), so a shorter struct (an older caller)
 * leaves the later fields at their defaults and a longer one (a newer caller) is accepted. Fields are only ever
 * appended, and no struct has implicit padding before its last field (explicit reserved fields fill any gap). A
 * function that fills a struct writes the whole fields that fit into the caller's struct_size, leaves anything past
 * them untouched (zero-initialise it) and sets struct_size to the end of the last field it wrote, never to its own
 * sizeof: tail padding (MochizukiNrInfo has 4 bytes after network_dispatches) is where the next field goes, so a
 * newer host must not take it for a field an older runtime wrote.
 *
 * LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK, for this runtime: when the network fails on a frame, the frame's
 * original colour is passed through to Super Resolution; the output is never zeros. */

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* MochizukiNrGetFeatures bits. ANY_QUEUE: the runtime follows whichever queue executes the list, so a queue change
 * needs no new session. */
#define MOCHIZUKI_NR_FEATURE_ANY_QUEUE 1u

    /* One later pass's model controls (MochizukiNrControls::pass[0] is pass 2, pass[1] is pass 3). */
    typedef struct MochizukiNrPassControls
    {
        uint32_t used;           /* 0: the pass inherits pass 1's controls with local_tone 0 (the default) */
        uint32_t style;          /* as MochizukiNrControls::style */
        float intensity;         /* 0..2, default 1 */
        float local_tone;        /* 0..2, default 0: tone re-applied on every pass compounds */
        float local_structure;   /* 0..2, default 1 */
        float skin_structure;    /* -1..2, default -1 */
        uint32_t automatic_mask; /* default 1 */
        uint32_t reserved;
    } MochizukiNrPassControls;

    /* The model's controls. Every value is sanitised by the runtime: NaN or infinity becomes the default, anything
     * else is clamped to the range given. Detail strength, colour strength, model scale and passes come with each
     * frame in LmxxfNrFrameInfo. */
    typedef struct MochizukiNrControls
    {
        uint32_t struct_size;
        uint32_t flags;          /* none defined yet: 0 */
        float intensity;         /* 0..2, default 1; above 1 raises the luminance ratio */
        uint32_t style;          /* 0 standard (default), 1 natural, 2 cinematic */
        float local_tone;        /* 0..2, default 1 */
        float local_structure;   /* 0..2, default 1 */
        float skin_structure;    /* -1..2, default -1 (follows local_structure); needs automatic_mask */
        uint32_t automatic_mask; /* default 1 */
        float max_ratio;         /* highlight guard, 1..8, default 2 */
        float history_strength;  /* 0..1, default 1; with motion vectors only */
        float white_point;       /* 0.01..100, default 1; linear input only */
        uint32_t apply_model;    /* default 1; 0 runs the network at full cost and shows the original */
        uint32_t linear_input;   /* 0 auto (float formats are linear, default), 1 on, 2 off; rebuilds the network */
        uint32_t max_passes;     /* 0 (default): the frame's passes; else 1..3, at least the frame's; rebuilds */
        /* Dynamic resolution. 0 exact (the default here and in a zeroed struct): the network is built for the
         * frame's render subrect, so every change of it rebuilds. 1 auto (the host's default, through
         * MochizukiDynamicResolution): as exact until a subrect smaller than the colour texture comes, then a
         * bucket (per axis the largest subrect seen rounded up to 64, within the texture) that only grows, except
         * after 30 s of frames of one colour texture whose subrects all stay at least 128 px inside it on both
         * axes. 2 always: the bucket for every frame, within the largest texture seen. Above 2 runs as 0, and so do
         * 1 and 2 while the motion vectors' format cannot be blitted. */
        uint32_t drs_mode;
        uint32_t reserved[3];
        MochizukiNrPassControls pass[2]; /* passes 2 and 3 */
        /* Preprocess (mochizuki0323's 729a05d): the picture the network is shown is changed before it runs and every
         * change is taken back out of its answer, so it changes how the network edits the frame. 0 (the default)
         * records nothing; the first frame with it on rebuilds the network once, able to run it. */
        uint32_t preprocess;
        uint32_t preprocess_exposure; /* 0 off, 1 auto (default: histogram auto exposure), 2 fixed: bias alone */
        float preprocess_bias_ev;     /* -8..8, default 0 */
        uint32_t preprocess_curve;    /* 0 none, 1 neutral, 2 reinhard, 3 filmic (default), 4 gt, 5 aces, 6 agx */
        float preprocess_contrast;    /* about mid grey, 0.5..2, default 1 */
        float preprocess_saturation;  /* 0.05..2, default 1 */
    } MochizukiNrControls;

    /* What the session is doing, for a menu. Safe to ask from any thread, at any rate. */
    typedef struct MochizukiNrInfo
    {
        uint32_t struct_size;
        uint32_t building;            /* 1 while a network is being built */
        uint32_t frame_w, frame_h;    /* the network's frame extent; 0 before the first network */
        uint32_t model_w, model_h;    /* the extent the network runs at (model scale applied) */
        uint32_t max_passes;          /* the passes the current network was built for */
        uint32_t motion_refused_dxgi; /* DXGI format of motion vectors that were refused (running without history) */
        float gpu_ms_median;          /* network GPU time over the last 120 frames */
        float gpu_ms_p95;
        float build_seconds;           /* the last network build */
        uint32_t reserved0;            /* 0; keeps frames 8-aligned without implicit padding */
        uint64_t frames;               /* frames run in this session */
        uint32_t history_consumed_pct; /* of the last 64 frames, those that used the history */
        uint32_t failed;               /* 1: the session failed; last_error says why */
        char last_error[256];
        uint32_t network_dispatches; /* the network's dispatches in the last frame (all passes) */
        float preprocess_ev;         /* the preprocess's auto exposure in EV, bias included; NaN when not metering */
        float white_point; /* the white point the last frame used, the game's exposure applied (linear colour) */
        float gpu_ms_last, gpu_ms_mean, gpu_ms_max;
        uint64_t gpu_samples, gpu_tick;
        /* The fields end at byte 332 (struct_size as the runtime fills it); sizeof is 336 (tail padding). */
    } MochizukiNrInfo;

    /* All but GetFeatures return an LmxxfNrStatus. SetControls takes effect on the next PrepareFrame; call it on the
     * thread that calls PrepareFrame. It never fails the session. */
    typedef int32_t (*PFN_MochizukiNrSetControls)(void* context, const MochizukiNrControls* controls);
    typedef int32_t (*PFN_MochizukiNrGetInfo)(void* context, MochizukiNrInfo* info);
    typedef int32_t (*PFN_MochizukiNrGetControlDefaults)(MochizukiNrControls* controls);
    typedef uint32_t (*PFN_MochizukiNrGetFeatures)(void);
    /* The game's exposure for the frames that follow: what the frame's linear colour is multiplied by to be shown
     * (the upscaler's exposure texture, times its scale, over the pre-exposure). The white point becomes
     * MochizukiNrControls::white_point divided by it, so the network sees the frame at the brightness the game shows
     * it. 0, or a value that is not finite and positive, goes back to white_point alone. Call it before PrepareFrame
     * on the thread that calls it; it never fails the session. */
    typedef int32_t (*PFN_MochizukiNrSetExposure)(void* context, float exposure);

#ifdef _WIN32
#ifdef MOCHIZUKI_NR_RUNTIME_EXPORTS
#define MOCHIZUKI_NR_EXPORT __declspec(dllexport)
#else
#define MOCHIZUKI_NR_EXPORT __declspec(dllimport)
#endif
#else
#define MOCHIZUKI_NR_EXPORT
#endif

    MOCHIZUKI_NR_EXPORT int32_t MochizukiNrSetControls(void* context, const MochizukiNrControls* controls);
    MOCHIZUKI_NR_EXPORT int32_t MochizukiNrGetInfo(void* context, MochizukiNrInfo* info);
    MOCHIZUKI_NR_EXPORT int32_t MochizukiNrGetControlDefaults(MochizukiNrControls* controls);
    MOCHIZUKI_NR_EXPORT uint32_t MochizukiNrGetFeatures(void);

#ifdef __cplusplus
}
#endif
