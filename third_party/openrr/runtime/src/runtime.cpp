// SPDX-License-Identifier: MIT
// OpenNrRrRuntime.dll: the rr2 network (gpu/ library) behind the C interface of opennr_rr_api.h, on a Vulkan device
// of its own on the game's adapter. The game's textures are copied into a shared D3D12 buffer on the game's list,
// the network reads them there through Vulkan, and writes the result into a second shared buffer the game's list
// copies into its output texture. A shared fence pair orders the two queues. ABI 2 splits the network's submit at
// the denoised image (EnqueueNetwork / EnqueuePost) so the host can edit it on the game's queue in between.
// ABI 4, D3D12 backend: the same kernels (the Vulkan driver's pipeline ELFs) on the game's device, recorded into the
// game's list by Record / RecordPost; the Vulkan device then only builds the ELFs (and is the fallback).
// ABI 5, Vulkan host backend: a Vulkan game's own device (set up at its vkCreateDevice by PrepareVkDevice), the frame
// recorded into the game's command buffer by RecordVk.
#define NOMINMAX
#define ONR_RR_BUILDING
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "opennr_rr/d3d12.hpp"
#include "opennr_rr/engine.hpp"
#include "opennr_rr_api.h"
#include "vk_host.hpp"

using namespace onr;

namespace {

thread_local std::string t_error;

// contexts alive in the process. The flow kernels' windows spin on row flags, which only holds while one network runs
// at a time: two of them on two queues can fill the GPU with waiting windows whose producers never get launched (a
// GPU hang; Indiana Jones creates a second, small feature for another view). With more than one context every level
// keeps its barrier, and on a Vulkan game's device always (its features come and go with others in flight).
// ONR_RR_FLOW=0: never, =1: always (measurements).
std::atomic<int> g_live{0};
const char* const g_flow = getenv("ONR_RR_FLOW");

int32_t fail(int32_t code, const std::string& what) {
    t_error = what;
    return code;
}

std::wstring module_dir() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_dir), &m);
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(m, p, MAX_PATH);
    return std::filesystem::path(p).parent_path().wstring();
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void copy_str(char* dst, size_t n, const std::string& s) {
    if (!n) return;
    size_t k = std::min(n - 1, s.size());
    memcpy(dst, s.data(), k);
    dst[k] = 0;
}

class Log {
public:
    void open(const std::wstring& path) {
        std::lock_guard l(m_);
        if (!f_) f_ = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
    }
    void close() {
        std::lock_guard l(m_);
        if (f_) fclose(f_), f_ = nullptr;
    }
    void operator()(const char* fmt, ...) {
        char b[4096];
        va_list a;
        va_start(a, fmt);
        vsnprintf(b, sizeof b, fmt, a);
        va_end(a);
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::lock_guard l(m_);
        if (!f_) return;
        fprintf(f_, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, b);
        fflush(f_);
    }

private:
    std::mutex m_;
    FILE* f_ = nullptr;
};
Log g_log;

// ------------------------------------------------------------------------------------------------ formats

// runtime texel format codes (gpu/shaders/include/onr_texel.glsl)
enum Tf : uint32_t {
    TF_NONE = 0, TF_RGBA16F, TF_RGBA32F, TF_R11G11B10F, TF_RGB10A2, TF_RGBA8, TF_BGRA8, TF_RG16F, TF_RG32F, TF_R16F, TF_R32F,
    TF_R8, TF_R16, TF_D24, TF_D32S8, TF_RGBA16, TF_RGBA8_SNORM, TF_RGBA16_SNORM, TF_RG8, TF_RG16, TF_RGBA8_SRGB, TF_BGRA8_SRGB,
    TF_RG16_SNORM, TF_RGB9E5
};

Tf tf_of(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return TF_RGBA16F;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return TF_RGBA32F;
    case DXGI_FORMAT_R11G11B10_FLOAT: return TF_R11G11B10F;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return TF_RGB10A2;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: return TF_RGBA8;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return TF_RGBA8_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM: return TF_BGRA8;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return TF_BGRA8_SRGB;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: return TF_RG16F;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: return TF_RG32F;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: return TF_R16F;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_D32_FLOAT: return TF_R32F;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: return TF_R8;
    case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_D16_UNORM: return TF_R16;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return TF_D24;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return TF_D32S8;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return TF_RGBA16;
    case DXGI_FORMAT_R8G8B8A8_SNORM: return TF_RGBA8_SNORM;
    case DXGI_FORMAT_R16G16B16A16_SNORM: return TF_RGBA16_SNORM;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: return TF_RG8;
    case DXGI_FORMAT_R16G16_UNORM: return TF_RG16;
    case DXGI_FORMAT_R16G16_SNORM: return TF_RG16_SNORM;
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return TF_RGB9E5;
    default: return TF_NONE;
    }
}

// a Vulkan game's formats; depth formats as their depth aspect copies (D32S8 gives 4-byte texels, D24S8 the depth in
// the low 24 bits of 4)
Tf tf_of_vk(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: return TF_RGBA16F;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return TF_RGBA32F;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return TF_R11G11B10F;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return TF_RGB10A2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return TF_RGBA8;
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return TF_RGBA8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return TF_BGRA8;
    case VK_FORMAT_B8G8R8A8_SRGB: return TF_BGRA8_SRGB;
    case VK_FORMAT_R16G16_SFLOAT: return TF_RG16F;
    case VK_FORMAT_R32G32_SFLOAT: return TF_RG32F;
    case VK_FORMAT_R16_SFLOAT: return TF_R16F;
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return TF_R32F;
    case VK_FORMAT_R8_UNORM: return TF_R8;
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_D16_UNORM: return TF_R16;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32: return TF_D24;
    case VK_FORMAT_R16G16B16A16_UNORM: return TF_RGBA16;
    case VK_FORMAT_R8G8B8A8_SNORM: return TF_RGBA8_SNORM;
    case VK_FORMAT_R16G16B16A16_SNORM: return TF_RGBA16_SNORM;
    case VK_FORMAT_R8G8_UNORM: return TF_RG8;
    case VK_FORMAT_R16G16_UNORM: return TF_RG16;
    case VK_FORMAT_R16G16_SNORM: return TF_RG16_SNORM;
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return TF_RGB9E5;
    default: return TF_NONE;
    }
}

bool vk_depth(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D16_UNORM_S8_UINT;
}
// the aspects a layout transition of the image names: depth and stencil together for a combined format
VkImageAspectFlags vk_aspects(VkFormat f) {
    if (f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT)
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    return vk_depth(f) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

bool tf_unorm(Tf t) { return t == TF_RGB10A2 || t == TF_RGBA8 || t == TF_BGRA8 || t == TF_RGBA16 || t == TF_RGBA8_SRGB || t == TF_BGRA8_SRGB; }
bool tf_writable(Tf t) {
    return t == TF_RGBA16F || t == TF_RGBA32F || t == TF_R11G11B10F || t == TF_RGB10A2 || t == TF_RGBA8 || t == TF_BGRA8 ||
           t == TF_RGBA8_SRGB || t == TF_BGRA8_SRGB || t == TF_RGBA16 || t == TF_RGB9E5;
}
// as tf_bytes in onr_texel.glsl
uint32_t tf_bytes(Tf t) {
    switch (t) {
    case TF_RGBA32F: return 16;
    case TF_RGBA16F: case TF_RG32F: case TF_D32S8: case TF_RGBA16: case TF_RGBA16_SNORM: return 8;
    case TF_R16F: case TF_R16: case TF_RG8: return 2;
    case TF_R8: return 1;
    default: return 4;
    }
}

// ------------------------------------------------------------------------------------------------ interop

struct VkFns {
    PFN_vkGetMemoryWin32HandlePropertiesKHR memProps = nullptr;
    PFN_vkImportSemaphoreWin32HandleKHR importSem = nullptr;
};

// A committed D3D12 buffer of the game's device that the Vulkan device reads and writes by address.
struct SharedBuffer {
    ID3D12Resource* res = nullptr;
    HANDLE handle = nullptr;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceAddress addr = 0;
    UINT64 size = 0;

    void create_d3d(ID3D12Device* d, UINT64 bytes) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        // small shared buffers had their handles refused for a while in a game (E_INVALIDARG): never below 16 MB
        rd.Width = std::max((bytes + 65535) & ~UINT64(65535), UINT64(16) << 20);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res));
        if (FAILED(hr)) throw std::runtime_error("shared buffer: CreateCommittedResource 0x" + std::to_string(unsigned(hr)));
        hr = d->CreateSharedHandle(res, nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(hr)) throw std::runtime_error("shared buffer: CreateSharedHandle 0x" + std::to_string(unsigned(hr)));
        size = rd.Width;
    }

    void import(Device& dev, const VkFns& fn, ID3D12Device* d) {
        if (buf) return;
        VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext};
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (vkCreateBuffer(dev.vk(), &bi, nullptr, &buf) != VK_SUCCESS) throw std::runtime_error("vkCreateBuffer (shared)");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev.vk(), buf, &req);
        VkMemoryWin32HandlePropertiesKHR props{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        if (fn.memProps(dev.vk(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, &props) != VK_SUCCESS)
            throw std::runtime_error("vkGetMemoryWin32HandlePropertiesKHR");
        VkMemoryAllocateFlagsInfo af{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        af.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &af};
        ded.buffer = buf;
        VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        imp.handle = handle;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
        D3D12_RESOURCE_DESC rd = res->GetDesc();
        ai.allocationSize = std::max<UINT64>(req.size, d->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes);
        ai.memoryTypeIndex = dev.mem_type_index(req.memoryTypeBits & props.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(dev.vk(), &ai, nullptr, &mem) != VK_SUCCESS) throw std::runtime_error("import of a D3D12 buffer");
        vkBindBufferMemory(dev.vk(), buf, mem, 0);
        VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        da.buffer = buf;
        addr = vkGetBufferDeviceAddress(dev.vk(), &da);
    }

    void release(VkDevice v) {
        if (buf) vkDestroyBuffer(v, buf, nullptr);
        if (mem) vkFreeMemory(v, mem, nullptr);
        if (handle) CloseHandle(handle);
        if (res) res->Release();
        *this = {};
    }
};

// A shared D3D12 fence the Vulkan device sees as a timeline semaphore.
struct SharedFence {
    ID3D12Fence* fence = nullptr;
    HANDLE handle = nullptr;
    VkSemaphore sem = VK_NULL_HANDLE;

    void create(ID3D12Device* d, Device& dev, const VkFns& fn) {
        if (FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) throw std::runtime_error("shared fence");
        if (FAILED(d->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &handle))) throw std::runtime_error("shared fence handle");
        VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
        if (vkCreateSemaphore(dev.vk(), &si, nullptr, &sem) != VK_SUCCESS) throw std::runtime_error("vkCreateSemaphore");
        VkImportSemaphoreWin32HandleInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        im.semaphore = sem;
        im.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        im.handle = handle;
        if (fn.importSem(dev.vk(), &im) != VK_SUCCESS) throw std::runtime_error("import of the D3D12 fence");
    }

    void release(VkDevice v) {
        if (sem && v) vkDestroySemaphore(v, sem, nullptr);
        if (handle) CloseHandle(handle);
        if (fence) fence->Release();
        *this = {};
    }
};

void transition(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    c->ResourceBarrier(1, &b);
}

// ------------------------------------------------------------------------------------------------ context

// G_RAY is never read (rr2 has no input for it); the slots from G_RESP on get their buffers on their first frame, and
// from 16 on they are always copied (the D3D12 table holds 16 views)
enum Slot { G_COLOR, G_ALBEDO, G_SPEC, G_NORMALS, G_ROUGH, G_DEPTH, G_MV, G_HIT, G_RAY, G_TL, G_DM, G_SSS, G_RESP, G_CBT, G_DOF, G_SMV, G_TLO, G_TLMV,
           G_COUNT };

void frame_textures(const OnrRrFrame& g, const OnrRrTexture* (&tex)[G_COUNT]) {
    const OnrRrTexture* t[G_COUNT] = {&g.color, &g.diffuse_albedo, &g.specular_albedo, &g.normals, &g.roughness, &g.depth,
                                       &g.motion, &g.specular_hit_distance, &g.specular_ray_direction_hit_distance, &g.transparency_layer,
                                       &g.disocclusion_mask, &g.sss_guide, &g.responsivity_mask, &g.color_before_transparency,
                                       &g.depth_of_field_guide, &g.specular_motion, &g.transparency_opacity, &g.transparency_motion};
    std::copy(t, t + G_COUNT, tex);
}

struct Copy {
    bool present = false;
    Tf tf = TF_NONE;
    UINT64 offset = 0;
    UINT pitch = 0;
    uint32_t x = 0, y = 0;   // subrect base
    bool direct = false;     // D3D12: read from the texture itself, view `slot` of the recording's table
    uint32_t tw = 0, th = 0; // the texture's size
    uint32_t fmt = 0;        // its DXGI_FORMAT or VkFormat
};

struct Job {
    uint64_t seq = 0;
    OnrRrFrame f{};
    Copy in[G_COUNT];
    std::shared_ptr<SharedBuffer> input, output, image;
    UINT64 out_bytes = 0;
    UINT out_pitch = 0;
    Tf out_tf = TF_NONE;
    bool enqueued = false, output_recorded = false;
    // ABI 2: the network ran up to the denoised image; EnqueuePost runs the rest
    bool post_pending = false;
    uint32_t edit = ONR_RR_EDIT_OFF;
    UINT image_pitch = 0;
    bool image_half = false;   // rgba16f export (the host asked for it)
    bool d3d = false;          // ABI 4: Record stopped at the image, RecordPost runs the rest
};

// a readback copy of the disocclusion mask or the SSS guide, summed up in the log some frames later
struct GuideStats {
    ID3D12Resource* rb = nullptr;
    UINT64 bytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t tw = 0, th = 0, x = 0, y = 0, w = 0, h = 0;
    bool pending = false;
};

// frames in flight; each has a submit for the network (or the whole frame) and one for the post (ABI 2)
constexpr int kSlots = 3;
constexpr int kSubmits = 2 * kSlots;

struct Ctx {
    Ctx() { ++g_live; }
    std::mutex m;
    ID3D12Device* d3d = nullptr;
    Device dev;
    VkFns fn;
    Kernels kern;                    // ingest, pack
    Kernels net;                      // the fast network (*_f); published by `built`
    // the user's "Fast mode": the cheaper kernels (*_lt, Engine::set_lite), built once it is asked for
    Kernels net_lite;
    std::atomic<bool> lite_want{false}, lite_started{false}, lite_built{false}, lite_failed{false};
    std::thread lite_builder;
    bool lite_logged = false;
    std::atomic<bool> built{false};
    std::atomic<bool> building{false};
    std::thread builder;
    Weights w;
    std::atomic<bool> weights_ok{false};
    Engine engine;
    bool engine_ready = false;
    std::atomic<uint32_t> state{ONR_RR_STATE_BUILDING};
    std::string message, weights_source;
    std::wstring dlssd_path, shader_dir;
    I2 out_size{}, max_render{};
    SharedFence toVk, fromVk;
    uint64_t value = 0;
    std::shared_ptr<SharedBuffer> input, output, image;
    std::vector<std::shared_ptr<SharedBuffer>> retired;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd[kSubmits] = {};
    VkFence done[kSubmits] = {};
    VkQueryPool qp[kSubmits] = {};
    bool inflight[kSubmits] = {}, timed[kSubmits] = {};
    uint32_t nq[kSubmits] = {};
    uint32_t qnet[kSubmits][2] = {};   // first and end query of the network's dispatches in the submit
    uint64_t span[kSubmits][2] = {};   // GPU ticks of the first dispatch's start and the last one's end
    uint64_t hop_of[kSubmits] = {};    // ABI 3: the hop a submit belongs to, 0 for none
    uint64_t hops = 0;
    OnrRrHopTiming hop_ring[16] = {};
    float split[3] = {};               // ingest / network / pack time, ms, summed over split_n submits since the last log
    std::vector<double> gaps;          // ONR_RR_STAMPS: every timestamp interval, summed over gaps_n recordings
    uint32_t gaps_n = 0;
    std::vector<std::string> stamp_names;   // ONR_RR_STAMPS on D3D12: the dispatches, logged once
    uint32_t split_n = 0;
    Buffer guides[G_COUNT];
    Gpu* guides_dev = nullptr;
    Buffer out16;
    I2 guide_size{};
    uint64_t seq = 0, frames = 0;
    float gpu_last = 0;
    std::vector<float> gpu_hist;
    uint32_t gpu_timing_key = 0;
    float gpu_base_ms = 0;
    float gpu_clean_ms = 0;
    std::map<Job*, std::unique_ptr<Job>> jobs;
    std::string last_logged_error;
    uint32_t logged_edit = ONR_RR_EDIT_OFF;
    int logged_tl = -1;                // the transparency layer's presence last logged
    int logged_opt = -1;               // the disocclusion mask's and the SSS guide's presence last logged
    float logged_dm_scale = 1.0f;
    int logged_more = -1;              // the V4 guides' presence (and the hit distance's absence) last logged
    bool logged_ray = false;
    GuideStats stats[2];               // disocclusion mask, SSS guide
    std::shared_ptr<std::atomic<bool>> stats_busy = std::make_shared<std::atomic<bool>>(false);   // a summing thread runs
    std::thread stats_thread;
    uint64_t stats_seq = 0;            // the frame their copies were recorded on
    ULONGLONG stats_tick = 0;
    // D3D12 backend (ABI 4): the game's device, its own kernels and buffers; d3d says it runs the frames
    std::unique_ptr<D3dDevice> dx;
    std::atomic<bool> use_d3d{false};
    std::string d3d_message;           // why it is not used
    Kernels dkern;                     // ingest, pack, d3d_ingest, d3d_pack, selftest on dx
    bool direct_ok = false;            // d3d_ingest / d3d_pack read and write the game's textures (their check passed)
    Buffer dinput, doutput, dimage;    // the guides' copies, the output's rows, the denoised image (hop)
    uint64_t drecords = 0;             // recordings into the game's lists (dx ring slots)
    uint32_t dq[D3dDevice::kSlots] = {}, dqnet[D3dDevice::kSlots][2] = {};
    std::wstring cache_dir;
    // Vulkan host backend (ABI 5): dev is the game's device, the frames recorded into its command buffers
    bool vk_host = false;
    bool small_view = false;           // Vulkan host: a secondary view's feature, the colour scaled without the network
    static constexpr uint32_t kHostSlots = 64;
    VkQueryPool hqp[kHostSlots] = {};
    uint32_t hq[kHostSlots] = {}, hqnet[kHostSlots][2] = {};
    uint64_t hrecords = 0;
    Buffer vinput, voutput;            // the guides' copies, the output's rows
    VkImageLayout logged_layout[G_COUNT + 1] = {};
    int logged_flow = -1;

    void set_message(const std::string& s) {
        message = s;
        g_log("%s", s.c_str());
    }
};

// optional: the enc0 halves with the 64-byte record, for a depth of field guide or a responsivity mask
void load_wide(Kernels& k, Gpu& d, const std::wstring& dir) {
    for (const char* n : {"enc0_attr_f_w", "enc0_stage_32_64_f_w", "d3d_enc0_attr_f_w"}) {
        if (!strncmp(n, "d3d_", 4) && !k.has("d3d_enc0_attr_f")) continue;
        try {
            k.load(d, utf8(dir), {n});
        } catch (const std::exception& e) {
            g_log("%s not loaded (the responsivity mask and the depth of field guide are left out): %s", n, e.what());
        }
    }
}

