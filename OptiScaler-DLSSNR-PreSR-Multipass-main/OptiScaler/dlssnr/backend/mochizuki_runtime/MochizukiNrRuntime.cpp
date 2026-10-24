// MochizukiNrRuntime.dll: mochizuki0323's Vulkan network (third_party/mochizuki) behind the lmxxf C ABI.
//
// Proton adopts vkd3d's device and records into the current native game command
// buffer, following pinned Linux PE run/run_after. Game-tail certificates own
// recipes and resources; no frame submit or cross-API handshake runs there.
// Windows keeps its separate-device split/inline shared-buffer bridge: toVk ->
// private Vulkan recording/submit -> fromVk -> D3D12 output. Controls and the
// asynchronous network construction policy are shared by both adapters.
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
#include "RecordingDeviceIdentity.h"
#include "RecordingPoolTrim.h"
#include <unordered_map>
#include <wrl/client.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <winternl.h> // NTSTATUS, for d3dkmthk.h
#if __has_include(<d3dkmthk.h>)
#include <d3dkmthk.h> // D3DKMTQueryVideoMemoryInfo, for the VRAM check (VideoMemory)
#else
#include "compat/mz_d3dkmthk.h" // Same three user-mode entries, for a build without the WDK
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include "MochizukiStageTiming.h"
#include "MochizukiVkd3dInterop.h"
#include "MochizukiCompilePolicy.h"
#include "MochizukiVulkanDevice.h"
#include "MochizukiTransportPolicy.h"
using mz::compile::CompilePolicy;

#include "mz_interpose.h"
#include "nr_log.hpp"
#include "nr_pipeline_binary.hpp"
#include "nr_runtime.hpp"

#include "nr_renodx.hpp"
#include "MochizukiAutoExposure.h" // the lane's own exposure meter: the game hands over none

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
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(MZ_BUILD_VERSION) || !defined(MZ_BUILD_COMMIT) || !defined(MZ_BUILD_SOURCE_SHA256)
#error Build MochizukiNrRuntime with tools/build/build-mochizuki-runtime.cmd to stamp its source identity.
#endif

namespace
{
thread_local char g_lastError[256] = {};
std::atomic<mz::device::Query> g_queryCreatedDevice { nullptr };
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
    uint64_t upgradeRefusals = 0; // MZ_TEST_UPGRADE_REFUSALS=N: reject N capacity expansion attempts
    bool upgradeOomOnce = false; // MZ_TEST_UPGRADE_OOM_ONCE=1: fail the first multi-pass build
    bool frameBudgetOnce = false; // MZ_TEST_FRAME_BUDGET_ONCE=1: refuse admission 1 when AT is unset
    uint64_t frameBudgetAt = 0;  // MZ_TEST_FRAME_BUDGET_AT=N: refuse the Nth candidate frame-buffer admission
    bool rejectAcoDevice = false; // MZ_TEST_REJECT_ACO_DEVICE=1: the first pipeline-binary device is treated as rejected
    uint64_t dropSubmitAt = 0;   // MZ_TEST_DROP_SUBMIT_AT=N: Enqueue N skips its Vulkan submit, the game still waits
    uint64_t phaseSubmitFailAt = 0; // MZ_TEST_PHASE_SUBMIT_FAIL_AT=N: fail phase N of an inline chain
    uint64_t computeRefuseAt = 0; // MZ_TEST_COMPUTE_REFUSE_AT=N: Enqueue N's whole-chain compute recording is refused
    int sharedInput = 1; // MZ_TEST_SHARED_INPUT=0 keeps the complete buffer route; 1 requests safe admission.
    int sharedMotion = 1; // MZ_TEST_SHARED_MOTION=0 retains buffer motion independently of colour.
    // MZ_TEST_SHARED_EXTENDED=0 keeps shared input to the 2026-10-19 admission (full extent, single pass, full model
    // scale, no RenoDX); 1 also admits DRS/bucket subrects, model scaling, multipass without first-pass export, RenoDX.
    int sharedExtended = 1;
    bool rejectSharedMotion = false; // MZ_TEST_REJECT_SHARED_MOTION=1: recoverable handle rejection.
    uint64_t sharedMotionDelay = 0; // MZ_TEST_SHARED_MOTION_DELAY=N: defer ready admission.
    bool rejectSharedInput = false; // MZ_TEST_REJECT_SHARED_INPUT=1: recoverable handle rejection.
    bool rejectSharedStorage = false; // MZ_TEST_REJECT_SHARED_STORAGE=1: optional RGBA16F allocation fails.
    uint64_t sharedInputDelay = 0; // MZ_TEST_SHARED_INPUT_DELAY=N: defer ready admission N prepares.
    uint64_t inlineChain = 0; // MZ_TEST_INLINE_CHAIN=1: an inline frame must run whole on compute; =2: in segments;
                              // =3: segments are forced
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
        h.upgradeRefusals = number("MZ_TEST_UPGRADE_REFUSALS");
        h.upgradeOomOnce = on("MZ_TEST_UPGRADE_OOM_ONCE");
        h.frameBudgetOnce = on("MZ_TEST_FRAME_BUDGET_ONCE");
        h.frameBudgetAt = number("MZ_TEST_FRAME_BUDGET_AT");
        if (h.frameBudgetOnce && h.frameBudgetAt == 0)
            h.frameBudgetAt = 1;
        h.rejectAcoDevice = on("MZ_TEST_REJECT_ACO_DEVICE");
        h.dropSubmitAt = number("MZ_TEST_DROP_SUBMIT_AT");
        h.phaseSubmitFailAt = number("MZ_TEST_PHASE_SUBMIT_FAIL_AT");
        h.inlineChain = number("MZ_TEST_INLINE_CHAIN");
        h.computeRefuseAt = number("MZ_TEST_COMPUTE_REFUSE_AT");
        if (const char* value = std::getenv("MZ_TEST_SHARED_INPUT")) h.sharedInput = std::strcmp(value, "0") != 0;
        if (const char* value = std::getenv("MZ_TEST_SHARED_MOTION")) h.sharedMotion = std::strcmp(value, "0") != 0;
        if (const char* value = std::getenv("MZ_TEST_SHARED_EXTENDED")) h.sharedExtended = std::strcmp(value, "0") != 0;
        h.rejectSharedMotion = on("MZ_TEST_REJECT_SHARED_MOTION");
        h.rejectSharedStorage = on("MZ_TEST_REJECT_SHARED_STORAGE");
        h.sharedMotionDelay = number("MZ_TEST_SHARED_MOTION_DELAY");
        h.rejectSharedInput = on("MZ_TEST_REJECT_SHARED_INPUT");
        h.sharedInputDelay = number("MZ_TEST_SHARED_INPUT_DELAY");
        return h;
    }();
    return hooks;
}
std::atomic<bool> g_buildOomInjected { false }; // MZ_TEST_BUILD_OOM_ONCE: once per process
std::atomic<uint64_t> g_frameBudgetAttempts { 0 }; // candidate frame-buffer admissions, from 1
std::atomic<bool> g_rejectAcoDeviceInjected { false }; // MZ_TEST_REJECT_ACO_DEVICE: once per process

// Adapters the network cannot run on: missing device features or a fixed Proton external-import failure.
// These do not change while the process lives, so later PrepareSession calls on the same adapter fail at once,
// without creating another VkInstance or rebuilding a network; the reason is logged once.
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
// Rotates to mochizuki_nr.1.log at 4 MiB so recent diagnostics are always preserved.
void LogLine(const char* line)
{
    if (!g_logging.load(std::memory_order_relaxed)) return;
    static std::mutex mutex;
    static bool identityWritten = false; // Protected by mutex; repeat after rotation or log deletion.
    static const std::wstring path = DataDirectory() + L"\\mochizuki_nr.log";
    static const std::wstring backupPath = DataDirectory() + L"\\mochizuki_nr.1.log";
    std::lock_guard lock(mutex);
    // Keep the runtime/core diagnostics bounded: rotate at 4 MiB to keep the most recent history.
    WIN32_FILE_ATTRIBUTE_DATA attr {};
    const bool exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) != FALSE;
    if (!exists || (!attr.nFileSizeHigh && !attr.nFileSizeLow))
        identityWritten = false;
    if (exists && (attr.nFileSizeHigh || attr.nFileSizeLow >= 4u * 1024u * 1024u))
    {
        DeleteFileW(backupPath.c_str());
        if (MoveFileW(path.c_str(), backupPath.c_str()))
            identityWritten = false;
    }
    FILE* f = _wfopen(path.c_str(), L"ab");
    if (!f)
        return;
    SYSTEMTIME t {};
    GetLocalTime(&t);
    if (!identityWritten)
    {
        std::fprintf(f, "%02u:%02u:%02u.%03u [mochizuki] MochizukiNrRuntime build version=" MZ_BUILD_VERSION
                        " commit=" MZ_BUILD_COMMIT " source_sha256=" MZ_BUILD_SOURCE_SHA256 "\r\n",
                     t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        identityWritten = true;
    }
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
        Interop,
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
    static const char* const names[] = { "device lost", "timeout", "out of memory", "unsupported", "invalid", "external interop unsupported", "error" };
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
    DXGI_FORMAT shared = DXGI_FORMAT_UNKNOWN; // typed, bit-compatible shared carrier; no implicit sRGB conversion
};

// The Vulkan format with the same bits as the D3D12 one, so a buffer copy carries the texels unchanged.
Format ColourFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return { VK_FORMAT_R16G16B16A16_SFLOAT, 8, DXGI_FORMAT_R16G16B16A16_FLOAT };
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return { VK_FORMAT_R32G32B32A32_SFLOAT, 16, DXGI_FORMAT_R32G32B32A32_FLOAT };
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return { VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, DXGI_FORMAT_R11G11B10_FLOAT };
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, DXGI_FORMAT_R10G10B10A2_UNORM };
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return { VK_FORMAT_R8G8B8A8_UNORM, 4, DXGI_FORMAT_R8G8B8A8_UNORM };
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return { VK_FORMAT_B8G8R8A8_UNORM, 4, DXGI_FORMAT_B8G8R8A8_UNORM };
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return { VK_FORMAT_B8G8R8A8_UNORM, 4, DXGI_FORMAT_B8G8R8X8_UNORM };
    // Upstream 743326d's further colour formats. The runtime blits them, so PrepareFrame takes them only where this GPU
    // can (Blittable). R9G9B9E5 is RE Engine's upscaler colour (Resident Evil Requiem, Monster Hunter Wilds): three
    // 9-bit mantissas sharing a 5-bit exponent, linear like R11G11B10.
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        return { VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4, DXGI_FORMAT_R9G9B9E5_SHAREDEXP };
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return { VK_FORMAT_R16G16B16A16_UNORM, 8, DXGI_FORMAT_R16G16B16A16_UNORM };
    case DXGI_FORMAT_R16G16B16A16_SNORM:
        return { VK_FORMAT_R16G16B16A16_SNORM, 8, DXGI_FORMAT_R16G16B16A16_SNORM };
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        return { VK_FORMAT_R8G8B8A8_SNORM, 4, DXGI_FORMAT_R8G8B8A8_SNORM };
    case DXGI_FORMAT_B5G6R5_UNORM:
        return { VK_FORMAT_R5G6B5_UNORM_PACK16, 2, DXGI_FORMAT_B5G6R5_UNORM };
    case DXGI_FORMAT_B5G5R5A1_UNORM:
        return { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, DXGI_FORMAT_B5G5R5A1_UNORM };
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return { VK_FORMAT_A4R4G4B4_UNORM_PACK16, 2, DXGI_FORMAT_B4G4R4A4_UNORM };
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

// Plan A: one network format per session, so a cutscene's transient colour-format flip (FF7 Rebirth flips
// R11G11B10 <-> RGBA16F at camera cuts) no longer forces a full network rebuild.
//
// The canonical format is not a constant: it is the *first* colour format the session sees, i.e. the format the
// network was already built for before Plan A existed. Until the game hands a *different* format, nothing changes
// at all -- no canonical image, no conversion pass, the same network key and buffers as before (so a game that
// always outputs one format, R11G11B10, RGBA16F or anything else, pays nothing). Only when a later frame's
// format differs is that frame converted into the canonical format by a compute pass (ConvertPass), the network
// and its history are kept, and the answer is converted back into the game's format for the readback. Frames in
// the canonical format itself keep taking the original path. vkCmdBlitImage cannot do the conversion: the two
// formats are in different Vulkan format compatibility classes.
//
// Both directions are supported for the R11G11B10 / RGBA16F pair (runtime_planA_convert_* and
// runtime_planA_unconvert_*, one module per destination format). Any other pair keeps the pre-Plan-A behaviour
// (a new key, i.e. a rebuild) and is logged once.

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

// The most passes a frame can ask for. Passes 2 and 3 have MochizukiNrControls::pass overrides; later
// layers inherit pass 1 with local tone 0. The host slider stops at 3. 30 is its separate layer mode.
// Every pass the network is built for costs a model-extent RGBA32F history image.
constexpr uint32_t kMaxPasses = 30;

// What SetControls last gave, sanitised. Until it is called, the core's defaults: today's output.
struct SessionControls
{
    nr::Controls model;      // detail_strength, colour_strength and passes come with each frame
    float history = 1.f;     // Runtime::set_history_strength
    float white = 1.f;       // Runtime::set_white_point
    uint32_t linearMode = 0; // MochizukiNrControls::linear_input: 0 auto, 1 on, 2 off
    bool compactTransfer = true;
    bool acoRequested = false;
    bool int4 = false, renodx = false;
    uint32_t networkModel = 1;
    nr::renodx::Settings reno;
    uint32_t maxPasses = 0;  // 0: the frame's passes
    uint32_t drsMode = 0;    // MochizukiNrControls::drs_mode: 0 exact, 1 auto, 2 always (Session::DrsExtent)
};

