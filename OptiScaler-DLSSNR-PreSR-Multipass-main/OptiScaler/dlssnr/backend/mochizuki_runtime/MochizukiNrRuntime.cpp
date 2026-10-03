// MochizukiNrRuntime.dll: mochizuki0323's Vulkan network (third_party/mochizuki) behind the lmxxf C ABI.
//
// The network runs on a Vulkan device of its own, on the game's adapter. Before the cut, the game's colour and
// motion vectors are copied into D3D12 buffers that Vulkan imports. Between the two halves of the game's command
// list, the game queue signals a shared fence (toVk), a Vulkan submit waits on it, runs the network and writes the
// result into a third shared buffer, then signals a second shared fence (fromVk), which the game queue waits on.
// After the cut, the result is copied into the texture handed to Super Resolution. Any game queue may run the list
// (MOCHIZUKI_NR_FEATURE_ANY_QUEUE): a frame from another queue than the last one waits for the last one first. The
// model's controls come from the frame (strengths, model scale, passes) and from MochizukiNrSetControls
// (MochizukiNrControls.h), sanitised before the network sees them.
//
// Networks are built off the render thread, on a second queue of the Vulkan device when its family has one, one core
// build at a time in the process. A build for another model scale, pass count or colour encoding then runs while the
// frames keep the old network, which is swapped for the new one when it is ready; another frame extent or format
// replaces the network and the frame's buffers without waiting for the game's queue. The render thread never waits for
// the GPU there: a network the frames no longer use is freed on another thread once the Vulkan side has finished the
// frames that used it. Destroy during a build stops it between two pipelines and waits for it 2 s at most
// (Session::Abandon).
//
// Dynamic resolution (MochizukiNrControls::drs_mode): by default (exact) the network and the frame's buffers are built
// for the frame's render subrect, so every change of it rebuilds the network. In auto, once a subrect smaller than the
// colour allocation is seen, they are built for a bucket instead (Session::DrsExtent): the largest subrect seen,
// rounded up to 64 within the allocation. The frame's buffers are then keyed on the allocation, each frame copies only
// its subrect, the part of the bucket past it repeats the subrect's edge, the motion vectors are laid over the subrect
// in the bucket's uv, and the history restarts whenever the subrect changes. A subrect equal to the bucket runs as it
// does in exact. Always buckets every frame, also across allocations.
//
// Errors (NrError): only a lost device ends the session. It, and a Vulkan side that stops finishing frames (the
// watchdog), releases every game queue waiting on fromVk. A failed network build or buffer allocation is retried
// when the frame asks for something else, or after a backoff when it ran out of memory; a build or buffers the VRAM
// budget cannot hold are refused before anything is allocated, and a network whose buffers are refused goes with
// them; a frame the network fails on passes its colour through.
#define MOCHIZUKI_NR_RUNTIME_EXPORTS
#include "MochizukiNrApi.h"
#include <unordered_map>
#include <wrl/client.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <winternl.h> // NTSTATUS, for d3dkmthk.h
#include <d3dkmthk.h> // D3DKMTQueryVideoMemoryInfo, for the VRAM check (VideoMemory)
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "mz_interpose.h"
#include "nr_log.hpp"
#include "nr_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <clocale>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
thread_local char g_lastError[256] = {};
// Last resort after device loss: never run a Vulkan destructor under uncertain work.
template<class T> void Quarantine(std::shared_ptr<T>& p) noexcept
{
    if (!p) return;
    auto* retained = new (std::nothrow) std::shared_ptr<T>(std::move(p));
    if (!retained) std::terminate();
}
std::atomic<bool> g_logging { false };


void SetError(const char* text)
{
    std::strncpy(g_lastError, text ? text : "", sizeof(g_lastError) - 1);
    g_lastError[sizeof(g_lastError) - 1] = 0;
}

int32_t Fail(int32_t status, const char* text)
{
    SetError(text);
    return status;
}

std::wstring DllDirectory()
{
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&MochizukiNrGetApi), &mod);
    if (!mod)
        return {};
    // GetModuleFileNameW truncates to the buffer and returns its size then: grow it until the name fits.
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD n = GetModuleFileNameW(mod, path.data(), DWORD(path.size()));
        if (!n)
            return {};
        if (n < path.size())
        {
            path.resize(n);
            break;
        }
        if (path.size() >= 65536)
            return {};
        path.resize(path.size() * 2);
    }
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    path.resize(slash);
    return path;
}

// Where dlssnr-amd\ (shaders, model, prewarm) and the log are: beside the DLL, or else beside the game's exe. RE
// Engine games load every DLL from a copy in _storage_\ and leave the folders behind.
std::wstring DataDirectory()
{
    static const std::wstring dir = []
    {
        const std::wstring dll = DllDirectory();
        if (GetFileAttributesW((dll + L"\\dlssnr-amd").c_str()) != INVALID_FILE_ATTRIBUTES)
            return dll;
        std::wstring exe(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
        exe.resize(n < exe.size() ? n : 0);
        const size_t slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos &&
            GetFileAttributesW((exe.substr(0, slash) + L"\\dlssnr-amd").c_str()) != INVALID_FILE_ATTRIBUTES)
            return exe.substr(0, slash);
        return dll;
    }();
    return dir;
}

// The runtime's folder as the core's RuntimeConfig::root, in UTF-8: with LC_CTYPE set to UTF-8 (UseUtf8Paths), the
// core's narrow paths reach the file system unchanged, whatever the ANSI code page is.
std::string RootUtf8(const std::wstring& dir)
{
    const int n = dir.empty() ? 0
                              : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, dir.c_str(), int(dir.size()),
                                                    nullptr, 0, nullptr, nullptr);
    std::string root(size_t(std::max(n, 0)), '\0');
    if (n <= 0 || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, dir.c_str(), int(dir.size()), root.data(), n,
                                      nullptr, nullptr) != n)
        throw std::runtime_error("the runtime's folder name cannot be converted to UTF-8");
    return root;
}

// The core opens its files through narrow paths (std::filesystem::path from std::string, std::ifstream, fopen),
// which the CRT decodes in the LC_CTYPE code page, the ANSI one by default. Only LC_CTYPE, never LC_ALL (number
// formatting stays "C"), and only safe because this DLL is built /MT: the locale belongs to its own CRT, not to the
// game's.
void UseUtf8Paths()
{
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       if (!std::setlocale(LC_CTYPE, ".UTF8"))
                           nr::logf("[mochizuki] setlocale(LC_CTYPE, \".UTF8\") failed; a folder name outside the "
                                    "ANSI code page will fail the network build");
                   });
}

// Fault injection for mz_stress, read once per process at the first Create; nothing happens when unset. N counts a
// session's Enqueue calls (EnqueueHip) from 1.
struct TestHooks
{
    bool failStart = false;      // MZ_TEST_FAIL_START=1: Start throws right after vkCreateInstance
    bool unsupported = false;    // MZ_TEST_UNSUPPORTED=1: Start throws the [unsupported] path after vkCreateInstance
    uint64_t slotTimeoutAt = 0;  // MZ_TEST_SLOT_TIMEOUT_AT=N: Enqueue N finds its slot's wait timed out
    uint64_t enqueueThrowAt = 0; // MZ_TEST_ENQUEUE_THROW_AT=N: Enqueue N throws while recording the network
    uint64_t deviceLostAt = 0;   // MZ_TEST_DEVICE_LOST_AT=N: Enqueue N throws a lost device
    bool buildOomOnce = false;   // MZ_TEST_BUILD_OOM_ONCE=1: the process's first network build runs out of memory
    uint64_t vramBudgetMb = 0;   // MZ_TEST_VRAM_BUDGET_MB=N: the VRAM check sees a budget of N MB
    uint64_t networkMb = 0;      // MZ_TEST_NETWORK_MB=N: the VRAM check estimates every network at N MB
    uint64_t dropSubmitAt = 0;   // MZ_TEST_DROP_SUBMIT_AT=N: Enqueue N skips its Vulkan submit, the game still waits
};

const TestHooks& Hooks()
{
    static const TestHooks hooks = []
    {
        auto on = [](const char* name)
        {
            const char* value = std::getenv(name);
            return value && !std::strcmp(value, "1");
        };
        auto number = [](const char* name) -> uint64_t
        {
            const char* value = std::getenv(name);
            return value ? std::strtoull(value, nullptr, 10) : 0;
        };
        TestHooks h;
        h.failStart = on("MZ_TEST_FAIL_START");
        h.unsupported = on("MZ_TEST_UNSUPPORTED");
        h.slotTimeoutAt = number("MZ_TEST_SLOT_TIMEOUT_AT");
        h.enqueueThrowAt = number("MZ_TEST_ENQUEUE_THROW_AT");
        h.deviceLostAt = number("MZ_TEST_DEVICE_LOST_AT");
        h.buildOomOnce = on("MZ_TEST_BUILD_OOM_ONCE");
        h.vramBudgetMb = number("MZ_TEST_VRAM_BUDGET_MB");
        h.networkMb = number("MZ_TEST_NETWORK_MB");
        h.dropSubmitAt = number("MZ_TEST_DROP_SUBMIT_AT");
        return h;
    }();
    return hooks;
}
std::atomic<bool> g_buildOomInjected { false }; // MZ_TEST_BUILD_OOM_ONCE: once per process

// Adapters the network cannot run on: a Start that threw "[unsupported] ..." (a missing extension or feature, or
// no Vulkan device for the adapter). That does not change while the process lives, so a later PrepareSession on the
// same adapter fails at once, without a VkInstance, and the reason is logged once.
std::atomic<bool> g_unsupported { false };
std::mutex g_unsupportedMutex;
std::vector<std::pair<LUID, std::string>> g_unsupportedAdapters;

std::string UnsupportedReason(LUID luid)
{
    if (!g_unsupported.load(std::memory_order_acquire))
        return {};
    std::lock_guard lock(g_unsupportedMutex);
    for (const auto& [adapter, reason] : g_unsupportedAdapters)
        if (!std::memcmp(&adapter, &luid, sizeof luid))
            return reason;
    return {};
}

void RememberUnsupported(LUID luid, const char* reason)
{
    std::lock_guard lock(g_unsupportedMutex);
    for (const auto& known : g_unsupportedAdapters)
        if (!std::memcmp(&known.first, &luid, sizeof luid))
            return;
    g_unsupportedAdapters.emplace_back(luid, reason);
    g_unsupported.store(true, std::memory_order_release);
    nr::logf("[mochizuki] %s; not retried on this adapter", reason);
}

// The runtime's own lines and the network's, in mochizuki_nr.log beside dlssnr-amd\ (DataDirectory).
void LogLine(const char* line)
{
    if (!g_logging.load(std::memory_order_relaxed)) return;
    static std::mutex mutex;
    static const std::wstring path = DataDirectory() + L"\\mochizuki_nr.log";
    std::lock_guard lock(mutex);
    // Keep the runtime/core diagnostics bounded even during a long build session.
    WIN32_FILE_ATTRIBUTE_DATA attr {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) &&
        (attr.nFileSizeHigh || attr.nFileSizeLow >= 4u * 1024u * 1024u)) return;
    FILE* f = _wfopen(path.c_str(), L"ab");
    if (!f)
        return;
    SYSTEMTIME t {};
    GetLocalTime(&t);
    std::fprintf(f, "%02u:%02u:%02u.%03u %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
    std::fclose(f);
}

// An error, classified for recovery. Only DeviceLost ends the session (Guard); every other kind fails one call, one
// network build (EnsureNetwork's retry policy), the frame's buffers (PrepareFrame) or one frame (Passthrough).
struct NrError : std::runtime_error
{
    enum Kind
    {
        DeviceLost,
        Timeout,
        OutOfMemory,
        Unsupported,
        Invalid,
        Other,
    } kind;
    VkResult vk;
    HRESULT hr;

    NrError(Kind k, const std::string& what, VkResult v = VK_SUCCESS, HRESULT h = S_OK)
        : std::runtime_error(what), kind(k), vk(v), hr(h)
    {
    }
};

const char* KindName(NrError::Kind k)
{
    static const char* const names[] = { "device lost", "timeout", "out of memory", "unsupported", "invalid", "error" };
    return names[k];
}

NrError::Kind KindOf(VkResult r)
{
    switch (r)
    {
    case VK_ERROR_DEVICE_LOST:
        return NrError::DeviceLost;
    case VK_TIMEOUT:
        return NrError::Timeout;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return NrError::OutOfMemory;
    default:
        return NrError::Other;
    }
}

NrError::Kind KindOf(HRESULT hr)
{
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
        return NrError::DeviceLost;
    if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
        return NrError::Timeout;
    if (hr == E_OUTOFMEMORY)
        return NrError::OutOfMemory;
    if (hr == E_INVALIDARG)
        return NrError::Invalid;
    return NrError::Other;
}

// Any exception's kind: an NrError's own; the core's (nrvk::check: "<call>: VkResult=N") by its VkResult;
// bad_alloc and invalid_argument by type. It neither allocates nor throws, so the builder may call it.
NrError::Kind KindOf(const std::exception& e)
{
    if (const auto* error = dynamic_cast<const NrError*>(&e))
        return error->kind;
    if (dynamic_cast<const std::bad_alloc*>(&e))
        return NrError::OutOfMemory;
    if (dynamic_cast<const std::invalid_argument*>(&e))
        return NrError::Invalid;
    const char* what = e.what();
    if (const char* at = std::strstr(what, "VkResult="))
    {
        char* end = nullptr;
        const long r = std::strtol(at + 9, &end, 10);
        if (end != at + 9)
            return KindOf(VkResult(r));
    }
    if (!std::strncmp(what, "[unsupported] ", 14))
        return NrError::Unsupported;
    return NrError::Other;
}

void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char text[160];
        std::snprintf(text, sizeof text, "%s failed, HRESULT 0x%08lX", what, static_cast<unsigned long>(hr));
        throw NrError(KindOf(hr), text, VK_SUCCESS, hr);
    }
}

void VkCheck(VkResult r, const char* what)
{
    if (r != VK_SUCCESS)
        throw NrError(KindOf(r), std::string(what) + " failed, VkResult " + std::to_string(int(r)), r);
}

// This process's use of the adapter's local video memory and its budget, in bytes, through the kernel thunk DXGI's
// QueryVideoMemoryInfo is built on: the same numbers, D3D12 and Vulkan allocations alike. The adapter is opened once,
// in Start (inside the host's device-creation scope), so a check on the render thread creates no DXGI factory or
// adapter, meets none of OptiScaler's DXGI hooks (nor any of its gdi32 ones, which cover D3DKMTQueryAdapterInfo only)
// and the DLL imports no dxgi.dll. VK_EXT_memory_budget cannot stand in: the AMD driver reports this process's usage
// as 0 while the Vulkan device holds no memory of its own, as at the first build.
struct VideoMemory
{
    D3DKMT_HANDLE adapter = 0;
    decltype(&D3DKMTQueryVideoMemoryInfo) query = nullptr;
    decltype(&D3DKMTCloseAdapter) close = nullptr;

    // The adapter behind luid; without it there is no VRAM check.
    void Open(LUID luid)
    {
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi)
            gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); // kept: a system DLL
        if (!gdi)
            return;
        const auto open =
            reinterpret_cast<decltype(&D3DKMTOpenAdapterFromLuid)>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
        query =
            reinterpret_cast<decltype(&D3DKMTQueryVideoMemoryInfo)>(GetProcAddress(gdi, "D3DKMTQueryVideoMemoryInfo"));
        close = reinterpret_cast<decltype(&D3DKMTCloseAdapter)>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
        D3DKMT_OPENADAPTERFROMLUID o {};
        o.AdapterLuid = luid;
        if (open && query && close && open(&o) >= 0)
            adapter = o.hAdapter;
    }

    // False when unknown: no adapter, a failed query, or a budget of 0 (which would refuse everything).
    bool Read(UINT64& usage, UINT64& budget) const
    {
        if (!adapter)
            return false;
        D3DKMT_QUERYVIDEOMEMORYINFO q {}; // hProcess null: this process
        q.hAdapter = adapter;
        q.MemorySegmentGroup = D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
        if (query(&q) < 0 || !q.Budget)
            return false;
        usage = q.CurrentUsage;
        budget = q.Budget;
        return true;
    }

    void Close()
    {
        if (adapter)
        {
            D3DKMT_CLOSEADAPTER c {};
            c.hAdapter = adapter;
            close(&c);
        }
        adapter = 0;
    }
};

// A vkEnumerate* list, whole: call(count, items) is the enumeration. A list that grew between the count and the fill
// (VK_INCOMPLETE) is asked for again, one that shrank is cut to what was written. An error throws VkCheck's plain
// message, never "[unsupported] ...": a failed enumeration (an ICD or layer failing once, out of memory) may pass,
// so only a list that was read in full may show that something is absent.
template <typename T, typename Call> std::vector<T> VkEnumerate(const char* what, Call call)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        uint32_t count = 0;
        VkCheck(call(&count, nullptr), what);
        if (!count)
            return {};
        std::vector<T> items(count);
        const VkResult r = call(&count, items.data());
        if (r == VK_INCOMPLETE)
            continue;
        VkCheck(r, what);
        items.resize(count);
        return items;
    }
    throw std::runtime_error(std::string(what) + " kept returning VK_INCOMPLETE");
}

struct Format
{
    VkFormat vk = VK_FORMAT_UNDEFINED;
    UINT bytes = 0;
};

// The Vulkan format with the same bits as the D3D12 one, so a buffer copy carries the texels unchanged.
Format ColourFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return { VK_FORMAT_R16G16B16A16_SFLOAT, 8 };
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return { VK_FORMAT_R32G32B32A32_SFLOAT, 16 };
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return { VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4 };
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4 };
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return { VK_FORMAT_R8G8B8A8_UNORM, 4 };
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return { VK_FORMAT_B8G8R8A8_UNORM, 4 };
    // Upstream 743326d's further colour formats. The runtime blits them, so PrepareFrame takes them only where this GPU
    // can (Blittable). R9G9B9E5 is RE Engine's upscaler colour (Resident Evil Requiem, Monster Hunter Wilds): three
    // 9-bit mantissas sharing a 5-bit exponent, linear like R11G11B10.
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        return { VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4 };
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return { VK_FORMAT_R16G16B16A16_UNORM, 8 };
    case DXGI_FORMAT_R16G16B16A16_SNORM:
        return { VK_FORMAT_R16G16B16A16_SNORM, 8 };
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        return { VK_FORMAT_R8G8B8A8_SNORM, 4 };
    case DXGI_FORMAT_B5G6R5_UNORM:
        return { VK_FORMAT_R5G6B5_UNORM_PACK16, 2 };
    case DXGI_FORMAT_B5G5R5A1_UNORM:
        return { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2 };
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return { VK_FORMAT_A4R4G4B4_UNORM_PACK16, 2 };
    default:
        return {};
    }
}

// The formats the runtime takes by blit alone (upstream 743326d), which not every GPU can blit.
bool NeedsBlitCheck(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_A4R4G4B4_UNORM_PACK16:
        return true;
    default:
        return false;
    }
}

// The runtime blits the vectors into its own RG32F image, so any float format with at least two channels will do;
// only red and green are read.
Format MotionFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS:
        return { VK_FORMAT_R16G16_SFLOAT, 4 };
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS:
        return { VK_FORMAT_R32G32_SFLOAT, 8 };
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return { VK_FORMAT_R16G16B16A16_SFLOAT, 8 };
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return { VK_FORMAT_R32G32B32A32_SFLOAT, 16 };
    default:
        return {};
    }
}

// Scene-referred float colour: the network sees it through the runtime's sRGB encode.
bool IsLinear(VkFormat f)
{
    return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
           f == VK_FORMAT_R32G32B32A32_SFLOAT || f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
}

// A versioned struct (FrameInfo, the Mochizuki* ones) holds a field when its struct_size covers all of it: older
// callers send shorter structs.
#define MZ_HOLDS(s, Type, field) ((s)->struct_size >= offsetof(Type, field) + sizeof(Type::field))

// A control as the core's validate() accepts it: NaN or infinity is the default, anything else is clamped. The core
// throws on a value out of range, and Guard then fails the session, so nothing from a host reaches it unsanitised.
float San(float v, float lo, float hi, float def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; }

// FrameInfo::model_scale: 0 (a host that does not set it) is 1. Quantised to 0.05, since every change rebuilds the
// network; 1 stays exactly 1, which keeps the model extent the frame's own.
float ModelScale(float v) { return v == 0 ? 1.f : std::round(San(v, 0.25f, 1.f, 1.f) * 20.f) / 20.f; }

// The most passes a frame can ask for (FrameInfo::passes; MochizukiNrControls::pass holds passes 2 and 3), and so the
// most a network is built for: every pass it is built for costs a model-extent RGBA32F history image.
constexpr uint32_t kMaxPasses = 3;

// What SetControls last gave, sanitised. Until it is called, the core's defaults: today's output.
struct SessionControls
{
    nr::Controls model;      // detail_strength, colour_strength and passes come with each frame
    float history = 1.f;     // Runtime::set_history_strength
    float white = 1.f;       // Runtime::set_white_point
    uint32_t linearMode = 0; // MochizukiNrControls::linear_input: 0 auto, 1 on, 2 off
    uint32_t maxPasses = 0;  // 0: the frame's passes
    uint32_t drsMode = 0;    // MochizukiNrControls::drs_mode: 0 exact, 1 auto, 2 always (Session::DrsExtent)
};

void DefaultControls(MochizukiNrControls& c)
{
    const nr::Controls model;
    const nr::PassControls pass;
    c = {};
    c.struct_size = sizeof c;
    c.intensity = model.intensity;
    c.style = uint32_t(model.style);
    c.local_tone = model.local_tone;
    c.local_structure = model.local_structure;
    c.skin_structure = model.skin_structure;
    c.automatic_mask = model.automatic_mask;
    c.max_ratio = model.max_ratio;
    c.history_strength = nr::TemporalConfig {}.history_strength;
    c.white_point = nr::RuntimeConfig {}.white_point;
    c.apply_model = model.apply_model;
    c.drs_mode = 0; // exact, as for a zeroed struct: a host must ask for dynamic resolution (its INI default is auto)
    const nr::Preprocess prep;
    c.preprocess = prep.enabled;
    c.preprocess_exposure = uint32_t(prep.exposure);
    c.preprocess_bias_ev = prep.bias_ev;
    c.preprocess_curve = uint32_t(prep.curve);
    c.preprocess_contrast = prep.contrast;
    c.preprocess_saturation = prep.saturation;
    for (MochizukiNrPassControls& p : c.pass)
    {
        p.style = uint32_t(pass.style);
        p.intensity = pass.intensity;
        p.local_tone = pass.local_tone;
        p.local_structure = pass.local_structure;
        p.skin_structure = pass.skin_structure;
        p.automatic_mask = pass.automatic_mask;
    }
}

