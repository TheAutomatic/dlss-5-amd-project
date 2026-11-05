// SPDX-License-Identifier: MIT
// OpenNR RR runtime (OpenNrRrRuntime.dll): C interface for a host that intercepts a game's Ray Reconstruction
// (NGX DLSS-D) evaluate on D3D12. The network runs on a Vulkan device of its own on the game's adapter; the game's
// textures reach it through shared D3D12 buffers and a shared fence.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ONR_RR_ABI_VERSION 1
#define ONR_RR_ABI_VERSION_2 2
#define ONR_RR_ABI_VERSION_3 3
#define ONR_RR_ABI_VERSION_4 4
#define ONR_RR_ABI_VERSION_5 5

#define ONR_RR_RUNTIME_VERSION "0.1.0"
#define ONR_RR_RUNTIME_VERSION_RC 0, 1, 0, 0

enum OnrRrResult {
    ONR_RR_OK = 0,
    ONR_RR_INVALID_ARGUMENT = -1,
    ONR_RR_UNSUPPORTED = -2,   // GPU or nvngx_dlssd.dll not supported; GetLastError says why
    ONR_RR_FAILED = -3,
    ONR_RR_DEVICE_LOST = -4,
};

enum OnrRrMode {
    ONR_RR_MODE_FAST = 0,      // fp32 accumulation (kernels *_f), the default
    ONR_RR_MODE_FAITHFUL = 1,  // accepted from older hosts and run as fast (the k8 kernels are harness-only)
};

enum OnrRrState {
    ONR_RR_STATE_BUILDING = 0, // weights / kernels being built: frames get the scaled colour
    ONR_RR_STATE_READY = 1,
    ONR_RR_STATE_FAILED = 2,   // frames get the scaled colour; message says why
};

// What runs the kernels (ABI 4).
enum OnrRrBackend {
    ONR_RR_BACKEND_VULKAN = 0,     // a Vulkan device of the runtime's own, shared buffers and fences (RecordInputs...)
    ONR_RR_BACKEND_D3D12 = 1,      // the game's D3D12 device, recorded into the game's list (Record / RecordPost)
    ONR_RR_BACKEND_VULKAN_HOST = 2, // the game's Vulkan device, recorded into the game's command buffer (RecordVk, ABI 5)
};

// OnrRrCreateInfo.backend: which one the host wants.
enum OnrRrBackendChoice {
    ONR_RR_BACKEND_CHOICE_AUTO = 0,   // D3D12 when the GPU check at Create passes, otherwise Vulkan
    ONR_RR_BACKEND_CHOICE_D3D12 = 1,  // the same, logged as asked for
    ONR_RR_BACKEND_CHOICE_VULKAN = 2,
};

enum OnrRrFrameFlags {
    ONR_RR_FRAME_RESET = 1u << 0,          // drop the history (camera cut)
    ONR_RR_FRAME_HW_DEPTH = 1u << 1,       // depth is the hardware (NDC) depth, not linear view depth
    ONR_RR_FRAME_DEPTH_INVERTED = 1u << 2, // with HW_DEPTH: reversed Z (near = 1)
    ONR_RR_FRAME_MV_JITTERED = 1u << 3,    // motion vectors include the jitter difference
    ONR_RR_FRAME_ROUGHNESS_PACKED = 1u << 4, // roughness is normals.w (roughness texture ignored)
    ONR_RR_FRAME_NORMALS_UNPACK = 1u << 5, // normals are stored as n * 0.5 + 0.5 (the runtime also assumes it for
                                           // UNORM formats)
    ONR_RR_FRAME_DM_INVERT = 1u << 6,      // diagnostic: the disocclusion mask goes in as 1 - mask (NGX takes it as is)
    ONR_RR_FRAME_DM_CLAMP = 1u << 7,       // the disocclusion mask (after the scale and DM_INVERT) is clamped to 0..1
                                           // (NGX takes it as is)
};

typedef struct OnrRrTexture {
    void* resource;   // ID3D12Resource*
    uint32_t state;
    uint32_t x, y;
} OnrRrTexture;