void DefaultControls(MochizukiNrControls& c)
{
    const nr::Controls model;
    const nr::PassControls pass;
    c = {};
    c.struct_size = sizeof c;
    c.network_model = 1;
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
    c.compact_transfer = 1; // product policy: reuse the RGBA32F keep when official store_native does not
    c.enlarge_mode = uint32_t(std::clamp(model.transfer, 0, 2));
    c.classic_scaler = uint32_t(std::clamp(model.classic_scaler, 0, 3));
    c.aco_mode = 1;
#define RDX_F(member, field, def, low, high) c.member = def;
#define RDX_U RDX_F
#define RDX_B RDX_F
#include "MochizukiRenoOptions.inc"
#undef RDX_F
#undef RDX_U
#undef RDX_B
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
    m.transfer = c.enlarge_mode <= 2 ? int(c.enlarge_mode) : 0;
    m.classic_scaler = c.classic_scaler <= 3 ? int(c.classic_scaler) : 1;
    out.compactTransfer = c.compact_transfer != 0;
    out.acoRequested = c.aco_mode != 0;
    out.int4 = c.int4_mixed != 0;
    out.networkModel = c.network_model <= 1 ? c.network_model : 1;
    out.renodx = c.renodx_enabled != 0;
    m.noise_every_frame = c.noise_every_frame != 0;
#define RDX_F(member, field, def, low, high) out.reno.field = San(c.member, low, high, def);
#define RDX_U(member, field, def, low, high) out.reno.field = std::clamp(c.member, uint32_t(low), uint32_t(high));
#define RDX_B(member, field, def, low, high) out.reno.field = c.member != 0;
#include "MochizukiRenoOptions.inc"
#undef RDX_F
#undef RDX_U
#undef RDX_B
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
    uint32_t computeFamily = VK_QUEUE_FAMILY_IGNORED;
    VkQueue computeQueue {};
    bool bufferDeviceAddress = false; // enabled on `device`; the network reads its arena through it where faster
    VkPhysicalDeviceMemoryProperties memory {};
    PFN_vkGetMemoryWin32HandlePropertiesKHR memoryHandleProperties {};
    PFN_vkImportSemaphoreWin32HandleKHR importSemaphore {};
    std::string name;
    // Proton borrows vkd3d's device and game command stream. Only background
    // builds submit independently, under the vkd3d queue lock.
    bool adoptedVkd3d = false;
    ID3D12DXVKInteropDevice* interop {};  // AddRef'd, with adoptedVkd3d
    ID3D12CommandQueue* interopQueue {};  // ours, DIRECT: the network builds' vkd3d queue (vk.queue)

    // Under Wine, when the game's D3D12 device is vkd3d-proton's, take its Vulkan objects instead of creating a
    // device: winevulkan cannot import a D3D12 resource handle, so the own-device route never gets its buffers
    // there. Following the interop of the Linux route (linux/src/pe/nr_pe_interop.cpp): ID3D12DXVKInteropDevice's
    // GetVulkanHandles, a D3D12 DIRECT queue of our own for the builds, whose vkd3d queue LockCommandQueue
    // serialises with the game's, and the physical device's support checked rather than requested, as the
    // Linux route's adopt() does. MZ_PROTON_DIRECT=0 keeps the own-device route. False: nothing was taken.
    bool AdoptVkd3d(ID3D12Device* d3d)
    {
        if (!d3d || !mochizuki_vkd3d::UnderWine())
            return false;
        if (const char* off = std::getenv("MZ_PROTON_DIRECT"); off && !std::strcmp(off, "0"))
        {
            nr::logf("[mochizuki] Proton direct path: off (MZ_PROTON_DIRECT=0); the own device is used");
            return false;
        }
        ID3D12DXVKInteropDevice* found = nullptr;
        if (FAILED(d3d->QueryInterface(__uuidof(ID3D12DXVKInteropDevice), reinterpret_cast<void**>(&found))) ||
            !found)
        {
            nr::logf("[mochizuki] Proton direct path: the D3D12 device has no ID3D12DXVKInteropDevice (not "
                     "vkd3d-proton); the own device is used");
            return false;
        }
        auto decline = [&](const char* why)
        {
            nr::logf("[mochizuki] Proton direct path declined: %s; the own device is used", why);
            if (interopQueue)
                interopQueue->Release();
            interopQueue = nullptr;
            found->Release();
            instance = VkInstance {};
            physical = VkPhysicalDevice {};
            device = VkDevice {};
            queue = buildQueue = VkQueue {};
            return false;
        };
        if (const char* mismatch = mochizuki_vkd3d::EntryPointMismatch(); *mismatch)
            return decline(mismatch);
        if (FAILED(found->GetVulkanHandles(&instance, &physical, &device)) || !instance || !physical || !device)
            return decline("GetVulkanHandles returned no handles");
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(d3d->CreateCommandQueue(&qd, IID_PPV_ARGS(&interopQueue))) || !interopQueue)
            return decline("no D3D12 queue for the network builds");
        UINT32 queueFamily = 0;
        if (FAILED(found->GetVulkanQueueInfo(interopQueue, &queue, &queueFamily)) || !queue)
            return decline("GetVulkanQueueInfo returned no queue");
        family = queueFamily;
        buildQueue = queue;
        computeFamily = VK_QUEUE_FAMILY_IGNORED;
        computeQueue = VkQueue {};

        VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        vkGetPhysicalDeviceProperties2(physical, &props);
        if (props.properties.apiVersion < VK_API_VERSION_1_3)
            return decline("the device offers less than Vulkan 1.3");
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR
        };
        VkPhysicalDeviceShaderFloat8FeaturesEXT fp8 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT };
        VkPhysicalDeviceVulkan11Features v11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features v12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features v13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wml {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR
        };
        VkPhysicalDeviceFeatures2 f2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &coop };
        coop.pNext = &fp8;
        fp8.pNext = &v11;
        v11.pNext = &v12;
        v12.pNext = &v13;
        v13.pNext = &wml;
        vkGetPhysicalDeviceFeatures2(physical, &f2);
        const struct { VkBool32 has; const char* name; } needed[] = {
            { coop.cooperativeMatrix, "cooperativeMatrix" },
            { fp8.shaderFloat8, "shaderFloat8" },
            { fp8.shaderFloat8CooperativeMatrix, "shaderFloat8CooperativeMatrix" },
            { v11.storageBuffer16BitAccess, "storageBuffer16BitAccess" },
            { v12.storageBuffer8BitAccess, "storageBuffer8BitAccess" },
            { v12.shaderFloat16, "shaderFloat16" },
            { v12.shaderInt8, "shaderInt8" },
            { v12.vulkanMemoryModel, "vulkanMemoryModel" },
            { v12.timelineSemaphore, "timelineSemaphore" },
            { v13.subgroupSizeControl, "subgroupSizeControl" },
            { v13.synchronization2, "synchronization2" },
            { wml.workgroupMemoryExplicitLayout, "workgroupMemoryExplicitLayout" },
            { wml.workgroupMemoryExplicitLayout8BitAccess, "workgroupMemoryExplicitLayout8BitAccess" },
            { wml.workgroupMemoryExplicitLayout16BitAccess, "workgroupMemoryExplicitLayout16BitAccess" },
        };
        for (const auto& n : needed)
            if (!n.has)
            {
                char text[128];
                std::snprintf(text, sizeof text, "the GPU lacks the Vulkan feature %s", n.name);
                return decline(text);
            }
        // The hook records the final request, including augmentation invisible
        // to vkd3d reflection. Only fall back when no created-device record exists.
        mz::device::Enabled enabled;
        const auto query = g_queryCreatedDevice.load(std::memory_order_acquire);
        const bool recorded = query && query(reinterpret_cast<uint64_t>(device), &enabled);
        if (!recorded)
        {
            UINT count = 0;
            std::vector<const char*> extensions;
            if (SUCCEEDED(found->GetDeviceExtensions(&count, nullptr)) && count)
            {
                extensions.resize(count);
                if (FAILED(found->GetDeviceExtensions(&count, extensions.data()))) extensions.clear();
                extensions.resize(std::min<size_t>(count, extensions.size()));
            }
            enabled.extensions = mz::device::ExtensionBits(static_cast<uint32_t>(extensions.size()), extensions.data());
            const VkPhysicalDeviceFeatures2* feats = nullptr;
            if (SUCCEEDED(found->GetDeviceFeatures(&feats)) && feats)
                enabled.features = mz::device::FeatureBits(feats->pNext);
        }
        nr::logf("[mochizuki] Proton enabled contract: source=%s extensions=0x%x features=0x%llx",
                 recorded ? "host vkCreateDevice record" : "vkd3d reflection", enabled.extensions,
                 static_cast<unsigned long long>(enabled.features));
        for (uint32_t i = 0; i < 4; ++i)
            if (!(enabled.extensions & (1u << i)))
                return decline((std::string("vkd3d-proton's device did not enable ") + mz::device::Extensions[i]).c_str());
        bufferDeviceAddress = (enabled.features & mz::device::BufferAddress) != 0;
        const struct { uint64_t bit; const char* name; } enabledNeeds[] = {
            { mz::device::CooperativeMatrix, "cooperativeMatrix" },
            { mz::device::Float8, "shaderFloat8" },
            { mz::device::Float8CooperativeMatrix, "shaderFloat8CooperativeMatrix" },
            { mz::device::Storage16, "storageBuffer16BitAccess" },
            { mz::device::Storage8, "storageBuffer8BitAccess" },
            { mz::device::Float16, "shaderFloat16" },
            { mz::device::Int8, "shaderInt8" },
            { mz::device::MemoryModel, "vulkanMemoryModel" },
            { mz::device::Timeline, "timelineSemaphore" },
            { mz::device::BufferAddress, "bufferDeviceAddress" },
            { mz::device::SubgroupSize, "subgroupSizeControl" },
            { mz::device::Synchronization2, "synchronization2" },
            { mz::device::WorkgroupLayout, "workgroupMemoryExplicitLayout" },
            { mz::device::Workgroup8, "workgroupMemoryExplicitLayout8BitAccess" },
            { mz::device::Workgroup16, "workgroupMemoryExplicitLayout16BitAccess" },
        };
        for (const auto& n : enabledNeeds)
            if (!(enabled.features & n.bit))
            {
                char text[160];
                std::snprintf(text, sizeof text, "vkd3d-proton's device did not enable the Vulkan feature %s", n.name);
                return decline(text);
            }
        // The fences stay shared D3D12 fences imported as timeline semaphores, as on the own device under Proton
        // (vkd3d-proton exports them as D3D12_FENCE handles). No memory is imported on this path.
        importSemaphore = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR"));
        if (!importSemaphore)
            return decline("vkImportSemaphoreWin32HandleKHR is not enabled on vkd3d-proton's device");
        memoryHandleProperties = nullptr;
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        name = props.properties.deviceName;
        interop = found;
        adoptedVkd3d = true;
        nr::logf("[mochizuki] Proton direct path: vkd3d-proton's device %s, queue family %u, bufferDeviceAddress %d; "
                 "frames record in the game's current command stream; background builds hold the vkd3d queue",
                 name.c_str(), family, bufferDeviceAddress ? 1 : 0);
        return true;
    }

    // Direct path, a frame's submit: drains `producer` (the game queue the frame came from: its signal of the
    // produced value is then on the GPU queue ahead of the wait) and holds its vkd3d queue in `hold`; `out` is that
    // queue's VkQueue. The command buffers are of `family`, so a producer of another family is refused.
    VkResult HoldProducer(ID3D12CommandQueue* producer, mochizuki_vkd3d::QueueHold& hold, VkQueue& out) const
    {
        if (!adoptedVkd3d || !producer)
            return VK_ERROR_INITIALIZATION_FAILED;
        VkQueue q {};
        UINT32 f = 0;
        if (FAILED(interop->GetVulkanQueueInfo(producer, &q, &f)) || !q)
        {
            nr::logf("[mochizuki] Proton direct path: GetVulkanQueueInfo failed for the game queue");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (f != family)
        {
            nr::logf("[mochizuki] Proton direct path: the game queue is of family %u, the frames' of %u",
                     unsigned(f), family);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (FAILED(interop->LockCommandQueue(producer)))
        {
            nr::logf("[mochizuki] Proton direct path: LockCommandQueue failed for the game queue");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        hold.interop = interop;
        hold.queue = producer;
        out = q;
        return VK_SUCCESS;
    }

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
        // A stale second ICD can list the same adapter again. Check candidates newest first, so a missing
        // capability in one driver does not hide a usable driver on the game's LUID.
        struct Candidate
        {
            VkPhysicalDevice device {};
            std::string name;
            uint32_t driverVersion = 0;
            size_t index = 0;
        };
        std::vector<Candidate> matches;
        for (VkPhysicalDevice p : all)
        {
            VkPhysicalDeviceDriverProperties driver { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
            VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &driver };
            VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
            vkGetPhysicalDeviceProperties2(p, &props);
            if (!id.deviceLUIDValid || std::memcmp(id.deviceLUID, &luid, sizeof luid))
                continue;
            const uint32_t api = props.properties.apiVersion;
            nr::logf("[mochizuki] Vulkan device %zu on the adapter: %s, driver %s (%s), driverVersion 0x%08X, Vulkan "
                     "%u.%u.%u",
                     matches.size(), props.properties.deviceName, driver.driverName, driver.driverInfo,
                     props.properties.driverVersion, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
                     VK_API_VERSION_PATCH(api));
            matches.push_back({ p, props.properties.deviceName, props.properties.driverVersion, matches.size() });
        }
        if (matches.empty())
        {
            char text[112];
            std::snprintf(text, sizeof text,
                          "[unsupported] no Vulkan device matches the game's D3D12 adapter (%zu Vulkan devices)",
                          all.size());
            throw std::runtime_error(text);
        }
        if (hooks.unsupported)
            throw std::runtime_error("[unsupported] MZ_TEST_UNSUPPORTED: the capability check failed (test hook)");

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

        // The capability precheck names what vkCreateDevice would refuse. Check Vulkan 1.3 and the
        // extensions before querying their feature structures, using each candidate's own queue families.
        std::vector<VkQueueFamilyProperties> families;
        std::vector<VkExtensionProperties> offered;
        const auto check = [&](VkPhysicalDevice p) -> std::string
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(p, &count, nullptr);
            families.assign(count, {});
            vkGetPhysicalDeviceQueueFamilyProperties(p, &count, families.data());
            family = count;
            computeFamily = VK_QUEUE_FAMILY_IGNORED;
            for (uint32_t i = 0; i < count; ++i)
                if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                    (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                {
                    family = i;
                    break;
                }
            if (family == count)
                return "[unsupported] no graphics and compute queue family";
            for (uint32_t i = 0; i < count; ++i)
                if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                    !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                    families[i].timestampValidBits >= families[family].timestampValidBits)
                {
                    computeFamily = i;
                    break;
                }
            VkPhysicalDeviceProperties props {};
            vkGetPhysicalDeviceProperties(p, &props);
            if (props.apiVersion < VK_API_VERSION_1_3)
            {
                char text[96];
                std::snprintf(text, sizeof text, "[unsupported] the device offers Vulkan %u.%u; 1.3 is required",
                              VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
                return text;
            }
            offered = VkEnumerate<VkExtensionProperties>(
                "vkEnumerateDeviceExtensionProperties", [p](uint32_t* n, VkExtensionProperties* e)
                { return vkEnumerateDeviceExtensionProperties(p, nullptr, n, e); });
            for (const char* e : extensions)
                if (std::none_of(offered.begin(), offered.end(),
                                 [e](const VkExtensionProperties& x) { return !std::strcmp(x.extensionName, e); }))
                    return std::string("[unsupported] the driver lacks ") + e;
            VkPhysicalDeviceCooperativeMatrixFeaturesKHR hasCoop {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR
            };
            VkPhysicalDeviceShaderFloat8FeaturesEXT hasFp8 {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT
            };
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
            vkGetPhysicalDeviceFeatures2(p, &has);
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
                    return std::string("[unsupported] the driver lacks the Vulkan feature ") + f.name;
            // Optional: without it the network takes its descriptor reads.
            bufferDeviceAddress = has12.bufferDeviceAddress == VK_TRUE;
            return {};
        };
        std::sort(matches.begin(), matches.end(), [](const Candidate& a, const Candidate& b)
        {
            return a.driverVersion != b.driverVersion ? a.driverVersion > b.driverVersion : a.index < b.index;
        });
        std::string refused;
        for (Candidate& c : matches)
        {
            const std::string reason = check(c.device);
            if (reason.empty())
            {
                physical = c.device;
                name = std::move(c.name);
                if (matches.size() > 1)
                    nr::logf("[mochizuki] %zu Vulkan devices share this adapter (a duplicate driver); using device %zu",
                             matches.size(), c.index);
                break;
            }
            if (matches.size() > 1)
                nr::logf("[mochizuki] Vulkan device %zu fails the check: %s", c.index, reason.c_str());
            if (refused.empty())
                refused = reason;
        }
        if (!physical)
        {
            if (matches.size() > 1)
                refused += " (" + std::to_string(matches.size()) +
                           " Vulkan devices share this adapter, none passes: a duplicate or stale driver may be "
                           "installed)";
            throw std::runtime_error(refused);
        }
        f12.bufferDeviceAddress = bufferDeviceAddress ? VK_TRUE : VK_FALSE;
        // Official Kernel::create chains pipeline robustness when the physical device offers it.
        // Enable the extension on the device this runtime creates, or that chain is illegal.
        std::vector<const char*> enabled(std::begin(extensions), std::end(extensions));
        VkPhysicalDevicePipelineRobustnessFeaturesEXT robust {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT
        };
        {
            VkPhysicalDevicePipelineRobustnessFeaturesEXT hasRobust {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT
            };
            VkPhysicalDeviceFeatures2 robustQuery { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &hasRobust };
            vkGetPhysicalDeviceFeatures2(physical, &robustQuery);
            const bool listed = std::any_of(offered.begin(), offered.end(), [](const VkExtensionProperties& p)
            { return !std::strcmp(p.extensionName, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME); });
            if (hasRobust.pipelineRobustness && listed)
            {
                robust.pipelineRobustness = VK_TRUE;
                wml.pNext = &robust;
                enabled.push_back(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
            }
        }
        // MZ_PROBE_PIPELINE_BINARY=1 only (mz_interpose.h): two more extensions, and their features after wml.
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
        VkDeviceQueueCreateInfo queues[2] { qi, {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO} };
        queues[1].queueFamilyIndex = computeFamily; queues[1].queueCount = 1; queues[1].pQueuePriorities = priorities;
        di.queueCreateInfoCount = computeFamily == VK_QUEUE_FAMILY_IGNORED ? 1 : 2;
        di.pQueueCreateInfos = queues;
        di.enabledExtensionCount = uint32_t(enabled.size());
        di.ppEnabledExtensionNames = enabled.data();
        // prepare() adds pipeline-binary extensions only when a bundle was configured.
        // Reassign the name pointer afterwards: push_back can move the vector.
        // The native chain is kept so a recoverable ACO rejection can create the same device without it.
        const void* const nativeNext = di.pNext;
        const size_t nativeExtensions = enabled.size();
        nr::binary::Features acoFeatures;
        acoFeatures.prepare(physical, di, enabled);
        di.enabledExtensionCount = uint32_t(enabled.size());
        di.ppEnabledExtensionNames = enabled.data();
        VkDevice createdDevice = VK_NULL_HANDLE;
        VkResult deviceResult = vkCreateDevice(physical, &di, nullptr, &createdDevice);
        if (deviceResult == VK_SUCCESS && hooks.rejectAcoDevice && acoFeatures.active &&
            !g_rejectAcoDeviceInjected.exchange(true))
        {
            if (createdDevice)
                vkDestroyDevice(createdDevice, nullptr);
            createdDevice = VK_NULL_HANDLE;
            deviceResult = VK_ERROR_INITIALIZATION_FAILED;
            nr::logf("[mochizuki] MZ_TEST_REJECT_ACO_DEVICE: pipeline-binary device create is treated as rejected");
        }
        if (deviceResult != VK_SUCCESS && acoFeatures.active && KindOf(deviceResult) == NrError::Other)
        {
            if (createdDevice)
                vkDestroyDevice(createdDevice, nullptr);
            createdDevice = VK_NULL_HANDLE;
            nr::logf("[mochizuki] vkCreateDevice rejected pipeline binaries (VkResult %d); creating the device with "
                     "the driver's compiler",
                     int(deviceResult));
            nr::binary::configure("", {}, "", {});
            nr::binary::disable("the driver rejected pipeline binaries; the driver's compiler is used");
            acoFeatures.active = false;
            di.pNext = nativeNext;
            enabled.resize(nativeExtensions);
            di.enabledExtensionCount = uint32_t(enabled.size());
            di.ppEnabledExtensionNames = enabled.data();
            deviceResult = vkCreateDevice(physical, &di, nullptr, &createdDevice);
        }
        if (deviceResult != VK_SUCCESS && computeFamily != VK_QUEUE_FAMILY_IGNORED &&
            KindOf(deviceResult) != NrError::DeviceLost) {
            if (createdDevice) vkDestroyDevice(createdDevice, nullptr);
            createdDevice = VK_NULL_HANDLE;
            nr::logf("[mochizuki] device rejected the optional compute family (%d); inline keeps the graphics queue",
                     int(deviceResult));
            computeFamily = VK_QUEUE_FAMILY_IGNORED;
            di.queueCreateInfoCount = 1;
            di.pQueueCreateInfos = &qi;
            deviceResult = vkCreateDevice(physical, &di, nullptr, &createdDevice);
        }
        VkCheck(deviceResult, "vkCreateDevice");
        device = createdDevice;
        if (acoFeatures.active)
            nr::binary::mark(device);
        probe.Report(device, physical);
        vkGetDeviceQueue(device, family, 0, &queue);
        if (computeFamily != VK_QUEUE_FAMILY_IGNORED) vkGetDeviceQueue(device, computeFamily, 0, &computeQueue);
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
        if (adoptedVkd3d)
        {
            // vkd3d-proton's: the game's device, never destroyed here.
            if (interopQueue)
                interopQueue->Release();
            if (interop)
                interop->Release();
            interopQueue = nullptr;
            interop = nullptr;
            adoptedVkd3d = false;
        }
        else
        {
            if (device)
                vkDestroyDevice(device, nullptr);
            if (instance)
                vkDestroyInstance(instance, nullptr);
        }
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
    // Proton direct path: `buffer` is vkd3d-proton's own (not destroyed here) and the resource starts `offset` bytes
    // into it. Zero, and the buffer ours, everywhere else.
    VkDeviceSize offset = 0;
    bool borrowed = false;

    void Create(ID3D12Device* d, Vulkan& vk, UINT64 bytes, const char* name, bool probe = false)
    {
        const bool wine = mochizuki_vkd3d::UnderWine();
        const auto handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkMemoryRequirements req {};
        VkMemoryWin32HandlePropertiesKHR props { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
        VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        // Real buffers retain the 16 MB shared-handle workaround; only Wine's probe is 64 KiB.
        rd.Width = mz::transport::BufferBytes(bytes, wine && probe);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto describe = [&](const char* api, VkResult result, HRESULT hr)
        {
            nr::logf("[mochizuki] shared buffer name=%s requested=%llu size=%llu handleType=D3D12_RESOURCE(0x%X) "
                     "memoryTypeBits=0x%X handleMemoryTypeBits=0x%X compatibleMemoryTypeBits=0x%X "
                     "allocationSize=%llu API=%s VkResult=%d HRESULT=0x%08lX",
                     name, static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(rd.Width),
                     unsigned(handleType), req.memoryTypeBits, props.memoryTypeBits,
                     req.memoryTypeBits & props.memoryTypeBits, static_cast<unsigned long long>(ai.allocationSize),
                     api, int(result), static_cast<unsigned long>(hr));
        };
        auto d3dCheck = [&](HRESULT hr, const char* api)
        {
            if (SUCCEEDED(hr)) return;
            describe(api, VK_SUCCESS, hr);
            const bool fixed = wine && (hr == E_INVALIDARG || hr == E_NOTIMPL || hr == E_NOINTERFACE);
            char text[256];
            std::snprintf(text, sizeof text, "shared buffer %s: %s failed, HRESULT 0x%08lX%s", name, api,
                          static_cast<unsigned long>(hr), fixed ? "; Proton cross-device external interop unsupported" : "");
            throw NrError(fixed ? NrError::Interop : KindOf(hr), text, VK_SUCCESS, hr);
        };
        auto importCheck = [&](VkResult result, const char* api, bool externalImport)
        {
            if (result == VK_SUCCESS) return;
            describe(api, result, S_OK);
            char text[256];
            std::snprintf(text, sizeof text, "shared buffer %s: %s failed, VkResult %d%s", name, api, int(result),
                          externalImport && mz::transport::FixedImportFailure(wine, probe, result)
                              ? "; Proton cross-device D3D12_RESOURCE import unsupported (not memory pressure)" : "");
            throw NrError(externalImport && mz::transport::FixedImportFailure(wine, probe, result)
                              ? NrError::Interop : KindOf(result), text, result);
        };
        // Proton direct path: no cross-API sharing to do. The D3D12 buffer is already memory of the device the
        // network runs on; its VkBuffer and offset come from vkd3d-proton.
        if (vk.adoptedVkd3d)
        {
            Check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&resource)),
                  "buffer (Proton direct path)");
            UINT64 handleValue = 0, at = 0;
            if (const HRESULT hr = vk.interop->GetVulkanResourceInfo(resource, &handleValue, &at);
                FAILED(hr) || !handleValue)
            {
                resource->Release();
                resource = nullptr;
                Check(FAILED(hr) ? hr : E_FAIL, "GetVulkanResourceInfo (Proton direct path)");
            }
            buffer = reinterpret_cast<VkBuffer>(handleValue);
            offset = at;
            borrowed = true;
            return;
        }
        d3dCheck(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&resource)), "ID3D12Device::CreateCommittedResource");
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
            d3dCheck(hr, "ID3D12Device::CreateSharedHandle");
        }
        ++alive;

        VkExternalMemoryBufferCreateInfo external { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external };
        bi.size = rd.Width;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        // The whole-chain compute path and the segments/passthrough on graphics
        // copy through the same buffers; like the adapter images, shared by both
        // families instead of exclusive ownership transfers.
        const uint32_t queueFamilies[] {vk.family, vk.computeFamily};
        if (vk.computeQueue && vk.computeFamily != vk.family)
        {
            bi.sharingMode = VK_SHARING_MODE_CONCURRENT;
            bi.queueFamilyIndexCount = 2;
            bi.pQueueFamilyIndices = queueFamilies;
        }
        importCheck(vkCreateBuffer(vk.device, &bi, nullptr, &buffer), "vkCreateBuffer", false);
        vkGetBufferMemoryRequirements(vk.device, buffer, &req);
        ai.allocationSize = std::max<UINT64>(req.size, d->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes);
        importCheck(vk.memoryHandleProperties(vk.device, handleType, handle, &props),
                    "vkGetMemoryWin32HandlePropertiesKHR", true);
        VkMemoryDedicatedAllocateInfo dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.buffer = buffer;
        VkImportMemoryWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated };
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import.handle = handle;
        ai.pNext = &import;
        if (!(req.memoryTypeBits & props.memoryTypeBits))
            importCheck(VK_ERROR_INVALID_EXTERNAL_HANDLE, "memory type intersection", true);
        ai.memoryTypeIndex =
            vk.MemoryType(req.memoryTypeBits & props.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        importCheck(vkAllocateMemory(vk.device, &ai, nullptr, &memory), "vkAllocateMemory", true);
        importCheck(vkBindBufferMemory(vk.device, buffer, memory, 0), "vkBindBufferMemory", true);
        describe("import ready", VK_SUCCESS, S_OK);
    }

    void Release(VkDevice device)
    {
        if (buffer && !borrowed)
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

    // `usage` adds to kImageUsage: Plan A's canonical image is also written by a compute pass, which needs
    // STORAGE_IMAGE (its blit-free conversion pass, ConvertPass).
    void Create(Vulkan& vk, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage = kImageUsage)
    {
        VkImageCreateInfo ci { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = { w, h, 1 };
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        // Native pre/post blocks may directly sample/store this adapter image.
        // The phase timeline orders accesses across both queue families.
        const uint32_t queueFamilies[] {vk.family, vk.computeFamily};
        if (vk.computeQueue) {ci.sharingMode = VK_SHARING_MODE_CONCURRENT; ci.queueFamilyIndexCount = 2; ci.pQueueFamilyIndices = queueFamilies;}
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

// Plan A's colour-format conversion, both directions, as compute passes rather than blits:
//
//   input  (runtime_planA_convert.comp)    the game's frame      -> the canonical image the network runs on
//   output (runtime_planA_unconvert.comp)  the network's answer -> the game's image the readback expects
//
// A blit cannot do either: vkCmdBlitImage only converts *inside* one Vulkan format compatibility class, keyed by
// (bits per pixel, component count), and the pair FF7 Rebirth flips between -- R11G11B10_UFLOAT (32 bpp) and
// R16G16B16A16_SFLOAT (64 bpp) -- is in different classes. Recording such a blit is an invalid command inside the
// game's own command list: Close() then fails with E_INVALIDARG and the device is removed with
// DXGI_ERROR_INVALID_CALL, which surfaces later as the DXGI_ERROR_DEVICE_REMOVED crash. A compute pass has no such
// rule: it reads the source through a sampler (no format qualifier on the binding) and writes the destination
// through a storage image (format-qualified), and the texel values carry over unchanged. ColourConvertible
// decides whether this can be built at all; both directions are checked before.
//
// The output pass is not optional once the input one is taken. The readback is not format-agnostic: the buffer's
// row pitch and the D3D12 CopyTextureRegion placed footprint both come from colourFootprint, which is built for
// the *game's* format. Copying the canonical answer out with that layout reads a 4-byte-per-texel image as if it
// were 8-byte-per-texel -- half of every row, at twice the stride -- so the colours come out wrong and every row
// is offset by half its width. Hence the answer is converted back into colourImage (the game's format) first, and
// everything downstream of the copy is unchanged.
//
// A storage image's format qualifier belongs to the module, so the output pass exists once per destination
// format (see PLAN_A_DST_FORMAT in build_network.py). Only the formats ColourConvertible accepts are built;
// otherwise a frame could be canonicalised with no way to get the answer back into the game's layout.
//
// One pass per frame's buffer set, created with it (the images are the set's) and destroyed with it. The passes
// are built once per session and shared: only the descriptor sets are per set, since they name the set's images.
struct ConvertPass
{
    // The session-wide half: sampler, pipeline and layouts, none of which depend on the images.
    struct Pipeline
    {
        VkSampler sampler {};
        VkDescriptorSetLayout setLayout {};
        VkPipelineLayout layout {};

        // The input pass's module is format-qualified as well (PLAN_A_DST_FORMAT = the canonical format), so
        // there is one input pipeline per canonical format too. Keyed by the canonical VkFormat.
        std::vector<std::pair<VkFormat, VkPipeline>> convertPipelines;
        // The output pass's module is format-qualified (PLAN_A_DST_FORMAT), so there is one pipeline per
        // destination format the runtime offers a canonical channel for. Keyed by that VkFormat.
        std::vector<std::pair<VkFormat, VkPipeline>> unconvertPipelines;

        // Reads a SPIR-V module from dlssnr-amd/shaders/runtime/, the same directory the core loads its
        // runtime passes from.
        static VkShaderModule LoadModule(Vulkan& vk, const char* name)
        {
            const std::string path = RootUtf8(DataDirectory()) + "/dlssnr-amd/shaders/runtime/" + name;
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                throw std::runtime_error(std::string("plan A's conversion pass is missing (") + path + ")");
            const std::streamsize bytes = file.tellg();
            if (bytes <= 0 || (bytes % 4) || bytes > (1 << 20))
                throw std::runtime_error(std::string("plan A's conversion pass is not a SPIR-V module (") + path + ")");
            std::vector<uint32_t> code(size_t(bytes) / 4);
            file.seekg(0);
            file.read(reinterpret_cast<char*>(code.data()), bytes);
            if (!file || code[0] != 0x07230203u)
                throw std::runtime_error(std::string("plan A's conversion pass is not a SPIR-V module (") + path + ")");

            VkShaderModuleCreateInfo si { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            si.codeSize = size_t(bytes);
            si.pCode = code.data();
            VkShaderModule made {};
            VkCheck(vkCreateShaderModule(vk.device, &si, nullptr, &made), "vkCreateShaderModule (plan A convert)");
            return made;
        }

        static VkPipeline MakePipeline(Vulkan& vk, VkPipelineLayout layout, VkShaderModule module)
        {
            VkComputePipelineCreateInfo ci { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = module;
            ci.stage.pName = "main";
            ci.layout = layout;
            VkPipeline made {};
            VkCheck(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &ci, nullptr, &made),
                    "vkCreateComputePipelines (plan A convert)");
            return made;
        }

        // `formats` are the game's colour formats the output pass is built for; each needs its own module
        // (the storage image qualifier) and pipeline. One of them failing does not stop the others, nor the
        // input pass: ColourConvertible only offers the channel for a format that got both.
        void Create(Vulkan& vk, const std::vector<VkFormat>& formats)
        {
            VkSamplerCreateInfo sci { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sci.magFilter = sci.minFilter = VK_FILTER_NEAREST; // texelFetch: exact, no filtering
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sci.addressModeU = sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
            VkCheck(vkCreateSampler(vk.device, &sci, nullptr, &sampler), "vkCreateSampler (plan A convert)");

            // binding 0: the source, sampled, in whatever format it is in. binding 1: the destination image.
            VkDescriptorSetLayoutBinding bindings[2] {};
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo sli { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            sli.bindingCount = 2;
            sli.pBindings = bindings;
            VkCheck(vkCreateDescriptorSetLayout(vk.device, &sli, nullptr, &setLayout),
                    "vkCreateDescriptorSetLayout (plan A convert)");

            VkPushConstantRange push {};
            push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            push.offset = 0;
            push.size = sizeof(uint32_t) * 2; // {width, height}: the valid subrect
            VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pli.setLayoutCount = 1;
            pli.pSetLayouts = &setLayout;
            pli.pushConstantRangeCount = 1;
            pli.pPushConstantRanges = &push;
            VkCheck(vkCreatePipelineLayout(vk.device, &pli, nullptr, &layout),
                    "vkCreatePipelineLayout (plan A convert)");

            for (const VkFormat format : formats)
            {
                VkShaderModule module {};
                VkPipeline made {};
                try
                {
                    module = LoadModule(vk, ConvertModule(format));
                    made = MakePipeline(vk, layout, module);
                }
                catch (const std::exception& e)
                {
                    nr::logf("[mochizuki] plan A: no input conversion pass into VkFormat %d (%s); it cannot be "
                             "a session's canonical format", int(format), e.what());
                    if (module) vkDestroyShaderModule(vk.device, module, nullptr);
                    continue;
                }
                vkDestroyShaderModule(vk.device, module, nullptr);
                convertPipelines.emplace_back(format, made);
            }

            for (const VkFormat format : formats)
            {
                VkShaderModule module {};
                VkPipeline made {};
                try
                {
                    module = LoadModule(vk, UnconvertModule(format));
                    made = MakePipeline(vk, layout, module);
                }
                catch (const std::exception& e)
                {
                    nr::logf("[mochizuki] plan A: no output conversion pass for VkFormat %d (%s); that colour "
                             "format keeps its own path", int(format), e.what());
                    if (module) vkDestroyShaderModule(vk.device, module, nullptr);
                    continue;
                }
                vkDestroyShaderModule(vk.device, module, nullptr);
                unconvertPipelines.emplace_back(format, made);
            }
        }

        // NrInlineComputeMotion: the same layout with a LINEAR sampler and a 16-byte push {width, height,
        // srcWidth, srcHeight}; convertPipelines holds runtime_mv_rescale_* keyed by the motion format. A missing
        // module only leaves that motion format on its blit.
        void CreateMotion(Vulkan& vk)
        {
            VkSamplerCreateInfo sci { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sci.addressModeU = sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            VkCheck(vkCreateSampler(vk.device, &sci, nullptr, &sampler), "vkCreateSampler (motion rescale)");
            VkDescriptorSetLayoutBinding bindings[2] {};
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo sli { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            sli.bindingCount = 2;
            sli.pBindings = bindings;
            VkCheck(vkCreateDescriptorSetLayout(vk.device, &sli, nullptr, &setLayout),
                    "vkCreateDescriptorSetLayout (motion rescale)");
            VkPushConstantRange push {};
            push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            push.size = sizeof(uint32_t) * 4;
            VkPipelineLayoutCreateInfo pli { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pli.setLayoutCount = 1;
            pli.pSetLayouts = &setLayout;
            pli.pushConstantRangeCount = 1;
            pli.pPushConstantRanges = &push;
            VkCheck(vkCreatePipelineLayout(vk.device, &pli, nullptr, &layout), "vkCreatePipelineLayout (motion rescale)");
            const std::pair<VkFormat, const char*> modules[] {
                { VK_FORMAT_R16G16_SFLOAT, "runtime_mv_rescale_rg16f.spv" },
                { VK_FORMAT_R32G32_SFLOAT, "runtime_mv_rescale_rg32f.spv" },
                { VK_FORMAT_R16G16B16A16_SFLOAT, "runtime_mv_rescale_rgba16f.spv" },
                { VK_FORMAT_R32G32B32A32_SFLOAT, "runtime_mv_rescale_rgba32f.spv" } };
            for (const auto& [format, name] : modules)
            {
                VkFormatProperties fp {};
                vkGetPhysicalDeviceFormatProperties(vk.physical, format, &fp);
                if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
                    !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
                {
                    nr::logf("[mochizuki] motion rescale: VkFormat %d has no linear-sampled/storage support; it keeps "
                             "its blit", int(format));
                    continue;
                }
                VkShaderModule module {};
                try
                {
                    module = LoadModule(vk, name);
                    convertPipelines.emplace_back(format, MakePipeline(vk, layout, module));
                }
                catch (const std::exception& e)
                {
                    nr::logf("[mochizuki] motion rescale: no compute pass for VkFormat %d (%s); it keeps its blit",
                             int(format), e.what());
                }
                if (module) vkDestroyShaderModule(vk.device, module, nullptr);
            }
        }

        // The input module for a canonical format (the destination of the input pass).
        static const char* ConvertModule(VkFormat format)
        {
            switch (format)
            {
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                return "runtime_planA_convert_r11g11b10.spv";
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                return "runtime_planA_convert_rgba16f.spv";
            default:
                throw std::runtime_error("plan A: no input conversion module for this format");
            }
        }

        // The input pipeline into canonical `format`, or null when there is none.
        VkPipeline Convert(VkFormat format) const
        {
            for (const auto& [f, p] : convertPipelines)
                if (f == format)
                    return p;
            return nullptr;
        }

        // The output module for a game's colour format. Only the formats listed here are built, and
        // Unconvertible keeps them and the module list in step (see ColourConvertible).
        static const char* UnconvertModule(VkFormat format)
        {
            switch (format)
            {
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                return "runtime_planA_unconvert_r11g11b10.spv";
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                return "runtime_planA_unconvert_rgba16f.spv";
            default:
                throw std::runtime_error("plan A: no output conversion module for this format");
            }
        }

        // The output pipeline for `format`, or null when the format has none (then the channel is not offered).
        VkPipeline Unconvert(VkFormat format) const
        {
            for (const auto& [f, p] : unconvertPipelines)
                if (f == format)
                    return p;
            return nullptr;
        }

        void Release(VkDevice device)
        {
            for (const auto& [format, made] : unconvertPipelines)
                vkDestroyPipeline(device, made, nullptr);
            unconvertPipelines.clear();
            for (const auto& [format, made] : convertPipelines)
                vkDestroyPipeline(device, made, nullptr);
            convertPipelines.clear();
            if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
            if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            if (sampler) vkDestroySampler(device, sampler, nullptr);
            *this = {};
        }
    };

    // The per-set half. The input set names (colourImage -> canonicalColour); the output set names the same two
    // images the other way round, so one descriptor set could serve both -- but a storage image's format
    // qualifier is baked into the module, so the output pass is a different pipeline, and it keeps its own set
    // so a frame that is not canonicalised never binds one it will not run.
    VkImageView srcView {}, dstView {};
    VkDescriptorPool pool {};
    VkDescriptorSet set {};

    // Plan A's output-side set: canonicalColour -> colourImage, so the readback below finds the answer in the
    // game's own format. Zero when the frame is not canonicalised or the format has no output pass.
    VkImageView unconvertSrcView {}, unconvertDstView {};
    VkDescriptorPool unconvertPool {};
    VkDescriptorSet unconvertSet {};
    // The game format an output pipeline was built for, or UNDEFINED when there is none. Recorded rather than
    // looked up so CanUnconvert cannot be fooled by a module name whose pipeline failed to build.
    VkFormat unconvertFor = VK_FORMAT_UNDEFINED;
    // The canonical format the input pipeline was built for (the destination of the input pass), or UNDEFINED.
    VkFormat convertFor = VK_FORMAT_UNDEFINED;

    // `srcImage` in the game's format, `dstImage` in the canonical one. Both GENERAL, as the caller leaves them.
    void Create(Vulkan& vk, const Pipeline& p, VkImage srcImage, VkFormat srcFormat, VkImage dstImage,
                VkFormat dstFormat)
    {
        if (!p.Convert(dstFormat))
            throw std::logic_error("plan A: no input conversion pipeline for the canonical colour format");
        // Made into locals and moved out at the end, so a throw anywhere leaves nothing behind.
        auto view = [&](VkImage image, VkFormat format)
        {
            VkImageViewCreateInfo vi { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vi.image = image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkImageView made {};
            VkCheck(vkCreateImageView(vk.device, &vi, nullptr, &made), "vkCreateImageView (plan A convert)");
            return made;
        };
        VkImageView src = view(srcImage, srcFormat);
        VkImageView dst {};
        VkDescriptorPool madePool {};
        VkDescriptorSet madeSet {};
        struct Guard
        {
            VkDevice device;
            VkImageView src {}, dst {};
            VkDescriptorPool pool {};
            VkDescriptorSet set {};
            ~Guard()
            {
                if (set && pool) vkFreeDescriptorSets(device, pool, 1, &set);
                if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
                if (src) vkDestroyImageView(device, src, nullptr);
                if (dst) vkDestroyImageView(device, dst, nullptr);
            }
        } guard { vk.device, src, dst };
        dst = view(dstImage, dstFormat);

        VkDescriptorPoolSize sizes[2] {};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = 1;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo pi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.maxSets = 1;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = sizes;
        VkCheck(vkCreateDescriptorPool(vk.device, &pi, nullptr, &madePool), "vkCreateDescriptorPool (plan A convert)");
        guard.pool = madePool;

        VkDescriptorSetAllocateInfo ai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = madePool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &p.setLayout;
        VkCheck(vkAllocateDescriptorSets(vk.device, &ai, &madeSet), "vkAllocateDescriptorSets (plan A convert)");
        guard.set = madeSet;

        VkDescriptorImageInfo infos[2] {};
        infos[0].sampler = p.sampler;
        infos[0].imageView = src;
        infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[1].imageView = dst;
        infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2] {};
        for (uint32_t i = 0; i < 2; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = madeSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(vk.device, 2, writes, 0, nullptr);

        srcView = src;
        dstView = dst;
        pool = madePool;
        set = madeSet;
        convertFor = dstFormat;
        guard.src = guard.dst = nullptr;
        guard.pool = nullptr;
        guard.set = nullptr;

        // The output set: the same two images, the other way round, so the network's answer lands back in
        // the game's format before the readback. Only when an output pipeline exists for that format --
        // CanonicalFor does not offer the channel otherwise, so this is belt and braces.
        //
        // The pipeline is selected by the *source* format, which is the game's: the pass reads the canonical
        // image and stores into `srcImage`, so the game's format is the destination the module has to name.
        if (!p.Unconvert(srcFormat))
            return;
        VkImageView backSrc = view(dstImage, dstFormat);
        VkImageView backDst = view(srcImage, srcFormat);
        VkDescriptorPool backPool {};
        VkDescriptorSet backSet {};
        struct BackGuard
        {
            VkDevice device;
            VkImageView src {}, dst {};
            VkDescriptorPool pool {};
            VkDescriptorSet set {};
            ~BackGuard()
            {
                if (set && pool) vkFreeDescriptorSets(device, pool, 1, &set);
                if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
                if (src) vkDestroyImageView(device, src, nullptr);
                if (dst) vkDestroyImageView(device, dst, nullptr);
            }
        } backGuard { vk.device, backSrc, backDst };
        try
        {
            VkDescriptorPoolCreateInfo bpi { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            bpi.maxSets = 1;
            bpi.poolSizeCount = 2;
            bpi.pPoolSizes = sizes;
            VkCheck(vkCreateDescriptorPool(vk.device, &bpi, nullptr, &backPool), "vkCreateDescriptorPool (plan A unconvert)");
            backGuard.pool = backPool;

            VkDescriptorSetAllocateInfo bai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            bai.descriptorPool = backPool;
            bai.descriptorSetCount = 1;
            bai.pSetLayouts = &p.setLayout;
            VkCheck(vkAllocateDescriptorSets(vk.device, &bai, &backSet), "vkAllocateDescriptorSets (plan A unconvert)");
            backGuard.set = backSet;

            infos[0].imageView = backSrc;
            infos[1].imageView = backDst;
            for (uint32_t i = 0; i < 2; ++i)
            {
                writes[i].dstSet = backSet;
                writes[i].pImageInfo = &infos[i];
            }
            vkUpdateDescriptorSets(vk.device, 2, writes, 0, nullptr);
        }
        catch (const std::exception&)
        {
            // The input set is already owned by `this`, so the frame still converts in; without this one it
            // would convert in and then read back with the wrong layout, so it must not be left half made.
            Release(vk.device);
            throw;
        }

        unconvertSrcView = backSrc;
        unconvertDstView = backDst;
        unconvertPool = backPool;
        unconvertSet = backSet;
        unconvertFor = srcFormat;
        backGuard.src = backGuard.dst = nullptr;
        backGuard.pool = nullptr;
        backGuard.set = nullptr;
    }

    // The valid w x h subrect of `src` into `dst`, both GENERAL. The caller has already made the writes to `src`
    // visible to a shader read (see the caller's barriers) and left `dst` GENERAL for a shader write.
    void Dispatch(VkCommandBuffer c, const Pipeline& p, uint32_t w, uint32_t h)
    {
        const VkPipeline pipeline = p.Convert(convertFor);
        if (!set || !pipeline)
            return;
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
        const uint32_t push[2] { w, h };
        vkCmdPushConstants(c, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        vkCmdDispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
    }

    // NrInlineComputeMotion: the valid srcW x srcH of the source onto w x h of the destination (bilinear, as the
    // LINEAR blit). Same preconditions as Dispatch.
    void DispatchMotion(VkCommandBuffer c, const Pipeline& p, uint32_t w, uint32_t h, uint32_t srcW, uint32_t srcH)
    {
        const VkPipeline pipeline = p.Convert(convertFor);
        if (!set || !pipeline)
            throw std::logic_error("motion rescale: no compute pass for this motion format");
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
        const uint32_t push[4] { w, h, srcW, srcH };
        vkCmdPushConstants(c, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        vkCmdDispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
    }

    // The output direction: the network's answer in the canonical image into `gameFormat`'s image, so the
    // readback's row pitch and placed footprint (both built for the game's format) describe it correctly.
    // Same preconditions as Dispatch, with the two images the other way round.
    //
    // Refuses rather than returns when there is nothing to run: the caller copies `net->image` out with the
    // game's row pitch afterwards, and after this point that image is the canonical one, so a skipped
    // conversion here is the wrong-layout readback itself, not a lost optimisation.
    void DispatchUnconvert(VkCommandBuffer c, const Pipeline& p, VkFormat gameFormat, uint32_t w, uint32_t h)
    {
        const VkPipeline pipeline = p.Unconvert(gameFormat);
        if (!unconvertSet || !pipeline)
            throw std::logic_error("plan A: no output conversion pipeline for the game's colour format");
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &unconvertSet, 0, nullptr);
        const uint32_t push[2] { w, h };
        vkCmdPushConstants(c, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        vkCmdDispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
    }

    // Whether the answer can be brought back into the game's format. A canonicalised frame without this would
    // be read back with the wrong layout, so RunNetwork refuses rather than copying a canonical image out
    // with the game's row pitch -- that is the defect this pass exists to prevent.
    //
    // The test is `unconvertFor` -- the format an output pipeline was actually built for, recorded by Create --
    // and not Pipeline::UnconvertModule, which is only a file name: Pipeline::Create skips a format whose
    // module or pipeline failed to build, after which the name is still there but nothing can run. Asking the
    // module would let such a frame through and then read it back exactly as if Plan A were not there.
    bool CanUnconvert(VkFormat gameFormat) const
    {
        return unconvertSet && unconvertFor == gameFormat;
    }

    void Release(VkDevice device)
    {
        if (unconvertSet && unconvertPool)
            vkFreeDescriptorSets(device, unconvertPool, 1, &unconvertSet);
        if (unconvertPool) vkDestroyDescriptorPool(device, unconvertPool, nullptr);
        if (unconvertSrcView) vkDestroyImageView(device, unconvertSrcView, nullptr);
        if (unconvertDstView) vkDestroyImageView(device, unconvertDstView, nullptr);
        if (set && pool)
            vkFreeDescriptorSets(device, pool, 1, &set);
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
        if (srcView) vkDestroyImageView(device, srcView, nullptr);
        if (dstView) vkDestroyImageView(device, dstView, nullptr);
        *this = {};
    }
};

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

#include "MochizukiImageInput.inl"

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
    bool firstPass = false;
    bool inlineExecution = false;
    bool directRecording = false;
    bool stageTiming = false; // MOCHIZUKI_NR_FRAME_FLAG_STAGE_TIMING (diagnostic)
    bool enqueued = false; // EnqueueHip ran for it: a second call does nothing
    uint64_t vkFinished = 0; // actual execution's accepted submission, not its preparation order
    // Plan A: this frame's colour is converted (compute pass, not a blit) into the geometry's canonical image
    // before the network sees it. Only for a frame whose format differs from the session's first-seen one.
    bool canonicalise = false;
    ID3D12Resource* colour {};
    D3D12_RESOURCE_STATES colourState {};
    ID3D12Resource* motion {};
    D3D12_RESOURCE_STATES motionState {};
    ID3D12Resource* depth {};
    D3D12_RESOURCE_STATES depthState {};
    UINT depthWidth = 0, depthHeight = 0;
    bool depthInverted = false;
    // The frame's valid extents, copied from the top-left of its colour and motion: the geometry's own, except with
    // dynamic resolution.
    UINT width = 0, height = 0, motionWidth = 0, motionHeight = 0;
    MotionPath motionPath = Uploaded;
    float motionScaleX = 0, motionScaleY = 0;
    bool reset = false;
    bool normalizationReset = false; // explicit cut/queue discontinuity, separate from network history
    nr::renodx::Settings reno;
    nr::Controls controls;            // the frame's, sanitised: validate() cannot refuse them
    bool afterUpscale = false;
    float renoExposure = 0.f; // validated game exposure, already scaled / pre-exposure
    float history = 1.f, white = 1.f; // set on the runtime before the frame is recorded
    // Exposure sources for CoreRecordInputs (PR #9 follow-up): whether the white point is a linear encode at all
    // (the meter is skipped otherwise: white is unused), whether the host enabled the meter, and the game's
    // exposure texture to read back, if the host handed one over (MochizukiNrSetExposureTexture).
    bool linear = false;
    bool meter = false;
    ID3D12Resource* exposureTexture {};
    D3D12_RESOURCE_STATES exposureState {};
    float exposureFactor = 1.f;
    MochizukiAutoExposure::Probe::Ticket meterTicket, texelTicket;
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
    // Plan A: on a converted frame this is the canonical format's DXGI twin (for the key and the logs);
    // otherwise the game's own, as before. A flip still changes the geometry through gameColourFormat below,
    // which remakes the frame's buffers (not the network).
    DXGI_FORMAT colourFormat = DXGI_FORMAT_UNKNOWN;
    // The game's own colour format, always. It IS part of operator==: the colour buffers, the colour image and
    // the upload's footprint are all built for it, and a buffer-to-image copy cannot convert. When Plan A's
    // channel is on, a flip therefore retires the geometry -- which remakes the *input side* (the adapter, Plan
    // A's third step) -- while the network key stays on the canonical format, so the network and its weights are
    // kept (whether the temporal history survives the adapter remake is to be confirmed in-game). That is exactly the split Plan A asks for; without it the footprint would describe
    // the old format while the game hands the new one, which is an invalid command in the game's own list.
    DXGI_FORMAT gameColourFormat = DXGI_FORMAT_UNKNOWN;
    UINT64 colourWidth = 0;
    UINT colourHeight = 0;
    DXGI_FORMAT motionFormat = DXGI_FORMAT_UNKNOWN;
    UINT motionWidth = 0, motionHeight = 0;
    bool firstPass = false;
    bool bucketed = false; // the motion buffer is the allocation's, and a bucket-sized motion image is made too
    // Plan A: this geometry's colour is converted by a compute pass into canonicalVk before the network sees it.
    // False when the frame is already in the session's canonical (first-seen) format, or the pair cannot be
    // converted both ways (then the key keeps the game's format, i.e. the pre-Plan-A rebuild). Part of the
    // geometry so the buffers and the key's format agree.
    bool canonicalise = false;
    // Plan A: the canonical format the network runs on when canonicalise is set (UNDEFINED otherwise).
    VkFormat canonicalVk = VK_FORMAT_UNDEFINED;
    // NrInlineComputeMotion on a bucketed geometry with a compute rescale for its motion format: motionScaled is
    // also a storage image and motionRescale has its set, so a Rescaled frame needs no blit.
    bool computeMotion = false;
    bool operator==(const Geometry& o) const
    {
        return width == o.width && height == o.height && colourFormat == o.colourFormat &&
               gameColourFormat == o.gameColourFormat && colourWidth == o.colourWidth &&
               colourHeight == o.colourHeight && motionFormat == o.motionFormat &&
               motionWidth == o.motionWidth && motionHeight == o.motionHeight && firstPass == o.firstPass &&
               bucketed == o.bucketed && canonicalise == o.canonicalise && canonicalVk == o.canonicalVk &&
               computeMotion == o.computeMotion;
    }
};

// Plan A: the DXGI twin of a canonical format, for Geometry::colourFormat (which only decides equality, the key
// and the logs, never the frame's buffers). UNKNOWN for a format Plan A does not convert.
inline DXGI_FORMAT CanonicalDxgi(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return DXGI_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}


// What a network is built for: the frame extent (the bucket, with dynamic resolution), model scale
// (quantised), pass count and colour encoding. Plan A: once a session has seen a second colour
// format, `format` stays the session's first-seen (canonical) format and the other format is converted to it on
// the input side, so a transient colour-format flip no longer changes this key and never rebuilds.
struct NetworkKey
{
    UINT width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    float scale = 1.f;
    uint32_t maxPasses = 1;
    bool linear = false;
    bool prep = false; // RuntimeConfig::preprocess
    bool compactTransfer = true;
    bool firstPass = false;
    bool aco = false; // this build imports the ACO network; the device must already expose pipeline binaries
    CompilePolicy compilePolicy = CompilePolicy::WindowsDefault;
    bool int4 = false, renodx = false;
    uint32_t networkModel = 1;
    uint32_t renoWidth = 0, renoHeight = 0, sourceEncoding = 0;
    float linearNits = 0.f;
    bool computeConvert = false; // NrInlineComputeConvert on a listed-format key (RuntimeConfig::compute_convert_colour)
    bool computeDownscale = false; // NrInlineComputeDownscale on a key with scale < 1 (RuntimeConfig::compute_downscale)
    bool computeExport = false; // NrInlineComputeExport on a first-pass key (RuntimeConfig::compute_export)
    bool motionPasses = false; // NrInlineComputeMotion on a multi-pass key (RuntimeConfig::motion_in_place_passes)
    bool operator==(const NetworkKey&) const = default;
    // Geometry alone may change; every other build semantic stays exact.
    bool SameGraphSemantics(const NetworkKey& other) const
    {
        auto geometryOnly = other;
        geometryOnly.width = width;
        geometryOnly.height = height;
        geometryOnly.renoWidth = renoWidth;
        geometryOnly.renoHeight = renoHeight;
        return *this == geometryOnly;
    }
    bool SameModel(const NetworkKey& other) const
    {
        return width == other.width && height == other.height && format == other.format &&
               scale == other.scale && linear == other.linear && prep == other.prep &&
               compactTransfer == other.compactTransfer && firstPass == other.firstPass && aco == other.aco &&
               compilePolicy == other.compilePolicy &&
               networkModel == other.networkModel && int4 == other.int4 && renodx == other.renodx && renoWidth == other.renoWidth &&
               renoHeight == other.renoHeight && sourceEncoding == other.sourceEncoding && linearNits == other.linearNits &&
               computeConvert == other.computeConvert && computeDownscale == other.computeDownscale &&
               computeExport == other.computeExport && motionPasses == other.motionPasses;
    }
};

static std::atomic<uint64_t> g_nextNetworkId { 0 };

// Stage resources share the network's retained-recording ownership and GPU retirement.
struct ProductRuntime : nr::Runtime
{
    struct ExposureImage {
        Image image;
        VkDevice device {};
        ~ExposureImage() { image.Release(device); }
    } exposure;
    std::unique_ptr<nr::renodx::Stage> reno;
    MochizukiStageTiming stageTiming;
    uint32_t renoPasses = 0;
    uint64_t id = 0;
    bool graphAco = false; // Actual compiler provenance, not requested ACO mode.
    CompilePolicy graphPolicy = CompilePolicy::WindowsDefault;
    NetworkKey key {};
    // Set by whoever tears the network down, logged once in the destructor; a network
    // destroyed by nothing but the last shared_ptr going (session end) keeps the default.
    const char* destroyReason = "session_end";
    VkQueue queue {};

    ProductRuntime(const nr::HostDevice& host, const nr::RuntimeConfig& config,
                   const nr::TemporalConfig& temporal, const NetworkKey& key, Vulkan& vk)
        : nr::Runtime(host, config, nr::ControlMaskConfig {}, temporal), id(++g_nextNetworkId), key(key), queue(vk.queue)
    {
        nr::logf("[mochizuki] network #%llu created (%ux%u, model=%u, maxPasses=%u, renodx=%d)",
                 static_cast<unsigned long long>(id), key.width, key.height, key.networkModel, key.maxPasses,
                 key.renodx ? 1 : 0);
        if (!key.renodx) return;
        nr::renodx::StageConfig sc;
        sc.collect_facts = false; // No engine-panel Facts consumer in the product.
        sc.shaders = (std::filesystem::path(config.root) / "dlssnr-amd/shaders/renodx").string();
        sc.width = key.width; sc.height = key.height;
        sc.nr_width = key.renoWidth; sc.nr_height = key.renoHeight;
        sc.frame_format = key.format; sc.hdr_flag = key.linear;
        sc.settings.source_encoding = key.sourceEncoding;
        sc.settings.linear_unit_nits = key.linearNits;
        // RunNetwork re-records Vulkan commands on every execution, and waits the
        // three-slot fence before reuse. Stage's 64-record retirement is therefore
        // safe even when the D3D12 host replays a retained recording much later.
        reno = std::make_unique<nr::renodx::Stage>(host, sc);
        exposure.device = host.device;
        exposure.image.Create(vk, VK_FORMAT_R32_SFLOAT, 1, 1);
        static_assert(MochizukiStageTiming::kPasses == nr::renodx::kMaxUnlockedPasses);
        uint32_t familiesCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(host.physical, &familiesCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familiesCount);
        vkGetPhysicalDeviceQueueFamilyProperties(host.physical, &familiesCount, families.data());
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(host.physical, &props);
        stageTiming.Create(host.device, props.limits.timestampPeriod, families[vk.family].timestampValidBits,
            vk.computeFamily < familiesCount ? families[vk.computeFamily].timestampValidBits : 0);
    }

    ~ProductRuntime()
    {
        nr::logf("[mochizuki] network #%llu destroy.begin (%ux%u, reason=%s)", static_cast<unsigned long long>(id),
                 key.width, key.height, destroyReason);
    }
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
    std::shared_ptr<ImageInput> colourInput;
    std::shared_ptr<ImageInput> motionInput;
    Geometry geometry;
    D3D12_RESOURCE_DESC resultDesc {};
    Footprint colourFootprint, motionFootprint;
    VkFormat colourVk = VK_FORMAT_UNDEFINED, motionVk = VK_FORMAT_UNDEFINED;
    SharedBuffer input, output, motionBuffer, firstOutput;
    Image colourImage, motionImage, firstColour;
    // Plan A: the frame in the canonical format, when the game's own differs from it. Null when the game
    // already gives the canonical format (then colourImage is fed to the network as is, no conversion).
    Image canonicalColour;
    // Plan A: the compute pass that fills canonicalColour from colourImage (no set when not canonicalised).
    ConvertPass convert;
    Image motionScaled; // bucketed geometry with motion: the vectors laid over the subrect, at the bucket's extent
    ConvertPass motionRescale; // geometry.computeMotion: motionImage -> motionScaled on compute (no set otherwise)
    ID3D12Resource* firstResult {};
    ID3D12Resource* result {};

    // Default move copies the raw handles and would free them twice. Steal leaves
    // the source empty. Copy stays shallow: the heap allocation owns the handles.
    FrameBuffers() = default;
    FrameBuffers(const FrameBuffers&) = default;
    FrameBuffers& operator=(const FrameBuffers&) = default;
    FrameBuffers(FrameBuffers&& other) noexcept { Steal(other); }
    FrameBuffers& operator=(FrameBuffers&& other) noexcept { Steal(other); return *this; }

    void Steal(FrameBuffers& other) noexcept
    {
        if (this == &other) return;
        allocation = std::move(other.allocation);
        colourInput = std::move(other.colourInput);
        motionInput = std::move(other.motionInput);
        geometry = std::exchange(other.geometry, {});
        resultDesc = std::exchange(other.resultDesc, {});
        colourFootprint = std::exchange(other.colourFootprint, {});
        motionFootprint = std::exchange(other.motionFootprint, {});
        colourVk = std::exchange(other.colourVk, VK_FORMAT_UNDEFINED);
        motionVk = std::exchange(other.motionVk, VK_FORMAT_UNDEFINED);
        input = std::exchange(other.input, {});
        output = std::exchange(other.output, {});
        firstOutput = std::exchange(other.firstOutput, {});
        firstColour = std::exchange(other.firstColour, {});
        firstResult = std::exchange(other.firstResult, nullptr);
        motionBuffer = std::exchange(other.motionBuffer, {});
        colourImage = std::exchange(other.colourImage, {});
        canonicalColour = std::exchange(other.canonicalColour, {});
        convert = std::exchange(other.convert, {});
        motionImage = std::exchange(other.motionImage, {});
        motionScaled = std::exchange(other.motionScaled, {});
        motionRescale = std::exchange(other.motionRescale, {});
        result = std::exchange(other.result, nullptr);
    }

    void KeepOnLoss() noexcept
    {
        Quarantine(allocation);
        Quarantine(colourInput);
        Quarantine(motionInput);
    }

    void Release(VkDevice device)
    {
        if (allocation) { *this = {}; return; }
        if (device)
        {
            if (colourInput) {
                input = {}; colourImage = {}; colourInput.reset();
            } else {
                input.Release(device);
                colourImage.Release(device);
            }
            output.Release(device);
            firstOutput.Release(device);
            firstColour.Release(device);
            if (motionInput) {
                motionBuffer = {}; motionImage = {}; motionInput.reset();
            } else {
                motionBuffer.Release(device);
                motionImage.Release(device);
            }
            canonicalColour.Release(device);
            convert.Release(device);
            motionRescale.Release(device);
            motionScaled.Release(device);
        }
        if (firstResult) firstResult->Release();
        firstResult = nullptr;
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
constexpr double kPassFactor[] = { 1.0, 1.2, 1.3 }; // calibrated for 1..3 passes
// Past that fit, repeat the 1->2 step so a 30-layer build is not under-budgeted.
double PassMemoryFactor(uint32_t passes)
{
    passes = std::clamp(passes, 1u, kMaxPasses);
    if (passes <= std::size(kPassFactor))
        return kPassFactor[passes - 1];
    return kPassFactor[std::size(kPassFactor) - 1] + 0.2 * double(passes - std::size(kPassFactor));
}
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
constexpr ULONGLONG kExactExtentSettleMs = 300;

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
    std::shared_ptr<ProductRuntime> network;
    NetworkKey net;
    Microsoft::WRL::ComPtr<ID3D12Resource> colour, motion, depth, exposureTexture;
    Microsoft::WRL::ComPtr<ID3D12Fence> tail;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> executingQueue;
    uint64_t tailValue = 0, executions = 0;
    std::shared_ptr<void> coreResources;
    std::shared_ptr<void> renoResources;
    std::unique_ptr<MochizukiStageTiming> directTiming;
    uint32_t directTimingSlot = MochizukiStageTiming::kUnavailable;
    uint64_t completedExecutions = 0;
    uint64_t directSerial = 0;
    bool invalidated = false, uncertain = false;
    // inlineExecution is this recording's mode, not the create-time INLINE_QUEUE bit.
    // privateBuffers is a checked-out set. Split recordings share the session set.
    bool inlineExecution = false, privateBuffers = false, ended = false;
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
    Microsoft::WRL::ComPtr<ID3D12Resource> pipelineInvalidator;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> invalidatorCpu, invalidatorGpu;
    bool invalidatorFailed = false;
    std::atomic<bool> frameAbiRejected { false };
    std::atomic<bool> adoptedLegacyRejected { false };
    uint64_t directRecorded = 0, directSubmitted = 0, directCompleted = 0;
    std::shared_ptr<FrameBuffers> allocation;
    std::shared_ptr<ImageInput> colourInput;
    std::shared_ptr<ImageInput> motionInput;
    std::shared_ptr<ImageInput> pendingColour;
    std::vector<std::shared_ptr<ImageInput>> inputInitializations;
    std::vector<std::shared_ptr<ImageInput>> retiredInputs;
    std::weak_ptr<ImageInput> bufferColourCache, sharedColourCache;
    Hold<Geometry> sharedColourHold;
    bool sharedColourStorageRefused = false; // session-local: no repeated optional allocation failures
    Hold<Geometry> bufferColourHold;
    int sharedInputRequest = Hooks().sharedInput;
    int sharedExtendedRequest = Hooks().sharedExtended;
    uint64_t sharedInputDelay = Hooks().sharedInputDelay;
    uint64_t loggedColourGeneration = 0;
    std::shared_ptr<ImageInput> pendingMotion;
    std::weak_ptr<ImageInput> bufferMotionCache, sharedMotionCache;
    Hold<Geometry> sharedMotionHold, bufferMotionHold;
    int sharedMotionRequest = Hooks().sharedMotion;
    uint64_t sharedMotionDelay = Hooks().sharedMotionDelay;
    uint64_t loggedMotionGeneration = 0;

    std::unordered_map<void*, std::unique_ptr<Recording>> recordings;
    static constexpr uint32_t kRecordingSlots = 4;
    std::vector<FrameBuffers> recordingPool;
    Geometry poolHot {};
    Geometry poolPrevious {};
    bool poolHavePrevious = false;
    uint32_t poolTrimReleases = 0;
    uint32_t recordingBuffersOutstanding = 0;

    uint32_t RecipeCount() const { return static_cast<uint32_t>(recordings.size()); }

    // Inline admission counts a reservation or an unfinished execution.
    // A closed recording whose tail has completed stays in the map for replay
    // and does not consume a slot. Split recordings never do.
    uint32_t InFlightRecordings() const
    {
        uint32_t n = 0;
        for (const auto& entry : recordings)
        {
            const Recording* rec = entry.second.get();
            if (!rec || !rec->inlineExecution || rec->invalidated) continue;
            if (rec->executingQueue || !rec->ended) { ++n; continue; }
            if (std::find(enqueueOrder.begin(), enqueueOrder.end(), entry.first) != enqueueOrder.end())
            { ++n; continue; }
            if (!rec->tail) continue;
            const uint64_t done = rec->tail->GetCompletedValue();
            if (done == UINT64_MAX || done < rec->tailValue) ++n;
        }
        return n;
    }
    D3D12_RESOURCE_DESC bufferResultDesc {};
    // Tokens whose Begin ran while `executing` was already set. Oldest is next.
    std::deque<void*> enqueueOrder;
    Microsoft::WRL::ComPtr<ID3D12Fence> previousTail;
    uint64_t previousTailValue = 0;
    void* executing = nullptr;
    bool uncertainExecution = false;
    // History belongs to actual execution order, not PrepareFrame order.
    std::weak_ptr<nr::Runtime> historyNetwork;
    uint64_t historyFrame = 0, historyTick = 0;
    uint64_t recordingHistoryResets = 0; // Under RecordingMutex; diagnostic only.
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
    // MZ_DIAG_NET_TRACE=1 (default off): a sampler thread timestamps (QPC) when each frame's produced value lands in
    // toVk (game side -> Vulkan), its finished value in the Vulkan view of fromVk (network done) and in the D3D12 view
    // (interop back). Read-only CPU reads, no waits added; logs only frames slower than 50 ms (plus the two before).
    struct NetTrace { UINT64 produced = 0, finished = 0; int64_t enq = 0, sub = 0, toVk = 0, vkDone = 0, d3dDone = 0;
                      uint32_t phases = 0, slotWaitUs = 0; };
    std::mutex traceMutex; std::deque<NetTrace> tracePending; std::thread tracer; std::atomic<bool> traceStop {false};
    bool traceOn = [] { const char* v = std::getenv("MZ_DIAG_NET_TRACE"); return v && !std::strcmp(v, "1"); }();
    int64_t traceEnq = 0; uint32_t traceSlotWaitUs = 0, tracePhases = 0; // under submitMutex
    static int64_t TraceQpc() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
    void TraceLoop()
    {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); const double ms = 1000.0 / double(f.QuadPart);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        std::deque<NetTrace> done; uint64_t logged = 0;
        while (!traceStop.load(std::memory_order_acquire))
        {
            if (timer) { LARGE_INTEGER due {}; due.QuadPart = -5000; SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                         WaitForSingleObject(timer, 5); } else Sleep(1);
            const UINT64 tv = toVk.fence ? toVk.fence->GetCompletedValue() : 0;
            UINT64 vv = 0; if (vkGetSemaphoreCounterValue(vk.device, fromVk.semaphore, &vv) != VK_SUCCESS) vv = 0;
            const UINT64 dv = fromVk.fence ? fromVk.fence->GetCompletedValue() : 0;
            const int64_t now = TraceQpc();
            std::lock_guard lock(traceMutex);
            for (auto& t : tracePending) {
                if (!t.toVk && tv != UINT64_MAX && tv >= t.produced) t.toVk = now;
                if (!t.vkDone && vv != UINT64_MAX && vv >= t.finished) t.vkDone = now;
                if (!t.d3dDone && dv != UINT64_MAX && dv >= t.finished) t.d3dDone = now;
            }
            while (!tracePending.empty() && tracePending.front().toVk && tracePending.front().vkDone && tracePending.front().d3dDone) {
                const NetTrace t = tracePending.front(); tracePending.pop_front();
                done.push_back(t); if (done.size() > 3) done.pop_front();
                if ((t.d3dDone - t.enq) * ms > 50.0 && logged < 64) {
                    ++logged;
                    // Own file so MZ_DIAG_NET_TRACE works without MochizukiNrSetLogging.
                    const std::wstring path = DataDirectory() + L"\\mochizuki_net_trace.log";
                    FILE* f = _wfopen(path.c_str(), L"ab");
                    for (const auto& d : done) {
                        char line[384];
                        std::snprintf(line, sizeof line,
                            "[mochizuki] net_trace finished=%llu phases=%u slot_wait_us=%u enq_to_submit=%.3f submit_to_tovk=%.3f "
                            "tovk_to_vkdone=%.3f vkdone_to_d3d=%.3f total=%.3f ms",
                            (unsigned long long)d.finished, d.phases, d.slotWaitUs, (d.sub - d.enq) * ms,
                            (d.toVk - d.sub) * ms, (d.vkDone - d.toVk) * ms, (d.d3dDone - d.vkDone) * ms, (d.d3dDone - d.enq) * ms);
                        nr::logf("%s", line);
                        if (f) { SYSTEMTIME st {}; GetLocalTime(&st);
                            std::fprintf(f, "%02u:%02u:%02u.%03u %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line); }
                    }
                    if (f) std::fclose(f);
                    done.clear();
                }
            }
        }
        if (timer) CloseHandle(timer);
    }
    std::mutex watchMutex;
    std::condition_variable watchWake;
    bool watchStop = false;
    Awaited awaited;
    std::atomic<bool> released { false }; // Lose's signal of fromVk to its maximum was made
    uint32_t rereleases = 0;              // Rerelease's signals: the watchdog's, then ~Session's
    VkCommandPool pool {};
    VkCommandPool computePool {};
    VkSemaphore phaseSemaphore {};
    uint64_t phaseValue = 0;
    size_t loggedPhaseCount = 0;
    bool loggedFullCompute = false;
    // Plan A: frames whose answer was converted back into the game's format before the readback. Logged on a
    // period so the output pass is visible in a session log without a line per frame; before this existed the
    // only sign of Plan A was the input-side line, which a same-format frame also prints.
    uint64_t unconvertDispatches = 0, loggedUnconvertAt = 0;
    uint64_t chainPathChanges = 0;
    std::string loggedChainWhy;
    const char* computeChainOff = nullptr; // set once a compute-family recording hit a graphics-only command
    struct PhaseCommand {VkCommandBuffer command; bool compute;};
    // A slot's final graphics fence certifies every preceding compute segment.
    // Retained game recordings still own their normal buffers independently.
    std::vector<VkCommandBuffer> phaseGraphics[kSlots], phaseCompute[kSlots];
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
    uint64_t fallbackColourGeneration[kFallbacks] {};
    VkBuffer fallbackOutput[kFallbacks] {};
    VkDeviceSize fallbackOutputOffset[kFallbacks] {}; // the Proton direct path's buffers can share a VkBuffer
    UINT fallbackWidth[kFallbacks] {}, fallbackHeight[kFallbacks] {};
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
    bool inlineQueue = false;
    TestHooks hooks;
    std::atomic<uint32_t> errors { 0 }; // failed calls and passed-through frames, for the log's rate limit

    // The network for one extent, colour format, model scale, pass count and colour encoding, built off the render
    // thread: `runtime` is the one the frames record with (for `net`), `built` a finished build's (for buildKey) until
    // EnsureNetwork installs it. With a queue of their own for the builds, a network for another model scale, pass
    // count or colour encoding is built while the frames keep the old one (keep serving); runtime and net change under
    // submitMutex. Only EnsureNetwork and ~Session join the builder, under buildMutex once buildDone is set.
    std::shared_ptr<ProductRuntime> runtime;
    NetworkKey net, buildKey;
    nr::renodx::Stage::ExposureSnapshot exposureSnapshot; // only the small shared meter resources
    ULONGLONG executionLogAt = 0;
    // Auto DRS can still use exact extents (whole allocations, or no blit support).
    // A transient request must not take the working network away from normal frames.
    NetworkKey extentCandidate;
    ULONGLONG extentCandidateSince = 0;
    bool extentCandidatePending = false;
    uint32_t extentDeferrals = 0;
    std::thread builder;
    std::mutex buildMutex;
    std::condition_variable buildEnded; // buildDone was set
    bool building = false, buildDone = false;
    // Destroy during a build (Abandon): the build stops between two pipelines, and when it has not ended within
    // kAbandonWaitMs its thread owns the session and ends it (EndAbandoned).
    std::atomic<bool> abandon { false };
    bool builderOwnsSession = false; // under buildMutex
    std::chrono::steady_clock::time_point abandonedAt;
    std::shared_ptr<ProductRuntime> built;
    char buildError[1024] {}; // a fixed buffer, so the builder cannot throw on its way to buildDone
    NrError::Kind buildErrorKind = NrError::Other;
    double buildSeconds = 0;
    // Networks the frames no longer use (RetireNetwork), each with the finished value of the last frame handed to the
    // Vulkan side while the frames had it: the releaser thread frees them, oldest first, once fromVk is there
    // (Releaser). What is left when the session ends goes after its drain, or leaks with a lost device.
    struct Retired
    {
        std::shared_ptr<ProductRuntime> network;
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
    uint64_t upgradeRefusals = 0;
    std::atomic<bool> upgradeOomInjected { false };
    std::atomic<uint32_t> infoRequestedPasses { 1 };
    // Host lifecycle calls serialize updates; GetStatus also reads under errorMutex.
    char networkDeferred[192] {};
    char loggedCapacity[256] {};

    // runtime->last_gpu_ms() of the last kGpuSamples frames, for GetStatus. Pushed under submitMutex as well.
    std::mutex statsMutex;
    float gpuMs[kGpuSamples] {};
    float networkMs[kGpuSamples] {}, otherMs[kGpuSamples] {};
    float encodeMsLast = -1.f, transferMsLast = -1.f; // stage timing, last accepted sample (statsMutex)
    uint32_t gpuMsCount = 0, gpuMsNext = 0;
    uint64_t statsSerial = 0, gpuSamples = 0, gpuTick = 0;
    uint64_t statsNetworkId = 0;
    ProductRuntime* pendingTimingNetwork = nullptr;
    uint32_t pendingTimingSlot = MochizukiStageTiming::kUnavailable;

    // SetControls' last word, which PrepareFrame copies into every job. The mutex only guards a host that sets them
    // from another thread than the render thread.
    std::mutex controlsMutex;
    SessionControls controls;
    bool deviceStarted = false;
    bool acoAtDevice = false;
    CompilePolicy compilePolicy = CompilePolicy::WindowsDefault;
    bool acoEffective = false;
    std::atomic<uint32_t> infoInt4 {0}, infoReno {0}, infoNetwork {0};
    char acoFallback[128] {};
    // The builder's result for the network it is about to hand to Install. A discarded candidate leaves these unused.
    bool candidateReady = false;
    bool candidateOn = false;
    char candidateWhy[128] {};
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
    // What the lane measured itself when the game supplied nothing (MochizukiAutoExposure.h): the divisor the white
    // point of the frame below is put through. 0 until the first measurement lands, and it stays the game's exposure
    // that wins while the host supplies one.
    std::atomic<float> meteredExposure { 0.f };
    // MochizukiNrSetAutoExposure: the host's switch for the meter above (INI MochizukiWhitePointSource 2/3). Off by
    // default, so a session whose host never calls it keeps the manual white point exactly as before PR #9.
    std::atomic<bool> meterEnabled { false };
    // MochizukiNrSetExposureTexture: the game's 1x1 exposure texture with its scale and pre-exposure. Its texel is
    // read back through `gameTexel` (fence-gated, one frame late), and textureExposure = texel * scale / pre is the
    // divisor, the same expression lmxxf's codec evaluates on the GPU (native_codec_encode.hlsl EffectivePaperWhite).
    // Written under controlsMutex; the job copies the pointer, the host keeps the texture alive for the frame.
    ID3D12Resource* exposureTexture {};
    D3D12_RESOURCE_STATES exposureTextureState {};
    float exposurePre = 1.f, exposureScale = 1.f;
    std::atomic<float> textureExposure { 0.f };
    MochizukiAutoExposure::Probe gameTexel;
    // Frames since the meter last produced a measurement. It can stop for good (a colour image it cannot read, no free
    // slot, a recording that never published a tail), and dividing the white point by an exposure from an arbitrary
    // earlier frame is worse than the manual setting: after this many frames the manual white point is back in charge.
    static constexpr uint32_t kMeterIdleFrames = 60;
    uint32_t meterIdle = 0;
    MochizukiAutoExposure::Probe exposure;
    // Set by the first frame that asks for the preprocess: from then on every network is built able to run it.
    bool prepWanted = false;
    // What the last SetControls asked for, before any frame has run: RuntimeConfig::preprocess is fixed when the network
    // is built, so a network built while this is false cannot run the control at all and the frames' requests could
    // never build one that can. Only ever set, never cleared: a network able to preprocess serves frames that do not.
    bool prepRequested = false;
    // When the input line below was last written, so the log has its own heartbeat rather than the session end.
    ULONGLONG inputLogAt = 0;
    // One bit a frame, newest lowest: its history was consumed. Written under submitMutex only.
    std::atomic<uint64_t> historyBits { 0 };
    std::atomic<uint32_t> historyCount { 0 };
    std::mutex errorMutex;
    // Menu status: the inline path of the last inline frame (0 none/split, 1 whole compute, 2 segments) and why.
    std::atomic<uint32_t> infoInlinePath { 0 };
    std::mutex inlinePathMutex;
    char infoInlineWhy[124] {};
    char lastError[256] {};

    Geometry geometry;
    Footprint colourFootprint, motionFootprint;
    VkFormat colourVk = VK_FORMAT_UNDEFINED, motionVk = VK_FORMAT_UNDEFINED;
    SharedBuffer input, output, motionBuffer, firstOutput;
    Image colourImage, motionImage, motionScaled, firstColour;
    // Plan A: the frame in the canonical format, when geometry.canonicalise is set. Null otherwise.
    Image canonicalColour;
    // Plan A: this set's conversion pass descriptor set (the pipeline itself is the session's, below).
    ConvertPass convert;
    // Plan A: the conversion pipelines, built once for the session (they name no image); each frame's buffer
    // set allocates its own descriptor sets from them. Empty until Start made them, and left empty when the
    // passes are not there, which keeps every frame on its own format (the pre-Plan-A path).
    ConvertPass::Pipeline convertPipeline;
    // NrInlineComputeMotion: the session's motion rescale pipelines and this set's descriptor set.
    ConvertPass::Pipeline motionPipeline;
    ConvertPass motionRescale;
    ID3D12Resource* firstResult {};
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
    // Plan A, render thread only. firstColourVk: the first colour format the session saw, which is the format
    // the network was built for and so the session's canonical format. canonicalChannel: latched on the first
    // frame whose format differs from it and can be converted both ways; until then nothing about Plan A runs.
    VkFormat firstColourVk = VK_FORMAT_UNDEFINED;
    bool canonicalChannel = false;
    // The last game colour format logged as converted / unconvertible, so each is logged once per change.
    VkFormat loggedGameColourFormat = VK_FORMAT_UNDEFINED;
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
    enum HistoryResetReason : uint32_t {
        ResetStartup = 1, ResetQueue = 2, ResetNetwork = 4, ResetExtent = 8,
        ResetPasses = 16, ResetNoMotion = 32, ResetPassthrough = 64,
        ResetCancel = 128, ResetExternal = 256, ResetEnqueue = 512
    };
    std::atomic<bool> resetPending { true };
    std::atomic<uint32_t> resetReasons { ResetStartup };
    void RequestHistoryReset(uint32_t reason) {
        resetReasons.fetch_or(reason, std::memory_order_relaxed);
        resetPending.store(true, std::memory_order_release);
    }
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
        if (SUCCEEDED(hr)) {
            const DWORD waited = WaitForSingleObject(ev, 30000);
            const auto completed = f->GetCompletedValue();
            if (completed == UINT64_MAX) hr = DXGI_ERROR_DEVICE_REMOVED;
            else if (completed < 1) hr = waited == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
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
        if (vk.adoptedVkd3d) {
            // Recipes own allocations until invalidation AND their real game tail.
            for (auto& b : buried) b.buffers.Release(vk.device);
            buried.clear();
            return;
        }
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
        if (vk.adoptedVkd3d) return VK_SUCCESS; // Recipes retain the network through their actual game tail.
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
    void RetireNetwork(std::shared_ptr<ProductRuntime> network, UINT64 finished) noexcept
    {
        if (!network) return;
        const uint64_t nid = network->id;
        size_t qCount = 0;
        try
        {
            std::lock_guard lock(releaseMutex);
            retired.reserve(retired.size() + 1); // the one step that may throw, before the network moves
            retired.push_back({ std::move(network), finished });
            ++releasing;
            qCount = retired.size();
        }
        catch (...)
        {
            Quarantine(network);
            return;
        }
        nr::logf("[mochizuki] network #%llu retired to release queue (queue length %zu, fence %llu)",
                 static_cast<unsigned long long>(nid), qCount, static_cast<unsigned long long>(finished));
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
    void ReleaseLater(std::shared_ptr<ProductRuntime> network, UINT64 finished) noexcept
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
            std::shared_ptr<ProductRuntime> network = std::move(retired.front().network);
            retired.erase(retired.begin());
            const uint64_t nid = network ? network->id : 0;
            const size_t remaining = retired.size();
            lock.unlock();
            const auto t0 = std::chrono::steady_clock::now();
            UINT64 usageBefore = 0, usageAfter = 0, budget = 0;
            const bool usageKnown = vram.Read(usageBefore, budget);
            network.reset();
            const bool usageAfterKnown = vram.Read(usageAfter, budget);
            nr::logf("[mochizuki] network #%llu queue reference dropped in %.1f ms (remaining queue length %zu; "
                     "process VRAM %llu -> %llu MB, budget %llu MB, known=%d)",
                     static_cast<unsigned long long>(nid),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                     remaining, static_cast<unsigned long long>(usageBefore >> 20),
                     static_cast<unsigned long long>(usageAfter >> 20), static_cast<unsigned long long>(budget >> 20),
                     usageKnown && usageAfterKnown ? 1 : 0);
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
            RequestHistoryReset(ResetQueue);
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
        // vkQueueSubmit has accepted the work. Preserve its release credential
        // before optional trace/awaited bookkeeping can allocate or throw.
        if (pendingTimingNetwork) pendingTimingNetwork->stageTiming.Submitted(pendingTimingSlot, finished);
        job.vkFinished = finished;
        if (colourInput) colourInput->finished = finished;
        if (motionInput) motionInput->finished = finished;
        if (traceOn) {
            if (!tracer.joinable()) { try { tracer = std::thread([this] { TraceLoop(); }); } catch (...) { traceOn = false; } }
            NetTrace t; t.produced = produced; t.finished = finished; t.enq = traceEnq ? traceEnq : TraceQpc(); t.sub = TraceQpc();
            t.phases = tracePhases; t.slotWaitUs = traceSlotWaitUs;
            std::lock_guard lock(traceMutex); if (tracePending.size() < 64) tracePending.push_back(t);
        }
        awaited.Add(produced, finished);
        lastProduced.store(produced, std::memory_order_relaxed);
        lastFinished.store(finished, std::memory_order_release);
    }

    // A frame's Vulkan submit: c after the frame's produced value in toVk, then its finished value into fromVk; then
    // Submitted. None once the session is lost (Lose), whose release a later signal would undo. `producer`: the game
    // queue the frame came from, which the Proton direct path drains and holds around the submit.
    VkResult Submit(VkCommandBuffer c, UINT64 produced, UINT64 finished, VkFence signal, ID3D12CommandQueue* producer)
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
        VkQueue target = vk.queue;
        const bool marker = enqueueCalls <= 2;
        if (marker) nr::logf("[mochizuki] old-chain LockCommandQueue.begin");
        mochizuki_vkd3d::QueueHold hold;
        if (vk.adoptedVkd3d)
            if (const VkResult held = vk.HoldProducer(producer, hold, target); held != VK_SUCCESS)
                return held;
        if (marker) nr::logf("[mochizuki] old-chain LockCommandQueue.end QueueSubmit.begin");
        const VkResult r = vkQueueSubmit(target, 1, &si, signal);
        if (marker) nr::logf("[mochizuki] old-chain QueueSubmit.end result=%d UnlockCommandQueue.begin", int(r));
        if (hold.interop && hold.queue) {
            hold.interop->UnlockCommandQueue(hold.queue);
            hold.queue = nullptr;
        }
        if (marker) nr::logf("[mochizuki] old-chain UnlockCommandQueue.end");
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
        traceStop.store(true, std::memory_order_release);
        if (tracer.joinable()) tracer.join();
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

    int32_t RejectFrameAbiMismatch(const char* text) noexcept
    {
        RememberError(text);
        if (!frameAbiRejected.exchange(true, std::memory_order_relaxed))
            nr::logf("[mochizuki] REJECTED: OptiScaler.dll and MochizukiNrRuntime.dll are from different builds "
                     "(frame ABI size mismatch). Install the FULL package (both DLLs from the same build). "
                     "Runtime commit=" MZ_BUILD_COMMIT ".");
        return Fail(LMXXF_NR_INVALID_ARGUMENT, text);
    }

    int32_t RejectAdoptedLegacyPath(const char* text) noexcept
    {
        RememberError(text);
        if (!adoptedLegacyRejected.exchange(true, std::memory_order_relaxed))
            nr::logf("[mochizuki] REJECTED: host is driving the legacy execution path on an adopted vkd3d device. "
                     "OptiScaler.dll is older than MochizukiNrRuntime.dll — install the FULL package (both DLLs from "
                     "the same build). Runtime commit=" MZ_BUILD_COMMIT ".");
        return Fail(LMXXF_NR_INVALID_ARGUMENT, text);
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
        if (computePool) vkDestroyCommandPool(vk.device, computePool, nullptr);
        if (phaseSemaphore) vkDestroySemaphore(vk.device, phaseSemaphore, nullptr);
        computePool = {}; phaseSemaphore = {}; phaseValue = 0;
        for (auto& x : phaseGraphics) x.clear(); for (auto& x : phaseCompute) x.clear();
        toVk.Release(vk.device);
        fromVk.Release(vk.device);
        vk.Destroy();
        vk = {};
        vram.Close();
    }

    void PushGpuSample(float total, float network, float encode, float transfer)
    {
        std::lock_guard stats(statsMutex);
        ++gpuSamples;
        gpuTick = GetTickCount64();
        gpuMs[gpuMsNext] = total;
        networkMs[gpuMsNext] = network;
        otherMs[gpuMsNext] = std::max(total - network, 0.f);
        encodeMsLast = encode; transferMsLast = transfer;
        gpuMsNext = (gpuMsNext + 1) % kGpuSamples;
        gpuMsCount = std::min(gpuMsCount + 1, kGpuSamples);
    }

    // Vulkan core/stage TOTAL median and p95, not pure network dispatch time.
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
        for (ID3D12Resource* r : { colourInput && colourInput->Shared() ? colourInput->texture : input.resource, motionInput && motionInput->Shared() ? motionInput->texture : motionBuffer.resource, output.resource, result, firstOutput.resource, firstResult })
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
        b.colourInput = std::move(colourInput);
        b.motionInput = std::move(motionInput);
        b.geometry = std::exchange(geometry, {});
        b.resultDesc = bufferResultDesc;
        b.colourFootprint = colourFootprint;
        b.motionFootprint = motionFootprint;
        b.colourVk = colourVk;
        b.motionVk = motionVk;
        b.input = std::exchange(input, {});
        b.output = std::exchange(output, {});
        b.firstOutput = std::exchange(firstOutput, {});
        b.firstColour = std::exchange(firstColour, {});
        b.firstResult = std::exchange(firstResult, nullptr);
        b.motionBuffer = std::exchange(motionBuffer, {});
        b.colourImage = std::exchange(colourImage, {});
        b.canonicalColour = std::exchange(canonicalColour, {});
        b.convert = std::exchange(convert, {});
        b.motionImage = std::exchange(motionImage, {});
        b.motionScaled = std::exchange(motionScaled, {});
        b.motionRescale = std::exchange(motionRescale, {});
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
        const double passes = PassMemoryFactor(key.maxPasses);
        const UINT64 activations = UINT64(double(kModelPixelBytes * model(key.width) * model(key.height)) * passes);
        const UINT64 scaled = key.scale < 1.f ? kScaledFramePixelBytes * key.width * key.height : 0;
        // Conservative upper bound for the optional private FP16 input/output
        // conversion images. Direct input/store routes may omit one or both.
        const UINT64 computeConversions = vk.computeQueue && key.scale >= 1.f &&
            key.format == VK_FORMAT_R16G16B16A16_SFLOAT ? UINT64(key.width) * key.height * 16 + 131072 : 0;
        // Includes optional RenoDX codec, look intermediates and distinct per-pass
        // histories. Keep admission conservative before lazy stage allocations.
        const UINT64 renoBytes = key.renodx ? UINT64(key.width) * key.height * 128 +
            UINT64(key.renoWidth) * key.renoHeight * (64 + 48 * key.maxPasses) : 0;
        return kNetworkFixedBytes + activations + scaled + computeConversions + renoBytes +
               (key.firstPass ? UINT64(key.width) * key.height * 16 + 65536 : 0);
    }

    // What the network for `key` costs in VRAM: the estimate, or what building or releasing it measured when that was
    // more.
    UINT64 NetworkBytes(const NetworkKey& key) const
    {
        const UINT64 estimate = NetworkEstimate(key);
        return measuredKey == key ? std::max(estimate, measuredBytes) : estimate;
    }

    // The frame's buffers for g; a bucketed geometry with motion adds its bucket-sized motion image.
    static UINT64 FrameBytes(const Geometry& g, const ImageInput* actualColour = nullptr, const ImageInput* actualMotion = nullptr)
    {
        const UINT64 scaled = g.bucketed ? MotionFormat(g.motionFormat).bytes : 0;
        UINT64 first = 0;
        if (g.firstPass)
        {
            // Plan A: the buffers are built for the game's own format, which colourFormat may no longer name.
            const UINT64 pixelBytes = ColourFormat(g.gameColourFormat ? g.gameColourFormat : g.colourFormat).bytes;
            const UINT64 pitch = (UINT64(g.width) * pixelBytes + 255) & ~UINT64(255);
            // The shared buffer/Vulkan image use the bucket, but the returned
            // D3D texture preserves the game's full colour allocation extent.
            first = pitch * g.height + pixelBytes * g.width * g.height +
                    pixelBytes * g.colourWidth * g.colourHeight + 3 * 65536;
        }
        const auto colourFormat = g.gameColourFormat ? g.gameColourFormat : g.colourFormat;
        const UINT64 colourBytes = ColourFormat(colourFormat).bytes;
        const UINT64 pitch = (UINT64(g.width) * colourBytes + 255) & ~UINT64(255);
        const UINT64 buffer = std::max((pitch * g.height + 65535) & ~UINT64(65535), UINT64(16) << 20);
        const UINT64 ordinaryColour = buffer + colourBytes * g.width * g.height + 65536;
        // Preserve the calibrated allowance for the other resources. Account for
        // the buffer floor, then replace only the colour term for an admitted route.
        UINT64 ordinaryMotion = 0;
        if (g.motionFormat) {
            const UINT64 bpp = MotionFormat(g.motionFormat).bytes;
            const UINT64 motionPitch = (UINT64(g.motionWidth) * bpp + 255) & ~UINT64(255);
            ordinaryMotion = std::max((motionPitch * g.motionHeight + 65535) & ~UINT64(65535), UINT64(16) << 20) +
                             bpp * g.motionWidth * g.motionHeight + 65536;
        }
        const UINT64 baseline = std::max({kFramePixelBytes * g.width * g.height,
            ordinaryColour + UINT64(32) * g.width * g.height,
            ordinaryColour + ordinaryMotion + UINT64(16) * g.width * g.height});
        return baseline - ordinaryColour - ordinaryMotion + (actualColour ? actualColour->bytes : ordinaryColour) +
               (actualMotion ? actualMotion->bytes : ordinaryMotion) + scaled * g.width * g.height + first;
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
        if (colourInput && colourInput->Shared()) {
            // The recording's retained carrier already holds the captured input.
            // Publish the same ownership/fence handoff as NR; RecordOutputs copies
            // this texture directly, so passthrough needs no buffer download.
            colourInput->Ownership(c, true, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            colourInput->Ownership(c, false, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        } else {
            // Offsets are zero but on the Proton direct path (vkd3d-proton's buffers).
            VkBufferCopy toOutput = region;
            toOutput.srcOffset += input.offset;
            toOutput.dstOffset += output.offset;
            vkCmdCopyBuffer(c, input.buffer, output.buffer, 1, &toOutput);
            if (firstOutput.buffer)
            {
                VkBufferCopy toFirst = region;
                toFirst.srcOffset += input.offset;
                toFirst.dstOffset += firstOutput.offset;
                vkCmdCopyBuffer(c, input.buffer, firstOutput.buffer, 1, &toFirst);
            }
        }
        VkCheck(vkEndCommandBuffer(c), "vkEndCommandBuffer (passthrough)");
        fallbackColourGeneration[k] = colourInput ? colourInput->generation : 0;
        fallbackOutput[k] = output.buffer;
        fallbackOutputOffset[k] = output.offset;
        fallbackWidth[k] = job.width; fallbackHeight[k] = job.height;
        fallbackCurrent[k] = true;
    }

    // The frame's buffers for g, and Super Resolution's texture as `desc`, into b; on a throw b holds what was made.
    // Any thread: the device calls are free-threaded, and nothing the frames use is touched.
    void MakeBuffers(FrameBuffers& b, const Geometry& g, const D3D12_RESOURCE_DESC& desc)
    {
        // Plan A: the colour buffers and colourImage are built for the *game's* format (gameColourFormat), since
        // the upload (a buffer-to-image copy) cannot convert; the colourFormat the equality test sees may be the
        // canonical one. They always agree unless the frame is canonicalised.
        const DXGI_FORMAT colourDxgi = g.gameColourFormat ? g.gameColourFormat : g.colourFormat;
        const Format cf = ColourFormat(colourDxgi), mf = MotionFormat(g.motionFormat);
        b.geometry = g;
        b.resultDesc = desc;
        b.colourFootprint = Footprint(colourDxgi, cf.bytes, g.width, g.height);
        b.colourVk = cf.vk;
        if (vk.adoptedVkd3d) {
            // Same-device image adapters only. No D3D12/Vulkan transfer buffers,
            // external memory, ownership handoff, or private frame submit.
            b.colourImage.Create(vk, cf.vk, g.width, g.height,
                                 g.canonicalise ? kImageUsage | VK_IMAGE_USAGE_STORAGE_BIT : kImageUsage);
        } else {
            b.colourInput = std::make_shared<ImageInput>(vk.device);
            b.colourInput->CreateBuffer(device, vk, b.colourFootprint, cf.vk, g.width, g.height,
                                       g.canonicalise ? kImageUsage | VK_IMAGE_USAGE_STORAGE_BIT : kImageUsage);
            b.input = b.colourInput->upload;
            b.colourImage = b.colourInput->image;
            b.output.Create(device, vk, b.colourFootprint.bytes, "output");
        }
        // Plan A: when the frame is canonicalised the answer comes back out of the canonical image *into this
        // one* (the readback's layout is the game's), so it needs to be a storage image as well as a sampled
        // and transfer one. Otherwise the plain usage, which is all a non-canonicalised frame ever needs.

        // Plan A: when the geometry says to canonicalise (the frame's format differs from the session's first-seen
        // one), a canonical image is made here and the frame is converted into it by a compute pass (RunNetwork),
        // so a colour-format flip costs one dispatch instead of a whole network rebuild. Otherwise colourImage is
        // fed as is, exactly as before Plan A.
        if (g.canonicalise)
        {
            b.canonicalColour.Create(vk, g.canonicalVk, g.width, g.height,
                                     kImageUsage | VK_IMAGE_USAGE_STORAGE_BIT);
            b.convert.Create(vk, convertPipeline, b.colourImage.image, b.colourVk, b.canonicalColour.image,
                             g.canonicalVk);
        }
        b.motionVk = mf.vk;
        if (mf.vk)
        {
            b.motionFootprint = Footprint(g.motionFormat, mf.bytes, g.motionWidth, g.motionHeight);
            if (vk.adoptedVkd3d)
                b.motionImage.Create(vk, mf.vk, g.motionWidth, g.motionHeight, kImageUsage);
            else {
                b.motionInput = std::make_shared<ImageInput>(vk.device);
                b.motionInput->CreateBuffer(device, vk, b.motionFootprint, mf.vk, g.motionWidth, g.motionHeight, kImageUsage);
                b.motionBuffer = b.motionInput->upload;
                b.motionImage = b.motionInput->image;
            }
            if (g.bucketed)
                b.motionScaled.Create(vk, mf.vk, g.width, g.height,
                                      g.computeMotion ? kImageUsage | VK_IMAGE_USAGE_STORAGE_BIT : kImageUsage);
            if (g.computeMotion)
                b.motionRescale.Create(vk, motionPipeline, b.motionImage.image, mf.vk, b.motionScaled.image, mf.vk);
        }
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                              IID_PPV_ARGS(&b.result)),
              "output texture");
        if (g.firstPass) {
            if (!vk.adoptedVkd3d)
                b.firstOutput.Create(device, vk, b.colourFootprint.bytes, "first-pass output");
            b.firstColour.Create(vk, cf.vk, g.width, g.height);
            Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&b.firstResult)),
                "first pass output texture");
        }
        // The caller owns this set. Session geometry keeps one; each in-flight recording checks out its own.
        auto* raw = new FrameBuffers(b);
        // Each snapshot retains its colour owner directly. The allocation of the
        // other resources must not pin an obsolete buffer carrier after a route switch.
        if (!vk.adoptedVkd3d) {
            raw->input = {}; raw->colourImage = {}; raw->colourInput.reset();
            raw->motionBuffer = {}; raw->motionImage = {}; raw->motionInput.reset();
        }
        b.allocation = std::shared_ptr<FrameBuffers>(raw, [dev = vk.device](FrameBuffers* p) {
            p->Release(dev); delete p;
        });
    }

#include "MochizukiColourRouting.inl"
#include "MochizukiMotionRouting.inl"

    // The frames take b over (there is no geometry now); its passthrough is recorded when first used. Render thread.
    void UseBuffers(FrameBuffers& b)
    {
        allocation = std::move(b.allocation);
        colourInput = std::move(b.colourInput);
        motionInput = std::move(b.motionInput);
        bufferResultDesc = b.resultDesc;
        colourFootprint = b.colourFootprint;
        motionFootprint = b.motionFootprint;
        colourVk = b.colourVk;
        motionVk = b.motionVk;
        input = std::exchange(b.input, {});
        output = std::exchange(b.output, {});
        firstOutput = std::exchange(b.firstOutput, {});
        firstColour = std::exchange(b.firstColour, {});
        firstResult = std::exchange(b.firstResult, nullptr);
        motionBuffer = std::exchange(b.motionBuffer, {});
        colourImage = std::exchange(b.colourImage, {});
        canonicalColour = std::exchange(b.canonicalColour, {});
        convert = std::exchange(b.convert, {});
        motionImage = std::exchange(b.motionImage, {});
        motionScaled = std::exchange(b.motionScaled, {});
        motionRescale = std::exchange(b.motionRescale, {});
        result = std::exchange(b.result, nullptr);
        geometry = b.geometry;
    }

    void NotePoolGeometry()
    {
        if (!geometry.width) return;
        if (!poolHot.width) { poolHot = geometry; return; }
        if (poolHot == geometry) return;
        poolPrevious = poolHot;
        poolHavePrevious = true;
        poolHot = geometry;
    }

    // Drop idle copies that are neither the live geometry nor the one before it.
    // A resolution that bounces between two sizes keeps both. A third size does not
    // stay until session teardown. In-flight sets are outstanding, not in the pool.
    void TrimIdleRecordingPool()
    {
        NotePoolGeometry();
        if (!poolHot.width || recordingPool.empty()) return;
        const auto drop = DlssNr::Backend::IdlePoolDropIndices(
            recordingPool.size(), poolHot, poolHavePrevious, poolPrevious,
            [&](size_t i) -> const Geometry& { return recordingPool[i].geometry; },
            [](const Geometry& a, const Geometry& b) { return a == b; });
        if (drop.empty()) return;
        for (size_t n = drop.size(); n-- > 0;)
        {
            const size_t i = drop[n];
            recordingPool[i].Release(vk.device);
            recordingPool.erase(recordingPool.begin() + static_cast<std::ptrdiff_t>(i));
        }
        poolTrimReleases += static_cast<uint32_t>(drop.size());
        nr::logf("[mochizuki] idle recording pool dropped %u entries outside the current and previous geometry (pool=%u outstanding=%u total_dropped=%u)",
                 static_cast<unsigned>(drop.size()), static_cast<unsigned>(recordingPool.size()),
                 recordingBuffersOutstanding, poolTrimReleases);
    }

    // A physical set for one in-flight recording. The session template stays installed
    // so EnsureNetwork can keep building; BoundRecording swaps this set in for Record/Enqueue.
    bool CheckoutRecordingBuffers(FrameBuffers& out)
    {
        if (!geometry.width) return false;
        TrimIdleRecordingPool();
        auto take = [&](FrameBuffers& slot)
        {
            out = std::move(slot);
            ++recordingBuffersOutstanding;
            return out.result != nullptr || out.input.resource != nullptr;
        };
        for (size_t i = 0; i < recordingPool.size(); ++i)
        {
            if (!(recordingPool[i].geometry == geometry) || !SameDesc(recordingPool[i].resultDesc, bufferResultDesc))
                continue;
            const bool ok = take(recordingPool[i]);
            recordingPool.erase(recordingPool.begin() + static_cast<std::ptrdiff_t>(i));
            return ok;
        }
        // Idle copies only. A completed recording keeps its own set until Collect,
        // so the in-flight cap is not a cap on retained recipes.
        while (recordingPool.size() >= kRecordingSlots)
        {
            FrameBuffers extra = std::move(recordingPool.back());
            recordingPool.pop_back();
            extra.Release(vk.device);
        }
        FrameBuffers made;
        try { MakeBuffers(made, geometry, bufferResultDesc); }
        catch (...) { made.Release(vk.device); throw; }
        return take(made);
    }

    void RecycleRecordingBuffers(FrameBuffers& b)
    {
        if (recordingBuffersOutstanding) --recordingBuffersOutstanding;
        TrimIdleRecordingPool();
        const bool live = static_cast<bool>(b.allocation) || b.result || b.input.resource;
        if (!live) return;
        const bool same = b.geometry == geometry && SameDesc(b.resultDesc, bufferResultDesc);
        // A buffer checked out before the latest resize is the previous hot geometry.
        // Keep it so a bounce back does not allocate again. Any older size was already trimmed.
        const bool previousHot = poolHavePrevious && b.geometry == poolPrevious;
        if ((!same && !previousHot) || recordingPool.size() >= kRecordingSlots)
        {
            b.Release(vk.device);
            return;
        }
        recordingPool.push_back(std::move(b));
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

    // ACO is process-wide and fixed at device creation. own_directory() keeps NR_ACO_* from
    // overriding the product switch. The loaded network is aco/shaders plus aco/shaders/temporal.
    // Windows dlssnr-amd/shaders is the native shell and runtime source, not the hashed corpus.
    // Each network SPIR-V must have a record whose spv_hash matches pal::hash of those bytes.
    // A shell is the same name or its alias under the native shader directories, or the ACO
    // SPIR-V itself (the official fallback). An incomplete match does not configure.
    bool ConfigureAco(bool requested, std::string& why)
    {
        // This thread only. The builder thread configures again for the network it is building.
        nr::binary::own_directory();
        nr::binary::configure("", {}, "", {});
        why.clear();
        if (!requested)
            return false;
        try
        {
            namespace fs = std::filesystem;
            const fs::path root = fs::path(assets) / L"dlssnr-amd";
            const fs::path bundle = root / L"aco";
            const fs::path records = bundle / L"records";
            const fs::path acoShaders = bundle / L"shaders";
            const fs::path acoTemporal = acoShaders / L"temporal";
            const fs::path native = root / L"shaders";
            const fs::path nativeTemporal = native / L"temporal";
            auto directory = [](const fs::path& path)
            {
                std::error_code ec;
                return fs::is_directory(path, ec);
            };
            auto regular = [](const fs::path& path)
            {
                std::error_code ec;
                return fs::is_regular_file(path, ec);
            };
            if (!directory(bundle) || !directory(records) || !directory(acoShaders) || !directory(acoTemporal) ||
                !directory(native) || !regular(bundle / L"shell-aliases.txt"))
            {
                why = "ACO bundle is incomplete; the driver's compiler is used";
                nr::logf("[mochizuki] %s", why.c_str());
                return false;
            }
            std::map<std::string, std::string> aliases;
            {
                std::ifstream aliasFile(bundle / L"shell-aliases.txt");
                std::string line;
                while (std::getline(aliasFile, line))
                {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (line.empty() || line[0] == '#')
                        continue;
                    std::istringstream fields(line);
                    std::string from, to, extra;
                    if (!(fields >> from >> to) || (fields >> extra) || from.size() < 5 || to.size() < 5)
                    {
                        why = "ACO shell aliases could not be read; the driver's compiler is used";
                        nr::logf("[mochizuki] %s", why.c_str());
                        return false;
                    }
                    aliases.emplace(std::move(from), std::move(to));
                }
                if (aliasFile.bad())
                {
                    why = "ACO shell aliases could not be read; the driver's compiler is used";
                    nr::logf("[mochizuki] %s", why.c_str());
                    return false;
                }
            }
            std::vector<fs::path> shellDirs;
            if (directory(bundle / L"shell"))
                shellDirs.push_back(bundle / L"shell");
            shellDirs.push_back(native);
            if (directory(nativeTemporal))
                shellDirs.push_back(nativeTemporal);
            auto shellFile = [&](const std::string& name)
            {
                for (const fs::path& dir : shellDirs)
                    if (regular(dir / name))
                        return true;
                return false;
            };
            int checked = 0, matched = 0, nativeShells = 0, aliasShells = 0, selfShells = 0;
            auto consider = [&](const fs::path& file)
            {
                const auto name = file.filename().string();
                if (name.find("nobda") != std::string::npos || !nr::binary::network_path(name))
                    return;
                std::ifstream in(file, std::ios::binary);
                std::vector<char> bytes;
                if (in)
                    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                ++checked;
                if (bytes.empty())
                    return;
                const uint64_t hash = nr::pal::hash(bytes.data(), bytes.size());
                char key[24];
                std::snprintf(key, sizeof key, "%016llx.nrp", static_cast<unsigned long long>(hash));
                std::ifstream recordFile(records / key, std::ios::binary);
                std::vector<char> raw;
                if (recordFile)
                    raw.assign(std::istreambuf_iterator<char>(recordFile), std::istreambuf_iterator<char>());
                bool content = false;
                if (!raw.empty())
                {
                    try
                    {
                        const nr::pal::Bytes view(raw.begin(), raw.end());
                        content = nr::pal::Record::parse(view).spv_hash == hash;
                    }
                    catch (const std::exception&)
                    {
                        content = false;
                    }
                }
                if (content)
                    ++matched;
                if (shellFile(name))
                    ++nativeShells;
                else
                {
                    const auto alias = aliases.find(name);
                    if (alias != aliases.end() && shellFile(alias->second))
                        ++aliasShells;
                    else
                        ++selfShells;
                }
            };
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(acoShaders, ec))
                if (!ec && entry.is_regular_file())
                    consider(entry.path());
            ec.clear();
            for (const auto& entry : fs::directory_iterator(acoTemporal, ec))
                if (!ec && entry.is_regular_file())
                    consider(entry.path());
            if (!checked || matched != checked)
            {
                char text[160];
                std::snprintf(text, sizeof text,
                              "ACO records do not match this build's network SPIR-V (%d/%d); the driver's compiler is used",
                              matched, checked);
                why = text;
                nr::logf("[mochizuki] %s", why.c_str());
                return false;
            }
            nr::logf("[mochizuki] ACO records %d/%d match aco/shaders; shells native=%d alias=%d self=%d", matched,
                     checked, nativeShells, aliasShells, selfShells);
            // Cache is local disposable driver data, never package/model input.
            std::wstring localAppData(32768, L'\0');
            const DWORD localLen = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), DWORD(localAppData.size()));
            fs::path cache;
            if (localLen && localLen < localAppData.size()) {
                localAppData.resize(localLen);
                if (fs::path(localAppData).is_absolute()) {
                    cache = fs::path(localAppData) / L"OptScaler-NR" / L"Mochizuki" / L"aco-cache";
                    fs::create_directories(cache, ec);
                    if (ec) cache.clear();
                }
            }
            std::vector<std::string> shellUtf8;
            shellUtf8.reserve(shellDirs.size());
            for (const fs::path& dir : shellDirs)
                shellUtf8.push_back(RootUtf8(dir.wstring()));
            nr::binary::configure(RootUtf8(bundle.wstring()), std::move(shellUtf8),
                                  cache.empty() ? std::string{} : RootUtf8(cache.wstring()),
                                  std::move(aliases));
            return true;
        }
        catch (const std::exception& e)
        {
            nr::binary::configure("", {}, "", {});
            why = "ACO bundle could not be read; the driver's compiler is used";
            nr::logf("[mochizuki] %s: %s", why.c_str(), e.what());
            return false;
        }
    }

    void Start()
    {
        bool requested = false;
        {
            std::lock_guard lock(controlsMutex);
            requested = controls.acoRequested;
            acoAtDevice = false;
            acoEffective = false;
            acoFallback[0] = 0;
        }
        std::string acoWhy;
        const bool configured = ConfigureAco(requested, acoWhy);
        // Proton first: vkd3d-proton's own Vulkan device, so the frames' buffers need no cross-API import (Windows:
        // AdoptVkd3d returns false at once and the device is created as before).
        if (!vk.AdoptVkd3d(device))
            vk.Create(device->GetAdapterLuid(), hooks);
        else
            // The shared textures are D3D12_RESOURCE imports, which winevulkan cannot make; the direct path's
            // buffers are the game's own memory already. PlanShared refuses as well.
            sharedInputRequest = sharedMotionRequest = 0;
        VkPhysicalDeviceDriverProperties driver {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 properties {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
        vkGetPhysicalDeviceProperties2(vk.physical, &properties);
        static_assert(uint32_t(VK_DRIVER_ID_MESA_RADV) == 3);
        compilePolicy = mz::compile::PolicyForDriver(uint32_t(driver.driverID));
        deviceStarted = true;
        // Bundle selection is this thread's. The extension is on only when this creation saw a bundle.
        // effective stays false until Install publishes the network that actually runs.
        const bool deviceAco = !nr::binary::directory().empty() && nr::binary::enabled(vk.device);
        if (!requested && deviceAco)
            acoWhy = "ACO mode applies on the next game start";
        else if (requested && !(configured && deviceAco))
        {
            if (acoWhy.empty())
            {
                acoWhy = nr::binary::disabled_why();
                if (acoWhy.empty())
                    acoWhy = "ACO was not enabled on this device; the driver's compiler is used";
            }
        }
        {
            std::lock_guard lock(controlsMutex);
            if (deviceAco)
                acoAtDevice = true;
            if (mz::compile::Mandatory(compilePolicy))
                std::snprintf(acoFallback, sizeof acoFallback, "RADV Linux SPIR-V driver compile selected (no PAL import)");
            else if (controls.acoRequested == requested)
            {
                if (deviceAco && requested)
                    acoFallback[0] = 0;
                else
                    std::snprintf(acoFallback, sizeof acoFallback, "%s", acoWhy.c_str());
            }
            else
                std::snprintf(acoFallback, sizeof acoFallback, "ACO mode applies on the next game start");
            if (requested || deviceAco)
                nr::logf("[mochizuki] ACO %s%s%s", deviceAco && controls.acoRequested == requested ? "requested" : "off",
                         acoFallback[0] ? ": " : "", acoFallback);
            if (mz::compile::Mandatory(compilePolicy))
                nr::logf("[mochizuki] RADV driver compile policy: Linux SPIR-V required; PAL import off; Windows network fallback forbidden");
        }
        if (!vk.adoptedVkd3d)
        {
            // Prove the transport before any network construction. No work is submitted;
            // release every partial object before AbandonStart can destroy the device.
            SharedBuffer probe;
            try
            {
                probe.Create(device, vk, 65536, "own-device transport probe", true);
            }
            catch (...)
            {
                probe.Release(vk.device);
                throw;
            }
            probe.Release(vk.device);
            nr::logf("[mochizuki] own-device transport probe passed; network construction permitted");
        }
        if (!vk.adoptedVkd3d) {
        toVk.Create(device, vk, "toVk");
        fromVk.Create(device, vk, "fromVk");
        VkCommandPoolCreateInfo pi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.queueFamilyIndex = vk.family;
        if (vk.computeQueue) {
            pi.queueFamilyIndex = vk.computeFamily;
            pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            VkCheck(vkCreateCommandPool(vk.device, &pi, nullptr, &computePool), "compute command pool");
            VkSemaphoreTypeCreateInfo type {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo si {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
            VkCheck(vkCreateSemaphore(vk.device, &si, nullptr, &phaseSemaphore), "phase semaphore");
            pi.queueFamilyIndex = vk.family;
        }
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
        }
        vram.Open(device->GetAdapterLuid());
        // Plan A's colour-format conversion passes, both directions, for R11G11B10 and RGBA16F. They are optional:
        // when one is missing or will not build, a pair that needs it is never converted (CanonicalFor) and the
        // frame keeps its own format, i.e. the pre-Plan-A behaviour (a format change rebuilds the network).
        try
        {
            convertPipeline.Create(vk, { VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT });
            nr::logf("[mochizuki] plan A: conversion passes ready (%zu input, %zu output)",
                     convertPipeline.convertPipelines.size(), convertPipeline.unconvertPipelines.size());
        }
        catch (const std::exception& e)
        {
            nr::logf("[mochizuki] plan A conversion unavailable (%s); a colour-format change rebuilds the network "
                     "as before", e.what());
        }
        try
        {
            motionPipeline.CreateMotion(vk);
        }
        catch (const std::exception& e)
        {
            nr::logf("[mochizuki] motion rescale on compute unavailable (%s); rescaled vectors keep their blit", e.what());
        }
        nr::logf("[mochizuki] Vulkan device %s, queue family %u%s", vk.name.c_str(), vk.family,
                 vk.buildQueue != vk.queue ? ", a second queue for the network builds" : "");
        if (DataDirectory() != DllDirectory())
            nr::logf("[mochizuki] loaded from %s; dlssnr-amd is read from %s", RootUtf8(DllDirectory()).c_str(),
                     RootUtf8(DataDirectory()).c_str());
    }

    // Explain temporary use of the existing capacity through the current status ABI.
    void SetNetworkDeferred(const char* reason)
    {
        std::lock_guard lock(errorMutex);
        std::snprintf(networkDeferred, sizeof networkDeferred, "%s", reason);
    }
    void LogPassCapacity(uint32_t served)
    {
        char reason[192] {};
        { std::lock_guard lock(errorMutex); std::memcpy(reason, networkDeferred, sizeof reason); }
        char text[256] {};
        std::snprintf(text, sizeof text, "requested=%u active=%u capacity=%u reason=%s",
                      infoRequestedPasses.load(), served, net.maxPasses, reason[0] ? reason : "ready");
        if (std::strcmp(text, loggedCapacity) == 0) return;
        std::memcpy(loggedCapacity, text, sizeof text);
        nr::logf("[mochizuki] pass capacity: %s", text);
    }

    // Failed capacity expansion preserves a compatible network and its buffers.
    // Incompatible replacements retain the ordinary passthrough/retry behavior.
    void BuildFailed(bool preserveCurrent)
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
        if (runtime && !preserveCurrent)
        {
            std::shared_ptr<ProductRuntime> old;
            UINT64 finished = 0;
            {
                std::lock_guard lock(submitMutex);
                old = TakeNetwork("the network the frames kept goes with the failed build", finished);
            }
            ReleaseLater(std::move(old), finished);
        }
        if (!preserveCurrent) RetireGeometry();
    }

    // The frames' network, taken from them: no frame records with it after, and `finished` gets the finished value of
    // the last frame handed to the Vulkan side, the last one that may use it. It may go once fromVk is there, which the
    // render thread does not wait for (ReleaseLater, or the builder). `why` goes to the log with the network's time.
    // Under submitMutex.
    std::shared_ptr<ProductRuntime> TakeNetwork(const char* why, UINT64& finished)
    {
        LogNetworkTime(why);
        finished = lastFinished.load();
        infoInt4 = infoReno = infoNetwork = 0;
        exposureSnapshot = runtime && runtime->reno ? runtime->reno->exposure_state() : nullptr;
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
        std::shared_ptr<ProductRuntime> old;
        UINT64 oldFinished = 0;
        {
            std::lock_guard lock(submitMutex);
            if (runtime)
                old = TakeNetwork("replacing the network", oldFinished);
            runtime = std::move(built);
            if (runtime->reno && runtime->reno->seed_from(exposureSnapshot))
                nr::logf("[mochizuki] network #%llu inherited shared scalar exposure state", static_cast<unsigned long long>(runtime->id));
            exposureSnapshot.reset();
            net = buildKey;
            RequestHistoryReset(ResetNetwork);
            historyBits = 0;
            historyCount = 0;
        }
        {
            // Published with the network above. Effective is that candidate's mode.
            // The latest request does not decide whether to publish it; a switch moved during the build
            // only changes the request and, below, whether a native install clears the waiting reason.
            // A candidate that never reaches here leaves the previous network's state.
            std::lock_guard published(controlsMutex);
            if (candidateReady)
            {
                const bool installedAco = runtime->graphAco;
                acoEffective = installedAco;
                if (runtime->graphPolicy == CompilePolicy::LinuxSpirvDriverCompile)
                    std::snprintf(acoFallback, sizeof acoFallback, "RADV driver-compiled Linux SPIR-V (no PAL import)");
                else if (installedAco)
                    acoFallback[0] = 0;
                else if (buildKey.aco)
                {
                    if (candidateWhy[0])
                        std::snprintf(acoFallback, sizeof acoFallback, "%s", candidateWhy);
                    else
                        std::snprintf(acoFallback, sizeof acoFallback,
                                      "ACO was not enabled on this network; the driver's compiler is used");
                }
                else if (!controls.acoRequested)
                    acoFallback[0] = 0;
                candidateReady = false;
            }
        }
        // The buffers made beside it wait for the next frame (MakeGeometry).
        if (prepared)
            prepared->Release(vk.device);
        prepared = std::move(builtBuffers);
        const char* const kept = old ? "; the frames ran on the old one meanwhile" : "";
        if (old)
        {
            old->destroyReason = old->key.networkModel != buildKey.networkModel ? "model_change"
                : (old->key.width != buildKey.width || old->key.height != buildKey.height) ? "extent" : "settings";
            ReleaseLater(std::move(old), oldFinished);
        }
        buildHold = {};
        SetNetworkDeferred("");
        {
            std::lock_guard stats(statsMutex);
            gpuMsCount = gpuMsNext = 0;
            gpuTick = 0;
            encodeMsLast = transferMsLast = -1.f;
            statsNetworkId = statsSerial = 0;
        }
        infoFrameWidth = net.width;
        infoFrameHeight = net.height;
        infoInt4 = runtime->int4_mixed();
        infoNetwork = runtime->network() == nr::NetworkModel::Small ? 1u : 0u;
        infoReno = runtime->reno != nullptr;
        infoModelWidth = runtime->model_width();
        infoModelHeight = runtime->model_height();
        infoMaxPasses = net.maxPasses;
        infoBuildSeconds = float(buildSeconds);
        nr::logf("[mochizuki] network #%llu installed (%ux%u, maxPasses=%u, model=%u)",
                 static_cast<unsigned long long>(runtime->id), net.width, net.height, net.maxPasses,
                 unsigned(runtime->network()));
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
    bool EnsureNetwork(const NetworkKey& key, const Geometry& g, ID3D12Resource* colour, std::string& why,
                       bool automaticCapacity, bool settleExtent)
    {
        static const char* const kBuilding = "building the network (the first time takes about half a minute)";
        // Exact resolution changes and bucket shrinks must be stable for kExactExtentSettleMs before
        // rebuilding. Bucketed subrect changes inside an existing bucket extent do not rebuild.
        if (!settleExtent || !runtime || (net.width == key.width && net.height == key.height))
            extentCandidatePending = false;
        else
        {
            const auto now = GetTickCount64();
            if (!extentCandidatePending || !(extentCandidate == key))
            {
                extentCandidate = key;
                extentCandidateSince = now;
                extentCandidatePending = true;
                if (++extentDeferrals <= 8 || extentDeferrals % 100 == 0)
                    nr::logf("[mochizuki] extent settle #%u: keeping %ux%u; request %ux%u (%s), colour allocation "
                             "%llux%u DXGI %u, motion %ux%u DXGI %u; waiting %llu ms before rebuilding",
                             extentDeferrals, net.width, net.height, key.width, key.height,
                             (key.width < net.width || key.height < net.height) ? "shrinking" : "exact change",
                             static_cast<unsigned long long>(g.colourWidth), g.colourHeight, unsigned(g.colourFormat),
                             g.motionWidth, g.motionHeight, unsigned(g.motionFormat), kExactExtentSettleMs);
            }
            if (now - extentCandidateSince < kExactExtentSettleMs)
            {
                // This incompatible frame uses its own original colour, never an
                // old image. A following frame of the old size can resume NR at once.
                why = "input resolution is settling (keeping the current network)";
                return false;
            }
        }
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
            BuildFailed(runtime && net.SameModel(key) && geometry == g);
        if (built)
            Install();
        // A lower execution count needs no smaller network. Explicit capacity
        // edits still request an exact rebuild; automatic mode retains capacity.
        if (runtime && net.SameModel(key) &&
            (net.maxPasses == key.maxPasses || (automaticCapacity && net.maxPasses >= key.maxPasses)))
        {
            SetNetworkDeferred("");
            return true;
        }
        const bool preserveCurrent = runtime && net.SameModel(key) && geometry == g;
        if (buildHold.Holds(key, GetTickCount64()))
        {
            why = buildHold.message;
            if (preserveCurrent) SetNetworkDeferred(why.c_str());
            return preserveCurrent;
        }
        // With a queue of their own for the builds, the frames keep a network for their extent and format meanwhile.
        const bool serve = Serves(key) && vk.buildQueue != vk.queue;
        // Buffers for another frame go now, unless the frames keep running on them.
        if (!serve && geometry.width && !(geometry == g))
            RetireGeometry();
        // The network, and the frame's buffers unless they are made already, beside what the frames still hold.
        const UINT64 need = NetworkBytes(key) + (geometry == g ? 0 : FrameBytes(g)) + kVramMarginBytes;
        bool admitted = VramAdmits(need, key.width, key.height, "the network", why, &buildBaseUsage);
        if (preserveCurrent && key.maxPasses > net.maxPasses && upgradeRefusals < hooks.upgradeRefusals)
        {
            ++upgradeRefusals;
            admitted = false;
            why = "insufficient VRAM for capacity expansion (test hook)";
        }
        if (!admitted && !preserveCurrent && (!buried.empty() || Releasing()))
        {
            // Release already retired work first, without giving up the network the frames still hold.
            Drain();
            ReleasesDone(kDrainWaitMs);
            admitted = VramAdmits(need, key.width, key.height, "the network", why, &buildBaseUsage);
        }
        const bool freeGeometry = geometry.width && !(geometry == g);
        if (!admitted && !preserveCurrent && (runtime || freeGeometry))
        {
            // Only give up the current network if reclaiming it would make room. The game can take back
            // freed VRAM, so destroying it for an impossible rebuild also loses the immediate way back.
            const UINT64 reclaim = (runtime ? NetworkBytes(net) : 0) + (freeGeometry ? FrameBytes(geometry) : 0);
            std::string notEnough;
            if (VramAdmits(need > reclaim ? need - reclaim : 0, key.width, key.height,
                           "the network once the running one is freed", notEnough))
            {
                Drain();
                if (runtime)
                {
                    std::lock_guard lock(submitMutex);
                    LogNetworkTime("replacing the network");
                    exposureSnapshot = runtime->reno ? runtime->reno->exposure_state() : nullptr;
                    runtime->destroyReason = "vram_reclaim";
                    runtime.reset();
                }
                if (freeGeometry)
                    ReleaseGeometry();
                ReleasesDone(kDrainWaitMs);
                admitted = VramAdmits(need, key.width, key.height, "the network", why, &buildBaseUsage);
            }
            else
                nr::logf("[mochizuki] %s, the running network's %llu MB included: it keeps serving", notEnough.c_str(),
                         static_cast<unsigned long long>(reclaim >> 20));
        }
        if (!admitted)
        {
            buildHold.Refused(key, why.c_str());
            NoteError(why.c_str());
            if (preserveCurrent) SetNetworkDeferred(why.c_str());
            return preserveCurrent;
        }
        const bool keep = (serve || preserveCurrent) && runtime;
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
        std::shared_ptr<ProductRuntime> old;
        UINT64 oldFinished = 0;
        if (runtime && !keep)
        {
            std::lock_guard lock(submitMutex);
            old = TakeNetwork("replacing the network", oldFinished);
            old->destroyReason = old->key.networkModel != key.networkModel ? "model_change"
                : (old->key.width != key.width || old->key.height != key.height) ? "extent" : "settings";
        }
        buildKey = key;
        StartBuild(std::move(old), oldFinished, std::move(buffers), keep);
        why = kBuilding;
        if (preserveCurrent) SetNetworkDeferred(kBuilding);
        return keep;
    }

    // The build for buildKey, on the builder thread (Build). `old`, a network no frame uses any more, is freed there
    // first, once fromVk has reached `oldFinished`; `buffers`, when set, asks for the next frame's buffers too
    // (geometry and resultDesc). `keep`: the frames keep their network meanwhile. On a throw, `old` goes to the
    // releaser (ReleaseLater), never here.
    void StartBuild(std::shared_ptr<ProductRuntime> old, UINT64 oldFinished, std::unique_ptr<FrameBuffers> buffers,
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
            config.network = int(key.networkModel); // Product INI/menu overrides DLSSNR_MODEL and upstream INI.
            config.motion_without_depth = vk.adoptedVkd3d ? 0 : 1; // Direct Linux host supplies game depth; Windows bridge does not.
            // Keep independent graph compilation/import accounting and whole-graph
            // native fallback until cross-network pipeline reuse is separately proved.
            config.share_pipelines = false;
            config.require_style_grade = true; // A complete package must include every grade variant.
            config.max_passes = key.maxPasses;
            config.preprocess = key.prep;
            config.compact_transfer = key.compactTransfer;
            config.separate_answer = !key.compactTransfer;
            config.first_pass_output = key.firstPass;
            config.int4_mixed = key.int4 ? 1 : 0; // INI/menu owns this; never DLSSNR_INT4 or the upstream ini.
            if (key.renodx) {
                config.width = key.renoWidth; config.height = key.renoHeight;
                config.colour_format = VK_FORMAT_R16G16B16A16_SFLOAT;
                config.linear_input = false; config.model_scale = 1.f;
                config.max_passes = 1; config.native_compose = true;
                config.preprocess = false; config.first_pass_output = false;
            }
            config.compute_convert_colour = key.computeConvert;
            config.compute_downscale = key.computeDownscale;
            config.compute_export = key.computeExport;
            config.motion_in_place_passes = key.motionPasses;
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
            if (vk.adoptedVkd3d)
            {
                // Proton direct path: the builds' queue is vkd3d-proton's, shared with the game's queues.
                ID3D12DXVKInteropDevice* interop = vk.interop;
                ID3D12CommandQueue* queue = vk.interopQueue;
                host.queue_lock = [interop, queue] {
                    Check(interop->LockCommandQueue(queue), "LockCommandQueue (Proton network builder)");
                };
                host.queue_unlock = [interop, queue] { interop->UnlockCommandQueue(queue); };
            }
            host.secondary_family = vk.computeFamily;
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
            const bool buildAco = key.aco;
            // No native-plan work or GPU wait on this caller. The builder probes the immutable core.
            // This one-shot reference is not a model cache and is never searched across Sessions.
            auto donor = old ? old : runtime;
            if (inlineQueue || !donor || !donor->key.SameGraphSemantics(key) ||
                donor->queue != vk.queue ||
                donor->network() != (key.networkModel ? nr::NetworkModel::Small : nr::NetworkModel::Original) ||
                donor->int4_mixed() != key.int4 || !donor->network_note().empty())
                donor.reset();
            // Keep the caller's reference until thread creation succeeds, then transfer the
            // lambda's only reference into Build so old.reset there actually releases the net.
            auto handed = old;
            std::lock_guard handoff(buildMutex);
            builder = std::thread(
                [this, host, config, temporal, handed = std::move(handed), donor = std::move(donor), oldFinished, buildAco, key, buffers = std::move(buffers)]() mutable
                {
                    { std::lock_guard ownership(buildMutex); } // caller drops its copy before Build starts
                    Build(host, config, temporal, std::move(handed), oldFinished, std::move(buffers), buildAco, key, std::move(donor));
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
               std::shared_ptr<ProductRuntime> old, UINT64 oldFinished, std::unique_ptr<FrameBuffers> buffers,
               bool buildAco, const NetworkKey& key, std::shared_ptr<ProductRuntime> donor)
    {
        const auto t0 = std::chrono::steady_clock::now();
        nr::g_build_stage.store(0, std::memory_order_relaxed);
        {
            std::lock_guard lock(progressMutex);
            progress = {sizeof(progress), 1, 0, 0, GetTickCount64(), GetTickCount64()};
            progressLogTick = 0;
        }
        ReportProgress(this, "Waiting for previous GPU work", 0, 0);
        if (old)
        {
            const uint64_t oldId = old->id;
            if (WaitFinished(oldFinished, [this] { return abandon.load(); }) == VK_SUCCESS)
            {
                UINT64 usageBefore = 0, usage = 0, budget = 0;
                const bool knownBefore = vram.Read(usageBefore, budget);
                old.reset();
                const bool knownAfter = vram.Read(usage, budget);
                nr::logf("[mochizuki] network #%llu builder reference dropped (process VRAM %llu -> %llu MB, "
                         "budget %llu MB, known=%d)", static_cast<unsigned long long>(oldId),
                         static_cast<unsigned long long>(usageBefore >> 20), static_cast<unsigned long long>(usage >> 20),
                         static_cast<unsigned long long>(budget >> 20), knownBefore && knownAfter ? 1 : 0);
                if (buildBaseUsage && knownAfter)
                    buildBaseUsage = usage; // the growth is measured from after the release
            }
            else
            {
                RetireNetwork(std::move(old), oldFinished);
                donor.reset(); // No sharing after an unresolved previous completion.
            }
        }
        std::shared_ptr<ProductRuntime> made;
        char error[sizeof buildError] {};
        NrError::Kind kind = NrError::Other;
        bool acoOn = false;
        std::string acoWhy;
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
            if (hooks.upgradeOomOnce && config.max_passes > 1 && !upgradeOomInjected.exchange(true))
                throw std::runtime_error("vkAllocateMemory (MZ_TEST_UPGRADE_OOM_ONCE test hook): VkResult=-2");
            if (hooks.buildOomOnce && !g_buildOomInjected.exchange(true))
                throw std::runtime_error("vkAllocateMemory (MZ_TEST_BUILD_OOM_ONCE test hook): VkResult=-2");
            // The compiler choice is local to this build thread. Pipelines already created stay with their
            // recording. A failed import discards that graph and builds a complete native one.
            // own_directory() keeps NR_ACO_* from selecting a bundle on this thread.
            mz::compile::BackgroundScope policyScope(key.compilePolicy,
                [&](CompilePolicy policy)
                {
                    nr::binary::own_directory();
                    nr::binary::configure("", {}, "", {});
                    if (mz::compile::Mandatory(policy))
                    {
                        const auto bundle = std::filesystem::path(assets) / L"dlssnr-amd" / L"aco";
                        nr::binary::configure(RootUtf8(bundle.wstring()), {}, "", {}, false);
                    }
                }, []() noexcept { nr::binary::configure("", {}, "", {}); });
            auto useNative = [&](const std::string& reason)
            {
                mz::compile::GuardWindowsFallback(key.compilePolicy);
                acoOn = false;
                acoWhy = reason;
                nr::logf("[mochizuki] ACO unavailable: %s; using the driver compiler", reason.c_str());
                nr::binary::configure("", {}, "", {});
            };
            if (!mz::compile::Mandatory(key.compilePolicy) && buildAco)
            {
                try
                {
                    if (!nr::binary::enabled(host.device))
                        useNative("this device does not support pipeline binaries");
                    else if (!ConfigureAco(true, acoWhy))
                        useNative(acoWhy);
                }
                catch (const std::exception& e)
                {
                    if (abandon || KindOf(e) != NrError::Other)
                        throw;
                    useNative(e.what());
                }
            }
            auto construct = [&]()
            {
                nr::RuntimeConfig local = config;
                const bool linuxGraph = !nr::binary::directory().empty();
                const bool importing = linuxGraph && nr::binary::import_enabled();
                if (donor && donor->graphAco == importing && donor->graphPolicy == key.compilePolicy)
                    local.share = donor->share_for(config.width, config.height);
                if (!local.share)
                    donor.reset();
                // share_for can reject geometries; the core can reject arguments. Both use its real full build.
                // No with_slot_of: its per-plan mutable tail/noise protocol needs transactional enqueue proof.
                return std::make_shared<ProductRuntime>(host, local, temporal, key, vk);
            };
            if (!nr::binary::directory().empty())
            {
                try
                {
                    const nr::BuildCancelScope cancel(&abandon);
                    nr::g_build_stage.store(1, std::memory_order_relaxed);
                    ReportProgress(this, key.compilePolicy == CompilePolicy::LinuxSpirvDriverCompile
                        ? "Building RADV Linux SPIR-V network" : "Building ACO network", 0, 0);
                    made = construct();
                    nr::g_build_stage.store(0, std::memory_order_relaxed);
                    const uint32_t imported = nr::binary::mode().imported;
                    if (nr::binary::import_enabled())
                    {
                        if (!imported)
                            throw std::runtime_error("no ACO pipelines were imported");
                        acoOn = true;
                        nr::logf("[mochizuki] ACO network ready: %u imported pipelines", imported);
                    }
                    else
                        nr::logf("[mochizuki] RADV network ready: driver-compiled Linux SPIR-V (no PAL import)");
                    acoWhy.clear();
                }
                catch (const std::exception& e)
                {
                    nr::g_build_stage.store(0, std::memory_order_relaxed);
                    if (abandon || KindOf(e) != NrError::Other)
                        throw;
                    made.reset();
                    useNative(e.what());
                }
            }
            if (!made)
            {
                // Native description cache. It is not used for an ACO graph: those pipelines are imported,
                // and a fallback reaches here only after the imported graph was discarded.
                mzi::Capture pipelines(host.device, host.physical, assets);
                ReportProgress(this, "Prewarming cached shader descriptions", 0, 0);
                pipelines.Prewarm(&abandon);
                if (abandon)
                    throw std::runtime_error("stopped: the session is being destroyed");
                const nr::BuildCancelScope cancel(&abandon);
                nr::g_build_stage.store(1, std::memory_order_relaxed);
                ReportProgress(this, "Preparing network graph", 0, 0);
                made = construct();
                nr::g_build_stage.store(0, std::memory_order_relaxed);
                ReportProgress(this, "Saving shader cache", 0, 0);
                pipelines.Finish();
            }
        }
        catch (const std::exception& e)
        {
            nr::g_build_stage.store(0, std::memory_order_relaxed);
            kind = KindOf(e);
            std::snprintf(error, sizeof error, "network build failed at %ux%u: %s", config.width, config.height,
                          e.what());
        }
        catch (...)
        {
            nr::g_build_stage.store(0, std::memory_order_relaxed);
            std::snprintf(error, sizeof error, "network build failed at %ux%u: an unknown exception", config.width,
                          config.height);
        }
        // Releasing a donor never changes a retained recipe: it owns its original runtime/images.
        // Shared arenas/core survive through the candidate's own shared ownership.
        donor.reset();
        if (made) { made->graphAco = acoOn; made->graphPolicy = key.compilePolicy; }
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
                // Admit the required buffers after the candidate holds its own share.
                // A refusal discards this uninstalled candidate, not the active network.
                const Geometry& g = buffers->geometry;
                std::string why;
                const bool admitted = VramAdmits(FrameBytes(g) + kVramMarginBytes, g.width, g.height,
                                                 "the frame's buffers", why);
                const uint64_t frameBudgetAttempt = g_frameBudgetAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
                const bool injected = hooks.frameBudgetAt != 0 && frameBudgetAttempt == hooks.frameBudgetAt;
                if (!admitted || injected)
                    throw NrError(NrError::OutOfMemory, injected ? "frame buffer budget refused (test hook)" : why);
                MakeBuffers(*buffers, g, buffers->resultDesc);
            }
            catch (const std::exception& e)
            {
                buffers->Release(vk.device);
                buffers.reset();
                // A network without its required buffers is not installable.
                // It has never served a recording, so no frame owns it yet.
                made.reset();
                kind = KindOf(e);
                std::snprintf(error, sizeof error, "candidate frame buffers failed at %ux%u: %s",
                              config.width, config.height, e.what());
                nr::logf("[mochizuki] %s", error);
            }
            catch (...)
            {
                buffers->Release(vk.device);
                buffers.reset();
                made.reset();
                kind = NrError::Other;
                std::snprintf(error, sizeof error, "candidate frame buffers failed at %ux%u", config.width, config.height);
            }
        }
        else if (buffers)
            buffers.reset();
        {
            std::lock_guard progressLock(progressMutex);
            progress.active = 0;
        }
        std::unique_lock lock(buildMutex);
        candidateReady = static_cast<bool>(made);
        candidateOn = candidateReady && acoOn;
        if (candidateReady)
            std::snprintf(candidateWhy, sizeof candidateWhy, "%s", acoWhy.c_str());
        else
            candidateWhy[0] = 0;
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
            exposureSnapshot = runtime && runtime->reno ? runtime->reno->exposure_state() : nullptr;
            if (runtime) runtime->destroyReason = "vram_refused";
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

    // Plan A: Vulkan's vkCmdBlitImage only accepts formats in the *same* format compatibility class --
    // the BLIT_SRC/BLIT_DST feature bits are necessary but not sufficient. The classes are keyed by
    // (bits per pixel, component count), so B10G11R11_UFLOAT_PACK32 (32 bpp, 4 lanes) and
    // R16G16B16A16_SFLOAT (64 bpp) are in DIFFERENT classes and a blit between them is an invalid
    // command. That invalid command is recorded into the game's command list, which poisons it:
    // Close() then fails with E_INVALIDARG (so it looks like "already closed", and the split path dies)
    // and the device is removed with DXGI_ERROR_INVALID_CALL -- which surfaces later as the
    // AllocateTexture / DXGI_ERROR_DEVICE_REMOVED crash (seen on the RGBA16F save-load screen).
    // Returns 0 for a format we have no class for, so it is never treated as cross-convertible. Kept as the
    // record of why the conversion is a compute pass and not a blit; the conversion never calls this.
    [[maybe_unused]] static int BlitClass(VkFormat f)
    {
        switch (f)
        {
        // 32 bpp, 4 lanes.
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
            return 0x20 | 4; // 32 bpp, 4 lanes
        // 64 bpp, 4 lanes.
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT:
            return 0x40 | 4;
        // 128 bpp, 4 lanes.
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
            return 0x80 | 4;
        // 32 bpp, 2 lanes.
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16_SINT:
            return 0x20 | 2;
        // 64 bpp, 2 lanes.
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
            return 0x40 | 2;
        default:
            return 0; // unknown: never assume cross-class blit is legal
        }
    }

    // Plan A: whether this GPU can convert the game's colour format `from` into canonical format `to` and the
    // answer back again. The conversion is a compute pass (ConvertPass), not a blit, so the formats need NOT
    // share a Vulkan format compatibility class (see BlitClass). Both formats must be samplable (each is read by
    // one of the passes) and usable as a storage image (each is written by the other), and both pipelines must
    // exist. Without the output direction the frame would be read back with the wrong layout, which is worse
    // than not converting at all. Render thread, cached like Blittable.
    //
    // The bits come from VkFormatFeatureFlags (the `formatFeatures` cache holds that field, as Blittable does);
    // the sampled/storage-image bits have the same values in VkFormatFeatureFlags2.
    bool ColourConvertible(VkFormat from, VkFormat to)
    {
        if (from == VK_FORMAT_UNDEFINED || to == VK_FORMAT_UNDEFINED || from == to ||
            !convertPipeline.Convert(to) || !convertPipeline.Unconvert(from))
            return false;
        auto flags = [&](VkFormat f)
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
            return known->second;
        };
        constexpr VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        return (flags(from) & need) == need && (flags(to) & need) == need;
    }

    // Plan A: the canonical format this frame is converted into, or UNDEFINED for the original path.
    // Render thread only.
    //  * The first frame records its format as the session's canonical one (the network is built for it) and
    //    takes the original path. So does every later frame in that same format: a game that never switches
    //    formats runs exactly as before Plan A, with no canonical image and no dispatch.
    //  * A frame in another format is converted into the canonical one when the pair can go both ways
    //    (ColourConvertible); the network key stays on the canonical format, so the network and its history are
    //    kept. This works in either direction (first R11G11B10 then RGBA16F, or the reverse).
    //  * A pair that cannot be converted keeps the frame's own format: the key changes and the network is rebuilt,
    //    which is the pre-Plan-A behaviour. Logged once per format.
    // MZ_TEST_NO_PLAN_A=1 disables the channel outright, for isolating the conversion from the rest of the runtime.
    VkFormat CanonicalFor(VkFormat gameVk)
    {
        static const bool disabled = [] {
            const char* v = std::getenv("MZ_TEST_NO_PLAN_A");
            return v && *v && *v != '0';
        }();
        if (disabled || gameVk == VK_FORMAT_UNDEFINED)
            return VK_FORMAT_UNDEFINED;
        if (firstColourVk == VK_FORMAT_UNDEFINED)
            firstColourVk = gameVk;
        if (gameVk == firstColourVk)
            return VK_FORMAT_UNDEFINED;
        if (!ColourConvertible(gameVk, firstColourVk))
        {
            if (loggedGameColourFormat != gameVk)
            {
                loggedGameColourFormat = gameVk;
                nr::logf("[mochizuki] plan A: colour format changed VkFormat %d -> %d but this pair has no conversion "
                         "both ways on this device; keeping the pre-Plan-A path (the network is rebuilt)",
                         int(firstColourVk), int(gameVk));
            }
            return VK_FORMAT_UNDEFINED;
        }
        if (!canonicalChannel || loggedGameColourFormat != gameVk)
        {
            loggedGameColourFormat = gameVk;
            nr::logf("[mochizuki] plan A: colour format changed VkFormat %d -> %d; converting it to the session's "
                     "first format %d (compute pass both ways; the network and its history are kept)",
                     int(firstColourVk), int(gameVk), int(firstColourVk));
        }
        canonicalChannel = true;
        return firstColourVk;
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
        bool segmentedRecording = false;   // core layout/history state crossed a new command buffer
    };

    // One frame, between the halves of the game's list, from any game queue: the network, or its colour passed
    // through (Passthrough) when anything before the network's submit fails and zeroOutputFallback is set; `why` then
    // says what failed. False for a frame passed through. Throws a lost device, and any error it cannot pass the frame
    // through for.
    bool Enqueue(ID3D12CommandQueue* producer, std::string& why)
    {
        std::lock_guard lock(submitMutex);
        const uint64_t call = ++enqueueCalls;
        if (traceOn) { traceEnq = TraceQpc(); traceSlotWaitUs = 0; tracePhases = 0; }
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
            if (!job.inlineExecution && sub.finished && job.vkFinished == sub.finished) {
                // Accepted work must never be retried as passthrough, including
                // an exception inside Submitted before its caller returns.
                sub.submitted = true;
                failed = true;
            }
            if (sub.segmentedRecording && !sub.submitted) {
                // Recording changed the core's tracked layouts/history. Do not
                // silently retry that state after a partially recorded chain.
                // Actual queued work retains its ordinary retirement proofs.
                failed = true;
                throw;
            }
            if (!zeroOutputFallback || !(colourInput && colourInput->Shared() ? colourImage.image != VkImage{} : input.buffer != VkBuffer{}) || kind == NrError::DeviceLost || sub.submitted)
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
        if (!fallbackCurrent[k] || fallbackColourGeneration[k] != (colourInput ? colourInput->generation : 0) ||
            fallbackOutput[k] != output.buffer || fallbackOutputOffset[k] != output.offset || fallbackWidth[k] != job.width || fallbackHeight[k] != job.height)
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
        if (const VkResult r = Submit(fallbackCommands[k], sub.produced, sub.finished, fallbackDone[k], producer);
            r != VK_SUCCESS)
            throw lost("vkQueueSubmit", r, S_OK);
        fallbackInFlight[k] = true;
        if (const HRESULT hr = producer->Wait(fromVk.fence, sub.finished); FAILED(hr))
            throw lost("the game queue's wait", VK_SUCCESS, hr);
        ++passthroughs;
        RequestHistoryReset(ResetPassthrough);
    }

    VkCommandBuffer PhaseBuffer(uint32_t slot, bool compute, size_t index) {
        auto& buffers = compute ? phaseCompute[slot] : phaseGraphics[slot];
        if (index >= buffers.size()) {
            VkCommandBufferAllocateInfo ai {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = compute ? computePool : pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
            VkCommandBuffer buffer {}; VkCheck(vkAllocateCommandBuffers(vk.device, &ai, &buffer), "phase allocate"); buffers.push_back(buffer);
        }
        auto result = buffers[index]; VkCheck(vkResetCommandBuffer(result, 0), "phase reset");
        VkCommandBufferBeginInfo bi {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCheck(vkBeginCommandBuffer(result, &bi), "phase begin"); return result;
    }
    void SubmitPhases(const std::vector<PhaseCommand>& phases, UINT64 produced, UINT64 finished, VkFence fence,
                      ID3D12CommandQueue* producer) {
        if (deviceLost) throw NrError(NrError::DeviceLost, "phase submit after device loss");
        // Proton direct path: the game queue drained and held for all the segments (no compute queue there).
        VkQueue graphics = vk.queue;
        mochizuki_vkd3d::QueueHold hold;
        if (vk.adoptedVkd3d)
            if (const VkResult held = vk.HoldProducer(producer, hold, graphics); held != VK_SUCCESS)
                throw NrError(NrError::DeviceLost, "Proton direct path: the game queue could not be held", held);
        if (phaseValue > UINT64_MAX - (phases.size() - 1))
            throw NrError(NrError::DeviceLost, "network phase timeline exhausted");
        const auto base = phaseValue; phaseValue += phases.size() - 1;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        gpuUsed = true;
        // Semaphore memory dependencies cover all prior writes. Concurrent
        // resources avoid exclusive ownership transfers; FIFO post->next pre
        // on graphics also serializes the shared temporal/activation state.
        for (size_t i = 0; i < phases.size(); ++i) {
            const UINT64 wait = i ? base + i : produced;
            const UINT64 signal = i + 1 == phases.size() ? finished : base + i + 1;
            VkSemaphore waiting = i ? phaseSemaphore : toVk.semaphore;
            VkSemaphore signalling = i + 1 == phases.size() ? fromVk.semaphore : phaseSemaphore;
            VkTimelineSemaphoreSubmitInfo values {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            values.waitSemaphoreValueCount = values.signalSemaphoreValueCount = 1; values.pWaitSemaphoreValues = &wait; values.pSignalSemaphoreValues = &signal;
            VkSubmitInfo si {VK_STRUCTURE_TYPE_SUBMIT_INFO, &values}; si.waitSemaphoreCount = si.signalSemaphoreCount = si.commandBufferCount = 1;
            si.pWaitSemaphores = &waiting; si.pWaitDstStageMask = &stage; si.pCommandBuffers = &phases[i].command; si.pSignalSemaphores = &signalling;
            const auto r = deviceLost ? VK_ERROR_DEVICE_LOST :
                (hooks.phaseSubmitFailAt == i + 1 ? VK_ERROR_OUT_OF_HOST_MEMORY :
                 vkQueueSubmit(phases[i].compute ? vk.computeQueue : graphics, 1, &si,
                               i + 1 == phases.size() ? fence : VK_NULL_HANDLE));
            if (r != VK_SUCCESS) {
                uncertainExecution = true;
                char message[192] {};
                std::snprintf(message, sizeof message, "inline network phase %zu/%zu submit failed (%d); resources retained",
                              i + 1, phases.size(), int(r));
                throw NrError(NrError::DeviceLost, message, r);
            }
        }
        Submitted(produced, finished);
    }
    // A graphics-only command reached in a compute-family recording: the frame
    // fails like a partial segmented recording (no passthrough retry).
    void ComputeChainRefused(bool fullCompute, const std::exception& e)
    {
        if (!fullCompute || computeChainOff) return;
        computeChainOff = "an earlier frame reached a graphics-only command";
        nr::logf("[mochizuki] inline network: %s; the whole-chain compute path is off for this session", e.what());
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
            const int64_t slotWait0 = traceOn ? TraceQpc() : 0;
            const VkResult waited = call == hooks.slotTimeoutAt
                                        ? VK_TIMEOUT
                                        : vkWaitForFences(vk.device, 1, &done[slot], VK_TRUE, kSlotWaitNs);
            if (traceOn) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); traceSlotWaitUs = uint32_t((TraceQpc() - slotWait0) * 1000000 / f.QuadPart); }
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
        if (statsNetworkId != runtime->id) {
            std::lock_guard stats(statsMutex);
            gpuMsCount = gpuMsNext = 0; gpuTick = 0;
            encodeMsLast = transferMsLast = -1.f;
            statsNetworkId = runtime->id; statsSerial = 0;
            runtime->stageTiming.DiscardSamples();
        }
        if (runtime->reno) {
            uint64_t completed = 0;
            if (vkGetSemaphoreCounterValue(vk.device, fromVk.semaphore, &completed) == VK_SUCCESS)
                runtime->stageTiming.Harvest(completed, [this](const MochizukiStageTiming::Sample& sample) {
                    PushGpuSample(sample.total, sample.network, -1.f, -1.f);
                });
        }
        struct TimingReservation {
            Session& session;
            ~TimingReservation() {
                if (session.pendingTimingNetwork)
                    session.pendingTimingNetwork->stageTiming.CancelUnsubmitted(session.pendingTimingSlot);
                session.pendingTimingNetwork = nullptr;
                session.pendingTimingSlot = MochizukiStageTiming::kUnavailable;
            }
        } timingReservation {*this};
        // Inline: the whole chain goes on the compute family when nothing in it
        // needs a graphics-only command (a format-converting blit). Otherwise the
        // conversions stay on graphics and only the network loop moves (segments).
        const bool motion = job.motion && motionImage.image;
        const char* chainWhy = nullptr;
        bool fullCompute = false;
        if (job.inlineExecution && vk.computeQueue)
        {
            if (computeChainOff) chainWhy = computeChainOff;
            else if (hooks.inlineChain == 3) chainWhy = "MZ_TEST_INLINE_CHAIN=3 (test hook)";
            else if (runtime->reno) chainWhy = "RenoDX processing uses the graphics queue";
            else if (job.firstPass && !runtime->first_pass_packed()) chainWhy = "the first-pass export needs a blit";
            else if (motion && job.motionPath != Job::Uploaded && job.motionPath != Job::Padded &&
                     !(job.motionPath == Job::Rescaled && geometry.computeMotion))

                chainWhy = "the motion vectors are rescaled by a blit";
            // mask: the host binds no explicit ControlMaskFrame (record/record_engine pass none);
            // the automatic mask is not a ControlMaskFrame.
            else chainWhy = runtime->compute_refusal(motion, motionVk, kImageUsage, job.controls.passes,
                                                     /*mask=*/false);
            fullCompute = !chainWhy;
            // Logged when the path or reason changes; a game that flips every frame
            // (first-pass export, DRS) is bounded like the error lines.
            const uint32_t pathNow = fullCompute ? 1u : 2u;
            if (infoInlinePath.load(std::memory_order_relaxed) != pathNow ||
                fullCompute != loggedFullCompute || (chainWhy && loggedChainWhy != chainWhy))
            {
                {
                    std::lock_guard lock(inlinePathMutex);
                    std::snprintf(infoInlineWhy, sizeof infoInlineWhy, "%s", chainWhy ? chainWhy : "");
                }
                infoInlinePath.store(pathNow, std::memory_order_relaxed);
            }
            if (fullCompute != loggedFullCompute || (chainWhy && loggedChainWhy != chainWhy))
            {
                loggedFullCompute = fullCompute;
                loggedChainWhy = chainWhy ? chainWhy : "";
                const uint64_t change = ++chainPathChanges;
                if (change > 8 && change % 1000 != 0) {}
                else if (fullCompute)
                    nr::logf("[mochizuki] inline network: the whole chain on compute family %u", vk.computeFamily);
                else
                    nr::logf("[mochizuki] inline network: conversions stay on family %u, network segments on compute "
                             "family %u (%s)", vk.family, vk.computeFamily, chainWhy);
            }
        }
        if (!job.inlineExecution && infoInlinePath.load(std::memory_order_relaxed))
            infoInlinePath.store(0, std::memory_order_relaxed);
        if (job.inlineExecution && vk.computeQueue && (hooks.inlineChain == 1 || hooks.inlineChain == 2) &&
            (hooks.inlineChain == 1) != fullCompute)
            throw NrError(NrError::Other, fullCompute ? "MZ_TEST_INLINE_CHAIN=2: the frame took the whole compute chain"
                                                      : "MZ_TEST_INLINE_CHAIN=1: the frame took the segments (test hook)");
        VkCommandBuffer c {};
        if (fullCompute)
            c = PhaseBuffer(slot, true, 0);
        else
        {
            c = commands[slot];
            VkCheck(vkResetCommandBuffer(c, 0), "vkResetCommandBuffer");
            VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VkCheck(vkBeginCommandBuffer(c, &bi), "vkBeginCommandBuffer");
            if (call <= 2) nr::logf("[mochizuki] old-chain frame=%llu BeginCommandBuffer.end", (unsigned long long)call);
        }

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
            region.bufferOffset += from.offset;
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
        const bool sharedColour = colourInput && colourInput->Shared();
        if (sharedColour) {
            // The core gets the same image usage/layouts from either carrier, so scaling, passes and RenoDX consume
            // it alike; a DRS subrect was captured into the top-left and is padded below like an uploaded one.
            // Plan A conversion binds the buffer carrier's image and first-pass export downloads from a buffer.
            if (job.inlineExecution || job.canonicalise || job.firstPass || !motion ||
                (!sharedExtendedRequest && (padded || runtime->reno || job.controls.passes != 1 ||
                                            job.motionPath != Job::Uploaded)))
                throw std::logic_error("shared colour recording has incompatible geometry or controls");
            colourInput->Ownership(c, true, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        } else
            upload(input, colour, colourFootprint, w, h, padded);
        if (padded)
        {
            Pad(c, colour.image, w, h, W, H);
            ready(colour.image);
        }
        // Plan A: the network is built for the session's first-seen (canonical) format. When this frame is in
        // another one (FF7 Rebirth's cutscene flip), convert it here with one compute dispatch and hand the network
        // the canonical image instead; the network and its history are kept, so the flip is invisible.
        // `net` is the image and format the core is given; it is the frame's own when no conversion is needed.
        const Image* net = &colour;
        VkFormat netVk = colourVk;
        if (job.canonicalise && canonicalColour.image && convert.set)
        {
            // `colour.image` is GENERAL with its last transfer writes visible to transfers (upload's barrier,
            // Pad's, or ready()'s), which a compute read does not follow; make them visible to a shader read
            // instead. The pass writes canonicalColour, which starts UNDEFINED: GENERAL, for a shader write.
            ImageBarrier(c, colour.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            ImageBarrier(c, canonicalColour.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_ACCESS_SHADER_WRITE_BIT);
            convert.Dispatch(c, convertPipeline, w, h);
            // The dispatch's writes visible to the copies Pad makes next (its first copy relies on this).
            ImageBarrier(c, canonicalColour.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
            if (padded)
                Pad(c, canonicalColour.image, w, h, W, H);
            ready(canonicalColour.image);
            net = &canonicalColour;
            netVk = convert.convertFor;
        }
        // The image the network reads the vectors from, and its extent: the uploaded one, or the bucket-sized one with
        // the vectors laid over the subrect (dynamic resolution).
        VkImage motionSource = motionImage.image;
        uint32_t motionW = job.motionWidth, motionH = job.motionHeight;
        const bool sharedMotion = motionInput && motionInput->Shared();
        if (sharedMotion) {
            if (!sharedColour || job.motionPath != Job::Uploaded || motionW != w || motionH != h)
                throw std::logic_error("shared motion recording has incompatible geometry or colour route");
            motionInput->Ownership(c, true, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        else if (motion && job.motionPath == Job::Uploaded)
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
            if (geometry.computeMotion)
            {
                // NrInlineComputeMotion: the same bilinear rescale as a compute pass, so no graphics-only command.
                ImageBarrier(c, motionImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                ImageBarrier(c, motionScaled.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT);
                motionRescale.DispatchMotion(c, motionPipeline, w, h, motionW, motionH);
                // Its writes visible to Pad's copies and to ready()'s barrier, which both start at transfer.
                ImageBarrier(c, motionScaled.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
            }
            else
            {
            ImageBarrier(c, motionScaled.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
            Blit(c, motionImage.image, motionScaled.image, { 0, 0 }, { int32_t(motionW), int32_t(motionH) }, { 0, 0 },
                 { int32_t(w), int32_t(h) }, VK_FILTER_LINEAR);
            }
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

        // Record the whole chain before submitting any segment. Conversion and
        // history work stay on graphics; only the network loop changes engines.
        std::vector<PhaseCommand> phases;
        struct ClearSegments {nr::Runtime* runtime; ~ClearSegments() { runtime->set_network_segments({}); runtime->set_compute_recording(false); }} clearSegments {runtime.get()};
        if (fullCompute) {
            // One compute-family buffer; the core refuses any graphics-only command in it.
            runtime->set_compute_recording(true);
            phases.push_back({c, true});
            // The core updates CPU-side state while recording (layouts, history
            // ping-pong, recording count) and refuses a graphics-only command
            // only when it reaches it. A throw from here on must fail the frame
            // like a partial segmented recording: no passthrough retry.
            sub.segmentedRecording = true;
            if (call == hooks.computeRefuseAt)
            {
                const std::logic_error refused("MZ_TEST_COMPUTE_REFUSE_AT: compute-family recording refused (test hook)");
                ComputeChainRefused(true, refused);
                throw refused;
            }
        } else if (job.inlineExecution && vk.computeQueue && !runtime->reno) {
            phases.push_back({c, false}); size_t graphics = 0, compute = 0;
            runtime->set_network_segments([&, graphics, compute](VkCommandBuffer previous, bool enter) mutable {
                sub.segmentedRecording = true;
                VkCheck(vkEndCommandBuffer(previous), "phase end");
                auto next = PhaseBuffer(slot, enter, enter ? compute++ : graphics++);
                phases.push_back({next, enter}); c = next; return next;
            });
        }
        // Once the core can advance its tracked history/layout state, a shared
        // input frame must not retry as passthrough after a recording exception.
        if (sharedColour) sub.segmentedRecording = true;
        nr::EngineFrame frame {};
        frame.colour.image = net->image;
        frame.colour.format = netVk;
        frame.colour.width = W;
        frame.colour.height = H;
        frame.colour.usage = kImageUsage;
        frame.reset = job.reset;
        // Plain fields the core reads while recording; the runtime is used under submitMutex only.
        runtime->set_history_strength(job.history);
        runtime->set_white_point(job.white);
        runtime->set_stage_timing(job.stageTiming);
        bool consumed = false;
        std::string recordedStyles;
        uint32_t recordedPasses = 0;
        const bool logExecution = GetTickCount64() - executionLogAt >= 10000;
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
        }
        if (runtime->reno)
        {
            auto& rs = job.reno;
            const uint32_t count = nr::renodx::stack_passes(rs);
            for (uint32_t k = count; k < runtime->renoPasses; ++k) runtime->release_feature(k + 1);
            runtime->renoPasses = count;
            runtime->reno->set(rs);
            nr::renodx::StageFrame sf;
            sf.colour = sf.output = frame.colour;
            sf.reset = job.reset;
            sf.preserve_normalization = !job.normalizationReset;
            sf.after_upscale = job.afterUpscale;
            sf.motion = frame.motion;
            frame.motion_scale_x *= rs.mvec_scale_x;
            frame.motion_scale_y *= rs.mvec_scale_y;
            sf.motion_scale_x = frame.motion_scale_x;
            sf.motion_scale_y = frame.motion_scale_y;
            sf.preprocess = job.controls.preprocess;
            // Use the host's validated game-exposure readback (one frame late).
            // A clear embeds this recording's value, so later CPU settings cannot
            // overwrite a queued frame's exposure. Absent data uses the content meter.
            sf.create_flags_known = job.renoExposure > 0.f;
            if (sf.create_flags_known) {
                const VkImage image = runtime->exposure.image.image;
                ImageBarrier(c, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkClearColorValue value {}; value.float32[0] = job.renoExposure;
                const VkImageSubresourceRange range {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(c, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
                ImageBarrier(c, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                sf.exposure.image = image; sf.exposure.format = VK_FORMAT_R32_SFLOAT;
                sf.exposure.width = sf.exposure.height = 1; sf.exposure.usage = kImageUsage;
            }
            uint32_t calls = 0;
            pendingTimingNetwork = runtime.get();
            pendingTimingSlot = runtime->stageTiming.Begin(c, fullCompute);
            const nr::renodx::Stage::Network network = [&](uint32_t pass, const nr::ColourFrame& proxy,
                                                          const nr::ColourFrame& neural, bool reset) {
                nr::EngineFrame e = frame;
                e.colour = proxy; e.target = neural; e.reset = reset; e.feature = pass + 1;
                nr::Controls ck = job.controls;
                ck.preprocess = {}; ck.per_pass.clear(); ck.passes = 1;
                ck.enabled = ck.apply_model = true;
                const uint32_t k = std::min(pass, 3u);
                const bool own = k && !rs.pass_follow[k - 1];
                ck.intensity = k ? rs.pass_intensity[k - 1] : rs.intensity;
                ck.style = int(own ? rs.pass_style[k - 1] : rs.style);
                ck.local_tone = own ? rs.pass_local_tone[k - 1] : rs.local_tone;
                ck.local_structure = own ? rs.pass_local_structure[k - 1] : rs.local_structure;
                ck.skin_structure = own ? rs.pass_skin_structure[k - 1] : rs.skin_structure;
                ck.automatic_mask = own ? rs.pass_auto_mask[k - 1] : rs.auto_mask;
                if (logExecution) {
                    if (recordedPasses) recordedStyles += ',';
                    recordedStyles += std::to_string(ck.style);
                    ++recordedPasses;
                }
                if (rs.skin_independent && ck.automatic_mask) {
                    ck.local_structure = std::min(ck.local_structure, 1.f);
                    if (ck.skin_structure < 0.f) ck.skin_structure = 1.f;
                }
                runtime->stageTiming.BeginNetwork(c, pendingTimingSlot);
                if (call <= 2) nr::logf("[mochizuki] old-chain record_engine.begin renodx pass=%u", pass);
                const auto result = runtime->record_engine(c, e, ck);
                if (call <= 2) nr::logf("[mochizuki] old-chain record_engine.end renodx pass=%u", pass);
                runtime->stageTiming.EndNetwork(c, pendingTimingSlot);
                calls += result.frame.network_dispatches;
                consumed |= result.history_consumed;
            };
            runtime->reno->record(c, sf, network);
            runtime->stageTiming.End(c, pendingTimingSlot);
            dispatches.store(calls, std::memory_order_relaxed);
        }
        else if (motion)
        {
            try
            {
                if (call <= 2) nr::logf("[mochizuki] old-chain record_engine.begin");
                const nr::EngineResult r = runtime->record_engine(c, frame, job.controls);
                if (call <= 2) nr::logf("[mochizuki] old-chain record_engine.end");
                dispatches.store(r.frame.network_dispatches, std::memory_order_relaxed);
                consumed = r.history_consumed;
            }
            catch (const std::logic_error& e)
            {
                ComputeChainRefused(fullCompute, e);
                throw;
            }
        }
        else
        {
            try
            {
                dispatches.store(runtime->record(c, frame.colour, job.controls, job.reset).network_dispatches,
                                 std::memory_order_relaxed);
            }
            catch (const std::logic_error& e)
            {
                ComputeChainRefused(fullCompute, e);
                throw;
            }
        }
        if (logExecution && !runtime->reno) {
            for (int p = 0; p < job.controls.passes; ++p) {
                const auto index = size_t(p ? p - 1 : 0);
                const int style = p && index < job.controls.per_pass.size() && job.controls.per_pass[index].used
                    ? job.controls.per_pass[index].style : job.controls.style;
                if (p) recordedStyles += ',';
                recordedStyles += std::to_string(style);
                ++recordedPasses;
            }
        }
        if (job.firstPass && runtime->first_pass_packed())
        {
            // NrInlineComputeExport: pass 0 packed into the frame's format on compute, then copied out.
            runtime->record_first_pack(c, w, h);
            VkBufferImageCopy copy = colourFootprint.vk;
            copy.imageExtent = { w, h, 1 };
            copy.bufferOffset += firstOutput.offset;
            vkCmdCopyImageToBuffer(c, runtime->first_pass_packed(), VK_IMAGE_LAYOUT_GENERAL, firstOutput.buffer, 1, &copy);
        }
        else if (job.firstPass)
        {
            const VkImage first = runtime->first_pass_output();
            if (!first) throw std::runtime_error("first pass output missing");
            ImageBarrier(c, first, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            ImageBarrier(c, firstColour.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            // The network ran on the padded bucket. Export the same valid w x h
            // subrect as the final output, without rescaling that subrect.
            VkImageBlit region {};
            region.srcSubresource = region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.srcOffsets[1] = region.dstOffsets[1] = { int32_t(w), int32_t(h), 1 };
            vkCmdBlitImage(c, first, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          firstColour.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
            ImageBarrier(c, firstColour.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy = colourFootprint.vk;
            copy.imageExtent = { w, h, 1 };
            copy.bufferOffset += firstOutput.offset;
            vkCmdCopyImageToBuffer(c, firstColour.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   firstOutput.buffer, 1, &copy);
            ImageBarrier(c, first, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        }
        const nr::Preprocess& prep = job.controls.preprocess;
        float prepEv = NAN;
        if (prep.active() && prep.exposure == 1)
            prepEv = runtime->preprocess_meter().first + prep.bias_ev;
        infoPrepEv.store(prepEv, std::memory_order_relaxed);
        historyBits.store((historyBits.load(std::memory_order_relaxed) << 1) | uint64_t(consumed),
                          std::memory_order_relaxed);
        historyCount.store(std::min(historyCount.load(std::memory_order_relaxed) + 1, kHistorySamples),
                           std::memory_order_relaxed);
        if (!runtime->reno) {
            if (const float ms = runtime->last_gpu_ms(); ms > 0 && statsSerial != runtime->gpu_timing_serial()) {
                statsSerial = runtime->gpu_timing_serial();
                PushGpuSample(ms, runtime->last_network_ms(), runtime->last_encode_ms(), runtime->last_transfer_ms());
            }
        }

        // Plan A, output side: the network wrote its answer into the canonical image, but the readback below
        // is not format-agnostic -- colourFootprint (and so both this copy's row pitch and the D3D12
        // CopyTextureRegion placed footprint) is built for the *game's* format. Copying a 4-byte-per-texel
        // canonical image out with an 8-byte-per-texel RGBA16F layout would read half of every row at twice
        // the stride, so the colours come out wrong and every row is offset by half its width. Convert the
        // answer back into `colour`, which is in the game's format, and read *that* out instead: nothing
        // downstream of this point has to know a conversion happened.
        if (net != &colour)
        {
            if (!convert.CanUnconvert(colourVk))
                throw std::logic_error("plan A: the answer cannot be converted back into the game's colour format");
            // The network's writes are in GENERAL with its last writes visible to all commands (ready()'s
            // barrier), which a compute read does not follow; make them visible to a shader read instead.
            // `colour` is GENERAL and, as an input conversion destination, no longer holds anything we need.
            ImageBarrier(c, net->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            ImageBarrier(c, colour.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            convert.DispatchUnconvert(c, convertPipeline, colourVk, w, h);
            // The store's writes visible to the copy below.
            ImageBarrier(c, colour.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            ++unconvertDispatches;
            net = &colour;
        }
        else
        {
            // No conversion: the network wrote into the frame's own image, so that is what is copied out.
            // (When there was one, the conversion's own barrier already left `colour` TRANSFER_SRC_OPTIMAL.)
            ImageBarrier(c, net->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }

        // A shared colour recording delivers the answer from its retained texture.
        // Buffer/canonical/DRS paths keep their existing valid-extent row-pitch copy.
        if (!sharedColour) {
            VkBufferImageCopy out = colourFootprint.vk;
            out.imageExtent = { w, h, 1 };
            out.bufferOffset += output.offset;
            vkCmdCopyImageToBuffer(c, net->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1, &out);
        }
        if (sharedColour)
            colourInput->Ownership(c, false, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        if (sharedMotion)
            motionInput->Ownership(c, false, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        if (call <= 2) nr::logf("[mochizuki] old-chain EndCommandBuffer.begin");
        VkCheck(vkEndCommandBuffer(c), "vkEndCommandBuffer");
        if (call <= 2) nr::logf("[mochizuki] old-chain EndCommandBuffer.end");

        // Plan A: report the output pass. The first conversions are named individually (they are what tells a
        // cutscene's RGBA16F frames apart from the same-format ones), then every 3600. The row pitch is the
        // one the readback used -- the game's, not the canonical format's -- which is the whole point.
        if (const uint64_t n = unconvertDispatches; n != loggedUnconvertAt && (n <= 5 || n % 3600 == 0))
        {
            loggedUnconvertAt = n;
            nr::logf("[mochizuki] plan A: output conversion pass ran%llu times (VkFormat %d -> %d, answer read back "
                     "at the game's row pitch %u bytes)",
                     static_cast<unsigned long long>(n), int(netVk), int(colourVk),
                     unsigned(colourFootprint.d3d.Footprint.RowPitch));
        }

        // The producer half is already on the game queue: after Follow, its signal releases our submit, ours releases
        // the rest.
        sub.produced = ++producedValue;
        sub.finished = ++finishedValue;
        if (call <= 2) nr::logf("[mochizuki] old-chain Follow.begin");
        Check(Follow(producer), "the new queue's wait for the last frame");
        if (call <= 2) nr::logf("[mochizuki] old-chain Follow.end");
        sub.followed = true;
        if (call <= 2) nr::logf("[mochizuki] old-chain frame=%llu producer.Signal.begin", (unsigned long long)call);
        Check(producer->Signal(toVk.fence, sub.produced), "signal after the producer");
        if (call <= 2) nr::logf("[mochizuki] old-chain frame=%llu producer.Signal.end", (unsigned long long)call);
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
            if (traceOn) tracePhases = uint32_t(phases.size());
            if (fullCompute) SubmitPhases(phases, sub.produced, sub.finished, done[slot], producer);
            else if (phases.size() > 1) {
                if (loggedPhaseCount != phases.size()) {
                    loggedPhaseCount = phases.size();
                    nr::logf("[mochizuki] inline network: compute family %u, conversion family %u, %zu segments for %d passes",
                             vk.computeFamily, vk.family, phases.size(), job.controls.passes);
                }
                SubmitPhases(phases, sub.produced, sub.finished, done[slot], producer);
            }
            else VkCheck(Submit(c, sub.produced, sub.finished, done[slot], producer), "vkQueueSubmit");
            inFlight[slot] = true;
        }
        sub.submitted = true;
        if (logExecution && call != hooks.dropSubmitAt) {
            executionLogAt = GetTickCount64();
            nr::logf("[mochizuki] submitted network #%llu model=%s int4=%d renodx=%d passes=%u styles=[%s] fence=%llu",
                static_cast<unsigned long long>(runtime->id), runtime->network() == nr::NetworkModel::Small ? "small" : "original",
                runtime->int4_mixed() ? 1 : 0, runtime->reno ? 1 : 0, recordedPasses, recordedStyles.c_str(),
                static_cast<unsigned long long>(sub.finished));
        }
        if (call <= 2) nr::logf("[mochizuki] old-chain frame=%llu producer.Wait.begin", (unsigned long long)call);
        Check(producer->Wait(fromVk.fence, sub.finished), "wait for the network");
        if (call <= 2) nr::logf("[mochizuki] old-chain frame=%llu producer.Wait.end", (unsigned long long)call);
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
        // The meter's own D3D12 objects go first: nothing can reference them (Destroy refuses live recordings, and
        // every recording that carried a measurement has been collected), and they hold the device themselves.
        exposure.Destroy();
        gameTexel.Destroy();
        StopWatchdog();
        JoinBuilder();
        StopReleaser(); // what it has not freed is in `retired`
        vram.Close();   // a kernel handle to the adapter, which no GPU work uses
        // Destroy rejects live recordings; CollectRecording only removes them
        // after their submitted D3D12 consumer tails complete. No game command
        // can still reference this session. Do not drain whole game queues here:
        // later, unrelated waits may need the same thread that is turning NR off.
        // The private Vulkan device still needs its own completion proof (and the
        // builder was joined above). On loss, retain its imported resources.
        if (gpuUsed && !deviceLost && vk.device &&
            (VulkanIdle(kDrainWaitMs) != VK_SUCCESS || !InputInitializationsDone(kDrainWaitMs)))
            Lose("the session ended with Vulkan work that did not finish");
        if (gpuUsed && deviceLost)
        {
            Rerelease();
            Quarantine(exposureSnapshot);
            Quarantine(runtime);
            Quarantine(built);
            Quarantine(allocation);
            Quarantine(colourInput);
            Quarantine(motionInput);
            Quarantine(pendingMotion);
            Quarantine(pendingColour);
            for (auto& input : inputInitializations) Quarantine(input);
            for (auto& input : retiredInputs) Quarantine(input);
            for (auto& b : buried) b.buffers.KeepOnLoss();
            for (auto& b : recordingPool) b.KeepOnLoss();
            recordingPool.clear();
            if (builtBuffers) builtBuffers->KeepOnLoss();
            if (prepared) prepared->KeepOnLoss();
            for (Retired& r : retired)
                Quarantine(r.network);
            ReleaseGameQueues();
            return;
        }
        if (vk.device && vk.adoptedVkd3d)
        {
            // The game's device: idling it would wait for the game too, unlocked. This session's last submit
            // signals fromVk's last value; that is all there is to wait for (at once, as below).
            VkSemaphoreWaitInfo wi { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
            wi.semaphoreCount = 1;
            wi.pSemaphores = &fromVk.semaphore;
            wi.pValues = &finishedValue;
            if (fromVk.semaphore && finishedValue)
                vkWaitSemaphores(vk.device, &wi, kSlotWaitNs);
        }
        else if (vk.device)
            vkDeviceWaitIdle(vk.device); // returns at once: all of this session's work has finished
        if (runtime)
            LogNetworkTime("session end");
        const char* const sessionEndReason = deviceLost ? "device_lost" : "session_end";
        if (runtime) runtime->destroyReason = sessionEndReason;
        if (built) built->destroyReason = sessionEndReason;
        runtime.reset();
        built.reset();
        exposureSnapshot.reset();
        retired.clear();
        pendingColour.reset();
        pendingMotion.reset();
        inputInitializations.clear();
        retiredInputs.clear();
        ReleaseGeometry();
        for (auto& pooled : recordingPool) pooled.Release(vk.device);
        recordingPool.clear();
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
            if (computePool) vkDestroyCommandPool(vk.device, computePool, nullptr);
            if (phaseSemaphore) vkDestroySemaphore(vk.device, phaseSemaphore, nullptr);
        }
        toVk.Release(vk.device);
        fromVk.Release(vk.device);
        convertPipeline.Release(vk.device);
        motionPipeline.Release(vk.device);
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
    if (info->flags != LMXXF_NR_CREATE_FLAG_RECORDING_LEASES &&
        info->flags != (LMXXF_NR_CREATE_FLAG_RECORDING_LEASES | LMXXF_NR_CREATE_FLAG_INLINE_QUEUE))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: current package recording ABI required");
    auto* s = new (std::nothrow) Session;
    if (!s)
        return Fail(LMXXF_NR_FAILED, "Create: out of memory");
    s->zeroOutputFallback = true;
    s->inlineQueue = (info->flags & LMXXF_NR_CREATE_FLAG_INLINE_QUEUE) != 0;
    s->assets = info->assets_directory ? info->assets_directory : DataDirectory();
    s->hooks = Hooks();
    static_cast<IUnknown*>(info->device)->QueryInterface(IID_PPV_ARGS(&s->device));
    static_cast<IUnknown*>(info->queue)->QueryInterface(IID_PPV_ARGS(&s->queue));
    if (!s->device || !s->queue)
    {
        delete s;
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: device or queue is not a D3D12 object");
    }
    // The lane's own exposure meter. Monster Hunter Wilds publishes no exposure texture and nothing calls
    // MochizukiNrSetExposure, so without this the linear encode is handed a white point of 1.0 and the frame's
    // highlights are normalised by the wrong level (lmxxf_runtime/LmxxfNrRuntime.cpp:1020-1028 measures the same
    // thing for the same reason). Built once here; a failure only means the frames keep the white point they had.
    // Built here, used only when the host enables it (MochizukiNrSetAutoExposure).
    if (s->exposure.Create(s->device))
        nr::logf("[mochizuki] auto exposure meter ready (64x64 samples, highlight-clipped mean; off until the host "
                 "enables it)");
    else
        nr::logf("[mochizuki] auto exposure unavailable: %s", s->exposure.Error().c_str());
    if (!s->gameTexel.CreateTexel(s->device))
        nr::logf("[mochizuki] game exposure readback unavailable: %s", s->gameTexel.Error().c_str());
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
// later. Missing capabilities and fixed external-import failures are remembered per adapter, without retry.
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
                         if (KindOf(e) == NrError::Interop || !std::strncmp(e.what(), "[unsupported] ", 14))
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
    if (!s || !info || !job || job->struct_size != sizeof(LmxxfNrJob))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: bad arguments");
    if (info->struct_size != sizeof(MochizukiNrFrameInfo))
        return s->RejectFrameAbiMismatch("PrepareFrame: bad arguments");
    job->handle = nullptr;
    job->private_output = nullptr;
    job->first_pass_output = nullptr;
    return Guard(
        s,
        [&]
        {
            if (!s->vk.device)
                return Fail(LMXXF_NR_NOT_IMPLEMENTED, "PrepareFrame: call PrepareSession first");
            s->FreeBuried(false);
            const bool directRecording = (info->flags & MOCHIZUKI_NR_FRAME_FLAG_DIRECT_RECORDING) != 0;
            if (directRecording != s->vk.adoptedVkd3d)
            {
                if (s->vk.adoptedVkd3d)
                    return s->RejectAdoptedLegacyPath("PrepareFrame: adopted vkd3d requires direct recording");
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: adopted vkd3d requires direct recording");
            }
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
            g.gameColourFormat = cd.Format; // Plan A: never changes on the geometry test; the buffers are built for it
            g.colourFormat = cd.Format;     // plan A may replace this with the canonical twin just below
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
            uint32_t linearMode = 0, maxPassesSetting = 0, drsMode = 0, networkModel = 1;
            bool compactTransfer = true, aco = false, prepRequested = false, int4 = false, renodx = false;
            nr::renodx::Settings reno;
            float renoExposure = 0.f;
            ID3D12Resource* exposureTexture = nullptr;
            D3D12_RESOURCE_STATES exposureState {};
            float exposureFactor = 1.f;
            {
                std::lock_guard lock(s->controlsMutex);
                model = s->controls.model;
                history = s->controls.history;
                white = s->controls.white;
                linearMode = s->controls.linearMode;
                // Priority: the game's exposure texture (read back), then a scalar game exposure
                // (MochizukiNrSetExposure, e.g. 1/pre-exposure), then the lane's meter when the host enabled it,
                // then the manual white point alone.
                if (const float t = s->textureExposure.load(std::memory_order_relaxed); t > 0.f && s->exposureTexture)
                    white = std::clamp(white / t, 1e-4f, 1e4f);
                else if (const float e = s->gameExposure.load(std::memory_order_relaxed); e > 0.f)
                    white = std::clamp(white / e, 1e-4f, 1e4f);
                else if (const float m = s->meteredExposure.load(std::memory_order_relaxed);
                         m > 0.f && s->meterEnabled.load(std::memory_order_relaxed))
                    white = std::clamp(white / m, 1e-4f, 1e4f); // the lane's own meter, MochizukiAutoExposure.h
                exposureTexture = s->exposureTexture;
                renoExposure = s->exposureTexture ? s->textureExposure.load(std::memory_order_relaxed) : 0.f;
                if (!(renoExposure > 0.f)) renoExposure = s->gameExposure.load(std::memory_order_relaxed);
                exposureState = s->exposureTextureState;
                exposureFactor = s->exposureScale / s->exposurePre;
                maxPassesSetting = s->controls.maxPasses;
                drsMode = s->controls.drsMode;
                compactTransfer = s->controls.compactTransfer;
                aco = s->controls.acoRequested && s->acoAtDevice;
                networkModel = s->controls.networkModel;
                int4 = s->controls.int4; renodx = s->controls.renodx; reno = s->controls.reno;
                prepRequested = s->prepRequested; // the one field of the session SetControls writes under this lock
            }
            // Automatic capacity grows as needed and is retained when fewer passes run.
            // An explicit prebuild count requests that exact capacity, at least the frame's passes.
            const uint32_t maxPasses = maxPassesSetting ? std::clamp(maxPassesSetting, passes, kMaxPasses) : passes;
            // The colour encoding stays the *game's* own: Plan A converts the frame to the canonical format
            // on the input side, so a linear (HDR/scene-referred) frame must still be seen as linear whatever
            // the canonical format is. Deriving this from the canonical format would treat
            // HDR as sRGB and wash the picture out.
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
                // NrInlineComputeMotion: the host sends it on inline frames only (switching modes remakes the buffers).
                g.computeMotion = (info->flags & MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_MOTION) && g.bucketed &&
                                  s->motionPipeline.Convert(mf.vk);
            }
            // Neither a network nor buffers that cannot be had now fail the session: the frame goes without NR, and
            // they are tried again as their holds say.
            s->prepWanted = s->prepWanted || model.preprocess.active();
            // Plan A: a frame whose colour format differs from the session's first-seen one is converted to that
            // format on the input side, so a cutscene's transient flip (FF7 Rebirth: R11G11B10 <-> RGBA16F) does not
            // rebuild the network. A session that only ever sees one format never canonicalises (see CanonicalFor).
            const VkFormat canonicalVk = s->CanonicalFor(cf.vk);
            g.canonicalise = canonicalVk != VK_FORMAT_UNDEFINED;
            g.canonicalVk = canonicalVk;
            // Plan A: the network's first-pass output and firstColour would be in different formats on a converted
            // frame, and RunNetwork's blit between them would cross compatibility classes (an illegal command that
            // removes the device). So once the session has switched formats at all, the first pass is no longer
            // offered -- on every frame, not just converted ones, or the key would toggle firstPass on each flip and
            // rebuild. Costs at most one rebuild at the first switch, and only when the first pass was in use (it
            // needs passes > 1; FF7 Rebirth runs one pass).
            g.firstPass = (info->flags & LMXXF_NR_FRAME_FLAG_FIRST_PASS) && passes > 1 && !s->canonicalChannel &&
                model.apply_model && !model.preprocess.active() && s->Blittable(cf.vk);
            // Plan A: on a converted frame the geometry's colourFormat is the canonical twin (for the key and logs);
            // the buffers stay built for gameColourFormat.
            if (g.canonicalise && CanonicalDxgi(canonicalVk) != DXGI_FORMAT_UNKNOWN)
                g.colourFormat = CanonicalDxgi(canonicalVk);
            // Plan A: the key's format is the canonical one on a converted frame; otherwise the game's, which is
            // then the first-seen format itself (or, for an unconvertible pair, the new one: a rebuild, as before).
            const VkFormat keyFormat = g.canonicalise ? canonicalVk : cf.vk;
            // NrInlineComputeConvert: only a listed-format key carries it (other formats never rebuild for it). The host
            // sends it on inline frames only, so split keeps its key; switching modes rebuilds once.
            const bool convertListed = keyFormat == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
                keyFormat == VK_FORMAT_R8G8B8A8_UNORM || keyFormat == VK_FORMAT_R8G8B8A8_SRGB ||
                keyFormat == VK_FORMAT_B8G8R8A8_UNORM || keyFormat == VK_FORMAT_B8G8R8A8_SRGB ||
                keyFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || keyFormat == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            NetworkKey key { g.width, g.height, keyFormat, scale, maxPasses, linear, s->prepWanted || prepRequested,
                                    compactTransfer, g.firstPass, aco };
            key.compilePolicy = s->compilePolicy;
            key.networkModel = networkModel;
            key.int4 = int4 && networkModel == 0; // INT4 records/weights describe the original model only.
            key.renodx = renodx;
            if (renodx) {
                nr::renodx::working_resolution(reno, g.width, g.height, motionW, motionH, &key.renoWidth, &key.renoHeight);
                key.sourceEncoding = reno.source_encoding; key.linearNits = reno.linear_unit_nits;
                key.maxPasses = nr::renodx::stack_passes(reno);
                key.scale = 1.f; key.prep = false; key.firstPass = g.firstPass = false;
            }
            key.computeConvert = (info->flags & MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_CONVERT) && convertListed;
            key.computeDownscale = (info->flags & MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_DOWNSCALE) && scale < 1.f;
            key.computeExport = (info->flags & MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_EXPORT) && g.firstPass;
            // NrInlineComputeMotion with more than one layer: every layer samples the motion
            // vectors in place, so multi-pass needs no motion blit (inline frames only).
            key.motionPasses = (info->flags & MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_MOTION) && maxPasses > 1;
            // What the network is about to be shown, on the first frame and then ten seconds apart: the one line that
            // says whether the model input is normalised at all. `white` is the divisor the linear encode takes out of
            // the frame; `linear` whether that encode runs at all (a frame whose colour is already display-encoded
            // must not get one); `exposure` what MochizukiNrSetExposure last supplied, 0 when the game never did, and
            // `meter` what the lane measured itself in that case. The
            // rest is what the host asked for, so one session's log shows both ends of a settings change.
            if (GetTickCount64() - s->inputLogAt >= 10000)
            {
                s->inputLogAt = GetTickCount64();
                const char* const modelName = networkModel == 1 ? "small" : (int4 ? "original(int4 requested)" : "original");
                int effStyle[3] = { model.style, model.style, model.style };
                if (renodx)
                {
                    effStyle[0] = int(reno.style);
                    effStyle[1] = int(reno.pass_follow[0] ? reno.style : reno.pass_style[0]);
                    effStyle[2] = int(reno.pass_follow[1] ? reno.style : reno.pass_style[1]);
                }
                nr::logf("[mochizuki] input: %ux%u frame %ux%u colour VkFormat %d key VkFormat %d DXGI %u linearMode=%u linear=%d "
                         "white=%.4f exposure=%.4f texture=%.4f meter=%.4f(%s) prep(want=%d latch=%d active=%d mode=%d curve=%d bias=%.2f) "
                         "requested.model=%s aco=%d compact=%d passes=%u capacity=%u scale=%.2f history=%.3f "
                         "controls(intensity=%.3f model.style=%d tone=%.2f structure=%.2f skin=%.2f mask=%d detail=%.3f colour=%.3f) "
                         "renodx=%d",
                         g.width, g.height, w, h, int(cf.vk), int(keyFormat), unsigned(cd.Format), linearMode, linear ? 1 : 0, white,
                         s->gameExposure.load(std::memory_order_relaxed),
                         s->textureExposure.load(std::memory_order_relaxed),
                         s->meteredExposure.load(std::memory_order_relaxed),
                         s->meterEnabled.load(std::memory_order_relaxed) ? "on" : "off", s->prepWanted ? 1 : 0,
                         prepRequested ? 1 : 0, model.preprocess.active() ? 1 : 0, model.preprocess.exposure,
                         model.preprocess.curve, model.preprocess.bias_ev,
                         modelName, aco ? 1 : 0, compactTransfer ? 1 : 0,
                         passes, maxPasses, scale, history, model.intensity, model.style, model.local_tone,
                         model.local_structure, model.skin_structure, model.automatic_mask ? 1 : 0, detail,
                         colourStrength, renodx ? 1 : 0);
                if (renodx)
                    nr::logf("[mochizuki] renodx style: requested=%d requested.passes=[%d,%d,%d] feed_mode=%u",
                             int(reno.style), effStyle[0], effStyle[1], effStyle[2], reno.feed_mode);
            }
            std::string why;
            s->infoRequestedPasses = passes;
            const bool extentShrinking = s->runtime && (key.width < s->net.width || key.height < s->net.height);
            const bool settleExtent = !g.bucketed || extentShrinking;
            if (!s->EnsureNetwork(key, g, colour, why, maxPassesSetting == 0, settleExtent))
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
                // Plan A: the input side (footprint, colour image, upload) is built for the game's own format,
                // which is what changed here; the network and its history are kept, so a flip costs one set of
                // buffers rather than a rebuild.
                if (g.canonicalise)
                    nr::logf("[mochizuki] plan A: input adapter remade for the game's colour DXGI %u (the network "
                             "keeps the canonical VkFormat %d)",
                             unsigned(g.gameColourFormat), int(g.canonicalVk));
            }
            // A new valid extent restarts the history: it holds the frame's content where the old one had it.
            if (w != s->validWidth || h != s->validHeight)
            {
                s->validWidth = w;
                s->validHeight = h;
                s->infoValidWidth = w;
                s->infoValidHeight = h;
                s->RequestHistoryReset(Session::ResetExtent);
            }

            // While a build for new settings runs, the frame keeps the network it ran on: no more passes than that one
            // was built for.
            const uint32_t served = std::min(passes, s->net.maxPasses);
            s->LogPassCapacity(served);

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
                s->RequestHistoryReset(Session::ResetPasses);
            }

            Job next;
            next.colour = colour;
            next.normalizationReset = info->reset != 0;
            next.colourState = static_cast<D3D12_RESOURCE_STATES>(info->color_state);
            next.directRecording = directRecording;
            next.depth = static_cast<ID3D12Resource*>(info->depth);
            next.depthState = static_cast<D3D12_RESOURCE_STATES>(info->depth_state);
            next.depthWidth = info->depth_width;
            next.depthHeight = info->depth_height;
            next.depthInverted = info->depth_inverted != 0;
            next.canonicalise = g.canonicalise;
            next.width = w;
            next.height = h;
            next.controls = std::move(model);
            next.controls.detail_strength = detail;
            next.controls.colour_strength = colourStrength;
            next.controls.passes = int(served);
            next.history = history;
            next.white = white;
            next.linear = linear;
            next.meter = s->meterEnabled.load(std::memory_order_relaxed);
            next.reno = reno;
            next.afterUpscale = (info->flags & MOCHIZUKI_NR_FRAME_FLAG_AFTER_UPSCALE) != 0;
            next.renoExposure = renoExposure;
            next.exposureTexture = exposureTexture;
            next.exposureFactor = exposureFactor;
            next.exposureState = exposureState;
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
                s->RequestHistoryReset(Session::ResetNoMotion); // consumed at execution
            next.inlineExecution = (info->flags & MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION) != 0;
            next.stageTiming = (info->flags & MOCHIZUKI_NR_FRAME_FLAG_STAGE_TIMING) != 0;
            next.firstPass = g.firstPass && s->net.firstPass && served > 1 && s->firstResult;
            job->first_pass_output = next.firstPass ? s->firstResult : nullptr;
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
                     if (s->colourInput && s->colourInput->Shared())
                         s->colourInput->Capture(c, j.colour, j.colourState, j.width, j.height);
                     else
                         CopyToBuffer(c, j.colour, j.colourState, s->input.resource, s->colourFootprint, j.width, j.height);
                     if (j.motion) {
                         if (s->motionInput && s->motionInput->Shared())
                             s->motionInput->Capture(c, j.motion, j.motionState);
                         else
                             CopyToBuffer(c, j.motion, j.motionState, s->motionBuffer.resource, s->motionFootprint,
                                          j.motionWidth, j.motionHeight);
                     }
                     // Exposure (MochizukiAutoExposure.h). Both probes ride this list, which carries the tail
                     // EndRecordingExecution is handed at the end of the frame, so a value is picked up one frame
                     // later once that tail has passed - nothing here waits on the GPU. Everything here runs under
                     // jobMutex (the lock above), so the probes and meterIdle need no lock of their own.
                     float value = 0.f;
                     if (s->gameTexel.Poll(&value))
                     {
                         const float e = value;
                         s->textureExposure.store(std::isfinite(e) && e > 0.f ? e : 0.f, std::memory_order_relaxed);
                     }
                     if (s->exposure.Poll(&value))
                         s->meteredExposure.store(value, std::memory_order_relaxed);
                     if (j.exposureTexture)
                     {
                         // The game's own level wins; the meter would be ignored work.
                         s->gameTexel.RecordTexel(c, j.exposureTexture, j.exposureState, j.texelTicket, j.exposureFactor);
                         s->meterIdle = 0;
                         s->meteredExposure.store(0.f, std::memory_order_relaxed);
                     }
                     else if (s->gameExposure.load(std::memory_order_relaxed) > 0.f || !j.meter || !j.linear)
                     {
                         // A scalar game exposure, the meter switched off, or a display-encoded frame (white is not
                         // applied at all): no measurement. Drop the lane's own value so a level measured before
                         // cannot come back as if it were this frame's.
                         s->meterIdle = 0;
                         s->meteredExposure.store(0.f, std::memory_order_relaxed);
                     }
                     // The meter can pause (a colour image it cannot read, no free slot, a recording that has not
                     // published a newer tail yet). After a second without a measurement the manual white point is
                     // back in charge; it resumes by itself as soon as a measurement lands again.
                     else if (s->exposure.Record(c, j.colour, j.colourState, j.width, j.height, j.meterTicket))
                         s->meterIdle = 0;
                     else if (++s->meterIdle == Session::kMeterIdleFrames)
                     {
                         s->meteredExposure.store(0.f, std::memory_order_relaxed);
                         nr::logf("[mochizuki] exposure meter paused: white point back to the manual setting until "
                                  "it measures again");
                     }
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
            // BoundRecording restores the carrier captured by this closed recipe,
            // not the session's latest route. Begin waits for the prior consumer;
            // Enqueue publishes the Vulkan completion wait before this list runs.
            const bool sharedOutput = s->colourInput && s->colourInput->Shared();
            ID3D12Resource* outputSource = sharedOutput ? s->colourInput->texture : s->output.resource;
            Transition(c, outputSource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(c, s->result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst { s->result, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
            D3D12_TEXTURE_COPY_LOCATION src { outputSource, sharedOutput ? D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX :
                D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
            if (!sharedOutput) src.PlacedFootprint = s->colourFootprint.d3d;
            // The frame's valid extent into result's top-left: all of the buffer, except with dynamic resolution.
            const D3D12_BOX box { 0, 0, 0, j.width, j.height, 1 };
            const bool whole = j.width == s->colourFootprint.d3d.Footprint.Width &&
                               j.height == s->colourFootprint.d3d.Footprint.Height;
            c->CopyTextureRegion(&dst, 0, 0, 0, &src, whole ? nullptr : &box);
            Transition(c, s->result, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(c, outputSource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (j.firstPass)
            {
                Transition(c, s->firstOutput.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(c, s->firstResult, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                dst.pResource = s->firstResult;
                src.pResource = s->firstOutput.resource;
                c->CopyTextureRegion(&dst, 0, 0, 0, &src, whole ? nullptr : &box);
                Transition(c, s->firstResult, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Transition(c, s->firstOutput.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
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
        s->RequestHistoryReset(Session::ResetCancel);
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
        s->RequestHistoryReset(Session::ResetExternal);
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
        char deferred[192] {};
        { std::lock_guard lock(s->errorMutex); std::memcpy(deferred, s->networkDeferred, sizeof deferred); }
        if (deferred[0])
            std::snprintf(text, sizeof text, "mochizuki %ux%u: requested %u passes, using %u (capacity %u); %s",
                          s->net.width, s->net.height, s->infoRequestedPasses.load(), passes, s->net.maxPasses, deferred);
        else
            // Fixed-width numbers: this line is the menu's live status, and a value that
            // changes digit count every refresh reflows the auto-sized menu layout.
            std::snprintf(text, sizeof text, "mochizuki %ux%u%s%s, network %6.2f ms (p95 %6.2f), %8llu frames", s->net.width,
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

// MochizukiNrControls.h. Every Controls field is 4 bytes and 4-aligned; complete-package callers must match its
// exact size, ending at network_model. Reject both shorter and longer control blocks.
static_assert(alignof(MochizukiNrControls) == 4 &&
              sizeof(MochizukiNrControls) == offsetof(MochizukiNrControls, network_model) +
                                                 sizeof(MochizukiNrControls::network_model) &&
              offsetof(MochizukiNrControls, pass) == 72 && offsetof(MochizukiNrControls, preprocess) == 136 &&
              sizeof(MochizukiNrPassControls) == 32);
// Info has no implicit padding (reserved0 fills the gap before frames), so its offsets do not depend on packing; its
// sizeof does (tail padding after network_dispatches), which is why a filler reports the end of its last field.
static_assert(offsetof(MochizukiNrInfo, reserved0) == 44 && offsetof(MochizukiNrInfo, frames) == 48 &&
              offsetof(MochizukiNrInfo, last_error) == 64 && offsetof(MochizukiNrInfo, network_dispatches) == 320 &&
              offsetof(MochizukiNrInfo, preprocess_ev) == 324 && offsetof(MochizukiNrInfo, white_point) == 328 &&
              offsetof(MochizukiNrInfo, aco_requested) ==
                  offsetof(MochizukiNrInfo, gpu_ms_other_mean) + sizeof(MochizukiNrInfo::gpu_ms_other_mean));

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
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "SetControls: package ABI mismatch or bad arguments; update the complete package");
    try
    {
        const MochizukiNrControls c = *in; // Exact current-package ABI only.
        SessionControls sanitised = Sanitise(c);
        std::lock_guard lock(s->controlsMutex);
        // RuntimeConfig::preprocess is part of the network's identity, so the setting has to be known before a network
        // is built: wait for a frame to request it and the request can never be met, because the network that would
        // run it was built without it. Latched, because a network built able to preprocess serves frames that do not
        // ask for it; the core's own controls preprocess=false leaves such a frame exactly as it was.
        if (sanitised.model.preprocess.active())
            s->prepRequested = true;
        s->controls = std::move(sanitised);
        // The device's pipeline-binary extension is fixed. Turning ACO on without it waits for a new device.
        // Turning it off, or back on, rebuilds the next network; the one already installed keeps its pipelines.
        if (s->deviceStarted && mz::compile::Mandatory(s->compilePolicy))
            std::snprintf(s->acoFallback, sizeof s->acoFallback, "RADV driver-compiled Linux SPIR-V (no PAL import)");
        else if (s->deviceStarted && s->controls.acoRequested && !s->acoAtDevice)
            std::snprintf(s->acoFallback, sizeof s->acoFallback, "ACO mode applies on the next game start");
        else if (s->deviceStarted && !s->controls.acoRequested && !s->acoAtDevice)
            s->acoFallback[0] = 0;
        else if (s->deviceStarted && s->controls.acoRequested && s->acoEffective)
            s->acoFallback[0] = 0;
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
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetInfo: package ABI mismatch or bad arguments; update the complete package");
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
        {
            std::lock_guard stats(s->statsMutex);
            info.gpu_samples = s->gpuSamples;
            info.gpu_ms_encode_last = s->gpuMsCount ? s->encodeMsLast : -1.f;
            info.gpu_ms_transfer_last = s->gpuMsCount ? s->transferMsLast : -1.f;
            info.gpu_tick = s->gpuTick;
            if (s->gpuMsCount) {
                const uint32_t last = (s->gpuMsNext + kGpuSamples - 1) % kGpuSamples;
                info.gpu_ms_last = s->gpuMs[last];
                info.gpu_ms_network_last = s->networkMs[last];
                info.gpu_ms_other_last = s->otherMs[last];
                for (uint32_t i = 0; i < s->gpuMsCount; ++i) {
                    info.gpu_ms_mean += s->gpuMs[i];
                    info.gpu_ms_network_mean += s->networkMs[i];
                    info.gpu_ms_other_mean += s->otherMs[i];
                    info.gpu_ms_max = std::max(info.gpu_ms_max, s->gpuMs[i]);
                }
                info.gpu_ms_mean /= s->gpuMsCount;
                info.gpu_ms_network_mean /= s->gpuMsCount;
                info.gpu_ms_other_mean /= s->gpuMsCount;
                float totals[kGpuSamples], networks[kGpuSamples];
                std::copy_n(s->gpuMs, s->gpuMsCount, totals);
                std::copy_n(s->networkMs, s->gpuMsCount, networks);
                std::sort(totals, totals + s->gpuMsCount);
                std::sort(networks, networks + s->gpuMsCount);
                const uint32_t n = s->gpuMsCount;
                info.gpu_ms_median = n % 2 ? totals[n / 2] : (totals[n / 2 - 1] + totals[n / 2]) / 2;
                info.gpu_ms_p95 = totals[(n * 95 + 99) / 100 - 1];
                info.gpu_ms_network_median = n % 2 ? networks[n / 2] : (networks[n / 2 - 1] + networks[n / 2]) / 2;
                info.gpu_ms_network_p95 = networks[(n * 95 + 99) / 100 - 1];
            }
        }
        info.build_seconds = s->infoBuildSeconds;
        info.frames = s->frames;
        const uint32_t n = s->historyCount;
        const uint64_t window = n < kHistorySamples ? (uint64_t(1) << n) - 1 : ~uint64_t(0);
        info.history_consumed_pct = n ? uint32_t(std::popcount(s->historyBits & window)) * 100 / n : 0;
        info.failed = s->failed;
        info.network_dispatches = s->dispatches;
        info.int4_effective = s->infoInt4;
        info.network_effective = s->infoNetwork;
        info.renodx_effective = s->infoReno;
        info.preprocess_ev = s->infoPrepEv;
        info.white_point = s->infoWhite;
        {
            std::lock_guard lock(s->controlsMutex);
            info.aco_requested = s->controls.acoRequested ? 1u : 0u;
            info.int4_requested = s->controls.int4;
            info.network_requested = s->controls.networkModel;
            info.aco_effective = s->acoEffective ? 1u : 0u;
            std::memcpy(info.aco_fallback, s->acoFallback, sizeof info.aco_fallback);
        }
        info.inline_path = s->infoInlinePath.load(std::memory_order_relaxed);
        {
            std::lock_guard lock(s->inlinePathMutex);
            std::memcpy(info.inline_path_why, s->infoInlineWhy, sizeof info.inline_path_why);
        }
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

extern "C" __declspec(dllexport) void MochizukiNrSetVulkanDeviceQuery(mz::device::Query query)
{
    g_queryCreatedDevice.store(query, std::memory_order_release);
}

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

// The host's switch for the lane's own meter (INI MochizukiWhitePointSource 2/3). Off unless called with nonzero.
extern "C" __declspec(dllexport) int32_t MochizukiNrSetAutoExposure(void* context, uint32_t enabled)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "SetAutoExposure: no session");
    const bool on = enabled != 0;
    if (s->meterEnabled.exchange(on, std::memory_order_relaxed) != on)
        nr::logf("[mochizuki] auto exposure meter %s", on ? "on" : "off");
    if (!on)
        s->meteredExposure.store(0.f, std::memory_order_relaxed);
    return LMXXF_NR_OK;
}

// The game's 1x1 exposure texture for the frames that follow (null: none), with the upscaler's exposure scale and
// pre-exposure. Its texel is read back (one frame late) and white = white_point / (texel * scale / pre). A texture
// that is not a 1x1 R16/R32 float is refused here (the frame goes on without it), as in the lmxxf runtime.
extern "C" __declspec(dllexport) int32_t MochizukiNrSetExposureTexture(void* context, void* texture, uint32_t state,
                                                                      float pre_exposure, float exposure_scale)
{
    auto* s = static_cast<Session*>(context);
    if (!s)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "SetExposureTexture: no session");
    auto* t = static_cast<ID3D12Resource*>(texture);
    if (t && !MochizukiAutoExposure::Probe::UsableExposureTexture(t))
        t = nullptr;
    const float pre = std::isfinite(pre_exposure) && pre_exposure > 0.f && pre_exposure < 1e6f ? pre_exposure : 1.f;
    const float scale =
        std::isfinite(exposure_scale) && exposure_scale > 0.f && exposure_scale < 1e6f ? exposure_scale : 1.f;
    bool changed = false;
    {
        std::lock_guard lock(s->controlsMutex);
        changed = (s->exposureTexture != nullptr) != (t != nullptr);
        s->exposureTexture = t;
        s->exposureTextureState = static_cast<D3D12_RESOURCE_STATES>(state);
        s->exposurePre = pre;
        s->exposureScale = scale;
    }
    if (!t)
        s->textureExposure.store(0.f, std::memory_order_relaxed);
    if (changed)
        nr::logf(t ? "[mochizuki] white point from the game's exposure texture (scale %.4f, pre-exposure %.4f)"
                   : "[mochizuki] the game's exposure texture is gone (scale %.4f, pre-exposure %.4f ignored)",
                 double(scale), double(pre));
    return LMXXF_NR_OK;
}

// ANY_QUEUE: a frame may come from any game queue (Session::Follow), so the host keeps the session on a queue change.
extern "C" __declspec(dllexport) uint32_t MochizukiNrGetFeatures(void) { return MOCHIZUKI_NR_FEATURE_ANY_QUEUE; }

extern "C" __declspec(dllexport) int32_t MochizukiNrPrepareFrame(
    void* context, const MochizukiNrFrameInfo* info, LmxxfNrJob* job)
{ return PrepareFrame(context, info, job); }

extern "C" __declspec(dllexport) int32_t MochizukiNrGetDirectCommandBuffer(
    void* context, void* nativeList, uint64_t* commandBuffer)
{ return GetDirectCommandBuffer(context, nativeList, commandBuffer); }

extern "C" __declspec(dllexport) int32_t MochizukiNrRecordDirect(
    void* context, void* job, void* nativeList, uint64_t commandBuffer)
{ return RecordDirect(context, job, nativeList, commandBuffer); }

extern "C" __declspec(dllexport) void MochizukiNrSetLogging(uint32_t enabled)
{ g_logging.store(enabled != 0, std::memory_order_relaxed); }

extern "C" __declspec(dllexport) int32_t MochizukiNrGetBuildProgress(void* context, MochizukiNrBuildProgress* out)
{
    if (!context || !out || out->struct_size != sizeof(*out)) return LMXXF_NR_INVALID_ARGUMENT;
    auto* s = static_cast<Session*>(context);
    std::lock_guard lock(s->progressMutex);
    *out = s->progress;
    if (out->active)
    {
        const int stage = nr::g_build_stage.load(std::memory_order_relaxed);
        const char* name = nullptr;
        switch (stage)
        {
        case 1: name = "Planning"; break;
        case 2: name = "Reading weights"; break;
        case 3: name = "Uploading weights"; break;
        case 4: name = "Creating pipelines"; break;
        case 5: name = "Finishing"; break;
        default: break;
        }
        if (name)
        {
            std::snprintf(out->stage, sizeof out->stage, "%s", name);
            if (stage == 4)
            {
                out->completed = nr::g_build_pipes_done.load(std::memory_order_relaxed);
                out->total = nr::g_build_pipes_total.load(std::memory_order_relaxed);
            }
        }
    }
    const auto now = GetTickCount64();
    if (out->active && now - s->progressLogTick >= 10000) {
        s->progressLogTick = now;
        nr::logf("[mochizuki] build %.1f s: %s (%u/%u), last progress %.1f s ago",
            double(now - out->start_tick) / 1000, out->stage, out->completed, out->total,
            double(now - out->update_tick) / 1000);
    }
    return LMXXF_NR_OK;
}