// Every field sanitised to what the core accepts. Allocates only for pass overrides.
SessionControls Sanitise(const MochizukiNrControls& c)
{
    SessionControls out;
    nr::Controls& m = out.model;
    m.intensity = San(c.intensity, 0.f, 2.f, 1.f);
    m.style = int(std::min(c.style, 2u));
    m.local_tone = San(c.local_tone, 0.f, 2.f, 1.f);
    m.local_structure = San(c.local_structure, 0.f, 2.f, 1.f);
    m.skin_structure = San(c.skin_structure, -1.f, 2.f, -1.f);
    m.automatic_mask = c.automatic_mask != 0;
    m.max_ratio = San(c.max_ratio, 1.f, 8.f, 2.f);
    m.apply_model = c.apply_model != 0;
    out.history = San(c.history_strength, 0.f, 1.f, 1.f);
    out.white = San(c.white_point, 0.01f, 100.f, 1.f);
    out.linearMode = c.linear_input <= 2 ? c.linear_input : 0;
    out.maxPasses = std::min(c.max_passes, kMaxPasses);
    out.drsMode = c.drs_mode <= 2 ? c.drs_mode : 0; // a mode this runtime does not know runs exact
    nr::Preprocess& prep = m.preprocess;
    prep.enabled = c.preprocess != 0;
    prep.exposure = int(c.preprocess_exposure <= 2 ? c.preprocess_exposure : 1);
    prep.bias_ev = San(c.preprocess_bias_ev, -8.f, 8.f, 0.f);
    prep.curve = int(c.preprocess_curve <= 6 ? c.preprocess_curve : 3);
    prep.contrast = San(c.preprocess_contrast, .5f, 2.f, 1.f);
    prep.saturation = San(c.preprocess_saturation, .05f, 2.f, 1.f);
    // validate() does not look at these, and the core puts them straight into push constants.
    for (size_t k = std::size(c.pass); k-- > 0;)
    {
        const MochizukiNrPassControls& p = c.pass[k];
        if (!p.used)
            continue;
        if (m.per_pass.size() <= k)
            m.per_pass.resize(k + 1);
        nr::PassControls& o = m.per_pass[k];
        o.used = true;
        o.style = int(std::min(p.style, 2u));
        o.intensity = San(p.intensity, 0.f, 2.f, 1.f);
        o.local_tone = San(p.local_tone, 0.f, 2.f, 0.f);
        o.local_structure = San(p.local_structure, 0.f, 2.f, 1.f);
        o.skin_structure = San(p.skin_structure, -1.f, 2.f, -1.f);
        o.automatic_mask = p.automatic_mask != 0;
    }
    return out;
}

struct Vulkan
{
    VkInstance instance {};
    VkPhysicalDevice physical {};
    VkDevice device {};
    VkQueue queue {};      // the frames'
    VkQueue buildQueue {}; // the network builds': a second queue of the family at a lower priority, else `queue`
    uint32_t family = 0;
    bool bufferDeviceAddress = false; // enabled on `device`; the network reads its arena through it where faster
    VkPhysicalDeviceMemoryProperties memory {};
    PFN_vkGetMemoryWin32HandlePropertiesKHR memoryHandleProperties {};
    PFN_vkImportSemaphoreWin32HandleKHR importSemaphore {};
    std::string name;

    // Throws "[unsupported] ..." for what cannot change while the process lives (no Vulkan 1.3 driver, no device
    // for the adapter, a missing extension or feature), anything else for a failure that may pass. What is absent is
    // decided only from lists that were read in full (VkEnumerate); a failed enumeration throws a plain error.
    void Create(LUID luid, const TestHooks& hooks)
    {
        VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "OptiScaler NR";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ii { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ii.pApplicationInfo = &app;
        VkInstance createdInstance {};
        const VkResult created = vkCreateInstance(&ii, nullptr, &createdInstance);
        if (created == VK_ERROR_INCOMPATIBLE_DRIVER)
            throw std::runtime_error("[unsupported] no Vulkan 1.3 driver (vkCreateInstance: "
                                     "VK_ERROR_INCOMPATIBLE_DRIVER)");
        VkCheck(created, "vkCreateInstance");
        instance = createdInstance;
        if (hooks.failStart)
            throw std::runtime_error("MZ_TEST_FAIL_START: Start failed after vkCreateInstance (test hook)");

        const std::vector<VkPhysicalDevice> all =
            VkEnumerate<VkPhysicalDevice>("vkEnumeratePhysicalDevices", [this](uint32_t* n, VkPhysicalDevice* p)
                                          { return vkEnumeratePhysicalDevices(instance, n, p); });
        for (VkPhysicalDevice p : all)
        {
            VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
            VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
            vkGetPhysicalDeviceProperties2(p, &props);
            if (id.deviceLUIDValid && !std::memcmp(id.deviceLUID, &luid, sizeof luid))
            {
                physical = p;
                name = props.properties.deviceName;
                break;
            }
        }
        if (!physical)
        {
            char text[112];
            std::snprintf(text, sizeof text,
                          "[unsupported] no Vulkan device matches the game's D3D12 adapter (%zu Vulkan devices)",
                          all.size());
            throw std::runtime_error(text);
        }
        if (hooks.unsupported)
            throw std::runtime_error("[unsupported] MZ_TEST_UNSUPPORTED: the capability check failed (test hook)");

        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        family = count;
        for (uint32_t i = 0; i < count; ++i)
            if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            {
                family = i;
                break;
            }
        if (family == count)
            throw std::runtime_error("[unsupported] no graphics and compute queue family");

        // What nrvk::Context::create enables, plus the timeline semaphores the game's fences are imported as.
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR
        };
        VkPhysicalDeviceShaderFloat8FeaturesEXT fp8 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT };
        VkPhysicalDeviceVulkan11Features f11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features f13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wml {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR
        };
        coop.cooperativeMatrix = VK_TRUE;
        fp8.shaderFloat8 = fp8.shaderFloat8CooperativeMatrix = VK_TRUE;
        f11.storageBuffer16BitAccess = VK_TRUE;
        f12.storageBuffer8BitAccess = f12.shaderFloat16 = f12.shaderInt8 = f12.vulkanMemoryModel = VK_TRUE;
        f12.timelineSemaphore = VK_TRUE;
        f13.subgroupSizeControl = f13.synchronization2 = VK_TRUE;
        wml.workgroupMemoryExplicitLayout = wml.workgroupMemoryExplicitLayout8BitAccess =
            wml.workgroupMemoryExplicitLayout16BitAccess = VK_TRUE;
        coop.pNext = &fp8;
        fp8.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        f13.pNext = &wml;
        const char* extensions[] = { VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
                                     VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
                                     VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                     VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };

        // The capability precheck: what vkCreateDevice would refuse with the chain above, found and named before it
        // is called. Vulkan 1.3 and the extensions first, since only then may their structures be chained.
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(physical, &props);
        if (props.apiVersion < VK_API_VERSION_1_3)
        {
            char text[96];
            std::snprintf(text, sizeof text, "[unsupported] the device offers Vulkan %u.%u; 1.3 is required",
                          VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
            throw std::runtime_error(text);
        }
        const std::vector<VkExtensionProperties> offered = VkEnumerate<VkExtensionProperties>(
            "vkEnumerateDeviceExtensionProperties", [this](uint32_t* n, VkExtensionProperties* p)
            { return vkEnumerateDeviceExtensionProperties(physical, nullptr, n, p); });
        for (const char* e : extensions)
            if (std::none_of(offered.begin(), offered.end(),
                             [e](const VkExtensionProperties& p) { return !std::strcmp(p.extensionName, e); }))
                throw std::runtime_error(std::string("[unsupported] the driver lacks ") + e);
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR hasCoop {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR
        };
        VkPhysicalDeviceShaderFloat8FeaturesEXT hasFp8 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT };
        VkPhysicalDeviceVulkan11Features has11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features has12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features has13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR hasWml {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR
        };
        hasCoop.pNext = &hasFp8;
        hasFp8.pNext = &has11;
        has11.pNext = &has12;
        has12.pNext = &has13;
        has13.pNext = &hasWml;
        VkPhysicalDeviceFeatures2 has { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &hasCoop };
        vkGetPhysicalDeviceFeatures2(physical, &has);
        const struct
        {
            VkBool32 wanted, offered;
            const char* name;
        } features[] = {
            { coop.cooperativeMatrix, hasCoop.cooperativeMatrix, "cooperativeMatrix" },
            { fp8.shaderFloat8, hasFp8.shaderFloat8, "shaderFloat8" },
            { fp8.shaderFloat8CooperativeMatrix, hasFp8.shaderFloat8CooperativeMatrix,
              "shaderFloat8CooperativeMatrix" },
            { f11.storageBuffer16BitAccess, has11.storageBuffer16BitAccess, "storageBuffer16BitAccess" },
            { f12.storageBuffer8BitAccess, has12.storageBuffer8BitAccess, "storageBuffer8BitAccess" },
            { f12.shaderFloat16, has12.shaderFloat16, "shaderFloat16" },
            { f12.shaderInt8, has12.shaderInt8, "shaderInt8" },
            { f12.vulkanMemoryModel, has12.vulkanMemoryModel, "vulkanMemoryModel" },
            { f12.timelineSemaphore, has12.timelineSemaphore, "timelineSemaphore" },
            { f13.subgroupSizeControl, has13.subgroupSizeControl, "subgroupSizeControl" },
            { f13.synchronization2, has13.synchronization2, "synchronization2" },
            { wml.workgroupMemoryExplicitLayout, hasWml.workgroupMemoryExplicitLayout,
              "workgroupMemoryExplicitLayout" },
            { wml.workgroupMemoryExplicitLayout8BitAccess, hasWml.workgroupMemoryExplicitLayout8BitAccess,
              "workgroupMemoryExplicitLayout8BitAccess" },
            { wml.workgroupMemoryExplicitLayout16BitAccess, hasWml.workgroupMemoryExplicitLayout16BitAccess,
              "workgroupMemoryExplicitLayout16BitAccess" },
        };
        for (const auto& f : features)
            if (f.wanted && !f.offered)
                throw std::runtime_error(std::string("[unsupported] the driver lacks the Vulkan feature ") + f.name);
        // Optional: without it the network takes its descriptor reads.
        bufferDeviceAddress = has12.bufferDeviceAddress == VK_TRUE;
        f12.bufferDeviceAddress = bufferDeviceAddress ? VK_TRUE : VK_FALSE;
        // MZ_PROBE_PIPELINE_BINARY=1 only (mz_interpose.h): two more extensions, and their features after wml.
        std::vector<const char*> enabled(std::begin(extensions), std::end(extensions));
        mzi::BinaryProbe probe;
        wml.pNext = probe.Prepare(physical, offered, enabled, wml.pNext);

        // A second queue, when the family has one, takes the network builds (the core submits only while it is
        // constructed), so a build never holds the frames' queue; the frames keep the higher priority.
        const float priorities[] = { 1.0f, 0.5f };
        VkDeviceQueueCreateInfo qi { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qi.queueFamilyIndex = family;
        qi.queueCount = std::min<uint32_t>(families[family].queueCount, uint32_t(std::size(priorities)));
        qi.pQueuePriorities = priorities;
        VkDeviceCreateInfo di { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &coop };
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = uint32_t(enabled.size());
        di.ppEnabledExtensionNames = enabled.data();
        VkDevice createdDevice {};
        VkCheck(vkCreateDevice(physical, &di, nullptr, &createdDevice), "vkCreateDevice");
        device = createdDevice;
        probe.Report(device, physical);
        vkGetDeviceQueue(device, family, 0, &queue);
        buildQueue = queue;
        if (qi.queueCount > 1)
            vkGetDeviceQueue(device, family, 1, &buildQueue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        memoryHandleProperties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR"));
        importSemaphore = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR"));
        if (!memoryHandleProperties || !importSemaphore)
            throw std::runtime_error("the Win32 external memory entry points are missing");
    }

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const
    {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
                return i;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if (bits & (1u << i))
                return i;
        throw std::runtime_error("no memory type for an image or buffer");
    }

    void Destroy()
    {
        if (device)
            vkDestroyDevice(device, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
        device = {};
        instance = {};
    }
};

// A D3D12 buffer the Vulkan device reads and writes through an imported handle.
struct SharedBuffer
{
    ID3D12Resource* resource {};
    HANDLE handle {};
    VkBuffer buffer {};
    VkDeviceMemory memory {};

    void Create(ID3D12Device* d, Vulkan& vk, UINT64 bytes)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        // Never below 16 MB. In Cyberpunk 2077 the driver refused the handle (E_INVALIDARG) of buffers of 1.9 to 4.2
        // MB, for seconds to minutes, and never of those of 7.4 MB and more.
        rd.Width = std::max((bytes + 65535) & ~UINT64(65535), UINT64(16) << 20);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         IID_PPV_ARGS(&resource)),
              "shared buffer");
        if (const HRESULT hr = d->CreateSharedHandle(resource, nullptr, GENERIC_ALL, nullptr, &handle); FAILED(hr))
        {
            // Seen in a game only (E_INVALIDARG); these say which of its causes it was.
            D3D12_HEAP_PROPERTIES got {};
            D3D12_HEAP_FLAGS flags {};
            const HRESULT query = resource->GetHeapProperties(&got, &flags);
            nr::logf("[mochizuki] shared buffer handle failed (0x%08lX): %llu bytes, heap type %d flags 0x%X (query "
                     "0x%08lX), device 0x%08lX, %d other shared buffers alive",
                     static_cast<unsigned long>(hr), static_cast<unsigned long long>(rd.Width), int(got.Type),
                     unsigned(flags), static_cast<unsigned long>(query),
                     static_cast<unsigned long>(d->GetDeviceRemovedReason()), alive.load());
            Check(hr, "shared buffer handle");
        }
        ++alive;

        VkExternalMemoryBufferCreateInfo external { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external };
        bi.size = rd.Width;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkCheck(vkCreateBuffer(vk.device, &bi, nullptr, &buffer), "vkCreateBuffer (shared)");
        VkMemoryRequirements req {};
        vkGetBufferMemoryRequirements(vk.device, buffer, &req);
        VkMemoryWin32HandlePropertiesKHR props { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
        VkCheck(vk.memoryHandleProperties(vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, &props),
                "vkGetMemoryWin32HandlePropertiesKHR");
        VkMemoryDedicatedAllocateInfo dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.buffer = buffer;
        VkImportMemoryWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated };
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import.handle = handle;
        VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import };
        ai.allocationSize = std::max<UINT64>(req.size, d->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes);
        ai.memoryTypeIndex =
            vk.MemoryType(req.memoryTypeBits & props.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkCheck(vkAllocateMemory(vk.device, &ai, nullptr, &memory), "import of a D3D12 buffer");
        VkCheck(vkBindBufferMemory(vk.device, buffer, memory, 0), "vkBindBufferMemory (shared)");
    }

    void Release(VkDevice device)
    {
        if (buffer)
            vkDestroyBuffer(device, buffer, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        if (handle)
        {
            CloseHandle(handle);
            --alive;
        }
        if (resource)
            resource->Release();
        *this = {};
    }

    static inline std::atomic<int> alive {};
};

// A D3D12 fence of the game's device that the Vulkan device imports as a timeline semaphore.
struct SharedFence
{
    ID3D12Fence* fence {};
    HANDLE handle {};
    VkSemaphore semaphore {};

    void Create(ID3D12Device* d, Vulkan& vk, const char* name)
    {
        char what[64];
        std::snprintf(what, sizeof what, "shared fence (%s)", name);
        Check(d->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)), what);
        Check(d->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &handle), what);
        VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type };
        VkCheck(vkCreateSemaphore(vk.device, &si, nullptr, &semaphore), "vkCreateSemaphore (timeline)");
        VkImportSemaphoreWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
        import.semaphore = semaphore;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        import.handle = handle;
        std::snprintf(what, sizeof what, "import of the D3D12 fence (%s)", name);
        VkCheck(vk.importSemaphore(vk.device, &import), what);
    }

    // The semaphore only with its device.
    void Release(VkDevice device)
    {
        if (semaphore && device)
            vkDestroySemaphore(device, semaphore, nullptr);
        if (handle)
            CloseHandle(handle);
        if (fence)
            fence->Release();
        *this = {};
    }
};

// A COM object's identity, AddRef'd: its IUnknown, or the object itself if it has none.
IUnknown* Identity(IUnknown* object)
{
    IUnknown* id = nullptr;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&id))) || !id)
    {
        object->AddRef();
        id = object;
    }
    return id;
}

// A game queue frames come from, and its COM identity, both AddRef'd.
struct Producer
{
    ID3D12CommandQueue* queue {};
    IUnknown* id {};

    static Producer Of(ID3D12CommandQueue* q)
    {
        q->AddRef();
        return { q, Identity(q) };
    }

    Producer Copy() const
    {
        queue->AddRef();
        id->AddRef();
        return *this;
    }

    void Release()
    {
        if (queue)
            queue->Release();
        if (id)
            id->Release();
        *this = {};
    }
};

// The frames handed to the Vulkan side, oldest first: each one's produced value (toVk), which its Vulkan submit waits
// for, and its finished value (fromVk), which the game queue waits for. Submitted adds, the watchdog drops what fromVk
// has passed. When full it forgets the oldest, long finished by then: the slots let no more than kSlots network
// submits and kFallbacks passthroughs be in flight.
struct Awaited
{
    struct Frame
    {
        UINT64 produced = 0, finished = 0;
    };
    std::mutex mutex;
    Frame frames[64];
    uint32_t first = 0, count = 0;

    void Add(UINT64 produced, UINT64 finished)
    {
        std::lock_guard lock(mutex);
        if (count == std::size(frames))
        {
            first = (first + 1) % std::size(frames);
            --count;
        }
        frames[(first + count++) % std::size(frames)] = { produced, finished };
    }

    // The oldest frame the Vulkan side has not finished, fromVk being at `done`; false when there is none.
    bool Oldest(UINT64 done, Frame& oldest)
    {
        std::lock_guard lock(mutex);
        while (count && frames[first].finished <= done)
        {
            first = (first + 1) % std::size(frames);
            --count;
        }
        if (count)
            oldest = frames[first];
        return count != 0;
    }
};

// Every image of ours: copied into and out of, and sampled, which lets the core read the frame and the vectors in place.
constexpr VkImageUsageFlags kImageUsage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

struct Image
{
    VkImage image {};
    VkDeviceMemory memory {};

