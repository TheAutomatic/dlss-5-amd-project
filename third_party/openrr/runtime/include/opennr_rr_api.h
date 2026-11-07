// SPDX-License-Identifier: MIT
// OpenNR RR runtime (OpenNrRrRuntime.dll): C interface for a host that intercepts a game's Ray Reconstruction
// (NGX DLSS-D) evaluate on D3D12. The network runs on a Vulkan device of its own on the game's adapter; the game's
// textures reach it through shared D3D12 buffers and a shared fence.
//
// Per frame, on the game's command list:
//   RecordInputs(ctx, frame, list, &job)   copies the guides into the shared buffers (the textures go to
//                                          COPY_SOURCE and back to the state given in the frame)
//   the host splits the list in two and, between the halves, on the queue that runs them, calls
//   Enqueue(ctx, job, queue)               signals the shared fence, submits the network on Vulkan behind it and
//                                          makes the queue wait for the network
//   RecordOutput(ctx, job, list)           on the second half: copies the result into frame.output
// Cancel(ctx, job) drops a job whose list will not run (reset or released unexecuted).
//
// ABI 2 (OnrRrGetApi(2, &api2.v1), OnrRrApi2) adds a hop for the host on the denoised image:
//   PrepareDenoised(ctx, job, edit, &image)     after RecordInputs: asks for the hop and names the shared buffer
//                                               the image will be in, so the host can record its lists now
//   between the halves, in place of Enqueue:
//   EnqueueNetwork(ctx, job, queue, &exported)  runs the network; for a prepared job it stops at the denoised
//                                               render-size image, leaves it in image.buffer (colour / exposure,
//                                               the game's colour space) and sets exported; the queue waits for it
//   the host edits the image on the same queue (its own lists), writing the result back into image.buffer
//   EnqueuePost(ctx, job, queue, format)        reads the edit back (format = what the host wrote, 0 = no edit)
//                                               and runs the temporal post and the downsample; the queue waits
// Without PrepareDenoised (or exported = 0) EnqueueNetwork is Enqueue and EnqueuePost a no-op.
//
// ABI 3 (OnrRrGetApi(3, &api3.v2.v1), OnrRrApi3) adds GetHopTiming: when the network and the post of a hop ran on the
// GPU, in QueryPerformanceCounter ticks, so the host can place its own work between them.
//
// ABI 4 (OnrRrGetApi(4, &api4.v3.v2.v1), OnrRrApi4) adds the D3D12 backend: the kernels run on the game's own device
// and are recorded straight into the game's command list at the evaluate point, with no queue of their own, no
// fence and no split of the list:
//   Record(ctx, frame, list, edit, &image, &job)   the whole frame (inputs, network, post, output) into list; with an
//                                                  edit it stops at the denoised image in image.buffer (job set)
//   the host splits the list there, edits the image on the queue between the halves, then on the second half
//   RecordPost(ctx, job, list, format)             the edit back in, the post, the output
// The backend is picked at Create (OnrRrCreateInfo.backend; a host that does not set it gets Vulkan) after a check on
// the GPU; GetBackend says which one runs. It can change from D3D12 to Vulkan once, while the network is being built
// (a kernel the D3D12 driver did not take), so a host asks every frame. With the Vulkan backend the frame goes
// through RecordInputs / Enqueue / RecordOutput as before; with D3D12 those return ONR_RR_UNSUPPORTED.
//
// ABI 5 (OnrRrGetApi(5, &api5.v4.v3.v2.v1), OnrRrApi5) adds the Vulkan host backend, for Vulkan games: the kernels run
// on the game's own VkDevice and are recorded into the game's command buffer at the evaluate point, as on D3D12.
//   at vkCreateDevice: PrepareVkDevice(physical, &info, &out, &setup) adds what the kernels need to the game's create
//   info (extensions, features, one queue of the device left to the runtime); the host creates the device with out,
//   then FinishVkDevice(setup, ...) (always, also when the create failed) says which queue that is
//   Create with OnrRrCreateInfo.vk_device (d3d12_device null)
//   per frame RecordVk(ctx, frame, command_buffer): the whole frame (inputs, network, post, output)
// OnrRrTexture.resource is then an OnrRrVkImage* and OnrRrTexture.state its VkImageLayout.
//
// Create returns at once; the weights and the kernels are built on a background thread. Until they are ready
// (and after a failure) frames still go through Enqueue/RecordOutput and the output is the colour, scaled
// bilinearly. GetInfo says which state the runtime is in and why.
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