// the small levels without barriers between them (Engine::flow_kernel_names); without them each keeps its barrier
void load_flow(Kernels& k, Gpu& d, const std::wstring& dir) {
    try {
        k.load(d, utf8(dir), Engine::flow_kernel_names());
    } catch (const std::exception& e) {
        g_log("small levels with barriers (flow kernels not loaded): %s", e.what());
    }
}

void build_lite(Ctx* c);

std::mutex g_cached_weights_m;
std::wstring g_cached_dlssd_path;
Weights g_cached_weights;

void build_network(Ctx* c) {
    try {
        if (!c->weights_ok) {
            auto t0 = std::chrono::steady_clock::now();
            std::string err;
            bool reused = false;
            {
                std::lock_guard wl(g_cached_weights_m);
                if (!g_cached_dlssd_path.empty() && g_cached_dlssd_path == c->dlssd_path && !g_cached_weights.version.empty()) {
                    c->w = g_cached_weights;
                    reused = true;
                }
            }
            if (!reused && !build_weights(c->dlssd_path, c->w, err)) throw std::runtime_error("nvngx_dlssd.dll: " + err);
            c->weights_ok = true;
            g_log("weights %s from %s (version %s) in %.0f ms", reused ? "reused" : "rebuilt", c->weights_source.c_str(), c->w.version.c_str(),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        auto t0 = std::chrono::steady_clock::now();
        auto names = Engine::kernel_names(c->w, true);
        Kernels& k = c->net;
        if (c->use_d3d) {
            const uint32_t h0 = c->dx->cache_hits(), m0 = c->dx->cache_misses();
            try {
                k.load(*c->dx, utf8(c->shader_dir), names);
                try {   // optional: the downsample straight into the output texture (without them, d3d_pack)
                    k.load(*c->dx, utf8(c->shader_dir), {"d3d_downsample", "d3d_downsample_tl"});
                } catch (const std::exception& e) {
                    g_log("downsample into the output texture off: %s", e.what());
                }
                try {   // optional: the guides read from the game's textures by the network (without them, the ingest)
                    k.load(*c->dx, utf8(c->shader_dir), {"d3d_enc0_attr_f", "d3d_ae"});
                } catch (const std::exception& e) {
                    g_log("guides through the ingest: %s", e.what());
                }
                load_wide(k, *c->dx, c->shader_dir);
                load_flow(k, *c->dx, c->shader_dir);
                g_log("network kernels ready on D3D12 (%zu pipelines: %u ELFs from the cache, %u built) in %.1f s", names.size(), c->dx->cache_hits() - h0, c->dx->cache_misses() - m0,
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            } catch (const std::exception& e) {
                std::lock_guard l(c->m);
                k.unload(*c->dx);
                // the network never ran on D3D12 here: the frames simply go to Vulkan from now on
                if (c->engine_ready) throw;
                c->use_d3d = false;
                c->d3d_message = std::string("a network kernel did not build on D3D12: ") + e.what();
                g_log("D3D12 backend off, Vulkan from now on: %s", c->d3d_message.c_str());
                t0 = std::chrono::steady_clock::now();
            }
        }
        if (!c->use_d3d) {
            k.load(c->dev, utf8(c->shader_dir), names);
            if (c->vk_host && c->direct_ok) {
                try {
                    k.load(c->dev, utf8(c->shader_dir), {"d3d_downsample", "d3d_downsample_tl"});
                } catch (const std::exception& e) {
                    g_log("downsample into the output image off: %s", e.what());
                }
                try {
                    k.load(c->dev, utf8(c->shader_dir), {"d3d_enc0_attr_f", "d3d_ae"});
                } catch (const std::exception& e) {
                    g_log("guides through the ingest: %s", e.what());
                }
            }
            load_wide(k, c->dev, c->shader_dir);
            load_flow(k, c->dev, c->shader_dir);
            g_log("network kernels ready (%zu pipelines) in %.1f s", names.size(),
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        if (c->lite_want && !c->lite_started.exchange(true)) build_lite(c);
        c->built = true;
    } catch (const std::exception& e) {
        std::lock_guard l(c->m);
        c->state = ONR_RR_STATE_FAILED;
        c->set_message(std::string("network not built: ") + e.what());
    }
    c->building = false;
}

// the cheaper kernels, beside the running ones; until they are ready the frames stay on the others
void build_lite(Ctx* c) {
    Gpu& d = c->use_d3d ? static_cast<Gpu&>(*c->dx) : static_cast<Gpu&>(c->dev);
    auto t0 = std::chrono::steady_clock::now();
    try {
        const uint32_t h0 = c->use_d3d ? c->dx->cache_hits() : 0, m0 = c->use_d3d ? c->dx->cache_misses() : 0;
        c->net_lite.load_lite(d, utf8(c->shader_dir), c->net.names());
        if (c->net_lite.names().empty()) throw std::runtime_error("no *_lt kernels in the shader folder");
        g_log("fast mode kernels ready (%zu pipelines: %u ELFs from the cache, %u built) in %.2f s", c->net_lite.names().size(),
              c->use_d3d ? c->dx->cache_hits() - h0 : 0, c->use_d3d ? c->dx->cache_misses() - m0 : 0,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        c->lite_built = true;
    } catch (const std::exception& e) {
        { std::lock_guard l(c->m); c->net_lite.unload(d); }
        c->lite_failed = true;
        g_log("fast mode not available: %s", e.what());
    }
}

void start_build(Ctx* c) {
    if (c->building.exchange(true)) return;
    if (c->builder.joinable()) c->builder.join();
    c->builder = std::thread(build_network, c);
}

Buffer make_buffer(Gpu& d, size_t n) {
    Buffer b = d.create(n + 64);
    d.fill(b, 0);
    return b;
}

// the guides in the kernels' formats, on the device that runs the frames (preallocated up to max_render for DRS)
void ensure_guides(Ctx* c, Gpu& d, I2 render) {
    I2 cap{std::max({render.x, c->max_render.x, c->guide_size.x}),
           std::max({render.y, c->max_render.y, c->guide_size.y})};
    if (c->guides_dev == &d && cap.x <= c->guide_size.x && cap.y <= c->guide_size.y) return;
    if (c->guides_dev)
        for (auto& b : c->guides)
            if (b.addr) c->guides_dev->destroy(b);
    size_t px = size_t(cap.x) * cap.y;
    const size_t bpp[G_COUNT] = {8, 4, 4, 8, 2, 4, 4, 2, 0, 8, 4, 4};   // the rest: on their first frame (record_frame)
    for (int i = 0; i < G_COUNT; ++i)
        if (bpp[i] && !(c->use_d3d && c->direct_ok)) c->guides[i] = make_buffer(d, px * bpp[i]);
    c->guide_size = cap;
    c->guides_dev = &d;
}

// ingest push constants (gpu/shaders/ingest.comp): every guide in one dispatch
struct IngestPC {
    uint32_t pad[3];
    float depth_sign;
    uint64_t src;
    uint64_t dst[8];
    uint32_t g[8][4];   // offset, pitch, format | kind << 8 | flags << 16 | channel << 24, base x | y << 16
    I2 size;
    float proj[4];
};
static_assert(sizeof(IngestPC) <= 256);
struct PackPC {
    uint64_t src, dst;
    I2 src_size, dst_size;
    uint32_t pitch, fmt;
};

// the projection that turns hardware depth into view depth, and the sign that makes view depth positive ahead
void depth_projection(const OnrRrFrame& f, float proj[4], float& sign) {
    const float persp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, -0.1f, 0};
    bool zero = true;
    for (int i = 0; i < 16; ++i)
        if (f.view_to_clip[i] != 0.0f) { zero = false; break; }
    const float* m = zero ? persp : f.view_to_clip;
    proj[0] = m[10], proj[1] = m[11], proj[2] = m[14], proj[3] = m[15];
    sign = m[11] < 0 ? -1.0f : 1.0f;
}

// the part of the layer's motion vectors the downsample reads: from the subrect, at most the output size
I2 tlmv_extent(const Ctx* c, const Job& j) {
    const Copy& cp = j.in[G_TLMV];
    if (!cp.present) return {};
    const int64_t w = int64_t(cp.tw) - j.f.transparency_motion.x, h = int64_t(cp.th) - j.f.transparency_motion.y;
    return {int32_t(std::clamp<int64_t>(w, 1, c->out_size.x)), int32_t(std::clamp<int64_t>(h, 1, c->out_size.y))};
}

EngineFrame engine_frame(const Ctx* c, const Job& j) {
    const OnrRrFrame& f = j.f;
    float proj[4], sign;
    depth_projection(f, proj, sign);
    EngineFrame ef;
    ef.render = {int32_t(f.render_width), int32_t(f.render_height)}, ef.output = c->out_size;
    ef.jitter[0] = f.jitter_x, ef.jitter[1] = f.jitter_y;
    ef.mv_scale[0] = f.mv_scale_x, ef.mv_scale[1] = f.mv_scale_y;
    memcpy(ef.world_to_view, f.world_to_view, sizeof ef.world_to_view);
    memcpy(ef.view_to_clip, f.view_to_clip, sizeof ef.view_to_clip);
    ef.depth_sign = sign;
    ef.reset = (f.flags & ONR_RR_FRAME_RESET) != 0;
    if (j.in[G_TL].present) ef.transparency = c->guides[G_TL].addr;
    if (j.in[G_DM].present) ef.disocclusion = c->guides[G_DM].addr;
    if (j.in[G_SSS].present) ef.sss = c->guides[G_SSS].addr;
    if (j.in[G_RESP].present) ef.responsivity = c->guides[G_RESP].addr;
    if (j.in[G_CBT].present) ef.color_before_transparency = c->guides[G_CBT].addr;
    if (j.in[G_DOF].present) ef.depth_of_field = c->guides[G_DOF].addr;
    if (j.in[G_SMV].present) ef.specular_motion = c->guides[G_SMV].addr;
    if (j.in[G_TL].present && j.in[G_TLO].present) ef.transparency_opacity = c->guides[G_TLO].addr;
    if (j.in[G_TL].present && j.in[G_TLMV].present)
        ef.transparency_motion = c->guides[G_TLMV].addr, ef.transparency_motion_size = tlmv_extent(c, j);
    ef.no_hit = !j.in[G_HIT].present;
    return ef;
}

// where one recording goes: the Vulkan device or the D3D12 one, its ingest / pack kernels, the buffers the guides
// were copied into, the output rows go to and the denoised image goes through, and the network's two queries
struct Rec {
    Gpu& d;
    const Kernels& k;
    uint64_t input, output, image;
    uint32_t* qnet;
    const char* ingest = "ingest";
    const char* pack = "pack";
    uint32_t out_fmt = 0, out_pitch = 0;   // D3D12 straight into the output texture: TF_RGBA16F | 256, x | y << 16
};

void record_pack(Ctx* c, Rec& r, Job& j) {
    if (!c->out16.addr) c->out16 = r.d.create(size_t(c->out_size.x) * c->out_size.y * 8 + 64);
    r.d.barrier();
    PackPC pk{c->out16.addr, r.output, c->out_size, c->out_size, r.out_fmt ? r.out_pitch : j.out_pitch, r.out_fmt ? r.out_fmt : uint32_t(j.out_tf)};
    r.d.dispatch(r.k[r.pack], &pk, sizeof pk, div_up(c->out_size.x, 16), div_up(c->out_size.y, 16));
}

// ABI 2, second submit (ABI 4: RecordPost): the host's edit back in, the post, the downsample and the output
void record_post(Ctx* c, Rec& r, Job& j, Tf edited) {
    DfnImport in;
    DfnEdit edit = DfnEdit::none;
    if (edited != TF_NONE) {
        in = DfnImport{r.image, (j.f.render_width * tf_bytes(edited) + 255) & ~255u, uint32_t(edited)};
        edit = j.edit == ONR_RR_EDIT_DISPLAY ? DfnEdit::display : DfnEdit::loop;
    }
    const bool img = r.out_fmt && c->engine.output_image_ok();
    c->engine.set_output_image(img);
    if (!img && !c->out16.addr) c->out16 = r.d.create(size_t(c->out_size.x) * c->out_size.y * 8 + 64);
    r.qnet[0] = r.d.queries();
    r.d.stamp();
    c->engine.record_post(engine_frame(c, j), img ? uint64_t(r.out_pitch) : c->out16.addr, edit, in);
    c->engine.set_output_image(false);
    r.d.stamp();
    r.qnet[1] = r.d.queries();
    if (!img) record_pack(c, r, j);
}

// ONR_RR_DIRECT_GUIDES=0 (measurements): the ingest writes the guides even where the network can read them itself
const bool g_no_direct_guides = getenv("ONR_RR_DIRECT_GUIDES") && !strcmp(getenv("ONR_RR_DIRECT_GUIDES"), "0");

// network: the whole frame, or with split (ABI 2) only up to the denoised image, exported for the host
void record_frame(Ctx* c, Rec& r, Job& j, bool network, bool split = false) {
    Gpu& d = r.d;
    const OnrRrFrame& f = j.f;
    I2 render{int32_t(f.render_width), int32_t(f.render_height)};
    float proj[4], sign;
    depth_projection(f, proj, sign);
    IngestPC ip{{}, sign, r.input, {}, {}, render, {proj[0], proj[1], proj[2], proj[3]}};
    // guide slot from the game's texture src (absent: zeros)
    auto guide = [&](int slot, int src, uint32_t kind, uint32_t flags, uint32_t channel) {
        const Copy& cp = j.in[src];
        ip.dst[slot] = c->guides[slot].addr;
        if (!cp.present) flags = 4, channel = 0;
        if (cp.present && cp.direct) flags |= 8;
        ip.g[slot][0] = cp.direct ? uint32_t(src) : uint32_t(cp.offset), ip.g[slot][1] = cp.pitch;
        ip.g[slot][2] = uint32_t(cp.tf) | kind << 8 | flags << 16 | channel << 24;
        ip.g[slot][3] = cp.x | cp.y << 16;
    };
    const bool packed = (f.flags & ONR_RR_FRAME_ROUGHNESS_PACKED) || !j.in[G_ROUGH].present;
    const bool unpack = (f.flags & ONR_RR_FRAME_NORMALS_UNPACK) || tf_unorm(j.in[G_NORMALS].tf);
    guide(G_COLOR, G_COLOR, 0, 0, 0);
    guide(G_ALBEDO, G_ALBEDO, 1, 0, 0);
    guide(G_SPEC, G_SPEC, 1, 0, 0);
    guide(G_NORMALS, G_NORMALS, 2, unpack ? 1 : 0, 0);
    if (packed) guide(G_ROUGH, G_NORMALS, 3, 0, 3);
    else guide(G_ROUGH, G_ROUGH, 3, 0, 0);
    guide(G_DEPTH, G_DEPTH, 4, (f.flags & ONR_RR_FRAME_HW_DEPTH) ? 2 : 0, 0);
    guide(G_MV, G_MV, 5, 0, 0);
    guide(G_HIT, G_HIT, 3, 0, 0);
    // D3D12: the network reads the guides given as views (and the absent ones) itself, the same words through a table;
    // the ingest runs only for the copied ones
    GuideTable gt{};
    // Vulkan host: with a depth of field guide or a responsivity mask (the 64-byte record) the network's direct reads
    // (d3d_enc0_attr_f_w) hung the GPU beside another feature's work (validator: SECOND + CHURN + DOF); the ingest's
    // copies give the same bits
    const bool wide = j.in[G_DOF].present || j.in[G_RESP].present;
    const bool direct = network && !strcmp(r.ingest, "d3d_ingest") && c->engine.direct_guides_ok() && !g_no_direct_guides && !(c->vk_host && wide);
    bool ingest = true;
    if (direct) {
        memcpy(gt.g, ip.g, sizeof gt.g);
        memcpy(gt.proj, ip.proj, sizeof gt.proj);
        gt.depth_sign = ip.depth_sign;
        ingest = false;
        const size_t bpp8[8] = {8, 4, 4, 8, 2, 4, 4, 2};
        for (int s = 0; s < 8; ++s) {
            gt.buf[s] = ip.dst[s];
            if (((ip.g[s][2] >> 16) & 12u) != 0) {
                ip.dst[s] = 0;
            } else {
                if (!c->guides[s].addr) {
                    c->guides[s] = d.create(size_t(c->guide_size.x) * c->guide_size.y * bpp8[s] + 64);
                    d.fill(c->guides[s], 0, c->guides[s].size, 0);
                }
                ip.dst[s] = gt.buf[s] = c->guides[s].addr;
                ingest = true;
            }
        }
    }
    if (ingest) {
        const size_t bpp8[8] = {8, 4, 4, 8, 2, 4, 4, 2};
        for (int s = 0; s < 8; ++s) {
            if (!direct && (network || s == G_COLOR) && !c->guides[s].addr)
                c->guides[s] = d.create(size_t(c->guide_size.x) * c->guide_size.y * bpp8[s] + 64), ip.dst[s] = c->guides[s].addr;
        }
        d.dispatch(r.k[r.ingest], &ip, sizeof ip, div_up(render.x, 16), div_up(render.y, 16));
    }
    if (network) {
        // the optional guides the game gave, in dispatches of their own (eight slots each, the others without a
        // destination): the transparency layer, ColorBeforeTransparency and the layer's opacity as rgba16f, the
        // disocclusion mask, the SSS guide, the responsivity mask and the depth of field guide as r32f (kind 4 without
        // hardware depth), the reflections' and the layer's motion vectors as rg16f
        const int opt[] = {G_TL, G_DM, G_SSS, G_RESP, G_CBT, G_DOF, G_SMV, G_TLO};
        const uint32_t kind[] = {0, 4, 4, 4, 0, 4, 5, 0};
        const size_t bpp[] = {8, 4, 4, 4, 8, 4, 4, 8};
        IngestPC tp{{}, 1.0f, r.input, {}, {}, render, {}};
        const float dm_scale = f.disocclusion_mask_scale > 0.0f ? f.disocclusion_mask_scale : 1.0f;
        tp.proj[0] = dm_scale;   // read with flag 64 only
        int k = 0;
        for (int o = 0; o < int(std::size(opt)); ++o) {
            const Copy& cp = j.in[opt[o]];
            if (!cp.present) continue;
            Buffer& gb = c->guides[opt[o]];
            if (!gb.addr) gb = d.create(size_t(c->guide_size.x) * c->guide_size.y * bpp[o] + 64);   // fully written below
            tp.dst[k] = gb.addr;
            tp.g[k][0] = cp.direct ? uint32_t(opt[o]) : uint32_t(cp.offset), tp.g[k][1] = cp.pitch;
            const bool invert = opt[o] == G_DM && (f.flags & ONR_RR_FRAME_DM_INVERT);
            const bool clamp01 = opt[o] == G_DM && (f.flags & ONR_RR_FRAME_DM_CLAMP);
            const bool scale = opt[o] == G_DM && dm_scale != 1.0f;
            tp.g[k][2] = uint32_t(cp.tf) | kind[o] << 8 | (cp.direct ? 8u << 16 : 0u) | (invert ? 16u << 16 : 0u) |
                         (clamp01 ? 32u << 16 : 0u) | (scale ? 64u << 16 : 0u);
            tp.g[k][3] = cp.x | cp.y << 16;
            if (++k == 8) {
                d.dispatch(r.k[r.ingest], &tp, sizeof tp, div_up(render.x, 16), div_up(render.y, 16));
                k = 0;
                memset(tp.dst, 0, sizeof tp.dst);
            }
        }
        if (k) d.dispatch(r.k[r.ingest], &tp, sizeof tp, div_up(render.x, 16), div_up(render.y, 16));
        // the layer's motion vectors: NGX reads them at the output pixel, so they come in up to the output size
        if (const Copy& cp = j.in[G_TLMV]; cp.present && j.in[G_TL].present) {
            const I2 ext = tlmv_extent(c, j);
            Buffer& gb = c->guides[G_TLMV];
            if (!gb.addr) gb = d.create(size_t(c->out_size.x) * c->out_size.y * 4 + 64);
            IngestPC mp{{}, 1.0f, r.input, {}, {}, ext, {}};
            mp.dst[0] = gb.addr;
            mp.g[0][0] = uint32_t(cp.offset), mp.g[0][1] = cp.pitch;   // slot 17: always copied
            mp.g[0][2] = uint32_t(cp.tf) | 5u << 8;
            mp.g[0][3] = cp.x | cp.y << 16;
            d.dispatch(r.k[r.ingest], &mp, sizeof mp, div_up(ext.x, 16), div_up(ext.y, 16));
        }
    }
    if (network) {
        d.barrier();
        const bool flow = g_flow ? strcmp(g_flow, "0") != 0 : !c->vk_host && g_live.load() == 1;
        c->engine.set_flow(flow);
        if (int(flow) != c->logged_flow) {
            g_log("small levels: %s", flow ? "row flags" : c->vk_host && !g_flow ? "barriers (Vulkan host)" : "barriers (more than one network, or ONR_RR_FLOW=0)");
            c->logged_flow = int(flow);
        }
        const EngineFrame ef = engine_frame(c, j);
        Guides g{c->guides[G_COLOR].addr, c->guides[G_ALBEDO].addr, c->guides[G_SPEC].addr, c->guides[G_NORMALS].addr,
                 c->guides[G_ROUGH].addr, c->guides[G_DEPTH].addr, c->guides[G_MV].addr, c->guides[G_HIT].addr, direct ? &gt : nullptr};
        r.qnet[0] = d.queries();
        d.stamp();
        if (split) {
            c->engine.record_net(ef, g, r.image, j.image_pitch, j.edit == ONR_RR_EDIT_LOOP ? DfnEdit::loop : DfnEdit::display, j.image_half);
            d.stamp();
            r.qnet[1] = d.queries();
            return;
        }
        // straight into the output texture: the downsample writes it, no pack
        const bool img = r.out_fmt && c->engine.output_image_ok();
        c->engine.set_output_image(img);
        if (!img && !c->out16.addr) c->out16 = d.create(size_t(c->out_size.x) * c->out_size.y * 8 + 64);
        c->engine.record(ef, g, img ? uint64_t(r.out_pitch) : c->out16.addr);
        c->engine.set_output_image(false);
        d.stamp();
        r.qnet[1] = d.queries();
        if (!img) record_pack(c, r, j);
    } else {
        d.barrier();
        PackPC pk{c->guides[G_COLOR].addr, r.output, render, c->out_size, r.out_fmt ? r.out_pitch : j.out_pitch, r.out_fmt ? r.out_fmt : uint32_t(j.out_tf)};
        d.dispatch(r.k[r.pack], &pk, sizeof pk, div_up(c->out_size.x, 16), div_up(c->out_size.y, 16));
    }
}

// ONR_RR_STAMPS (measurements): a timestamp around every dispatch, the intervals summed for the log
const bool g_stamps = getenv("ONR_RR_STAMPS") != nullptr;
// ONR_RR_GUIDE_STATS (diagnostics): the disocclusion mask and the SSS guide summed up in the log every 3 s
const bool g_guide_stats = getenv("ONR_RR_GUIDE_STATS") != nullptr;
void add_gaps(Ctx* c, const std::vector<uint64_t>& ts, double ms) {
    if (!g_stamps || ts.size() < 2) return;
    if (c->gaps.size() != ts.size() - 1) c->gaps.assign(ts.size() - 1, 0.0), c->gaps_n = 0;
    for (size_t i = 0; i + 1 < ts.size(); ++i) c->gaps[i] += double(ts[i + 1] - ts[i]) * ms;
    ++c->gaps_n;
}

// the GPU time of a submit whose work finished (0 when it was not timed); part adds its ingest / network / pack
float harvest(Ctx* c, int s, float part[3]) {
    if (!c->timed[s] || c->nq[s] < 2) return 0;
    c->timed[s] = false;
    std::vector<uint64_t> ts(c->nq[s]);
    if (vkGetQueryPoolResults(c->dev.vk(), c->qp[s], 0, c->nq[s], ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return 0;
    c->span[s][0] = ts.front(), c->span[s][1] = ts.back();
    const double ms = c->dev.timestamp_period_ns() * 1e-6;
    add_gaps(c, ts, ms);
    const uint32_t a = c->qnet[s][0], b = c->qnet[s][1];
    if (b > a && b <= c->nq[s]) {
        part[0] += float(double(ts[a] - ts.front()) * ms);
        part[1] += float(double(ts[b - 1] - ts[a]) * ms);
        part[2] += float(double(ts.back() - ts[b - 1]) * ms);
    }
    return float(double(ts.back() - ts.front()) * ms);
}

// a hop's two submits, harvested, in QueryPerformanceCounter ticks (GetHopTiming)
void keep_hop_timing(Ctx* c, int s) {
    const uint64_t hop = c->hop_of[s];
    c->hop_of[s] = c->hop_of[s + kSlots] = 0;
    uint64_t gpu = 0, qpc = 0;
    LARGE_INTEGER fq;
    // a whole frame timed through ONR_RR_TIME_ALL: the post span is the network's end
    if (hop && c->span[s][1] && !c->span[s + kSlots][1]) c->span[s + kSlots][0] = c->span[s + kSlots][1] = c->span[s][1];
    if (!hop || !c->span[s][1] || !c->span[s + kSlots][1] || !c->dev.calibrate(gpu, qpc) || !QueryPerformanceFrequency(&fq)) return;
    const double k = c->dev.timestamp_period_ns() * 1e-9 * double(fq.QuadPart);
    auto at = [&](uint64_t t) { return uint64_t(int64_t(qpc) + int64_t(double(int64_t(t - gpu)) * k)); };
    c->hop_ring[hop % 16] = OnrRrHopTiming{sizeof(OnrRrHopTiming), 0, hop, at(c->span[s][0]), at(c->span[s][1]), at(c->span[s + kSlots][0]),
                                           at(c->span[s + kSlots][1])};
}

uint32_t timing_key(const Ctx* c) {
    if (!c->engine_ready) return 0;
    const I2 r = c->engine.render();
    const uint32_t tiles = uint32_t(((r.x + 31) >> 5) * ((r.y + 31) >> 5));
    uint32_t k = (tiles << 1) | uint32_t(c->engine.lite_active() ? 1u : 0u);
    k ^= uint32_t((c->logged_tl + 1) & 3) << 18;
    k ^= uint32_t((c->logged_opt + 1) & 15) << 20;
    k ^= uint32_t((c->logged_more + 1) & 255) << 24;
    return k;
}

// Filters out wall-clock timestamp inflation caused by concurrent game async-compute queues preempting between
// the start and end timestamp queries of a frame at the same grid resolution and mode.
void record_gpu_sample(Ctx* c, float raw_ms, float part[3], bool has_parts) {
    if (raw_ms <= 0) return;
    float clean_ms = raw_ms;
    const uint32_t key = timing_key(c);
    if (key && raw_ms >= 1.0f) {
        if (c->gpu_timing_key != key || c->gpu_base_ms <= 0.0f) {
            c->gpu_timing_key = key;
            c->gpu_base_ms = raw_ms;
            c->gpu_clean_ms = raw_ms;
        } else if (raw_ms < c->gpu_base_ms) {
            c->gpu_base_ms = raw_ms < c->gpu_base_ms * 0.94f ? raw_ms : (c->gpu_base_ms * 0.2f + raw_ms * 0.8f);
            c->gpu_clean_ms = raw_ms < c->gpu_clean_ms * 0.94f ? raw_ms : (c->gpu_clean_ms * 0.7f + raw_ms * 0.3f);
        } else if (raw_ms <= c->gpu_base_ms * 1.06f) {
            c->gpu_base_ms = c->gpu_base_ms * 0.99f + raw_ms * 0.01f;
            c->gpu_clean_ms = c->gpu_clean_ms * 0.85f + raw_ms * 0.15f;
        } else {
            c->gpu_base_ms += 0.002f;
            clean_ms = c->gpu_clean_ms;
        }
    }
    if (has_parts) {
        const float excess = raw_ms - clean_ms;
        if (excess > 0.0f && part[1] > excess) part[1] -= excess;
        for (int i = 0; i < 3; ++i) c->split[i] += part[i];
        ++c->split_n;
    }
    c->gpu_last = clean_ms;
    c->gpu_hist.push_back(clean_ms);
    if (c->gpu_hist.size() > 64) c->gpu_hist.erase(c->gpu_hist.begin());
}

// waits for a frame slot's submits (network and post, the host's hop between them not counted) and keeps its time
void retire_slot(Ctx* c, int s) {
    float ms = 0, part[3] = {};
    for (int k : {s, s + kSlots}) {
        c->span[k][0] = c->span[k][1] = 0;
        if (!c->inflight[k]) continue;
        VkResult r = vkWaitForFences(c->dev.vk(), 1, &c->done[k], VK_TRUE, 5ull * 1000 * 1000 * 1000);
        if (r != VK_SUCCESS) throw std::runtime_error("the network's earlier frame did not finish (" + std::to_string(int(r)) + ")");
        vkResetFences(c->dev.vk(), 1, &c->done[k]);
        c->inflight[k] = false;
        ms += harvest(c, k, part);
    }
    keep_hop_timing(c, s);
    record_gpu_sample(c, ms, part, ms > 0);
}

// the game queue signals, the Vulkan submit k runs behind that and signals back, the game queue waits for it
void submit(Ctx* c, int k, ID3D12CommandQueue* q) {
    const uint64_t v = ++c->value;
    HRESULT hr = q->Signal(c->toVk.fence, v);
    if (FAILED(hr)) throw std::runtime_error("game queue Signal failed");
    VkTimelineSemaphoreSubmitInfo tl{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tl.waitSemaphoreValueCount = 1;
    tl.pWaitSemaphoreValues = &v;
    tl.signalSemaphoreValueCount = 1;
    tl.pSignalSemaphoreValues = &v;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &tl};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &c->toVk.sem;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c->cmd[k];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &c->fromVk.sem;
    VkResult r = vkQueueSubmit(c->dev.queue(), 1, &si, c->done[k]);
    if (r != VK_SUCCESS) throw std::runtime_error("vkQueueSubmit failed (" + std::to_string(int(r)) + ")");
    c->inflight[k] = true;
    hr = q->Wait(c->fromVk.fence, v);
    if (FAILED(hr)) throw std::runtime_error("game queue Wait failed");
}

template <class F>
void record_submit(Ctx* c, int k, bool timed, F&& record) {
    VkCommandBuffer cb = c->cmd[k];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    c->dev.bind(cb, c->qp[k]);
    c->dev.stamp();
    try {
        record();
    } catch (...) {
        c->dev.unbind();
        vkEndCommandBuffer(cb);
        throw;
    }
    c->dev.stamp();
    c->nq[k] = c->dev.queries();
    c->timed[k] = timed;
    c->dev.barrier();
    c->dev.unbind();
    vkEndCommandBuffer(cb);
}

void log_vram(Ctx* c);

// the network for a frame on device d: weights and kernels built in the background, the engine set up here (it
// uploads) and sized; idle() makes sure nothing of ours still runs before buffers change. Whether the network runs.
template <class F>
bool prepare_network(Ctx* c, Gpu& d, I2 render, F&& idle) {
    if (c->state != ONR_RR_STATE_FAILED && c->built && !c->engine_ready) {
        c->engine.init(d, c->net, c->w);
        if (getenv("ONR_RR_NO_SAMPLER")) c->engine.set_history_sampler(false);   // measurements
        c->engine_ready = true;
        c->gpu_hist.clear();
        c->gpu_timing_key = 0;
        c->gpu_base_ms = 0;
        c->gpu_clean_ms = 0;
        c->state = ONR_RR_STATE_READY;
        c->set_message("ready");
    } else if (c->state != ONR_RR_STATE_FAILED && !c->building && !c->built && !c->small_view) {
        start_build(c);
    }
    const bool network = c->engine_ready && c->state == ONR_RR_STATE_READY && c->engine.kernels() != nullptr;
    if (network && c->lite_want && !c->lite_started.exchange(true)) c->lite_builder = std::thread(build_lite, c);
    if (network && c->lite_built) {
        c->engine.set_lite(&c->net_lite);
        c->engine.set_lite_on(c->lite_want);
        if (c->lite_logged != c->lite_want) {
            c->lite_logged = c->lite_want;
            g_log("fast mode %s at frame %llu", c->lite_logged ? "on" : "off", (unsigned long long)c->frames);
        }
    }
    if (network && (c->engine.render().x != render.x || c->engine.render().y != render.y || c->engine.output().x != c->out_size.x ||
                    c->engine.output().y != c->out_size.y)) {
        idle();
        c->engine.resize(render, c->out_size, {std::max(render.x, c->max_render.x), std::max(render.y, c->max_render.y)});
        if (!(c->use_d3d && c->direct_ok && c->engine.output_image_ok()) && !c->out16.addr)
            c->out16 = make_buffer(d, size_t(c->out_size.x) * c->out_size.y * 8);
        g_log("network sized: render %dx%d (grid %dx%d), output %dx%d, internal %dx%d", render.x, render.y, (render.x + 31) & ~31,
              (render.y + 31) & ~31, c->out_size.x, c->out_size.y, c->out_size.x * 3 / 2, c->out_size.y * 3 / 2);
        if (c->vk_host || c->use_d3d) log_vram(c);
    }
    ensure_guides(c, d, render);
    return network;
}

// the runtime's buffers on the game's device and the process's VRAM against its budget
void log_vram(Ctx* c) {
    uint64_t used = 0, budget = 0;
    const double mb = 1.0 / (1 << 20);
    const double self_bytes = double(c->use_d3d && c->dx ? c->dx->bytes() : c->dev.bytes());
    const bool has_vram = c->use_d3d && c->dx ? c->dx->vram(used, budget) : c->dev.vram(used, budget);
    if (has_vram)
        g_log("VRAM: this feature %.0f MB (%d features alive); the game's process %.0f MB of a %.0f MB budget%s", self_bytes * mb,
              g_live.load(), double(used) * mb, double(budget) * mb, used > budget ? " (over it: the driver pages, frames slow down)" : "");
    else
        g_log("VRAM: this feature %.0f MB (%d features alive)", self_bytes * mb, g_live.load());
}

void log_frame(Ctx* c, I2 render) {
    ++c->frames;
    if (c->frames != 1 && c->frames % 600 != 0) return;
    float avg = 0;
    for (float x : c->gpu_hist) avg += x;
    avg = c->gpu_hist.empty() ? 0 : avg / float(c->gpu_hist.size());
    g_log("frame %llu: render %dx%d -> %dx%d, %s, %s, GPU %.2f ms (average of %zu: %.2f ms; mean since the last line: ingest %.3f, network %.2f, "
          "pack %.3f)",
          (unsigned long long)c->frames, render.x, render.y, c->out_size.x, c->out_size.y, c->engine.lite_active() ? "fast mode on" : "fast mode off",
          c->vk_host ? "Vulkan (the game's device)" : c->use_d3d ? "D3D12" : "Vulkan", c->gpu_last, c->gpu_hist.size(), avg, c->split[0] / std::max(c->split_n, 1u),
          c->split[1] / std::max(c->split_n, 1u), c->split[2] / std::max(c->split_n, 1u));
    c->split[0] = c->split[1] = c->split[2] = 0, c->split_n = 0;
    if (c->gaps_n) {
        std::string g;
        char b[24];
        for (double x : c->gaps) snprintf(b, sizeof b, " %.4f", x / c->gaps_n), g += b;
        g_log("timestamp intervals, mean of %u (ms):%s", c->gaps_n, g.c_str());
        if (!c->stamp_names.empty()) {
            g.clear();
            for (auto& n : c->stamp_names) g += " " + n;
            g_log("stamped dispatches:%s", g.c_str());
        }
        c->gaps.assign(c->gaps.size(), 0.0), c->gaps_n = 0;
    }
}

// a prepared job (ABI 2): the network stops at the denoised image and exports it; exported says whether it did
int32_t enqueue(Ctx* c, Job& j, ID3D12CommandQueue* q, uint32_t* exported = nullptr) {
    I2 render{int32_t(j.f.render_width), int32_t(j.f.render_height)};
    const int s = int(j.seq % kSlots);
    retire_slot(c, s);
    j.input->import(c->dev, c->fn, c->d3d);
    j.output->import(c->dev, c->fn, c->d3d);
    // nothing of ours may run while the buffers change
    const bool network = prepare_network(c, c->dev, render, [&] {
        for (int k = 0; k < kSlots; ++k) retire_slot(c, k);
    });

    const bool split = network && exported && j.edit != ONR_RR_EDIT_OFF && j.image;
    if (split) {
        j.image->import(c->dev, c->fn, c->d3d);
        j.post_pending = true;
        *exported = 1;
    }
    // the display's own history lives only while display is asked for (a frame the host could not edit keeps it)
    if (network && c->engine.display_history() && !(split && j.edit == ONR_RR_EDIT_DISPLAY)) {
        for (int k = 0; k < kSlots; ++k) retire_slot(c, k);
        c->engine.drop_display_history();
    }
    if (network && (split ? j.edit : uint32_t(ONR_RR_EDIT_OFF)) != c->logged_edit) {
        c->logged_edit = split ? j.edit : uint32_t(ONR_RR_EDIT_OFF);
        g_log("denoised image hop: %s", c->logged_edit == ONR_RR_EDIT_LOOP ? "loop" : c->logged_edit == ONR_RR_EDIT_DISPLAY ? "display" : "off");
    }

    Rec r{c->dev, c->kern, j.input->addr, j.output->addr, j.image ? j.image->addr : 0, c->qnet[s]};
    record_submit(c, s, network, [&] { record_frame(c, r, j, network, split); });
    // validation only: whole frames numbered like hops, so GetHopTiming covers the path without the hop too
    static const bool time_all = getenv("ONR_RR_TIME_ALL") != nullptr;
    if (time_all && network && !split) c->hop_of[s] = ++c->hops;
    submit(c, s, q);
    if (network) log_frame(c, render);
    return ONR_RR_OK;
}

void teardown_ctx_resources(Ctx* c) {
    if (c->builder.joinable()) c->builder.join();
    if (c->lite_builder.joinable()) c->lite_builder.join();
    if (c->stats_thread.joinable()) c->stats_thread.join();
    if (c->dev.vk() && !c->vk_host) vkDeviceWaitIdle(c->dev.vk());
    c->engine.destroy();
    if (c->dx || c->dev.vk()) {
        Gpu& net = c->use_d3d ? static_cast<Gpu&>(*c->dx) : c->dev;
        c->net_lite.unload(net);
        c->net.unload(net);
        if (c->out16.addr) net.destroy(c->out16);
    }
    if (c->dev.vk()) c->kern.unload(c->dev);
    for (auto& b : c->guides)
        if (b.addr && c->guides_dev) c->guides_dev->destroy(b);
    if (c->dx) {
        c->dkern.unload(*c->dx);
        for (Buffer* b : {&c->dinput, &c->doutput, &c->dimage})
            if (b->addr) c->dx->destroy(*b);
        for (int k = 0; k < 2; ++k)
            if (c->stats[k].rb) {
                c->dx->destroy_raw(c->stats[k].rb);
                c->stats[k].rb = nullptr;
            }
    }
    if (c->dev.vk()) {
        for (Buffer* b : {&c->vinput, &c->voutput})
            if (b->addr) c->dev.destroy(*b);
    }
}

void free_ctx(Ctx* c) {
    teardown_ctx_resources(c);
    c->dx.reset();
    VkDevice v = c->dev.vk();
    for (VkQueryPool q : c->hqp)
        if (q) vkDestroyQueryPool(v, q, nullptr);
    for (int s = 0; s < kSubmits; ++s) {
        if (c->done[s]) vkDestroyFence(v, c->done[s], nullptr);
        if (c->qp[s]) vkDestroyQueryPool(v, c->qp[s], nullptr);
    }
    if (c->pool) vkDestroyCommandPool(v, c->pool, nullptr);
    if (c->input) c->input->release(v);
    if (c->output) c->output->release(v);
    if (c->image) c->image->release(v);
    for (auto& b : c->retired) b->release(v);
    c->toVk.release(v);
    c->fromVk.release(v);
    for (int k = 0; k < 2; ++k)
        if (c->stats[k].rb) {
            c->stats[k].rb->Release();
            c->stats[k].rb = nullptr;
        }
    if (c->d3d) c->d3d->Release();
    delete c;
}

bool ctx_retired(Ctx* c) {
    if (c->stats_busy && c->stats_busy->load()) return false;
    if (c->use_d3d && c->dx && !c->dx->all_retired()) return false;
    if (c->vk_host && c->dev.vk()) {
        if (!c->dev.all_retired()) return false;
        for (size_t s = 0; s < Ctx::kHostSlots; ++s) {
            if (!c->hq[s] || !c->hqp[s]) continue;
            uint64_t ts = 0;
            if (vkGetQueryPoolResults(c->dev.vk(), c->hqp[s], c->hq[s] - 1, 1, sizeof(ts), &ts, sizeof(ts), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
                return false;
            c->hq[s] = 0;
        }
    }
    return true;
}

struct RetiredCtx {
    Ctx* c = nullptr;
    ULONGLONG tick = 0;
    uint32_t sweeps = 0;
};
std::mutex g_retired_m;
std::vector<RetiredCtx> g_retired;

void sweep_retired(VkDevice = VK_NULL_HANDLE) {
    std::vector<Ctx*> go;
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard l(g_retired_m);
        size_t keep = 0;
        for (auto& r : g_retired)
            if (ctx_retired(r.c) || ++r.sweeps >= 64 || now - r.tick >= 2000) go.push_back(r.c);
            else g_retired[keep++] = r;
        g_retired.resize(keep);
    }
    for (Ctx* c : go) free_ctx(c);
}

void destroy_ctx(Ctx* c) {
    --g_live;
    teardown_ctx_resources(c);
    sweep_retired();
    if (!ctx_retired(c)) {
        std::lock_guard l(g_retired_m);
        g_retired.push_back({c, GetTickCount64(), 0});
        return;
    }
    free_ctx(c);
}

std::string gpu_reason(const Device& d) {
    const auto& i = d.info();
    if (!i.coopmat) return i.name + ": no cooperative matrices (needs an RDNA4 GPU)";
    if (!i.fp8_coopmat) return i.name + ": not an RDNA4 GPU (RX 7000 and RX 6000 support comes later)";
    return {};
}

// ------------------------------------------------------------------------------------------------ API

void d3d_setup(Ctx* c);

int32_t Check(const void* luid, const wchar_t* dlssd) {
    try {
        Device d;
        std::string err;
        DeviceOptions o;
        o.luid = static_cast<const uint8_t*>(luid);
        o.interop = true;
        if (!d.init(err, o)) return fail(ONR_RR_UNSUPPORTED, "Vulkan: " + err);
        std::string why = gpu_reason(d);
        if (!why.empty()) return fail(ONR_RR_UNSUPPORTED, why);
        if (dlssd) {
            Weights w;
            if (!build_weights(dlssd, w, err)) return fail(ONR_RR_UNSUPPORTED, "nvngx_dlssd.dll: " + err);
            std::lock_guard wl(g_cached_weights_m);
            g_cached_dlssd_path = dlssd;
            g_cached_weights = std::move(w);
        }
        return ONR_RR_OK;
    } catch (const std::exception& e) {
        return fail(ONR_RR_FAILED, e.what());
    }
}

// what PrepareVkDevice / FinishVkDevice did at the game's vkCreateDevice, for the log Create opens
std::mutex g_vk_setup_m;
std::string g_vk_setup;

void vk_selftest(Ctx* c);

// Vulkan host backend: the game's device adopted, ingest and pack on it (and the self-test), the network built in the
// background
void create_vk_host(Ctx* c, const OnrRrVkDevice& v) {
    if (v.api_version < VK_API_VERSION_1_3) throw std::runtime_error("the game's Vulkan instance is older than 1.3");
    {
        std::lock_guard l(g_vk_setup_m);
        g_log("Vulkan device setup at the game's vkCreateDevice: %s", g_vk_setup.empty() ? "not seen (the device may lack what the kernels need)" : g_vk_setup.c_str());
    }
    HostDevice h{static_cast<VkInstance>(v.instance), static_cast<VkPhysicalDevice>(v.physical_device), static_cast<VkDevice>(v.device),
                 v.queue_family, v.queue_index};
    std::string err;
    if (!c->dev.adopt(h, err)) throw std::runtime_error("Vulkan host device: " + err);
    c->vk_host = true;
    c->dev.set_stamps(g_stamps);
    g_log("Vulkan host device: %s, driver %s; the runtime's queue: family %u, index %u", c->dev.info().name.c_str(),
          c->dev.info().driver_info.c_str(), v.queue_family, v.queue_index);
    if (std::string why = gpu_reason(c->dev); !why.empty()) throw std::runtime_error(why);
    c->kern.load(c->dev, utf8(c->shader_dir), {"ingest", "pack", "selftest"});
    vk_selftest(c);
    // the guides read through the game's views and the output written through its view (d3d_ kernels, set 0 = views)
    try {
        // off unless asked for: reading the game's views hung the GPU in Indiana Jones after its features were made again
        // (ColorBeforeTransparency, reflection MVs, DoF guide); the copies follow NGX's layouts (guides
        // SHADER_READ_ONLY, output GENERAL) and cost ~0.2 ms
        if (const char* e = getenv("ONR_RR_DIRECT"); !e || strcmp(e, "1")) throw std::runtime_error("the default; ONR_RR_DIRECT=1 reads them through the game's views");
        c->kern.load(c->dev, utf8(c->shader_dir), {"d3d_ingest", "d3d_pack"});
        c->direct_ok = true;
        g_log("Vulkan host backend: guides read through the game's views where their formats allow, output written in place when rgba16f");
    } catch (const std::exception& e) {
        g_log("Vulkan host backend: guides and output copied (%s)", e.what());
    }
    for (uint32_t s = 0; s < 8; ++s) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kMaxQueries;
        if (vkCreateQueryPool(c->dev.vk(), &qi, nullptr, &c->hqp[s]) != VK_SUCCESS) throw std::runtime_error("vkCreateQueryPool");
    }
    g_log("backend: Vulkan host (the game's device), kernels recorded into the game's command buffer");
    c->state = ONR_RR_STATE_BUILDING;
    // A feature for a small secondary view (Indiana Jones makes one at 640x360 or less beside the main one): its
    // network beside the main one on another queue is where the GPU timeouts came from, so it shows the scaled colour.
    // ONR_RR_SMALL_VIEWS=1 runs the network there too.
    if (c->out_size.x < 960 && !getenv("ONR_RR_SMALL_VIEWS")) {
        c->small_view = true;
        c->state = ONR_RR_STATE_READY;
        c->message = "small secondary view: colour scaled without the network";
        g_log("output %dx%d: a small secondary view, the colour is scaled without the network (ONR_RR_SMALL_VIEWS=1 runs it)", c->out_size.x,
              c->out_size.y);
        return;
    }
    c->message = "building the network";
    start_build(c);
}

int32_t Create(const OnrRrCreateInfo* in, void** out) {
    const OnrRrVkDevice* vkd = in && in->size >= sizeof(OnrRrCreateInfo) ? in->vk_device : nullptr;
    if (!in || !out || in->size < ONR_RR_CREATE_INFO_SIZE_V1 || (!in->d3d12_device && !vkd) || !in->output_width || !in->output_height ||
        (vkd && (vkd->size < sizeof(OnrRrVkDevice) || !vkd->device || !vkd->physical_device)))
        return fail(ONR_RR_INVALID_ARGUMENT, "Create: missing arguments");
    auto* c = new Ctx;
    try {
        std::wstring dir = module_dir();
        g_log.open(in->log_path ? std::wstring(in->log_path) : dir + L"\\opennr_rr.log");
        if (!vkd) {
            c->d3d = static_cast<ID3D12Device*>(in->d3d12_device);
            c->d3d->AddRef();
        }
        std::wstring data = in->data_dir ? std::wstring(in->data_dir) : dir + L"\\opennr-rr";
        c->shader_dir = data + L"\\shaders";
        c->dlssd_path = in->dlssd_path ? in->dlssd_path : L"";
        c->weights_source = utf8(c->dlssd_path);
        c->out_size = {int32_t(in->output_width), int32_t(in->output_height)};
        c->max_render = {int32_t(in->max_render_width), int32_t(in->max_render_height)};
        g_log("OpenRR " ONR_RR_RUNTIME_VERSION " runtime: create, output %dx%d, render up to %dx%d, shaders %s, weights from %s", c->out_size.x,
              c->out_size.y, c->max_render.x, c->max_render.y, utf8(c->shader_dir).c_str(), c->weights_source.c_str());
        if (in->mode == ONR_RR_MODE_FAITHFUL) g_log("faithful mode asked for: this runtime has only the _f kernels, those it is");
        if (g_guide_stats) g_log("guide statistics on (ONR_RR_GUIDE_STATS): the disocclusion mask and the SSS guide are read back every 3 s");
        if (vkd) {
            create_vk_host(c, *vkd);
            *out = c;
            return ONR_RR_OK;
        }
        LUID luid = c->d3d->GetAdapterLuid();
        DeviceOptions o;
        o.luid = reinterpret_cast<const uint8_t*>(&luid);
        o.interop = true;
        o.descriptors = true;
        std::string err;
        if (!c->dev.init(err, o)) throw std::runtime_error("Vulkan: " + err);
        // timestamps only at the submit's boundaries: one around every dispatch serialises the ones without a barrier
        c->dev.set_stamps(g_stamps);
        g_log("Vulkan device: %s, driver %s", c->dev.info().name.c_str(), c->dev.info().driver_info.c_str());
        std::string why = gpu_reason(c->dev);
        if (!why.empty()) {
            destroy_ctx(c);
            return fail(ONR_RR_UNSUPPORTED, why);
        }
        c->fn.memProps = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(vkGetDeviceProcAddr(c->dev.vk(), "vkGetMemoryWin32HandlePropertiesKHR"));
        c->fn.importSem = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(c->dev.vk(), "vkImportSemaphoreWin32HandleKHR"));
        if (!c->fn.memProps || !c->fn.importSem) throw std::runtime_error("Win32 interop entry points missing");
        c->kern.load(c->dev, utf8(c->shader_dir), {"ingest", "pack"});
        // D3D12 backend: only for a host that knows Record (ABI 4 sets the choice), and only after its self-test
        uint32_t choice = in->size > ONR_RR_CREATE_INFO_SIZE_V1 ? in->backend : uint32_t(ONR_RR_BACKEND_CHOICE_VULKAN);
        if (const char* e = getenv("ONR_RR_BACKEND"))   // validation override
            choice = !_stricmp(e, "vulkan") ? ONR_RR_BACKEND_CHOICE_VULKAN : !_stricmp(e, "d3d12") ? ONR_RR_BACKEND_CHOICE_D3D12 : choice;
        if (choice != ONR_RR_BACKEND_CHOICE_VULKAN) {
            wchar_t local[MAX_PATH] = {};
            c->cache_dir = in->cache_dir ? std::wstring(in->cache_dir)
                           : GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) ? std::wstring(local) + L"\\OpenNR\\rr"
                                                                                     : std::wstring();
            try {
                d3d_setup(c);
                c->use_d3d = true;
                g_log("backend: D3D12 (%s), kernels recorded into the game's command list; ELF cache %s", choice == ONR_RR_BACKEND_CHOICE_D3D12 ? "asked for" : "auto",
                      utf8(c->cache_dir).c_str());
            } catch (const std::exception& e) {
                c->d3d_message = e.what();
                g_log("backend: Vulkan, the D3D12 path is not usable here: %s", e.what());
            }
        } else {
            g_log("backend: Vulkan (%s)", in->size > ONR_RR_CREATE_INFO_SIZE_V1 ? "asked for" : "host without the backend choice");
        }
        c->toVk.create(c->d3d, c->dev, c->fn);
        c->fromVk.create(c->d3d, c->dev, c->fn);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = c->dev.family();
        if (vkCreateCommandPool(c->dev.vk(), &pci, nullptr, &c->pool) != VK_SUCCESS) throw std::runtime_error("vkCreateCommandPool");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = c->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = kSubmits;
        if (vkAllocateCommandBuffers(c->dev.vk(), &cai, c->cmd) != VK_SUCCESS) throw std::runtime_error("vkAllocateCommandBuffers");
        for (int s = 0; s < kSubmits; ++s) {
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vkCreateFence(c->dev.vk(), &fci, nullptr, &c->done[s]);
            VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            q.queryType = VK_QUERY_TYPE_TIMESTAMP;
            q.queryCount = kMaxQueries;
            vkCreateQueryPool(c->dev.vk(), &q, nullptr, &c->qp[s]);
        }
        c->state = ONR_RR_STATE_BUILDING;
        c->message = "building the network";
        start_build(c);
        *out = c;
        return ONR_RR_OK;
    } catch (const std::exception& e) {
        g_log("create failed: %s", e.what());
        destroy_ctx(c);
        return fail(ONR_RR_FAILED, e.what());
    }
}

int32_t Destroy(void* ctx) {
    if (!ctx) return ONR_RR_OK;
    auto* c = static_cast<Ctx*>(ctx);
    g_log("destroy after %llu network frames", (unsigned long long)c->frames);
    destroy_ctx(c);
    return ONR_RR_OK;
}

int32_t SetMode(void* ctx, uint32_t mode) {
    if (!ctx) return fail(ONR_RR_INVALID_ARGUMENT, "SetMode: no context");
    // only the fast mode is built; a host that still asks for faithful keeps fast
    static std::atomic<bool> logged{false};
    if (mode == ONR_RR_MODE_FAITHFUL && !logged.exchange(true))
        g_log("faithful mode asked for: this runtime has only the _f kernels, those it is");
    return ONR_RR_OK;
}

// ------------------------------------------------------------------------------------------------ guide stats

// bytes per texel of the formats a mask or a guide comes in (their first channel is read), 0 for others
UINT stats_texel_bytes(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: return 1;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: return 2;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: return 4;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT: return 8;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 0;
    }
}

float stats_texel(const uint8_t* p, DXGI_FORMAT f) {
    uint16_t h;
    float v;
    switch (f) {
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
        return float(*p) / 255.0f;
    case DXGI_FORMAT_R16_UNORM: memcpy(&h, p, 2); return float(h) / 65535.0f;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        memcpy(&v, p, 4);
        return v;
    default: {   // half
        memcpy(&h, p, 2);
        const uint32_t e = (h >> 10) & 31, m = h & 1023;
        v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
        return h >> 15 ? -v : v;
    }
    }
}

// What the game puts in the disocclusion mask and the SSS guide (diagnostics, ONR_RR_GUIDE_STATS=1): every 3 s both
// are copied on the frame's list into readback buffers of ours and summed up in the log 8 frames later (the copies
// have run by then), on a thread of its own so the game's thread does not wait for the sums.
void stats_sum(GuideStats s, int k, uint64_t seq) {
    const char* name[2] = {"disocclusion mask", "SSS guide"};
    const UINT tb = stats_texel_bytes(s.format);
    if (!tb) {
        g_log("frame %llu: %s in DXGI format %d: no statistics for it", (unsigned long long)seq, name[k], int(s.format));
        return;
    }
    uint8_t* p = nullptr;
    D3D12_RANGE rr{0, SIZE_T(s.bytes)}, none{0, 0};
    if (FAILED(s.rb->Map(0, &rr, reinterpret_cast<void**>(&p)))) return;
    float mn = INFINITY, mx = -INFINITY;
    double sum = 0;
    uint64_t n = 0, half = 0, nonzero = 0, neg = 0, bad = 0;
    uint64_t hist[5] = {};   // positive values in (0,1], (1,10], (10,100], (100,1000], > 1000
    const uint32_t x1 = std::min(s.tw, s.x + s.w), y1 = std::min(s.th, s.y + s.h);
    for (uint32_t y = s.y; y < y1; ++y)
        for (uint32_t x = s.x; x < x1; ++x) {
            const float v = stats_texel(p + s.fp.Offset + UINT64(y) * s.fp.Footprint.RowPitch + UINT64(x) * tb, s.format);
            if (!std::isfinite(v)) {
                ++bad;
                continue;
            }
            mn = std::min(mn, v), mx = std::max(mx, v), sum += v, ++n;
            half += v > 0.5f, nonzero += v != 0.0f, neg += v < 0.0f;
            if (v > 0.0f) ++hist[v <= 1.0f ? 0 : v <= 10.0f ? 1 : v <= 100.0f ? 2 : v <= 1000.0f ? 3 : 4];
        }
    s.rb->Unmap(0, &none);
    const double pc = n ? 100.0 / double(n) : 0.0;
    char hs[160] = "";
    if (k == 0)
        snprintf(hs, sizeof hs, "; positive in (0,1] %llu, (1,10] %llu, (10,100] %llu, (100,1000] %llu, > 1000 %llu",
                 (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
                 (unsigned long long)hist[3], (unsigned long long)hist[4]);
    g_log("frame %llu: %s (%ux%u at %u,%u of a %ux%u texture, DXGI format %d): min %g, max %g, mean %g, > 0.5 in %.2f%%, "
          "non-zero in %.2f%%, negative in %.2f%%, NaN/inf %llu%s",
          (unsigned long long)seq, name[k], x1 > s.x ? x1 - s.x : 0, y1 > s.y ? y1 - s.y : 0, s.x, s.y, s.tw, s.th,
          int(s.format), n ? mn : 0.0f, n ? mx : 0.0f, n ? sum / double(n) : 0.0, double(half) * pc, double(nonzero) * pc,
          double(neg) * pc, (unsigned long long)bad, hs);
}

void stats_read(Ctx* c, uint64_t seq) {
    if (!(c->stats[0].pending || c->stats[1].pending) || seq < c->stats_seq + 8) return;
    GuideStats g[2] = {c->stats[0], c->stats[1]};
    for (int k = 0; k < 2; ++k)
        if (g[k].pending) g[k].rb->AddRef();
    c->stats[0].pending = c->stats[1].pending = false;
    if (c->stats_thread.joinable()) c->stats_thread.join();
    c->stats_busy->store(true);
    c->stats_thread = std::thread([g0 = g[0], g1 = g[1], seq = c->stats_seq, busy = c->stats_busy] {
        const GuideStats g[2] = {g0, g1};
        for (int k = 0; k < 2; ++k)
            if (g[k].pending) stats_sum(g[k], k, seq), g[k].rb->Release();
        busy->store(false);
    });
}

void stats_record(Ctx* c, const Job& j, ID3D12GraphicsCommandList* cl) {
    const ULONGLONG now = GetTickCount64();
    if (!g_guide_stats || c->stats[0].pending || c->stats[1].pending || c->stats_busy->load() || now - c->stats_tick < 3000) return;
    const OnrRrTexture* tex[2] = {&j.f.disocclusion_mask, &j.f.sss_guide};
    const int slot[2] = {G_DM, G_SSS};
    for (int k = 0; k < 2; ++k) {
        if (!j.in[slot[k]].present) continue;
        auto* r = static_cast<ID3D12Resource*>(tex[k]->resource);
        D3D12_RESOURCE_DESC rd = r->GetDesc();
        GuideStats& s = c->stats[k];
        UINT64 total = 0;
        c->d3d->GetCopyableFootprints(&rd, 0, 1, 0, &s.fp, nullptr, nullptr, &total);
        if (!s.rb || s.bytes < total) {
            if (s.rb) s.rb->Release();   // idle: its last copy was read back
            s.rb = nullptr, s.bytes = 0;
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
            if (FAILED(c->d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&s.rb))))
                continue;
            s.bytes = total;
        }
        s.format = rd.Format, s.tw = uint32_t(rd.Width), s.th = rd.Height;
        s.x = tex[k]->x, s.y = tex[k]->y, s.w = j.f.render_width, s.h = j.f.render_height;
        const auto st = D3D12_RESOURCE_STATES(tex[k]->state);
        D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, st, D3D12_RESOURCE_STATE_COPY_SOURCE};
        if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{s.rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.PlacedFootprint = s.fp;
        D3D12_TEXTURE_COPY_LOCATION src{r, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        src.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) cl->ResourceBarrier(1, &b);
        s.pending = true;
        c->stats_seq = j.seq, c->stats_tick = now;
    }
}

// a frame's job: the guides laid out as placed footprints of one buffer from offset 0 (in_bytes in all) and the
// output's rows (out_bytes); ONR_RR_UNSUPPORTED for an output format that is not written
int32_t plan_job(Ctx* c, const OnrRrFrame* f, std::unique_ptr<Job>& j, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp, UINT64& off, UINT64& obytes) {
    j = std::make_unique<Job>();
    j->seq = ++c->seq;
    memcpy(&j->f, f, std::min<size_t>(f->size, sizeof(OnrRrFrame)));
    j->f.size = sizeof(OnrRrFrame);
    // a host from before the V4 tail gave the layer's opacity knowing it was not read: it stays unread
    if (f->size <= ONR_RR_FRAME_SIZE_V4) j->f.transparency_opacity = {};
    if (!j->f.transparency_layer.resource) j->f.transparency_opacity = {}, j->f.transparency_motion = {};
    if (j->f.flags & ONR_RR_FRAME_ROUGHNESS_PACKED) j->f.roughness = {};
    // rr2 has no input for the ray direction + hit distance (nvngx_dlssd.dll binds only DLSSD.SpecularHitDistance)
    if (j->f.specular_ray_direction_hit_distance.resource && !c->logged_ray) {
        c->logged_ray = true;
        g_log("specular ray direction + hit distance given: not read (rr2 takes only the specular hit distance)");
    }
    j->f.specular_ray_direction_hit_distance = {};
    if ((!j->f.render_width || !j->f.render_height) && c->vk_host) {
        auto* ci = static_cast<const OnrRrVkImage*>(f->color.resource);
        j->f.render_width = ci->width, j->f.render_height = ci->height;
    } else if (!j->f.render_width || !j->f.render_height) {
        D3D12_RESOURCE_DESC cd = static_cast<ID3D12Resource*>(f->color.resource)->GetDesc();
        j->f.render_width = uint32_t(cd.Width), j->f.render_height = cd.Height;
    }
    const OnrRrFrame* g = &j->f;
    const OnrRrTexture* tex[G_COUNT];
    frame_textures(*g, tex);
    off = 0;
    for (int i = 0; i < G_COUNT; ++i) {
        if (tex[i]->resource && c->vk_host) {
            // Vulkan: the mip level's rows at 256-byte pitch, as the D3D12 footprints
            auto* im = static_cast<const OnrRrVkImage*>(tex[i]->resource);
            const Tf tf = tf_of_vk(VkFormat(im->format));
            if (tf == TF_NONE || !im->image || !im->width || !im->height) {
                if (c->last_logged_error.find("vk" + std::to_string(im->format)) == std::string::npos) {
                    c->last_logged_error += " vk" + std::to_string(im->format);
                    g_log("guide %d has VkFormat %u (%ux%u), which is not read; it is left out", i, im->format, im->width, im->height);
                }
                continue;
            }
            const UINT pitch = (im->width * tf_bytes(tf) + 255) & ~255u;
            j->in[i] = Copy{true, tf, off, pitch, tex[i]->x, tex[i]->y};
            j->in[i].tw = im->width, j->in[i].th = im->height, j->in[i].fmt = im->format;
            off = (off + UINT64(pitch) * im->height + 511) & ~UINT64(511);
            continue;
        }
        auto* r = static_cast<ID3D12Resource*>(tex[i]->resource);
        if (!r) continue;
        D3D12_RESOURCE_DESC rd = r->GetDesc();
        Tf tf = tf_of(rd.Format);
        if (tf == TF_NONE) {
            if (c->last_logged_error.find(std::to_string(int(rd.Format))) == std::string::npos) {
                c->last_logged_error += " " + std::to_string(int(rd.Format));
                g_log("guide %d has DXGI format %d, which is not read; it is left out", i, int(rd.Format));
            }
            continue;
        }
        UINT64 total = 0;
        c->d3d->GetCopyableFootprints(&rd, 0, 1, off, &fp[i], nullptr, nullptr, &total);
        // a planar depth-stencil copies its depth plane alone: R32G8X24 gives R32 rows, not 8-byte texels
        if (Tf plane = tf_of(fp[i].Footprint.Format)) tf = plane;
        j->in[i] = Copy{true, tf, fp[i].Offset, fp[i].Footprint.RowPitch, tex[i]->x, tex[i]->y};
        j->in[i].tw = uint32_t(rd.Width), j->in[i].th = rd.Height, j->in[i].fmt = uint32_t(rd.Format);
        off = (fp[i].Offset + total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    }
    if (int(j->in[G_TL].present) != c->logged_tl) {
        c->logged_tl = int(j->in[G_TL].present);
        if (j->in[G_TL].present) {
            const Copy& tl = j->in[G_TL];
            g_log("frame %llu: transparency layer given (%ux%u, %s %u, subrect %u,%u): composited over the output",
                  (unsigned long long)j->seq, tl.tw, tl.th, c->vk_host ? "VkFormat" : "DXGI format", tl.fmt, g->transparency_layer.x,
                  g->transparency_layer.y);
        } else {
            g_log("frame %llu: no transparency layer", (unsigned long long)j->seq);
        }
    }
    const bool dm_invert = j->in[G_DM].present && (g->flags & ONR_RR_FRAME_DM_INVERT);
    const bool dm_clamp = j->in[G_DM].present && (g->flags & ONR_RR_FRAME_DM_CLAMP);
    const float dm_scale = j->in[G_DM].present && g->disocclusion_mask_scale > 0.0f ? g->disocclusion_mask_scale : 1.0f;
    if (const int opt = int(j->in[G_DM].present) | int(j->in[G_SSS].present) << 1 | int(dm_invert) << 2 | int(dm_clamp) << 3;
        opt != c->logged_opt || dm_scale != c->logged_dm_scale) {
        c->logged_opt = opt, c->logged_dm_scale = dm_scale;
        char sc[48] = "";
        if (dm_scale != 1.0f) snprintf(sc, sizeof sc, ", scaled by %g", dm_scale);
        g_log("frame %llu: disocclusion mask %s%s%s, SSS guide %s (both go into the first encoder)", (unsigned long long)j->seq,
              opt & 4 ? "given, read as 1 - mask (diagnostic)" : opt & 1 ? "given" : "absent", sc,
              opt & 8 ? ", clamped to 0..1" : "", opt & 2 ? "given" : "absent");
    }
    if (const int more = int(j->in[G_RESP].present) | int(j->in[G_CBT].present) << 1 | int(j->in[G_DOF].present) << 2 |
                         int(j->in[G_SMV].present) << 3 | int(j->in[G_TLO].present) << 4 | int(j->in[G_TLMV].present) << 5 |
                         int(!j->in[G_HIT].present) << 6;
        more != c->logged_more) {
        c->logged_more = more;
        const bool narrow = (more & 5) && c->engine_ready && c->engine.fast() && !c->engine.wide_attr_ok();
        g_log("frame %llu: responsivity mask %s, ColorBeforeTransparency %s, depth of field guide %s, reflection motion "
              "vectors %s (first encoder); transparency opacity %s, its motion vectors %s (downsample, with the layer); "
              "specular hit distance %s%s",
              (unsigned long long)j->seq, more & 1 ? "given" : "absent", more & 2 ? "given" : "absent", more & 4 ? "given" : "absent",
              more & 8 ? "given" : "absent", more & 16 ? "given" : "absent", more & 32 ? "given" : "absent",
              more & 64 ? "absent (the reflections take the pixel's motion vector)" : "given",
              narrow ? "; the 64-byte record kernels are missing, the mask and the depth of field guide are left out" : "");
    }
    if (c->vk_host) {
        auto* oi = static_cast<const OnrRrVkImage*>(f->output.resource);
        j->out_tf = tf_of_vk(VkFormat(oi->format));
        if (!tf_writable(j->out_tf) || !oi->image)
            return fail(ONR_RR_UNSUPPORTED, "output VkFormat " + std::to_string(oi->format) +
                                                " is not written (written: RGBA16F/32F, B10G11R11, A2B10G10R10, RGBA8/BGRA8 (sRGB too), RGBA16, E5B9G9R9)");
        if (oi->width < uint32_t(c->out_size.x) || oi->height < uint32_t(c->out_size.y))
            return fail(ONR_RR_INVALID_ARGUMENT, "the output image is smaller than the output size given at Create");
        j->out_pitch = (uint32_t(c->out_size.x) * tf_bytes(j->out_tf) + 255) & ~255u;
        j->out_bytes = obytes = UINT64(j->out_pitch) * uint32_t(c->out_size.y);
        return ONR_RR_OK;
    }
    auto* outr = static_cast<ID3D12Resource*>(f->output.resource);
    D3D12_RESOURCE_DESC od = outr->GetDesc();
    j->out_tf = tf_of(od.Format);
    if (!tf_writable(j->out_tf))
        return fail(ONR_RR_UNSUPPORTED, "output texture format " + std::to_string(int(od.Format)) +
                                            " is not written (written: RGBA16F/32F, R11G11B10F, RGB10A2, RGBA8/BGRA8 (sRGB too), RGBA16, R9G9B9E5)");
    D3D12_RESOURCE_DESC box = od;
    box.Width = UINT64(c->out_size.x), box.Height = UINT(c->out_size.y);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ofp{};
    c->d3d->GetCopyableFootprints(&box, 0, 1, 0, &ofp, nullptr, nullptr, &obytes);
    j->out_bytes = obytes;
    j->out_pitch = ofp.Footprint.RowPitch;
    return ONR_RR_OK;
}

int32_t RecordInputs(void* ctx, const OnrRrFrame* f, void* list, void** job) {
    if (!ctx || !f || !list || !job || f->size < ONR_RR_FRAME_SIZE_V1 || !f->color.resource || !f->output.resource)
        return fail(ONR_RR_INVALID_ARGUMENT, "RecordInputs: missing arguments");
    auto* c = static_cast<Ctx*>(ctx);
    auto* cl = static_cast<ID3D12GraphicsCommandList*>(list);
    if (c->use_d3d) return fail(ONR_RR_UNSUPPORTED, "RecordInputs: the D3D12 backend runs, use Record");
    if (c->vk_host) return fail(ONR_RR_UNSUPPORTED, "RecordInputs: the Vulkan host backend runs, use RecordVk");
    try {
        std::lock_guard l(c->m);
        std::unique_ptr<Job> j;
        UINT64 off = 0, obytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[G_COUNT]{};
        if (int32_t rc = plan_job(c, f, j, fp, off, obytes)) return rc;
        stats_read(c, j->seq);
        stats_record(c, *j, cl);
        const OnrRrFrame* g = &j->f;
        const OnrRrTexture* tex[G_COUNT];
        frame_textures(*g, tex);
        if (!c->input || c->input->size < off) {
            if (c->input) c->retired.push_back(c->input);
            c->input = std::make_shared<SharedBuffer>();
            c->input->create_d3d(c->d3d, off);
            g_log("input buffer: %llu MB", (unsigned long long)(c->input->size >> 20));
        }
        if (!c->output || c->output->size < obytes) {
            if (c->output) c->retired.push_back(c->output);
            c->output = std::make_shared<SharedBuffer>();
            c->output->create_d3d(c->d3d, obytes);
        }
        j->input = c->input, j->output = c->output;
        // all transitions in one barrier each way, so the copies overlap (a texture given twice is moved once)
        auto* in = c->input->res;
        D3D12_RESOURCE_BARRIER bar[G_COUNT + 1];
        UINT nb = 0;
        auto add = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
            if (a == b) return;
            for (UINT k = 0; k < nb; ++k)
                if (bar[k].Transition.pResource == r) return;
            bar[nb] = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
            bar[nb++].Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        };
        add(in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        for (int i = 0; i < G_COUNT; ++i)
            if (j->in[i].present) add(static_cast<ID3D12Resource*>(tex[i]->resource), D3D12_RESOURCE_STATES(tex[i]->state), D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (nb) cl->ResourceBarrier(nb, bar);
        for (int i = 0; i < G_COUNT; ++i) {
            if (!j->in[i].present) continue;
            D3D12_TEXTURE_COPY_LOCATION dst{in, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            dst.PlacedFootprint = fp[i];
            D3D12_TEXTURE_COPY_LOCATION src{static_cast<ID3D12Resource*>(tex[i]->resource), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            src.SubresourceIndex = 0;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        for (UINT k = 0; k < nb; ++k) std::swap(bar[k].Transition.StateBefore, bar[k].Transition.StateAfter);
        if (nb) cl->ResourceBarrier(nb, bar);
        Job* p = j.get();
        c->jobs[p] = std::move(j);
        // jobs the host neither enqueued nor cancelled
        while (c->jobs.size() > 64) {
            auto oldest = c->jobs.begin();
            for (auto it = c->jobs.begin(); it != c->jobs.end(); ++it)
                if (it->second->seq < oldest->second->seq) oldest = it;
            c->jobs.erase(oldest);
        }
        *job = p;
        return ONR_RR_OK;
    } catch (const std::exception& e) {
        g_log("RecordInputs: %s", e.what());
        return fail(ONR_RR_FAILED, e.what());
    }
}

// ------------------------------------------------------------------------------------------------ D3D12 backend

constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// selftest.comp: dst, k, mid[57], last (61 dwords, the most the root signature carries)
struct SelfTestPC {
    uint32_t dst[2];   // the address as two dwords: 244 bytes without tail padding
    uint32_t k, mid[57], last;
};
static_assert(sizeof(SelfTestPC) == kMaxPushBytes);

// Vulkan host backend, at Create on the runtime's queue of the game's device: selftest.comp in a direct and an
// indirect dispatch, every value checked (the device has what the kernels need, push constants and addresses reach
// them). Throws why not.
void vk_selftest(Ctx* c) {
    Device& d = c->dev;
    Buffer out[2] = {d.create(256 * 4), d.create(256 * 4)}, args = d.create(16);
    std::vector<uint32_t> got[2] = {std::vector<uint32_t>(256), std::vector<uint32_t>(256)};
    SelfTestPC p[2]{};
    const uint32_t groups[2] = {4, 3};
    auto drop = [&] {
        for (auto& b : out) d.destroy(b);
        d.destroy(args);
    };
    try {
        const uint32_t a[4] = {groups[1], 1, 1, 0};
        d.upload(args, a, 16);
        for (int t = 0; t < 2; ++t) {
            p[t].dst[0] = uint32_t(out[t].addr), p[t].dst[1] = uint32_t(out[t].addr >> 32), p[t].k = 3 + 7 * t, p[t].last = 0x5000000u + t;
            for (uint32_t i = 0; i < 57; ++i) p[t].mid[i] = 1000u * i + 11 * t;
        }
        d.begin();
        d.dispatch(c->kern["selftest"], &p[0], sizeof p[0], groups[0]);
        d.dispatch_indirect(c->kern["selftest"], &p[1], sizeof p[1], args);
        d.submit();
        for (int t = 0; t < 2; ++t) d.download(out[t], got[t].data(), 256 * 4);
    } catch (...) {
        drop();
        throw;
    }
    drop();
    for (int t = 0; t < 2; ++t)
        for (uint32_t i = 0; i < groups[t] * 64; ++i) {
            const uint32_t want = p[t].k * i + p[t].mid[i % 57] + p[t].last + groups[t] * 65536u;
            if (got[t][i] != want) {
                char b[160];
                snprintf(b, sizeof b, "self-test: %s dispatch, element %u is %08x, %08x expected", t ? "indirect" : "direct", i, got[t][i], want);
                throw std::runtime_error(b);
            }
        }
    g_log("Vulkan host backend: self-test passed on the game's device (push constants, addresses, workgroup count, indirect dispatch)");
}

// The kernels' push constants must fit the root constants of the D3D12 path.
template <size_t N> struct PushFits { static_assert(N <= kMaxPushBytes, "push constants past the root constants"); };
template struct PushFits<sizeof(IngestPC)>;
template struct PushFits<sizeof(PackPC)>;
template struct PushFits<sizeof(Enc0AttrPC)>;
template struct PushFits<sizeof(Enc0StagePC)>;
template struct PushFits<sizeof(EncStagePC)>;
template struct PushFits<sizeof(DecUpPC)>;
template struct PushFits<sizeof(DecDfnPC)>;
template struct PushFits<sizeof(PostDltPC)>;
template struct PushFits<sizeof(DownsampleTlPC)>;
template struct PushFits<sizeof(DfnImportPC)>;

// The texture paths of d3d_ingest and d3d_pack on known data, on the device's own queue: an rgba16f texture (finite
// halves, denormals included) and an rgba8 one (every byte value) read as the colour and the albedo, and the colour
// stored into an rgba16f UAV; each must come out with the bits the buffer paths give. Empty when they do.
std::string d3d_check_direct(Ctx* c, D3dDevice& dx) {
    constexpr UINT W = 16, H = 4;
    std::vector<uint16_t> half(W * H * 4);
    std::vector<uint8_t> bytes(W * H * 4);
    for (size_t i = 0; i < half.size(); ++i) {
        uint16_t h = uint16_t(i * 2654435761u >> 7);
        if ((h & 0x7c00) == 0x7c00) h &= 0xbfff;   // no inf / NaN
        half[i] = i < 8 ? uint16_t(i + 1) : h;      // a few denormals first
        bytes[i] = uint8_t(i);
    }
    ID3D12Device* dev = c->d3d;
    std::vector<ID3D12Resource*> own;
    auto tex = [&](DXGI_FORMAT f, bool uav) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, f, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                               uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE};
        ID3D12Resource* r = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r))))
            throw std::runtime_error("texture");
        own.push_back(r);
        return r;
    };
    auto buf = [&](UINT64 n, D3D12_HEAP_TYPE t) {
        D3D12_HEAP_PROPERTIES hp{t};
        D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, n, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
        ID3D12Resource* r = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                t == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&r))))
            throw std::runtime_error("buffer");
        own.push_back(r);
        return r;
    };
    Buffer out[2] = {dx.create(W * H * 8), dx.create(W * H * 4)};
    std::string why;
    try {
        ID3D12Resource *colour = tex(DXGI_FORMAT_R16G16B16A16_FLOAT, false), *albedo = tex(DXGI_FORMAT_R8G8B8A8_UNORM, false),
                       *target = tex(DXGI_FORMAT_R16G16B16A16_FLOAT, true), *up = buf(512 * H * 2, D3D12_HEAP_TYPE_UPLOAD),
                       *rb = buf(256 * H, D3D12_HEAP_TYPE_READBACK);
        uint8_t* m = nullptr;
        up->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (UINT y = 0; y < H; ++y) {
            memcpy(m + y * 256, half.data() + y * W * 4, W * 8);
            memcpy(m + 256 * H + y * 256, bytes.data() + y * W * 4, W * 4);
        }
        up->Unmap(0, nullptr);
        dx.begin_own();
        ID3D12GraphicsCommandList* l = dx.list();
        auto put = [&](ID3D12Resource* t, DXGI_FORMAT f, UINT64 off) {
            D3D12_TEXTURE_COPY_LOCATION to{t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, from{up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            from.PlacedFootprint = {off, {f, W, H, 1, 256}};
            l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        };
        put(colour, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
        put(albedo, DXGI_FORMAT_R8G8B8A8_UNORM, 256 * H);
        D3D12_RESOURCE_BARRIER b[3] = {};
        ID3D12Resource* rs[3] = {colour, albedo, target};
        for (int i = 0; i < 3; ++i) {
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[i].Transition = {rs[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                               i < 2 ? kRead : D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        }
        l->ResourceBarrier(3, b);
        dx.srv(colour, G_COLOR, DXGI_FORMAT_R16G16B16A16_FLOAT);
        dx.srv(albedo, G_ALBEDO, DXGI_FORMAT_R8G8B8A8_UNORM);
        dx.uav(target, 16, DXGI_FORMAT_R16G16B16A16_FLOAT);
        IngestPC ip{{}, 1.0f, 0, {}, {}, I2{int32_t(W), int32_t(H)}, {}};
        ip.dst[G_COLOR] = out[0].addr, ip.dst[G_ALBEDO] = out[1].addr;
        ip.g[G_COLOR][0] = G_COLOR, ip.g[G_COLOR][2] = TF_RGBA16F | 0u << 8 | 8u << 16;
        ip.g[G_ALBEDO][0] = G_ALBEDO, ip.g[G_ALBEDO][2] = TF_RGBA8 | 1u << 8 | 8u << 16;
        dx.dispatch(c->dkern["d3d_ingest"], &ip, sizeof ip, 1, 1);
        dx.barrier();
        PackPC pk{out[0].addr, 0, I2{int32_t(W), int32_t(H)}, I2{int32_t(W), int32_t(H)}, 0, TF_RGBA16F | 256u};
        dx.dispatch(c->dkern["d3d_pack"], &pk, sizeof pk, 1, 1);
        dx.barrier();
        b[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, b[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        l->ResourceBarrier(1, &b[2]);
        D3D12_TEXTURE_COPY_LOCATION to{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}, from{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        to.PlacedFootprint = {0, {DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 1, 256}};
        l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        dx.submit_own();
        std::vector<uint16_t> got_h(W * H * 4), got_t(W * H * 4);
        std::vector<uint8_t> got_b(W * H * 4);
        dx.download(out[0], got_h.data(), W * H * 8);
        dx.download(out[1], got_b.data(), W * H * 4);
        rb->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (UINT y = 0; y < H; ++y) memcpy(got_t.data() + y * W * 4, m + y * 256, W * 8);
        rb->Unmap(0, nullptr);
        if (got_h != half) why = "an rgba16f texel read through the view differs from its bits";
        else if (got_b != bytes) why = "an rgba8 texel read through the view does not pack back to its byte";
        else if (got_t != half) why = "an rgba16f store through the UAV changed the bits";
    } catch (const std::exception& e) {
        why = e.what();
    }
    for (auto& b : out) dx.destroy(b);
    for (auto* r : own) r->Release();   // the own queue is idle
    return why;
}

// The sampled-image table on known data: an rgba16f buffer seen as a linear image (alias_image / set_texture, slot
// 1), sampled at every texel centre by selftest_tex, must give its texels back. Empty when it does.
std::string d3d_check_sampler(Ctx* c, D3dDevice& dx) {
    constexpr uint32_t W = 64, H = 8;
    std::vector<uint16_t> half(W * H * 4), got(W * H * 4);
    for (size_t i = 0; i < half.size(); ++i) {
        uint16_t h = uint16_t(i * 2654435761u >> 9);
        half[i] = (h & 0x7c00) == 0x7c00 ? uint16_t(h & 0xbfff) : h;
    }
    Buffer src = dx.create(W * H * 8 + 65536), dst = dx.create(W * H * 8);
    Image im;
    std::string why;
    try {
        dx.upload(src, half.data(), W * H * 8);
        im = dx.alias_image(src, W, H, VK_FORMAT_R16G16B16A16_SFLOAT, 8);
        if (!im.img) throw std::runtime_error("no linear image");
        dx.set_texture(1, im, dx.sampler(true, true));
        struct { uint64_t dst; I2 size; uint32_t slot; } pc{dst.addr, I2{int32_t(W), int32_t(H)}, 1};
        dx.begin_own();
        dx.dispatch(c->dkern["selftest_tex"], &pc, sizeof pc, W * H / 64);
        dx.barrier();
        dx.submit_own();
        dx.download(dst, got.data(), W * H * 8);
        if (got != half) why = "a texel sampled through the table differs from the buffer";
    } catch (const std::exception& e) {
        why = e.what();
    }
    if (im.img) dx.destroy(im);
    dx.destroy(src), dx.destroy(dst);
    return why;
}

// float -> R9G9B9E5_SHAREDEXP in plain double arithmetic (rgb9e5_of in onr_texel.glsl does it in integers): clamp to
// [0, 65408], shared exponent from the largest, mantissas rounded toward zero
uint32_t rgb9e5_ref(const float c[3]) {
    double v[3], mx = 0;
    for (int i = 0; i < 3; ++i) v[i] = c[i] > 0 ? std::min(double(c[i]), 65408.0) : 0.0, mx = std::max(mx, v[i]);
    int k = 0;
    std::frexp(mx, &k);   // mx = f * 2^k, f in [0.5, 1): floor(log2(mx)) = k - 1
    const int e = std::max(-16, mx > 0 ? k - 1 : -16) + 16;
    uint32_t r = uint32_t(e) << 27;
    for (int i = 0; i < 3; ++i) r |= uint32_t(std::floor(v[i] / std::ldexp(1.0, e - 24))) << (9 * i);
    return r;
}

// ONR_RR_E5_PROBE (measurement): the R9G9B9E5 encode of the pack kernel's buffer path against rgb9e5_ref and against
// the GPU's own typed UAV store of that format, on every fp16 value in each channel (ties, exponent carries, clamps)
std::string d3d_probe_e5(Ctx* c, D3dDevice& dx) {
    constexpr UINT W = 256, H = 256, N = W * H;
    std::vector<uint16_t> half(N * 4);
    for (uint32_t i = 0; i < N; ++i)
        half[4 * i] = uint16_t(i), half[4 * i + 1] = uint16_t(i * 40503u + 7), half[4 * i + 2] = uint16_t(i * 7919u + 12345u), half[4 * i + 3] = 0;
    auto h2f = [](uint16_t h) {
        uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
        float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
        return s ? -v : v;
    };
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{DXGI_FORMAT_R9G9B9E5_SHAREDEXP};
    const bool uav = c->direct_ok && SUCCEEDED(c->d3d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) &&
                     (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    Buffer src = dx.create(N * 8), dst = dx.create(N * 4);
    ID3D12Resource *tex = nullptr, *rb = nullptr;
    std::vector<uint32_t> ours(N), hw(N);
    std::string out;
    try {
        dx.upload(src, half.data(), N * 8);
        if (uav) {
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT}, hr{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_R9G9B9E5_SHAREDEXP, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
            D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, N * 4, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
            if (FAILED(c->d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&tex))) ||
                FAILED(c->d3d->CreateCommittedResource(&hr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
                throw std::runtime_error("probe resources");
        }
        dx.begin_own();
        PackPC pk{src.addr, dst.addr, I2{int32_t(W), int32_t(H)}, I2{int32_t(W), int32_t(H)}, W * 4, TF_RGB9E5};
        dx.dispatch(c->dkern["pack"], &pk, sizeof pk, W / 16, H / 16);
        if (uav) {
            dx.uav(tex, 16, DXGI_FORMAT_R9G9B9E5_SHAREDEXP);
            pk.pitch = 0, pk.fmt = 256u;
            dx.dispatch(c->dkern["d3d_pack"], &pk, sizeof pk, W / 16, H / 16);
            dx.barrier();
            transition(dx.list(), tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION to{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}, from{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            to.PlacedFootprint = {0, {DXGI_FORMAT_R9G9B9E5_SHAREDEXP, W, H, 1, W * 4}};
            dx.list()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        dx.barrier();
        dx.submit_own();
        dx.download(dst, ours.data(), N * 4);
        if (uav) {
            void* m = nullptr;
            rb->Map(0, nullptr, &m);
            memcpy(hw.data(), m, N * 4);
            rb->Unmap(0, nullptr);
        }
        uint32_t bad_ref = 0, bad_hw = 0, bad_hw_finite = 0;
        std::string first_ref, first_hw;
        for (uint32_t i = 0; i < N; ++i) {
            const float f[3] = {h2f(half[4 * i]), h2f(half[4 * i + 1]), h2f(half[4 * i + 2])};
            char b[160];
            snprintf(b, sizeof b, "(%g, %g, %g): ours %08x, reference %08x, GPU store %08x", f[0], f[1], f[2], ours[i], rgb9e5_ref(f), hw[i]);
            if (ours[i] != rgb9e5_ref(f) && !bad_ref++) first_ref = b;
            if (uav && ours[i] != hw[i]) {
                const bool finite = std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]);
                bad_hw_finite += finite;
                if (!bad_hw++ || (finite && bad_hw_finite == 1)) first_hw = b;
            }
        }
        char b[640];
        snprintf(b, sizeof b, "R9G9B9E5 probe, %u texels: pack vs reference %u differ%s%s; %s", N, bad_ref, bad_ref ? ", first " : "",
                 first_ref.c_str(),
                 uav ? ("pack vs the GPU's typed UAV store " + std::to_string(bad_hw) + " differ (" + std::to_string(bad_hw_finite) +
                        " with finite inputs)" + (bad_hw ? ", e.g. " + first_hw : std::string()))
                           .c_str()
                     : "no typed UAV store of R9G9B9E5 here (or the direct paths are off)");
        out = b;
    } catch (const std::exception& e) {
        out = std::string("R9G9B9E5 probe: ") + e.what();
    }
    dx.destroy(src), dx.destroy(dst);
    if (tex) tex->Release();
    if (rb) rb->Release();
    return out;
}

// The D3D12 device on the game's device, ingest / pack / selftest built through it and the self-test run on its own
// queue: a direct dispatch and an indirect one of selftest.comp, every value checked. Throws why it is not usable.
void d3d_setup(Ctx* c) {
    auto t0 = std::chrono::steady_clock::now();
    auto dx = std::make_unique<D3dDevice>();
    std::string err;
    if (!dx->init(c->d3d, &c->dev, c->cache_dir, err)) throw std::runtime_error(err);
    dx->set_stamps(g_stamps);
    struct Unload {   // the kernels go with the device when the check fails
        Ctx* c; D3dDevice* d;
        ~Unload() { if (d) c->dkern.unload(*d); }
    } unload{c, dx.get()};
    c->dkern.load(*dx, utf8(c->shader_dir), {"ingest", "pack", "selftest"});
    std::string direct;   // the texture paths are optional: without them the guides and the output are copied
    try {
        c->dkern.load(*dx, utf8(c->shader_dir), {"d3d_ingest", "d3d_pack"});
    } catch (const std::exception& e) {
        direct = e.what();
    }
    Buffer out[2] = {dx->create(256 * 4), dx->create(256 * 4)}, args = dx->create(16);
    std::vector<uint32_t> got[2] = {std::vector<uint32_t>(256), std::vector<uint32_t>(256)};
    SelfTestPC p[2]{};
    const uint32_t groups[2] = {4, 3};
    try {
        const uint32_t a[4] = {groups[1], 1, 1, 0};
        dx->upload(args, a, 16);
        for (int t = 0; t < 2; ++t) {
            p[t].dst[0] = uint32_t(out[t].addr), p[t].dst[1] = uint32_t(out[t].addr >> 32), p[t].k = 3 + 7 * t, p[t].last = 0x5000000u + t;
            for (uint32_t i = 0; i < 57; ++i) p[t].mid[i] = 1000u * i + 11 * t;
        }
        dx->begin_own();
        dx->dispatch(c->dkern["selftest"], &p[0], sizeof p[0], groups[0]);
        dx->dispatch_indirect(c->dkern["selftest"], &p[1], sizeof p[1], args);
        dx->barrier();
        dx->submit_own();
        for (int t = 0; t < 2; ++t) dx->download(out[t], got[t].data(), 256 * 4);
    } catch (...) {
        for (auto& b : out) dx->destroy(b);
        dx->destroy(args);
        throw;
    }
    for (auto& b : out) dx->destroy(b);
    dx->destroy(args);
    for (int t = 0; t < 2; ++t)
        for (uint32_t i = 0; i < groups[t] * 64; ++i) {
            const uint32_t want = p[t].k * i + p[t].mid[i % 57] + p[t].last + groups[t] * 65536u;
            if (got[t][i] != want) {
                char b[160];
                snprintf(b, sizeof b, "self-test: %s dispatch, element %u is %08x, %08x expected", t ? "indirect" : "direct", i, got[t][i], want);
                throw std::runtime_error(b);
            }
        }
    g_log("D3D12 backend: self-test passed (root constants, spill table, workgroup count, indirect dispatch); %u ELFs from the cache, "
          "%u built, in %.0f ms",
          dx->cache_hits(), dx->cache_misses(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    if (const char* e = getenv("ONR_RR_DIRECT"); e && !strcmp(e, "0")) direct = "turned off (ONR_RR_DIRECT=0)";   // measurements
    std::string why = !direct.empty() ? direct : !dx->views() ? std::string("the D3D12 descriptor size is not 32 bytes") : d3d_check_direct(c, *dx);
    c->direct_ok = why.empty();
    if (c->direct_ok) g_log("D3D12 backend: the guides are read from the game's textures and the output written in place (check passed)");
    else g_log("D3D12 backend: guides copied, output copied (%s)", why.c_str());
    if (getenv("ONR_RR_E5_PROBE")) g_log("D3D12 backend: %s", d3d_probe_e5(c, *dx).c_str());   // measurements
    why.clear();
    if (const char* e = getenv("ONR_RR_NO_SAMPLER")) why = "turned off (ONR_RR_NO_SAMPLER)";   // measurements
    else if (dx->init_sampler(why)) {
        try {
            c->dkern.load(*dx, utf8(c->shader_dir), {"selftest_tex"});
            why = d3d_check_sampler(c, *dx);
        } catch (const std::exception& e) {
            why = e.what();
        }
    }
    if (!why.empty()) dx->sampler_off();
    if (dx->sampler_ready()) g_log("D3D12 backend: the post samples its history through the sampler, as on Vulkan (check passed)");
    else g_log("D3D12 backend: the post blends its history by hand (%s)", why.c_str());
    unload.d = nullptr;
    c->dx = std::move(dx);
}

// a D3D12 recording's times once its slot retired: the network's span and the parts around it
void d3d_harvest(Ctx* c) {
    const double ms = 1000.0 / double(c->dx->timestamp_frequency());
    for (uint32_t slot = 0; slot < D3dDevice::kSlots; ++slot) {
        const uint32_t n = c->dq[slot];
        if (n < 2) continue;
        std::vector<uint64_t> ts(n);
        if (!c->dx->read_stamps(slot, ts)) continue;
        c->dq[slot] = 0;
        add_gaps(c, ts, ms);
        const uint32_t a = c->dqnet[slot][0], b = c->dqnet[slot][1];
        float part[3] = {};
        const bool has_parts = b > a && b <= n;
        if (has_parts) {
            part[0] = float(double(ts[a] - ts.front()) * ms);
            part[1] = float(double(ts[b - 1] - ts[a]) * ms);
            part[2] = float(double(ts.back() - ts[b - 1]) * ms);
        }
        record_gpu_sample(c, float(double(ts.back() - ts.front()) * ms), part, has_parts);
    }
}

// a D3D12 buffer of at least n bytes, kept for larger frames
void d3d_ensure(Ctx* c, Buffer& b, UINT64 n) {
    if (b.addr && b.size >= n) return;
    if (b.addr) c->dx->destroy(b);
    b = c->dx->create(n);
}

// The typed view d3d_ingest reads a guide through, or UNKNOWN when its texel fetch might not give the bits tf_read
// gives: float formats convert exactly; 8-bit unorm only into the albedos, which pack it back to the same bytes.
DXGI_FORMAT direct_view(DXGI_FORMAT f, int slot) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
        return slot == G_ALBEDO || slot == G_SPEC ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM:
        return slot == G_ALBEDO || slot == G_SPEC ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_UNKNOWN;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// The guides d3d_ingest reads from the game's textures (a view each in the bound recording's table, at the guide's
// slot), moved into a readable state; back: the moves to undo after the frame. A texture any guide must copy is
// copied for all of them.
void d3d_plan_direct(Ctx* c, Job& j, std::vector<D3D12_RESOURCE_BARRIER>& back) {
    const OnrRrTexture* tex[G_COUNT];
    frame_textures(j.f, tex);
    DXGI_FORMAT view[G_COUNT] = {};
    for (int i = 0; i < G_COUNT; ++i) {
        if (!j.in[i].present || i >= 16) continue;   // views 0..15 only
        D3D12_RESOURCE_DESC rd = static_cast<ID3D12Resource*>(tex[i]->resource)->GetDesc();
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.SampleDesc.Count == 1 &&
            !(rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            view[i] = direct_view(rd.Format, i);
    }
    for (int i = 0; i < G_COUNT; ++i)
        for (int k = 0; k < G_COUNT; ++k)
            if (j.in[i].present && j.in[k].present && tex[i]->resource == tex[k]->resource && !view[k]) view[i] = DXGI_FORMAT_UNKNOWN;
    std::vector<D3D12_RESOURCE_BARRIER> to;
    for (int i = 0; i < G_COUNT; ++i) {
        if (!view[i]) continue;
        auto* r = static_cast<ID3D12Resource*>(tex[i]->resource);
        j.in[i].direct = true;
        c->dx->srv(r, uint32_t(i), uint32_t(view[i]));
        const auto st = D3D12_RESOURCE_STATES(tex[i]->state);
        if (st & kRead) continue;
        bool seen = false;
        for (auto& b : to) seen |= b.Transition.pResource == r;
        if (seen) continue;
        D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, st, kRead};
        to.push_back(b);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        back.push_back(b);
    }
    if (!to.empty()) c->dx->list()->ResourceBarrier(UINT(to.size()), to.data());
    else c->dx->barrier();
}

// the job's guides that are not read directly, copied into dinput (one barrier each way), on the bound list
void d3d_copy_inputs(Ctx* c, Job& j) {
    const OnrRrTexture* tex[G_COUNT];
    frame_textures(j.f, tex);
    D3dDevice::TexCopy cp[G_COUNT];
    uint32_t n = 0;
    uint64_t compact_off = 0;
    for (int i = 0; i < G_COUNT; ++i) {
        if (!j.in[i].present || j.in[i].direct) continue;
        j.in[i].offset = compact_off;
        cp[n++] = {static_cast<ID3D12Resource*>(tex[i]->resource), tex[i]->state, compact_off, j.in[i].pitch, 0, 0};
        compact_off = (compact_off + uint64_t(j.in[i].pitch) * j.in[i].th + 511) & ~uint64_t(511);
    }
    if (n) {
        d3d_ensure(c, c->dinput, compact_off);
        c->dx->copy_textures_to_buffer(cp, n, c->dinput);
    }
}

// d3d_pack straight into the output: an rgba16f texture that takes a UAV (the store keeps the bits); r set up, the
// texture moved into UNORDERED_ACCESS (back: the move to undo). False: the rows go through doutput and a copy.
DXGI_FORMAT direct_uav_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool d3d_direct_output(Ctx* c, const Job& j, Rec& r, std::vector<D3D12_RESOURCE_BARRIER>& back) {
    auto* out = static_cast<ID3D12Resource*>(j.f.output.resource);
    D3D12_RESOURCE_DESC od = out->GetDesc();
    const DXGI_FORMAT uav_fmt = direct_uav_format(od.Format);
    if (!c->direct_ok || uav_fmt == DXGI_FORMAT_UNKNOWN || !(od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        od.SampleDesc.Count != 1 || j.f.output.x > 65535 || j.f.output.y > 65535)
        return false;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{uav_fmt};
    if (FAILED(c->d3d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) ||
        !(fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) ||
        !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return false;
    c->dx->uav(out, 16, uav_fmt);
    r.out_fmt = uint32_t(j.out_tf) | 256u, r.out_pitch = j.f.output.x | j.f.output.y << 16;
    const auto st = D3D12_RESOURCE_STATES(j.f.output.state);
    if (st != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition = {out, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, st, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        c->dx->list()->ResourceBarrier(1, &b);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        back.push_back(b);
    }
    return true;
}

void d3d_copy_output(Ctx* c, const Job& j) {
    c->dx->copy_buffer_to_texture(c->doutput, j.out_pitch, static_cast<ID3D12Resource*>(j.f.output.resource), j.f.output.state, j.f.output.x,
                                  j.f.output.y, UINT(c->out_size.x), UINT(c->out_size.y));
}

int32_t GetBackend(void* ctx, uint32_t* backend) {
    if (!ctx || !backend) return fail(ONR_RR_INVALID_ARGUMENT, "GetBackend: missing arguments");
    auto* c = static_cast<Ctx*>(ctx);
    *backend = c->vk_host ? ONR_RR_BACKEND_VULKAN_HOST : c->use_d3d ? ONR_RR_BACKEND_D3D12 : ONR_RR_BACKEND_VULKAN;
    return ONR_RR_OK;
}

int32_t Record(void* ctx, const OnrRrFrame* f, void* list, uint32_t edit, OnrRrDenoisedImage* image, void** job) {
    if (!ctx || !f || !list || !job || f->size < ONR_RR_FRAME_SIZE_V1 || !f->color.resource || !f->output.resource ||
        (image && image->size < sizeof(OnrRrDenoisedImage)))
        return fail(ONR_RR_INVALID_ARGUMENT, "Record: missing arguments");
    *job = nullptr;
    if (image) image->buffer = nullptr;
    auto* c = static_cast<Ctx*>(ctx);
    auto* cl = static_cast<ID3D12GraphicsCommandList*>(list);
    sweep_retired();
    std::lock_guard l(c->m);
    if (!c->use_d3d) return fail(ONR_RR_UNSUPPORTED, c->vk_host ? "Record: the Vulkan host backend runs, use RecordVk" : "Record: the Vulkan backend runs");
    D3dDevice& d = *c->dx;
    try {
        std::unique_ptr<Job> j;
        UINT64 in_bytes = 0, out_bytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[G_COUNT]{};
        if (int32_t rc = plan_job(c, f, j, fp, in_bytes, out_bytes)) return rc;
        stats_read(c, j->seq);
        stats_record(c, *j, cl);
        I2 render{int32_t(j->f.render_width), int32_t(j->f.render_height)};
        // as PrepareDenoised: no hop on the frame that finds the network ready
        const bool was_ready = c->state == ONR_RR_STATE_READY;
        // our buffers outlive the lists that use them by D3dDevice's deferred release, so nothing waits here
        const bool network = prepare_network(c, d, render, [] {});
        // dinput and doutput are allocated on demand below only when a texture is not read/written directly.
        (void)out_bytes;
        const bool hop = network && was_ready && image && (edit == ONR_RR_EDIT_LOOP || edit == ONR_RR_EDIT_DISPLAY);
        if (hop) {
            j->edit = edit;
            j->image_half = image->format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            j->image_pitch = (j->f.render_width * (j->image_half ? 8 : 16) + 255) & ~255u;
            d3d_ensure(c, c->dimage, UINT64(j->image_pitch) * j->f.render_height);
        }
        if (network && c->engine.display_history() && !(hop && edit == ONR_RR_EDIT_DISPLAY)) c->engine.drop_display_history();
        if (network && (hop ? edit : uint32_t(ONR_RR_EDIT_OFF)) != c->logged_edit) {
            c->logged_edit = hop ? edit : uint32_t(ONR_RR_EDIT_OFF);
            g_log("denoised image hop: %s", c->logged_edit == ONR_RR_EDIT_LOOP ? "loop" : c->logged_edit == ONR_RR_EDIT_DISPLAY ? "display" : "off");
        }

        const uint32_t hint = uint32_t(c->drecords++ % D3dDevice::kGameSlots);
        d3d_harvest(c);
        d.bind(cl, hint);
        const uint32_t slot = d.bound_slot();
        const bool overflow = d.using_overflow();
        uint32_t dummy_qnet[2] = {};
        Rec r{d, c->dkern, c->dinput.addr, c->doutput.addr, c->dimage.addr, overflow ? dummy_qnet : c->dqnet[slot]};
        std::vector<D3D12_RESOURCE_BARRIER> back;
        try {
            d.stamp();
            if (c->direct_ok) {
                r.ingest = "d3d_ingest", r.pack = "d3d_pack";
                d3d_plan_direct(c, *j, back);
            }
            d3d_copy_inputs(c, *j);
            r.input = c->dinput.addr;
            const bool direct_out = !hop && d3d_direct_output(c, *j, r, back);
            if (!direct_out && !hop) {
                d3d_ensure(c, c->doutput, out_bytes);
                r.output = c->doutput.addr;
            }
            record_frame(c, r, *j, network, hop);
            if (!hop) {
                if (direct_out) d.barrier();
                else d3d_copy_output(c, *j);
                d.stamp();
            }
            if (!back.empty()) cl->ResourceBarrier(UINT(back.size()), back.data());
        } catch (...) {
            if (!overflow) c->dq[slot] = 0;
            d.unbind();
            throw;
        }
        // a hop's times come with its RecordPost
        if (!overflow) c->dq[slot] = hop ? 0 : d.queries();
        if (g_stamps && !hop && !overflow && network) c->stamp_names = d.stamp_names;
        d.unbind();
        if (network) log_frame(c, render);
        if (!hop) return ONR_RR_OK;
        j->d3d = true;
        j->post_pending = true;
        image->buffer = c->dimage.res;
        image->format = j->image_half ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
        image->width = j->f.render_width, image->height = j->f.render_height;
        image->row_pitch = j->image_pitch;
        Job* p = j.get();
        c->jobs[p] = std::move(j);
        while (c->jobs.size() > 64) c->jobs.erase(c->jobs.begin());
        *job = p;
        return ONR_RR_OK;
    } catch (const std::exception& e) {
        if (c->state != ONR_RR_STATE_FAILED) {
            c->state = ONR_RR_STATE_FAILED;
            c->set_message(std::string("frame failed: ") + e.what());
        }
        return fail(ONR_RR_FAILED, e.what());
    }
}

int32_t RecordPost(void* ctx, void* job, void* list, uint32_t format) {
    if (!ctx || !job || !list) return fail(ONR_RR_INVALID_ARGUMENT, "RecordPost: missing arguments");
    auto* c = static_cast<Ctx*>(ctx);
    std::lock_guard l(c->m);
    auto it = c->jobs.find(static_cast<Job*>(job));
    if (it == c->jobs.end() || !it->second->d3d || !it->second->post_pending) return fail(ONR_RR_INVALID_ARGUMENT, "RecordPost: unknown job");
    Job& j = *it->second;
    D3dDevice& d = *c->dx;
    int32_t rc = ONR_RR_OK;
    try {
        Tf tf = format ? tf_of(DXGI_FORMAT(format)) : TF_NONE;
        if (format && (tf == TF_NONE || tf_bytes(tf) > 16)) {
            static bool said = false;
            if (!std::exchange(said, true)) g_log("RecordPost: edit format %u is not read; the network's image is used", format);
            tf = TF_NONE;
        }
        const uint32_t hint = uint32_t(c->drecords++ % D3dDevice::kGameSlots);
        d3d_harvest(c);
        d.bind(static_cast<ID3D12GraphicsCommandList*>(list), hint);
        const uint32_t slot = d.bound_slot();
        const bool overflow = d.using_overflow();
        uint32_t dummy_qnet[2] = {};
        Rec r{d, c->dkern, c->dinput.addr, c->doutput.addr, c->dimage.addr, overflow ? dummy_qnet : c->dqnet[slot]};
        std::vector<D3D12_RESOURCE_BARRIER> back;
        try {
            d.stamp();
            if (c->direct_ok) r.pack = "d3d_pack";
            const bool direct_out = d3d_direct_output(c, j, r, back);
            if (!direct_out) {
                d3d_ensure(c, c->doutput, j.out_bytes);
                r.output = c->doutput.addr;
            }
            record_post(c, r, j, tf);
            if (direct_out) d.barrier();
            else d3d_copy_output(c, j);
            d.stamp();
            if (!back.empty()) d.list()->ResourceBarrier(UINT(back.size()), back.data());
        } catch (...) {
            if (!overflow) c->dq[slot] = 0;
            d.unbind();
            throw;
        }
        if (!overflow) c->dq[slot] = d.queries();
        d.unbind();
    } catch (const std::exception& e) {
        if (c->state != ONR_RR_STATE_FAILED) {
            c->state = ONR_RR_STATE_FAILED;
            c->set_message(std::string("frame failed: ") + e.what());
        }
        rc = fail(ONR_RR_FAILED, e.what());
    }
    c->jobs.erase(it);
    return rc;
}

int32_t PrepareDenoised(void* ctx, void* job, uint32_t edit, OnrRrDenoisedImage* image) {
    if (!ctx || !job || !image || image->size < sizeof(OnrRrDenoisedImage))
        return fail(ONR_RR_INVALID_ARGUMENT, "PrepareDenoised: missing arguments");
    image->buffer = nullptr;
    auto* c = static_cast<Ctx*>(ctx);
    std::lock_guard l(c->m);
    auto it = c->jobs.find(static_cast<Job*>(job));
    if (it == c->jobs.end()) return fail(ONR_RR_INVALID_ARGUMENT, "PrepareDenoised: unknown job");
    Job& j = *it->second;
    j.edit = ONR_RR_EDIT_OFF;
    if ((edit != ONR_RR_EDIT_LOOP && edit != ONR_RR_EDIT_DISPLAY) || c->state != ONR_RR_STATE_READY) return ONR_RR_OK;
    try {
        // rgba32f (rgba16f when asked for) rows in a buffer shared like the input and output, kept for larger frames
        j.image_half = image->format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        j.image_pitch = (j.f.render_width * (j.image_half ? 8 : 16) + 255) & ~255u;
        const UINT64 bytes = UINT64(j.image_pitch) * j.f.render_height;
        if (!c->image || c->image->size < bytes) {
            if (c->image) c->retired.push_back(c->image);
            c->image = std::make_shared<SharedBuffer>();
            c->image->create_d3d(c->d3d, bytes);
            g_log("denoised image buffer: %llu MB", (unsigned long long)(c->image->size >> 20));
        }
    } catch (const std::exception& e) {
        g_log("PrepareDenoised: %s", e.what());
        return fail(ONR_RR_FAILED, e.what());
    }
    j.image = c->image;
    j.edit = edit;
    image->buffer = j.image->res;
    image->format = j.image_half ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    image->width = j.f.render_width, image->height = j.f.render_height;
    image->row_pitch = j.image_pitch;
    return ONR_RR_OK;
}

int32_t EnqueueNetwork(void* ctx, void* job, void* queue, uint32_t* exported) {
    if (!ctx || !job || !queue) return fail(ONR_RR_INVALID_ARGUMENT, "Enqueue: missing arguments");
    if (exported) *exported = 0;
    auto* c = static_cast<Ctx*>(ctx);
    std::lock_guard l(c->m);
    auto it = c->jobs.find(static_cast<Job*>(job));
    if (it == c->jobs.end()) return fail(ONR_RR_INVALID_ARGUMENT, "Enqueue: unknown job");
    Job& j = *it->second;
    if (j.enqueued || j.post_pending) return ONR_RR_OK;
    int32_t rc;
    try {
        rc = enqueue(c, j, static_cast<ID3D12CommandQueue*>(queue), exported);
    } catch (const std::exception& e) {
        if (c->state != ONR_RR_STATE_FAILED) {
            c->state = ONR_RR_STATE_FAILED;
            c->set_message(std::string("frame failed: ") + e.what());
        }
        rc = fail(ONR_RR_FAILED, e.what());
        j.post_pending = false;
        if (exported) *exported = 0;
    }
    j.enqueued = !j.post_pending;
    if (j.enqueued && j.output_recorded) c->jobs.erase(it);
    return rc;
}

int32_t Enqueue(void* ctx, void* job, void* queue) { return EnqueueNetwork(ctx, job, queue, nullptr); }

int32_t EnqueuePost(void* ctx, void* job, void* queue, uint32_t format) {
    if (!ctx || !job || !queue) return fail(ONR_RR_INVALID_ARGUMENT, "EnqueuePost: missing arguments");
    auto* c = static_cast<Ctx*>(ctx);
    std::lock_guard l(c->m);
    auto it = c->jobs.find(static_cast<Job*>(job));
    // a job EnqueueNetwork ran whole is gone once its output was recorded
    if (it == c->jobs.end()) return ONR_RR_OK;
    Job& j = *it->second;
    if (!j.post_pending) return ONR_RR_OK;
    j.post_pending = false;
    j.enqueued = true;
    int32_t rc = ONR_RR_OK;
    try {
        Tf tf = format ? tf_of(DXGI_FORMAT(format)) : TF_NONE;
        if (format && (tf == TF_NONE || tf_bytes(tf) > 16)) {
            static bool said = false;
            if (!std::exchange(said, true)) g_log("EnqueuePost: edit format %u is not read; the network's image is used", format);
            tf = TF_NONE;
        }
        const int s = int(j.seq % kSlots);
        Rec r{c->dev, c->kern, j.input->addr, j.output->addr, j.image->addr, c->qnet[s + kSlots]};
        record_submit(c, s + kSlots, true, [&] { record_post(c, r, j, tf); });
        c->hop_of[s] = c->hop_of[s + kSlots] = ++c->hops;
        submit(c, int(j.seq % kSlots) + kSlots, static_cast<ID3D12CommandQueue*>(queue));
    } catch (const std::exception& e) {
        if (c->state != ONR_RR_STATE_FAILED) {
            c->state = ONR_RR_STATE_FAILED;
            c->set_message(std::string("frame failed: ") + e.what());
        }
        rc = fail(ONR_RR_FAILED, e.what());
    }
    if (j.output_recorded) c->jobs.erase(it);
    return rc;
}

int32_t RecordOutput(void* ctx, void* job, void* list) {
    if (!ctx || !job || !list) return fail(ONR_RR_INVALID_ARGUMENT, "RecordOutput: missing arguments");
    auto* c = static_cast<Ctx*>(ctx);
    std::lock_guard l(c->m);
    auto it = c->jobs.find(static_cast<Job*>(job));
    if (it == c->jobs.end()) return fail(ONR_RR_INVALID_ARGUMENT, "RecordOutput: unknown job");
    Job& j = *it->second;
    auto* cl = static_cast<ID3D12GraphicsCommandList*>(list);
    auto* out = static_cast<ID3D12Resource*>(j.f.output.resource);
    auto st = D3D12_RESOURCE_STATES(j.f.output.state);
    D3D12_RESOURCE_DESC od = out->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    fp.Footprint = {od.Format, UINT(c->out_size.x), UINT(c->out_size.y), 1, j.out_pitch};
    transition(cl, j.output->res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(cl, out, st, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{j.output->res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    src.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION dst{out, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    dst.SubresourceIndex = 0;
    D3D12_BOX box{0, 0, 0, UINT(c->out_size.x), UINT(c->out_size.y), 1};
    cl->CopyTextureRegion(&dst, j.f.output.x, j.f.output.y, 0, &src, &box);
    transition(cl, out, D3D12_RESOURCE_STATE_COPY_DEST, st);
    transition(cl, j.output->res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    j.output_recorded = true;
    if (j.enqueued) c->jobs.erase(it);
    return ONR_RR_OK;
}

// ------------------------------------------------------------------------------------------------ Vulkan host backend

// a slot's timestamps once its command buffer ran; nothing while they are not written
void vk_harvest(Ctx* c) {
    const double ms = c->dev.timestamp_period_ns() * 1e-6;
    for (uint32_t slot = 0; slot < Ctx::kHostSlots; ++slot) {
        const uint32_t n = c->hq[slot];
        if (n < 2 || !c->hqp[slot] || !c->dev.pool_retired(c->hqp[slot])) continue;
        c->hq[slot] = 0;
        std::vector<uint64_t> ts(n);
        if (vkGetQueryPoolResults(c->dev.vk(), c->hqp[slot], 0, n, ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) continue;
        add_gaps(c, ts, ms);
        const uint32_t a = c->hqnet[slot][0], b = c->hqnet[slot][1];
        float part[3] = {};
        const bool has_parts = b > a && b <= n;
        if (has_parts) {
            part[0] = float(double(ts[a] - ts.front()) * ms);
            part[1] = float(double(ts[b - 1] - ts[a]) * ms);
            part[2] = float(double(ts.back() - ts[b - 1]) * ms);
        }
        record_gpu_sample(c, float(double(ts.back() - ts.front()) * ms), part, has_parts);
    }
}

void vk_ensure(Ctx* c, Buffer& b, UINT64 n) {
    if (b.addr && b.size >= n) return;
    if (b.addr) c->dev.destroy(b);
    b = c->dev.create(n);
}

// the layout a texture is in: the frame's, UNDEFINED read as GENERAL (a host that does not know); logged once per slot
VkImageLayout vk_layout(Ctx* c, int slot, uint32_t state) {
    const VkImageLayout l = state ? VkImageLayout(state) : VK_IMAGE_LAYOUT_GENERAL;
    if (c->logged_layout[slot] != l + 1) {
        c->logged_layout[slot] = VkImageLayout(l + 1);
        g_log("%s %d: layout %u%s", slot == G_COUNT ? "output" : "guide", slot == G_COUNT ? 0 : slot, unsigned(l), state ? "" : " (none given, GENERAL assumed)");
    }
    return l;
}

VkImageMemoryBarrier2 vk_image_barrier(const OnrRrVkImage& im, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                                       VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = ss, b.srcAccessMask = sa, b.dstStageMask = ds, b.dstAccessMask = da;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = reinterpret_cast<VkImage>(im.image);
    b.subresourceRange = {vk_aspects(VkFormat(im.format)), im.mip, 1, im.layer, 1};
    return b;
}

void vk_barrier(VkCommandBuffer cb, const VkMemoryBarrier2& mb, const std::vector<VkImageMemoryBarrier2>& ib) {
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    di.imageMemoryBarrierCount = uint32_t(ib.size());
    di.pImageMemoryBarriers = ib.data();
    vkCmdPipelineBarrier2(cb, &di);
}

// as direct_view: the formats whose texel fetch gives the bits tf_read gives
bool vk_direct_format(VkFormat f, int slot) {
    switch (f) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        return slot == G_ALBEDO || slot == G_SPEC;
    default: return false;
    }
}

bool vk_sampled_layout(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_GENERAL || l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
           l == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL ||
           l == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
}

// the guides d3d_ingest (and the network) read through the game's views, at their slots of the recording's set; a
// texture any guide must copy is copied for all of them
void vk_plan_direct(Ctx* c, Job& j) {
    const OnrRrTexture* tex[G_COUNT];
    frame_textures(j.f, tex);
    bool ok[G_COUNT] = {};
    for (int i = 0; i < 16; ++i) {
        if (!j.in[i].present) continue;
        auto& im = *static_cast<const OnrRrVkImage*>(tex[i]->resource);
        ok[i] = im.view && (im.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && vk_sampled_layout(vk_layout(c, i, tex[i]->state)) &&
                vk_direct_format(VkFormat(im.format), i);
    }
    for (int i = 0; i < G_COUNT; ++i)
        for (int k = 0; k < G_COUNT; ++k)
            if (j.in[i].present && j.in[k].present && tex[i]->resource && tex[k]->resource &&
                static_cast<const OnrRrVkImage*>(tex[i]->resource)->image == static_cast<const OnrRrVkImage*>(tex[k]->resource)->image && !ok[k])
                ok[i] = false;
    for (int i = 0; i < 16; ++i) {
        if (!ok[i]) continue;
        auto& im = *static_cast<const OnrRrVkImage*>(tex[i]->resource);
        j.in[i].direct = true;
        c->dev.set_view(uint32_t(i), reinterpret_cast<VkImageView>(im.view), vk_layout(c, i, tex[i]->state));
    }
}

// pack / downsample straight into the output: an rgba16f image with a storage view in GENERAL (the store keeps the
// bits); r set up. False: the rows go through voutput and a copy.
bool vk_direct_output(Ctx* c, const Job& j, Rec& r) {
    auto& im = *static_cast<const OnrRrVkImage*>(j.f.output.resource);
    if (!c->direct_ok || j.out_tf != TF_RGBA16F || !im.view || !(im.usage & VK_IMAGE_USAGE_STORAGE_BIT) ||
        vk_layout(c, G_COUNT, j.f.output.state) != VK_IMAGE_LAYOUT_GENERAL || j.f.output.x > 65535 || j.f.output.y > 65535)
        return false;
    c->dev.set_view(16, reinterpret_cast<VkImageView>(im.view), VK_IMAGE_LAYOUT_GENERAL);
    r.out_fmt = TF_RGBA16F | 256u, r.out_pitch = j.f.output.x | j.f.output.y << 16;
    return true;
}

// the guides not read directly into vinput: every texture to TRANSFER_SRC in one barrier (a texture given twice moves
// once), the copies, and back; GENERAL ones are copied where they are. The first barrier also orders the game's work
// before our reads of its textures and our write of the output.
void vk_copy_inputs(Ctx* c, const Job& j, VkCommandBuffer cb) {
    const OnrRrTexture* tex[G_COUNT];
    frame_textures(j.f, tex);
    std::vector<VkImageMemoryBarrier2> to, back;
    std::vector<VkImage> moved;
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    // the transfer stages: also the fills and updates of the frame (clear stage), after the last frame's reads
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    for (int i = 0; i < G_COUNT; ++i) {
        if (!j.in[i].present || j.in[i].direct) continue;
        auto& im = *static_cast<const OnrRrVkImage*>(tex[i]->resource);
        const VkImageLayout l = vk_layout(c, i, tex[i]->state);
        const VkImage img = reinterpret_cast<VkImage>(im.image);
        if (l == VK_IMAGE_LAYOUT_GENERAL || l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || std::find(moved.begin(), moved.end(), img) != moved.end()) continue;
        moved.push_back(img);
        to.push_back(vk_image_barrier(im, l, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT));
        back.push_back(vk_image_barrier(im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE,
                                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT));
    }
    vk_barrier(cb, mb, to);
    for (int i = 0; i < G_COUNT; ++i) {
        const Copy& cp = j.in[i];
        if (!cp.present || cp.direct) continue;
        auto& im = *static_cast<const OnrRrVkImage*>(tex[i]->resource);
        const VkImage img = reinterpret_cast<VkImage>(im.image);
        const bool here = std::find(moved.begin(), moved.end(), img) == moved.end();
        VkBufferImageCopy r{};
        r.bufferOffset = cp.offset;
        r.bufferRowLength = cp.pitch / tf_bytes(cp.tf);
        r.imageSubresource = {vk_depth(VkFormat(im.format)) ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                              im.mip, im.layer, 1};
        r.imageExtent = {im.width, im.height, 1};
        // only the part the kernels read (render size from the guide's origin, output size for the layer's MVs), at its
        // place in the rows: a view's size from NGX larger than the image would copy past it
        const uint32_t nw = i == G_TLMV ? uint32_t(c->out_size.x) : j.f.render_width, nh = i == G_TLMV ? uint32_t(c->out_size.y) : j.f.render_height;
        const UINT64 at = cp.offset + UINT64(cp.y) * cp.pitch + UINT64(cp.x) * tf_bytes(cp.tf);
        if (cp.x < im.width && cp.y < im.height && at % 4 == 0) {
            r.bufferOffset = at;
            r.imageOffset = {int32_t(cp.x), int32_t(cp.y), 0};
            r.imageExtent = {std::min(nw, im.width - cp.x), std::min(nh, im.height - cp.y), 1};
        }
        vkCmdCopyImageToBuffer(cb, img, here ? vk_layout(c, i, tex[i]->state) : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->vinput.buf, 1, &r);
    }
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    vk_barrier(cb, mb, back);
}

// voutput's rows into the output image (to TRANSFER_DST and back, or where it is when GENERAL)
void vk_copy_output(Ctx* c, const Job& j, VkCommandBuffer cb) {
    auto& im = *static_cast<const OnrRrVkImage*>(j.f.output.resource);
    const VkImageLayout l = vk_layout(c, G_COUNT, j.f.output.state);
    const bool here = l == VK_IMAGE_LAYOUT_GENERAL || l == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    std::vector<VkImageMemoryBarrier2> ib;
    if (here) {
        // the game's own reads and writes of the image before ours
        mb.srcStageMask |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.srcAccessMask |= VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstAccessMask |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    } else {
        ib.push_back(vk_image_barrier(im, l, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT));
    }
    vk_barrier(cb, mb, ib);
    VkBufferImageCopy r{};
    r.bufferRowLength = j.out_pitch / tf_bytes(j.out_tf);
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, im.mip, im.layer, 1};
    r.imageOffset = {int32_t(j.f.output.x), int32_t(j.f.output.y), 0};
    r.imageExtent = {uint32_t(c->out_size.x), uint32_t(c->out_size.y), 1};
    vkCmdCopyBufferToImage(cb, c->voutput.buf, reinterpret_cast<VkImage>(im.image), here ? l : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    for (auto& b : ib) {
        std::swap(b.oldLayout, b.newLayout);
        b.srcStageMask = mb.srcStageMask, b.srcAccessMask = mb.srcAccessMask, b.dstStageMask = mb.dstStageMask, b.dstAccessMask = mb.dstAccessMask;
    }
    vk_barrier(cb, mb, ib);
}

// after the output was written in place: our stores before the game's next use of it
void vk_output_written(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    vk_barrier(cb, mb, {});
}

int32_t RecordVk(void* ctx, const OnrRrFrame* f, void* cmd) {
    if (!ctx || !f || !cmd || f->size < ONR_RR_FRAME_SIZE_V1 || !f->color.resource || !f->output.resource)
        return fail(ONR_RR_INVALID_ARGUMENT, "RecordVk: missing arguments");
    auto* c = static_cast<Ctx*>(ctx);
    auto* cb = static_cast<VkCommandBuffer>(cmd);
    if (c->vk_host) sweep_retired(c->dev.vk());
    std::lock_guard l(c->m);
    if (!c->vk_host) return fail(ONR_RR_UNSUPPORTED, "RecordVk: the runtime was not created on a Vulkan device");
    Device& d = c->dev;
    try {
        std::unique_ptr<Job> j;
        UINT64 in_bytes = 0, out_bytes = 0;
        if (int32_t rc = plan_job(c, f, j, nullptr, in_bytes, out_bytes)) return rc;
        I2 render{int32_t(j->f.render_width), int32_t(j->f.render_height)};
        // buffers replaced here outlive the recordings that use them (the device's grave), so nothing waits
        const bool network = prepare_network(c, d, render, [] {});
        vk_ensure(c, c->vinput, in_bytes);
        vk_ensure(c, c->voutput, out_bytes);
        vk_harvest(c);
        const uint32_t hint = uint32_t(c->hrecords++ % Ctx::kHostSlots);
        uint32_t slot = UINT32_MAX;
        for (uint32_t i = 0; i < Ctx::kHostSlots; ++i) {
            const uint32_t cand = (hint + i) % Ctx::kHostSlots;
            if (!c->hqp[cand] || d.pool_retired(c->hqp[cand])) {
                slot = cand;
                break;
            }
        }
        if (slot < Ctx::kHostSlots && !c->hqp[slot]) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = kMaxQueries;
            if (vkCreateQueryPool(d.vk(), &qi, nullptr, &c->hqp[slot]) != VK_SUCCESS) throw std::runtime_error("vkCreateQueryPool");
        }
        d.bind(cb, slot < Ctx::kHostSlots ? c->hqp[slot] : VK_NULL_HANDLE);
        d.use_views(slot);
        uint32_t dummy_qnet[2] = {};
        Rec r{d, c->kern, c->vinput.addr, c->voutput.addr, 0, slot < Ctx::kHostSlots ? c->hqnet[slot] : dummy_qnet};
        try {
            d.stamp();
            if (c->direct_ok) {
                r.ingest = "d3d_ingest", r.pack = "d3d_pack";
                vk_plan_direct(c, *j);
            }
            const bool direct_out = vk_direct_output(c, *j, r);
            vk_copy_inputs(c, *j, cb);
            record_frame(c, r, *j, network);
            if (direct_out) vk_output_written(cb);
            else vk_copy_output(c, *j, cb);
            d.stamp();
        } catch (...) {
            if (slot < Ctx::kHostSlots) c->hq[slot] = 0;
            d.unbind();
            throw;
        }
        if (slot < Ctx::kHostSlots) c->hq[slot] = d.queries();
        d.unbind();
        if (network) log_frame(c, render);
        return ONR_RR_OK;
    } catch (const std::exception& e) {
        if (c->state != ONR_RR_STATE_FAILED) {
            c->state = ONR_RR_STATE_FAILED;
            c->set_message(std::string("frame failed: ") + e.what());
        }
        return fail(ONR_RR_FAILED, e.what());
    }
}

int32_t PrepareVkDevice(void* phys, const void* info, void* out, void** setup) {
    if (!phys || !info || !out || !setup) return fail(ONR_RR_INVALID_ARGUMENT, "PrepareVkDevice: missing arguments");
    *setup = nullptr;
    try {
        std::string why;
        if (onr_host::prepare(static_cast<VkPhysicalDevice>(phys), static_cast<const VkDeviceCreateInfo*>(info), static_cast<VkDeviceCreateInfo*>(out), setup,
                              why)) {
            std::lock_guard l(g_vk_setup_m);
            g_vk_setup = "not offered: " + why;
            return fail(ONR_RR_UNSUPPORTED, why);
        }
        return ONR_RR_OK;
    } catch (const std::exception& e) {
        *static_cast<VkDeviceCreateInfo*>(out) = *static_cast<const VkDeviceCreateInfo*>(info);
        return fail(ONR_RR_FAILED, e.what());
    }
}

int32_t FinishVkDevice(void* setup, void* device, uint32_t* family, uint32_t* index) {
    if (!setup) return fail(ONR_RR_INVALID_ARGUMENT, "FinishVkDevice: no setup");
    std::string added;
    uint32_t f = 0, i = 0;
    const int r = onr_host::finish(setup, static_cast<VkDevice>(device), &f, &i, &added);
    if (family) *family = f;
    if (index) *index = i;
    std::lock_guard l(g_vk_setup_m);
    g_vk_setup = r ? std::string("the device was not created with it") :
                     "added " + (added.empty() ? std::string("nothing (the game had it all)") : added) + "; the runtime's queue: family " + std::to_string(f) +
                         ", index " + std::to_string(i);
    return r ? fail(ONR_RR_FAILED, "the device was not created") : ONR_RR_OK;
}

int32_t Cancel(void* ctx, void* job) {
    if (!ctx || !job) return ONR_RR_OK;
    auto* c = static_cast<Ctx*>(ctx);
    std::lock_guard l(c->m);
    c->jobs.erase(static_cast<Job*>(job));
    return ONR_RR_OK;
}

int32_t GetInfo(void* ctx, OnrRrInfo* info) {
    if (!ctx || !info || info->size < ONR_RR_INFO_SIZE_V1) return fail(ONR_RR_INVALID_ARGUMENT, "GetInfo: missing arguments");
    auto* c = static_cast<Ctx*>(ctx);
    std::lock_guard l(c->m);
    info->state = c->state;
    info->mode = ONR_RR_MODE_FAST;
    info->frames = uint32_t(c->frames);
    info->gpu_ms_last = c->gpu_last;
    float avg = 0;
    for (float x : c->gpu_hist) avg += x;
    info->gpu_ms_average = c->gpu_hist.empty() ? 0 : avg / float(c->gpu_hist.size());
    info->render_width = uint32_t(c->engine.render().x), info->render_height = uint32_t(c->engine.render().y);
    info->output_width = uint32_t(c->out_size.x), info->output_height = uint32_t(c->out_size.y);
    copy_str(info->weights_source, sizeof info->weights_source,
             c->weights_ok ? c->weights_source + " (" + c->w.version + ")" : c->weights_source);
    std::string msg = c->message;
    if (c->state == ONR_RR_STATE_BUILDING) msg = c->building ? "building the network (frames show the colour until it is ready)" : msg;
    copy_str(info->message, sizeof info->message, msg);
    if (info->size >= sizeof(OnrRrInfo)) {
        info->backend = c->vk_host ? ONR_RR_BACKEND_VULKAN_HOST : c->use_d3d ? ONR_RR_BACKEND_D3D12 : ONR_RR_BACKEND_VULKAN;
        copy_str(info->backend_message, sizeof info->backend_message, c->use_d3d ? std::string() : c->d3d_message);
    }
    return ONR_RR_OK;
}

int32_t GetHopTiming(void* ctx, uint64_t hop, OnrRrHopTiming* t) {
    if (!ctx || !t || t->size < sizeof(OnrRrHopTiming)) return fail(ONR_RR_INVALID_ARGUMENT, "GetHopTiming: missing arguments");
    auto* c = static_cast<Ctx*>(ctx);
    std::lock_guard l(c->m);
    const OnrRrHopTiming& h = c->hop_ring[hop % 16];
    if (!hop || h.hop != hop) return fail(ONR_RR_FAILED, "GetHopTiming: not known");
    *t = h;
    return ONR_RR_OK;
}

int32_t GetLastErrorText(char* buf, uint32_t n) {
    if (buf && n) copy_str(buf, n, t_error);
    return ONR_RR_OK;
}

}  // namespace

// the user's "Fast mode" (docs/FASE24-FASTMODE.md), outside the ABI as OnrRrSetDeltaRate: a DLL without it has no such mode.
// Safe every frame; the first frame asked starts the kernels' build, and the next recorded frame after it uses them.
extern "C" int32_t OnrRrSetFastMode(void* context, int32_t on) {
    if (!context) return ONR_RR_INVALID_ARGUMENT;
    static_cast<Ctx*>(context)->lite_want = on != 0;
    return ONR_RR_OK;
}

// 0 off, 1 on, 2 asked and its kernels still building, 3 asked and not available
extern "C" int32_t OnrRrGetFastMode(void* context) {
    if (!context) return 0;
    auto* c = static_cast<Ctx*>(context);
    if (!c->lite_want) return 0;
    return c->lite_failed ? 3 : c->lite_built ? 1 : 2;
}

extern "C" int32_t OnrRrSetDeltaRate(void* context, float rate) {
    if (!context || !(rate > 0.0f)) return ONR_RR_INVALID_ARGUMENT;
    static_cast<Ctx*>(context)->engine.set_delta_alpha(std::min(rate, 1.0f));
    return ONR_RR_OK;
}

extern "C" int32_t OnrRrGetApi(uint32_t abi, OnrRrApi* api) {
    if (abi < ONR_RR_ABI_VERSION || abi > ONR_RR_ABI_VERSION_5 || !api) return ONR_RR_INVALID_ARGUMENT;
    if (abi >= ONR_RR_ABI_VERSION_5) {
        auto* a5 = reinterpret_cast<OnrRrApi5*>(api);
        a5->PrepareVkDevice = PrepareVkDevice;
        a5->FinishVkDevice = FinishVkDevice;
        a5->RecordVk = RecordVk;
    }
    if (abi >= ONR_RR_ABI_VERSION_4) {
        auto* a4 = reinterpret_cast<OnrRrApi4*>(api);
        a4->GetBackend = GetBackend;
        a4->Record = Record;
        a4->RecordPost = RecordPost;
    }
    if (abi >= ONR_RR_ABI_VERSION_3) reinterpret_cast<OnrRrApi3*>(api)->GetHopTiming = GetHopTiming;
    if (abi >= ONR_RR_ABI_VERSION_2) {
        auto* a2 = reinterpret_cast<OnrRrApi2*>(api);
        a2->PrepareDenoised = PrepareDenoised;
        a2->EnqueueNetwork = EnqueueNetwork;
        a2->EnqueuePost = EnqueuePost;
    }
    api->abi_version = abi;
    api->Check = Check;
    api->Create = Create;
    api->Destroy = Destroy;
    api->SetMode = SetMode;
    api->RecordInputs = RecordInputs;
    api->Enqueue = Enqueue;
    api->RecordOutput = RecordOutput;
    api->Cancel = Cancel;
    api->GetInfo = GetInfo;
    api->GetLastError = GetLastErrorText;
    return ONR_RR_OK;
}