    void Create(Vulkan& vk, VkFormat format, uint32_t w, uint32_t h)
    {
        VkImageCreateInfo ci { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = { w, h, 1 };
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = kImageUsage;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkCheck(vkCreateImage(vk.device, &ci, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements req {};
        vkGetImageMemoryRequirements(vk.device, image, &req);
        VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = vk.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkCheck(vkAllocateMemory(vk.device, &ai, nullptr, &memory), "vkAllocateMemory (image)");
        VkCheck(vkBindImageMemory(vk.device, image, memory, 0), "vkBindImageMemory");
    }

    void Release(VkDevice device)
    {
        if (image)
            vkDestroyImage(device, image, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        *this = {};
    }
};

void ImageBarrier(VkCommandBuffer c, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage,
                  VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// A transfer's writes to `image` (GENERAL) made visible to the next transfer.
void TransferBarrier(VkCommandBuffer c, VkImage image)
{
    ImageBarrier(c, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
}

// The rectangle [s0, s1) of `src` onto [d0, d1) of `dst`, both in GENERAL (they may be one image).
void Blit(VkCommandBuffer c, VkImage src, VkImage dst, VkOffset2D s0, VkOffset2D s1, VkOffset2D d0, VkOffset2D d1,
          VkFilter filter)
{
    VkImageBlit b {};
    b.srcSubresource = b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    b.srcOffsets[0] = { s0.x, s0.y, 0 };
    b.srcOffsets[1] = { s1.x, s1.y, 1 };
    b.dstOffsets[0] = { d0.x, d0.y, 0 };
    b.dstOffsets[1] = { d1.x, d1.y, 1 };
    vkCmdBlitImage(c, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &b, filter);
}

// The part of `image` in [0, W) x [0, H) past its valid w x h, filled as the image's own edge is past its border: the
// right strip repeats column w - 1, then the bottom strip, corner included, repeats row h - 1. Exact texel copies, each
// doubling the band that repeats the edge (about log2 of the strip's width of them): a NEAREST blit stretching the
// one-texel edge over the strip is not exact, since near the strip's far end its sampling can land on the texel past
// the edge (a 1706 to 2560 strip did, on an RX 9070 XT with driver 26.8.1: WORK/S5). The image is in GENERAL with its
// last transfer writes visible to transfers (TransferBarrier); the strips' writes are left for the caller's next
// barrier.
void Pad(VkCommandBuffer c, VkImage image, uint32_t w, uint32_t h, uint32_t W, uint32_t H)
{
    bool first = true;
    auto copy = [&](VkOffset3D from, VkOffset3D to, uint32_t cw, uint32_t ch)
    {
        if (!first)
            TransferBarrier(c, image); // each copy reads what the last one wrote
        first = false;
        VkImageCopy r {};
        r.srcSubresource = r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.srcOffset = from;
        r.dstOffset = to;
        r.extent = { cw, ch, 1 };
        vkCmdCopyImage(c, image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    };
    // Columns (rows) [edge, edge + n) hold the edge; the first m of them are copied to edge + n.
    for (uint32_t n = 1, m = 0; w - 1 + n < W; n += m)
    {
        m = std::min(n, W - (w - 1 + n));
        copy({ int32_t(w - 1), 0, 0 }, { int32_t(w - 1 + n), 0, 0 }, m, h);
    }
    for (uint32_t n = 1, m = 0; h - 1 + n < H; n += m)
    {
        m = std::min(n, H - (h - 1 + n));
        copy({ 0, int32_t(h - 1), 0 }, { 0, int32_t(h - 1 + n), 0 }, W, m);
    }
}

void Transition(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
    c->ResourceBarrier(1, &b);
}

// A texture's top-left w x h rows, packed the way both APIs copy buffers. With dynamic resolution a frame copies only
// its subrect, the top-left of this, at the same pitch.
struct Footprint
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT d3d {};
    VkBufferImageCopy vk {};
    UINT64 bytes = 0;

    Footprint() = default;
    Footprint(DXGI_FORMAT format, UINT texelBytes, UINT w, UINT h)
    {
        const UINT pitch =
            (w * texelBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~UINT(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        d3d.Footprint = { format, w, h, 1, pitch };
        vk.bufferRowLength = pitch / texelBytes;
        vk.bufferImageHeight = h;
        vk.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        vk.imageExtent = { w, h, 1 };
        bytes = UINT64(pitch) * h;
    }
};

// The texture's top-left w x h into the buffer laid out as fp.
void CopyToBuffer(ID3D12GraphicsCommandList* c, ID3D12Resource* texture, D3D12_RESOURCE_STATES state,
                  ID3D12Resource* buffer, const Footprint& fp, UINT w, UINT h)
{
    Transition(c, texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(c, buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst { buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
    dst.PlacedFootprint = fp.d3d;
    D3D12_TEXTURE_COPY_LOCATION src { texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
    const D3D12_BOX box { 0, 0, 0, w, h, 1 };
    c->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    Transition(c, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Transition(c, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
}

struct Job
{
    // How the frame's motion vectors reach the network: as uploaded (their extent is the frame's, or the whole frame
    // is the network's); with dynamic resolution, uploaded into the bucket-sized image and padded (they cover the
    // subrect), or rescaled onto the subrect there first (another extent).
    enum MotionPath : uint8_t
    {
        Uploaded,
        Padded,
        Rescaled,
    };
    uint32_t state = LMXXF_NR_JOB_NONE;
    bool enqueued = false; // EnqueueHip ran for it: a second call does nothing
    ID3D12Resource* colour {};
    D3D12_RESOURCE_STATES colourState {};
    ID3D12Resource* motion {};
    D3D12_RESOURCE_STATES motionState {};
    // The frame's valid extents, copied from the top-left of its colour and motion: the geometry's own, except with
    // dynamic resolution.
    UINT width = 0, height = 0, motionWidth = 0, motionHeight = 0;
    MotionPath motionPath = Uploaded;
    float motionScaleX = 0, motionScaleY = 0;
    bool reset = false;
    nr::Controls controls;            // the frame's, sanitised: validate() cannot refuse them
    float history = 1.f, white = 1.f; // set on the runtime before the frame is recorded
};

// The handle PrepareFrame hands out for job generation gen: odd, so never null and never a pointer.
void* JobHandle(uint64_t gen) { return reinterpret_cast<void*>(uintptr_t((gen << 1) | 1)); }

// Everything sized by the frame; the frame's buffers are made again when any of it changes. The extent the network and
// the colour buffers are built for (width, height), the colour allocation, and the motion buffer's extent: in exact,
// the frame's render subrect and the motion's valid extent; with dynamic resolution (bucketed), the bucket and the
// motion allocation, so that a subrect change is no geometry change (the frame's valid extents are the job's).
struct Geometry
{
    UINT width = 0, height = 0;
    DXGI_FORMAT colourFormat = DXGI_FORMAT_UNKNOWN;
    UINT64 colourWidth = 0;
    UINT colourHeight = 0;
    DXGI_FORMAT motionFormat = DXGI_FORMAT_UNKNOWN;
    UINT motionWidth = 0, motionHeight = 0;
    bool bucketed = false; // the motion buffer is the allocation's, and a bucket-sized motion image is made too
    bool operator==(const Geometry&) const = default;
};

// What a network is built for: the frame extent (the bucket, with dynamic resolution), colour format, model scale
// (quantised), pass count and colour encoding.
struct NetworkKey
{
    UINT width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    float scale = 1.f;
    uint32_t maxPasses = 1;
    bool linear = false;
    bool prep = false; // RuntimeConfig::preprocess
    bool operator==(const NetworkKey&) const = default;
};

// Super Resolution reads this texture after CopyTextureRegion fills it. It does
// not need UAV access; avoid imposing typed UAV support on RGB9E5/sRGB inputs.
D3D12_RESOURCE_DESC ResultDesc(ID3D12Resource* colour)
{
    D3D12_RESOURCE_DESC td = colour->GetDesc();
    td.MipLevels = 1;
    td.DepthOrArraySize = 1;
    td.SampleDesc = { 1, 0 };
    td.Alignment = 0;
    td.Flags = D3D12_RESOURCE_FLAG_NONE;
    return td;
}

bool SameDesc(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
{
    return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width && a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality &&
           a.Layout == b.Layout && a.Flags == b.Flags;
}

// The frame's buffers for one geometry (Session::MakeBuffers): the shared buffers the Vulkan device imports, its
// images, and Super Resolution's texture. Whoever holds a set releases it (Release), once no GPU work uses it.
struct FrameBuffers
{
    std::shared_ptr<FrameBuffers> allocation;
    Geometry geometry;
    D3D12_RESOURCE_DESC resultDesc {};
    Footprint colourFootprint, motionFootprint;
    VkFormat colourVk = VK_FORMAT_UNDEFINED, motionVk = VK_FORMAT_UNDEFINED;
    SharedBuffer input, output, motionBuffer;
    Image colourImage, motionImage;
    Image motionScaled; // bucketed geometry with motion: the vectors laid over the subrect, at the bucket's extent
    ID3D12Resource* result {};

    void Release(VkDevice device)
    {
        if (allocation) { *this = {}; return; }
        if (device)
        {
            input.Release(device);
            output.Release(device);
            motionBuffer.Release(device);
            colourImage.Release(device);
            motionImage.Release(device);
            motionScaled.Release(device);
        }
        if (result)
            result->Release();
        result = nullptr;
    }
};

constexpr uint32_t kSlots = 3;
constexpr uint32_t kGpuSamples = 120;    // the frames GetStatus's network median and p95 cover
constexpr uint32_t kHistorySamples = 64; // the frames GetInfo's history_consumed_pct covers, one bit each
// The wait for an earlier frame's slot. Past it the GPU is taken to be stuck: the frame passes through, and after
// kSlotTimeoutsLost such waits in a row the device counts as lost.
constexpr uint64_t kSlotWaitNs = 250'000'000;
constexpr uint32_t kSlotTimeoutsLost = 3;
constexpr uint32_t kFallbacks = 2; // passthrough command buffers, each with its fence
// A drain's wait for the Vulkan side, once the game's queues are idle: fromVk, then the submits' fences, each this
// long at most.
constexpr uint32_t kDrainWaitMs = 2'000;
// Destroy during a network build: the build is told to stop and waited for this long; past it, Destroy returns and the
// build's thread ends the session (Session::Abandon).
constexpr uint32_t kAbandonWaitMs = 2'000;
// The watchdog reads fromVk this often. A frame the game produced that the Vulkan side has not finished through
// kStallMs of reads in a row (above the 2 s TdrDelay, so a TDR comes first) counts as a lost device.
constexpr uint32_t kWatchMs = 250;
constexpr uint32_t kStallMs = 5'000;
// Out of memory: a network build or the frame's buffers are tried again after these, then no more for that key.
constexpr ULONGLONG kOomRetryMs[] = { 5'000, 30'000, 120'000 };
// The frame's buffers after the driver refused a shared handle (E_INVALIDARG; SharedBuffer's 16 MB floor keeps it
// away in Cyberpunk 2077): sooner and more often than after running out of memory.
constexpr ULONGLONG kInvalidRetryMs[] = { 1'000, 2'000, 5'000, 10'000, 30'000, 60'000 };
constexpr ULONGLONG kVramRetryMs = 10'000; // the VRAM check, after it refused
// The VRAM check: what a network costs (its weights and fixed buffers, then per model pixel, times a factor for the
// passes it is built for, plus images at the frame's extent when the model is scaled down), what the frame's buffers
// cost per frame pixel, and a margin; allowed while it all fits in this share of the budget. The network's terms are
// this process's VRAM growth while a network was built on an RX 9070 XT, driver 26.8.1, at 720p to 2160p, 1 to 3
// passes and model scale 0.5 to 1 (WORK/S2/fix1/calib): every estimate is 0.5-4.3% above what was measured. The per
// model pixel term adds 16 bytes to that calibration for the second RGBA32F history image a one-pass network keeps.
constexpr UINT64 kNetworkFixedBytes = 160ull << 20;
constexpr UINT64 kModelPixelBytes = 288;
constexpr double kPassFactor[] = { 1.0, 1.2, 1.3 }; // by the passes built for (max_passes), 1 to kMaxPasses
static_assert(std::size(kPassFactor) == kMaxPasses);
constexpr UINT64 kScaledFramePixelBytes = 36;
constexpr UINT64 kFramePixelBytes = 48;
constexpr UINT64 kVramMarginBytes = 64ull << 20;
constexpr double kVramBudgetShare = 0.9;
constexpr uint32_t kErrorsLogged = 10; // a session's first errors logged in full; then one in 1000
// Dynamic resolution (Session::DrsExtent): the bucket's alignment, and how long every frame must stay this far inside
// it on both axes before it shrinks to what those frames needed (a rebuild).
constexpr UINT kBucketAlign = 64;
constexpr UINT kBucketShrinkMargin = 128;
constexpr ULONGLONG kBucketShrinkMs = 30'000;

// Held by a network build from the prewarm to the end of the core's constructor (Session::Build), so that builds never
// overlap in the process: the core's build writes process-wide state (the model pack's index, which every weight read
// looks up; the arena probe's swap of the log sink; the tile and shape switches), and so do P3's prewarm and its
// pipeline.cache and manifest files. A build Destroy left running (Session::Abandon) holds it until it returns. Never
// destroyed: such a build may still hold it when the process exits.
std::timed_mutex& CoreBuildMutex()
{
    static auto* mutex = new std::timed_mutex;
    return *mutex;
}

// A step that failed for a key (a network build, the frame's buffers) or that the VRAM check refused: not run again
// for that key before `until`, or ever when `again` is false. Any other key runs at once.
template <class Key> struct Hold
{
    bool active = false, again = false;
    Key key {};
    ULONGLONG until = 0;
    uint32_t oomFailures = 0; // out-of-memory failures in a row for this key
    std::string message;      // what PrepareFrame says while it holds

    bool Holds(const Key& k, ULONGLONG now) const { return active && key == k && (!again || now < until); }

    // The step failed for k: out of memory is tried again after kOomRetryMs, and an invalid argument, when
    // `invalidToo`, after kInvalidRetryMs; then no more for k. Any other kind never for k. The delay in ms, 0 for none.
    ULONGLONG Failed(const Key& k, NrError::Kind kind, const char* text, bool invalidToo = false)
    {
        const bool invalid = invalidToo && kind == NrError::Invalid;
        const uint32_t oom = kind == NrError::OutOfMemory || invalid ? (active && key == k ? oomFailures : 0) + 1 : 0;
        const std::span<const ULONGLONG> retry = invalid ? std::span<const ULONGLONG>(kInvalidRetryMs) : kOomRetryMs;
        const ULONGLONG delay = oom && oom <= retry.size() ? retry[oom - 1] : 0;
        Set(k, delay != 0, delay, text);
        oomFailures = oom;
        return delay;
    }

    // The VRAM check refused k: tried again after kVramRetryMs.
    void Refused(const Key& k, const char* text)
    {
        const uint32_t oom = active && key == k ? oomFailures : 0;
        Set(k, true, kVramRetryMs, text);
        oomFailures = oom;
    }

    void Set(const Key& k, bool retry, ULONGLONG ms, const char* text)
    {
        active = true;
        again = retry;
        key = k;
        until = GetTickCount64() + ms;
        message = text;
    }
};

struct Recording
{
    Job job;
    uint64_t frameId = 0;
    FrameBuffers buffers;
    std::shared_ptr<nr::Runtime> network;
    NetworkKey net;
    Microsoft::WRL::ComPtr<ID3D12Resource> colour, motion;
    Microsoft::WRL::ComPtr<ID3D12Fence> tail;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> executingQueue;
    uint64_t tailValue = 0, executions = 0;
    bool invalidated = false, uncertain = false;
};

struct Session
{
    std::mutex progressMutex;
    MochizukiNrBuildProgress progress {sizeof(MochizukiNrBuildProgress)};
    uint64_t progressLogTick = 0;
    static void ReportProgress(void* context, const char* stage, uint32_t done, uint32_t total) noexcept
    {
        auto& s = *static_cast<Session*>(context);
        const auto now = GetTickCount64();
        std::lock_guard lock(s.progressMutex);
        std::snprintf(s.progress.stage, sizeof(s.progress.stage), "%s", stage);
        s.progress.completed = done; s.progress.total = total;
        s.progress.update_tick = now;
        // Build-only diagnostics; bounded even with many pipeline callbacks.
        if (!s.progressLogTick || now - s.progressLogTick >= 10000 || (total && done == total)) {
            s.progressLogTick = now;
            nr::logf("[mochizuki] build %.1f s: %s (%u/%u)",
                double(now - s.progress.start_tick) / 1000, stage, done, total);
        }
    }
    std::wstring assets;
    std::shared_ptr<FrameBuffers> allocation;
    std::unordered_map<void*, std::unique_ptr<Recording>> recordings;
    Microsoft::WRL::ComPtr<ID3D12Fence> previousTail;
    uint64_t previousTailValue = 0;
    void* executing = nullptr;
    bool uncertainExecution = false;
    // History belongs to actual execution order, not PrepareFrame order.
    std::weak_ptr<nr::Runtime> historyNetwork;
    uint64_t historyFrame = 0, historyTick = 0;
    UINT historyWidth = 0, historyHeight = 0;
    int historyPasses = 0;
    bool historyMotion = false;
    ID3D12Device* device {};
    ID3D12CommandQueue* queue {};
    Vulkan vk;
    // The frame's two sync points, one per direction, each with its own counter: the producer queue signals the
    // frame's produced value into toVk, which its Vulkan submit waits for; the submit signals the frame's finished
    // value into fromVk, which the producer queue waits for. Only the Vulkan queue signals fromVk (and Lose and
    // Rerelease, from the CPU, to its maximum), so no game queue can move it backwards, and by default WDDM forces it
    // to its maximum when the Vulkan device is reset, which frees every game queue waiting on it. Game queues signal
    // toVk one frame after another (Follow), and nothing signals it from the CPU: the Vulkan side never runs a frame
    // the game has not produced.
    SharedFence toVk, fromVk;
    UINT64 producedValue = 0, finishedValue = 0; // the last values handed out, under submitMutex
    UINT64 lastSignalled = 0; // the last produced value a game queue signalled into toVk, under submitMutex
    // The last frame handed to the Vulkan side (Submitted), whose finished value fromVk will reach whether or not the
    // game queue's wait went through: Follow's and Drain's waits, the watchdog's log.
    std::atomic<UINT64> lastProduced { 0 }, lastFinished { 0 };
    // The game queues frames come from (EnqueueHip's), under submitMutex: the last frame's, and every one since the
    // last drain, which Drain and ~Session drain besides `queue`.
    Producer lastProducer;
    bool lastProducerListed = false; // it is in producers
    std::vector<Producer> producers;
    uint64_t queueSwitches = 0;
    // The watchdog (Watch), from the first Enqueue until ~Session, and the frames it watches.
    std::thread watchdog;
    bool watchdogStarted = false; // under submitMutex
    std::mutex watchMutex;
    std::condition_variable watchWake;
    bool watchStop = false;
    Awaited awaited;
    std::atomic<bool> released { false }; // Lose's signal of fromVk to its maximum was made
    uint32_t rereleases = 0;              // Rerelease's signals: the watchdog's, then ~Session's
    VkCommandPool pool {};
    VkCommandBuffer commands[kSlots] {};
    VkFence done[kSlots] {};
    bool inFlight[kSlots] {};
    uint32_t nextSlot = 0;
    uint32_t slotTimeouts = 0; // timed-out slot waits in a row
    uint64_t enqueueCalls = 0; // Enqueue calls, for the MZ_TEST_* hooks
    // The passthrough (Passthrough): input copied to output, each command buffer with its own fence, recorded for the
    // current geometry when it is next used (one of a frame before the geometry changed may still run until then).
    VkCommandBuffer fallbackCommands[kFallbacks] {};
    VkFence fallbackDone[kFallbacks] {};
    bool fallbackInFlight[kFallbacks] {};
    bool fallbackCurrent[kFallbacks] {}; // recorded for the current geometry's buffers
    uint32_t nextFallback = 0;
    uint64_t passthroughs = 0; // frames passed through
    // The frames' VkQueue, the slots and fallbacks, and the network the frames record with; a build holds it only when
    // the builds have no queue of their own (vk.buildQueue is vk.queue).
    std::mutex submitMutex;
    // Sticky, and set only with deviceLost (Lose): every other error leaves the session usable.
    std::atomic<bool> failed { false }, deviceLost { false };
    // Set before the first build starts and before every submit. Until then no GPU queue has seen anything of this
    // session, so it can always be freed.
    std::atomic<bool> gpuUsed { false };
    bool zeroOutputFallback = false; // Create's LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK: frames pass through
    TestHooks hooks;
    std::atomic<uint32_t> errors { 0 }; // failed calls and passed-through frames, for the log's rate limit

    // The network for one extent, colour format, model scale, pass count and colour encoding, built off the render
    // thread: `runtime` is the one the frames record with (for `net`), `built` a finished build's (for buildKey) until
    // EnsureNetwork installs it. With a queue of their own for the builds, a network for another model scale, pass
    // count or colour encoding is built while the frames keep the old one (keep serving); runtime and net change under
    // submitMutex. Only EnsureNetwork and ~Session join the builder, under buildMutex once buildDone is set.
    std::shared_ptr<nr::Runtime> runtime;
    NetworkKey net, buildKey;
    std::thread builder;
    std::mutex buildMutex;
    std::condition_variable buildEnded; // buildDone was set
    bool building = false, buildDone = false;
    // Destroy during a build (Abandon): the build stops between two pipelines, and when it has not ended within
    // kAbandonWaitMs its thread owns the session and ends it (EndAbandoned).
    std::atomic<bool> abandon { false };
    bool builderOwnsSession = false; // under buildMutex
    std::chrono::steady_clock::time_point abandonedAt;
    std::shared_ptr<nr::Runtime> built;
    char buildError[1024] {}; // a fixed buffer, so the builder cannot throw on its way to buildDone
    NrError::Kind buildErrorKind = NrError::Other;
    double buildSeconds = 0;
    // Networks the frames no longer use (RetireNetwork), each with the finished value of the last frame handed to the
    // Vulkan side while the frames had it: the releaser thread frees them, oldest first, once fromVk is there
    // (Releaser). What is left when the session ends goes after its drain, or leaks with a lost device.
    struct Retired
    {
        std::shared_ptr<nr::Runtime> network;
        UINT64 finished = 0;
    };
    std::mutex releaseMutex;
    std::condition_variable releaseWake; // a network retired, one freed, releaseStop, or the releaser ended
    std::vector<Retired> retired;        // under releaseMutex
    size_t releasing = 0;                // retired networks not freed yet, under releaseMutex
    bool releaserEnded = false;          // under releaseMutex
    std::atomic<bool> releaseStop { false };
    std::thread releaser; // started by ReleaseLater (render thread), joined by ~Session
    // Why the network for a key, or the buffers for a geometry, are not being made; render thread only.
    Hold<NetworkKey> buildHold;
    Hold<Geometry> geometryHold;
    // The VRAM check's view of the adapter (opened in Start), and what a network was measured to cost: this process's
    // growth while measuredKey was built, or what releasing it gave back, whichever is more. Render thread only.
    VideoMemory vram;
    NetworkKey measuredKey;
    UINT64 measuredBytes = 0;
    UINT64 buildBaseUsage = 0; // this process's VRAM use when the running build was admitted; 0 when unknown
    UINT64 buildGrowth = 0;    // what the finished build's network added to it; 0 when unknown

    // runtime->last_gpu_ms() of the last kGpuSamples frames, for GetStatus. Pushed under submitMutex as well.
    std::mutex statsMutex;
    float gpuMs[kGpuSamples] {};
    uint32_t gpuMsCount = 0, gpuMsNext = 0;
    nr::Runtime* statsNetwork = nullptr;
    uint64_t statsSerial = 0, gpuSamples = 0, gpuTick = 0;

    // SetControls' last word, which PrepareFrame copies into every job. The mutex only guards a host that sets them
    // from another thread than the render thread.
    std::mutex controlsMutex;
    SessionControls controls;
    uint32_t lastPasses = 0; // the previous frame's pass count; a change drops the history

    // What GetInfo reports: atomics and a copy of the last error, never the runtime, since it may ask from any thread.
    std::atomic<bool> infoBuilding { false };
    std::atomic<uint32_t> infoModelWidth { 0 }, infoModelHeight { 0 }, infoFrameWidth { 0 }, infoFrameHeight { 0 },
        infoMaxPasses { 0 }, infoPasses { 1 }, motionRefused { 0 }, dispatches { 0 };
    std::atomic<float> infoBuildSeconds { 0 };
    std::atomic<float> infoPrepEv { NAN };
    std::atomic<float> infoWhite { 1.f };
    // MochizukiNrSetExposure's last word: the game's exposure, 0 for none.
    std::atomic<float> gameExposure { 0.f };
    // Set by the first frame that asks for the preprocess: from then on every network is built able to run it.
    bool prepWanted = false;
    // One bit a frame, newest lowest: its history was consumed. Written under submitMutex only.
    std::atomic<uint64_t> historyBits { 0 };
    std::atomic<uint32_t> historyCount { 0 };
    std::mutex errorMutex;
    char lastError[256] {};

    Geometry geometry;
    Footprint colourFootprint, motionFootprint;
    VkFormat colourVk = VK_FORMAT_UNDEFINED, motionVk = VK_FORMAT_UNDEFINED;
    SharedBuffer input, output, motionBuffer;
    Image colourImage, motionImage, motionScaled;
    ID3D12Resource* result {}; // what Super Resolution gets in place of the game's colour

    // Dynamic resolution (DrsExtent), render thread only: the bucket, and what it has seen.
    struct Bucket
    {
        UINT64 allocWidth = 0; // the colour allocation auto engaged in
        UINT allocHeight = 0;
        DXGI_FORMAT allocFormat = DXGI_FORMAT_UNKNOWN;
        bool engaged = false;             // auto: a subrect smaller than that allocation came
        UINT maxWidth = 0, maxHeight = 0; // the largest subrect seen
        UINT capWidth = 0, capHeight = 0; // the largest allocation seen (always)
        // Bucketed frames of this allocation, all kBucketShrinkMargin inside the bucket, since lowSince (0: the last
        // frame was not one of them, or the allocation changed), and the largest subrect among them.
        ULONGLONG lowSince = 0;
        UINT lowWidth = 0, lowHeight = 0;
        UINT loggedWidth = 0, loggedHeight = 0; // the bucket the log last showed
    } bucket;
    // Colour and motion formats the bucket may blit (Blittable), render thread only.
    std::vector<std::pair<VkFormat, VkFormatFeatureFlags>> formatFeatures;
    bool drsRefusedLogged = false;
    // The last frame's valid extent: a change restarts the history. The atomics are GetStatus's copy.
    UINT validWidth = 0, validHeight = 0;
    std::atomic<uint32_t> infoValidWidth { 0 }, infoValidHeight { 0 };

    // The frame's job, one at a time, handed out as JobHandle(jobGen). jobMutex guards its state and the job
    // bookkeeping below against a Retire, Cancel or Poll from another thread; the rest of the job is the host's to
    // order (PrepareFrame, Record*, EnqueueHip).
    std::mutex jobMutex;
    Job job;
    uint64_t jobGen = 0;
    uint64_t retiredGen = 0; // the last job Retire acted on
    // A job cancelled after its inputs, or outputs, were recorded: the game may still run that list, which copies
    // into the geometry's buffers and result. Until a later job was retired and a drain passed, ReleaseGeometry
    // buries them in the graveyard (D3D12 references only) instead of freeing them.
    bool orphaned = false;
    uint64_t orphanGen = 0;
    std::vector<ID3D12Resource*> graveyard;
    uint64_t graveyardGen = 0; // the newest orphaned job whose buffers are in it
    // The frame's buffers from before the frame changed (RetireGeometry), render thread only. They go once the Vulkan
    // side has finished the last frame handed to it (fromVk at `finished`) and the one producer queue has signalled
    // the next frame's produced value (toVk at `nextProduced`), which it does only after running what it had of the
    // frames before: FreeBuried, from PrepareFrame, or at once after a drain.
    struct Buried
    {
        FrameBuffers buffers;
        UINT64 finished = 0, nextProduced = 0;
    };
    std::vector<Buried> buried;
    // The next frame's buffers, made on the builder thread beside a network built for a frame that had none at its
    // extent and format (builtBuffers), then kept for that frame (prepared, render thread): it takes them over instead
    // of spending the time on the render thread (MakeGeometry).
    std::unique_ptr<FrameBuffers> builtBuffers, prepared;

    uint64_t lastFrameId = 0;
    ULONGLONG lastFrameTick = 0;
    std::atomic<bool> resetPending { true };
    std::atomic<uint64_t> frames { 0 };

    // A game queue idle within 30 s: S_OK, HRESULT_FROM_WIN32(ERROR_TIMEOUT) or what failed.
    HRESULT DrainD3D12(ID3D12CommandQueue* q)
    {
        ID3D12Fence* f = nullptr;
        HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
        if (FAILED(hr))
            return hr;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ev)
        {
            f->Release();
            return E_OUTOFMEMORY;
        }
        hr = q->Signal(f, 1);
        if (SUCCEEDED(hr))
            hr = f->SetEventOnCompletion(1, ev);
        if (SUCCEEDED(hr) && WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0)
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        CloseHandle(ev);
        f->Release();
        return hr;
    }

    // Game queues, AddRef'd while it lives.
    struct Queues
    {
        std::vector<ID3D12CommandQueue*> list;

        Queues() = default;
        Queues(const Queues&) = delete;
        Queues& operator=(const Queues&) = delete;
        ~Queues()
        {
            for (ID3D12CommandQueue* q : list)
                q->Release();
        }
    };

    // The session's queue and every queue a frame came from since the last drain, each idle within 30 s: S_OK or
    // the first failure. `queues` gets them all. Never throws (~Session calls it).
    HRESULT DrainGameQueues(Queues& queues)
    {
        {
            std::lock_guard lock(submitMutex);
            try
            {
                queues.list.reserve(producers.size() + 1);
            }
            catch (const std::bad_alloc&)
            {
                return E_OUTOFMEMORY;
            }
            queues.list.push_back(queue);
            for (const Producer& p : producers)
                queues.list.push_back(p.queue);
            for (ID3D12CommandQueue* q : queues.list)
                q->AddRef();
        }
        for (ID3D12CommandQueue* q : queues.list)
            if (const HRESULT hr = DrainD3D12(q); FAILED(hr))
                return hr;
        return S_OK;
    }

    // `queues` were drained: nothing of this session's is left on them. Under submitMutex.
    void ForgetProducers(const Queues& queues)
    {
        size_t kept = 0;
        for (size_t i = 0; i < producers.size(); ++i)
        {
            if (std::find(queues.list.begin(), queues.list.end(), producers[i].queue) != queues.list.end())
                producers[i].Release();
            else
                producers[kept++] = producers[i];
        }
        if (kept != producers.size())
            lastProducerListed = false;
        producers.resize(kept);
    }

    // Both sides idle, so sized resources and the network can go: every game queue a frame came from, then the
    // Vulkan side within kDrainWaitMs a wait (VulkanIdle), whose timeout fails the call only. A game queue that does
    // not drain within 30 s is taken as lost, as before errors were classified: otherwise the render thread would wait
    // on it again every frame.
    void Drain()
    {
        Queues queues;
        if (const HRESULT hr = DrainGameQueues(queues); FAILED(hr))
        {
            char text[96];
            std::snprintf(text, sizeof text, "drain of the game queue failed, HRESULT 0x%08lX",
                          static_cast<unsigned long>(hr));
            throw NrError(hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ? NrError::DeviceLost : KindOf(hr), text, VK_SUCCESS,
                          hr);
        }
        if (vk.device)
        {
            std::lock_guard lock(submitMutex);
            if (const VkResult r = VulkanIdle(kDrainWaitMs); r != VK_SUCCESS)
            {
                char text[128];
                std::snprintf(text, sizeof text, "drain of the Vulkan side failed: %s (VkResult %d)",
                              r == VK_TIMEOUT ? "its work did not finish within 2 s" : "a wait failed", int(r));
                throw NrError(KindOf(r), text, r);
            }
            // Every slot's fence is now signalled or unused; the next submit on a slot needs it unsignalled.
            VkCheck(vkResetFences(vk.device, kSlots, done), "vkResetFences");
            VkCheck(vkResetFences(vk.device, kFallbacks, fallbackDone), "vkResetFences (passthrough)");
            for (bool& f : inFlight)
                f = false;
            for (bool& f : fallbackInFlight)
                f = false;
            ForgetProducers(queues);
        }
        // A job retired after the orphaned one means the game has run its lists past it; this drain then saw the
        // orphaned list's copies finish, if the game ran it at all.
        std::vector<ID3D12Resource*> released;
        {
            std::lock_guard lock(jobMutex);
            if (orphaned && retiredGen > orphanGen)
                orphaned = false;
            if (!graveyard.empty() && retiredGen > graveyardGen)
                released.swap(graveyard);
        }
        for (ID3D12Resource* r : released)
            r->Release();
        // Both sides idle: every retired set of the frame's buffers can go too.
        FreeBuried(true);
    }

    void FreeGraveyard()
    {
        for (ID3D12Resource* r : graveyard)
            r->Release();
        graveyard.clear();
    }

    // The retired sets of the frame's buffers the GPU is past (Buried); all of them when `all` (both sides idle).
    void FreeBuried(bool all)
    {
        if (buried.empty())
            return;
        UINT64 produced = UINT64_MAX, finished = UINT64_MAX;
        if (!all)
        {
            // Only while the frames still come from one queue: once another one signals toVk, a value there says
            // nothing of the first one's work (a drain frees them then). Left for a later frame while a build holds
            // submitMutex (a build without a queue of its own).
            std::unique_lock lock(submitMutex, std::try_to_lock);
            if (!lock || producers.size() > 1)
                return;
            produced = toVk.fence->GetCompletedValue();
            // fromVk read through Vulkan: the same value, and it also tells a validation layer that the submits which
            // signalled it are done (it cannot see the D3D12 fence read).
            if (vkGetSemaphoreCounterValue(vk.device, fromVk.semaphore, &finished) != VK_SUCCESS)
                return;
        }
        auto gone = [&](Buried& b)
        {
            if (produced < b.nextProduced || finished < b.finished)
                return false;
            b.buffers.Release(vk.device);
            return true;
        };
        buried.erase(std::remove_if(buried.begin(), buried.end(), gone), buried.end());
    }

    void DestroyFences()
    {
        for (VkFence& f : done)
            if (f)
                vkDestroyFence(vk.device, std::exchange(f, VkFence {}), nullptr);
        for (VkFence& f : fallbackDone)
            if (f)
                vkDestroyFence(vk.device, std::exchange(f, VkFence {}), nullptr);
    }

    // Every submit of this session's that may still run, done within `ms`: its slots' and passthroughs' fences.
    VkResult WaitInFlight(uint32_t ms)
    {
        VkFence pending[kSlots + kFallbacks] {};
        uint32_t n = 0;
        for (uint32_t i = 0; i < kSlots; ++i)
            if (inFlight[i])
                pending[n++] = done[i];
        for (uint32_t i = 0; i < kFallbacks; ++i)
            if (fallbackInFlight[i])
                pending[n++] = fallbackDone[i];
        return n ? vkWaitForFences(vk.device, n, pending, VK_TRUE, uint64_t(ms) * 1'000'000) : VK_SUCCESS;
    }

    // Every frame handed to the Vulkan side done within `ms`: fromVk at the last one's finished value, which its submit
    // signals once its work has run. VK_TIMEOUT when it is not done by then. Under submitMutex.
    VkResult FramesDone(uint32_t ms)
    {
        const UINT64 last = lastFinished.load();
        if (!last)
            return VK_SUCCESS;
        VkSemaphoreWaitInfo wi { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores = &fromVk.semaphore;
        wi.pValues = &last;
        return vkWaitSemaphores(vk.device, &wi, uint64_t(ms) * 1'000'000);
    }

    // The Vulkan side idle, each wait within `ms`: fromVk at the last frame a game queue waits for, then every
    // submit's fence. VK_TIMEOUT when either is not done by then. Under submitMutex, or once nothing else runs.
    VkResult VulkanIdle(uint32_t ms)
    {
        if (const VkResult r = FramesDone(ms); r != VK_SUCCESS)
            return r;
        return WaitInFlight(ms);
    }

    // fromVk at `value`, waited for off the render thread in steps of kWatchMs, with no lock (vkWaitSemaphores
    // synchronises nothing of the session's): VK_SUCCESS once it is there, VK_TIMEOUT once stop() says to give up,
    // VK_ERROR_DEVICE_LOST once the device is lost (Lose puts fromVk at its maximum while the GPU may still use
    // everything, so a lost device is never a success), or the wait's own error.
    template <class Stop> VkResult WaitFinished(UINT64 value, Stop&& stop)
    {
        VkSemaphoreWaitInfo wi { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores = &fromVk.semaphore;
        wi.pValues = &value;
        for (;;)
        {
            if (deviceLost)
                return VK_ERROR_DEVICE_LOST;
            const VkResult r = vkWaitSemaphores(vk.device, &wi, uint64_t(kWatchMs) * 1'000'000);
            if (deviceLost)
                return VK_ERROR_DEVICE_LOST;
            if (r != VK_TIMEOUT)
                return r;
            if (stop())
                return VK_TIMEOUT;
        }
    }

    // `network`, which the frames up to `finished` in fromVk may still use, handed to the releaser (Releaser); without
    // memory for that it leaks rather than be freed under the GPU. Any thread.
    void RetireNetwork(std::shared_ptr<nr::Runtime> network, UINT64 finished) noexcept
    {
        try
        {
            std::lock_guard lock(releaseMutex);
            retired.reserve(retired.size() + 1); // the one step that may throw, before the network moves
            retired.push_back({ std::move(network), finished });
            ++releasing;
        }
        catch (...)
        {
            Quarantine(network);
            return;
        }
        releaseWake.notify_all();
    }

    // The releaser thread (render thread only: ReleaseLater, ReleasesDone), unless it runs already.
    void StartReleaser() noexcept
    {
        if (releaser.joinable())
            return;
        try
        {
            releaser = std::thread([this] { Releaser(); });
        }
        catch (const std::exception& e)
        {
            nr::logf("[mochizuki] the releaser thread could not start (%s): a network the frames no longer use goes "
                     "when the session ends",
                     e.what());
        }
    }

    // A network the frames no longer use goes on the releaser thread, once the Vulkan side has finished the frames
    // that used it: neither that wait nor the release (about 30 ms at 1080p) is the render thread's. Render thread.
    void ReleaseLater(std::shared_ptr<nr::Runtime> network, UINT64 finished) noexcept
    {
        RetireNetwork(std::move(network), finished);
        StartReleaser();
    }

    // The releaser thread: each retired network, oldest first, freed once fromVk has reached its finished value. It
    // ends at ~Session (releaseStop), or when a wait fails or the device is lost; what it has not freed then stays in
    // `retired` for ~Session.
    void Releaser()
    {
        std::unique_lock lock(releaseMutex);
        for (;;)
        {
            releaseWake.wait(lock, [this] { return releaseStop || !retired.empty(); });
            if (releaseStop)
                break;
            const UINT64 finished = retired.front().finished;
            lock.unlock();
            const VkResult r = WaitFinished(finished, [this] { return releaseStop.load(); });
            lock.lock();
            if (r != VK_SUCCESS)
            {
                if (r != VK_TIMEOUT)
                    nr::logf("[mochizuki] a network the frames no longer use is left to the session's end: %s "
                             "(VkResult %d)",
                             r == VK_ERROR_DEVICE_LOST ? "the device is lost" : "the wait for the Vulkan side failed",
                             int(r));
                break;
            }
            std::shared_ptr<nr::Runtime> network = std::move(retired.front().network);
            retired.erase(retired.begin());
            lock.unlock();
            const auto t0 = std::chrono::steady_clock::now();
            network.reset();
            nr::logf("[mochizuki] the old network released in %.1f ms",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            lock.lock();
            --releasing;
            releaseWake.notify_all();
        }
        releaserEnded = true;
        releaseWake.notify_all();
    }

    // Retired networks not freed yet. Render thread.
    bool Releasing()
    {
        std::lock_guard lock(releaseMutex);
        return releasing != 0;
    }

    // Every retired network freed, within `ms` (or none left to the releaser): after a drain, whose Vulkan side is
    // idle, before the VRAM check looks again. Render thread.
    void ReleasesDone(uint32_t ms)
    {
        if (Releasing())
            StartReleaser();
        std::unique_lock lock(releaseMutex);
        releaseWake.wait_for(lock, std::chrono::milliseconds(ms),
                             [this] { return !releasing || releaserEnded || !releaser.joinable(); });
    }

    // The releaser ended; `retired` is ~Session's.
    void StopReleaser()
    {
        {
            std::lock_guard lock(releaseMutex);
            releaseStop = true;
        }
        releaseWake.notify_all();
        if (releaser.joinable())
            releaser.join();
    }

    // The Vulkan side is lost, or stopped finishing frames (the watchdog): the session fails for good, and fromVk goes
    // to its maximum from the CPU, which frees every game queue's wait on it, queued or still to come. The flags and
    // the error come first, so that whoever sees a game queue freed also sees the session failed, and why. The frames
    // it frees get whatever the output buffer holds. A Vulkan submit still queued then may later signal fromVk lower
    // (VUID-VkSubmitInfo-pSignalSemaphores-03242 forbids it, but a queued submit cannot be taken back): Submit makes
    // no new one, and the watchdog and ~Session put fromVk back (Rerelease). Any thread; the first call records and
    // logs.
    void Lose(const char* why)
    {
        if (deviceLost.exchange(true))
        {
            failed = true;
            return;
        }
        failed = true;
        RememberError(why);
        const HRESULT hr = fromVk.fence ? fromVk.fence->Signal(UINT64_MAX) : S_OK;
        released = true;
        nr::logf("[mochizuki] %s; the session is stopped%s", why,
                 !fromVk.fence ? ""
                 : FAILED(hr)  ? " (releasing the game's queues failed)"
                               : " and the game's queues are released");
    }

    // After the release, fromVk back at its maximum when a Vulkan submit queued before it has since signalled a lower
    // value (the D3D12 fence does fall: RX 9070 XT, driver 26.8.1, WORK/S3/fix1). A game queue's wait queued before
    // the release stays satisfied, but one queued after the fall (an Enqueue that raced Lose) would wait for a value
    // the Vulkan side may never reach. The watchdog every kWatchMs, then ~Session once more; they never run together.
    void Rerelease()
    {
        if (!fromVk.fence)
            return;
        const UINT64 value = fromVk.fence->GetCompletedValue();
        if (value == UINT64_MAX)
            return;
        const HRESULT hr = fromVk.fence->Signal(UINT64_MAX);
        const uint32_t n = ++rereleases;
        if (n <= kErrorsLogged || n % 1000 == 0)
            nr::logf("[mochizuki] fromVk fell back to %llu after the release (a Vulkan submit queued before it): it "
                     "is signalled to its maximum again%s (%u times)",
                     static_cast<unsigned long long>(value), FAILED(hr) ? ", which failed" : "", n);
    }

    // Under submitMutex, before the producer's signal. A frame from another queue than the last frame's (by COM
    // identity) first waits there for the last frame's finished value, so its signal cannot land before the last
    // producer's (toVk would go backwards and meet a Vulkan wait early) and the Vulkan side takes the frames in order.
    // A signal no Vulkan submit took (its frame failed after it and was not passed through) has no finished value to
    // wait for: the new queue then also waits for it in toVk. It depends only on work queued earlier. The new queue's
    // first half has already copied its colour, which may race the last frame's network: both frames may look wrong,
    // and the history restarts on the next frame. The queue is also listed for Drain.
    HRESULT Follow(ID3D12CommandQueue* producer)
    {
        if (producer == lastProducer.queue && lastProducerListed)
            return S_OK;
        Producer p = Producer::Of(producer);
        const UINT64 last = lastFinished.load(std::memory_order_relaxed);
        const UINT64 stray = lastSignalled > lastProduced.load(std::memory_order_relaxed) ? lastSignalled : 0;
        if (lastProducer.id && p.id != lastProducer.id && (last || stray))
        {
            HRESULT hr = last ? producer->Wait(fromVk.fence, last) : S_OK;
            if (SUCCEEDED(hr) && stray)
                hr = producer->Wait(toVk.fence, stray);
            if (FAILED(hr))
            {
                p.Release();
                return hr;
            }
            resetPending = true;
            // Logged like NoteError: a game may switch every frame.
            const uint64_t n = ++queueSwitches;
            if (n <= kErrorsLogged || n % 1000 == 0)
                nr::logf("[mochizuki] a frame from queue %p after one from queue %p (switch %llu of the session%s): it "
                         "waits there for the last frame; this frame may look wrong, and the history restarts",
                         static_cast<void*>(producer), static_cast<void*>(lastProducer.queue),
                         static_cast<unsigned long long>(n),
                         n == kErrorsLogged ? "; from now on one in 1000 is logged" : "");
        }
        if (std::none_of(producers.begin(), producers.end(), [&](const Producer& q) { return q.id == p.id; }))
            producers.push_back(p.Copy());
        lastProducer.Release();
        lastProducer = p;
        lastProducerListed = true;
        return S_OK;
    }

    // A frame is on the Vulkan queue: fromVk will reach its finished value whether or not the game queue's wait for it
    // goes through. The watchdog, Drain and Follow learn of it. Under submitMutex.
    void Submitted(UINT64 produced, UINT64 finished)
    {
        awaited.Add(produced, finished);
        lastProduced.store(produced, std::memory_order_relaxed);
        lastFinished.store(finished, std::memory_order_release);
    }

    // A frame's Vulkan submit: c after the frame's produced value in toVk, then its finished value into fromVk; then
    // Submitted. None once the session is lost (Lose), whose release a later signal would undo.
    VkResult Submit(VkCommandBuffer c, UINT64 produced, UINT64 finished, VkFence signal)
    {
        if (deviceLost)
            return VK_ERROR_DEVICE_LOST;
        VkTimelineSemaphoreSubmitInfo values { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        values.waitSemaphoreValueCount = 1;
        values.pWaitSemaphoreValues = &produced;
        values.signalSemaphoreValueCount = 1;
        values.pSignalSemaphoreValues = &finished;
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO, &values };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &toVk.semaphore;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &c;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &fromVk.semaphore;
        gpuUsed.store(true, std::memory_order_relaxed);
        const VkResult r = vkQueueSubmit(vk.queue, 1, &si, signal);
        if (r == VK_SUCCESS)
            Submitted(produced, finished);
        return r;
    }

    // The watchdog, from the first Enqueue (under submitMutex). Without it (no thread) only the slot waits see a
    // stuck Vulkan side.
    void StartWatchdog()
    {
        if (watchdogStarted)
            return;
        watchdogStarted = true;
        try
        {
            watchdog = std::thread([this] { Watch(); });
        }
        catch (const std::exception& e)
        {
            nr::logf("[mochizuki] the watchdog thread could not start: %s", e.what());
        }
    }

    void StopWatchdog()
    {
        if (!watchdog.joinable())
            return;
        {
            std::lock_guard lock(watchMutex);
            watchStop = true;
        }
        watchWake.notify_all();
        watchdog.join();
    }

    // Every kWatchMs, fromVk's value. A lost device, or the oldest frame handed to the Vulkan side (Awaited) not
    // finished for kStallMs of reads in a row although the game produced it (toVk), means the Vulkan side will not
    // finish it: Lose frees the game's queues. Counted in reads, not in wall time, so a system sleep cannot fake a
    // stall. Once released, it keeps fromVk at its maximum (Rerelease) until ~Session. It never takes submitMutex,
    // which a build without a queue of its own holds for seconds.
    void Watch()
    {
        constexpr uint32_t kStallReads = kStallMs / kWatchMs + 1; // kStallMs between the first and the last
        UINT64 stalledOn = 0;                                     // the stalled frame's finished value
        uint32_t stalledReads = 0;
        std::unique_lock lock(watchMutex);
        while (!watchWake.wait_for(lock, std::chrono::milliseconds(kWatchMs), [this] { return watchStop; }))
        {
            if (released)
            {
                Rerelease();
                continue;
            }
            if (deviceLost)
                continue; // Lose is on its way to the release
            UINT64 reached = 0;
            const VkResult r = vkGetSemaphoreCounterValue(vk.device, fromVk.semaphore, &reached);
            if (r == VK_ERROR_DEVICE_LOST)
            {
                Lose("watchdog: the Vulkan device was lost");
                continue;
            }
            // The frame the Vulkan side has to finish next, if the game has produced it.
            Awaited::Frame oldest;
            const bool waiting = r == VK_SUCCESS && awaited.Oldest(reached, oldest);
            const UINT64 produced = waiting ? toVk.fence->GetCompletedValue() : 0;
            if (!waiting || produced < oldest.produced)
            {
                stalledReads = 0;
                continue;
            }
            if (oldest.finished != stalledOn)
            {
                stalledOn = oldest.finished;
                stalledReads = 0;
            }
            if (++stalledReads < kStallReads)
                continue;
            char text[256];
            std::snprintf(text, sizeof text,
                          "watchdog: the Vulkan side has not finished a frame the game produced %u s ago (fromVk at "
                          "%llu, the frame's value %llu; toVk at %llu, the last frame's value %llu): the device is "
                          "taken to be lost",
                          kStallMs / 1000, static_cast<unsigned long long>(reached),
                          static_cast<unsigned long long>(oldest.finished), static_cast<unsigned long long>(produced),
                          static_cast<unsigned long long>(lastProduced.load()));
            Lose(text);
        }
    }

    // A session error that leaves the session usable: kept for GetInfo and logged, the first kErrorsLogged in full
    // and then one in 1000, since it may come back every frame.
    void NoteError(const char* text)
    {
        RememberError(text);
        const uint32_t n = ++errors;
        if (n < kErrorsLogged)
            nr::logf("[mochizuki] %s", text);
        else if (n == kErrorsLogged || n % 1000 == 0)
            nr::logf("[mochizuki] %s (error %u of the session; from now on one in 1000 is logged)", text, n);
    }

    // A Start that threw has submitted nothing: free what it made, so that a later PrepareSession can try again.
    void AbandonStart()
    {
        if (vk.device)
        {
            DestroyFences();
            if (pool)
                vkDestroyCommandPool(vk.device, pool, nullptr); // frees commands[] and fallbackCommands[] with it
        }
        std::fill(std::begin(commands), std::end(commands), VkCommandBuffer {});
        std::fill(std::begin(fallbackCommands), std::end(fallbackCommands), VkCommandBuffer {});
        pool = {};
        toVk.Release(vk.device);
        fromVk.Release(vk.device);
        vk.Destroy();
        vk = {};
        vram.Close();
    }

    // Median and p95 of the network's GPU time over the last kGpuSamples frames; zero before the first sample.
    void NetworkMs(float& median, float& p95)
    {
        float sorted[kGpuSamples];
        uint32_t n = 0;
        {
            std::lock_guard lock(statsMutex);
            n = gpuMsCount;
            std::copy_n(gpuMs, n, sorted);
        }
        median = p95 = 0;
        if (!n)
            return;
        std::sort(sorted, sorted + n);
        median = n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
        p95 = sorted[(n * 95 + 99) / 100 - 1];
    }

    // The last error, for GetInfo: GetLastError's is the calling thread's.
    void RememberError(const char* text)
    {
        std::lock_guard lock(errorMutex);
        std::strncpy(lastError, text ? text : "", sizeof lastError - 1);
        lastError[sizeof lastError - 1] = 0;
    }

    // The network's time as GetStatus showed it, plus the core's running average, which only the log keeps.
    void LogNetworkTime(const char* when)
    {
        float median = 0, p95 = 0;
        NetworkMs(median, p95);
        nr::logf("[mochizuki] %s: network %ux%u %.2f ms (p95 %.2f, average %.2f), %llu frames in the session", when,
                 net.width, net.height, median, p95, runtime->average_gpu_ms(),
                 static_cast<unsigned long long>(frames.load()));
    }

    // An orphaned job's list may still copy into the geometry's D3D12 buffers, whenever the game runs it: they go to
    // the graveyard as well, AddRef'd, whatever happens to the geometry's own references.
    void KeepOrphaned()
    {
        std::lock_guard lock(jobMutex);
        if (!orphaned)
            return;
        for (ID3D12Resource* r : { input.resource, motionBuffer.resource, output.resource, result })
            if (r)
            {
                r->AddRef();
                graveyard.push_back(r);
            }
        graveyardGen = orphanGen;
        orphaned = false;
    }

    // The geometry's buffers after the frame changed, without waiting for the game's queue: with one producer queue
    // since the last drain, into `buried` until the GPU is past them (FreeBuried); with several, whose order among
    // each other is not known, after a drain, as before. Render thread.
    void RetireGeometry()
    {
        size_t queues = 0;
        UINT64 next = 0;
        {
            std::lock_guard lock(submitMutex);
            queues = producers.size();
            next = producedValue + 1;
        }
        if (!geometry.width || queues > 1)
        {
            if (geometry.width)
                Drain();
            ReleaseGeometry();
            return;
        }
        buried.reserve(buried.size() + 1); // the one step that may throw, before anything moves
        KeepOrphaned();
        buried.push_back({ TakeBuffers(), lastFinished.load(), next });
    }

    // Everything the geometry holds; nothing the GPU may still use (the callers drain first, or never used it). An
    // orphaned job's list may still copy into the D3D12 side, which is buried instead.
    void ReleaseGeometry()
    {
        KeepOrphaned();
        TakeBuffers().Release(vk.device);
    }

    // The geometry's buffers, taken from the frames: no geometry is left.
    FrameBuffers TakeBuffers()
    {
        FrameBuffers b;
        b.allocation = std::move(allocation);
        b.geometry = std::exchange(geometry, {});
        b.colourFootprint = colourFootprint;
        b.motionFootprint = motionFootprint;
        b.colourVk = colourVk;
        b.motionVk = motionVk;
        b.input = std::exchange(input, {});
        b.output = std::exchange(output, {});
        b.motionBuffer = std::exchange(motionBuffer, {});
        b.colourImage = std::exchange(colourImage, {});
        b.motionImage = std::exchange(motionImage, {});
        b.motionScaled = std::exchange(motionScaled, {});
        b.result = std::exchange(result, nullptr);
        std::fill(std::begin(fallbackCurrent), std::end(fallbackCurrent), false);
        return b;
    }

    // The estimated VRAM cost of the network for `key` (MZ_TEST_NETWORK_MB replaces it).
    UINT64 NetworkEstimate(const NetworkKey& key) const
    {
        if (hooks.networkMb)
            return hooks.networkMb << 20;
        // The model extent as the core pads it, for the activations.
        auto model = [&](UINT v)
        {
            const UINT scaled = key.scale >= 1.f ? v : std::max(8u, UINT(double(v) * key.scale + 0.5));
            return UINT64((scaled + 63) / 64 * 64);
        };
        const double passes = kPassFactor[std::clamp(key.maxPasses, 1u, kMaxPasses) - 1];
        const UINT64 activations = UINT64(double(kModelPixelBytes * model(key.width) * model(key.height)) * passes);
        const UINT64 scaled = key.scale < 1.f ? kScaledFramePixelBytes * key.width * key.height : 0;
        return kNetworkFixedBytes + activations + scaled;
    }

    // What the network for `key` costs in VRAM: the estimate, or what building or releasing it measured when that was
    // more.
    UINT64 NetworkBytes(const NetworkKey& key) const
    {
        const UINT64 estimate = NetworkEstimate(key);
        return measuredKey == key ? std::max(estimate, measuredBytes) : estimate;
    }

    // The frame's buffers for g; a bucketed geometry with motion adds its bucket-sized motion image.
    static UINT64 FrameBytes(const Geometry& g)
    {
        const UINT64 scaled = g.bucketed ? MotionFormat(g.motionFormat).bytes : 0;
        return (kFramePixelBytes + scaled) * g.width * g.height;
    }

    // A network measured: `bytes` is what it took, or gave back, in this process's VRAM.
    void Measured(const NetworkKey& key, UINT64 bytes)
    {
        measuredBytes = measuredKey == key ? std::max(measuredBytes, bytes) : bytes;
        measuredKey = key;
    }

    // The VRAM check before a network build or the frame's buffers (`what`): this process's use plus `need` must stay
    // within kVramBudgetShare of its budget. Says why not in `why`; lets it through when the budget is unknown. `seen`
    // gets the use it saw, 0 when unknown.
    bool VramAdmits(UINT64 need, UINT w, UINT h, const char* what, std::string& why, UINT64* seen = nullptr)
    {
        UINT64 usage = 0, budget = 0;
        const bool known = vram.Read(usage, budget);
        if (seen)
            *seen = known ? usage : 0;
        if (hooks.vramBudgetMb)
            budget = hooks.vramBudgetMb << 20;
        else if (!known)
        {
            static std::atomic<bool> logged { false };
            if (!logged.exchange(true))
                nr::logf("[mochizuki] the VRAM budget is unknown (D3DKMTQueryVideoMemoryInfo failed or reported no "
                         "budget): no VRAM check");
            return true;
        }
        const UINT64 allowed = UINT64(double(budget) * kVramBudgetShare);
        const UINT64 available = allowed > usage ? allowed - usage : 0;
        auto mb = [](UINT64 bytes) { return static_cast<unsigned long long>(bytes >> 20); };
        if (need <= available)
        {
            nr::logf(
                "[mochizuki] VRAM for %s at %ux%u: about %llu MB needed, %llu MB available (this process uses %llu "
                "MB of a %llu MB budget, %.0f%% of it usable)",
                what, w, h, mb(need), mb(available), mb(usage), mb(budget), kVramBudgetShare * 100);
            return true;
        }
        char text[160];
        std::snprintf(text, sizeof text, "insufficient VRAM for %ux%u (need %llu MB, free %llu MB of a %llu MB budget)",
                      w, h, mb(need), mb(available), mb(budget));
        why = text;
        return false;
    }

    // Passthrough k's copy, input to output, for the current geometry. Under submitMutex (it shares the pool), once
    // k's last submit is done: Passthrough records it when k is first used after the geometry changed, since a
    // passthrough of a frame before the change may run until then (no drain precedes a new geometry).
    void RecordFallback(uint32_t k)
    {
        VkCommandBuffer c = fallbackCommands[k];
        const VkBufferCopy region { 0, 0, colourFootprint.bytes };
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkCheck(vkResetCommandBuffer(c, 0), "vkResetCommandBuffer (passthrough)");
        VkCheck(vkBeginCommandBuffer(c, &bi), "vkBeginCommandBuffer (passthrough)");
        vkCmdCopyBuffer(c, input.buffer, output.buffer, 1, &region);
        VkCheck(vkEndCommandBuffer(c), "vkEndCommandBuffer (passthrough)");
        fallbackCurrent[k] = true;
    }

    // The frame's buffers for g, and Super Resolution's texture as `desc`, into b; on a throw b holds what was made.
    // Any thread: the device calls are free-threaded, and nothing the frames use is touched.
    void MakeBuffers(FrameBuffers& b, const Geometry& g, const D3D12_RESOURCE_DESC& desc)
    {
        const Format cf = ColourFormat(g.colourFormat), mf = MotionFormat(g.motionFormat);
        b.geometry = g;
        b.resultDesc = desc;
        b.colourFootprint = Footprint(g.colourFormat, cf.bytes, g.width, g.height);
        b.colourVk = cf.vk;
        b.input.Create(device, vk, b.colourFootprint.bytes);
        b.output.Create(device, vk, b.colourFootprint.bytes);
        b.colourImage.Create(vk, cf.vk, g.width, g.height);
        b.motionVk = mf.vk;
        if (mf.vk)
        {
            b.motionFootprint = Footprint(g.motionFormat, mf.bytes, g.motionWidth, g.motionHeight);
            b.motionBuffer.Create(device, vk, b.motionFootprint.bytes);
            b.motionImage.Create(vk, mf.vk, g.motionWidth, g.motionHeight);
            if (g.bucketed)
                b.motionScaled.Create(vk, mf.vk, g.width, g.height);
        }
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                              IID_PPV_ARGS(&b.result)),
              "output texture");
        // The active geometry and all closed recordings share this one allocation.
        auto* raw = new FrameBuffers(b);
        b.allocation = std::shared_ptr<FrameBuffers>(raw, [dev = vk.device](FrameBuffers* p) {
            p->Release(dev); delete p;
        });
    }

    // The frames take b over (there is no geometry now); its passthrough is recorded when first used. Render thread.
    void UseBuffers(FrameBuffers& b)
    {
        allocation = std::move(b.allocation);
        colourFootprint = b.colourFootprint;
        motionFootprint = b.motionFootprint;
        colourVk = b.colourVk;
        motionVk = b.motionVk;
        input = std::exchange(b.input, {});
        output = std::exchange(b.output, {});
        motionBuffer = std::exchange(b.motionBuffer, {});
        colourImage = std::exchange(b.colourImage, {});
        motionImage = std::exchange(b.motionImage, {});
        motionScaled = std::exchange(b.motionScaled, {});
        result = std::exchange(b.result, nullptr);
        geometry = b.geometry;
    }

    void CreateGeometry(const Geometry& g, ID3D12Resource* colour)
    {
        FrameBuffers b;
        try
        {
            MakeBuffers(b, g, ResultDesc(colour));
        }
        catch (...)
        {
            b.Release(vk.device);
            throw;
        }
        UseBuffers(b);
    }

    void Start()
    {
        vk.Create(device->GetAdapterLuid(), hooks);
        toVk.Create(device, vk, "toVk");
        fromVk.Create(device, vk, "fromVk");
        VkCommandPoolCreateInfo pi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.queueFamilyIndex = vk.family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCheck(vkCreateCommandPool(vk.device, &pi, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = kSlots;
        VkCheck(vkAllocateCommandBuffers(vk.device, &ai, commands), "vkAllocateCommandBuffers");
        ai.commandBufferCount = kFallbacks;
        VkCheck(vkAllocateCommandBuffers(vk.device, &ai, fallbackCommands), "vkAllocateCommandBuffers (passthrough)");
        VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        for (VkFence& f : done)
            VkCheck(vkCreateFence(vk.device, &fi, nullptr, &f), "vkCreateFence");
        for (VkFence& f : fallbackDone)
            VkCheck(vkCreateFence(vk.device, &fi, nullptr, &f), "vkCreateFence (passthrough)");
        vram.Open(device->GetAdapterLuid());
        nr::logf("[mochizuki] Vulkan device %s, queue family %u%s", vk.name.c_str(), vk.family,
                 vk.buildQueue != vk.queue ? ", a second queue for the network builds" : "");
        if (DataDirectory() != DllDirectory())
            nr::logf("[mochizuki] loaded from %s; dlssnr-amd is read from %s", RootUtf8(DllDirectory()).c_str(),
                     RootUtf8(DataDirectory()).c_str());
    }

    // A build ended without a network. Its key is not built again, except after running out of memory (kOomRetryMs).
    // The network the frames kept during the build goes (on the releaser thread), and the frame's buffers with it: a
    // retry then has the VRAM to itself, and meanwhile no frame runs, as when nothing was kept. A build that lost the
    // device fails the session like any other lost device.
    void BuildFailed()
    {
        if (buildErrorKind == NrError::DeviceLost)
            throw NrError(NrError::DeviceLost, buildError);
        const ULONGLONG delay = buildHold.Failed(buildKey, buildErrorKind, buildError);
        RememberError(buildError);
        if (delay)
            nr::logf("[mochizuki] out of memory: the network at %ux%u is built again in %llu s", buildKey.width,
                     buildKey.height, static_cast<unsigned long long>(delay / 1000));
        else
            nr::logf("[mochizuki] the network at %ux%u is not built again until the frame's extent, format or settings "
                     "change",
                     buildKey.width, buildKey.height);
        if (runtime)
        {
            std::shared_ptr<nr::Runtime> old;
            UINT64 finished = 0;
            {
                std::lock_guard lock(submitMutex);
                old = TakeNetwork("the network the frames kept goes with the failed build", finished);
            }
            ReleaseLater(std::move(old), finished);
        }
        RetireGeometry();
    }

    // The frames' network, taken from them: no frame records with it after, and `finished` gets the finished value of
    // the last frame handed to the Vulkan side, the last one that may use it. It may go once fromVk is there, which the
    // render thread does not wait for (ReleaseLater, or the builder). `why` goes to the log with the network's time.
    // Under submitMutex.
    std::shared_ptr<nr::Runtime> TakeNetwork(const char* why, UINT64& finished)
    {
        LogNetworkTime(why);
        finished = lastFinished.load();
        return std::move(runtime);
    }

    // The runtime serves a frame of `key`'s extent and colour format: all a frame needs of it while a build for the
    // new model scale, pass count or colour encoding runs (keep serving).
    bool Serves(const NetworkKey& key) const
    {
        return runtime && net.width == key.width && net.height == key.height && net.format == key.format;
    }

    // A finished build's network (built, for buildKey) becomes the frames'. The one they kept meanwhile is taken in the
    // same hold of submitMutex (TakeNetwork), so no frame records between the two, and goes on the releaser thread once
    // the Vulkan side has finished the frames that used it (ReleaseLater): the render thread waits for neither. The
    // history restarts.
    void Install()
    {
        std::shared_ptr<nr::Runtime> old;
        UINT64 oldFinished = 0;
        {
            std::lock_guard lock(submitMutex);
            if (runtime)
                old = TakeNetwork("replacing the network", oldFinished);
            runtime = std::move(built);
            net = buildKey;
            resetPending = true;
            historyBits = 0;
            historyCount = 0;
        }
        // The buffers made beside it wait for the next frame (MakeGeometry).
        if (prepared)
            prepared->Release(vk.device);
        prepared = std::move(builtBuffers);
        const char* const kept = old ? "; the frames ran on the old one meanwhile" : "";
        if (old)
            ReleaseLater(std::move(old), oldFinished);
        buildHold = {};
        {
            std::lock_guard stats(statsMutex);
            gpuMsCount = gpuMsNext = 0;
        }
        infoFrameWidth = net.width;
        infoFrameHeight = net.height;
        infoModelWidth = runtime->model_width();
        infoModelHeight = runtime->model_height();
        infoMaxPasses = net.maxPasses;
        infoBuildSeconds = float(buildSeconds);
        if (runtime->model_width() != net.width || runtime->model_height() != net.height || net.maxPasses > 1)
            nr::logf("[mochizuki] network ready at %ux%u (model %ux%u, up to %u passes) in %.1f s%s", net.width,
                     net.height, runtime->model_width(), runtime->model_height(), net.maxPasses, buildSeconds, kept);
        else
            nr::logf("[mochizuki] network ready at %ux%u in %.1f s%s", net.width, net.height, buildSeconds, kept);
        if (buildGrowth)
        {
            Measured(net, buildGrowth);
            nr::logf("[mochizuki] VRAM: the network at %ux%u was estimated at %llu MB; this process grew by %llu MB "
                     "while it was built",
                     net.width, net.height, static_cast<unsigned long long>(NetworkEstimate(net) >> 20),
                     static_cast<unsigned long long>(buildGrowth >> 20));
        }
    }

    // True once a network for `key` is ready, or while one for the frame's extent and colour format serves it during
    // the build for new settings (keep serving: the frame runs at the old ones). Otherwise starts its build, unless one
    // is running, the last one for this key failed (buildHold) or the VRAM check refuses it; `why` then says why there
    // is none. g is the frame's geometry, `colour` its colour: buffers made for another one are retired before a build,
    // and the build makes the new ones beside the network.
    bool EnsureNetwork(const NetworkKey& key, const Geometry& g, ID3D12Resource* colour, std::string& why)
    {
        static const char* const kBuilding = "building the network (the first time takes about half a minute)";
        bool finished = false;
        {
            std::lock_guard lock(buildMutex);
            if (buildDone)
            {
                builder.join();
                buildDone = building = false;
                infoBuilding = false;
                finished = true;
            }
            if (building)
            {
                if (Serves(key))
                    return true;
                why = kBuilding;
                return false;
            }
        }
        if (finished && !built)
            BuildFailed();
        if (built)
            Install();
        if (runtime && net == key)
            return true;
        if (buildHold.Holds(key, GetTickCount64()))
        {
            why = buildHold.message;
            return false;
        }
        // With a queue of their own for the builds, the frames keep a network for their extent and format meanwhile.
        const bool serve = Serves(key) && vk.buildQueue != vk.queue;
        // Buffers for another frame go now, unless the frames keep running on them.
        if (!serve && geometry.width && !(geometry == g))
            RetireGeometry();
        // The network, and the frame's buffers unless they are made already, beside what the frames still hold.
        bool admitted = VramAdmits(NetworkBytes(key) + (geometry == g ? 0 : FrameBytes(g)) + kVramMarginBytes,
                                   key.width, key.height, "the network", why, &buildBaseUsage);
        if (!admitted && (runtime || !buried.empty() || Releasing()))
        {
            // Not beside them: what the frames held goes first, after a drain of both sides, and the networks retired
            // before with it (the drain lets the releaser free them), then the check again.
            Drain();
            if (runtime)
            {
                std::lock_guard lock(submitMutex);
                LogNetworkTime("replacing the network");
                runtime.reset();
            }
            if (geometry.width && !(geometry == g))
                ReleaseGeometry();
            ReleasesDone(kDrainWaitMs);
            admitted = VramAdmits(NetworkBytes(key) + (geometry == g ? 0 : FrameBytes(g)) + kVramMarginBytes, key.width,
                                  key.height, "the network", why, &buildBaseUsage);
        }
        if (!admitted)
        {
            buildHold.Refused(key, why.c_str());
            NoteError(why.c_str());
            return false;
        }
        const bool keep = serve && runtime;
        // The first ready frame's buffers, when the frame has none, are made beside the network (the frames that keep
        // the old one make their own at once).
        std::unique_ptr<FrameBuffers> buffers;
        if (!keep && !(geometry == g))
        {
            buffers = std::make_unique<FrameBuffers>();
            buffers->geometry = g;
            buffers->resultDesc = ResultDesc(colour);
        }
        // A network the frames do not keep goes on the builder thread before the build, once the Vulkan side has
        // finished the frames that used it: the render thread waits for neither that nor its release.
        std::shared_ptr<nr::Runtime> old;
        UINT64 oldFinished = 0;
        if (runtime && !keep)
        {
            std::lock_guard lock(submitMutex);
            old = TakeNetwork("replacing the network", oldFinished);
        }
        buildKey = key;
        StartBuild(std::move(old), oldFinished, std::move(buffers), keep);
        why = kBuilding;
        return keep;
    }

    // The build for buildKey, on the builder thread (Build). `old`, a network no frame uses any more, is freed there
    // first, once fromVk has reached `oldFinished`; `buffers`, when set, asks for the next frame's buffers too
    // (geometry and resultDesc). `keep`: the frames keep their network meanwhile. On a throw, `old` goes to the
    // releaser (ReleaseLater), never here.
    void StartBuild(std::shared_ptr<nr::Runtime> old, UINT64 oldFinished, std::unique_ptr<FrameBuffers> buffers,
                    bool keep)
    {
        if (prepared)
        {
            prepared->Release(vk.device); // made for a frame that did not come
            prepared.reset();
        }
        bool started = false;
        try
        {
            const NetworkKey& key = buildKey;
            nr::RuntimeConfig config;
            config.root = RootUtf8(assets);
            config.width = key.width;
            config.height = key.height;
            config.colour_format = key.format;
            config.linear_input = key.linear;
            config.model_scale = key.scale;
            config.max_passes = key.maxPasses;
            config.preprocess = key.prep;
            // The linear path's frame is a proxy made with the soft knee, which the preprocess undoes first.
            config.preprocess_unknee = key.linear;
            nr::TemporalConfig temporal;
            temporal.enable = true;
            nr::HostDevice host;
            host.instance = vk.instance;
            host.physical = vk.physical;
            host.device = vk.device;
            host.queue = vk.buildQueue;
            host.queue_family = vk.family;
            host.buffer_device_address = vk.bufferDeviceAddress;
            char options[64] = {};
            if (key.scale != 1.f || key.maxPasses > 1)
                std::snprintf(options, sizeof options, ", model scale %.2f, up to %u passes", key.scale, key.maxPasses);
            nr::logf("[mochizuki] building the network at %ux%u, VkFormat %d%s%s%s", key.width, key.height,
                     int(key.format), config.linear_input ? ", linear colour" : "", options,
                     keep ? "; the frames keep the current one meanwhile" : "");
            UseUtf8Paths();
            {
                std::lock_guard lock(buildMutex);
                building = true;
            }
            started = true;
            infoBuilding = true;
            gpuUsed = true;
            // The builder takes `old` over only once its thread exists: a failed start must not free it here.
            const auto handed = old;
            builder = std::thread(
                [this, host, config, temporal, handed, oldFinished, buffers = std::move(buffers)]() mutable
                {
                    Build(host, config, temporal, handed, oldFinished,
                          std::move(buffers));
                });
            old.reset();
        }
        catch (...)
        {
            if (started)
            {
                {
                    std::lock_guard lock(buildMutex);
                    building = false;
                }
                infoBuilding = false;
            }
            if (old)
                ReleaseLater(std::move(old), oldFinished);
            throw;
        }
    }

    // The builder thread. Nothing may throw out of it, and nothing from the network's end to buildDone: a fixed
    // buffer, no allocation. First the network the frames no longer use (`old`), once the Vulkan side has finished the
    // frames that used it (fromVk at oldFinished), before the new one takes its VRAM; Destroy or a lost device leaves
    // it to the releaser instead. The build waits for any other in the process (CoreBuildMutex) and stops between two
    // pipelines once `abandon` is set (Destroy; P3's prewarm and the core's pipeline loop check it). Then the next
    // frame's buffers, when asked for: making them takes the render thread longer than a frame. When Destroy has given
    // the session up to this thread (Abandon), it ends it.
    void Build(const nr::HostDevice& host, const nr::RuntimeConfig& config, const nr::TemporalConfig& temporal,
               std::shared_ptr<nr::Runtime> old, UINT64 oldFinished, std::unique_ptr<FrameBuffers> buffers)
    {
        const auto t0 = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(progressMutex);
            progress = {sizeof(progress), 1, 0, 0, GetTickCount64(), GetTickCount64()};
            progressLogTick = 0;
        }
        ReportProgress(this, "Waiting for previous GPU work", 0, 0);
        if (old)
        {
            if (WaitFinished(oldFinished, [this] { return abandon.load(); }) == VK_SUCCESS)
            {
                old.reset();
                UINT64 usage = 0, budget = 0;
                if (buildBaseUsage && vram.Read(usage, budget))
                    buildBaseUsage = usage; // the growth is measured from after the release
            }
            else
                RetireNetwork(std::move(old), oldFinished);
        }
        std::shared_ptr<nr::Runtime> made;
        char error[sizeof buildError] {};
        NrError::Kind kind = NrError::Other;
        try
        {
            ReportProgress(this, "Waiting for network builder", 0, 0);
            // One core build at a time in the process, waited for in steps so that Destroy can end the wait.
            std::unique_lock core(CoreBuildMutex(), std::defer_lock);
            while (!core.try_lock_for(std::chrono::milliseconds(kWatchMs)))
                if (abandon)
                    throw std::runtime_error("stopped: the session is being destroyed");
            // Builds and frames on one queue: the frames wait for the build, as they always did.
            std::unique_lock lock(submitMutex, std::defer_lock);
            if (host.queue == vk.queue)
                lock.lock();
            if (hooks.buildOomOnce && !g_buildOomInjected.exchange(true))
                throw std::runtime_error("vkAllocateMemory (MZ_TEST_BUILD_OOM_ONCE test hook): VkResult=-2");
            // The pipelines an earlier build recorded are compiled on several threads first, into the pipeline.cache
            // the core then loads; the capture records this build's for the next one, and Finish saves what the core
            // compiled after its own save (mz_interpose.h).
            mzi::Capture pipelines(host.device, host.physical, assets);
            ReportProgress(this, "Prewarming cached shader descriptions", 0, 0);
            pipelines.Prewarm(&abandon);
            if (abandon)
                throw std::runtime_error("stopped: the session is being destroyed");
            {
                const nr::BuildCancelScope cancel(&abandon);
                const nr::BuildProgressScope report(ReportProgress, this);
                ReportProgress(this, "Preparing network graph", 0, 0);
                made = std::make_shared<nr::Runtime>(host, config, nr::ControlMaskConfig {}, temporal);
            }
            ReportProgress(this, "Saving shader cache", 0, 0);
            pipelines.Finish();
        }
        catch (const std::exception& e)
        {
            kind = KindOf(e);
            std::snprintf(error, sizeof error, "network build failed at %ux%u: %s", config.width, config.height,
                          e.what());
        }
        catch (...)
        {
            std::snprintf(error, sizeof error, "network build failed at %ux%u: an unknown exception", config.width,
                          config.height);
        }
        if (error[0] && abandon)
            nr::logf("[mochizuki] the network build at %ux%u stopped after %.1f s: the session is being destroyed",
                     config.width, config.height,
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        else if (error[0])
            nr::logf("[mochizuki] %s (%s)", error, KindName(kind));
        // The network's own growth, before the buffers add theirs.
        UINT64 usage = 0, budget = 0;
        const UINT64 grown =
            made && buildBaseUsage && vram.Read(usage, budget) && usage > buildBaseUsage ? usage - buildBaseUsage : 0;
        if (buffers && made && !abandon)
        {
            ReportProgress(this, "Preparing shared frame buffers", 0, 0);
            try
            {
                // The VRAM check the frame makes for them (MakeGeometry), now that the network holds its share. When
                // it refuses, the frame's own check does too, and the network goes (ReleaseNetwork).
                const Geometry& g = buffers->geometry;
                std::string why;
                if (VramAdmits(FrameBytes(g) + kVramMarginBytes, g.width, g.height, "the frame's buffers", why))
                    MakeBuffers(*buffers, g, buffers->resultDesc);
                else
                    buffers.reset();
            }
            catch (const std::exception& e)
            {
                buffers->Release(vk.device);
                buffers.reset();
                nr::logf("[mochizuki] the frame's buffers were not made beside the network (%s); the frame makes them",
                         e.what());
            }
            catch (...)
            {
                buffers->Release(vk.device);
                buffers.reset();
            }
        }
        else if (buffers)
            buffers.reset();
        {
            std::lock_guard progressLock(progressMutex);
            progress.active = 0;
        }
        std::unique_lock lock(buildMutex);
        built = std::move(made);
        builtBuffers = std::move(buffers);
        buildGrowth = grown;
        std::memcpy(buildError, error, sizeof error);
        buildErrorKind = kind;
        buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        buildDone = true;
        buildEnded.notify_all();
        if (!builderOwnsSession)
            return;
        lock.unlock();
        EndAbandoned(this);
    }

    // Destroy during a network build: the build is told to stop (between two pipelines) and given kAbandonWaitMs to
    // end. True when it has, or none runs: the caller ends the session, and joins the builder. Otherwise the builder
    // thread is detached and owns the session, which it ends once the build returns (EndAbandoned), and this module
    // is pinned, since that thread runs its code after Destroy returned and the host may unload it. The thread then
    // touches nothing of the host's but the device and queues the session holds references to. Until the build
    // returns it holds CoreBuildMutex: a new session's build waits for it (its frames say "building" meanwhile).
    bool Abandon()
    {
        std::unique_lock lock(buildMutex);
        if (!building || buildDone)
            return true;
        abandon = true;
        abandonedAt = std::chrono::steady_clock::now();
        if (buildEnded.wait_for(lock, std::chrono::milliseconds(kAbandonWaitMs), [this] { return buildDone; }))
            return true;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&MochizukiNrGetApi), &self);
        builder.detach();
        builderOwnsSession = true;
        nr::logf("[mochizuki] Destroy during the network build: it did not stop within %u s; its thread ends the "
                 "session once it returns",
                 kAbandonWaitMs / 1000);
        return false;
    }

    // The builder thread of a session Destroy gave up waiting for (Abandon), once the build returned.
    static void EndAbandoned(Session* s)
    {
        const double late = std::chrono::duration<double>(std::chrono::steady_clock::now() - s->abandonedAt).count();
        delete s;
        nr::logf("[mochizuki] the session Destroy left to the network build ended on the build's thread, %.1f s after "
                 "Destroy",
                 late);
    }

    // The builder, once its build ended. Only ~Session calls it; EnsureNetwork joins under buildMutex once buildDone
    // is set. A detached builder (Abandon) is not joinable: this runs on it.
    void JoinBuilder()
    {
        std::unique_lock lock(buildMutex);
        if (!builder.joinable())
            return;
        buildEnded.wait(lock, [this] { return buildDone; });
        builder.join();
    }

    // The network goes because the VRAM check refused the frame's buffers beside it (MakeGeometry): without them it
    // would only hold VRAM. What that gives back is its measured cost, and its key is held like a refused build, so it
    // is built again only once the check admits it together with the buffers (NetworkBytes), not every kVramRetryMs.
    void ReleaseNetwork(const std::string& why)
    {
        Drain();
        UINT64 before = 0, after = 0, budget = 0;
        const bool known = vram.Read(before, budget);
        {
            std::lock_guard lock(submitMutex);
            runtime.reset();
        }
        const UINT64 freed = known && vram.Read(after, budget) && before > after ? before - after : 0;
        if (freed)
            Measured(net, freed);
        buildHold.Refused(net, why.c_str());
        nr::logf("[mochizuki] the network at %ux%u is released (%llu MB given back): the frame's buffers do not fit "
                 "beside it; it is built again once both fit (checked every %llu s)",
                 net.width, net.height, static_cast<unsigned long long>(freed >> 20),
                 static_cast<unsigned long long>(kVramRetryMs / 1000));
    }

    // The frame's buffers for g, unless the last try for g failed (geometryHold) or the VRAM check refuses them;
    // `why` then says why. A refusal also releases the network (ReleaseNetwork). A lost device throws. Buffers made
    // for g beside the network (prepared) are taken over: the builder made them only after this same VRAM check.
    bool MakeGeometry(const Geometry& g, ID3D12Resource* colour, std::string& why)
    {
        std::unique_ptr<FrameBuffers> made = std::move(prepared);
        if (made && !(made->geometry == g && SameDesc(made->resultDesc, ResultDesc(colour))))
        {
            made->Release(vk.device); // no GPU work has used them
            made.reset();
        }
        if (!made && geometryHold.Holds(g, GetTickCount64()))
        {
            why = geometryHold.message;
            return false;
        }
        if (!made && !VramAdmits(FrameBytes(g) + kVramMarginBytes, g.width, g.height, "the frame's buffers", why))
        {
            NoteError(why.c_str());
            if (runtime)
                ReleaseNetwork(why);
            else
                geometryHold.Refused(g, why.c_str());
            return false;
        }
        try
        {
            if (made)
                UseBuffers(*made);
            else
                CreateGeometry(g, colour);
        }
        catch (const std::exception& e)
        {
            const NrError::Kind kind = KindOf(e);
            if (kind == NrError::DeviceLost)
                throw;
            ReleaseGeometry(); // what was made of it
            char text[384];
            std::snprintf(text, sizeof text, "the frame's buffers at %ux%u could not be made (%s): %s", g.width,
                          g.height, KindName(kind), e.what());
            // In Cyberpunk 2077 the driver refused a shared buffer's handle (E_INVALIDARG) for a while, then took it:
            // an invalid argument is tried again too.
            const ULONGLONG delay = geometryHold.Failed(g, kind, text, true);
            NoteError(text);
            if (delay)
                nr::logf("[mochizuki] the frame's buffers are made again in %llu s",
                         static_cast<unsigned long long>(delay / 1000));
            why = text;
            return false;
        }
        geometryHold = {};
        return true;
    }

    // The blit a bucket makes on the motion vectors' format f: LINEAR from one image of it into another (the vectors
    // rescaled onto the subrect). The padding copies, which every format takes. Render thread.
    bool Blittable(VkFormat f)
    {
        auto known =
            std::find_if(formatFeatures.begin(), formatFeatures.end(), [f](const auto& p) { return p.first == f; });
        if (known == formatFeatures.end())
        {
            VkFormatProperties p {};
            vkGetPhysicalDeviceFormatProperties(vk.physical, f, &p);
            formatFeatures.emplace_back(f, p.optimalTilingFeatures);
            known = formatFeatures.end() - 1;
        }
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        return (known->second & need) == need;
    }

    // The extent the network and the frame's buffers are built for (W x H), for a frame whose render subrect is w x h
    // in the colour allocation `desc`, under MochizukiNrControls::drs_mode `mode`; `bucketed` when it is the bucket.
    // Exact (0), or motion vectors the bucket cannot blit: the subrect. Auto (1): the subrect too until one smaller
    // than this allocation comes, then the bucket. Always (2): the bucket for every frame. The bucket is, per axis, the
    // largest subrect seen rounded up to kBucketAlign, within the allocation (always: the largest allocation seen), so
    // a subrect equal to it runs as in exact. It only grows, except after kBucketShrinkMs of bucketed frames of one
    // allocation, all kBucketShrinkMargin inside it on both axes: it then shrinks to what they needed. Render thread.
    void DrsExtent(uint32_t mode, UINT w, UINT h, const D3D12_RESOURCE_DESC& desc, bool blittable, UINT& W, UINT& H,
                   bool& bucketed)
    {
        W = w;
        H = h;
        bucketed = false;
        if (!mode || !blittable)
        {
            if (mode && !drsRefusedLogged)
                nr::logf("[mochizuki] dynamic resolution: the motion vectors' format cannot be blitted; the network "
                         "is built for the render subrect (as in exact)");
            drsRefusedLogged |= mode != 0;
            bucket = {};
            return;
        }
        Bucket& b = bucket;
        const UINT allocWidth = UINT(desc.Width), allocHeight = desc.Height;
        if (b.allocWidth != desc.Width || b.allocHeight != desc.Height || b.allocFormat != desc.Format)
        {
            b.allocWidth = desc.Width;
            b.allocHeight = desc.Height;
            b.allocFormat = desc.Format;
            b.engaged = false;
            b.lowSince = 0; // the shrink counts only this allocation's frames
        }
        b.maxWidth = std::max(b.maxWidth, w);
        b.maxHeight = std::max(b.maxHeight, h);
        b.capWidth = std::max(b.capWidth, allocWidth);
        b.capHeight = std::max(b.capHeight, allocHeight);
        b.engaged |= mode == 2 || w < allocWidth || h < allocHeight;
        if (!b.engaged)
        {
            b.lowSince = 0; // a whole frame is not inside the bucket
            return;
        }
        const auto extent = [&](UINT seen, UINT cap, UINT alloc)
        { return std::min(mode == 2 ? cap : alloc, (seen + kBucketAlign - 1) / kBucketAlign * kBucketAlign); };
        W = extent(b.maxWidth, b.capWidth, allocWidth);
        H = extent(b.maxHeight, b.capHeight, allocHeight);
        const ULONGLONG now = GetTickCount64();
        if (w + kBucketShrinkMargin > W || h + kBucketShrinkMargin > H)
            b.lowSince = 0;
        else if (!b.lowSince)
        {
            b.lowSince = now;
            b.lowWidth = w;
            b.lowHeight = h;
        }
        else
        {
            b.lowWidth = std::max(b.lowWidth, w);
            b.lowHeight = std::max(b.lowHeight, h);
            if (now - b.lowSince >= kBucketShrinkMs)
            {
                b.maxWidth = b.lowWidth;
                b.maxHeight = b.lowHeight;
                b.capWidth = allocWidth; // the only allocation since lowSince
                b.capHeight = allocHeight;
                b.lowSince = 0;
                W = extent(b.maxWidth, b.capWidth, allocWidth);
                H = extent(b.maxHeight, b.capHeight, allocHeight);
                nr::logf("[mochizuki] dynamic resolution: the render subrect stayed at most %ux%u for %llu s; the "
                         "bucket shrinks",
                         b.maxWidth, b.maxHeight, static_cast<unsigned long long>(kBucketShrinkMs / 1000));
            }
        }
        bucketed = true;
        if (W != b.loggedWidth || H != b.loggedHeight)
        {
            nr::logf("[mochizuki] dynamic resolution (%s): the network runs at %ux%u for render subrects up to that in "
                     "the %ux%u colour allocation (this frame %ux%u)",
                     mode == 2 ? "always" : "auto", W, H, allocWidth, allocHeight, w, h);
            b.loggedWidth = W;
            b.loggedHeight = H;
        }
    }

    // What a frame has put on the queues so far, for the passthrough to carry on from.
    struct Submission
    {
        UINT64 produced = 0, finished = 0; // the frame's values in toVk and fromVk, around its Vulkan submit
        bool followed = false;             // Follow ran for the producer
        bool signalled = false;            // the producer's Signal(toVk, produced) is on the game queue
        bool submitted = false;            // the network's vkQueueSubmit went through
    };

    // One frame, between the halves of the game's list, from any game queue: the network, or its colour passed
    // through (Passthrough) when anything before the network's submit fails and zeroOutputFallback is set; `why` then
    // says what failed. False for a frame passed through. Throws a lost device, and any error it cannot pass the frame
    // through for.
    bool Enqueue(ID3D12CommandQueue* producer, std::string& why)
    {
        std::lock_guard lock(submitMutex);
        const uint64_t call = ++enqueueCalls;
        StartWatchdog();
        Submission sub;
        try
        {
            RunNetwork(producer, call, sub);
            return true;
        }
        catch (const std::exception& e)
        {
            const NrError::Kind kind = KindOf(e);
            if (!zeroOutputFallback || !input.buffer || kind == NrError::DeviceLost || sub.submitted)
                throw;
            Passthrough(producer, sub);
            char text[384];
            std::snprintf(text, sizeof text,
                          "the network failed on this frame (%s: %s); its colour was passed through (%llu frames so "
                          "far)",
                          KindName(kind), e.what(), static_cast<unsigned long long>(passthroughs));
            why = text;
            return false;
        }
    }

    // The frame's colour as the output: input copied to output on the Vulkan queue, in the network's place between
    // the frame's values (an ExecuteCommandLists from here would re-enter the game's detoured one). Follows and
    // signals `produced` for the producer if the network did not get to it. A failure here leaves the game's queue
    // without the frame's wait, and counts as a lost device; only the copy's recording, the first time the command
    // buffer is used for this geometry, fails before anything of the passthrough is queued, and fails the frame as one
    // without a passthrough. The history restarts on the next frame.
    void Passthrough(ID3D12CommandQueue* producer, Submission& sub)
    {
        auto lost = [](const char* what, VkResult r, HRESULT hr)
        {
            char text[160];
            std::snprintf(text, sizeof text, "passthrough: %s failed (VkResult %d, HRESULT 0x%08lX)", what, int(r),
                          static_cast<unsigned long>(hr));
            return NrError(NrError::DeviceLost, text, r, hr);
        };
        const uint32_t k = nextFallback;
        if (fallbackInFlight[k])
        {
            VkResult r = vkWaitForFences(vk.device, 1, &fallbackDone[k], VK_TRUE, kSlotWaitNs);
            if (r == VK_SUCCESS)
                r = vkResetFences(vk.device, 1, &fallbackDone[k]);
            if (r != VK_SUCCESS)
                throw lost("the wait for an earlier passthrough", r, S_OK);
            fallbackInFlight[k] = false;
        }
        if (!fallbackCurrent[k])
            RecordFallback(k);
        nextFallback = (k + 1) % kFallbacks;
        if (!sub.produced)
        {
            sub.produced = ++producedValue;
            sub.finished = ++finishedValue;
        }
        if (!sub.followed)
        {
            if (const HRESULT hr = Follow(producer); FAILED(hr))
                throw lost("the new queue's wait for the last frame", VK_SUCCESS, hr);
            sub.followed = true;
        }
        if (!sub.signalled)
        {
            if (const HRESULT hr = producer->Signal(toVk.fence, sub.produced); FAILED(hr))
                throw lost("the signal after the producer", VK_SUCCESS, hr);
            sub.signalled = true;
            lastSignalled = sub.produced;
        }
        if (const VkResult r = Submit(fallbackCommands[k], sub.produced, sub.finished, fallbackDone[k]);
            r != VK_SUCCESS)
            throw lost("vkQueueSubmit", r, S_OK);
        fallbackInFlight[k] = true;
        if (const HRESULT hr = producer->Wait(fromVk.fence, sub.finished); FAILED(hr))
            throw lost("the game queue's wait", VK_SUCCESS, hr);
        ++passthroughs;
        resetPending = true;
    }

    void RunNetwork(ID3D12CommandQueue* producer, uint64_t call, Submission& sub)
    {
        if (!runtime)
            throw NrError(NrError::Other, "no network for this frame");
        if (call == hooks.deviceLostAt)
            throw NrError(NrError::DeviceLost, "MZ_TEST_DEVICE_LOST_AT: the device was lost (test hook)",
                          VK_ERROR_DEVICE_LOST);
        // The slot's previous frame must be done before its command buffer is recorded again. The slot moves on only
        // then, so a frame whose wait timed out leaves it to the next frame.
        const uint32_t slot = nextSlot;
        if (inFlight[slot] || call == hooks.slotTimeoutAt)
        {
            const VkResult waited = call == hooks.slotTimeoutAt
                                        ? VK_TIMEOUT
                                        : vkWaitForFences(vk.device, 1, &done[slot], VK_TRUE, kSlotWaitNs);
            if (waited == VK_TIMEOUT)
            {
                if (++slotTimeouts >= kSlotTimeoutsLost)
                    throw NrError(NrError::DeviceLost,
                                  "the wait for an earlier frame timed out 3 times in a row: the GPU is taken to be "
                                  "lost",
                                  VK_TIMEOUT);
                throw NrError(NrError::Timeout, "the wait for an earlier frame timed out (250 ms)", VK_TIMEOUT);
            }
            VkCheck(waited, "wait for an earlier frame");
            VkCheck(vkResetFences(vk.device, 1, &done[slot]), "vkResetFences");
            inFlight[slot] = false;
        }
        slotTimeouts = 0;
        nextSlot = (slot + 1) % kSlots;
        VkCommandBuffer c = commands[slot];
        VkCheck(vkResetCommandBuffer(c, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCheck(vkBeginCommandBuffer(c, &bi), "vkBeginCommandBuffer");

        // The frame's valid extent in the image the network runs on, W x H: the whole of it, except with dynamic
        // resolution, where the rest repeats the frame's edge (Pad).
        const uint32_t w = job.width, h = job.height, W = geometry.width, H = geometry.height;
        const bool padded = w < W || h < H;
        // Buffer to image, the valid w x h only; left in GENERAL for the network, or with `pad` for more transfers.
        auto upload =
            [&](const SharedBuffer& from, const Image& to, const Footprint& fp, uint32_t vw, uint32_t vh, bool pad)
        {
            VkBufferImageCopy region = fp.vk;
            region.imageExtent = { vw, vh, 1 };
            ImageBarrier(c, to.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(c, from.buffer, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            if (pad)
                ImageBarrier(c, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
            else
                ImageBarrier(c, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        };
        // The transfers into `image` (GENERAL) done, for the network.
        auto ready = [&](VkImage image)
        {
            ImageBarrier(c, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        };
        // The frame goes up into our image, the core reads it there (sampled, in place) and writes the answer back
        // into it.
        const Image& colour = colourImage;
        upload(input, colour, colourFootprint, w, h, padded);
        if (padded)
        {
            Pad(c, colour.image, w, h, W, H);
            ready(colour.image);
        }
        const bool motion = job.motion && motionImage.image;
        // The image the network reads the vectors from, and its extent: the uploaded one, or the bucket-sized one with
        // the vectors laid over the subrect (dynamic resolution).
        VkImage motionSource = motionImage.image;
        uint32_t motionW = job.motionWidth, motionH = job.motionHeight;
        if (motion && job.motionPath == Job::Uploaded)
            upload(motionBuffer, motionImage, motionFootprint, motionW, motionH, false);
        else if (motion && job.motionPath == Job::Padded)
        {
            // They cover the subrect: straight into the bucket-sized image.
            upload(motionBuffer, motionScaled, motionFootprint, motionW, motionH, padded);
            if (padded)
            {
                Pad(c, motionScaled.image, w, h, W, H);
                ready(motionScaled.image);
            }
            motionSource = motionScaled.image;
            motionW = W;
            motionH = H;
        }
        else if (motion)
        {
            // Another extent: uploaded, rescaled onto the subrect (LINEAR, as the network samples them) and padded. A
            // LINEAR tap may reach one texel past the valid extent when the image is larger, which gets its edge too.
            const uint32_t MW = geometry.motionWidth, MH = geometry.motionHeight;
            const bool border = motionW < MW || motionH < MH;
            upload(motionBuffer, motionImage, motionFootprint, motionW, motionH, true);
            if (border)
            {
                Pad(c, motionImage.image, motionW, motionH, std::min(motionW + 1, MW), std::min(motionH + 1, MH));
                TransferBarrier(c, motionImage.image);
            }
            ImageBarrier(c, motionScaled.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
            Blit(c, motionImage.image, motionScaled.image, { 0, 0 }, { int32_t(motionW), int32_t(motionH) }, { 0, 0 },
                 { int32_t(w), int32_t(h) }, VK_FILTER_LINEAR);
            if (padded)
            {
                TransferBarrier(c, motionScaled.image);
                Pad(c, motionScaled.image, w, h, W, H);
            }
            ready(motionScaled.image);
            motionSource = motionScaled.image;
            motionW = W;
            motionH = H;
        }
        if (call == hooks.enqueueThrowAt)
            throw std::runtime_error("MZ_TEST_ENQUEUE_THROW_AT: recording the network failed (test hook)");

        nr::EngineFrame frame {};
        frame.colour.image = colour.image;
        frame.colour.format = colourVk;
        frame.colour.width = W;
        frame.colour.height = H;
        frame.colour.usage = kImageUsage;
        frame.reset = job.reset;
        // Plain fields the core reads while recording; the runtime is used under submitMutex only.
        runtime->set_history_strength(job.history);
        runtime->set_white_point(job.white);
        bool consumed = false;
        if (motion)
        {
            frame.motion = frame.colour;
            frame.motion.image = motionSource;
            frame.motion.format = motionVk;
            frame.motion.width = motionW;
            frame.motion.height = motionH;
            frame.motion.usage = kImageUsage;
            frame.motion_scale_x = job.motionScaleX;
            frame.motion_scale_y = job.motionScaleY;
            const nr::EngineResult r = runtime->record_engine(c, frame, job.controls);
            dispatches.store(r.frame.network_dispatches, std::memory_order_relaxed);
            consumed = r.history_consumed;
        }
        else
            dispatches.store(runtime->record(c, frame.colour, job.controls).network_dispatches,
                             std::memory_order_relaxed);
        const nr::Preprocess& prep = job.controls.preprocess;
        float prepEv = NAN;
        if (prep.active() && prep.exposure == 1)
            prepEv = runtime->preprocess_meter().first + prep.bias_ev;
        infoPrepEv.store(prepEv, std::memory_order_relaxed);
        historyBits.store((historyBits.load(std::memory_order_relaxed) << 1) | uint64_t(consumed),
                          std::memory_order_relaxed);
        historyCount.store(std::min(historyCount.load(std::memory_order_relaxed) + 1, kHistorySamples),
                           std::memory_order_relaxed);
        if (const float ms = runtime->last_gpu_ms(); ms > 0 &&
            (statsNetwork != runtime.get() || statsSerial != runtime->gpu_timing_serial()))
        {
            std::lock_guard stats(statsMutex);
            if (statsNetwork != runtime.get()) { gpuMsCount = gpuMsNext = 0; }
            statsNetwork = runtime.get();
            statsSerial = runtime->gpu_timing_serial();
            ++gpuSamples;
            gpuTick = GetTickCount64();
            gpuMs[gpuMsNext] = ms;
            gpuMsNext = (gpuMsNext + 1) % kGpuSamples;
            gpuMsCount = std::min(gpuMsCount + 1, kGpuSamples);
        }

        ImageBarrier(c, colour.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy out = colourFootprint.vk;
        out.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(c, colour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &out);
        VkCheck(vkEndCommandBuffer(c), "vkEndCommandBuffer");

        // The producer half is already on the game queue: after Follow, its signal releases our submit, ours releases
        // the rest.
        sub.produced = ++producedValue;
        sub.finished = ++finishedValue;
        Check(Follow(producer), "the new queue's wait for the last frame");
        sub.followed = true;
        Check(producer->Signal(toVk.fence, sub.produced), "signal after the producer");
        sub.signalled = true;
        lastSignalled = sub.produced;
        if (call == hooks.dropSubmitAt)
        {
            nr::logf("[mochizuki] MZ_TEST_DROP_SUBMIT_AT: the Vulkan submit of Enqueue %llu is skipped; the game queue "
                     "waits for it all the same (test hook)",
                     static_cast<unsigned long long>(call));
            Submitted(sub.produced, sub.finished); // as if the Vulkan side had lost it
        }
        else
        {
            VkCheck(Submit(c, sub.produced, sub.finished, done[slot]), "vkQueueSubmit");
            inFlight[slot] = true;
        }
        sub.submitted = true;
        Check(producer->Wait(fromVk.fence, sub.finished), "wait for the network");
        ++frames;
    }

    // The game's device, queue and producer queues given back.
    void ReleaseGameQueues()
    {
        lastProducer.Release();
        for (Producer& p : producers)
            p.Release();
        producers.clear();
        if (queue)
            queue->Release();
        if (device)
            device->Release();
    }

    ~Session()
    {
        StopWatchdog();
        JoinBuilder();
        StopReleaser(); // what it has not freed is in `retired`
        vram.Close();   // a kernel handle to the adapter, which no GPU work uses
        // Once the GPU has seen the session, a lost device, a game queue that does not drain within 30 s or Vulkan
        // work that does not finish within kDrainWaitMs a wait (VulkanIdle) means it may still use it all: the game's
        // queues are released (Lose, and Rerelease once more, since the watchdog has stopped), and the Vulkan side and
        // the buffers leak rather than be freed under them; only the game's device and queues are given back.
        // Otherwise everything goes, after a failure too. Before the GPU has seen it (a Start that failed, no build
        // yet) nothing is in use.
        if (gpuUsed && !deviceLost)
        {
            Queues queues;
            if (FAILED(DrainGameQueues(queues)) || (vk.device && VulkanIdle(kDrainWaitMs) != VK_SUCCESS))
                Lose("the session ended with GPU work that did not finish");
        }
        if (gpuUsed && deviceLost)
        {
            Rerelease();
            Quarantine(runtime);
            Quarantine(built);
            Quarantine(allocation);
            for (auto& b : buried) Quarantine(b.buffers.allocation);
            if (builtBuffers) Quarantine(builtBuffers->allocation);
            if (prepared) Quarantine(prepared->allocation);
            for (Retired& r : retired)
                Quarantine(r.network);
            ReleaseGameQueues();
            return;
        }
        if (vk.device)
            vkDeviceWaitIdle(vk.device); // returns at once: all of this session's work has finished
        if (runtime)
            LogNetworkTime("session end");
        runtime.reset();
        built.reset();
        retired.clear();
        ReleaseGeometry();
        FreeBuried(true);
        for (std::unique_ptr<FrameBuffers>* b : { &builtBuffers, &prepared })
            if (*b)
                (*b)->Release(vk.device);
        FreeGraveyard(); // the game queues drained above, an orphaned list with them
        if (vk.device)
        {
            DestroyFences();
            if (pool)
                vkDestroyCommandPool(vk.device, pool, nullptr);
        }
        toVk.Release(vk.device);
        fromVk.Release(vk.device);
        vk.Destroy();
        ReleaseGameQueues();
    }
};

// A call that threw fails with FAILED. A lost device also fails the session for good and releases the game's queues
// (Lose); any other error is the call's alone: recorded for GetInfo from any thread (GetLastError's copy is the calling
// thread's) and logged.
int32_t Failed(Session* s, NrError::Kind kind, const char* what)
{
    char text[320];
    std::snprintf(text, sizeof text, "error (%s): %s", KindName(kind), what);
    if (s && kind == NrError::DeviceLost)
        s->Lose(text);
    else if (s)
        s->NoteError(text);
    else
        nr::logf("[mochizuki] %s", text);
    return Fail(LMXXF_NR_FAILED, what);
}

// whileFailed: the call also runs on a failed session whose device was not lost (Drain, which the host may call
// before Destroy).
template <class Fn> int32_t Guard(Session* s, Fn&& fn, bool whileFailed = false)
{
    if (s && s->failed && (!whileFailed || s->deviceLost))
        return Fail(LMXXF_NR_UNAVAILABLE, "mochizuki session failed earlier; see mochizuki_nr.log");
    try
    {
        return fn();
    }
    catch (const std::exception& e)
    {
        return Failed(s, KindOf(e), e.what());
    }
    catch (...)
    {
        return Failed(s, NrError::Other, "unhandled exception");
    }
}

int32_t QueryCapabilities(LmxxfNrCapabilities* out)
{
    if (!out || out->struct_size != sizeof(LmxxfNrCapabilities))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "QueryCapabilities: struct_size mismatch");
    out->abi_version = LMXXF_NR_ABI_VERSION;
    out->max_input_width = 16384;
    out->max_input_height = 16384;
    out->history_supported = 1;
    out->overlap_supported = out->graph_supported = 0;
    out->gfx1201_target = 1;
    return LMXXF_NR_OK;
}

int32_t Create(const LmxxfNrCreateInfo* info, void** context)
{
    if (!info || !context || info->struct_size != sizeof(LmxxfNrCreateInfo))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: struct_size mismatch");
    if (!info->device || !info->queue)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: device and queue required");
    if (info->flags != LMXXF_NR_CREATE_FLAG_RECORDING_LEASES)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: current package recording ABI required");
    auto* s = new (std::nothrow) Session;
    if (!s)
        return Fail(LMXXF_NR_FAILED, "Create: out of memory");
    s->zeroOutputFallback = true;
    s->assets = info->assets_directory ? info->assets_directory : DataDirectory();
    s->hooks = Hooks();
    static_cast<IUnknown*>(info->device)->QueryInterface(IID_PPV_ARGS(&s->device));
    static_cast<IUnknown*>(info->queue)->QueryInterface(IID_PPV_ARGS(&s->queue));
    if (!s->device || !s->queue)
    {
        delete s;
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: device or queue is not a D3D12 object");
    }
    *context = s;
    return LMXXF_NR_OK;
}

// During a network build, the build is stopped and waited for at most kAbandonWaitMs; one that takes longer ends the
// session on its own thread (Session::Abandon).
int32_t Destroy(void* context)
{
    auto* s = static_cast<Session*>(context);
    if (s && (!s->recordings.empty() || s->executing))
        return Fail(LMXXF_NR_UNAVAILABLE, "Destroy: live recording leases");
    if (s && !s->Abandon())
        return LMXXF_NR_OK;
    delete s;
    return LMXXF_NR_OK;
}

// A Start that fails is undone and reported UNAVAILABLE without failing the session, so the host may try again
// later; one that fails "[unsupported] ..." is remembered for the adapter, and later calls return at once.
int32_t PrepareSession(void* context)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareSession: no session");
    // Delay-load keeps ABI validation usable on a CI host without a Vulkan driver.
    // Probe explicitly so a missing loader produces a diagnostic, not an SEH exception.
    static HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader) return Fail(LMXXF_NR_UNAVAILABLE, "Vulkan loader missing; install the AMD display driver");
    return Guard(s,
                 [&]
                 {
                     if (s->vk.device)
                         return int32_t(LMXXF_NR_OK);
                     const LUID luid = s->device->GetAdapterLuid();
                     if (const std::string reason = UnsupportedReason(luid); !reason.empty())
                     {
                         s->RememberError(reason.c_str());
                         return Fail(LMXXF_NR_UNAVAILABLE, reason.c_str());
                     }
                     try
                     {
                         s->Start();
                     }
                     catch (const std::exception& e)
                     {
                         s->AbandonStart();
                         if (!std::strncmp(e.what(), "[unsupported] ", 14))
                             RememberUnsupported(luid, e.what());
                         else
                             nr::logf("[mochizuki] error: %s (the session start was undone)", e.what());
                         s->RememberError(e.what());
                         return Fail(LMXXF_NR_UNAVAILABLE, e.what());
                     }
                     catch (...)
                     {
                         s->AbandonStart();
                         nr::logf("[mochizuki] error: unhandled exception in Start (the session start was undone)");
                         s->RememberError("PrepareSession: unhandled exception in Start");
                         return Fail(LMXXF_NR_UNAVAILABLE, "PrepareSession: unhandled exception in Start");
                     }
                     s->RememberError(""); // an earlier failed start is over
                     return int32_t(LMXXF_NR_OK);
                 });
}

int32_t CorePrepareFrame(void* context, const MochizukiNrFrameInfo* info, LmxxfNrJob* job)
{
    auto* s = static_cast<Session*>(context);
    if (!s || !info || !job || job->struct_size != sizeof(LmxxfNrJob) || info->struct_size != sizeof(MochizukiNrFrameInfo))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: bad arguments");
    job->handle = nullptr;
    job->private_output = nullptr;
    return Guard(
        s,
        [&]
        {
            if (!s->vk.device)
                return Fail(LMXXF_NR_NOT_IMPLEMENTED, "PrepareFrame: call PrepareSession first");
            s->FreeBuried(false);
            auto* colour = static_cast<ID3D12Resource*>(info->color);
            if (!colour || !info->color_width || !info->color_height)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: colour and its size are required");
            const D3D12_RESOURCE_DESC cd = colour->GetDesc();
            const Format cf = ColourFormat(cd.Format);
            if (!cf.vk || (NeedsBlitCheck(cf.vk) && !s->Blittable(cf.vk)))
            {
                char text[96];
                std::snprintf(text, sizeof text, "colour format %u is not supported", unsigned(cd.Format));
                return Fail(LMXXF_NR_UNAVAILABLE, text);
            }
            // The frame's valid extent: its render subrect, at the top-left of the colour allocation.
            const UINT w = std::min<UINT>(info->color_width, UINT(cd.Width));
            const UINT h = std::min<UINT>(info->color_height, cd.Height);
            Geometry g;
            g.colourFormat = cd.Format;
            g.colourWidth = cd.Width;
            g.colourHeight = cd.Height;
            // The motion fields up to reset; a FrameInfo too short for them has no motion.
            const bool hasMotion = true;
            auto* motion = hasMotion ? static_cast<ID3D12Resource*>(info->motion) : nullptr;
            const bool temporal = (info->flags & MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL) && motion;
            Format mf;
            D3D12_RESOURCE_DESC md {};
            UINT motionW = 0, motionH = 0; // the motion's valid extent
            if (temporal)
            {
                md = motion->GetDesc();
                mf = MotionFormat(md.Format);
                if (!mf.vk && s->motionRefused.exchange(uint32_t(md.Format)) != uint32_t(md.Format))
                    nr::logf("[mochizuki] motion vectors in DXGI format %u are not supported; running without history",
                             unsigned(md.Format));
                if (mf.vk)
                {
                    s->motionRefused = 0;
                    motionW = std::min<UINT>(info->motion_width ? info->motion_width : w, UINT(md.Width));
                    motionH = std::min<UINT>(info->motion_height ? info->motion_height : h, md.Height);
                }
            }

            // The frame's controls, every one sanitised (see San); a FrameInfo without a field gets its default.
            float detail = 1.f, colourStrength = 1.f;
            if (info->flags & LMXXF_NR_FRAME_FLAG_STRENGTH)
            {
                if (MZ_HOLDS(info, MochizukiNrFrameInfo, transfer_strength))
                    detail = San(info->transfer_strength, 0.f, 2.f, 1.f);
                if (MZ_HOLDS(info, MochizukiNrFrameInfo, color_strength))
                    colourStrength = San(info->color_strength, 0.f, 4.f, 1.f);
            }
            const float scale = MZ_HOLDS(info, MochizukiNrFrameInfo, model_scale) ? ModelScale(info->model_scale) : 1.f;
            const uint32_t passes =
                MZ_HOLDS(info, MochizukiNrFrameInfo, passes) ? std::clamp(info->passes, 1u, kMaxPasses) : 1u; // 0 is 1
            nr::Controls model;
            float history = 1.f, white = 1.f;
            uint32_t linearMode = 0, maxPassesSetting = 0, drsMode = 0;
            {
                std::lock_guard lock(s->controlsMutex);
                model = s->controls.model;
                history = s->controls.history;
                white = s->controls.white;
                linearMode = s->controls.linearMode;
                if (const float e = s->gameExposure.load(std::memory_order_relaxed); e > 0.f)
                    white = std::clamp(white / e, 1e-4f, 1e4f);
                maxPassesSetting = s->controls.maxPasses;
                drsMode = s->controls.drsMode;
            }
            // By default the network is built for exactly the frame's passes, so one pass costs nothing extra; a
            // larger max_passes lets the pass count change without a rebuild.
            const uint32_t maxPasses = maxPassesSetting ? std::clamp(maxPassesSetting, passes, kMaxPasses) : passes;
            const bool linear = linearMode == 0 ? IsLinear(cf.vk) : linearMode == 1;
            // The extent the network and the frame's buffers are built for: the frame's subrect, or with dynamic
            // resolution the bucket, whose motion buffer is the motion allocation's.
            const bool blittable = !drsMode || !mf.vk || s->Blittable(mf.vk);
            s->DrsExtent(drsMode, w, h, cd, blittable, g.width, g.height, g.bucketed);
            if (mf.vk)
            {
                g.motionFormat = md.Format;
                g.motionWidth = g.bucketed ? UINT(md.Width) : motionW;
                g.motionHeight = g.bucketed ? md.Height : motionH;
            }
            // Neither a network nor buffers that cannot be had now fail the session: the frame goes without NR, and
            // they are tried again as their holds say.
            s->prepWanted = s->prepWanted || model.preprocess.active();
            const NetworkKey key { g.width, g.height, cf.vk, scale, maxPasses, linear, s->prepWanted };
            std::string why;
            if (!s->EnsureNetwork(key, g, colour, why))
                return Fail(LMXXF_NR_UNAVAILABLE, why.c_str());
            if (!(s->geometry == g))
            {
                s->RetireGeometry();
                if (!s->MakeGeometry(g, colour, why))
                    return Fail(LMXXF_NR_UNAVAILABLE, why.c_str());
                if (g.bucketed)
                    nr::logf("[mochizuki] frame buffers %ux%u for dynamic resolution, colour allocation %llux%u DXGI "
                             "%u, motion allocation %ux%u DXGI %u",
                             g.width, g.height, static_cast<unsigned long long>(g.colourWidth), g.colourHeight,
                             unsigned(g.colourFormat), g.motionWidth, g.motionHeight, unsigned(g.motionFormat));
                else
                    nr::logf("[mochizuki] frame %ux%u colour DXGI %u, motion %ux%u DXGI %u", g.width, g.height,
                             unsigned(g.colourFormat), g.motionWidth, g.motionHeight, unsigned(g.motionFormat));
            }
            // A new valid extent restarts the history: it holds the frame's content where the old one had it.
            if (w != s->validWidth || h != s->validHeight)
            {
                s->validWidth = w;
                s->validHeight = h;
                s->infoValidWidth = w;
                s->infoValidHeight = h;
                s->resetPending = true;
            }

            // While a build for new settings runs, the frame keeps the network it ran on: no more passes than that one
            // was built for.
            const uint32_t served = std::min(passes, s->net.maxPasses);

            // History carries over only to the frame right after this one.
            const ULONGLONG now = GetTickCount64();
            const bool continuous = info->frame_id == s->lastFrameId + 1 && now - s->lastFrameTick < 250;
            s->lastFrameId = info->frame_id;
            s->lastFrameTick = now;

            // Each pass keeps a history of its own, and one that did not run on the last frame holds a stale one.
            if (served != s->lastPasses)
            {
                if (s->lastPasses)
                    nr::logf("[mochizuki] %u passes (was %u): history reset", served, s->lastPasses);
                s->lastPasses = served;
                s->infoPasses = served;
                s->resetPending = true;
            }

            Job next;
            next.colour = colour;
            next.colourState = static_cast<D3D12_RESOURCE_STATES>(info->color_state);
            next.width = w;
            next.height = h;
            next.controls = std::move(model);
            next.controls.detail_strength = detail;
            next.controls.colour_strength = colourStrength;
            next.controls.passes = int(served);
            next.history = history;
            next.white = white;
            s->infoWhite.store(white, std::memory_order_relaxed);
            if (mf.vk)
            {
                next.motion = motion;
                next.motionState = static_cast<D3D12_RESOURCE_STATES>(info->motion_state);
                next.motionWidth = motionW;
                next.motionHeight = motionH;
                // value * scale = pixels of the motion extent; the network reads previous uv = uv + mv.
                next.motionScaleX = info->motion_scale_x / float(motionW);
                next.motionScaleY = info->motion_scale_y / float(motionH);
                if (g.bucketed)
                {
                    // In the bucket, as uploaded when they span the whole of it as in exact; otherwise laid over the
                    // subrect [0, w) x [0, h), so their uv is the bucket's times w / W.
                    const bool whole = w == g.width && h == g.height;
                    next.motionPath = whole && motionW == g.motionWidth && motionH == g.motionHeight ? Job::Uploaded
                                      : motionW == w && motionH == h                                 ? Job::Padded
                                                                                                     : Job::Rescaled;
                    if (next.motionPath != Job::Uploaded)
                    {
                        next.motionScaleX *= float(w) / float(g.width);
                        next.motionScaleY *= float(h) / float(g.height);
                    }
                }
                // Consume the session reset at execution: this recording may be
                // discarded, reordered, or already closed when ResetHistory runs.
                next.reset = info->reset || !continuous;
            }
            else
                s->resetPending = true; // record() neither updates the history nor consumes a reset
            next.state = LMXXF_NR_JOB_PREPARED;
            {
                // A new generation: whatever still holds the last job's handle can no longer reach this one.
                std::lock_guard lock(s->jobMutex);
                s->job = std::move(next);
                job->handle = JobHandle(++s->jobGen);
            }
            job->private_output = s->result;
            SetError("");
            return int32_t(LMXXF_NR_OK);
        });
}

// Record*, EnqueueHip and Poll act only on the current job (the handle of the last PrepareFrame); the job's state
// changes under jobMutex.
int32_t CoreRecordInputs(void* context, void* job, void* command_list)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordInputs: no session");
    return Guard(s,
                 [&]
                 {
                     auto* c = static_cast<ID3D12GraphicsCommandList*>(command_list);
                     std::lock_guard lock(s->jobMutex);
                     Job& j = s->job;
                     if (!c || job != JobHandle(s->jobGen) || j.state != LMXXF_NR_JOB_PREPARED)
                         return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordInputs: job not prepared");
                     CopyToBuffer(c, j.colour, j.colourState, s->input.resource, s->colourFootprint, j.width, j.height);
                     if (j.motion)
                         CopyToBuffer(c, j.motion, j.motionState, s->motionBuffer.resource, s->motionFootprint,
                                      j.motionWidth, j.motionHeight);
                     j.state = LMXXF_NR_JOB_PRODUCER_SUBMITTED;
                     return int32_t(LMXXF_NR_OK);
                 });
}

// Runs the job's frame once: a second call for it returns OK and does nothing. A frame the network failed on (with
// ZERO_OUTPUT_FALLBACK) passed its colour through: OK, with the diagnostic in GetLastError.
int32_t CoreEnqueueHip(void* context, void* job, void* command_queue)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "EnqueueHip: no session");
    return Guard(s,
                 [&]
                 {
                     auto* q = static_cast<ID3D12CommandQueue*>(command_queue ? command_queue : s->queue);
                     {
                         std::lock_guard lock(s->jobMutex);
                         Job& j = s->job;
                         if (job != JobHandle(s->jobGen) ||
                             (!j.enqueued && j.state != LMXXF_NR_JOB_PRODUCER_SUBMITTED &&
                              j.state != LMXXF_NR_JOB_CONSUMER_COMPLETE))
                             return Fail(LMXXF_NR_INVALID_ARGUMENT, "EnqueueHip: job not recorded");
                         if (j.enqueued)
                         {
                             SetError("");
                             return int32_t(LMXXF_NR_OK);
                         }
                         j.enqueued = true;
                     }
                     std::string why;
                     const bool ran = s->Enqueue(q, why);
                     {
                         std::lock_guard lock(s->jobMutex);
                         if (job == JobHandle(s->jobGen) && s->job.state == LMXXF_NR_JOB_PRODUCER_SUBMITTED)
                             s->job.state = LMXXF_NR_JOB_NR_COMPLETE;
                     }
                     if (!ran)
                         s->NoteError(why.c_str());
                     SetError(ran ? "" : why.c_str());
                     return int32_t(LMXXF_NR_OK);
                 });
}

int32_t CoreRecordOutputs(void* context, void* job, void* command_list)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordOutputs: no session");
    return Guard(
        s,
        [&]
        {
            auto* c = static_cast<ID3D12GraphicsCommandList*>(command_list);
            std::lock_guard lock(s->jobMutex);
            Job& j = s->job;
            if (!c || job != JobHandle(s->jobGen) ||
                (j.state != LMXXF_NR_JOB_PRODUCER_SUBMITTED && j.state != LMXXF_NR_JOB_NR_COMPLETE))
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordOutputs: inputs not recorded");
            Transition(c, s->output.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(c, s->result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst { s->result, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
            D3D12_TEXTURE_COPY_LOCATION src { s->output.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
            src.PlacedFootprint = s->colourFootprint.d3d;
            // The frame's valid extent into result's top-left: all of the buffer, except with dynamic resolution.
            const D3D12_BOX box { 0, 0, 0, j.width, j.height, 1 };
            const bool whole =
                j.width == src.PlacedFootprint.Footprint.Width && j.height == src.PlacedFootprint.Footprint.Height;
            c->CopyTextureRegion(&dst, 0, 0, 0, &src, whole ? nullptr : &box);
            Transition(c, s->result, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(c, s->output.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (j.state == LMXXF_NR_JOB_PRODUCER_SUBMITTED)
                j.state = LMXXF_NR_JOB_CONSUMER_COMPLETE;
            return int32_t(LMXXF_NR_OK);
        });
}

// The current job only. One whose inputs or outputs are recorded in a list the game may still run is orphaned: see
// ReleaseGeometry.
int32_t CoreCancelUnsubmitted(void* context, void* job)
{
    if (auto* s = static_cast<Session*>(context))
    {
        std::lock_guard lock(s->jobMutex);
        if (job != JobHandle(s->jobGen))
            return LMXXF_NR_OK;
        if (s->job.state == LMXXF_NR_JOB_PRODUCER_SUBMITTED || s->job.state == LMXXF_NR_JOB_CONSUMER_COMPLETE)
        {
            s->orphaned = true;
            s->orphanGen = s->jobGen;
        }
        s->job.state = LMXXF_NR_JOB_RETIRED;
        s->resetPending = true;
    }
    return LMXXF_NR_OK;
}

int32_t CorePoll(void* context, void* job, uint32_t* state)
{
    auto* s = static_cast<Session*>(context);
    if (state)
        *state = LMXXF_NR_JOB_NONE;
    if (!s)
        return LMXXF_NR_OK;
    std::lock_guard lock(s->jobMutex);
    if (job != JobHandle(s->jobGen))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Poll: not the current job");
    if (state)
        *state = s->job.state;
    return LMXXF_NR_OK;
}

// The current job only, once its network and outputs are in: a late Retire of an earlier job leaves the next alone.
int32_t CoreRetire(void* context, void* job)
{
    if (auto* s = static_cast<Session*>(context))
    {
        std::lock_guard lock(s->jobMutex);
        if (job == JobHandle(s->jobGen) &&
            (s->job.state == LMXXF_NR_JOB_NR_COMPLETE || s->job.state == LMXXF_NR_JOB_CONSUMER_COMPLETE))
        {
            s->job.state = LMXXF_NR_JOB_RETIRED;
            s->retiredGen = s->jobGen;
        }
    }
    return LMXXF_NR_OK;
}

int32_t ResetHistory(void* context)
{
    if (auto* s = static_cast<Session*>(context))
        s->resetPending = true;
    return LMXXF_NR_OK;
}

// Also on a failed session whose device was not lost.
int32_t Drain(void* context)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Drain: no session");
    return Guard(
        s,
        [&]
        {
            s->Drain();
            return int32_t(LMXXF_NR_OK);
        },
        true);
}

int32_t GetStatus(void* context, char* buf, uint32_t chars)
{
    if (!buf || !chars)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetStatus: empty buffer");
    auto* s = static_cast<Session*>(context);
    char text[256];
    if (!s)
        std::snprintf(text, sizeof text, "no session");
    else if (s->failed)
        std::snprintf(text, sizeof text, "mochizuki failed (see mochizuki_nr.log)");
    else if (s->runtime)
    {
        float median = 0, p95 = 0;
        s->NetworkMs(median, p95);
        // The model extent and the passes, when they are not the frame's own and one.
        char model[64] = {};
        const uint32_t mw = s->infoModelWidth, mh = s->infoModelHeight, passes = s->infoPasses;
        if (mw != s->net.width || mh != s->net.height || passes > 1)
            std::snprintf(model, sizeof model, " (model %ux%u, %u pass%s)", mw, mh, passes, passes > 1 ? "es" : "");
        // With dynamic resolution, the last frame's subrect when it is not the whole network extent.
        char frame[64] = {};
        const uint32_t vw = s->infoValidWidth, vh = s->infoValidHeight;
        if (vw && (vw != s->net.width || vh != s->net.height))
            std::snprintf(frame, sizeof frame, " (dynamic resolution, frame %ux%u)", vw, vh);
        std::snprintf(text, sizeof text, "mochizuki %ux%u%s%s, network %.2f ms (p95 %.2f), %llu frames", s->net.width,
                      s->net.height, model, frame, median, p95, static_cast<unsigned long long>(s->frames.load()));
    }
    else
    {
        bool building = false;
        {
            std::lock_guard lock(s->buildMutex);
            building = s->building;
        }
        std::snprintf(text, sizeof text, "mochizuki %s", building ? "building the network" : "idle");
    }
    std::strncpy(buf, text, chars - 1);
    buf[chars - 1] = 0;
    return LMXXF_NR_OK;
}

int32_t GetLastError(char* buf, uint32_t chars)
{
    if (!buf || !chars)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetLastError: empty buffer");
    std::strncpy(buf, g_lastError, chars - 1);
    buf[chars - 1] = 0;
    return LMXXF_NR_OK;
}

// MochizukiNrControls.h. Every Controls field is 4 bytes and 4-aligned and the struct ends at its last field, so the
// whole fields a caller's struct holds are its first struct_size & ~3 bytes.
static_assert(alignof(MochizukiNrControls) == 4 &&
              sizeof(MochizukiNrControls) == offsetof(MochizukiNrControls, preprocess_saturation) +
                                                 sizeof(MochizukiNrControls::preprocess_saturation) &&
              offsetof(MochizukiNrControls, pass) == 72 && offsetof(MochizukiNrControls, preprocess) == 136 &&
              sizeof(MochizukiNrPassControls) == 32);
// Info has no implicit padding (reserved0 fills the gap before frames), so its offsets do not depend on packing; its
// sizeof does (tail padding after network_dispatches), which is why a filler reports the end of its last field.
static_assert(offsetof(MochizukiNrInfo, reserved0) == 44 && offsetof(MochizukiNrInfo, frames) == 48 &&
              offsetof(MochizukiNrInfo, last_error) == 64 && offsetof(MochizukiNrInfo, network_dispatches) == 320 &&
              offsetof(MochizukiNrInfo, preprocess_ev) == 324 && offsetof(MochizukiNrInfo, white_point) == 328);

// The bytes GetInfo writes into a struct of `size` bytes: the whole fields that fit, never part of one, and never
// Info's tail padding. A host that appends a field there (sizeof does not grow) then sees struct_size end before it.
size_t InfoBytes(size_t size)
{
    constexpr size_t ends[] = {
        offsetof(MochizukiNrInfo, white_point) + sizeof(MochizukiNrInfo::white_point), // the last field
        offsetof(MochizukiNrInfo, white_point),
        offsetof(MochizukiNrInfo, preprocess_ev),
        offsetof(MochizukiNrInfo, network_dispatches),
        offsetof(MochizukiNrInfo, last_error),
        offsetof(MochizukiNrInfo, failed),
        offsetof(MochizukiNrInfo, history_consumed_pct),
        offsetof(MochizukiNrInfo, frames),
    };
    for (const size_t end : ends)
        if (size >= end)
            return end;
    return size & ~size_t(3); // below frames every field is 4 bytes
}

// Never fails the session: everything is sanitised, and only the pass overrides allocate.
int32_t SetControls(void* context, const MochizukiNrControls* in)
{
    auto* s = static_cast<Session*>(context);
    if (!s || !in || in->struct_size != sizeof(MochizukiNrControls) || in->flags)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "SetControls: bad arguments");
    try
    {
        MochizukiNrControls c;
        DefaultControls(c); // what a shorter struct does not hold
        std::memcpy(&c, in, std::min<size_t>(in->struct_size, sizeof c) & ~size_t(3));
        SessionControls sanitised = Sanitise(c);
        std::lock_guard lock(s->controlsMutex);
        s->controls = std::move(sanitised);
        return int32_t(LMXXF_NR_OK);
    }
    catch (const std::exception& e)
    {
        return Fail(LMXXF_NR_FAILED, e.what());
    }
}

// Atomics and the remembered error only: it may run on any thread, next to a frame or a build.
int32_t GetInfo(void* context, MochizukiNrInfo* out)
{
    auto* s = static_cast<Session*>(context);
    if (!s || !out || out->struct_size != sizeof(MochizukiNrInfo))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetInfo: bad arguments");
    try
    {
        MochizukiNrInfo info {};
        info.building = s->infoBuilding;
        info.frame_w = s->infoFrameWidth;
        info.frame_h = s->infoFrameHeight;
        info.model_w = s->infoModelWidth;
        info.model_h = s->infoModelHeight;
        info.max_passes = s->infoMaxPasses;
        info.motion_refused_dxgi = s->motionRefused;
        s->NetworkMs(info.gpu_ms_median, info.gpu_ms_p95);
        {
            std::lock_guard stats(s->statsMutex);
            info.gpu_samples = s->gpuSamples;
            info.gpu_tick = s->gpuTick;
            if (s->gpuMsCount) {
                info.gpu_ms_last = s->gpuMs[(s->gpuMsNext + kGpuSamples - 1) % kGpuSamples];
                for (uint32_t i = 0; i < s->gpuMsCount; ++i) {
                    info.gpu_ms_mean += s->gpuMs[i];
                    info.gpu_ms_max = std::max(info.gpu_ms_max, s->gpuMs[i]);
                }
                info.gpu_ms_mean /= s->gpuMsCount;
            }
        }
        info.build_seconds = s->infoBuildSeconds;
        info.frames = s->frames;
        const uint32_t n = s->historyCount;
        const uint64_t window = n < kHistorySamples ? (uint64_t(1) << n) - 1 : ~uint64_t(0);
        info.history_consumed_pct = n ? uint32_t(std::popcount(s->historyBits & window)) * 100 / n : 0;
        info.failed = s->failed;
        info.network_dispatches = s->dispatches;
        info.preprocess_ev = s->infoPrepEv;
        info.white_point = s->infoWhite;
        {
            std::lock_guard lock(s->errorMutex);
            std::memcpy(info.last_error, s->lastError, sizeof info.last_error);
        }
        const size_t bytes = sizeof(MochizukiNrInfo);
        info.struct_size = uint32_t(bytes);
        std::memcpy(out, &info, bytes);
        return int32_t(LMXXF_NR_OK);
    }
    catch (const std::exception& e)
    {
        return Fail(LMXXF_NR_FAILED, e.what());
    }
}

int32_t GetControlDefaults(MochizukiNrControls* out)
{
    if (!out || out->struct_size != sizeof(MochizukiNrControls))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetControlDefaults: bad arguments");
    MochizukiNrControls c;
    DefaultControls(c);
    const size_t bytes = std::min<size_t>(out->struct_size, sizeof c) & ~size_t(3);
    c.struct_size = uint32_t(bytes);
    std::memcpy(out, &c, bytes);
    return LMXXF_NR_OK;
}
#include "MochizukiRecording.inl"
} // namespace

extern "C" __declspec(dllexport) int32_t MochizukiNrGetApi(uint32_t abi_version, LmxxfNrApi* out)
{
    if (!out || out->struct_size != sizeof(LmxxfNrApi))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetApi: struct_size mismatch");
    if (abi_version != LMXXF_NR_ABI_VERSION)
        return Fail(LMXXF_NR_UNSUPPORTED_ABI, "GetApi: unsupported abi_version");
    nr::set_log_sink(LogLine);
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof(LmxxfNrApi);
    out->abi_version = LMXXF_NR_ABI_VERSION;
    out->QueryCapabilities = QueryCapabilities;
    out->Create = Create;
    out->Destroy = Destroy;
    out->PrepareSession = PrepareSession;
    out->PrepareFrame = RejectLmxxfFrame;
    out->RecordInputs = RecordInputs;
    out->EnqueueHip = EnqueueHip;
    out->RecordOutputs = RecordOutputs;
    out->ExecuteAfterProducer = EnqueueHip;
    out->CancelUnsubmitted = CancelUnsubmitted;
    out->Poll = Poll;
    out->Retire = Retire;
    out->ResetHistory = ResetHistory;
    out->Drain = Drain;
    out->GetStatus = GetStatus;
    out->GetLastError = GetLastError;
    out->BeginRecordingExecution = BeginRecordingExecution;
    out->EndRecordingExecution = EndRecordingExecution;
    out->InvalidateRecording = InvalidateRecording;
    out->CollectRecording = CollectRecording;
    out->GetTimings = GetTimings;
    return LMXXF_NR_OK;
}

// MochizukiNrControls.h: mochizuki's own exports, resolved by the host with GetProcAddress.
extern "C" __declspec(dllexport) int32_t MochizukiNrSetControls(void* context, const MochizukiNrControls* controls)
{
    return SetControls(context, controls);
}

extern "C" __declspec(dllexport) int32_t MochizukiNrGetInfo(void* context, MochizukiNrInfo* info)
{
    return GetInfo(context, info);
}

extern "C" __declspec(dllexport) int32_t MochizukiNrGetControlDefaults(MochizukiNrControls* controls)
{
    return GetControlDefaults(controls);
}

extern "C" __declspec(dllexport) int32_t MochizukiNrSetExposure(void* context, float exposure)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "SetExposure: no session");
    const float e = std::isfinite(exposure) && exposure > 0.f ? exposure : 0.f;
    if ((e > 0.f) != (s->gameExposure.exchange(e, std::memory_order_relaxed) > 0.f))
    {
        if (e > 0.f)
            nr::logf("[mochizuki] white point from the game's exposure (%.4f now)", double(e));
        else
            nr::logf("[mochizuki] the game's exposure is gone: the white point is the setting alone");
    }
    return LMXXF_NR_OK;
}

// ANY_QUEUE: a frame may come from any game queue (Session::Follow), so the host keeps the session on a queue change.
extern "C" __declspec(dllexport) uint32_t MochizukiNrGetFeatures(void) { return MOCHIZUKI_NR_FEATURE_ANY_QUEUE; }

extern "C" __declspec(dllexport) int32_t MochizukiNrPrepareFrame(
    void* context, const MochizukiNrFrameInfo* info, LmxxfNrJob* job)
{ return PrepareFrame(context, info, job); }

extern "C" __declspec(dllexport) void MochizukiNrSetLogging(uint32_t enabled)
{ g_logging.store(enabled != 0, std::memory_order_relaxed); }

extern "C" __declspec(dllexport) int32_t MochizukiNrGetBuildProgress(void* context, MochizukiNrBuildProgress* out)
{
    if (!context || !out || out->struct_size != sizeof(*out)) return LMXXF_NR_INVALID_ARGUMENT;
    auto* s = static_cast<Session*>(context);
    std::lock_guard lock(s->progressMutex);
    *out = s->progress;
    const auto now = GetTickCount64();
    if (out->active && now - s->progressLogTick >= 10000) {
        s->progressLogTick = now;
        nr::logf("[mochizuki] build %.1f s: %s (%u/%u), last progress %.1f s ago",
            double(now - out->start_tick) / 1000, out->stage, out->completed, out->total,
            double(now - out->update_tick) / 1000);
    }
    return LMXXF_NR_OK;
}