#define ONR_RR_RUNTIME_VERSION "0.1.2"
#define ONR_RR_RUNTIME_VERSION_RC 0, 1, 2, 0

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

// A texture of the frame: null resource = absent. state = the D3D12_RESOURCE_STATES it is in when the frame is
// recorded; it is left in that state. (x, y) = top-left of the subrect the frame uses.
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
    // Optional tail (size may be ONR_RR_CREATE_INFO_SIZE_V1 without it: Vulkan backend)
    uint32_t backend;              // OnrRrBackendChoice
    // Optional tail (size may be ONR_RR_CREATE_INFO_SIZE_V2 without it): the game's Vulkan device (ABI 5). With it
    // d3d12_device and backend are not read and the frames go through RecordVk.
    const struct OnrRrVkDevice* vk_device;
} OnrRrCreateInfo;

#define ONR_RR_CREATE_INFO_SIZE_V1 ((uint32_t)offsetof(OnrRrCreateInfo, backend))
#define ONR_RR_CREATE_INFO_SIZE_V2 ((uint32_t)offsetof(OnrRrCreateInfo, vk_device))

// The game's Vulkan device (ABI 5). Its instance must be Vulkan 1.3 or later and the device created with the create
// info PrepareVkDevice gave; queue_family / queue_index: the queue FinishVkDevice named, which the host never uses.
// The device must be idle when Destroy is called (no command buffer we recorded into still pending).
typedef struct OnrRrVkDevice {
    uint32_t size;                 // sizeof(OnrRrVkDevice)
    uint32_t api_version;          // the instance's apiVersion
    void* instance;                // VkInstance
    void* physical_device;         // VkPhysicalDevice
    void* device;                  // VkDevice
    uint32_t queue_family, queue_index;
} OnrRrVkDevice;

// A texture of a Vulkan frame: OnrRrTexture.resource points to one (read during the call only), OnrRrTexture.state is
// its VkImageLayout, left as it is. The NGX Vulkan resource's image view info, one mip level of one layer. With a view
// the runtime may read the texture through it (a float format, or 8-bit unorm albedos, in a layout shader reads take)
// and write the output through it (rgba16f, GENERAL, usage with STORAGE); otherwise it copies.
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
    // Optional tail (size may be ONR_RR_FRAME_SIZE_V1 without it; older runtimes ignore it): the transparency layer
    // NGX composites over the output (premultiplied RGBA at the render extent: DLSS.TransparencyLayer, or
    // DLSSD.ColorBeforeParticles, which NGX takes for it), and its separate opacity (DLSS.TransparencyLayerOpacity,
    // rgb, its own subrect; read only from ONR_RR_FRAME_SIZE_V4 on: older hosts left it unread).
    OnrRrTexture transparency_layer;
    OnrRrTexture transparency_opacity;
    // Optional tail (size may be ONR_RR_FRAME_SIZE_V2 without it): two more guides nvngx_dlssd.dll 310.9.1 feeds its
    // first encoder, red channel, render extent: DLSS.DisocclusionMask (at its own subrect) and
    // DLSSD.ScreenSpaceSubsurfaceScatteringGuide, for which NGX takes DLSSD.ColorBeforeFog when that is given (read at
    // the colour's subrect: pass the colour's x, y).
    OnrRrTexture disocclusion_mask;
    OnrRrTexture sss_guide;
    // Optional tail (size may be ONR_RR_FRAME_SIZE_V3 without it): the disocclusion mask is multiplied by this before
    // DM_INVERT and DM_CLAMP; 0 or absent = 1 (NGX takes the mask as is)
    float disocclusion_mask_scale;
    // Optional tail (size may be ONR_RR_FRAME_SIZE_V4 without it): the rest of what nvngx_dlssd.dll 310.9.1 feeds rr2
    // (docs/NOTA-ENTRADAS.md). The first encoder takes DLSSD.ResponsivityMask (red, its own subrect),
    // DLSSD.ColorBeforeTransparency (rgb) and DLSSD.DepthOfFieldGuide (red), both read at the colour's subrect (pass
    // the colour's x, y), and GBuffer.SpecularMvec, the reflections' motion vectors in place of the ones it derives
    // from depth, normals and the hit distance (read at the motion vectors' subrect, scaled as them). The downsample
    // takes DLSS.TransparencyLayerMvecs (the layer's own motion vectors, its own subrect; NGX reads them at the output
    // pixel and multiplies them by MV.Scale); transparency_opacity above is read from this size on. Without
    // DLSSD.SpecularHitDistance the reflections move with the pixel's motion vector, as in NGX
    // (specular_ray_direction_hit_distance is not read: rr2 has no input for it).
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
    // Optional tail (size may be ONR_RR_INFO_SIZE_V1; older runtimes leave it as it was)
    uint32_t backend;              // OnrRrBackend running now
    char backend_message[124];     // UTF-8: why D3D12 is not used (empty when it is, or when it was not asked for)
} OnrRrInfo;