typedef struct OnrRrCreateInfo {
    uint32_t size;                 // sizeof(OnrRrCreateInfo)
    uint32_t mode;                 // OnrRrMode
    void* d3d12_device;            // ID3D12Device* of the game
    uint32_t max_render_width, max_render_height;
    uint32_t output_width, output_height;
    const wchar_t* dlssd_path;     // nvngx_dlssd.dll the weights are built from (read only, never loaded)
    const wchar_t* cache_dir;      // built weights and pipeline cache; null: %LOCALAPPDATA%\OpenNR\rr
    const wchar_t* data_dir;       // folder holding shaders\; null: <runtime DLL folder>\opennr-rr
    const wchar_t* log_path;       // null: <runtime DLL folder>\opennr_rr.log
    uint32_t backend;              // OnrRrBackendChoice
    const struct OnrRrVkDevice* vk_device;
} OnrRrCreateInfo;

#define ONR_RR_CREATE_INFO_SIZE_V1 ((uint32_t)offsetof(OnrRrCreateInfo, backend))
#define ONR_RR_CREATE_INFO_SIZE_V2 ((uint32_t)offsetof(OnrRrCreateInfo, vk_device))

typedef struct OnrRrVkDevice {
    uint32_t size;                 // sizeof(OnrRrVkDevice)
    uint32_t api_version;          // the instance's apiVersion
    void* instance;                // VkInstance
    void* physical_device;         // VkPhysicalDevice
    void* device;                  // VkDevice
    uint32_t queue_family, queue_index;
} OnrRrVkDevice;

typedef struct OnrRrVkImage {
    uint64_t image;                // VkImage
    uint64_t view;                 // VkImageView of the format below, one aspect (may be 0)
    uint32_t format;               // VkFormat of the view (a depth format: its depth aspect is read)
    uint32_t width, height;        // of that mip level
    uint32_t mip, layer;
    uint32_t usage;                // VkImageUsageFlags the host knows of (0: unknown, the view is not used)
} OnrRrVkImage;

typedef struct OnrRrFrame {
    uint32_t size;                 // sizeof(OnrRrFrame)
    uint32_t flags;                // OnrRrFrameFlags
    uint32_t render_width, render_height;   // this frame's input extent (<= the create maximum)
    float jitter_x, jitter_y;      // render pixels, as NGX takes them
    float mv_scale_x, mv_scale_y;  // multiplies the motion vectors into render pixels
    float world_to_view[16];       // the NGX matrices as the game passes them (row-major, row vectors);
    float view_to_clip[16];        // all zero when the game gives none
    OnrRrTexture color;            // noisy radiance, render extent
    OnrRrTexture diffuse_albedo;
    OnrRrTexture specular_albedo;
    OnrRrTexture normals;          // world space; .w = roughness with ONR_RR_FRAME_ROUGHNESS_PACKED
    OnrRrTexture roughness;
    OnrRrTexture depth;
    OnrRrTexture motion;           // current -> previous
    OnrRrTexture specular_hit_distance;
    OnrRrTexture specular_ray_direction_hit_distance;   // not read (rr2 has no input for it)
    OnrRrTexture output;           // output extent, written by RecordOutput
    OnrRrTexture transparency_layer;
    OnrRrTexture transparency_opacity;
    OnrRrTexture disocclusion_mask;
    OnrRrTexture sss_guide;
    float disocclusion_mask_scale;
    OnrRrTexture responsivity_mask;
    OnrRrTexture color_before_transparency;
    OnrRrTexture depth_of_field_guide;
    OnrRrTexture specular_motion;
    OnrRrTexture transparency_motion;
} OnrRrFrame;

#define ONR_RR_FRAME_SIZE_V1 ((uint32_t)offsetof(OnrRrFrame, transparency_layer))
#define ONR_RR_FRAME_SIZE_V2 ((uint32_t)offsetof(OnrRrFrame, disocclusion_mask))
#define ONR_RR_FRAME_SIZE_V3 ((uint32_t)offsetof(OnrRrFrame, disocclusion_mask_scale))
#define ONR_RR_FRAME_SIZE_V4 ((uint32_t)offsetof(OnrRrFrame, responsivity_mask))

typedef struct OnrRrInfo {
    uint32_t size;                 // sizeof(OnrRrInfo)
    uint32_t state;                // OnrRrState
    uint32_t mode;                 // OnrRrMode in use
    uint32_t frames;               // frames the network ran
    float gpu_ms_last;             // network + post GPU time of the last measured frame (Vulkan timestamps)
    float gpu_ms_average;          // over the last 64 measured frames
    uint32_t render_width, render_height, output_width, output_height;
    char weights_source[260];      // UTF-8 path of the nvngx_dlssd.dll the weights came from
    char message[256];             // UTF-8: what it is doing or why it failed
    uint32_t backend;              // OnrRrBackend running now
    char backend_message[124];     // UTF-8: why D3D12 is not used (empty when it is, or when it was not asked for)
} OnrRrInfo;

