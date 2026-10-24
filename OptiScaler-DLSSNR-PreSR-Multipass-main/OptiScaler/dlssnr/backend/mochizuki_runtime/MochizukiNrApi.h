#pragma once
#include "../lmxxf_runtime/LmxxfNrApi.h"
#include "MochizukiNrControls.h"

// MochizukiNrInfo (declared in MochizukiNrControls.h) is an exact-size package contract.
// Its additive recording_timing_* diagnostics report core certified-harvest rereads/rejections;
// they do not include RenoDX timing. Host/runtime headers must be updated together.

// Separate frame contract; the shared v2 table supplies recording ownership and
// submission operations. It never reinterprets lmxxf's frame or the older fork ABI.
#define MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL (1u << 8)
// This frame records on the inline side queue. Without INLINE_EXECUTION or
// DIRECT_RECORDING it uses command-list split. Create-time INLINE_QUEUE only permits it.
#define MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION (1u << 9)
// SR -> NR placement; RenoDX Auto detail stability depends on this frame's route.
#define MOCHIZUKI_NR_FRAME_FLAG_AFTER_UPSCALE (1u << 10)
// NrInlineComputeConvert (host default on): an inline colour frame in one of the listed formats (B10G11R11,
// RGBA8/BGRA8 UNORM or SRGB, A2B10G10R10 / A2R10G10B10 UNORM) converts its input (copy + runtime_unpack_f16)
// and its write-back (a compute pack per format) on the compute family instead of graphics blits, so the
// whole chain can stay on compute. May rebuild the network.
#define MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_CONVERT (1u << 11)
// NrInlineComputeMotion (host default on, inline frames only): with dynamic resolution, motion vectors of another extent are rescaled onto the
// subrect by a compute pass (runtime_mv_rescale_*, bilinear like the blit) instead of a LINEAR blit, so such inline
// frames can keep the whole chain on compute. With more than one layer it also sets RuntimeConfig::motion_in_place_passes:
// every layer samples the motion vectors in place, with no motion blit. Changing it remakes the frame buffers, and the
// network when there is more than one layer.
#define MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_MOTION (1u << 12)
// NrInlineComputeDownscale (host default on, inline frames only): model scale below 1 keeps the frame in its own format and filters it down
// with runtime_downscale (the blit's own taps), and packs the full-size answer on compute when it is not stored in
// the frame's format, so such inline frames can keep the whole chain on compute. Rebuilds the network when scaled.
#define MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_DOWNSCALE (1u << 13)
// NrInlineComputeExport (host default on, inline frames only): the first-pass export (multi-layer NR) packs pass 0 into the frame's format on
// compute (runtime_pack_*) instead of a blit, so such inline frames can keep the whole chain on compute. RGBA16F,
// R11G11B10, RGBA8/BGRA8 UNORM and RGB10A2; other formats keep the blit. Rebuilds the network when exporting.
#define MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_EXPORT (1u << 14)
// Diagnostic: GPU timestamp pairs around the core's linear-input encode and transfer pass
// (MochizukiNrInfo::gpu_ms_encode_last / gpu_ms_transfer_last). Absent: none are written.
#define MOCHIZUKI_NR_FRAME_FLAG_STAGE_TIMING (1u << 15)
// Adopted vkd3d only: record into the game's current native Vulkan command buffer.
// Mutually exclusive with INLINE_EXECUTION; Windows retains the bridge path.
#define MOCHIZUKI_NR_FRAME_FLAG_DIRECT_RECORDING (1u << 16)
typedef struct MochizukiNrFrameInfo
{
    uint32_t struct_size;
    uint64_t frame_id;
    void* color;
    uint32_t color_width, color_height, color_state, flags;
    float transfer_strength, color_strength, model_scale;
    uint32_t passes;
    void* motion;
    uint32_t motion_state, motion_width, motion_height;
    float motion_scale_x, motion_scale_y;
    uint32_t reset;
    void* depth;
    uint32_t depth_state, depth_width, depth_height;
    uint32_t depth_inverted;
} MochizukiNrFrameInfo;

typedef int32_t (*PFN_MochizukiNrPrepareFrame)(void*, const MochizukiNrFrameInfo*, LmxxfNrJob*);
typedef int32_t (*PFN_MochizukiNrGetApi)(uint32_t, LmxxfNrApi*);
// These direct exports are required by the current package, alongside the exact-size frame ABI.
// Get returns OK + nonzero for an adopted vkd3d device, OK + zero on Windows,
// and an error when the adopted device cannot expose its current command buffer.
typedef int32_t (*PFN_MochizukiNrGetDirectCommandBuffer)(void* context, void* native_list, uint64_t* command_buffer);
typedef int32_t (*PFN_MochizukiNrRecordDirect)(void* context, void* job, void* native_list, uint64_t command_buffer);
// Observational only. Host holds RecordingMutex, as for Begin/End/Collect;
// timingLog gates per-recipe supply/generation markers, not timing collection.
typedef int32_t (*PFN_MochizukiNrPollDirectCompletions)(void* context, uint32_t timingLog);

#ifdef __cplusplus
extern "C" {
#endif
MOCHIZUKI_NR_EXPORT int32_t MochizukiNrGetApi(uint32_t version, LmxxfNrApi* out);
MOCHIZUKI_NR_EXPORT int32_t MochizukiNrPrepareFrame(void*, const MochizukiNrFrameInfo*, LmxxfNrJob*);
MOCHIZUKI_NR_EXPORT int32_t MochizukiNrGetDirectCommandBuffer(void* context, void* native_list, uint64_t* command_buffer);
MOCHIZUKI_NR_EXPORT int32_t MochizukiNrRecordDirect(void* context, void* job, void* native_list, uint64_t command_buffer);
MOCHIZUKI_NR_EXPORT int32_t MochizukiNrPollDirectCompletions(void* context, uint32_t timingLog);
#ifdef __cplusplus
}
#endif