#define ONR_RR_INFO_SIZE_V1 ((uint32_t)offsetof(OnrRrInfo, backend))

typedef struct OnrRrApi {
    uint32_t abi_version;
    // Without a context: whether this adapter (LUID) and nvngx_dlssd.dll can run the network; 0 = yes, otherwise
    // ONR_RR_UNSUPPORTED and the reason in GetLastError. dlssd_path may be null (only the GPU is checked).
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

// What the post does with the host's edit of the denoised image.
enum OnrRrEdit {
    ONR_RR_EDIT_OFF = 0,
    ONR_RR_EDIT_LOOP = 1,      // the edit replaces the network's image: history and display both take it
    ONR_RR_EDIT_DISPLAY = 2,   // the network keeps its own history; the shown image adds the edit (a delta with a fast history)
};

// The denoised image of one frame. buffer: ID3D12Resource* (a buffer in D3D12_RESOURCE_STATE_COMMON, left there),
// the image at offset 0 as a placed footprint of format / width / height / row_pitch. The host writes its edit back
// at offset 0 in a format of its choice with row pitch = width * texel bytes rounded up to 256 (no wider than the
// export's texel). buffer is null when there is no hop for this job (edit OFF, network not ready).
// format on the way in: DXGI_FORMAT_R16G16B16A16_FLOAT asks for a half-float export (half the bytes to copy; the
// colour over the exposure saturates at 65504); anything else gets R32G32B32A32_FLOAT.
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
    // exported: 1 when the image is in the buffer and EnqueuePost runs the rest, 0 when the frame ran whole
    int32_t (*EnqueueNetwork)(void* context, void* job, void* command_queue, uint32_t* exported);
    // format: the DXGI_FORMAT of the edit in image.buffer, 0 when the host wrote none (the network's image is used)
    int32_t (*EnqueuePost)(void* context, void* job, void* command_queue, uint32_t format);
} OnrRrApi2;

// GPU timing of one hop. hop: 1 for the first EnqueuePost that ran a post after an export, 2 for the next, and so on.
// The times are QueryPerformanceCounter ticks: the first dispatch's start and the last dispatch's end of the
// network's submit and of the post's (the waits on the shared fence before them not included).
typedef struct OnrRrHopTiming {
    uint32_t size;                 // sizeof(OnrRrHopTiming)
    uint32_t reserved;
    uint64_t hop;
    uint64_t net_begin, net_end, post_begin, post_end;
} OnrRrHopTiming;

typedef struct OnrRrApi3 {
    OnrRrApi2 v2;                  // v2.v1.abi_version = 3
    // the timing of hop hop once both its submits finished and were read back (a few frames later): ONR_RR_OK;
    // ONR_RR_FAILED while it is not known (yet, or any more: the last 16 are kept, or no calibrated clock)
    int32_t (*GetHopTiming)(void* context, uint64_t hop, OnrRrHopTiming* timing);
} OnrRrApi3;