#define ONR_RR_INFO_SIZE_V1 ((uint32_t)offsetof(OnrRrInfo, backend))

typedef struct OnrRrApi {
    uint32_t abi_version;
    int32_t (*Check)(const void* adapter_luid, const wchar_t* dlssd_path);
    int32_t (*Create)(const OnrRrCreateInfo* info, void** context);
    int32_t (*Destroy)(void* context);
    int32_t (*SetMode)(void* context, uint32_t mode);   // only fast is built: a no-op that keeps fast
    int32_t (*RecordInputs)(void* context, const OnrRrFrame* frame, void* command_list, void** job);
    int32_t (*Enqueue)(void* context, void* job, void* command_queue);
    int32_t (*RecordOutput)(void* context, void* job, void* command_list);
    int32_t (*Cancel)(void* context, void* job);
    int32_t (*GetInfo)(void* context, OnrRrInfo* info);
    int32_t (*GetLastError)(char* buffer, uint32_t chars);   // this thread's last error, UTF-8
} OnrRrApi;

enum OnrRrEdit {
    ONR_RR_EDIT_OFF = 0,
    ONR_RR_EDIT_LOOP = 1,
    ONR_RR_EDIT_DISPLAY = 2,
};

typedef struct OnrRrDenoisedImage {
    uint32_t size;                 // sizeof(OnrRrDenoisedImage)
    void* buffer;
    uint32_t format;               // DXGI_FORMAT: R32G32B32A32_FLOAT, or R16G16B16A16_FLOAT when asked for
    uint32_t width, height;        // this frame's render extent
    uint32_t row_pitch;
} OnrRrDenoisedImage;

typedef struct OnrRrApi2 {
    OnrRrApi v1;                   // abi_version = 2
    int32_t (*PrepareDenoised)(void* context, void* job, uint32_t edit, OnrRrDenoisedImage* image);
    int32_t (*EnqueueNetwork)(void* context, void* job, void* command_queue, uint32_t* exported);
    int32_t (*EnqueuePost)(void* context, void* job, void* command_queue, uint32_t format);
} OnrRrApi2;

typedef struct OnrRrHopTiming {
    uint32_t size;                 // sizeof(OnrRrHopTiming)
    uint32_t reserved;
    uint64_t hop;
    uint64_t net_begin, net_end, post_begin, post_end;
} OnrRrHopTiming;

typedef struct OnrRrApi3 {
    OnrRrApi2 v2;                  // v2.v1.abi_version = 3
    int32_t (*GetHopTiming)(void* context, uint64_t hop, OnrRrHopTiming* timing);
} OnrRrApi3;

typedef struct OnrRrApi4 {
    OnrRrApi3 v3;                  // v3.v2.v1.abi_version = 4
    int32_t (*GetBackend)(void* context, uint32_t* backend);   // OnrRrBackend
    int32_t (*Record)(void* context, const OnrRrFrame* frame, void* command_list, uint32_t edit, OnrRrDenoisedImage* image,
                      void** job);
    int32_t (*RecordPost)(void* context, void* job, void* command_list, uint32_t format);
} OnrRrApi4;

typedef struct OnrRrApi5 {
    OnrRrApi4 v4;                  // v4.v3.v2.v1.abi_version = 5
    int32_t (*PrepareVkDevice)(void* physical_device, const void* create_info, void* out, void** setup);
    int32_t (*FinishVkDevice)(void* setup, void* device, uint32_t* queue_family, uint32_t* queue_index);
    int32_t (*RecordVk)(void* context, const OnrRrFrame* frame, void* command_buffer);
    int32_t (*NoteD3d12Fence)(void* context, uint64_t serial, void* fence, uint64_t value);
    int32_t (*NoteD3d12Invalidated)(void* context, uint64_t serial);
} OnrRrApi5;

typedef int32_t (*PFN_OnrRrGetApi)(uint32_t abi_version, OnrRrApi* api);
#ifdef ONR_RR_BUILDING
__declspec(dllexport)
#endif
int32_t OnrRrGetApi(uint32_t abi_version, OnrRrApi* api);
#ifdef ONR_RR_BUILDING
__declspec(dllexport)
#endif
int32_t OnrRrSetDeltaRate(void* context, float rate);

#ifdef __cplusplus
}
#endif