typedef struct OnrRrApi4 {
    OnrRrApi3 v3;                  // v3.v2.v1.abi_version = 4
    int32_t (*GetBackend)(void* context, uint32_t* backend);   // OnrRrBackend
    // D3D12 backend: the frame into command_list (the game's, at the evaluate point; the textures are left in the
    // states the frame gives, the list's compute root signature and pipeline are not restored). edit (OnrRrEdit) and
    // image as PrepareDenoised: with a hop the network stops at the denoised image and *job is set for RecordPost;
    // without one *job is null and the frame is done. image may be null when edit is ONR_RR_EDIT_OFF.
    int32_t (*Record)(void* context, const OnrRrFrame* frame, void* command_list, uint32_t edit, OnrRrDenoisedImage* image,
                      void** job);
    // the rest of a job Record stopped at the image: format as EnqueuePost (0 = no edit). Cancel drops one that will
    // not run.
    int32_t (*RecordPost)(void* context, void* job, void* command_list, uint32_t format);
} OnrRrApi4;

typedef struct OnrRrApi5 {
    OnrRrApi4 v4;                  // v4.v3.v2.v1.abi_version = 5
    // Without a context, at the game's vkCreateDevice. physical_device: VkPhysicalDevice; create_info: the game's
    // VkDeviceCreateInfo; out: a VkDeviceCreateInfo the runtime fills, valid until FinishVkDevice. ONR_RR_OK: create the
    // device with out. ONR_RR_UNSUPPORTED (GetLastError says why): out is the game's own, the Vulkan host backend is
    // not offered. Structures of the game's chain may be changed in place until FinishVkDevice puts them back.
    int32_t (*PrepareVkDevice)(void* physical_device, const void* create_info, void* out, void** setup);
    // After the create (device: the VkDevice, null when it failed). The runtime's queue; ONR_RR_OK when setup was
    // prepared and the device created.
    int32_t (*FinishVkDevice)(void* setup, void* device, uint32_t* queue_family, uint32_t* queue_index);
    // The whole frame into command_buffer (a VkCommandBuffer of the device, recording, outside a render pass): the
    // textures go to TRANSFER_SRC (the output to TRANSFER_DST) and back to the layouts the frame gives; the compute
    // pipeline, descriptor sets and push constants bound in it are not restored. Until the network is ready the output
    // is the colour, scaled bilinearly.
    int32_t (*RecordVk)(void* context, const OnrRrFrame* frame, void* command_buffer);
} OnrRrApi5;

// abi_version 1 fills an OnrRrApi; 2 fills the OnrRrApi2 that api points to; 3 the OnrRrApi3; 4 the OnrRrApi4; 5 the
// OnrRrApi5.
typedef int32_t (*PFN_OnrRrGetApi)(uint32_t abi_version, OnrRrApi* api);
// exported by OpenNrRrRuntime.dll
#ifdef ONR_RR_BUILDING
__declspec(dllexport)
#endif
int32_t OnrRrGetApi(uint32_t abi_version, OnrRrApi* api);
// exported beside it, resolved by name (an older DLL has none): the rate the display's NR delta history blends at
// where the camera moves, 0..1, default 0.05 (FASE5-NR section 13). Before Record, on the render thread.
#ifdef ONR_RR_BUILDING
__declspec(dllexport)
#endif
int32_t OnrRrSetDeltaRate(void* context, float rate);
// The user's "Fast mode": cheaper kernels with a small, measured loss (docs/FASE24-FASTMODE.md). Takes effect on the next
// recorded frame, keeping every history; the kernels are built on the first frame that asks. Get: 0 off, 1 on, 2 building,
// 3 not available. Before Record, on the render thread.
#ifdef ONR_RR_BUILDING
__declspec(dllexport)
#endif
int32_t OnrRrSetFastMode(void* context, int32_t on);
#ifdef ONR_RR_BUILDING
__declspec(dllexport)
#endif
int32_t OnrRrGetFastMode(void* context);

#ifdef __cplusplus
}
#endif
