// FSR-RR offline chain: real production converter -> real AMD FSR Ray Regeneration 1.2
// -> composition -> real FSR upscaler, on real hardware, outside any game process.
//
// What this binary does NOT do: substitute Python shader math, mock dispatches, or
// accept an API return code as image-correctness evidence. Every reported stage image
// is copied out of the GPU through a fence-completed readback and every metric is
// computed from those bytes.
//
// Sources this runner borrows from (attribution-pinned):
//  - burak113/OptiScaler 5a6adb10601fd16d62504febba850d8839e8f5fe,
//    OptiScaler/shaders/shader_tools/tests/fsrd_gpu_runner.cpp and fsrd_rr_runner.cpp:
//    footprint-exact upload rows, staged upload/readback barrier pattern, bounded event
//    fence waits + device-removal checks, D3D12 info-queue accounting, direct effect-DLL
//    loading with ffxLoadFunctions, dispatch-controls recording.
//  - This repository's production FSRDFeature_Dx12.cpp (RR evaluate contract: sorted
//    typed-signal chain, conversion/composition descriptors, jitter/motion contracts,
//    near/far derivation) and FSR31Feature_Dx12.cpp (upscaler context/dispatch shape,
//    FLT_MAX + DEPTH_INFINITE far-plane contract).
// The converter TU compiled next to this file IS the production
// shaders/fsrd_preprocess/FSRDPreprocessor_Dx12.cpp (worktree closure, or the frozen
// 2c2f619b closure extracted by run.cmd --baseline-source).
//
// Modes:
//   probe  : enumerate adapters/DLLs, write a manifest template, exit 1 (review needed)
//   serial : correctness. Per-frame fence wait, full readback, metrics, images. Default.
//   perf   : timing. Bounded in-flight ring, diagnostics off, per-stage GPU timestamps,
//            readback only on the final frame (content sanity only).
//   native-diffuse : paired finite flat/detail Lambertian DirectDiffuse diagnostics;
//     static camera, warmup_frames then measured_frames, independent RR/SR histories.
// SR inputs: motion subrects are GPU-copied to their exact zero-origin render/display
// extent. Frozen linear-depth scenes retain signed view-Z for RR and independently
// project the same jittered renderer world samples to hardware depth for SR.
// Build: tests\fsr-rr\run.cmd gpu <build-dir> --build-only
// Run: <build-dir>\offline_chain.exe --mode native-diffuse --manifest <manifest> --out <out>
//
// Exit codes: 0 PASS, 1 FAIL (any prerequisite, dispatch, validation, metric or
// lifetime-contract failure), 3 game collision (another fullscreen app appeared).
// There is deliberately no SKIP path: a missing prerequisite is a FAIL, never a PASS.

#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <bcrypt.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <json.hpp>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "d3dcompiler.lib")

// FidelityFX SDK v2 headers (the closure the signed effect DLLs in
// external/FidelityFX-SDK-v2/Kits/FidelityFX/signedbin are built against).
// The official ffx_api_loader.h gates its whole body on _GAMING_XBOX/_WINDOWS/
// PLATFORM_WINDOWS and emits "#pragma error" otherwise, which compiles
// ffxLoadFunctions into nothing (all function pointers stay null). The upstream
// runner (burak113 fsrd_rr_runner.cpp) defines _WINDOWS for the same reason.
#ifndef _WINDOWS
#define _WINDOWS
#endif
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>

// Product ABI for RR 1.2 (local RR additions on top of the shared API types).
#include <fsr-rr/ffx_denoiser.h>
#include "upscalers/opennr_rr/opennr_rr_api.h"

// Production converter (run.cmd compiles FSRDPreprocessor_Dx12.cpp from this closure,
// or from the frozen baseline archive first on the include path).
#include "shaders/fsrd_preprocess/FSRDPreprocessor_Dx12.h"

// Real lifetime infrastructure the product uses around this chain.
#include "misc/DeferredRelease.h"
#include <dlssnr/submission/GpuBatchTracker.h>
#include <dlssnr/submission/CommandListProxy.h>

using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;
using DirectX::XMFLOAT4X4;
using DirectX::XMMATRIX;
using DirectX::XMUINT4;
using namespace DirectX;

namespace fs = std::filesystem;

using FSRDConversionDesc = FSRDPreprocessor_Dx12::ConversionDesc;
using FSRDCompositionDesc = FSRDPreprocessor_Dx12::CompositionDesc;
using FSRDConvFlags = FSRDPreprocessor_Dx12::ConvFlags;
using FSRDCompFlags = FSRDPreprocessor_Dx12::CompFlags;
#include "offline_features.h"

// ---------------------------------------------------------------------------
// Compile-time adaptation across converter API generations.
//
// The same harness TU must build against BOTH the frozen 2c2f619b closure (baseline
// archive) and the current production closure, whose ConversionDesc/CompositionDesc
// field sets differ (fields removed and added, optional member functions added).
// Member-existence traits gate every generation-specific access; no product-side macro
// or edit is required. Only the four traits actually used exist.
// ---------------------------------------------------------------------------
template <typename T, typename = void>
struct HasFloorIsolation : std::false_type
{
}; // frozen closure only
template <typename T>
struct HasFloorIsolation<T, std::void_t<decltype(std::declval<T&>().FloorIsolation)>> : std::true_type
{
};
template <typename T>
inline constexpr bool HasFloorIsolation_v = HasFloorIsolation<T>::value;

template <typename T, typename = void>
struct HasInRawColor : std::false_type
{
}; // frozen closure only
template <typename T>
struct HasInRawColor<T, std::void_t<decltype(std::declval<T&>().InRawColor)>> : std::true_type
{
};
template <typename T>
inline constexpr bool HasInRawColor_v = HasInRawColor<T>::value;

template <typename T, typename = void>
struct HasFinishHistory : std::false_type
{
}; // new closure only
template <typename T>
struct HasFinishHistory<T, std::void_t<decltype(std::declval<T&>().FinishCompositionHistory(true))>>
    : std::true_type
{
};
template <typename T>
inline constexpr bool HasFinishHistory_v = HasFinishHistory<T>::value;

template <typename T, typename = void>
struct HasInvalidateHistory : std::false_type
{
}; // new closure only
template <typename T>
struct HasInvalidateHistory<T, std::void_t<decltype(std::declval<T&>().InvalidateCompositionHistory())>>
    : std::true_type
{
};
template <typename T>
inline constexpr bool HasInvalidateHistory_v = HasInvalidateHistory<T>::value;

// Generation-gated accessors. A discarded if-constexpr branch inside a NON-template
// function is still fully name-checked, so direct member access would fail against
// whichever closure lacks the member; these dependent template bodies are only
// instantiated for the real converter type.
template <typename C>
static void SetFrameIndexIfPresent(C& conv, uint64_t frame)
{
    if constexpr (HasFloorIsolation_v<C>)
        conv.FrameIndex = frame; // frozen closure only
}
template <typename C>
static void SetBaselineCompositionFieldsIfPresent(C& comp, uint32_t baseX, uint32_t baseY,
                                                  ID3D12Resource* rawColor)
{
    if constexpr (HasInRawColor_v<C>)
    {
        comp.SourceBase = DirectX::XMUINT4{baseX, baseY, 0, 0};
        comp.CorrelationBias = 1.0f; // canonical frozen Config default, not aggregate zero
        comp.InRawColor = rawColor;
        comp.FloorHandoverAnchorClamp = 2.0f;
        comp.FloorHandoverCorrelationMix = 1.0f;
    }
}
template <typename C>
static void FinishCompositionHistoryIfPresent(C& converter, bool success)
{
    if constexpr (HasFinishHistory_v<C>)
        converter.FinishCompositionHistory(success); // new closure only
}
template <typename C>
static void InvalidateCompositionHistoryIfPresent(C& converter)
{
    if constexpr (HasInvalidateHistory_v<C>)
        converter.InvalidateCompositionHistory(); // new closure only
}

template <typename C, typename = void>
struct HasUpscalerDepth : std::false_type
{
}; // new closure only (PrepareUpscalerDepthInput/GetUpscalerDepthInput)
template <typename C>
struct HasUpscalerDepth<C, std::void_t<decltype(std::declval<C&>().GetUpscalerDepthInput())>>
    : std::true_type
{
};
template <typename C>
inline constexpr bool HasUpscalerDepth_v = HasUpscalerDepth<C>::value;

// Production SR depth path (new closure): canonical depth encoded to zero-origin
// hardware depth on the same command list as the current conversion. farPlane is
// physical units, or FLT_MAX with infiniteFar - never 0 (converter owner contract).
template <typename C>
static ID3D12Resource* PrepareUpscalerDepthIfPresent(C& converter, ID3D12GraphicsCommandList* cl,
                                                     float nearPlane, float farPlane, bool infiniteFar,
                                                     bool invertedOutput)
{
    if constexpr (HasUpscalerDepth_v<C>)
    {
        typename C::UpscalerDepthEncode encode = {};
        encode.nearPlane = nearPlane;
        encode.farPlane = farPlane;
        encode.infiniteFar = infiniteFar;
        encode.invertedOutput = invertedOutput;
        return converter.PrepareUpscalerDepthInput(cl, encode);
    }
    else
    {
        return nullptr; // frozen closure: authored hardware depth or independent renderer projection
    }
}

// ---------------------------------------------------------------------------
// Failure plumbing
// ---------------------------------------------------------------------------
struct Fatal : std::runtime_error
{
    explicit Fatal(const std::string& what) : std::runtime_error(what) {}
};
[[noreturn]] static void Die(const std::string& what)
{
    throw Fatal(what);
}
#define CHECK_HR(hr, what)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        const HRESULT _hr = (hr);                                                                                      \
        if (FAILED(_hr))                                                                                               \
            Die(std::string(what) + " HRESULT=0x" + std::to_string(unsigned(_hr)));                                    \
    } while (0)
#define CHECK_FFX(rc, what)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        const ffxReturnCode_t _rc = (rc);                                                                              \
        if (_rc != FFX_API_RETURN_OK)                                                                                  \
            Die(std::string(what) + " FFX=" + std::to_string(int64_t(_rc)));                                           \
    } while (0)

// ---------------------------------------------------------------------------
// IEEE 754 half conversion via DirectXPackedVector (XMConvertFloatToHalf /
// XMConvertHalfToFloat): exact round-to-nearest-even with negatives, subnormals,
// saturation and NaN propagation handled by the shipped library instead of a
// hand-rolled copy. (The previous runner's helper returned zero for every negative
// value.)
// ---------------------------------------------------------------------------
static uint16_t FloatToHalf(float value) { return DirectX::PackedVector::XMConvertFloatToHalf(value); }
static float HalfToFloat(uint16_t h) { return DirectX::PackedVector::XMConvertHalfToFloat(h); }

// Deterministic PCG32 (fixture noise is reproducible across runs and closures).
struct Pcg32
{
    uint64_t state;
    explicit Pcg32(uint64_t seed) : state(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
    uint32_t Next()
    {
        const uint64_t old = state;
        state = old * 6364136223846793005ULL + 1442695040888963407ULL;
        const uint32_t xorshifted = uint32_t(((old >> 18) ^ old) >> 27);
        const uint32_t rot = uint32_t(old >> 59);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
    }
    // Keep Box-Muller inputs strictly inside (0,1) even after float rounding.
    // Double precision avoids rounding the upper half-bin to exactly one.
    double Uniform() { return (double(Next()) + 0.5) / 4294967296.0; }
    float Normal() // Box-Muller, deterministic
    {
        const double u1 = Uniform(), u2 = Uniform();
        return float(sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2));
    }
};

// ---------------------------------------------------------------------------
// PFM image output (float, little endian via negative scale; "PF4" = 4 channels,
// read by tests/fsr-rr/compare_runs.py).
// ---------------------------------------------------------------------------
static void WritePfm(const fs::path& path, const float* data, uint32_t w, uint32_t h, uint32_t channels)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        Die("cannot open " + path.string());
    f << (channels == 1 ? "Pf" : channels == 3 ? "PF" : "PF4") << "\n" << w << " " << h << "\n-1.0\n";
    const size_t row = size_t(w) * channels;
    for (int64_t y = int64_t(h) - 1; y >= 0; --y)
        f.write(reinterpret_cast<const char*>(data + size_t(y) * row), std::streamsize(row * sizeof(float)));
    f.close();
    if (!f)
        Die("write failed " + path.string());
}

// SHA256 through BCrypt. A failure to hash a file we already opened is an environment
// failure, and identity is load-bearing here (manifest pins, baseline/candidate
// comparison), so it FAILs the run instead of reporting a placeholder string.
static std::string Sha256File(const fs::path& path)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<uint8_t> digest(32), temp(1 << 16);
    std::string hex;
    if (FAILED(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        Die("BCryptOpenAlgorithmProvider(SHA256) failed");
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(alg, 0);
        Die("BCryptCreateHash failed");
    }
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        Die("cannot read " + path.string());
    }
    while (f)
    {
        f.read(reinterpret_cast<char*>(temp.data()), std::streamsize(temp.size()));
        const std::streamsize got = f.gcount();
        if (got > 0 && BCryptHashData(hash, temp.data(), ULONG(got), 0) < 0)
        {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            Die("BCryptHashData failed for " + path.string());
        }
    }
    if (BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) < 0)
    {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        Die("BCryptFinishHash failed for " + path.string());
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    static const char* digits = "0123456789abcdef";
    hex.reserve(64);
    for (uint8_t b : digest)
    {
        hex += digits[b >> 4];
        hex += digits[b & 15];
    }
    return hex;
}

static std::string FileVersionOf(const fs::path& path)
{
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0)
        return "<no-version-resource>";
    std::vector<uint8_t> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data()))
        return "<no-version-resource>";
    wchar_t* value = nullptr;
    UINT len = 0;
    if (VerQueryValueW(block.data(), L"\\StringFileInfo\\040904b0\\FileVersion",
                       reinterpret_cast<void**>(&value), &len) &&
        value && len)
        return std::string(value, value + len - 1);
    VS_FIXEDFILEINFO* info = nullptr;
    if (VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&info), &len) && info)
    {
        std::ostringstream os;
        os << HIWORD(info->dwFileVersionMS) << "." << LOWORD(info->dwFileVersionMS) << "."
           << HIWORD(info->dwFileVersionLS) << "." << LOWORD(info->dwFileVersionLS);
        return os.str();
    }
    return "<no-version-resource>";
}

static std::string WideToUtf8(const wchar_t* wide)
{
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(len ? len - 1 : 0), '\0');
    if (len > 1)
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

static std::string JsonEscape(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (unsigned char(c) < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            }
            else
                out += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Manifest: explicit absolute provider selection. No silent defaults.
// ---------------------------------------------------------------------------
struct Manifest
{
    std::string label = "candidate";
    uint64_t adapterLuid = 0;
    bool adapterLuidSet = false;
    uint32_t vendorId = 0x1002;
    bool requireVendor = true;
    fs::path denoiserDll, upscalerDll;
    std::string denoiserSha256, upscalerSha256;
    std::string diffuseSignal = "direct";
    std::string specularSignal = "indirect";
    uint32_t frames = 24, warmupFrames = 60, measuredFrames = 300;
    uint32_t inFlight = 2;
    uint64_t seed = 0x52524F46464C494EULL;
    uint32_t timeoutMs = 30000;
    std::string scenes;
    bool validation = true;
    uint32_t readbackEvery = 4;
    bool dumpAllFrames = false;
    std::string featureCase;
    std::vector<std::pair<std::string, std::string>> featureSettings;
};

static std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

static uint64_t ParseHexOrDec(const std::string& v)
{
    if (v.rfind("0x", 0) == 0 || v.rfind("0X", 0) == 0)
        return strtoull(v.c_str(), nullptr, 16);
    return strtoull(v.c_str(), nullptr, 10);
}

static Manifest LoadManifest(const fs::path& path)
{
    Manifest m;
    std::ifstream f(path);
    if (!f)
        Die("manifest not found (run with --probe to generate a template): " + path.string());
    std::string line;
    unsigned lineno = 0;
    while (std::getline(f, line))
    {
        ++lineno;
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        if (key == "label") m.label = value;
        else if (key == "adapter_luid") { m.adapterLuid = ParseHexOrDec(value); m.adapterLuidSet = true; }
        else if (key == "adapter_vendor_id") m.vendorId = uint32_t(ParseHexOrDec(value));
        else if (key == "require_vendor") m.requireVendor = value != "0";
        else if (key == "denoiser_dll") m.denoiserDll = value;
        else if (key == "upscaler_dll") m.upscalerDll = value;
        else if (key == "denoiser_sha256") m.denoiserSha256 = value;
        else if (key == "upscaler_sha256") m.upscalerSha256 = value;
        else if (key == "diffuse_signal") m.diffuseSignal = value;
        else if (key == "specular_signal") m.specularSignal = value;
        else if (key == "frames") m.frames = uint32_t(ParseHexOrDec(value));
        else if (key == "warmup_frames") m.warmupFrames = uint32_t(ParseHexOrDec(value));
        else if (key == "measured_frames") m.measuredFrames = uint32_t(ParseHexOrDec(value));
        else if (key == "in_flight") m.inFlight = uint32_t(ParseHexOrDec(value));
        else if (key == "seed") m.seed = ParseHexOrDec(value);
        else if (key == "timeout_ms") m.timeoutMs = uint32_t(ParseHexOrDec(value));
        else if (key == "scenes") m.scenes = value;
        else if (key == "validation") m.validation = value != "0";
        else if (key == "readback_every") m.readbackEvery = uint32_t(ParseHexOrDec(value));
        else if (key == "dump_all_frames") m.dumpAllFrames = value != "0";
        else if (key == "feature_case") m.featureCase = value;
        else if (key.rfind("feature.", 0) == 0) m.featureSettings.emplace_back(key.substr(8), value);
        // Unknown keys (e.g. capture provenance from the Godot producer) are accepted
        // and documented; nothing is silently defaulted.
    }
    if (!m.adapterLuidSet)
        Die("manifest missing required key adapter_luid (scan ended at line " + std::to_string(lineno) + ")");
    if (m.denoiserDll.empty() || m.upscalerDll.empty())
        Die("manifest missing required absolute denoiser_dll/upscaler_dll");
    if (!m.denoiserDll.is_absolute() || !m.upscalerDll.is_absolute())
        Die("manifest DLL paths must be absolute (explicit provider selection)");
    if (m.diffuseSignal != "direct" && m.diffuseSignal != "indirect")
        Die("diffuse_signal must be direct|indirect");
    if (m.specularSignal != "direct" && m.specularSignal != "indirect")
        Die("specular_signal must be direct|indirect");
    if (m.inFlight == 0 || m.inFlight > 8)
        Die("in_flight must be within 1..8");
    if (m.frames < 4)
        Die("frames must be >= 4 (temporal metrics need a sequence)");
    if (m.readbackEvery == 0)
        Die("readback_every must be >= 1");
    if (m.timeoutMs < 1000)
        Die("timeout_ms must be >= 1000 (bounded waits, but not uselessly tight)");
    return m;
}

// ---------------------------------------------------------------------------
// Game-collision check: refuse to run beside a fullscreen app. Never kills it.
// ---------------------------------------------------------------------------
static bool OtherFullscreenWindowPresent(DWORD selfPid)
{
    struct EnumCtx
    {
        DWORD self;
        bool found;
    } ctx{selfPid, false};
    EnumWindows(
        [](HWND wnd, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<EnumCtx*>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(wnd, &pid);
            if (pid == c->self || !IsWindowVisible(wnd))
                return TRUE;
            const LONG_PTR style = GetWindowLongPtrW(wnd, GWL_STYLE);
            if (style & WS_CHILD)
                return TRUE;
            wchar_t windowClass[128] {};
            GetClassNameW(wnd, windowClass, 128);
            if (wnd == GetShellWindow() || _wcsicmp(windowClass, L"Progman") == 0 ||
                _wcsicmp(windowClass, L"WorkerW") == 0 || _wcsicmp(windowClass, L"Shell_TrayWnd") == 0 ||
                _wcsicmp(windowClass, L"Shell_SecondaryTrayWnd") == 0)
                return TRUE;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process)
            {
                wchar_t image[32768] {};
                DWORD count = 32768;
                const bool read = QueryFullProcessImageNameW(process, 0, image, &count) != FALSE;
                CloseHandle(process);
                const wchar_t* name = read ? wcsrchr(image, L'\\') : nullptr;
                if (name && _wcsicmp(name + 1, L"TextInputHost.exe") == 0)
                    return TRUE;
            }
            // Maximized framed desktop applications extend slightly beyond the monitor
            // and are not exclusive/borderless graphics workloads.
            if (IsZoomed(wnd) && (style & WS_CAPTION) == WS_CAPTION)
                return TRUE;
            RECT r{};
            if (!GetWindowRect(wnd, &r))
                return TRUE;
            HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{sizeof(MONITORINFO)};
            if (mon && GetMonitorInfoW(mon, &mi) && r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
                r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom)
            {
                wchar_t title[128]{};
                GetWindowTextW(wnd, title, 127);
                if (title[0])
                {
                    fwprintf(stderr, L"GPU collision: pid=%lu class=%ls title=%ls\n", pid, windowClass, title);
                    c->found = true;
                    return FALSE;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
struct GpuContext
{
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12InfoQueue> info;
    ComPtr<ID3D12Fence> utilFence;
    HANDLE utilEvent = nullptr;
    DXGI_ADAPTER_DESC3 adapterDesc{};
    bool debugLayer = false;
    unsigned validationErrors = 0, validationWarnings = 0;
    uint32_t timeoutMs = 30000;

    void Init(uint64_t luid, uint32_t vendorId, bool requireVendor, uint32_t timeout)
    {
        timeoutMs = timeout;
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            debugLayer = true;
        }
        CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        bool found = false;
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter4> a;
            if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                           IID_PPV_ARGS(&a))))
                break;
            DXGI_ADAPTER_DESC3 d{};
            a->GetDesc3(&d);
            const uint64_t candidate = uint64_t(d.AdapterLuid.HighPart) << 32 | uint32_t(d.AdapterLuid.LowPart);
            fprintf(stderr, "adapter[%u] vendor=0x%04X luid=0x%016llX %ls\n", i, d.VendorId,
                    (unsigned long long) candidate, d.Description);
            if (candidate == luid)
            {
                adapter = a;
                adapterDesc = d;
                found = true;
                break;
            }
        }
        if (!found)
            Die("no adapter matches manifest adapter_luid; rerun --probe and review");
        if (requireVendor && adapterDesc.VendorId != vendorId)
            Die("adapter vendor mismatch: device 0x" + std::to_string(adapterDesc.VendorId) + " manifest 0x" +
                std::to_string(vendorId));
        CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)),
                 "D3D12CreateDevice");
        device.As(&info); // optional; absence recorded, not fatal
        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
        CHECK_HR(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&utilFence)), "util fence");
        utilEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!utilEvent)
            Die("CreateEventW failed");
    }

    void WaitUtilFence(uint64_t value)
    {
        if (utilFence->GetCompletedValue() >= value)
            return;
        CHECK_HR(utilFence->SetEventOnCompletion(value, utilEvent), "SetEventOnCompletion");
        if (WaitForSingleObject(utilEvent, timeoutMs) != WAIT_OBJECT_0)
            Die("GPU fence timeout (device hung or workload stuck)");
        CheckAlive();
    }

    void CheckAlive()
    {
        const HRESULT removed = device->GetDeviceRemovedReason();
        if (FAILED(removed))
            Die(std::string("device removed: 0x") + std::to_string(unsigned(removed)));
    }

    void DrainInfoQueue(const char* scope)
    {
        if (!info)
            return;
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T n = 0;
            info->GetMessage(i, nullptr, &n);
            std::vector<char> storage(n);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(info->GetMessage(i, msg, &n)))
                continue;
            if (msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                ++validationErrors;
                fprintf(stderr, "[D3D12][%s][ERROR] %s\n", scope, msg->pDescription);
            }
            else if (msg->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
            {
                ++validationWarnings;
                if (validationWarnings <= 64)
                    fprintf(stderr, "[D3D12][%s][WARN] %s\n", scope, msg->pDescription);
            }
        }
        info->ClearStoredMessages();
    }
};

// ---------------------------------------------------------------------------
// Effect DLL wrapper (upstream fsrd_rr_runner pattern: load this exact file, resolve
// with ffxLoadFunctions, record the loaded module's own path back).
// ---------------------------------------------------------------------------
static unsigned g_sdkErrors = 0, g_sdkWarnings = 0;
static void FfxMessage(uint32_t type, const wchar_t* message)
{
    if (type == FFX_API_MESSAGE_TYPE_ERROR)
        ++g_sdkErrors;
    else if (type == FFX_API_MESSAGE_TYPE_WARNING)
        ++g_sdkWarnings;
    fwprintf(stderr, L"[FFX][%ls] %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? L"ERROR" : L"WARN", message);
}

struct EffectDll
{
    fs::path requested;
    std::wstring loadedPath;
    std::string sha256, fileVersion;
    uintmax_t size = 0;
    HMODULE module = nullptr;
    ffxFunctions api{};

    void Load(const fs::path& path, const std::string& shaPin)
    {
        requested = path;
        if (!fs::exists(path))
            Die("provider DLL missing: " + path.string());
        size = fs::file_size(path);
        sha256 = Sha256File(path);
        if (!shaPin.empty() && sha256 != shaPin)
            Die("provider DLL sha256 mismatch for " + path.string() + ": " + sha256 + " != " + shaPin);
        fileVersion = FileVersionOf(path);
        module = LoadLibraryExW(path.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module)
            Die("LoadLibraryEx failed for " + path.string() + " err=" + std::to_string(GetLastError()));
        wchar_t actual[32768]{};
        GetModuleFileNameW(module, actual, 32768);
        loadedPath = actual;
        ffxLoadFunctions(&api, module);
        if (!api.CreateContext || !api.Query || !api.Dispatch || !api.Configure || !api.DestroyContext)
            Die("FFX exports missing in " + path.string());
    }
    ~EffectDll()
    {
        if (module)
            FreeLibrary(module);
    }
};

// ---------------------------------------------------------------------------
// GPU textures with footprint-exact staging (upstream fsrd_gpu_runner pattern).
// ---------------------------------------------------------------------------
static constexpr D3D12_RESOURCE_STATES kRead =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

struct GpuTexture
{
    ComPtr<ID3D12Resource> tex;
    ComPtr<ID3D12Resource> staging;
    std::vector<ComPtr<ID3D12Resource>> uploadRing;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t stagingSize = 0;
    uint32_t width = 0, height = 0, bytesPerPixel = 0;
    DXGI_FORMAT format{};
    D3D12_RESOURCE_STATES state{};

    static uint32_t Bpp(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16_FLOAT: return 4;
        default: Die("unsupported fixture format"); return 0;
        }
    }

    void Init(ID3D12Device* dev, DXGI_FORMAT fmt, uint32_t w, uint32_t h, bool uav, bool needStaging,
              const wchar_t* name, uint32_t stagingSlots = 1)
    {
        format = fmt;
        width = w;
        height = h;
        bytesPerPixel = Bpp(fmt);
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        state = uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST;
        CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&tex)),
                 "CreateCommittedResource(texture)");
        tex->SetName(name);
        dev->GetCopyableFootprints(&d, 0, 1, 0, &footprint, nullptr, nullptr, &stagingSize);
        if (needStaging)
        {
            D3D12_HEAP_PROPERTIES shp{uav ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD};
            D3D12_RESOURCE_DESC sd{};
            sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            sd.Width = stagingSize;
            sd.Height = 1;
            sd.DepthOrArraySize = 1;
            sd.MipLevels = 1;
            sd.SampleDesc.Count = 1;
            sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            CHECK_HR(dev->CreateCommittedResource(&shp, D3D12_HEAP_FLAG_NONE, &sd,
                                                  uav ? D3D12_RESOURCE_STATE_COPY_DEST
                                                      : D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, IID_PPV_ARGS(&staging)),
                     "CreateCommittedResource(staging)");
            if (!uav && stagingSlots > 1)
            {
                uploadRing.resize(stagingSlots);
                uploadRing[0] = staging;
                for (uint32_t i = 1; i < stagingSlots; ++i)
                    CHECK_HR(dev->CreateCommittedResource(&shp, D3D12_HEAP_FLAG_NONE, &sd,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&uploadRing[i])),
                             "CreateCommittedResource(uploadRing)");
            }
        }
    }

    size_t RowBytes() const { return size_t(width) * bytesPerPixel; }

    ID3D12Resource* Staging(uint32_t slot = 0) const
    {
        return slot < uploadRing.size() ? uploadRing[slot].Get() : staging.Get();
    }

    void Upload(const std::vector<uint8_t>& packed, uint32_t slot = 0)
    {
        if (packed.size() != RowBytes() * height)
            Die("fixture upload size mismatch");
        uint8_t* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        ID3D12Resource* targetStaging = Staging(slot);
        CHECK_HR(targetStaging->Map(0, &none, reinterpret_cast<void**>(&mapped)), "upload map");
        for (uint32_t y = 0; y < height; ++y)
            memcpy(mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                   packed.data() + y * RowBytes(), RowBytes());
        targetStaging->Unmap(0, nullptr);
    }

    std::vector<float> DownloadAsFloat(uint32_t outChannels)
    {
        std::vector<float> out(size_t(width) * height * outChannels, 0.0f);
        uint8_t* mapped = nullptr;
        ID3D12Resource* targetStaging = staging.Get();
        CHECK_HR(targetStaging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "readback map");
        const uint8_t* base = mapped + footprint.Offset;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* row = base + size_t(y) * footprint.Footprint.RowPitch;
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t si = size_t(y) * width + x;
                const uint8_t* px = row + size_t(x) * bytesPerPixel;
                float v[4] = {0, 0, 0, 1};
                switch (format)
                {
                case DXGI_FORMAT_R16G16B16A16_FLOAT:
                    for (uint32_t c = 0; c < 4; ++c)
                        v[c] = HalfToFloat(reinterpret_cast<const uint16_t*>(px)[c]);
                    break;
                case DXGI_FORMAT_R16G16_FLOAT:
                    v[0] = HalfToFloat(reinterpret_cast<const uint16_t*>(px)[0]);
                    v[1] = HalfToFloat(reinterpret_cast<const uint16_t*>(px)[1]);
                    break;
                case DXGI_FORMAT_R32_FLOAT: v[0] = reinterpret_cast<const float*>(px)[0]; break;
                case DXGI_FORMAT_R10G10B10A2_UNORM:
                {
                    uint32_t packed; memcpy(&packed, px, sizeof packed);
                    v[0] = float(packed & 1023u) / 1023.0f; v[1] = float((packed >> 10) & 1023u) / 1023.0f;
                    v[2] = float((packed >> 20) & 1023u) / 1023.0f; v[3] = float(packed >> 30) / 3.0f;
                    break;
                }
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                    for (uint32_t c = 0; c < 4; ++c)
                        v[c] = float(px[c]) / 255.0f;
                    break;
                default: Die("download: unsupported format");
                }
                for (uint32_t c = 0; c < outChannels; ++c)
                    out[si * outChannels + c] = c < 4 ? v[c] : 0.0f;
            }
        }
        targetStaging->Unmap(0, nullptr);
        return out;
    }
};

// ---------------------------------------------------------------------------
// Fixture: deterministic analytic scenes.
//
// Motion-vector contract (and its honest limitation, recorded in run.json): MVs are
// full reprojection deltas (camera AND object motion) in render pixels, unjittered.
// Product conversion reconstructs camera-only MV.Z from current position and the
// previous view. These fixtures do not claim to supply missing dynamic-surface
// depth motion through the two-component NGX motion texture.
// ---------------------------------------------------------------------------
enum class Handed
{
    LH,
    RH
};

struct SceneConfig
{
    const char* name = "";
    uint32_t renderW = 256, renderH = 160;
    uint32_t targetScale = 2;
    uint32_t explicitTargetW = 0, explicitTargetH = 0; // captures: exact extent
    Handed handed = Handed::LH;
    bool reversedZ = true;
    bool infiniteFar = false;
    float nearPlane = 0.1f, farPlane = 1000.0f;
    float vfovRad = 1.04719755f; // 60 degrees
    float noiseSigma = 0.12f;   // relative noise amplitude for the noisy pass
    bool hdr = false;
    bool dark = false;
    bool detailPattern = false;
    bool occlusionReveal = false;
    uint32_t revealFrame = 12;
    bool orbit = true;
    bool exposureRamp = false;
    bool separateRoughness = false;
    bool displayResMV = false;
    bool linearDepth = false;
    bool subrect = false;
    bool hitDistance = false;
    bool nativeDiffuse = false;
    bool panStop = false;
    bool nativeSpecular = false;
    float nativeSignalAlpha = 0.0f; // diagnostic only; legal active DirectDiffuse alpha
    bool resetEveryFrame = false;
    std::vector<uint32_t> resetFrames{};
    uint32_t frames = 0;

    uint32_t TargetW() const { return explicitTargetW ? explicitTargetW : renderW * targetScale; }
    uint32_t TargetH() const { return explicitTargetH ? explicitTargetH : renderH * targetScale; }
};

static std::vector<SceneConfig> DefaultScenes(uint32_t frames)
{
    auto base = [&](SceneConfig c) {
        c.frames = frames;
        return c;
    };
    return {
        base({.name = "static_lh_reversed"}),
        base({.name = "detail_stripes", .reversedZ = false, .detailPattern = true}),
        base({.name = "dark_low_light", .noiseSigma = 0.25f, .dark = true}),
        base({.name = "hdr_bright", .hdr = true}),
        base({.name = "zero_rough_mirror", .reversedZ = false}),
        base({.name = "motion_orbit_drops_guides", .hitDistance = true}),
        base({.name = "occlusion_reveal", .occlusionReveal = true, .orbit = false}),
        base({.name = "exposure_change", .orbit = false, .exposureRamp = true}),
        base({.name = "reset_pulse", .reversedZ = false, .orbit = false, .resetFrames = {10, 11}}),
        base({.name = "odd_257x193_rh_inf", .renderW = 257, .renderH = 193, .targetScale = 3, .handed = Handed::RH,
              .infiniteFar = true}),
        base({.name = "subrect_display_mv", .reversedZ = false, .displayResMV = true, .subrect = true}),
        base({.name = "depth_linear_rh", .handed = Handed::RH, .reversedZ = false, .linearDepth = true}),
        base({.name = "separate_roughness_lh", .reversedZ = false, .separateRoughness = true}),
    };
}

struct Sphere
{
    XMFLOAT3 c;
    float r;
    float rough;
    XMFLOAT3 albDiffuse;
    XMFLOAT3 albSpecular;
    bool isOccluder = false;
};

struct AnalyticScene
{
    SceneConfig cfg;
    std::vector<Sphere> spheres;
    XMFLOAT3 lightDir{0.4f, 0.8f, 0.45f};
    float sun = 1.6f, ambient = 0.25f;

    void Build(const SceneConfig& c)
    {
        cfg = c;
        if (c.nativeDiffuse && !c.detailPattern) return; // constant-albedo flat diagnostic
        sun = c.hdr ? 48.0f : c.dark ? 0.05f : 1.6f;
        ambient = c.dark ? 0.002f : c.hdr ? 0.25f : 0.25f;
        const float alb = c.dark ? 0.004f : 1.0f;
        spheres.push_back({.c = {-1.4f, 1.0f, -1.0f}, .r = 1.0f, .rough = 0.0f,
                           .albDiffuse = {0.25f * alb, 0.27f * alb, 0.30f * alb},
                           .albSpecular = {0.90f, 0.90f, 0.92f}}); // exact zero roughness
        spheres.push_back({.c = {0.0f, 1.0f, 0.2f}, .r = 1.0f, .rough = 0.35f,
                           .albDiffuse = {0.75f * alb, 0.45f * alb, 0.25f * alb},
                           .albSpecular = {0.28f, 0.26f, 0.22f}});
        spheres.push_back({.c = {1.7f, 0.8f, -1.2f}, .r = 0.8f, .rough = 0.60f,
                           .albDiffuse = {0.10f * alb, 0.35f * alb, 0.70f * alb},
                           .albSpecular = {0.10f, 0.10f, 0.12f}});
        if (c.occlusionReveal)
            spheres.push_back({.c = {0.9f, 0.8f, -1.2f}, .r = 0.85f, .rough = 0.2f,
                               .albDiffuse = {0.6f * alb, 0.6f * alb, 0.6f * alb},
                               .albSpecular = {0.4f, 0.4f, 0.4f}, .isOccluder = true});
    }

    XMFLOAT3 OccluderCenter(uint32_t frame) const
    {
        const Sphere& s = spheres.back();
        const float t = cfg.revealFrame == 0 ? 1.0f : float(frame) / float(cfg.revealFrame);
        return {s.c.x + 1.9f * t, s.c.y, s.c.z};
    }
};

struct FrameCamera
{
    XMMATRIX view{}, invView{}, proj{}, viewProj{}, prevViewProj{}, baseView{}, prevView{};
    XMFLOAT3 camPos{}, prevCamPos{}, camDelta{};
    float jitterX = 0, jitterY = 0, prevJitterX = 0, prevJitterY = 0;
    bool cut = false;
};

static float Halton(uint32_t index, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    while (index > 0)
    {
        f /= float(base);
        r += f * float(index % base);
        index /= base;
    }
    return r;
}

static FrameCamera BuildCamera(const SceneConfig& cfg, uint32_t frame)
{
    FrameCamera cam;
    const bool reset =
        frame == 0 || cfg.resetEveryFrame || std::find(cfg.resetFrames.begin(), cfg.resetFrames.end(), frame) != cfg.resetFrames.end();
    auto PanOffset = [&](uint32_t f) {
        if (!cfg.panStop || f < 8) return 0.0f;
        if (f < 20) return float(f - 7) * 0.08f;
        return 12.0f * 0.08f;
    };
    auto Pose = [&](uint32_t f) {
        const float a = cfg.orbit ? (float(f) * 0.031f) : 0.35f;
        return XMFLOAT3{4.5f * cosf(a) + PanOffset(f), 2.3f + (cfg.orbit ? 0.1f * sinf(a * 2.0f) : 0.0f), 4.5f * sinf(a)};
    };
    const XMFLOAT3 basePos = Pose(0);
    cam.camPos = Pose(frame);
    cam.prevCamPos = (frame == 0 || reset) ? cam.camPos : Pose(frame - 1);
    cam.cut = frame == 0 || reset;
    cam.camDelta = {cam.prevCamPos.x - cam.camPos.x, cam.prevCamPos.y - cam.camPos.y,
                    cam.prevCamPos.z - cam.camPos.z};

    const XMVECTOR baseEye = XMLoadFloat3(&basePos);
    const XMVECTOR baseAt = XMVectorSet(0.15f, 1.0f, -0.2f, 1.0f);
    const XMVECTOR eye = XMLoadFloat3(&cam.camPos);
    const XMVECTOR at = XMVectorSet(0.15f + PanOffset(frame), 1.0f, -0.2f, 1.0f);
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    cam.baseView = cfg.handed == Handed::RH ? XMMatrixLookAtRH(baseEye, baseAt, up) : XMMatrixLookAtLH(baseEye, baseAt, up);
    cam.view = cfg.handed == Handed::RH ? XMMatrixLookAtRH(eye, at, up) : XMMatrixLookAtLH(eye, at, up);
    cam.invView = XMMatrixInverse(nullptr, cam.view);

    const float aspect = float(cfg.renderW) / float(cfg.renderH);
    XMMATRIX rowProj;
    if (cfg.infiniteFar)
    {
        // Stable reversed infinite: A=0, B=near, W=+1/-1. Device depth = near / L.
        const float yScale = 1.0f / tanf(cfg.vfovRad * 0.5f);
        const float W = cfg.handed == Handed::RH ? -1.0f : 1.0f;
        rowProj = XMMatrixSet(yScale / aspect, 0, 0, 0, 0, yScale, 0, 0, 0, 0, 0.0f, W, 0, 0, cfg.nearPlane, 0);
    }
    else
    {
        // Swapping physical near/far produces the reversed-Z projection (product pattern).
        const float mNear = cfg.reversedZ ? cfg.farPlane : cfg.nearPlane;
        const float mFar = cfg.reversedZ ? cfg.nearPlane : cfg.farPlane;
        rowProj = cfg.handed == Handed::RH ? XMMatrixPerspectiveFovRH(cfg.vfovRad, aspect, mNear, mFar)
                                           : XMMatrixPerspectiveFovLH(cfg.vfovRad, aspect, mNear, mFar);
    }
    cam.proj = rowProj; // CPU DirectXMath always uses row vectors.
    cam.viewProj = XMMatrixMultiply(cam.view, cam.proj);

    const uint32_t prevF = (frame == 0 || reset) ? frame : frame - 1;
    const XMVECTOR prevAt = XMVectorSet(0.15f + PanOffset(prevF), 1.0f, -0.2f, 1.0f);
    const XMVECTOR prevEye = XMLoadFloat3(&cam.prevCamPos);
    const XMMATRIX prevView = cfg.handed == Handed::RH ? XMMatrixLookAtRH(prevEye, prevAt, up)
                                                       : XMMatrixLookAtLH(prevEye, prevAt, up);
    cam.prevView = prevView;
    cam.prevViewProj = XMMatrixMultiply(prevView, cam.proj);

    // Jittered raster: pixel centers shifted by the Halton jitter; the pair is reported
    // to RR/SR and MVs stay unjittered (typical DLSS-RR title contract).
    cam.jitterX = Halton(frame + 1, 2) - 0.5f;
    cam.jitterY = Halton(frame + 1, 3) - 0.5f;
    cam.prevJitterX = frame == 0 ? cam.jitterX : Halton(frame, 2) - 0.5f;
    cam.prevJitterY = frame == 0 ? cam.jitterY : Halton(frame, 3) - 0.5f;
    return cam;
}

struct FixtureFrame
{
    std::vector<uint8_t> color, depth, motion, normals, roughness, diffAlbedo, specAlbedo, hitDistance, exposure;
    std::vector<uint8_t> diffuseHitDistance;
    // Independent renderer projection for SR when RR consumes signed linear Z.
    // Tight, zero-origin render extent; never derived from converter output.
    std::vector<uint8_t> srHardwareDepth;
    // Native RR guides are authored here, before any product conversion.
    std::vector<uint8_t> nativeDiffuse, nativeSpecular, nativeDepth, nativeNormals, nativeMotion;
    std::vector<float> truthRender, truthTarget, truthJittered;
};

static FixtureFrame RenderFixture(const AnalyticScene& scene, const FrameCamera& cam, uint32_t frame, uint64_t seed,
                                  uint32_t texW, uint32_t texH, uint32_t baseX, uint32_t baseY, float sigma,
                                  bool noisy)
{
    const SceneConfig& cfg = scene.cfg;
    const uint32_t rw = cfg.renderW, rh = cfg.renderH;
    FixtureFrame out;
    out.color.resize(size_t(texW) * 8 * texH);
    out.depth.resize(size_t(texW) * 4 * texH);
    if (cfg.linearDepth && !HasUpscalerDepth_v<FSRDPreprocessor_Dx12>)
        out.srHardwareDepth.resize(size_t(rw) * rh * 4);
    const uint32_t motionW = cfg.displayResMV ? cfg.TargetW() : rw;
    const uint32_t motionH = cfg.displayResMV ? cfg.TargetH() : rh;
    const uint32_t motionTexW = motionW + texW - rw;
    const uint32_t motionTexH = motionH + texH - rh;
    out.motion.resize(size_t(motionTexW) * 4 * motionTexH);
    out.normals.resize(size_t(texW) * 8 * texH);
    if (cfg.separateRoughness)
        out.roughness.resize(size_t(texW) * 4 * texH);
    out.diffAlbedo.resize(size_t(texW) * 4 * texH);
    out.specAlbedo.resize(size_t(texW) * 4 * texH);
    if (cfg.hitDistance)
    {
        out.hitDistance.resize(size_t(texW) * 4 * texH);
        out.diffuseHitDistance.resize(size_t(texW) * 4 * texH);
    }
    if (cfg.exposureRamp)
        out.exposure.resize(4);
    out.truthRender.resize(size_t(rw) * rh * 4);
    out.truthJittered.resize(size_t(rw) * rh * 4);
    out.truthTarget.resize(size_t(cfg.TargetW()) * cfg.TargetH() * 4);
    if (cfg.nativeDiffuse)
    {
        out.nativeDiffuse.resize(size_t(rw) * rh * 8);
        if (cfg.nativeSpecular) out.nativeSpecular.resize(size_t(rw) * rh * 8);
        out.nativeDepth.resize(size_t(rw) * rh * 4);
        out.nativeNormals.resize(size_t(rw) * rh * 4);
        out.nativeMotion.resize(size_t(rw) * rh * 8);
    }
    if (cfg.exposureRamp)
    {
        const float e = frame >= 12 ? 0.25f : 1.0f;
        memcpy(out.exposure.data(), &e, 4);
    }

    const XMMATRIX invViewProj = XMMatrixInverse(nullptr, cam.viewProj);
    Pcg32 rng(seed ^ (uint64_t(frame) * 0x9E3779B97F4A7C15ULL));

    struct Shaded
    {
        XMFLOAT3 radiance{0.004f, 0.006f, 0.012f}; // sky floor: positive everywhere
        XMFLOAT3 normal{0, 1, 0};
        float directDiffuse = 0.0f; // albedo-free Lambertian lighting, not divided final RGB
        float directSpecular = 0.0f; // albedo-free specular lighting
        float rough = 1.0f;
        XMFLOAT3 albD{0, 0, 0}, albS{0, 0, 0};
        float viewDist = 1000.0f, hitDist = 0.0f, diffuseHitDist = 0.0f;
        XMVECTOR world{};
        bool hit = false;
        bool movingOccluder = false;
    };

    XMFLOAT3 occPos{};
    const bool hasOccluder = [&]() {
        if (cfg.occlusionReveal)
        {
            occPos = scene.OccluderCenter(frame);
            return true;
        }
        return false;
    }();
    auto ResolveSphere = [&](const Sphere& s) {
        if (s.isOccluder && hasOccluder)
        {
            Sphere m = s;
            m.c = occPos;
            return m;
        }
        return s;
    };

    auto Trace = [&](float px, float py, uint32_t w, uint32_t h) {
        const float ndcX = (px / float(w)) * 2.0f - 1.0f;
        const float ndcY = 1.0f - (py / float(h)) * 2.0f;
        // Interior device depth avoids either infinite endpoint; the camera is
        // the ray origin for standard/reversed Z and both handednesses.
        XMVECTOR point = XMVector4Transform(XMVectorSet(ndcX, ndcY, 0.5f, 1.0f), invViewProj);
        point = XMVectorDivide(point, XMVectorSplatW(point));
        const XMFLOAT3 origin = cam.camPos;
        XMFLOAT3 dir;
        XMStoreFloat3(&dir, XMVector3Normalize(XMVectorSubtract(point, XMLoadFloat3(&cam.camPos))));

        float bestT = 1e30f;
        const Sphere* best = nullptr;
        if (cfg.nativeDiffuse)
        {
            // Finite camera-facing rectangle, view Z=12, half extent 14x9 (18x12 during panStop).
            // Anchored in baseView so world-space points remain rigid during camera pan.
            const XMMATRIX planeView = cfg.panStop ? cam.baseView : cam.view;
            XMFLOAT3 viewOrigin, viewDir;
            XMStoreFloat3(&viewOrigin, XMVector3Transform(XMLoadFloat3(&origin), planeView));
            XMStoreFloat3(&viewDir, XMVector3TransformNormal(XMLoadFloat3(&dir), planeView));
            const float forward = cfg.handed == Handed::RH ? -1.0f : 1.0f;
            const float t = (forward * 12.0f - viewOrigin.z) / viewDir.z;
            const float halfX = cfg.panStop ? 18.0f : 14.0f;
            const float halfY = cfg.panStop ? 12.0f : 9.0f;
            if (t > 0.0f && std::abs(viewOrigin.x + t * viewDir.x) <= halfX && std::abs(viewOrigin.y + t * viewDir.y) <= halfY)
                bestT = t;
        }
        else if (fabsf(dir.y) > 1e-6f)
        {
            const float t = -origin.y / dir.y;
            if (t > 1e-4f && t < bestT)
                bestT = t;
        }
        for (const Sphere& raw : scene.spheres)
        {
            const Sphere s = ResolveSphere(raw);
            const float ox = origin.x - s.c.x, oy = origin.y - s.c.y, oz = origin.z - s.c.z;
            const float bq = ox * dir.x + oy * dir.y + oz * dir.z;
            const float cq = ox * ox + oy * oy + oz * oz - s.r * s.r;
            const float disc = bq * bq - cq;
            if (disc > 0.0f)
            {
                const float t = -bq - sqrtf(disc);
                if (t > 1e-4f && t < bestT)
                {
                    bestT = t;
                    best = &raw;
                }
            }
        }
        if (cfg.nativeDiffuse && bestT < 1e29f)
        {
            const XMVECTOR hit = XMVectorSet(origin.x + dir.x * bestT, origin.y + dir.y * bestT,
                                            origin.z + dir.z * bestT, 1.0f);
            const float z = std::abs(XMVectorGetZ(XMVector3Transform(hit, cam.view)));
            if (z <= cfg.nearPlane || z >= cfg.farPlane ||
                std::log(z + 1.0f) / std::log(cfg.farPlane + 1.0f) >= 0.99f)
                bestT = 1e30f; // explicit miss, never an evaluated out-of-range surface
        }

        Shaded r;
        r.hit = bestT < 1e29f;
        r.world = XMVectorSet(origin.x + dir.x * bestT, origin.y + dir.y * bestT, origin.z + dir.z * bestT, 1.0f);
        if (!r.hit)
        {
            r.viewDist = cfg.nativeDiffuse ? cfg.farPlane + 1.0f
                : (cfg.infiniteFar ? 20000.0f : cfg.farPlane * 0.98f);
            const float viewZPerUnit = std::abs(XMVectorGetZ(XMVector3TransformNormal(XMLoadFloat3(&dir), cam.view)));
            r.world = XMVectorSet(origin.x + dir.x * r.viewDist / viewZPerUnit,
                origin.y + dir.y * r.viewDist / viewZPerUnit, origin.z + dir.z * r.viewDist / viewZPerUnit, 1.0f);
            if (cfg.nativeDiffuse) r.radiance = {0, 0, 0}; // no emissive/environment bypass
            return r;
        }
        XMFLOAT3 pos;
        XMStoreFloat3(&pos, r.world);
        if (best)
        {
            const Sphere s = ResolveSphere(*best);
            r.normal = {(pos.x - s.c.x) / s.r, (pos.y - s.c.y) / s.r, (pos.z - s.c.z) / s.r};
            r.rough = s.rough;
            r.albD = s.albDiffuse;
            r.albS = s.albSpecular;
            // Secondary reflection length is traced below, never primary camera distance.
            r.movingOccluder = best->isOccluder;
        }
        else
        {
            if (cfg.nativeDiffuse)
            {
                const XMMATRIX planeView = cfg.panStop ? cam.baseView : cam.view;
                const XMMATRIX invPlaneView = XMMatrixInverse(nullptr, planeView);
                const float facing = cfg.handed == Handed::RH ? 1.0f : -1.0f;
                XMStoreFloat3(&r.normal, XMVector3TransformNormal(XMVectorSet(0, 0, facing, 0), invPlaneView));
                const XMVECTOR viewPos = XMVector3Transform(r.world, planeView);
                const int checker = (int(floorf(XMVectorGetX(viewPos) * 4.0f)) +
                                     int(floorf(XMVectorGetY(viewPos) * 4.0f))) & 1;
                const float a = cfg.detailPattern && checker ? 0.45f : 1.0f;
                r.albD = {a * 0.8f, a * 0.75f, a * 0.6f};
                r.albS = cfg.nativeSpecular ? XMFLOAT3{0.06f, 0.06f, 0.07f} : XMFLOAT3{0, 0, 0};
                r.rough = cfg.nativeSpecular ? 0.35f : 1.0f;
            }
            else
            {
                r.normal = {0.0f, 1.0f, 0.0f};
                const float scale = cfg.detailPattern ? 4.0f : 1.0f;
                const int checker = (int(floorf(pos.x * scale)) + int(floorf(pos.z * scale))) & 1;
                const float a = checker ? 1.0f : 0.45f;
                r.albD = {a * 0.8f, a * 0.75f, a * 0.6f};
                r.albS = {0.04f, 0.04f, 0.05f};
                r.rough = 0.8f;
            }
        }
        const float dx = cam.camPos.x - pos.x, dy = cam.camPos.y - pos.y, dz = cam.camPos.z - pos.z;
        r.viewDist = std::abs(XMVectorGetZ(XMVector3Transform(r.world, cam.view)));
        if (cfg.nativeDiffuse && !cfg.nativeSpecular)
        {
            r.albS = {0, 0, 0};
            r.rough = 1.0f;
            XMStoreFloat3(&r.normal, XMVector3Normalize(XMLoadFloat3(&r.normal)));
            // The DLSS-shaped albedo is UNORM8. Render using that exact material,
            // so native and translated routes share both samples and reflectance.
            for (int c = 0; c < 3; ++c)
                (&r.albD.x)[c] = std::round(std::clamp((&r.albD.x)[c], 0.0f, 1.0f) * 255.0f) / 255.0f;
            const XMVECTOR light = XMVector3Normalize(XMLoadFloat3(&scene.lightDir));
            r.directDiffuse = scene.sun * std::max(0.0f, XMVectorGetX(XMVector3Dot(XMLoadFloat3(&r.normal), light))) / XM_PI;
            r.radiance = {r.albD.x * r.directDiffuse, r.albD.y * r.directDiffuse, r.albD.z * r.directDiffuse};
            return r;
        }
        // Actual secondary rays against the same geometry: mirror reflection for
        // specular, deterministic cosine-weighted hemisphere sample for diffuse.
        const auto secondaryDistance = [&](XMVECTOR direction) {
            const XMVECTOR start = XMVectorAdd(r.world, XMVectorScale(XMLoadFloat3(&r.normal), 1e-3f));
            XMFLOAT3 so, sd; XMStoreFloat3(&so, start); XMStoreFloat3(&sd, direction);
            float distance = 1e30f;
            if (cfg.nativeDiffuse)
            {
                const XMMATRIX planeView = cfg.panStop ? cam.baseView : cam.view;
                XMFLOAT3 vSo, vSd;
                XMStoreFloat3(&vSo, XMVector3Transform(start, planeView));
                XMStoreFloat3(&vSd, XMVector3TransformNormal(direction, planeView));
                const float forward = cfg.handed == Handed::RH ? -1.0f : 1.0f;
                if (std::abs(vSd.z) > 1e-6f)
                {
                    const float t = (forward * 12.0f - vSo.z) / vSd.z;
                    if (t > 1e-3f) distance = std::min(distance, t);
                }
            }
            else if (std::abs(sd.y) > 1e-6f)
            {
                const float t = -so.y / sd.y; if (t > 1e-3f) distance = t;
            }
            for (const Sphere& raw : scene.spheres)
            {
                const Sphere sphere = ResolveSphere(raw);
                const float ox = so.x - sphere.c.x, oy = so.y - sphere.c.y, oz = so.z - sphere.c.z;
                const float b = ox * sd.x + oy * sd.y + oz * sd.z;
                const float discriminant = b * b - (ox * ox + oy * oy + oz * oz - sphere.r * sphere.r);
                if (discriminant >= 0)
                {
                    const float t = -b - std::sqrt(discriminant); if (t > 1e-3f) distance = std::min(distance, t);
                }
            }
            return distance < 1e29f ? distance : 0.0f; // zero explicitly means ray miss
        };
        const XMVECTOR normal = XMLoadFloat3(&r.normal);
        r.hitDist = secondaryDistance(XMVector3Reflect(XMLoadFloat3(&dir), normal));
        const XMVECTOR axis = std::abs(r.normal.y) < 0.95f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
        const XMVECTOR tangent = XMVector3Normalize(XMVector3Cross(axis, normal));
        const XMVECTOR bitangent = XMVector3Cross(normal, tangent);
        const float u = Halton(uint32_t(px * 17 + py * 131) + frame + 1, 2);
        const float phi = XM_2PI * Halton(uint32_t(px * 31 + py * 73) + frame + 1, 3);
        const XMVECTOR diffuseRay = XMVectorAdd(XMVectorScale(normal, std::sqrt(1 - u)),
            XMVectorAdd(XMVectorScale(tangent, std::sqrt(u) * std::cos(phi)), XMVectorScale(bitangent, std::sqrt(u) * std::sin(phi))));
        r.diffuseHitDist = secondaryDistance(diffuseRay);
        const float ndl = std::max(0.0f, r.normal.x * scene.lightDir.x + r.normal.y * scene.lightDir.y +
                                             r.normal.z * scene.lightDir.z);
        const float gloss = r.rough <= 0.0f ? 2048.0f : 2.0f / std::max(1e-4f, r.rough * r.rough) - 2.0f;
        float hx = scene.lightDir.x + dx, hy = scene.lightDir.y + dy, hz = scene.lightDir.z + dz;
        const float hl = std::max(1e-6f, sqrtf(hx * hx + hy * hy + hz * hz));
        hx /= hl;
        hy /= hl;
        hz /= hl;
        const float ndh = std::max(0.0f, r.normal.x * hx + r.normal.y * hy + r.normal.z * hz);
        const float specPow = powf(ndh, gloss) * (r.rough <= 0.0f ? scene.sun * 1.5f : scene.sun * 0.25f);
        if (cfg.nativeDiffuse && cfg.nativeSpecular)
        {
            XMStoreFloat3(&r.normal, XMVector3Normalize(XMLoadFloat3(&r.normal)));
            for (int c = 0; c < 3; ++c)
            {
                (&r.albS.x)[c] = std::round(std::clamp((&r.albS.x)[c], 0.0f, 0.9f) * 255.0f) / 255.0f;
                (&r.albD.x)[c] = std::round(std::clamp((&r.albD.x)[c], 0.0f, 1.0f - (&r.albS.x)[c]) * 255.0f) / 255.0f;
            }
            const XMVECTOR light = XMVector3Normalize(XMLoadFloat3(&scene.lightDir));
            r.directDiffuse = scene.sun * std::max(0.0f, XMVectorGetX(XMVector3Dot(XMLoadFloat3(&r.normal), light))) / XM_PI;
            r.directSpecular = specPow + 0.15f * std::max(0.0f, 1.0f - r.hitDist / 16.0f);
            if (r.hitDist <= 0.0f) r.hitDist = 12.0f;
            r.radiance = {r.albD.x * r.directDiffuse + r.albS.x * r.directSpecular,
                          r.albD.y * r.directDiffuse + r.albS.y * r.directSpecular,
                          r.albD.z * r.directDiffuse + r.albS.z * r.directSpecular};
            return r;
        }
        r.radiance = {r.albD.x * (ndl * scene.sun + scene.ambient) + r.albS.x * specPow + 0.004f,
                      r.albD.y * (ndl * scene.sun + scene.ambient) + r.albS.y * specPow + 0.006f,
                      r.albD.z * (ndl * scene.sun + scene.ambient) + r.albS.z * specPow + 0.012f};
        return r;
    };

    auto HardwareDepth = [&](const Shaded& s) {
        if (cfg.infiniteFar)
            return cfg.nearPlane / std::max(1e-4f, s.viewDist);
        const XMVECTOR clip = XMVector4Transform(s.world, cam.viewProj);
        return XMVectorGetZ(clip) / std::max(1e-12f, XMVectorGetW(clip));
    };

    auto DeviceDepth = [&](const Shaded& s) {
        if (cfg.linearDepth)
        {
            const XMVECTOR v = XMVector3Transform(s.world, cam.view);
            return XMVectorGetZ(v); // signed view-space distance (RH: negative forward)
        }
        return HardwareDepth(s);
    };

    for (uint32_t y = 0; y < rh; ++y)
    {
        for (uint32_t x = 0; x < rw; ++x)
        {
            // Truth: the clean radiance at the un-jittered center sample (the chain's
            // reconstruction target; jitter is averaged out by RR/SR over frames).
            const Shaded center = Trace(float(x) + 0.5f, float(y) + 0.5f, rw, rh);
            const size_t ti = (size_t(y) * rw + x) * 4;
            out.truthRender[ti + 0] = center.radiance.x;
            out.truthRender[ti + 1] = center.radiance.y;
            out.truthRender[ti + 2] = center.radiance.z;
            out.truthRender[ti + 3] = 1.0f;

            const Shaded s = Trace(float(x) + 0.5f - cam.jitterX, float(y) + 0.5f - cam.jitterY, rw, rh);
            if (cfg.nativeDiffuse && (!s.hit || !center.hit))
                Die("native render ray missed bounded diagnostic geometry");
            out.truthJittered[ti + 0] = s.radiance.x;
            out.truthJittered[ti + 1] = s.radiance.y;
            out.truthJittered[ti + 2] = s.radiance.z;
            out.truthJittered[ti + 3] = 1.0f;
            // Multiplicative lognormal noise (approved positive unbiased estimator):
            // m = exp(sigma*Z - sigma^2/2) has E[m] = 1 exactly, so E[noisy] = clean
            // and every sample is strictly positive with no clamp. sigma is the
            // scene-specific RELATIVE amplitude; std(m) = sqrt(exp(sigma^2)-1).
            const float noiseMult = noisy ? expf(sigma * rng.Normal() - 0.5f * sigma * sigma) : 1.0f;
            const float specNoiseMult = (noisy && cfg.nativeSpecular) ? expf(sigma * rng.Normal() - 0.5f * sigma * sigma) : noiseMult;
            const float sampledDirectDiffuse = cfg.nativeDiffuse ? s.directDiffuse * noiseMult : 0.0f;
            const float sampledDirectSpecular = (cfg.nativeDiffuse && cfg.nativeSpecular) ? s.directSpecular * specNoiseMult : 0.0f;
            float noisyRgb[3];
            for (int c = 0; c < 3; ++c)
            {
                const float clean = (&s.radiance.x)[c];
                noisyRgb[c] = cfg.nativeDiffuse
                    ? ((&s.albD.x)[c] * sampledDirectDiffuse + (cfg.nativeSpecular ? (&s.albS.x)[c] * sampledDirectSpecular : 0.0f))
                    : clean * noiseMult;
            }

            float mvx = 0.0f, mvy = 0.0f, mvUvX = 0.0f, mvUvY = 0.0f, mvDepthDelta = 0.0f;
            if (s.hit && !cam.cut && (!cfg.nativeDiffuse || cfg.panStop))
            {
                XMVECTOR previousWorld = s.world;
                if (s.movingOccluder && frame > 0)
                {
                    const XMFLOAT3 previousCenter = scene.OccluderCenter(frame - 1);
                    previousWorld = XMVectorAdd(previousWorld, XMVectorSet(previousCenter.x - occPos.x,
                        previousCenter.y - occPos.y, previousCenter.z - occPos.z, 0.0f));
                }
                const XMVECTOR prevClip = XMVector4Transform(previousWorld, cam.prevViewProj);
                const float pw = std::max(1e-12f, XMVectorGetW(prevClip));
                const float pux = (XMVectorGetX(prevClip) / pw + 1.0f) * 0.5f;
                const float puy = (1.0f - XMVectorGetY(prevClip) / pw) * 0.5f;
                const float cux = (x + 0.5f - cam.jitterX) / float(rw);
                const float cuy = (y + 0.5f - cam.jitterY) / float(rh);
                mvUvX = pux - cux;
                mvUvY = puy - cuy;
                // Both endpoints are the same world sample; input MV excludes jitter.
                mvx = mvUvX * float(rw);
                mvy = mvUvY * float(rh);
                const float curZ = XMVectorGetZ(XMVector3Transform(s.world, cam.view));
                const float prvZ = XMVectorGetZ(XMVector3Transform(previousWorld, cam.prevView));
                mvDepthDelta = prvZ - curZ;
            }

            const size_t si = size_t(y + baseY) * texW + (x + baseX);
            uint16_t* colorPx = reinterpret_cast<uint16_t*>(out.color.data()) + si * 4;
            colorPx[0] = FloatToHalf(noisyRgb[0]);
            colorPx[1] = FloatToHalf(noisyRgb[1]);
            colorPx[2] = FloatToHalf(noisyRgb[2]);
            colorPx[3] = FloatToHalf(1.0f);
            reinterpret_cast<float*>(out.depth.data())[si] = DeviceDepth(s);
            if (!out.srHardwareDepth.empty())
                reinterpret_cast<float*>(out.srHardwareDepth.data())[size_t(y) * rw + x] = HardwareDepth(s);
            if (!cfg.displayResMV)
            {
                uint16_t* mvPx = reinterpret_cast<uint16_t*>(out.motion.data()) + si * 2;
                mvPx[0] = FloatToHalf(mvx);
                mvPx[1] = FloatToHalf(mvy);
            }
            if (cfg.nativeDiffuse)
            {
                const size_t ni = size_t(y) * rw + x;
                auto* signal = reinterpret_cast<uint16_t*>(out.nativeDiffuse.data()) + ni * 4;
                signal[0] = signal[1] = signal[2] = FloatToHalf(sampledDirectDiffuse);
                signal[3] = FloatToHalf(s.hit ? cfg.nativeSignalAlpha : -1.0f);
                if (cfg.nativeSpecular)
                {
                    auto* specSig = reinterpret_cast<uint16_t*>(out.nativeSpecular.data()) + ni * 4;
                    specSig[0] = specSig[1] = specSig[2] = FloatToHalf(sampledDirectSpecular);
                    specSig[3] = FloatToHalf(s.hit ? s.hitDist : -1.0f);
                }
                const float currentZ = s.hit ? XMVectorGetZ(XMVector3Transform(s.world, cam.view))
                    : (cfg.handed == Handed::RH ? -1.0f : 1.0f) * (cfg.farPlane + 1.0f);
                reinterpret_cast<float*>(out.nativeDepth.data())[ni] = currentZ;
                auto* motion = reinterpret_cast<uint16_t*>(out.nativeMotion.data()) + ni * 4;
                motion[0] = FloatToHalf(mvUvX);
                motion[1] = FloatToHalf(mvUvY);
                motion[2] = FloatToHalf(mvDepthDelta);
                motion[3] = FloatToHalf(cfg.panStop ? 1.0f : 0.0f);
                // SDK octahedral world normal, linear roughness, material ID 0.
                const float invL1 = 1.0f / (std::abs(s.normal.x) + std::abs(s.normal.y) + std::abs(s.normal.z));
                float nx = s.normal.x * invL1, ny = s.normal.y * invL1;
                if (s.normal.z < 0.0f)
                {
                    const float oldX = nx;
                    nx = (1.0f - std::abs(ny)) * (oldX < 0.0f ? -1.0f : 1.0f);
                    ny = (1.0f - std::abs(oldX)) * (ny < 0.0f ? -1.0f : 1.0f);
                }
                const uint32_t roughBits = uint32_t(std::round(std::clamp(s.rough, 0.0f, 1.0f) * 1023.0f));
                const uint32_t packedNormal = uint32_t(std::round((nx * 0.5f + 0.5f) * 1023.0f)) |
                    (uint32_t(std::round((ny * 0.5f + 0.5f) * 1023.0f)) << 10) | (roughBits << 20);
                memcpy(out.nativeNormals.data() + ni * 4, &packedNormal, sizeof packedNormal);
                if (!s.hit)
                {
                    // No surface: neutral oct normal, roughness 1, reserved material 3;
                    // alpha -1 deactivates the signal and depth is outside RR bounds.
                    const uint32_t inactiveNormal = 512u | (512u << 10) | (1023u << 20) | (3u << 30);
                    memcpy(out.nativeNormals.data() + ni * 4, &inactiveNormal, sizeof inactiveNormal);
                }
            }
            uint16_t* nPx = reinterpret_cast<uint16_t*>(out.normals.data()) + si * 4;
            nPx[0] = FloatToHalf(s.normal.x);
            nPx[1] = FloatToHalf(s.normal.y);
            nPx[2] = FloatToHalf(s.normal.z);
            nPx[3] = FloatToHalf(cfg.separateRoughness ? 1.0f : s.rough);
            if (cfg.separateRoughness)
                reinterpret_cast<float*>(out.roughness.data())[si] = s.rough;
            uint8_t* dPx = out.diffAlbedo.data() + si * 4;
            uint8_t* sPx = out.specAlbedo.data() + si * 4;
            for (int c = 0; c < 3; ++c)
            {
                dPx[c] = uint8_t(std::clamp((&s.albD.x)[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                sPx[c] = uint8_t(std::clamp((&s.albS.x)[c], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            dPx[3] = sPx[3] = 255;
            if (cfg.hitDistance)
            {
                reinterpret_cast<float*>(out.hitDistance.data())[si] = s.hitDist;
                reinterpret_cast<float*>(out.diffuseHitDistance.data())[si] = s.diffuseHitDist;
            }
        }
    }
    if (cfg.displayResMV)
    {
        // Display-resolution MVs need an independently sampled display plane,
        // not render-sized bytes uploaded into the larger display resource.
        for (uint32_t y = 0; y < motionH; ++y)
            for (uint32_t x = 0; x < motionW; ++x)
            {
                const float u = (float(x) + 0.5f) / float(motionW) - cam.jitterX / float(rw);
                const float v = (float(y) + 0.5f) / float(motionH) - cam.jitterY / float(rh);
                const Shaded s = Trace(u * motionW, v * motionH, motionW, motionH);
                float mvx = 0, mvy = 0;
                if (s.hit && !cam.cut)
                {
                    XMVECTOR previousWorld = s.world;
                    if (s.movingOccluder && frame > 0)
                    {
                        const XMFLOAT3 previousCenter = scene.OccluderCenter(frame - 1);
                        previousWorld = XMVectorAdd(previousWorld, XMVectorSet(previousCenter.x - occPos.x,
                            previousCenter.y - occPos.y, previousCenter.z - occPos.z, 0));
                    }
                    const XMVECTOR clip = XMVector4Transform(previousWorld, cam.prevViewProj);
                    const float w = std::max(1e-12f, XMVectorGetW(clip));
                    mvx = ((XMVectorGetX(clip) / w + 1) * 0.5f - u) * rw;
                    mvy = ((1 - XMVectorGetY(clip) / w) * 0.5f - v) * rh;
                }
                const size_t index = (size_t(y + baseY) * motionTexW + x + baseX) * 2;
                auto* destination = reinterpret_cast<uint16_t*>(out.motion.data()) + index;
                destination[0] = FloatToHalf(mvx); destination[1] = FloatToHalf(mvy);
            }
    }

    const uint32_t tw = cfg.TargetW(), th = cfg.TargetH();
    for (uint32_t y = 0; y < th; ++y)
        for (uint32_t x = 0; x < tw; ++x)
        {
            // Native target truth is a deterministic 4x4 spatial box integral
            // (16 analytic, noise-free rays per output pixel), never noisy bilinear.
            const uint32_t axisSamples = cfg.nativeDiffuse ? 4u : 1u;
            XMFLOAT3 sum{0, 0, 0};
            for (uint32_t sy = 0; sy < axisSamples; ++sy)
                for (uint32_t sx = 0; sx < axisSamples; ++sx)
                {
                    const Shaded s = Trace(float(x) + (float(sx) + 0.5f) / axisSamples,
                                           float(y) + (float(sy) + 0.5f) / axisSamples, tw, th);
                    if (cfg.nativeDiffuse && !s.hit) Die("native target truth ray missed bounded diagnostic geometry");
                    sum.x += s.radiance.x; sum.y += s.radiance.y; sum.z += s.radiance.z;
                }
            const float invSamples = 1.0f / float(axisSamples * axisSamples);
            const size_t i = (size_t(y) * tw + x) * 4;
            out.truthTarget[i + 0] = sum.x * invSamples;
            out.truthTarget[i + 1] = sum.y * invSamples;
            out.truthTarget[i + 2] = sum.z * invSamples;
            out.truthTarget[i + 3] = 1.0f;
        }
    return out;
}

// ---------------------------------------------------------------------------
// Metrics over read-back float planes.
// ---------------------------------------------------------------------------
struct PlaneStats
{
    double rmse = 0, bias = 0, energyRatio = 0, detailRatio = 0, psnr = 0, noiseStd = 0;
    uint64_t nonFinite = 0;
};

static double Luma4(const float* px) { return 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]; }

static PlaneStats ComparePlanes(const std::vector<float>& out, const std::vector<float>& truth, uint32_t w,
                                uint32_t h)
{
    PlaneStats st;
    const size_t n = size_t(w) * h;
    if (out.size() / 4 < n || truth.size() / 4 < n)
        Die("ComparePlanes extent mismatch");
    double se = 0, sgn = 0, sumOut = 0, sumTruth = 0, lumaSq = 0;
    std::vector<double> lo(n), lt(n);
    for (size_t i = 0; i < n; ++i)
    {
        for (uint32_t c = 0; c < 4; ++c)
        {
            const float v = out[i * 4 + c], t = truth[i * 4 + c];
            if (!std::isfinite(v) || !std::isfinite(t))
                ++st.nonFinite;
            if (c < 3) // Alpha is storage/provider data, not RGB radiance quality.
            {
                const double d = double(v) - double(t);
                se += d * d;
                sgn += d;
                sumOut += v;
                sumTruth += t;
            }
        }
        lo[i] = Luma4(&out[i * 4]);
        lt[i] = Luma4(&truth[i * 4]);
        const double d = lo[i] - lt[i];
        lumaSq += d * d;
    }
    st.rmse = sqrt(lumaSq / double(n)); // luminance truth-residual RMSE
    st.bias = sgn / double(n * 3);      // mean signed RGB radiance error
    st.energyRatio = sumTruth > 1e-12 ? sumOut / sumTruth : 0.0;
    st.noiseStd = sqrt(std::max(0.0, se / double(n * 3) - st.bias * st.bias));
    auto GradMean = [&](const std::vector<double>& l) { // detail contrast: mean gradient magnitude
        double sum = 0;
        uint64_t count = 0;
        for (uint32_t y = 1; y + 1 < h; ++y)
            for (uint32_t x = 1; x + 1 < w; ++x)
            {
                const size_t i = size_t(y) * w + x;
                const double gx = l[i + 1] - l[i - 1], gy = l[i + w] - l[i - w];
                sum += sqrt(gx * gx + gy * gy);
                ++count;
            }
        return count ? sum / double(count) : 0.0;
    };
    const double gTruth = GradMean(lt);
    st.detailRatio = gTruth > 1e-9 ? GradMean(lo) / gTruth : 0.0;
    double tp = 0;
    for (size_t i = 0; i < n; ++i)
        tp += lt[i] * lt[i];
    const double rms = sqrt(tp / double(n));
    st.psnr = st.rmse > 1e-12 ? 20.0 * log10(std::max(1e-12, rms) / st.rmse) : 999.0;
    return st;
}

static int MetricSelfTest()
{
    std::vector<float> truth(3 * 3 * 4), out(truth.size());
    for (size_t i = 0; i < 9; ++i)
    {
        for (size_t c = 0; c < 3; ++c)
        {
            truth[i * 4 + c] = 0.1f * float(i + c + 1);
            out[i * 4 + c] = truth[i * 4 + c] + 0.01f * float(int(i % 3) - 1);
        }
        truth[i * 4 + 3] = out[i * 4 + 3] = 1.0f;
    }
    const auto quality = [](const PlaneStats& st) {
        return std::array<double, 6>{st.rmse, st.bias, st.energyRatio, st.detailRatio, st.psnr, st.noiseStd};
    };
    const PlaneStats reference = ComparePlanes(out, truth, 3, 3);
    for (size_t i = 0; i < 9; ++i)
    {
        out[i * 4 + 3] = out[i * 4]; // Actual saved SR alpha can duplicate red.
        truth[i * 4 + 3] = float(i + 2);
    }
    const PlaneStats changedAlpha = ComparePlanes(out, truth, 3, 3);
    if (quality(reference) != quality(changedAlpha) || reference.nonFinite || changedAlpha.nonFinite)
        Die("metric self-test: finite alpha changed RGB quality");
    out[3] = std::numeric_limits<float>::quiet_NaN();
    truth[7] = std::numeric_limits<float>::infinity();
    const PlaneStats invalidAlpha = ComparePlanes(out, truth, 3, 3);
    if (quality(reference) != quality(invalidAlpha) || invalidAlpha.nonFinite != 2)
        Die("metric self-test: alpha finiteness was not checked independently of RGB quality");
    printf("metric self-test: PASS (RGB quality invariant; RGBA finiteness checked)\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Stage timing: GPU timestamps bracketing every real dispatch segment.
// ---------------------------------------------------------------------------
enum Stage
{
    StageUpload = 0,
    StageConversion,
    StageDenoiser,
    StageComposition,
    StageUpscale,
    StageReadback,
    StageCount
};
static const char* StageName(Stage s)
{
    static const char* names[] = {"upload", "conversion", "denoiser", "composition", "upscale", "readback"};
    return names[s];
}

struct StageTimer
{
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> resolve;
    uint64_t frequency = 0;
    uint32_t frameQueries = 0, queriesUsed = 0;

    void Init(ID3D12Device* dev, ID3D12CommandQueue* queue, uint32_t frames)
    {
        frameQueries = StageCount * 2;
        const uint32_t total = frames * frameQueries;
        D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, total};
        CHECK_HR(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&heap)), "CreateQueryHeap");
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = UINT64(total) * 8;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        CHECK_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&resolve)),
                 "timestamp buffer");
        CHECK_HR(queue->GetTimestampFrequency(&frequency), "GetTimestampFrequency");
        if (!frequency)
            Die("GPU timestamp frequency is zero");
        queriesUsed = 0;
    }
    void Begin(ID3D12GraphicsCommandList* cl, uint32_t frame, Stage s)
    {
        cl->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * frameQueries + uint32_t(s) * 2);
    }
    void End(ID3D12GraphicsCommandList* cl, uint32_t frame, Stage s)
    {
        const uint32_t idx = frame * frameQueries + uint32_t(s) * 2 + 1;
        cl->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx);
        queriesUsed = std::max(queriesUsed, idx + 1);
    }
    void Resolve(ID3D12GraphicsCommandList* cl)
    {
        cl->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, queriesUsed, resolve.Get(), 0);
    }
    std::vector<double> Read(uint32_t frames)
    {
        uint64_t* mapped = nullptr;
        CHECK_HR(resolve->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "timestamp map");
        std::vector<double> ms(size_t(frames) * StageCount, 0.0);
        for (uint32_t f = 0; f < frames; ++f)
            for (uint32_t s = 0; s < StageCount; ++s)
            {
                const uint64_t b = mapped[f * frameQueries + s * 2], a = mapped[f * frameQueries + s * 2 + 1];
                ms[f * StageCount + s] = a >= b ? double(a - b) * 1000.0 / double(frequency) : 0.0;
            }
        resolve->Unmap(0, nullptr);
        queriesUsed = 0; // next pass/scene overwrites the same indices
        return ms;
    }
};

// ---------------------------------------------------------------------------
// Artifacts
// ---------------------------------------------------------------------------
struct RunArtifacts
{
    fs::path outRoot;
    std::ofstream metrics, temporal, segments, controls;
    explicit RunArtifacts(const fs::path& root) : outRoot(root) {}
    void Open()
    {
        fs::create_directories(outRoot / "frames");
        metrics.open(outRoot / "metrics.csv");
        temporal.open(outRoot / "temporal.csv");
        segments.open(outRoot / "segments.csv");
        controls.open(outRoot / "dispatch_controls.bin", std::ios::binary);
        if (!metrics || !temporal || !segments || !controls)
            Die("cannot open output CSVs under " + outRoot.string());
        metrics << "scene,pass,frame,stage,rmse_luma,bias_mean,energy_ratio,detail_ratio,psnr_luma_db,nonfinite,"
                   "noise_std\n";
        temporal << "scene,pass,metric,value\n";
        segments << "scene,pass,frame,stage,gpu_ms\n";
    }
};

// Recorded verbatim per frame (binary). Hashes of this file are comparable across
// baseline/candidate runs by compare_runs.py: identical fixtures + identical applied
// controls means any image divergence belongs to the converter/provider closure.
#pragma pack(push, 1)
struct AppliedControls
{
    uint32_t frameIndex, flags, renderW, renderH;
    float mvScale[3], camDelta[3], jitter[2], depthBounds[2];
    float view[16], projection[16];
    uint32_t upscaleW, upscaleH;
    float srJitter[2], srNearFar[2];
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Converter/denoiser descriptor helpers (single source shared by serial and perf paths).
// ---------------------------------------------------------------------------
static XMMATRIX PrevViewOf(const FrameCamera& cam)
{
    return XMMatrixMultiply(cam.prevViewProj, XMMatrixInverse(nullptr, cam.proj));
}

struct FixtureTextures
{
    GpuTexture color, depth, motion, normals, roughness, diffAlb, specAlb, hitDist, exposure, srOut;
    GpuTexture diffuseHitDist;
    // Scene-owned SR inputs survive every submitted list and its fence wait.
    GpuTexture srMotion, srDepth;
};

struct InputBases
{
    uint32_t colorBaseX = 0, colorBaseY = 0;
    uint32_t depthBaseX = 0, depthBaseY = 0;
    uint32_t mvBaseX = 0, mvBaseY = 0;
    uint32_t normalsBaseX = 0, normalsBaseY = 0;
    uint32_t roughBaseX = 0, roughBaseY = 0;
    uint32_t albedoBaseX = 0, albedoBaseY = 0;
    uint32_t hitBaseX = 0, hitBaseY = 0;
};

static void FillConversionDesc(FSRDConversionDesc& conv, const SceneConfig& s, const FrameCamera& cam,
                               const FixtureTextures& t, const InputBases& b, bool hitDistBound,
                               bool historyValid, bool specularIndirect)
{
    conv = {};
    conv.Resources.InColor = t.color.tex.Get();
    conv.Resources.InDepth = t.depth.tex.Get();
    conv.Resources.InMotionVectors = t.motion.tex.Get();
    conv.Resources.InNormals = t.normals.tex.Get();
    conv.Resources.InRoughness = s.separateRoughness ? t.roughness.tex.Get() : nullptr;
    conv.Resources.InSpecHitDist = hitDistBound ? t.hitDist.tex.Get() : nullptr;
    conv.Resources.InDiffAlbedo = t.diffAlb.tex.Get();
    conv.Resources.InSpecAlbedo = t.specAlb.tex.Get();

    conv.RenderSize = XMFLOAT4(float(s.renderW), float(s.renderH), 1.0f / float(s.renderW), 1.0f / float(s.renderH));
    const uint32_t mw = s.displayResMV ? s.TargetW() : s.renderW;
    const uint32_t mh = s.displayResMV ? s.TargetH() : s.renderH;
    conv.MotionInputSize = XMFLOAT4(float(mw), float(mh), 1.0f / float(mw), 1.0f / float(mh));
    // MV scale 1 (render pixels); conversion normalizes by the render extent, matching
    // the production MotionTransform construction.
    conv.MotionTransform = XMFLOAT4(1.0f / float(s.renderW), 1.0f / float(s.renderH), 0.0f, 0.0f);
    conv.JitterOffsets = XMFLOAT4(cam.jitterX, cam.jitterY, cam.prevJitterX, cam.prevJitterY);
    conv.FloorSourceBase = XMUINT4{b.colorBaseX, b.colorBaseY, b.depthBaseX, b.depthBaseY};
    conv.InputBase0 = XMUINT4{b.colorBaseX, b.colorBaseY, b.mvBaseX, b.mvBaseY};
    conv.InputBase1 = XMUINT4{b.normalsBaseX, b.normalsBaseY,
                              s.separateRoughness ? b.roughBaseX : 0u, s.separateRoughness ? b.roughBaseY : 0u};
    conv.InputBase2 = XMUINT4{hitDistBound ? b.hitBaseX : 0u, hitDistBound ? b.hitBaseY : 0u,
                              b.albedoBaseX, b.albedoBaseY};
    conv.InputBase3 = XMUINT4{b.albedoBaseX, b.albedoBaseY, 0u, 0u};
    conv.InputBase4 = XMUINT4{0u, 0u, 0u, 0u};
    conv.NearPlane = s.nearPlane;
    conv.FarPlane = s.infiniteFar ? 65504.0f : s.farPlane;
    ApplyFrozenProductionConversionDefaults(conv);

    uint32_t flags = 0;
    if (s.handed == Handed::RH)
        flags |= uint32_t(FSRDConvFlags::RightHanded);
    if (s.linearDepth)
        flags |= uint32_t(FSRDConvFlags::IsDepthLinear);
    if (!s.separateRoughness)
        flags |= uint32_t(FSRDConvFlags::IsRoughnessPacked);
    if (hitDistBound)
        flags |= uint32_t(FSRDConvFlags::HasSpecHitDistance);
    if (specularIndirect)
        flags |= uint32_t(FSRDConvFlags::SpecularSignalIndirect);
    conv.Flags = flags;
    conv.MotionHistoryValid = historyValid;
    conv.MotionVectorsJittered = false;
    conv.DisplayResolutionMotion = s.displayResMV;
    conv.DiagnosticsEnabled = false; // diagnostics off for both correctness and timing
    conv.BiasMaskStrength = 1.0f;
    // HLSL default column-major memory interprets CPU row-vector matrices as
    // their column-vector transpose, exactly what mul(matrix, vector) needs.
    XMStoreFloat4x4(&conv.InvViewMatrix, cam.invView);
    XMStoreFloat4x4(&conv.InvProjMatrix, XMMatrixInverse(nullptr, cam.proj));
    XMStoreFloat4x4(&conv.PrevViewMatrix, PrevViewOf(cam));
}

static void FillDenoiserDesc(ffxDispatchDescDenoiser& dd, ID3D12GraphicsCommandList* cl, const SceneConfig& s,
                             const FrameCamera& cam, const FSRDConversionDesc& conv, bool reset, uint32_t frame)
{
    dd.commandList = cl;
    dd.motionVectorScale = {1.0f, 1.0f, 1.0f}; // conversion already normalized source motion
    dd.cameraPositionDelta = {cam.camDelta.x, cam.camDelta.y, cam.camDelta.z};
    XMFLOAT4X4 tmp;
    XMStoreFloat4x4(&tmp, cam.view); // RR row-major row-vector layout
    memcpy(&dd.view, &tmp, sizeof(tmp));
    XMStoreFloat4x4(&tmp, cam.proj);
    memcpy(&dd.projection, &tmp, sizeof(tmp));
    dd.linearDepthBounds = {conv.NearPlane, conv.FarPlane};
    dd.renderSize = {s.renderW, s.renderH};
    dd.frameIndex = frame;
    dd.flags = FFX_DENOISER_DISPATCH_NON_ENCODED_ALBEDO | (reset ? FFX_DENOISER_DISPATCH_RESET : 0u);
    dd.jitterOffsets = {cam.jitterX, cam.jitterY};
}

struct UpscaleInputs
{
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
};

// SR has no per-input origin fields. Copy the exact logical rectangle, preserving
// motion values/units and restoring the RR input's advertised shader-read state.
static ID3D12Resource* LocalizeUpscaleInput(ID3D12Device* device, ID3D12GraphicsCommandList* cl,
                                          GpuTexture& source, GpuTexture& local, uint32_t w, uint32_t h,
                                          uint32_t baseX, uint32_t baseY, const wchar_t* name)
{
    if (uint64_t(baseX) + w > source.width || uint64_t(baseY) + h > source.height)
        Die("SR input rectangle exceeds the authored resource extent");
    if (baseX == 0 && baseY == 0 && source.width == w && source.height == h)
        return source.tex.Get();
    if (!local.tex)
        local.Init(device, source.format, w, h, false, false, name);
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = {source.tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             source.state, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition = {local.tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             local.state, D3D12_RESOURCE_STATE_COPY_DEST};
    cl->ResourceBarrier(local.state == D3D12_RESOURCE_STATE_COPY_DEST ? 1 : 2, barriers);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = source.tex.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = local.tex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX region{baseX, baseY, 0, baseX + w, baseY + h, 1};
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &region);
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter = source.state;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = kRead;
    cl->ResourceBarrier(2, barriers);
    local.state = kRead;
    return local.tex.Get();
}

static UpscaleInputs PrepareUpscaleInputs(ID3D12Device* device, ID3D12GraphicsCommandList* cl,
                                          const SceneConfig& s, FixtureTextures& t, const InputBases& b,
                                          ID3D12Resource* encodedDepth)
{
    UpscaleInputs inputs;
    const uint32_t mw = s.displayResMV ? s.TargetW() : s.renderW;
    const uint32_t mh = s.displayResMV ? s.TargetH() : s.renderH;
    inputs.motion = LocalizeUpscaleInput(device, cl, t.motion, t.srMotion, mw, mh,
                                        b.mvBaseX, b.mvBaseY, L"sr_local_motion");
    if (encodedDepth)
        inputs.depth = encodedDepth;
    else if (s.linearDepth)
    {
        // Only analytic fixtures can author this independent hardware plane.
        if (!t.srDepth.tex)
            Die("SR requires hardware depth; no renderer hardware-depth plane is available");
        inputs.depth = t.srDepth.tex.Get();
    }
    else
        inputs.depth = LocalizeUpscaleInput(device, cl, t.depth, t.srDepth, s.renderW, s.renderH,
                                           b.depthBaseX, b.depthBaseY, L"sr_local_hardware_depth");
    return inputs;
}

static void FillUpscaleDesc(ffxDispatchDescUpscale& sr, ID3D12GraphicsCommandList* cl, const SceneConfig& s,
                            const FrameCamera& cam, const FixtureTextures& t, ID3D12Resource* composed,
                            bool useExposureTexture, bool reset, const UpscaleInputs& inputs)
{
    sr = {};
    sr.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    sr.commandList = cl;
    sr.color = ffxApiGetResourceDX12(composed, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    sr.depth = ffxApiGetResourceDX12(inputs.depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    sr.motionVectors = ffxApiGetResourceDX12(inputs.motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    if (useExposureTexture)
        sr.exposure = ffxApiGetResourceDX12(t.exposure.tex.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    sr.output = ffxApiGetResourceDX12(t.srOut.tex.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    sr.jitterOffset = {cam.jitterX, cam.jitterY};
    sr.motionVectorScale = {1.0f, 1.0f};
    sr.renderSize = {s.renderW, s.renderH};
    sr.upscaleSize = {s.TargetW(), s.TargetH()};
    sr.enableSharpening = false;
    sr.frameTimeDelta = 16.6667f;
    sr.preExposure = 1.0f;
    sr.reset = reset;
    sr.cameraNear = s.nearPlane;
    // SDK contract (setupDeviceDepthToViewSpaceDepthParams + parent DerivedRRCamera
    // work): an infinite far plane is declared with FLT_MAX, never 0.
    sr.cameraFar = s.infiniteFar ? FLT_MAX : s.farPlane;
    sr.cameraFovAngleVertical = s.vfovRad;
    sr.viewSpaceToMetersFactor = 0.0f;
}

// ---------------------------------------------------------------------------
// Captured-frame replay (--fixture-dir): consumes the producer capture schema
// (Godot raster engine chain). capture.json carries scene flags and provenance
// (unknown keys are preserved verbatim into run.json); frames/frameNNNN holds the
// DLSS-RR-shaped planes plus frame.json with the per-frame camera contract.
// Producer matrices are row-major serialized COLUMN-vector matrices (translation
// at indices 3,7,11), matching the production converter's column-vector contract.
// Serialization order must not be confused with DirectX row-vector algebra.
// ---------------------------------------------------------------------------
using CaptureJson = nlohmann::json;

static double JsonNumber(const CaptureJson& j, const char* key, double fallback = 0)
{
    const auto it = j.find(key);
    if (it == j.end())
        return fallback;
    if (!it->is_number())
        Die(std::string("capture json: expected number for ") + key);
    const double value = it->get<double>();
    if (!std::isfinite(value))
        Die(std::string("capture json: non-finite number for ") + key);
    return value;
}

static bool JsonBool(const CaptureJson& j, const char* key, bool fallback = false)
{
    const auto it = j.find(key);
    if (it == j.end())
        return fallback;
    if (!it->is_boolean())
        Die(std::string("capture json: expected boolean for ") + key);
    return it->get<bool>();
}

static std::string JsonString(const CaptureJson& j, const char* key, const std::string& fallback = {})
{
    const auto it = j.find(key);
    if (it == j.end())
        return fallback;
    if (!it->is_string())
        Die(std::string("capture json: expected string for ") + key);
    return it->get<std::string>();
}

static std::vector<double> JsonArray(const CaptureJson& j, const char* key, size_t expect)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != expect)
        Die(std::string("capture json: array ") + key + " missing or wrong length");
    std::vector<double> values;
    values.reserve(expect);
    for (const auto& item : *it)
    {
        if (!item.is_number())
            Die(std::string("capture json: non-numeric array element in ") + key);
        const double value = item.get<double>();
        if (!std::isfinite(value))
            Die(std::string("capture json: non-finite array element in ") + key);
        values.push_back(value);
    }
    return values;
}

static uint32_t CaptureUint(const CaptureJson& j, const char* key, uint32_t minimum = 0,
                            uint32_t maximum = UINT32_MAX)
{
    if (!j.contains(key))
        Die(std::string("capture json: missing integer ") + key);
    const double value = JsonNumber(j, key);
    if (value < minimum || value > maximum || value != std::floor(value))
        Die(std::string("capture json: invalid integer ") + key);
    return uint32_t(value);
}

static float CaptureVerticalFov(const CaptureJson& frame, double projectionY)
{
    const double derived = 2.0 * std::atan(1.0 / std::abs(projectionY));
    const double declared = JsonNumber(frame, "verticalFovRadians", derived);
    if (!(declared > 0.0 && declared < XM_PI) || std::abs(declared - derived) > 1e-3)
        Die("capture vertical FOV does not match its projection (radians required)");
    return float(declared);
}

static CaptureJson LoadJsonFile(const fs::path& path)
{
    std::ifstream f(path);
    if (!f)
        Die("cannot read " + path.string());
    CaptureJson j;
    try
    {
        f >> j;
    }
    catch (const nlohmann::json::exception& error)
    {
        Die("invalid capture json " + path.string() + ": " + error.what());
    }
    if (!j.is_object())
        Die("capture json must be an object: " + path.string());
    return j;
}

// PFM reader (PF/PF4/Pf, little endian via negative scale), rows flipped to top-down.
static std::vector<float> ReadPfm(const fs::path& path, uint32_t& w, uint32_t& h, uint32_t& channels)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        Die("cannot read " + path.string());
    std::string magic;
    f >> magic;
    if (magic != "PF" && magic != "PF4" && magic != "Pf")
        Die("not a PFM: " + path.string());
    channels = magic == "Pf" ? 1 : magic == "PF" ? 3 : 4;
    float scale = 0;
    f >> w >> h >> scale;
    if (w == 0 || h == 0 || scale >= 0.0f)
        Die("unsupported PFM header: " + path.string());
    f.get();
    std::vector<float> rows(size_t(w) * h * channels);
    f.read(reinterpret_cast<char*>(rows.data()), std::streamsize(rows.size() * sizeof(float)));
    if (!f)
        Die("truncated PFM: " + path.string());
    std::vector<float> topDown(rows.size());
    for (uint32_t y = 0; y < h; ++y)
        memcpy(&topDown[size_t(y) * w * channels], &rows[size_t(h - 1 - y) * w * channels],
               size_t(w) * channels * sizeof(float));
    return topDown;
}

static std::vector<uint8_t> LoadRawFile(const fs::path& path, size_t expectBytes)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        Die("missing capture plane: " + path.string());
    if (f.tellg() != std::streamoff(expectBytes))
        Die("capture plane byte count mismatch: " + path.string());
    f.seekg(0);
    std::vector<uint8_t> data(expectBytes);
    f.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    if (!f || static_cast<size_t>(f.gcount()) != expectBytes)
        Die("capture plane byte count mismatch: " + path.string());
    return data;
}

struct CaptureConfig
{
    SceneConfig scene;
    std::string provenance; // capture.json verbatim, recorded into run.json
    std::string sceneLabel;
    CaptureJson frameMetadata = CaptureJson::array();
};

struct FrameInput
{
    FrameCamera cam;
    FixtureFrame fx;
    InputBases bases;
    SceneConfig camera;
    XMMATRIX previousView {};
    double deltaSeconds = 1.0 / 60.0;
    float preExposure = 1.0f;
    std::string provenance;
    bool hasTruthRender = true, hasTruthTarget = true;
};

static CaptureConfig LoadCaptureConfig(const fs::path& dir)
{
    const auto j = LoadJsonFile(dir / "capture.json");
    CaptureConfig c;
    c.sceneLabel = JsonString(j, "scene", "captured");
    c.scene.name = "captured_engine_chain";
    c.scene.renderW = CaptureUint(j, "renderWidth", 1, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
    c.scene.renderH = CaptureUint(j, "renderHeight", 1, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
    c.scene.explicitTargetW = CaptureUint(j, "targetWidth", 1, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
    c.scene.explicitTargetH = CaptureUint(j, "targetHeight", 1, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
    if (!j.contains("provenance") || !j["provenance"].is_object() ||
        JsonString(j["provenance"], "inputClass") != "clean-raster-engine-chain-not-noisy-RT")
        Die("capture input class/provenance does not describe the supported Godot engine chain");
    c.scene.handed = JsonBool(j, "rightHanded") ? Handed::RH : Handed::LH;
    c.scene.reversedZ = JsonBool(j, "reversedZ");
    c.scene.infiniteFar = JsonBool(j, "infiniteFar");
    c.scene.linearDepth = !JsonBool(j, "hardwareDepth", true);
    c.scene.hdr = JsonBool(j, "hdr");
    c.scene.separateRoughness = JsonString(j, "roughnessMode", "packed") == "separate";
    c.scene.displayResMV = JsonBool(j, "mvDisplayRes");
    c.scene.orbit = false;
    c.scene.noiseSigma = 0.0f; // engine chain is its own input class; no synthetic noise
    // Camera bounds live in frame.json, not capture.json.
    const auto firstFrame = LoadJsonFile(dir / "frames" / "frame0000" / "frame.json");
    c.scene.nearPlane = float(JsonNumber(firstFrame, "nearPlane"));
    c.scene.farPlane = float(JsonNumber(firstFrame, "farPlane"));
    const auto projection = JsonArray(firstFrame, "proj", 16);
    c.scene.vfovRad = CaptureVerticalFov(firstFrame, projection[5]);
    if (!(c.scene.nearPlane > 0.0f && c.scene.farPlane > c.scene.nearPlane &&
          c.scene.vfovRad > 0.0f && c.scene.vfovRad < XM_PI))
        Die("capture camera bounds or projection are invalid");
    c.scene.frames = CaptureUint(j, "frames", 4);
    c.scene.exposureRamp = fs::exists(dir / "frames" / "frame0000" / "exposure.raw");
    c.scene.hitDistance = fs::exists(dir / "frames" / "frame0000" / "hitDistance.raw");
    std::ifstream prov(dir / "capture.json");
    std::stringstream ss;
    ss << prov.rdbuf();
    c.provenance = ss.str();
    return c;
}

static FrameInput LoadCaptureFrame(const fs::path& dir, const CaptureConfig& cfg, uint32_t frame)
{
    char idx[16];
    snprintf(idx, sizeof idx, "frame%04u", frame);
    const fs::path fdir = dir / "frames" / idx;
    const auto j = LoadJsonFile(fdir / "frame.json");
    FrameInput in;
    in.camera = cfg.scene;
    in.camera.nearPlane = float(JsonNumber(j, "nearPlane"));
    in.camera.farPlane = float(JsonNumber(j, "farPlane"));
    in.deltaSeconds = JsonNumber(j, "deltaSeconds");
    in.preExposure = float(JsonNumber(j, "preExposure"));
    if (!(in.camera.nearPlane > 0.0f && in.camera.farPlane > in.camera.nearPlane &&
          in.deltaSeconds > 0.0 && in.preExposure > 0.0f))
        Die("capture per-frame camera/exposure/time contract is invalid");
    if (!j.contains("engineLuminanceMultiplier") || JsonNumber(j, "engineLuminanceMultiplier") <= 0.0)
        Die("capture engine luminance storage multiplier is missing or invalid");
    in.provenance = j.dump();
    if (CaptureUint(j, "frameId") != frame)
        Die("capture frameId does not match its directory index");

    auto LoadMatrix = [&](const char* key) {
        const std::vector<double> d = JsonArray(j, key, 16);
        XMFLOAT4X4 f;
        for (int r = 0; r < 4; ++r)
            for (int c2 = 0; c2 < 4; ++c2)
                f.m[r][c2] = float(d[size_t(r) * 4 + c2]);
        return XMLoadFloat4x4(&f);
    };
    // Capture matrices serialize column-vector transforms; normalize once to
    // the CPU row-vector convention used throughout this runner.
    in.cam.view = XMMatrixTranspose(LoadMatrix("view"));
    in.cam.invView = XMMatrixTranspose(LoadMatrix("invView"));
    in.cam.proj = XMMatrixTranspose(LoadMatrix("proj"));
    in.previousView = XMMatrixTranspose(LoadMatrix("prevView"));
    in.camera.vfovRad = CaptureVerticalFov(j, in.cam.proj.r[1].m128_f32[1]);
    const XMMATRIX previousProjection = XMMatrixTranspose(LoadMatrix("prevProj"));
    in.cam.prevViewProj = XMMatrixMultiply(in.previousView, previousProjection);
    const std::vector<double> delta = JsonArray(j, "cameraPositionDelta", 3);
    in.cam.camDelta = {float(delta[0]), float(delta[1]), float(delta[2])};
    in.cam.jitterX = float(JsonNumber(j, "jitterX"));
    in.cam.jitterY = float(JsonNumber(j, "jitterY"));
    in.cam.prevJitterX = float(JsonNumber(j, "prevJitterX", in.cam.jitterX));
    in.cam.prevJitterY = float(JsonNumber(j, "prevJitterY", in.cam.jitterY));
    in.cam.cut = JsonBool(j, "reset");

    const auto ReadBase = [&](const char* key, uint32_t& x, uint32_t& y) {
        const auto base = JsonArray(j, key, 2);
        for (double value : base)
            if (value < 0.0 || value != std::floor(value) || value > UINT32_MAX)
                Die(std::string("capture subrect origin invalid: ") + key);
        x = uint32_t(base[0]);
        y = uint32_t(base[1]);
    };
    ReadBase("colorBase", in.bases.colorBaseX, in.bases.colorBaseY);
    ReadBase("depthBase", in.bases.depthBaseX, in.bases.depthBaseY);
    ReadBase("mvBase", in.bases.mvBaseX, in.bases.mvBaseY);
    ReadBase("normalsBase", in.bases.normalsBaseX, in.bases.normalsBaseY);
    ReadBase("albedoBase", in.bases.albedoBaseX, in.bases.albedoBaseY);
    ReadBase("hitDistBase", in.bases.hitBaseX, in.bases.hitBaseY);
    if (cfg.scene.separateRoughness)
        ReadBase("roughnessBase", in.bases.roughBaseX, in.bases.roughBaseY);
    // Current engine exporter writes full-size tightly packed planes and zero
    // origins. A non-zero origin cannot fit a render-sized plane; reject it
    // rather than issuing out-of-bounds shader reads.
    if (in.bases.colorBaseX || in.bases.colorBaseY || in.bases.depthBaseX || in.bases.depthBaseY ||
        in.bases.mvBaseX || in.bases.mvBaseY || in.bases.normalsBaseX || in.bases.normalsBaseY ||
        in.bases.roughBaseX || in.bases.roughBaseY || in.bases.albedoBaseX || in.bases.albedoBaseY ||
        in.bases.hitBaseX || in.bases.hitBaseY)
        Die("capture raw plane extents do not support non-zero subrect origins");

    const uint32_t w = cfg.scene.renderW, h = cfg.scene.renderH;
    const uint32_t mw = cfg.scene.displayResMV ? cfg.scene.TargetW() : w;
    const uint32_t mh = cfg.scene.displayResMV ? cfg.scene.TargetH() : h;
    FixtureFrame& fx = in.fx;
    fx.color = LoadRawFile(fdir / "color.raw", size_t(w) * h * 8);
    fx.depth = LoadRawFile(fdir / "depth.raw", size_t(w) * h * 4);
    fx.motion = LoadRawFile(fdir / "motion.raw", size_t(mw) * mh * 4);
    fx.normals = LoadRawFile(fdir / "normals.raw", size_t(w) * h * 8);
    if (cfg.scene.separateRoughness)
        fx.roughness = LoadRawFile(fdir / "roughness.raw", size_t(w) * h * 4);
    fx.diffAlbedo = LoadRawFile(fdir / "diffAlbedo.raw", size_t(w) * h * 4);
    fx.specAlbedo = LoadRawFile(fdir / "specAlbedo.raw", size_t(w) * h * 4);
    if (fs::exists(fdir / "hitDistance.raw"))
        fx.hitDistance = LoadRawFile(fdir / "hitDistance.raw", size_t(w) * h * 4);
    if (fs::exists(fdir / "exposure.raw"))
        fx.exposure = LoadRawFile(fdir / "exposure.raw", 4);
    if ((!fx.hitDistance.empty() && !cfg.scene.hitDistance) ||
        (!fx.exposure.empty() != cfg.scene.exposureRamp))
        Die("capture optional-plane layout changed after context/resource creation");
    if (!j.contains("resources") || !j["resources"].is_object())
        Die("capture frame lacks resource provenance");
    const char* required[] = {"color.raw", "depth.raw", "motion.raw", "normals.raw",
                              "diffAlbedo.raw", "specAlbedo.raw"};
    for (const char* name : required)
        if (!j["resources"].contains(name))
            Die(std::string("capture resource provenance missing: ") + name);
    if ((cfg.scene.exposureRamp && !j["resources"].contains("exposure.raw")) ||
        (cfg.scene.separateRoughness && !j["resources"].contains("roughness.raw")) ||
        (!fx.hitDistance.empty() && !j["resources"].contains("hitDistance.raw")))
        Die("capture optional-plane provenance missing");
    for (const auto& entry : j["resources"].items())
    {
        const fs::path name(entry.key());
        if (name.filename() != name || name.extension() != ".raw")
            Die("capture provenance contains a non-local raw resource name");
        const fs::path resource = fdir / name;
        const auto& identity = entry.value();
        if (!identity.contains("bytes") || !identity.contains("sha256") ||
            fs::file_size(resource) != identity["bytes"].get<uint64_t>() ||
            Sha256File(resource) != identity["sha256"].get<std::string>())
            Die("capture resource identity mismatch: " + resource.string());
    }

    in.hasTruthRender = false; // schema supplies target-resolution truth only
    char truthName[48];
    snprintf(truthName, sizeof truthName, "targetFrame%04u.pfm", frame);
    const fs::path truthPath = dir / "truth" / truthName;
    if (fs::exists(truthPath))
    {
        uint32_t tw = 0, th = 0, ch = 0;
        fx.truthTarget = ReadPfm(truthPath, tw, th, ch);
        if (tw != cfg.scene.TargetW() || th != cfg.scene.TargetH() || ch != 4)
            Die("capture truth extent mismatch: " + truthPath.string());
        in.hasTruthTarget = true;
    }
    else
        in.hasTruthTarget = false;
    return in;
}

// One-shot fence-completed texture readback through a private staging buffer and
// command list (serial correctness path; every reported image goes through here).
static std::vector<float> DownloadTexture(GpuContext& gpu, ID3D12Resource* res, D3D12_RESOURCE_STATES state,
                                          DXGI_FORMAT fmt, uint32_t w, uint32_t h, uint32_t baseX = 0, uint32_t baseY = 0)
{
    if (!res)
        Die("download requested a null stage resource");
    const D3D12_RESOURCE_DESC sourceDesc = res->GetDesc();
    if (uint64_t(baseX) + w > sourceDesc.Width || uint64_t(baseY) + h > sourceDesc.Height || sourceDesc.Format != fmt)
        Die("download stage format or logical extent does not match the resource");
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    CHECK_HR(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
             "download allocator");
    CHECK_HR(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                           IID_PPV_ARGS(&cl)),
             "download list");
    GpuTexture holder;
    holder.Init(gpu.device.Get(), fmt, w, h, true, true, L"download_once");
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, D3D12_RESOURCE_STATE_COPY_SOURCE};
    cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = holder.staging.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = holder.footprint;
    const D3D12_BOX region { baseX, baseY, 0, baseX + w, baseY + h, 1 };
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &region);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = state;
    cl->ResourceBarrier(1, &b);
    const HRESULT closeResult = cl->Close();
    if (FAILED(closeResult))
        gpu.DrainInfoQueue("failed stage readback recording");
    CHECK_HR(closeResult, "download close");
    static uint64_t s_value = 0;
    const uint64_t v = ++s_value;
    ID3D12CommandList* arr[] = {cl.Get()};
    gpu.queue->ExecuteCommandLists(1, arr);
    CHECK_HR(gpu.queue->Signal(gpu.utilFence.Get(), v), "download signal");
    gpu.WaitUtilFence(v);
    return holder.DownloadAsFloat(4);
}

// SDK native contract: signed view Z, UV/depth-delta motion, oct world normals;
// sample denoiser_compose.hlsl:155-159 remodulates DirectDiffuse with linear albedo.
struct NativeDiffuseRoute
{
    GpuTexture signal, specSignal, depth, normals, motion, output, specOutput, composed, srOutput;
    bool hasSpecular = false;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    static void Transition(ID3D12GraphicsCommandList* cl, GpuTexture& texture, D3D12_RESOURCE_STATES after)
    {
        if (texture.state == after) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {texture.tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, texture.state, after};
        cl->ResourceBarrier(1, &barrier); texture.state = after;
    }
    void Init(ID3D12Device* device, const SceneConfig& scene, ID3D12Resource* albedo, ID3D12Resource* specAlbedo = nullptr)
    {
        const auto w = scene.renderW, h = scene.renderH;
        hasSpecular = scene.nativeSpecular && specAlbedo != nullptr;
        signal.Init(device, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, false, true, L"native_direct_diffuse");
        if (hasSpecular)
        {
            specSignal.Init(device, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, false, true, L"native_indirect_specular");
            specOutput.Init(device, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, true, false, L"native_rr_spec_output");
        }
        depth.Init(device, DXGI_FORMAT_R32_FLOAT, w, h, false, true, L"native_signed_depth");
        normals.Init(device, DXGI_FORMAT_R10G10B10A2_UNORM, w, h, false, true, L"native_oct_normal_roughness");
        motion.Init(device, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, false, true, L"native_uv_depth_motion");
        output.Init(device, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, true, false, L"native_rr_output");
        composed.Init(device, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, true, false, L"native_remodulated");
        srOutput.Init(device, DXGI_FORMAT_R16G16B16A16_FLOAT, scene.TargetW(), scene.TargetH(), true, false, L"native_sr_output");
        const char* sourceDiff = R"(
Texture2D<float4> lighting : register(t0);
Texture2D<float4> albedo : register(t1);
RWTexture2D<float4> radiance : register(u0);
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID)
{
    uint w, h; radiance.GetDimensions(w, h);
    if (p.x < w && p.y < h) radiance[p.xy] = float4(lighting[p.xy].rgb * albedo[p.xy].rgb, 1);
})";
        const char* sourceSpec = R"(
Texture2D<float4> lighting : register(t0);
Texture2D<float4> albedo : register(t1);
Texture2D<float4> specLighting : register(t2);
Texture2D<float4> specAlbedo : register(t3);
RWTexture2D<float4> radiance : register(u0);
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID)
{
    uint w, h; radiance.GetDimensions(w, h);
    if (p.x < w && p.y < h) radiance[p.xy] = float4(lighting[p.xy].rgb * albedo[p.xy].rgb + specLighting[p.xy].rgb * specAlbedo[p.xy].rgb, 1);
})";
        const char* source = hasSpecular ? sourceSpec : sourceDiff;
        const uint32_t srvCount = hasSpecular ? 4u : 2u;
        ComPtr<ID3DBlob> shader, errors, signature;
        CHECK_HR(D3DCompile(source, std::strlen(source), "native_diffuse_remodulate", nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors), "compile native remodulation");
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srvCount, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, srvCount};
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {2, ranges};
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 1; desc.pParameters = &parameter;
        CHECK_HR(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors), "serialize native remodulation root");
        CHECK_HR(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root)), "native remodulation root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root.Get(); pso.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        CHECK_HR(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)), "native remodulation pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = srvCount + 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CHECK_HR(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "native remodulation heap");
        auto handle = heap->GetCPUDescriptorHandleForHeapStart();
        const auto step = device->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1; srv.Format = output.format;
        device->CreateShaderResourceView(output.tex.Get(), &srv, handle); handle.ptr += step;
        srv.Format = albedo->GetDesc().Format;
        device->CreateShaderResourceView(albedo, &srv, handle); handle.ptr += step;
        if (hasSpecular)
        {
            srv.Format = specOutput.format;
            device->CreateShaderResourceView(specOutput.tex.Get(), &srv, handle); handle.ptr += step;
            srv.Format = specAlbedo->GetDesc().Format;
            device->CreateShaderResourceView(specAlbedo, &srv, handle); handle.ptr += step;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; uav.Format = composed.format;
        device->CreateUnorderedAccessView(composed.tex.Get(), nullptr, &uav, handle);
    }
    void Fill(ffxDispatchDescDenoiser& dispatch, ffxDispatchDescDenoiserDirectDiffuse& diffuse, ffxDispatchDescDenoiserIndirectSpecular* spec = nullptr)
    {
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
        dispatch.header.pNext = &diffuse.header;
        dispatch.linearDepth = ffxApiGetResourceDX12(depth.tex.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatch.motionVectors = ffxApiGetResourceDX12(motion.tex.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatch.normals = ffxApiGetResourceDX12(normals.tex.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        diffuse.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;
        diffuse.header.pNext = (hasSpecular && spec) ? &spec->header : nullptr;
        diffuse.signal.input = ffxApiGetResourceDX12(signal.tex.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        diffuse.signal.output = ffxApiGetResourceDX12(output.tex.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        if (hasSpecular && spec)
        {
            spec->header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
            spec->header.pNext = nullptr;
            spec->signal.input = ffxApiGetResourceDX12(specSignal.tex.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
            spec->signal.output = ffxApiGetResourceDX12(specOutput.tex.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    void Compose(ID3D12GraphicsCommandList* cl)
    {
        Transition(cl, output, kRead);
        if (hasSpecular) Transition(cl, specOutput, kRead);
        Transition(cl, composed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        cl->SetDescriptorHeaps(1, heaps); cl->SetComputeRootSignature(root.Get()); cl->SetPipelineState(pipeline.Get());
        cl->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        cl->Dispatch((composed.width + 7) / 8, (composed.height + 7) / 8, 1);
        Transition(cl, composed, kRead);
    }
};

struct OpenRrRoute
{
    HMODULE module = nullptr;
    OnrRrApi5 api5{};
    void* ctx = nullptr;
    GpuTexture output;
    uint32_t activeBackend = ONR_RR_BACKEND_D3D12;
    std::string runtimeSha256, runtimeVersion, dlssdSha256, dlssdVersion;

    void Load(const fs::path& runtimePath, const fs::path& dlssdPath)
    {
        if (!fs::exists(runtimePath)) Die("OpenRR runtime missing: " + runtimePath.string());
        if (!fs::exists(dlssdPath)) Die("OpenRR dlssd missing: " + dlssdPath.string());
        runtimeSha256 = Sha256File(runtimePath); runtimeVersion = FileVersionOf(runtimePath);
        dlssdSha256 = Sha256File(dlssdPath); dlssdVersion = FileVersionOf(dlssdPath);
        module = LoadLibraryExW(runtimePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) Die("LoadLibraryExW failed for OpenNrRrRuntime.dll");
        auto getApi = reinterpret_cast<PFN_OnrRrGetApi>(GetProcAddress(module, "OnrRrGetApi"));
        if (!getApi || getApi(ONR_RR_ABI_VERSION_5, &api5.v4.v3.v2.v1) != ONR_RR_OK)
            Die("OpenNrRrRuntime.dll does not offer ABI 5");
    }
    uint32_t curRenderW = 0, curRenderH = 0, curOutW = 0, curOutH = 0;
    bool isReady = false;
    void CreateContext(ID3D12Device* dev, const SceneConfig& scene, const fs::path& runtimePath, const fs::path& dlssdPath, const std::string& backendStr)
    {
        output.Init(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, scene.TargetW(), scene.TargetH(), true, false, L"openrr_sr_output");
        if (ctx && curRenderW == scene.renderW && curRenderH == scene.renderH && curOutW == scene.TargetW() && curOutH == scene.TargetH())
            return;
        DestroyContext();
        curRenderW = scene.renderW; curRenderH = scene.renderH;
        curOutW = scene.TargetW(); curOutH = scene.TargetH();
        isReady = false;
        const std::wstring dlssdW = dlssdPath.wstring();
        const std::wstring dataDirW = (runtimePath.parent_path() / L"opennr-rr").wstring();
        OnrRrCreateInfo ci{};
        ci.size = sizeof(ci);
        ci.mode = ONR_RR_MODE_FAST;
        ci.d3d12_device = dev;
        ci.max_render_width = scene.renderW;
        ci.max_render_height = scene.renderH;
        ci.output_width = scene.TargetW();
        ci.output_height = scene.TargetH();
        ci.dlssd_path = dlssdW.c_str();
        ci.data_dir = dataDirW.c_str();
        ci.backend = backendStr == "vulkan" ? ONR_RR_BACKEND_CHOICE_VULKAN : ONR_RR_BACKEND_CHOICE_D3D12;
        if (api5.v4.v3.v2.v1.Create(&ci, &ctx) != ONR_RR_OK || !ctx)
            Die("OpenRR Create failed");
        if (api5.v4.GetBackend) api5.v4.GetBackend(ctx, &activeBackend);
    }
    void DestroyContext()
    {
        if (ctx && api5.v4.v3.v2.v1.Destroy)
        {
            api5.v4.v3.v2.v1.Destroy(ctx);
            ctx = nullptr;
        }
    }
    ~OpenRrRoute()
    {
        DestroyContext();
        if (module) FreeLibrary(module);
    }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
static int GuardedMain(int argc, char** argv)
{
    std::string mode = "serial";
    fs::path manifestPath, outDir, probeOut, closureLabel, fixtureDir, zeroReference;
    OfflineFeatures features;
    std::string sceneOverride;
    std::string nativeScene = "both", nativeAlpha = "0", nativeHistory = "normal";
    fs::path openrrRuntimePath, openrrDlssdPath;
    std::string openrrBackend = "d3d12";
    bool nativeControlsSpecified = false;
    std::vector<std::pair<std::string, std::string>> featureOverrides;
    const fs::path exeDir = fs::path(argv[0]).parent_path();
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
                Die("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--mode") mode = next();
        else if (arg == "--metric-self-test") return MetricSelfTest();
        else if (arg == "--manifest") manifestPath = next();
        else if (arg == "--out") outDir = next();
        else if (arg == "--probe") mode = "probe";
        else if (arg == "--manifest-out") probeOut = next();
        else if (arg == "--closure-label") closureLabel = next();
        else if (arg == "--fixture-dir") fixtureDir = next();
        else if (arg == "--scenes") sceneOverride = next();
        else if (arg == "--native-scene") { nativeScene = next(); nativeControlsSpecified = true; }
        else if (arg == "--native-signal-alpha") { nativeAlpha = next(); nativeControlsSpecified = true; }
        else if (arg == "--native-history") { nativeHistory = next(); nativeControlsSpecified = true; }
        else if (arg == "--openrr-runtime") openrrRuntimePath = next();
        else if (arg == "--openrr-dlssd") openrrDlssdPath = next();
        else if (arg == "--openrr-backend") openrrBackend = next();
        else if (arg == "--zero-reference") zeroReference = next();
        else if (arg == "--feature-case") features.Select(next());
        else if (arg == "--set")
        {
            const std::string setting = next(); const auto eq = setting.find('=');
            if (eq == std::string::npos) Die("--set requires key=value");
            featureOverrides.emplace_back(setting.substr(0, eq), setting.substr(eq + 1));
        }
        else if (arg == "--list-feature-cases")
        {
            for (const auto& name : OfflineFeatures::Cases()) printf("%s\n", name.c_str());
            return 0;
        }
        else Die("unknown argument " + arg);
    }
    const bool nativeComparison = mode == "native-diffuse";
    if (nativeControlsSpecified && !nativeComparison) Die("--native-* controls require --mode native-diffuse");
    if (nativeScene != "flat" && nativeScene != "detail" && nativeScene != "both" &&
        nativeScene != "motion_stop" && nativeScene != "specular" && nativeScene != "all")
        Die("--native-scene must be flat|detail|both|motion_stop|specular|all");
    const bool openrrEnabled = !openrrRuntimePath.empty() && !openrrDlssdPath.empty();
    if (nativeAlpha != "0" && nativeAlpha != "65504") Die("--native-signal-alpha must be 0|65504");
    if (nativeHistory != "normal" && nativeHistory != "reset-every-frame")
        Die("--native-history must be normal|reset-every-frame");
    if (nativeComparison) mode = "serial"; // reuse existing runner infrastructure
    if (mode != "serial" && mode != "perf" && mode != "probe")
        Die("--mode must be serial|perf|native-diffuse");
    if (!fixtureDir.empty() && mode != "serial")
        Die("--fixture-dir capture replay runs in serial mode only");
    if (outDir.empty())
        outDir = exeDir / "rr-offline-run";
    fs::create_directories(outDir);

    const DWORD selfPid = GetCurrentProcessId();
    if (mode != "probe" && OtherFullscreenWindowPresent(selfPid))
    {
        fprintf(stderr, "FAIL: another fullscreen application is present; refusing to run beside a game.\n");
        return 3;
    }

    // ---- probe: write a manifest template from the real environment, then fail ----
    if (mode == "probe")
    {
        ComPtr<IDXGIFactory6> factory;
        CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        uint64_t firstAmd = 0;
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter4> a;
            if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                           IID_PPV_ARGS(&a))))
                break;
            DXGI_ADAPTER_DESC3 d{};
            a->GetDesc3(&d);
            const uint64_t luid = uint64_t(d.AdapterLuid.HighPart) << 32 | uint32_t(d.AdapterLuid.LowPart);
            fprintf(stderr, "adapter[%u] vendor=0x%04X luid=0x%016llX %ls\n", i, d.VendorId,
                    (unsigned long long) luid, d.Description);
            if (!firstAmd && d.VendorId == 0x1002)
                firstAmd = luid;
        }
        if (!firstAmd)
            Die("no AMD adapter found; set adapter_luid to the reviewed hardware explicitly");
        const char* envSignedbin = getenv("RR_OFFLINE_SIGNEDBIN");
        const fs::path repoRoot = fs::canonical(exeDir / ".." / ".." / "..");
        const fs::path signedbin =
            envSignedbin ? fs::path(envSignedbin)
                         : repoRoot / "OptiScaler-DLSSNR-PreSR-Multipass-main" / "external" / "FidelityFX-SDK-v2" /
                               "Kits" / "FidelityFX" / "signedbin";
        const fs::path den121 = repoRoot / "third_party" / "fsr-rr-1.2.1" / "amd_fidelityfx_denoiser_dx12.dll";
        const fs::path den = (!envSignedbin && fs::exists(den121)) ? den121 : (signedbin / "amd_fidelityfx_denoiser_dx12.dll");
        const fs::path ups = signedbin / "amd_fidelityfx_upscaler_dx12.dll";
        const fs::path target = probeOut.empty() ? outDir / "manifest.ini" : probeOut;
        std::ofstream f(target);
        f << "# rr-offline manifest template. Review every value before running.\n"
          << "label=candidate\n"
          << "adapter_luid=0x" << std::hex << firstAmd << std::dec << "\n"
          << "adapter_vendor_id=0x1002\n"
          << "denoiser_dll=" << (fs::exists(den) ? den.string() : std::string("<REVIEW:denoiser path>")) << "\n"
          << "upscaler_dll=" << (fs::exists(ups) ? ups.string() : std::string("<REVIEW:upscaler path>")) << "\n"
          << "# denoiser_sha256=<optional pin>\n# upscaler_sha256=<optional pin>\n"
          << "diffuse_signal=direct\n"
          << "specular_signal=indirect\n"
          << "frames=24\nwarmup_frames=60\nmeasured_frames=300\nin_flight=2\n"
          << "seed=5964657725391699526\ntimeout_ms=30000\nvalidation=1\nreadback_every=4\n"
          << "dump_all_frames=0\n";
        f.close();
        if (!f)
            Die("cannot write manifest template " + target.string());
        fprintf(stderr,
                "manifest template written to %s\nreview it, then run: tests\\fsr-rr\\run.cmd gpu <out> --manifest "
                "<path>\n",
                target.string().c_str());
        return 1; // probe is never a PASS
    }

    if (manifestPath.empty())
        Die("--manifest is required (generate a reviewed template with --probe)");
    Manifest mf = LoadManifest(manifestPath);
    if (!sceneOverride.empty()) mf.scenes = sceneOverride;
    if (!mf.featureCase.empty() && !features.CandidateOnly()) features.Select(mf.featureCase);
    for (const auto& [key, value] : mf.featureSettings)
    {
        if (!features.CandidateOnly() && key != "capture") features.name = "custom";
        features.Set(key, value);
    }
    for (const auto& [key, value] : featureOverrides)
    {
        if (!features.CandidateOnly() && key != "capture") features.name = "custom";
        features.Set(key, value);
    }
    if (nativeComparison)
    {
        if (!fixtureDir.empty() || !sceneOverride.empty() || features.CandidateOnly() || !featureOverrides.empty() ||
            !mf.featureCase.empty() || !mf.featureSettings.empty() || !zeroReference.empty())
            Die("native-diffuse is a fixed analytic comparison; capture/scene/feature overrides are not supported");
        if (!HasOfflineFeatures<FSRDPreprocessor_Dx12>) Die("native-diffuse requires the current converter closure");
        if (!mf.warmupFrames || !mf.measuredFrames || mf.warmupFrames > UINT32_MAX - mf.measuredFrames)
            Die("native-diffuse requires positive warmup_frames and measured_frames without overflow");
        mf.frames = mf.warmupFrames + mf.measuredFrames;
        mf.scenes.clear(); mf.diffuseSignal = "direct"; mf.specularSignal = "direct";
        mf.readbackEvery = 1; mf.dumpAllFrames = true;
        features.name = "native-diffuse";
        features.FfxDenoiserDiffuseRoute = FSRDSignals::Direct;
        features.FfxDenoiserSpecularRoute = FSRDSignals::Indirect;
        features.FfxDenoiserDenoiseSpecular = (nativeScene == "specular");
        features.FfxDenoiserFloorEnabled = false;
        features.FfxDenoiserUnsupportedAlbedoRecovery = false;
    }
    if (features.CandidateOnly() && (mode != "serial" || !fixtureDir.empty()))
        Die("candidate matrix requires serial analytic fixtures");
    if (features.failureRecovery && mf.frames < 5) Die("failure-recovery requires at least five frames");
    if (!zeroReference.empty() && features.name != "additive-zero") Die("--zero-reference requires --feature-case additive-zero");
    if (!features.CandidateOnly())
    {
        features.FfxDenoiserDiffuseRoute = FSRDSignals::RouteFromCode(mf.diffuseSignal);
        features.FfxDenoiserSpecularRoute = FSRDSignals::RouteFromCode(mf.specularSignal);
    }
    OfflineEvidenceHash inputEvidence, inputControlEvidence, derivedControlEvidence, srRendererDepthEvidence;
    nlohmann::json featureControls = nlohmann::json::array();
    nlohmann::json captureEvidence = nlohmann::json::array();
    nlohmann::json srInputProvenance = nlohmann::json::array();
    const uint32_t perSceneFrames = mode == "perf" ? mf.warmupFrames + mf.measuredFrames : mf.frames;

    // ---- environment ----
    GpuContext gpu;
    gpu.Init(mf.adapterLuid, mf.vendorId, mf.requireVendor, mf.timeoutMs);
    fprintf(stderr, "debug_layer=%d adapter=%ls dedicatedMB=%llu\n", int(gpu.debugLayer), gpu.adapterDesc.Description,
            (unsigned long long)(gpu.adapterDesc.DedicatedVideoMemory >> 20));

    EffectDll denoiser, upscaler;
    denoiser.Load(mf.denoiserDll, mf.denoiserSha256);
    upscaler.Load(mf.upscalerDll, mf.upscalerSha256);
    OpenRrRoute openrrRoute;
    if (openrrEnabled) openrrRoute.Load(openrrRuntimePath, openrrDlssdPath);

    // Verbose SDK surface (upstream runner pattern); any SDK error fails the run.
    ffxConfigureDescGlobalDebug globalDebug = {};
    globalDebug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    globalDebug.header.pNext = nullptr;
    globalDebug.effectId = FFX_API_EFFECT_ID_DENOISER;
    globalDebug.fpMessage = FfxMessage;
    globalDebug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
    CHECK_FFX(denoiser.api.Configure(nullptr, &globalDebug.header), "global debug configure");

    // ---- scenes (analytic fixtures, or one scene from a producer capture) ----
    CaptureConfig capture;
    std::vector<SceneConfig> scenes;
    if (fixtureDir.empty())
        scenes = DefaultScenes(mf.frames);
    else
    {
        capture = LoadCaptureConfig(fixtureDir);
        scenes.push_back(capture.scene);
        fprintf(stderr, "capture replay: %s frames=%u render=%ux%u target=%ux%u\n", capture.sceneLabel.c_str(),
                capture.scene.frames, capture.scene.renderW, capture.scene.renderH, capture.scene.TargetW(),
                capture.scene.TargetH());
    }
    if (nativeComparison)
    {
        scenes.clear();
        SceneConfig diffuse;
        diffuse.name = "native_diffuse_flat";
        if (openrrEnabled) { diffuse.renderW = 512; diffuse.renderH = 256; }
        diffuse.nativeDiffuse = true; diffuse.orbit = false; diffuse.frames = mf.frames;
        diffuse.nearPlane = 0.1f; diffuse.farPlane = 32.0f;
        diffuse.nativeSignalAlpha = nativeAlpha == "65504" ? 65504.0f : 0.0f;
        diffuse.resetEveryFrame = nativeHistory == "reset-every-frame";
        if (nativeScene == "flat" || nativeScene == "both" || nativeScene == "all") scenes.push_back(diffuse);
        diffuse.name = "native_diffuse_detail"; diffuse.detailPattern = true;
        if (nativeScene == "detail" || nativeScene == "both" || nativeScene == "all") scenes.push_back(diffuse);
        if (nativeScene == "motion_stop" || nativeScene == "all")
        {
            SceneConfig ms = diffuse;
            ms.name = "native_diffuse_motion_stop";
            ms.detailPattern = true; ms.panStop = true;
            scenes.push_back(ms);
        }
        if (nativeScene == "specular" || nativeScene == "all")
        {
            SceneConfig sp = diffuse;
            sp.name = "native_specular_roughness";
            sp.detailPattern = true; sp.nativeSpecular = true; sp.hitDistance = true;
            scenes.push_back(sp);
        }
    }
    if (!mf.scenes.empty() && fixtureDir.empty())
    {
        std::vector<SceneConfig> filtered;
        std::stringstream ss(mf.scenes);
        std::string name;
        while (std::getline(ss, name, ','))
        {
            const std::string n = Trim(name);
            auto it = std::find_if(scenes.begin(), scenes.end(),
                                   [&](const SceneConfig& c) { return n == c.name; });
            if (it == scenes.end())
                Die("unknown scene filter entry: " + n);
            filtered.push_back(*it);
        }
        if (filtered.empty())
            Die("scene filter selected nothing");
        scenes = filtered;
    }
    if (features.CandidateOnly() && !nativeComparison)
        for (auto& s : scenes) s.hitDistance = true;

    // Allocation ceiling over every scene, mirroring the production converter contract
    // (aligned, with preset rounding headroom) so one RR context covers the run.
    constexpr uint32_t kAlign = 8;
    const auto ceilingFor = [](uint32_t extent) {
        const uint32_t padded = extent + std::max<uint32_t>(kAlign, extent / 20);
        return (padded + kAlign - 1) / kAlign * kAlign;
    };
    uint32_t maxRenderW = 0, maxRenderH = 0;
    for (const auto& s : scenes)
    {
        // The native pair has no dynamic resolution: both routes must have the
        // actual render-sized RR resources, not generic display-sized headroom.
        maxRenderW = nativeComparison ? std::max(maxRenderW, s.renderW)
            : std::max({maxRenderW, ceilingFor(s.renderW), ceilingFor(s.TargetW())});
        maxRenderH = nativeComparison ? std::max(maxRenderH, s.renderH)
            : std::max({maxRenderH, ceilingFor(s.renderH), ceilingFor(s.TargetH())});
    }

    if (!fixtureDir.empty() && (mf.diffuseSignal != "direct" || mf.specularSignal != "direct"))
        Die("Godot raster capture requires diffuse_signal=direct and specular_signal=direct; "
            "physical ray-hit lengths are absent and must not be fabricated");
    // ---- RR context (real provider, explicit selection) ----
    const auto signalFlagsFor = [](uint32_t mask) {
        return (mask & 1u ? uint32_t(FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE) : 0u) |
               (mask & 2u ? uint32_t(FFX_DENOISER_SIGNAL_DIRECT_SPECULAR) : 0u) |
               (mask & 4u ? uint32_t(FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE) : 0u) |
               (mask & 8u ? uint32_t(FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR) : 0u);
    };
    const uint32_t legacyMask = (mf.diffuseSignal == "direct" ? 1u : 4u) | (mf.specularSignal == "direct" ? 2u : 8u);
    FSRDSignals::Plan currentPlan = features.CandidateOnly()
        ? FSRDSignals::MakePlan(FSRDSignals::RequestFrom(features), false, false)
        : FSRDSignals::Plan{legacyMask};

    ffxCreateBackendDX12Desc rrBackend = {};
    rrBackend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    rrBackend.device = gpu.device.Get();
    ffxCreateContextDescDenoiser rrCreate = {};
    rrCreate.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    rrCreate.header.pNext = &rrBackend.header;
    rrCreate.version = FFX_DENOISER_VERSION;
    rrCreate.maxRenderSize = {maxRenderW, maxRenderH};
    rrCreate.signalFlags = signalFlagsFor(currentPlan.mask);
    rrCreate.checkerboardSignalFlags = FFX_DENOISER_SIGNAL_NONE;
    rrCreate.flags = (mode == "serial" && mf.validation) ? uint32_t(FFX_DENOISER_ENABLE_VALIDATION) : 0u;
    FfxApiEffectMemoryUsage rrPredictedMem {};
    bool rrPredictedMemQueried = false;
    {
        ffxQueryDescDenoiserGetGPUMemoryUsage memQuery = {};
        memQuery.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE;
        memQuery.device = gpu.device.Get();
        memQuery.maxRenderSize = rrCreate.maxRenderSize;
        memQuery.signalFlags = rrCreate.signalFlags;
        memQuery.checkerboardSignalFlags = rrCreate.checkerboardSignalFlags;
        memQuery.flags = rrCreate.flags;
        memQuery.gpuMemoryUsage = &rrPredictedMem;
        rrPredictedMemQueried = denoiser.api.Query(nullptr, &memQuery.header) == FFX_API_RETURN_OK;
    }
    ffxContext rrCtx = nullptr;
    CHECK_FFX(denoiser.api.CreateContext(&rrCtx, &rrCreate.header, nullptr), "RR CreateContext");
    FfxApiEffectMemoryUsage rrActualMem {};
    bool rrActualMemQueried = false;
    {
        ffxQueryDescDenoiserGetGPUMemoryUsage memQuery = {};
        memQuery.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE;
        memQuery.device = gpu.device.Get();
        memQuery.maxRenderSize = rrCreate.maxRenderSize;
        memQuery.signalFlags = rrCreate.signalFlags;
        memQuery.checkerboardSignalFlags = rrCreate.checkerboardSignalFlags;
        memQuery.flags = rrCreate.flags;
        memQuery.gpuMemoryUsage = &rrActualMem;
        rrActualMemQueried = denoiser.api.Query(&rrCtx, &memQuery.header) == FFX_API_RETURN_OK;
    }
    ffxContext nativeRrCtx = nullptr;
    if (nativeComparison)
        CHECK_FFX(denoiser.api.CreateContext(&nativeRrCtx, &rrCreate.header, nullptr), "native RR CreateContext");

    // Provider defaults queried from the live context and recorded verbatim: a
    // baseline/candidate comparison catches silent provider-default drift.
    struct DenoiserDefault
    {
        uint64_t key;
        const char* name;
        float value = 0.0f;
        bool queried = false;
    };
    DenoiserDefault defaults[] = {
        {FFX_API_CONFIGURE_DENOISER_KEY_DISOCCLUSION_THRESHOLD, "disocclusionThreshold"},
        {FFX_API_CONFIGURE_DENOISER_KEY_CROSS_BILATERAL_NORMAL_STRENGTH, "crossBilateralNormalStrength"},
        {FFX_API_CONFIGURE_DENOISER_KEY_STABILITY_BIAS, "stabilityBias"},
        {FFX_API_CONFIGURE_DENOISER_KEY_MAX_RADIANCE, "maxRadiance"},
        {FFX_API_CONFIGURE_DENOISER_KEY_RADIANCE_CLIP_STD_K, "radianceClipStdK"},
        {FFX_API_CONFIGURE_DENOISER_KEY_GAUSSIAN_KERNEL_RELAXATION, "gaussianKernelRelaxation"},
    };
    for (auto& d : defaults)
    {
        ffxQueryDescDenoiserGetDefaultKeyValue q = {};
        q.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE;
        q.key = d.key;
        q.count = 1;
        q.data = &d.value;
        d.queried = denoiser.api.Query(&rrCtx, &q.header) == FFX_API_RETURN_OK;
        if (nativeComparison)
        {
            if (!d.queried) Die("native comparison cannot pin an unqueryable RR setting");
            ffxConfigureDescDenoiserKeyValue setting{};
            setting.header.type = FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
            setting.key = d.key; setting.count = 1; setting.data = &d.value;
            CHECK_FFX(denoiser.api.Configure(&rrCtx, &setting.header), "translated RR pin setting");
            CHECK_FFX(denoiser.api.Configure(&nativeRrCtx, &setting.header), "native RR pin setting");
        }
    }

    // ---- production converter ----
    FSRDPreprocessor_Dx12 converter("RR Offline Converter", gpu.device.Get());
    if (!converter.IsInit())
        Die("production FSRDPreprocessor_Dx12 failed to initialize");
    ConfigureOfflineFeatures(converter, features);
    if (!converter.SetMaxRenderSize(maxRenderW, maxRenderH))
        Die("converter SetMaxRenderSize failed");
    if (!fixtureDir.empty() && capture.scene.linearDepth && !HasUpscalerDepth_v<FSRDPreprocessor_Dx12>)
        Die("capture publishes linear depth; SR requires the converter-encoded hardware depth, which this "
            "closure does not provide (run the candidate closure)");
    std::unique_ptr<FSRDPreprocessor_Dx12> pairedDefault;
    size_t pairedZeroReadbacks = 0;
    if (features.name == "additive-zero")
    {
        pairedDefault = std::make_unique<FSRDPreprocessor_Dx12>("RR Offline Default-Additive Pair", gpu.device.Get());
        if (!pairedDefault->IsInit()) Die("paired default converter initialization failed");
        ConfigureOfflineFeatures(*pairedDefault, features);
        if (!pairedDefault->SetMaxRenderSize(maxRenderW, maxRenderH)) Die("paired default converter allocation failed");
    }
    const auto selectFramePlan = [&](bool specularGuide, bool diffuseGuide) {
        const auto plan = features.CandidateOnly()
            ? FSRDSignals::MakePlan(FSRDSignals::RequestFrom(features), specularGuide, diffuseGuide)
            : FSRDSignals::Plan{legacyMask};
        const bool changed = !(plan == currentPlan);
        if (plan.mask != currentPlan.mask)
        {
            CHECK_FFX(denoiser.api.DestroyContext(&rrCtx, nullptr), "RR matrix DestroyContext");
            rrCreate.signalFlags = signalFlagsFor(plan.mask);
            CHECK_FFX(denoiser.api.CreateContext(&rrCtx, &rrCreate.header, nullptr), "RR matrix CreateContext");
        }
        currentPlan = plan;
        return changed;
    };

    // ---- frame infrastructure ----
    const uint32_t ringSize = mode == "perf" ? mf.inFlight : 1;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators(ringSize);
    std::vector<ComPtr<DlssNr::Submission::CommandListProxy>> lists(ringSize);
    for (uint32_t i = 0; i < ringSize; ++i)
    {
        CHECK_HR(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])),
                 "CreateCommandAllocator");
        ComPtr<ID3D12GraphicsCommandList> native;
        CHECK_HR(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
                                               IID_PPV_ARGS(&native)), "CreateCommandList");
        DlssNr::Submission::CommandListProxy* proxy = nullptr;
        CHECK_HR(DlssNr::Submission::CommandListProxy::Create(gpu.device.Get(), allocators[i].Get(),
                 native.Get(), &proxy), "CommandListProxy::Create");
        lists[i].Attach(proxy);
        CHECK_HR(lists[i]->Close(), "initial close");
    }
    ComPtr<ID3D12Fence> frameFence;
    CHECK_HR(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frameFence)), "frame fence");
    const HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent)
        Die("CreateEventW failed");

    auto& tracker = DlssNr::Submission::GpuBatch::Tracker::Instance();
    uint64_t fenceValue = 0;
    std::vector<uint64_t> slotFence(ringSize, 0);

    auto WaitFence = [&](uint64_t value) {
        if (frameFence->GetCompletedValue() >= value)
            return;
        CHECK_HR(frameFence->SetEventOnCompletion(value, fenceEvent), "SetEventOnCompletion");
        if (WaitForSingleObject(fenceEvent, mf.timeoutMs) != WAIT_OBJECT_0)
            Die("GPU frame fence timeout after " + std::to_string(mf.timeoutMs) + " ms");
        gpu.CheckAlive();
    };
    auto Execute = [&](uint32_t slot, ID3D12GraphicsCommandList* cl) {
        fenceValue += 1;
        const uint64_t value = fenceValue;
        ID3D12CommandList* arr[] = {cl};
        CHECK_HR(lists[slot]->ExecuteOn(gpu.queue.Get()), "CommandListProxy::ExecuteOn");
        // The hook sees the logical proxy identity after native execution.
        tracker.OnExecuted(1, arr, gpu.queue.Get());
        CHECK_HR(gpu.queue->Signal(frameFence.Get(), value), "queue Signal");
        slotFence[slot] = value;
        return value;
    };

    RunArtifacts art(outDir);
    art.Open();
    bool anyFailure = false;
    uint64_t trackerRecorded = 0, trackerRetired = 0;

    auto WriteSegments = [&](const std::vector<double>& ms, const SceneConfig& s, const char* pass,
                             uint32_t frameCount) {
        for (uint32_t f = 0; f < frameCount; ++f)
            for (uint32_t st = 0; st < StageCount; ++st)
                art.segments << s.name << ',' << pass << ',' << f << ',' << StageName(Stage(st)) << ','
                             << ms[f * StageCount + st] << "\n";
    };

    for (const SceneConfig& sceneCfg : scenes)
    {
        if (OtherFullscreenWindowPresent(selfPid))
        {
            fprintf(stderr, "FAIL: fullscreen application appeared mid-run; aborting (game left running).\n");
            return 3;
        }
        AnalyticScene scene;
        scene.Build(sceneCfg);

        // ---- SR context per scene (context flags legitimately vary) ----
        uint32_t srFlags = 0;
        if (sceneCfg.hdr)
            srFlags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
        if (sceneCfg.displayResMV)
            srFlags |= FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
        if (sceneCfg.reversedZ)
            srFlags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        if (sceneCfg.infiniteFar)
            srFlags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
        const bool useExposureTexture = sceneCfg.exposureRamp;
        if (!useExposureTexture)
            srFlags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;

        ffxCreateBackendDX12Desc srBackend = {};
        srBackend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        srBackend.device = gpu.device.Get();
        // Mirrors the production FSR31Feature context chain (sub-id 0x0b version desc).
        struct UpscaleVersionDesc
        {
            ffxCreateContextDescHeader header;
            uint32_t version;
        };
        UpscaleVersionDesc versionDesc = {};
        versionDesc.header.type = FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x0b);
        versionDesc.header.pNext = &srBackend.header;
        versionDesc.version = FFX_UPSCALER_VERSION;
        ffxCreateContextDescUpscale srCreate = {};
        srCreate.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        srCreate.header.pNext = &versionDesc.header;
        srCreate.flags = srFlags;
        srCreate.maxRenderSize = {ceilingFor(sceneCfg.renderW), ceilingFor(sceneCfg.renderH)};
        srCreate.maxUpscaleSize = {ceilingFor(sceneCfg.TargetW()), ceilingFor(sceneCfg.TargetH())};
        if (nativeComparison)
        {
            srCreate.maxRenderSize = {sceneCfg.renderW, sceneCfg.renderH};
            srCreate.maxUpscaleSize = {sceneCfg.TargetW(), sceneCfg.TargetH()};
        }
        srCreate.fpMessage = FfxMessage;
        ffxContext srCtx = nullptr;
        CHECK_FFX(upscaler.api.CreateContext(&srCtx, &srCreate.header, nullptr), "SR CreateContext");
        ffxContext nativeSrCtx = nullptr;
        if (nativeComparison)
            CHECK_FFX(upscaler.api.CreateContext(&nativeSrCtx, &srCreate.header, nullptr), "native SR CreateContext");

        // ---- fixture textures (subrect scenes use padded resources + real origins) ----
        const uint32_t pad = sceneCfg.subrect ? 64u : 0u, padY = sceneCfg.subrect ? 48u : 0u;
        const uint32_t baseX = sceneCfg.subrect ? 32u : 0u, baseY = sceneCfg.subrect ? 24u : 0u;
        const uint32_t texW = sceneCfg.renderW + pad, texH = sceneCfg.renderH + padY;
        const uint32_t mvTexW = sceneCfg.displayResMV ? sceneCfg.TargetW() + pad : texW;
        const uint32_t mvTexH = sceneCfg.displayResMV ? sceneCfg.TargetH() + padY : texH;
        const bool capturedScene = !fixtureDir.empty();
        const uint32_t frameCount = capturedScene ? capture.scene.frames : mf.frames;

        FixtureTextures t;
        t.color.Init(gpu.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, texW, texH, false, true, L"fx_color", ringSize);
        t.depth.Init(gpu.device.Get(), DXGI_FORMAT_R32_FLOAT, texW, texH, false, true, L"fx_depth", ringSize);
        t.motion.Init(gpu.device.Get(), DXGI_FORMAT_R16G16_FLOAT, mvTexW, mvTexH, false, true, L"fx_motion", ringSize);
        if (!capturedScene && sceneCfg.linearDepth && !HasUpscalerDepth_v<FSRDPreprocessor_Dx12>)
            t.srDepth.Init(gpu.device.Get(), DXGI_FORMAT_R32_FLOAT, sceneCfg.renderW, sceneCfg.renderH,
                           false, true, L"sr_renderer_hardware_depth", ringSize);
        t.normals.Init(gpu.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, texW, texH, false, true, L"fx_normals", ringSize);
        if (sceneCfg.separateRoughness)
            t.roughness.Init(gpu.device.Get(), DXGI_FORMAT_R32_FLOAT, texW, texH, false, true, L"fx_roughness", ringSize);
        t.diffAlb.Init(gpu.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, texW, texH, false, true, L"fx_diff_alb", ringSize);
        t.specAlb.Init(gpu.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, texW, texH, false, true, L"fx_spec_alb", ringSize);
        if (sceneCfg.hitDistance)
            t.hitDist.Init(gpu.device.Get(), DXGI_FORMAT_R32_FLOAT, texW, texH, false, true, L"fx_hitdist", ringSize);
        if (features.CandidateOnly() && !nativeComparison)
            t.diffuseHitDist.Init(gpu.device.Get(), DXGI_FORMAT_R32_FLOAT, texW, texH, false, true, L"fx_diffuse_hitdist", ringSize);
        if (useExposureTexture)
            t.exposure.Init(gpu.device.Get(), DXGI_FORMAT_R32_FLOAT, 1, 1, false, true, L"fx_exposure", ringSize);
        t.srOut.Init(gpu.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, sceneCfg.TargetW(), sceneCfg.TargetH(),
                     true, true, L"fx_sr_out");
        NativeDiffuseRoute nativeRoute;
        if (nativeComparison) nativeRoute.Init(gpu.device.Get(), sceneCfg, t.diffAlb.tex.Get(), sceneCfg.nativeSpecular ? t.specAlb.tex.Get() : nullptr);
        if (nativeComparison)
        {
            features.FfxDenoiserDenoiseSpecular = sceneCfg.nativeSpecular;
            features.FfxDenoiserAdditiveLightSplit = sceneCfg.nativeSpecular ? 1.0f : 0.0f;
            const uint32_t wantedMask = sceneCfg.nativeSpecular ? 9u : 1u;
            if (currentPlan.mask != wantedMask)
            {
                currentPlan = FSRDSignals::Plan{wantedMask};
                CHECK_FFX(denoiser.api.DestroyContext(&rrCtx, nullptr), "RR DestroyContext");
                CHECK_FFX(denoiser.api.DestroyContext(&nativeRrCtx, nullptr), "native RR DestroyContext");
                rrCreate.signalFlags = signalFlagsFor(currentPlan.mask);
                CHECK_FFX(denoiser.api.CreateContext(&rrCtx, &rrCreate.header, nullptr), "RR CreateContext");
                CHECK_FFX(denoiser.api.CreateContext(&nativeRrCtx, &rrCreate.header, nullptr), "native RR CreateContext");
                for (auto& d : defaults)
                {
                    ffxConfigureDescDenoiserKeyValue setting{};
                    setting.header.type = FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
                    setting.key = d.key; setting.count = 1; setting.data = &d.value;
                    CHECK_FFX(denoiser.api.Configure(&rrCtx, &setting.header), "translated RR pin setting");
                    CHECK_FFX(denoiser.api.Configure(&nativeRrCtx, &setting.header), "native RR pin setting");
                }
            }
        }
        if (openrrEnabled) openrrRoute.CreateContext(gpu.device.Get(), sceneCfg, openrrRuntimePath, openrrDlssdPath, openrrBackend);
        const uint32_t motionW = sceneCfg.displayResMV ? sceneCfg.TargetW() : sceneCfg.renderW;
        const uint32_t motionH = sceneCfg.displayResMV ? sceneCfg.TargetH() : sceneCfg.renderH;
        srInputProvenance.push_back({{"scene", sceneCfg.name},
            {"motion_source_origin", {baseX, baseY}}, {"motion_extent", {motionW, motionH}},
            {"motion_route", pad || padY ? "GPU CopyTextureRegion exact rectangle to zero-origin texture" : "authored zero-origin texture"},
            {"motion_units", "authored unjittered render-pixel deltas; no rescaling during localization"},
            {"depth_source", nativeComparison ? "shared-renderer-hardware-depth" :
                (HasUpscalerDepth_v<FSRDPreprocessor_Dx12> ? "production-encoded-hardware-depth" :
                    (sceneCfg.linearDepth ? "independent-renderer-world-position-projection" :
                        (capturedScene ? "capture-authored-hardware-depth" : "fixture-authored-hardware-depth")))},
            {"depth_extent", {sceneCfg.renderW, sceneCfg.renderH}},
            {"depth_origin", {0, 0}},
            {"renderer_depth_sampling", !capturedScene && sceneCfg.linearDepth && !HasUpscalerDepth_v<FSRDPreprocessor_Dx12>
                ? "same jittered world sample as RR; clip Z/W from renderer camera projection (near/view-distance for reversed infinite); no converter-derived reconstruction"
                : "no separate renderer hardware-depth plane"},
            {"rr_depth_input", sceneCfg.linearDepth ? "authored signed view-Z unchanged" : "authored hardware depth unchanged"},
            {"lifetime", "scene-owned SR localization textures; reused in queue order; released after scene fence completion"}});

        uint32_t activeUploadSlot = 0;
        auto CopyIn = [&](ID3D12GraphicsCommandList* cl, GpuTexture& tex, const std::vector<uint8_t>& data) {
            tex.Upload(data, activeUploadSlot);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = tex.Staging(activeUploadSlot);
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = tex.footprint;
            dst.pResource = tex.tex.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            if (tex.state != D3D12_RESOURCE_STATE_COPY_DEST)
            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition = {tex.tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, tex.state,
                                D3D12_RESOURCE_STATE_COPY_DEST};
                cl->ResourceBarrier(1, &b);
                tex.state = D3D12_RESOURCE_STATE_COPY_DEST;
            }
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {tex.tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_COPY_DEST, kRead};
            cl->ResourceBarrier(1, &b);
            tex.state = kRead;
        };

        // ================= serial correctness: clean + noisy passes =================
        if (mode == "serial")
        {
            StageTimer timer;
            timer.Init(gpu.device.Get(), gpu.queue.Get(), frameCount);
            StageTimer nativeTimer;
            if (nativeComparison) nativeTimer.Init(gpu.device.Get(), gpu.queue.Get(), frameCount);
            StageTimer openrrTimer;
            if (openrrEnabled) openrrTimer.Init(gpu.device.Get(), gpu.queue.Get(), frameCount);
            const int passCount = capturedScene ? 1 : 2; // captures are their own input class
            for (int pass = 0; pass < passCount; ++pass)
            {
                const bool noisy = pass == 1;
                const float sigma = noisy ? sceneCfg.noiseSigma : 0.0f;
                const char* passName = noisy ? "noisy" : "clean";
                bool hasHistory = false;
                struct ReadFrame
                {
                    uint32_t frameId = 0;
                    std::vector<float> presr, sr, nativeSr, openrrSr;
                    std::vector<float> truthR, truthT;
                    bool hasTruth = true;
                };
                std::vector<ReadFrame> reads;
                const uint32_t dumpFrames[] = {0, sceneCfg.revealFrame, sceneCfg.revealFrame + 2,
                                               frameCount - 1, frameCount / 2};

                for (uint32_t frame = 0; frame < frameCount; ++frame)
                {
                    bool reset = frame == 0 || sceneCfg.resetEveryFrame ||
                                 std::find(sceneCfg.resetFrames.begin(), sceneCfg.resetFrames.end(),
                                           frame) != sceneCfg.resetFrames.end();
                    FrameInput input;
                    if (!capturedScene)
                    {
                        input.cam = BuildCamera(sceneCfg, frame);
                        input.fx = RenderFixture(scene, input.cam, frame, mf.seed, texW, texH, baseX, baseY,
                                                 sigma, noisy);
                        input.bases.colorBaseX = baseX;
                        input.bases.colorBaseY = baseY;
                        input.bases.depthBaseX = baseX;
                        input.bases.depthBaseY = baseY;
                        input.bases.mvBaseX = baseX;
                        input.bases.mvBaseY = baseY;
                        input.bases.normalsBaseX = baseX;
                        input.bases.normalsBaseY = baseY;
                        input.bases.roughBaseX = baseX;
                        input.bases.roughBaseY = baseY;
                        input.bases.albedoBaseX = baseX;
                        input.bases.albedoBaseY = baseY;
                        input.bases.hitBaseX = baseX;
                        input.bases.hitBaseY = baseY;
                    }
                    else
                    {
                        input = LoadCaptureFrame(fixtureDir, capture, frame);
                        reset = frame == 0 || input.cam.cut;
                        capture.frameMetadata.push_back(CaptureJson::parse(input.provenance));
                    }
                    const FrameCamera& cam = input.cam;
                    const FixtureFrame& fx = input.fx;
                    const SceneConfig& dispatchScene = capturedScene ? input.camera : sceneCfg;
                    inputEvidence.Plane(fx.color); inputEvidence.Plane(fx.depth); inputEvidence.Plane(fx.motion);
                    inputEvidence.Plane(fx.normals); inputEvidence.Plane(fx.roughness); inputEvidence.Plane(fx.diffAlbedo);
                    inputEvidence.Plane(fx.specAlbedo); inputEvidence.Plane(fx.hitDistance); inputEvidence.Plane(fx.exposure);
                    inputEvidence.Plane(fx.diffuseHitDistance);
                    if (!fx.srHardwareDepth.empty()) srRendererDepthEvidence.Plane(fx.srHardwareDepth);
                    if (nativeComparison)
                    {
                        inputEvidence.Plane(fx.nativeDiffuse); inputEvidence.Plane(fx.nativeDepth);
                        inputEvidence.Plane(fx.nativeNormals); inputEvidence.Plane(fx.nativeMotion);
                    }
                    inputEvidence.Plane(fx.truthRender); inputEvidence.Plane(fx.truthJittered); inputEvidence.Plane(fx.truthTarget);
                    XMFLOAT4X4 rawView{}, rawProjection{}, rawPrevious{};
                    XMStoreFloat4x4(&rawView, cam.view); XMStoreFloat4x4(&rawProjection, cam.proj);
                    XMStoreFloat4x4(&rawPrevious, cam.prevViewProj);
                    inputEvidence.Add(&rawView, sizeof rawView); inputEvidence.Add(&rawProjection, sizeof rawProjection);
                    inputEvidence.Add(&rawPrevious, sizeof rawPrevious);
                    const float rawCamera[] = {cam.jitterX, cam.jitterY, cam.prevJitterX, cam.prevJitterY,
                        cam.camDelta.x, cam.camDelta.y, cam.camDelta.z, dispatchScene.nearPlane,
                        dispatchScene.farPlane, dispatchScene.vfovRad, input.preExposure, float(input.deltaSeconds)};
                    inputEvidence.Add(rawCamera, sizeof rawCamera);

                    CHECK_HR(allocators[0]->Reset(), "allocator Reset");
                    ID3D12GraphicsCommandList* cl = lists[0].Get();
                    CHECK_HR(cl->Reset(allocators[0].Get(), nullptr), "list Reset");
                    const uint64_t ticket = tracker.Record(cl); // production lifetime stamp
                    ++trackerRecorded;

                    // -- upload --
                    timer.Begin(cl, frame, StageUpload);
                    CopyIn(cl, t.color, fx.color);
                    CopyIn(cl, t.depth, fx.depth);
                    CopyIn(cl, t.motion, fx.motion);
                    if (!fx.srHardwareDepth.empty()) CopyIn(cl, t.srDepth, fx.srHardwareDepth);
                    CopyIn(cl, t.normals, fx.normals);
                    CopyIn(cl, t.diffAlb, fx.diffAlbedo);
                    CopyIn(cl, t.specAlb, fx.specAlbedo);
                    if (sceneCfg.separateRoughness)
                        CopyIn(cl, t.roughness, fx.roughness);
                    if (useExposureTexture)
                        CopyIn(cl, t.exposure, fx.exposure);
                    // Guide dropout: hit distance present for frames 0..7, absent after
                    // (flags follow the resources bound this frame, per product contract).
                    const bool featureGuideBound = features.guides == "both" || features.guides == "specular" ||
                        (features.guides == "dropout" && frame < frameCount / 2) ||
                        (features.guides == "arrival" && frame >= frameCount / 2);
                    const bool hitDistBound = (nativeComparison && sceneCfg.nativeSpecular) ? true :
                        (features.CandidateOnly() ? featureGuideBound :
                        (capturedScene ? !fx.hitDistance.empty() : (sceneCfg.hitDistance && frame < 8)));
                    const bool diffuseGuideBound = features.CandidateOnly() && featureGuideBound && features.guides != "specular";
                    if (selectFramePlan(hitDistBound, diffuseGuideBound)) reset = true;
                    const uint32_t feedableMask = FSRDSignals::Feedable(currentPlan,
                        features.FfxDenoiserEstimateHitDistances.value_or_default(), hitDistBound, diffuseGuideBound);
                    if (features.CandidateOnly() && (currentPlan.mask & ~feedableMask))
                        Die("production signal plan contains an unfeedable indirect descriptor");
                    if (features.failureRecovery && (frame == 2 || frame == 3 || frame == 4)) reset = true;
                    if (reset) InvalidateCompositionHistoryIfPresent(converter);
                    if (hitDistBound)
                        CopyIn(cl, t.hitDist, fx.hitDistance);
                    if (diffuseGuideBound) CopyIn(cl, t.diffuseHitDist, fx.diffuseHitDistance);
                    timer.End(cl, frame, StageUpload);
                    if (nativeComparison)
                    {
                        nativeTimer.Begin(cl, frame, StageUpload);
                        CopyIn(cl, nativeRoute.signal, fx.nativeDiffuse);
                        if (sceneCfg.nativeSpecular) CopyIn(cl, nativeRoute.specSignal, fx.nativeSpecular);
                        CopyIn(cl, nativeRoute.depth, fx.nativeDepth);
                        CopyIn(cl, nativeRoute.normals, fx.nativeNormals); CopyIn(cl, nativeRoute.motion, fx.nativeMotion);
                        nativeTimer.End(cl, frame, StageUpload);
                        nativeTimer.Begin(cl, frame, StageConversion); nativeTimer.End(cl, frame, StageConversion);
                    }

                    // -- conversion (production FSRDPreprocessor_Dx12) --
                    timer.Begin(cl, frame, StageConversion);
                    FSRDConversionDesc conv;
                    FillConversionDesc(conv, dispatchScene, cam, t, input.bases, hitDistBound, hasHistory && !reset,
                                       (currentPlan.mask & 8u) != 0);
                    if (capturedScene)
                        XMStoreFloat4x4(&conv.PrevViewMatrix, input.previousView);
                    SetFrameIndexIfPresent(conv, frame);
                    ApplyOfflineConversion(conv, features, currentPlan, diffuseGuideBound, t.diffuseHitDist.tex.Get(),
                                           input.bases.hitBaseX, input.bases.hitBaseY);
                    if (features.failureRecovery && frame == 2) ExerciseOfflineFailure(converter, cl, conv);
                    // Baseline instrumentation targets the requested checker/Floor ROI;
                    // clamp its origin to preserve a full 16x16 ROI in smaller fixtures.
                    if (!RequestOfflineCapture(converter, features.capture && pass == 1 && frame == 0,
                        features.CandidateOnly() ? 0u : std::min(128u, sceneCfg.renderW > 16 ? sceneCfg.renderW - 16 : 0u),
                        features.CandidateOnly() ? 0u : std::min(40u, sceneCfg.renderH > 16 ? sceneCfg.renderH - 16 : 0u), 16u))
                        Die("production additive capture request rejected");
                    if (!converter.DispatchConversion(cl, conv))
                        Die(std::string("DispatchConversion failed: scene=") + sceneCfg.name +
                            " frame=" + std::to_string(frame));
                    if (pairedDefault)
                    {
                        if (reset) InvalidateCompositionHistoryIfPresent(*pairedDefault);
                        auto defaultConversion = OfflineDefaultAdditiveDescriptor(conv);
                        if (!pairedDefault->DispatchConversion(cl, defaultConversion)) Die("paired default production conversion failed");
                    }
                    ID3D12Resource* encodedSrDepth =
                        PrepareUpscalerDepthIfPresent(converter, cl, dispatchScene.nearPlane,
                                                      dispatchScene.infiniteFar ? FLT_MAX : dispatchScene.farPlane,
                                                      dispatchScene.infiniteFar, dispatchScene.reversedZ);
                    if constexpr (HasUpscalerDepth_v<FSRDPreprocessor_Dx12>)
                    {
                        // Encoded depth is the mandatory production SR path on this
                        // closure: a null return after a successful conversion is a
                        // failure, never a silent fallback.
                        if (encodedSrDepth == nullptr)
                            Die("PrepareUpscalerDepthInput returned null after a successful conversion");
                    }
                    const bool bypass = features.failureRecovery && frame == 3;
                    timer.End(cl, frame, StageConversion);

                    // -- denoiser (real RR 1.2, sorted typed-signal chain) --
                    timer.Begin(cl, frame, StageDenoiser);
                    ffxDispatchDescDenoiser dd = {};
                    OfflineSignalChain signalChain;
                    signalChain.Fill(converter, dd, features, currentPlan);
                    FillDenoiserDesc(dd, cl, dispatchScene, cam, conv, reset, frame);
                    if (!bypass) CHECK_FFX(denoiser.api.Dispatch(&rrCtx, &dd.header), "RR Dispatch");
                    OfflineSignalChain pairedSignalChain;
                    if (pairedDefault)
                    {
                        ffxDispatchDescDenoiser pairedDispatch{};
                        pairedSignalChain.Fill(*pairedDefault, pairedDispatch, features, currentPlan);
                        CopyOfflineRrOutputs(cl, converter, *pairedDefault);
                    }
                    timer.End(cl, frame, StageDenoiser);
                    if (nativeComparison)
                    {
                        nativeTimer.Begin(cl, frame, StageDenoiser);
                        NativeDiffuseRoute::Transition(cl, nativeRoute.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        if (sceneCfg.nativeSpecular)
                            NativeDiffuseRoute::Transition(cl, nativeRoute.specOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        ffxDispatchDescDenoiser nativeDispatch{};
                        ffxDispatchDescDenoiserDirectDiffuse nativeDiffuse{};
                        ffxDispatchDescDenoiserIndirectSpecular nativeSpec{};
                        nativeRoute.Fill(nativeDispatch, nativeDiffuse, sceneCfg.nativeSpecular ? &nativeSpec : nullptr);
                        nativeDispatch.diffuseAlbedo = ffxApiGetResourceDX12(t.diffAlb.tex.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
                        if (sceneCfg.nativeSpecular)
                            nativeDispatch.specularAlbedo = ffxApiGetResourceDX12(t.specAlb.tex.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
                        FillDenoiserDesc(nativeDispatch, cl, dispatchScene, cam, conv, reset, frame);
                        CHECK_FFX(denoiser.api.Dispatch(&nativeRrCtx, &nativeDispatch.header), "native RR dispatch");
                        nativeTimer.End(cl, frame, StageDenoiser);
                    }

                    // -- composition --
                    // NOTE (converter alias): the composition target IS the converter's
                    // internal motion texture (GetDenoiserMotionInput and
                    // GetCompositionOutput alias). The denoiser consumed it as motion
                    // before this dispatch; after it, the same resource holds composed
                    // color. The lobe outputs (outputBuffer1/2) are NOT aliased, so
                    // reading them after composition is sound; nothing here reads the
                    // motion view after composition.
                    timer.Begin(cl, frame, StageComposition);
                    FSRDCompositionDesc comp = {};
                    comp.DstTexSize = conv.RenderSize;
                    comp.Flags = ((currentPlan.mask & 4u) && !(currentPlan.mask & 1u) ? uint32_t(FSRDCompFlags::DiffuseSignalIndirect) : 0u) |
                                 ((currentPlan.mask & 8u) ? uint32_t(FSRDCompFlags::SpecularSignalIndirect) : 0u);
                    ApplyOfflineComposition(comp, features, currentPlan);
                    if (bypass)
                    {
                        ExerciseOfflineBypass(converter, cl);
                        comp.Flags |= (1u << 4) | (1u << 5);
                    }
                    SetBaselineCompositionFieldsIfPresent(comp, input.bases.colorBaseX, input.bases.colorBaseY,
                                                t.color.tex.Get());
                    if (!converter.DispatchComposition(cl, comp))
                        Die(std::string("DispatchComposition failed: scene=") + sceneCfg.name +
                            " frame=" + std::to_string(frame));
                    if (pairedDefault && !pairedDefault->DispatchComposition(cl, comp))
                        Die("paired default production composition failed");
                    if (!bypass) CompleteOfflineCapture(converter, cl, dd, comp, input.preExposure, capturedScene,
                        dispatchScene.nearPlane, dispatchScene.infiniteFar ? FLT_MAX : dispatchScene.farPlane,
                        dispatchScene.infiniteFar, dispatchScene.reversedZ, encodedSrDepth != nullptr);
                    featureControls.push_back({{"scene", sceneCfg.name}, {"pass", passName}, {"frame", frame},
                        {"specular_guide", hitDistBound}, {"diffuse_guide", diffuseGuideBound}, {"signal_mask", currentPlan.mask},
                        {"plan_notes", currentPlan.notes}, {"albedo_fix_plan", currentPlan.albedoFix}, {"diffuse_copy", currentPlan.diffuseCopy},
                        {"albedo_fix_active", currentPlan.albedoFix && FSRDSignals::AlbedoFixAllowed(features)},
                        {"descriptor_count", signalChain.count}, {"context_signal_flags", rrCreate.signalFlags},
                        {"feedable_mask", feedableMask}, {"descriptor_types", [&] { nlohmann::json types = nlohmann::json::array();
                            for (size_t i = 0; i < signalChain.count; ++i) types.push_back(signalChain.headers[i]->type); return types; }()},
                        {"conversion_flags", conv.Flags}, {"composition_flags", comp.Flags}, {"reset", reset}, {"rr_bypass", bypass},
                        {"conversion_failure_rejected", features.failureRecovery && frame == 2}});
                    timer.End(cl, frame, StageComposition);
                    if (nativeComparison)
                    {
                        nativeTimer.Begin(cl, frame, StageComposition);
                        nativeRoute.Compose(cl);
                        nativeTimer.End(cl, frame, StageComposition);
                    }

                    // -- upscaler (real FSR upscaler over the composed color) --
                    timer.Begin(cl, frame, StageUpscale);
                    const UpscaleInputs srInputs = PrepareUpscaleInputs(gpu.device.Get(), cl, dispatchScene, t,
                        input.bases, nativeComparison ? nullptr : encodedSrDepth);
                    ffxDispatchDescUpscale sr;
                    FillUpscaleDesc(sr, cl, dispatchScene, cam, t, converter.GetCompositionOutput(),
                                    useExposureTexture, reset, srInputs);
                    if (capturedScene)
                    {
                        sr.frameTimeDelta = float(input.deltaSeconds * 1000.0);
                        sr.preExposure = input.preExposure;
                    }
                    CHECK_FFX(upscaler.api.Dispatch(&srCtx, &sr.header), "SR Dispatch");
                    timer.End(cl, frame, StageUpscale);
                    if (nativeComparison)
                    {
                        nativeTimer.Begin(cl, frame, StageUpscale);
                        ffxDispatchDescUpscale nativeSr{};
                        FillUpscaleDesc(nativeSr, cl, dispatchScene, cam, t, nativeRoute.composed.tex.Get(),
                                        useExposureTexture, reset, srInputs);
                        nativeSr.output = ffxApiGetResourceDX12(nativeRoute.srOutput.tex.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
                        CHECK_FFX(upscaler.api.Dispatch(&nativeSrCtx, &nativeSr.header), "native SR dispatch");
                        nativeTimer.End(cl, frame, StageUpscale);
                        nativeTimer.Begin(cl, frame, StageReadback); nativeTimer.End(cl, frame, StageReadback);
                    }
                    // Final frame's SR copy rides this list; other readbacks below use
                    // dedicated one-shot lists (stage 5 timing covers only what is
                    // recorded on the GPU).
                    timer.Begin(cl, frame, StageReadback);
                    if (frame + 1 == frameCount)
                    {
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition = {t.srOut.tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
                        cl->ResourceBarrier(1, &b);
                        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                        src.pResource = t.srOut.tex.Get();
                        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        dst.pResource = t.srOut.staging.Get();
                        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        dst.PlacedFootprint = t.srOut.footprint;
                        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        cl->ResourceBarrier(1, &b);
                    }
                    timer.End(cl, frame, StageReadback);

                    CHECK_HR(cl->Close(), "close");
                    const uint64_t execFv = Execute(0, cl);
                    WaitFence(execFv);
                    if (!tracker.Wait(ticket, mf.timeoutMs))
                        Die("GpuBatch ticket " + std::to_string(ticket) +
                            " did not retire; production lifetime contract broken");
                    ++trackerRetired;

                    if (openrrEnabled)
                    {
                        ComPtr<ID3D12CommandAllocator> orAlloc;
                        ComPtr<ID3D12GraphicsCommandList> orCl;
                        CHECK_HR(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&orAlloc)), "orAlloc");
                        CHECK_HR(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, orAlloc.Get(), nullptr, IID_PPV_ARGS(&orCl)), "orCl");
                        CHECK_HR(orCl->Close(), "orCl initial close");
                        OnrRrFrame of{};
                        of.size = sizeof(of);
                        of.flags = (reset ? ONR_RR_FRAME_RESET : 0u) |
                                   (!dispatchScene.linearDepth ? ONR_RR_FRAME_HW_DEPTH : 0u) |
                                   (dispatchScene.reversedZ ? ONR_RR_FRAME_DEPTH_INVERTED : 0u) |
                                   (!dispatchScene.separateRoughness ? ONR_RR_FRAME_ROUGHNESS_PACKED : 0u);
                        of.render_width = dispatchScene.renderW;
                        of.render_height = dispatchScene.renderH;
                        of.color = {t.color.tex.Get(), uint32_t(kRead), input.bases.colorBaseX, input.bases.colorBaseY};
                        of.depth = {t.depth.tex.Get(), uint32_t(kRead), input.bases.depthBaseX, input.bases.depthBaseY};
                        of.motion = {t.motion.tex.Get(), uint32_t(kRead), input.bases.mvBaseX, input.bases.mvBaseY};
                        of.normals = {t.normals.tex.Get(), uint32_t(kRead), input.bases.normalsBaseX, input.bases.normalsBaseY};
                        if (dispatchScene.separateRoughness)
                            of.roughness = {t.roughness.tex.Get(), uint32_t(kRead), input.bases.roughBaseX, input.bases.roughBaseY};
                        of.diffuse_albedo = {t.diffAlb.tex.Get(), uint32_t(kRead), input.bases.albedoBaseX, input.bases.albedoBaseY};
                        of.specular_albedo = {t.specAlb.tex.Get(), uint32_t(kRead), input.bases.albedoBaseX, input.bases.albedoBaseY};
                        if (hitDistBound)
                            of.specular_hit_distance = {t.hitDist.tex.Get(), uint32_t(kRead), input.bases.hitBaseX, input.bases.hitBaseY};
                        of.output = {openrrRoute.output.tex.Get(), uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS), 0, 0};
                        of.jitter_x = cam.jitterX; of.jitter_y = cam.jitterY;
                        of.mv_scale_x = 1.0f; of.mv_scale_y = 1.0f;
                        memcpy(of.world_to_view, &rawView, sizeof(rawView));
                        memcpy(of.view_to_clip, &rawProjection, sizeof(rawProjection));
                        if (!openrrRoute.isReady)
                        {
                            for (int attempt = 0; attempt < 600; ++attempt)
                            {
                                CHECK_HR(orAlloc->Reset(), "orAlloc Reset");
                                CHECK_HR(orCl->Reset(orAlloc.Get(), nullptr), "orCl Reset");
                                void* warmJob = nullptr;
                                if (openrrRoute.activeBackend == ONR_RR_BACKEND_D3D12)
                                {
                                    if (openrrRoute.api5.v4.Record(openrrRoute.ctx, &of, orCl.Get(), ONR_RR_EDIT_OFF, nullptr, &warmJob) != ONR_RR_OK)
                                        Die("OpenRR warmup Record failed");
                                    CHECK_HR(orCl->Close(), "close OpenRR warmup");
                                    ID3D12CommandList* wLists[] = {orCl.Get()};
                                    gpu.queue->ExecuteCommandLists(1, wLists);
                                    fenceValue += 1;
                                    const uint64_t wfv = fenceValue;
                                    CHECK_HR(gpu.queue->Signal(frameFence.Get(), wfv), "warm Signal");
                                    WaitFence(wfv);
                                    if (gpu.info) gpu.info->ClearStoredMessages();
                                }
                                OnrRrInfo info{}; info.size = sizeof(info);
                                if (openrrRoute.api5.v4.v3.v2.v1.GetInfo(openrrRoute.ctx, &info) != ONR_RR_OK)
                                    Die("OpenRR GetInfo failed");
                                if (info.state == ONR_RR_STATE_READY) { openrrRoute.isReady = true; break; }
                                if (info.state == ONR_RR_STATE_FAILED) Die(std::string("OpenRR build failed: ") + info.message);
                                Sleep(20);
                            }
                            if (!openrrRoute.isReady) Die("OpenRR did not reach ONR_RR_STATE_READY");
                            of.flags |= ONR_RR_FRAME_RESET;
                        }
                        CHECK_HR(orAlloc->Reset(), "orAlloc Reset timed");
                        CHECK_HR(orCl->Reset(orAlloc.Get(), nullptr), "orCl Reset timed");
                        openrrTimer.Begin(orCl.Get(), frame, StageUpload); openrrTimer.End(orCl.Get(), frame, StageUpload);
                        openrrTimer.Begin(orCl.Get(), frame, StageConversion); openrrTimer.End(orCl.Get(), frame, StageConversion);
                        openrrTimer.Begin(orCl.Get(), frame, StageDenoiser);
                        void* job = nullptr;
                        if (openrrRoute.activeBackend == ONR_RR_BACKEND_D3D12)
                        {
                            if (openrrRoute.api5.v4.Record(openrrRoute.ctx, &of, orCl.Get(), ONR_RR_EDIT_OFF, nullptr, &job) != ONR_RR_OK)
                                Die("OpenRR D3D12 Record failed");
                        }
                        openrrTimer.End(orCl.Get(), frame, StageDenoiser);
                        openrrTimer.Begin(orCl.Get(), frame, StageComposition); openrrTimer.End(orCl.Get(), frame, StageComposition);
                        openrrTimer.Begin(orCl.Get(), frame, StageUpscale); openrrTimer.End(orCl.Get(), frame, StageUpscale);
                        openrrTimer.Begin(orCl.Get(), frame, StageReadback); openrrTimer.End(orCl.Get(), frame, StageReadback);
                        CHECK_HR(orCl->Close(), "orCl Close timed");
                        ID3D12CommandList* oLists[] = {orCl.Get()};
                        gpu.queue->ExecuteCommandLists(1, oLists);
                        fenceValue += 1;
                        const uint64_t ofv = fenceValue;
                        CHECK_HR(gpu.queue->Signal(frameFence.Get(), ofv), "orCl Signal");
                        WaitFence(ofv);
                        if (gpu.info) gpu.info->ClearStoredMessages();
                    }

                    // New-closure temporal parity: flip the history read slot after each
                    // fully successful frame (required, per converter owner).
                    FinishCompositionHistoryIfPresent(converter, !bypass);
                    if (pairedDefault) FinishCompositionHistoryIfPresent(*pairedDefault, !bypass);
                    if (features.capture && pass == 1 && frame == 1)
                    {
                        const fs::path captureDestination = outDir / "additive_capture" / sceneCfg.name;
                        captureEvidence.push_back(ReadOfflineCapture(converter, captureDestination, Sha256File));
                    }

                    gpu.DrainInfoQueue(sceneCfg.name);

                    // Record the exact applied dispatch controls.
                    {
                        AppliedControls ac = {};
                        ac.frameIndex = frame;
                        ac.flags = dd.flags;
                        ac.renderW = sceneCfg.renderW;
                        ac.renderH = sceneCfg.renderH;
                        ac.mvScale[0] = ac.mvScale[1] = ac.mvScale[2] = 1.0f;
                        ac.camDelta[0] = cam.camDelta.x;
                        ac.camDelta[1] = cam.camDelta.y;
                        ac.camDelta[2] = cam.camDelta.z;
                        ac.jitter[0] = cam.jitterX;
                        ac.jitter[1] = cam.jitterY;
                        ac.depthBounds[0] = conv.NearPlane;
                        ac.depthBounds[1] = conv.FarPlane;
                        memcpy(ac.view, &dd.view, sizeof(ac.view));
                        memcpy(ac.projection, &dd.projection, sizeof(ac.projection));
                        ac.upscaleW = sceneCfg.TargetW();
                        ac.upscaleH = sceneCfg.TargetH();
                        ac.srJitter[0] = cam.jitterX;
                        ac.srJitter[1] = cam.jitterY;
                        ac.srNearFar[0] = sr.cameraNear;
                        ac.srNearFar[1] = sr.cameraFar;
                        art.controls.write(reinterpret_cast<const char*>(&ac), sizeof(ac));
                        inputControlEvidence.Add(&ac, offsetof(AppliedControls, srNearFar));
                        derivedControlEvidence.Add(ac.srNearFar, sizeof ac.srNearFar);
                    }

                    const bool readThisFrame = capturedScene || (frame % mf.readbackEvery == 0) || frame + 1 == frameCount;
                    if (readThisFrame)
                    {
                        // One-shot fence-completed readbacks of every reported stage.
                        const std::vector<float> lobeDiffuse = DownloadTexture(
                            gpu, converter.GetDenoiserDiffuseOutput(), kRead,
                            DXGI_FORMAT_R16G16B16A16_FLOAT, sceneCfg.renderW, sceneCfg.renderH);
                        const std::vector<float> lobeSpecular = nativeComparison
                            ? std::vector<float>{} // absent signal; never read an uninitialized RR output
                            : DownloadTexture(gpu, converter.GetDenoiserSpecularOutput(), kRead,
                                DXGI_FORMAT_R16G16B16A16_FLOAT, sceneCfg.renderW, sceneCfg.renderH);
                        std::vector<float> preSR =
                            DownloadTexture(gpu, converter.GetCompositionOutput(), kRead,
                                            DXGI_FORMAT_R16G16B16A16_FLOAT, sceneCfg.renderW, sceneCfg.renderH);
                        const std::vector<float> rawInput =
                            DownloadTexture(gpu, t.color.tex.Get(), kRead, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                            sceneCfg.renderW, sceneCfg.renderH, input.bases.colorBaseX, input.bases.colorBaseY);
                        if (pairedDefault)
                        {
                            const fs::path pairDir = outDir / "paired_zero" / (std::string(sceneCfg.name) + "_" + passName + "_f" + std::to_string(frame));
                            fs::create_directories(pairDir);
                            const auto checkPair = [&](ID3D12Resource* actual, ID3D12Resource* defaults, const char* stage) {
                                const auto a = DownloadTexture(gpu, actual, kRead, DXGI_FORMAT_R16G16B16A16_FLOAT, sceneCfg.renderW, sceneCfg.renderH);
                                const auto b = DownloadTexture(gpu, defaults, kRead, DXGI_FORMAT_R16G16B16A16_FLOAT, sceneCfg.renderW, sceneCfg.renderH);
                                WritePfm(pairDir / (std::string(stage) + "_zero.pfm"), a.data(), sceneCfg.renderW, sceneCfg.renderH, 4);
                                WritePfm(pairDir / (std::string(stage) + "_default.pfm"), b.data(), sceneCfg.renderW, sceneCfg.renderH, 4);
                                if (a.size() != b.size() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) != 0)
                                    Die(std::string("same-process additive default/zero actual GPU bytes differ: ") + stage);
                            };
                            checkPair(static_cast<ID3D12Resource*>(signalChain.diffuse.signal.input.resource),
                                      static_cast<ID3D12Resource*>(pairedSignalChain.diffuse.signal.input.resource), "conversion_diffuse");
                            checkPair(static_cast<ID3D12Resource*>(signalChain.specular.signal.input.resource),
                                      static_cast<ID3D12Resource*>(pairedSignalChain.specular.signal.input.resource), "conversion_specular");
                            checkPair(converter.GetCompositionOutput(), pairedDefault->GetCompositionOutput(), "composition");
                            ++pairedZeroReadbacks;
                            featureControls.back()["paired_zero_readback_result"] = "BYTE_IDENTICAL";
                        }
                        const fs::path signalDir = outDir / "signals" / (std::string(sceneCfg.name) + "_" + passName + "_f" + std::to_string(frame));
                        if (features.CandidateOnly())
                        {
                            fs::create_directories(signalDir);
                            const auto saveSignal = [&](const auto& descriptor) {
                                const auto image = DownloadTexture(gpu, static_cast<ID3D12Resource*>(descriptor.signal.output.resource),
                                    kRead, DXGI_FORMAT_R16G16B16A16_FLOAT, sceneCfg.renderW, sceneCfg.renderH);
                                WritePfm(signalDir / (std::to_string(descriptor.header.type) + ".pfm"), image.data(), sceneCfg.renderW, sceneCfg.renderH, 4);
                            };
                            if (currentPlan.mask & 5u) saveSignal(signalChain.diffuse);
                            if (currentPlan.mask & 10u) saveSignal(signalChain.specular);
                            if ((currentPlan.mask & 5u) == 5u) saveSignal(signalChain.extraDiffuse);
                            if ((currentPlan.mask & 10u) == 10u) saveSignal(signalChain.extraSpecular);
                            std::ofstream energy(signalDir / "energy.json");
                            energy << nlohmann::json({{"frame", frame}, {"raw_vs_truth", ComparePlanes(rawInput, fx.truthJittered, sceneCfg.renderW, sceneCfg.renderH).energyRatio},
                                {"presr_vs_truth", ComparePlanes(preSR, fx.truthJittered, sceneCfg.renderW, sceneCfg.renderH).energyRatio},
                                {"additive_strength", features.FfxDenoiserAdditiveLightSplit.value_or_default()}}).dump(2);
                            if (!energy) Die("cannot write actual signal energy evidence");
                            WritePfm(signalDir / "presr.pfm", preSR.data(), sceneCfg.renderW, sceneCfg.renderH, 4);
                            featureControls.back()["presr_readback_sha256"] = Sha256File(signalDir / "presr.pfm");
                        }
                        std::vector<float> srOut;
                        if (frame + 1 == frameCount)
                            srOut = t.srOut.DownloadAsFloat(4); // copied inside the frame list above
                        else
                            srOut = DownloadTexture(gpu, t.srOut.tex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                    DXGI_FORMAT_R16G16B16A16_FLOAT, sceneCfg.TargetW(),
                                                    sceneCfg.TargetH());
                        if (nativeComparison)
                        {
                            const auto nativeDiffuse = DownloadTexture(gpu, nativeRoute.output.tex.Get(), kRead,
                                nativeRoute.output.format, sceneCfg.renderW, sceneCfg.renderH);
                            const auto nativePre = DownloadTexture(gpu, nativeRoute.composed.tex.Get(), kRead,
                                nativeRoute.composed.format, sceneCfg.renderW, sceneCfg.renderH);
                            const auto nativeSr = DownloadTexture(gpu, nativeRoute.srOutput.tex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                nativeRoute.srOutput.format, sceneCfg.TargetW(), sceneCfg.TargetH());
                            const fs::path directory = art.outRoot / "frames" /
                                (std::string(sceneCfg.name) + "_" + passName + "_f" + std::to_string(frame));
                            fs::create_directories(directory);
                            WritePfm(directory / "native_direct_diffuse.pfm", nativeDiffuse.data(), sceneCfg.renderW, sceneCfg.renderH, 4);
                            WritePfm(directory / "native_presr.pfm", nativePre.data(), sceneCfg.renderW, sceneCfg.renderH, 4);
                            WritePfm(directory / "native_sr.pfm", nativeSr.data(), sceneCfg.TargetW(), sceneCfg.TargetH(), 4);
                            const auto dumpGuide = [&](GpuTexture& texture, const char* name) {
                                const auto values = DownloadTexture(gpu, texture.tex.Get(), kRead, texture.format, sceneCfg.renderW, sceneCfg.renderH);
                                WritePfm(directory / name, values.data(), sceneCfg.renderW, sceneCfg.renderH, 4);
                                return values;
                            };
                            dumpGuide(nativeRoute.signal, "native_input_direct_diffuse.pfm");
                            const auto nativeDepthGuide = dumpGuide(nativeRoute.depth, "native_linear_depth.pfm");
                            const auto nativeNormalGuide = dumpGuide(nativeRoute.normals, "native_packed_normals.pfm");
                            const auto nativeMotionGuide = dumpGuide(nativeRoute.motion, "native_motion.pfm");
                            dumpGuide(t.diffAlb, "shared_diffuse_albedo.pfm");
                            const auto convertedInput = DownloadTexture(gpu,
                                static_cast<ID3D12Resource*>(signalChain.diffuse.signal.input.resource), kRead,
                                DXGI_FORMAT_R16G16B16A16_FLOAT, sceneCfg.renderW, sceneCfg.renderH);
                            WritePfm(directory / "translated_input_direct_diffuse.pfm", convertedInput.data(), sceneCfg.renderW, sceneCfg.renderH, 4);
                            nlohmann::json guideComparison;
                            const auto dumpTranslatedGuide = [&](const auto& resource, const std::vector<float>& reference,
                                                                 const char* name, float tolerance, uint32_t channels, bool checkDepth = false) {
                                auto* texture = static_cast<ID3D12Resource*>(resource.resource);
                                if (!texture) Die("native comparison missing translated guide");
                                const auto desc = texture->GetDesc();
                                if (desc.Width != sceneCfg.renderW || desc.Height != sceneCfg.renderH)
                                    Die("native/translated guide physical extent mismatch");
                                const auto values = DownloadTexture(gpu, texture, kRead, desc.Format, sceneCfg.renderW, sceneCfg.renderH);
                                WritePfm(directory / name, values.data(), sceneCfg.renderW, sceneCfg.renderH, 4);
                                float maxAbs = 0.0f;
                                uint64_t aboveTolerance = 0;
                                for (size_t i = 0; i < values.size(); i += 4)
                                {
                                    if (checkDepth)
                                    {
                                        const float z = std::abs(values[i]);
                                        if (!std::isfinite(z) || z <= sceneCfg.nearPlane || z >= sceneCfg.farPlane ||
                                            std::log(z + 1.0f) / std::log(sceneCfg.farPlane + 1.0f) >= 0.99f)
                                            Die("translated diagnostic guide outside depth/bypass bounds");
                                    }
                                    for (uint32_t c = 0; c < channels; ++c)
                                    {
                                        if (!std::isfinite(values[i + c]) || !std::isfinite(reference[i + c]))
                                            Die("nonfinite paired diagnostic guide");
                                        const float delta = std::abs(values[i + c] - reference[i + c]);
                                        maxAbs = std::max(maxAbs, delta);
                                        aboveTolerance += delta > tolerance;
                                    }
                                }
                                guideComparison[name] = {{"max_abs_delta", maxAbs}, {"diagnostic_tolerance", tolerance},
                                    {"components_above_tolerance", aboveTolerance}, {"compared_channels", channels},
                                    {"physical_extent", {desc.Width, desc.Height}}};
                            };
                            dumpTranslatedGuide(dd.linearDepth, nativeDepthGuide, "translated_linear_depth.pfm", 1e-3f, 1, true);
                            // Half input XYZ + oct encoding + UNORM10 storage can differ by
                            // two normal codes. Compare decoded packed channels, not bytes.
                            dumpTranslatedGuide(dd.normals, nativeNormalGuide, "translated_packed_normals.pfm", 2.0f / 1023.0f, 4);
                            dumpTranslatedGuide(dd.motionVectors, nativeMotionGuide, "translated_motion.pfm", 1e-4f, 3);
                            float alphaMin = std::numeric_limits<float>::max(), alphaMax = -alphaMin;
                            for (size_t i = 3; i < convertedInput.size(); i += 4)
                            {
                                alphaMin = std::min(alphaMin, convertedInput[i]);
                                alphaMax = std::max(alphaMax, convertedInput[i]);
                            }
                            guideComparison["signal_alpha"] = {{"native", sceneCfg.nativeSignalAlpha},
                                {"translated_min", alphaMin}, {"translated_max", alphaMax},
                                {"contract", "active DirectDiffuse: non-negative, undefined payload; not hit distance"}};
                            guideComparison["depth_coverage"] = {{"misses", 0}, {"outside_rr_bounds", 0}, {"converter_far_skip", 0}};
                            std::ofstream guideReport(directory / "guide_comparison.json");
                            guideReport << guideComparison.dump(2);
                            if (!guideReport) Die("cannot write diagnostic guide comparison");
                            auto* translatedSignal = static_cast<ID3D12Resource*>(signalChain.diffuse.signal.input.resource);
                            if (translatedSignal->GetDesc().Width != sceneCfg.renderW || translatedSignal->GetDesc().Height != sceneCfg.renderH)
                                Die("native/translated signal physical extent mismatch");
                            const auto writeNativeMetric = [&](const char* stage, const auto& image, const auto& truth, uint32_t w, uint32_t h) {
                                const auto stats = ComparePlanes(image, truth, w, h);
                                anyFailure |= stats.nonFinite != 0;
                                art.metrics << sceneCfg.name << ',' << passName << ',' << frame << ',' << stage << ',' << stats.rmse << ','
                                    << stats.bias << ',' << stats.energyRatio << ',' << stats.detailRatio << ',' << stats.psnr << ','
                                    << stats.nonFinite << ',' << stats.noiseStd << "\n";
                            };
                            writeNativeMetric("native_presr", nativePre, fx.truthJittered, sceneCfg.renderW, sceneCfg.renderH);
                            writeNativeMetric("native_sr", nativeSr, fx.truthTarget, sceneCfg.TargetW(), sceneCfg.TargetH());
                            std::ofstream provenance(directory / "native_provenance.json");
                            provenance << nlohmann::json({{"input_class", "synthetic-analytic-Lambertian-with-synthetic-lognormal-noise"},
                                {"route", "independent-renderer-DirectDiffuse-and-guides; no-product-converter"},
                                {"signal_units", "E*max(dot(N,L),0)/pi; albedo-free"}, {"composition", "RR-DirectDiffuse*shared-linear-diffuse-albedo"},
                                {"frame", frame}, {"reset", reset}, {"phase", frame < mf.warmupFrames ? "warmup" : "measured"},
                                {"native_signal_alpha", sceneCfg.nativeSignalAlpha}, {"history_mode", nativeHistory},
                                {"geometry", "finite viewport-covering rectangle at view-Z 12 plus bounded spheres in detail scene; near=0.1 far=32; no horizon/bypass pixels"},
                                {"target_truth_samples_per_pixel", 16}, {"target_truth_filter", "4x4 stratified midpoint box"},
                                {"render_resource_extent", {sceneCfg.renderW, sceneCfg.renderH}},
                                {"native_presr_sha256", Sha256File(directory / "native_presr.pfm")},
                                {"native_sr_sha256", Sha256File(directory / "native_sr.pfm")}}).dump(2);
                            if (!provenance) Die("cannot write native frame provenance");
                        }
                        if (capturedScene)
                        {
                            const fs::path replayDir = art.outRoot / "replay";
                            fs::create_directories(replayDir);
                            char replayName[32];
                            snprintf(replayName, sizeof replayName, "frame%04u.raw", frame);
                            std::vector<uint16_t> rgba16(srOut.size());
                            std::transform(srOut.begin(), srOut.end(), rgba16.begin(), FloatToHalf);
                            std::ofstream replay(replayDir / replayName, std::ios::binary);
                            replay.write(reinterpret_cast<const char*>(rgba16.data()),
                                         std::streamsize(rgba16.size() * sizeof(uint16_t)));
                            replay.close();
                            if (!replay)
                                Die("cannot write captured-frame SR replay output");
                            CaptureJson replayIdentity = {
                                {"frameId", frame}, {"format", "RGBA16_FLOAT"},
                                {"width", sceneCfg.TargetW()}, {"height", sceneCfg.TargetH()},
                                {"bytes", fs::file_size(replayDir / replayName)},
                                {"sha256", Sha256File(replayDir / replayName)},
                                {"sourceFrame", CaptureJson::parse(input.provenance)}};
                            std::ofstream identity(replayDir / (std::string(replayName) + ".json"));
                            identity << replayIdentity.dump(2);
                            identity.close();
                            if (!identity)
                                Die("cannot write captured-frame replay provenance");
                        }

                        struct Row
                        {
                            const char* stage;
                            const std::vector<float>* image;
                            const std::vector<float>* truth;
                            uint32_t w, h;
                        };
                        const Row rows[] = {
                            {"raw", &rawInput, capturedScene ? &fx.truthRender : &fx.truthJittered, sceneCfg.renderW, sceneCfg.renderH},
                            {"lobe_diffuse", &lobeDiffuse, &fx.truthRender, sceneCfg.renderW, sceneCfg.renderH},
                            {"lobe_specular", &lobeSpecular, &fx.truthRender, sceneCfg.renderW, sceneCfg.renderH},
                            {"presr", &preSR, capturedScene ? &fx.truthRender : &fx.truthJittered, sceneCfg.renderW, sceneCfg.renderH},
                            {"sr", &srOut, &fx.truthTarget, sceneCfg.TargetW(), sceneCfg.TargetH()},
                        };
                        for (const Row& row : rows)
                        {
                            if (nativeComparison && std::strncmp(row.stage, "lobe_", 5) == 0) continue;
                            if (row.truth->empty())
                            {
                                // No truth published for this stage (capture without a
                                // target PFM): still fail on non-finite GPU output.
                                uint64_t nonFinite = 0;
                                for (float v : *row.image)
                                    if (!std::isfinite(v))
                                        ++nonFinite;
                                if (nonFinite)
                                {
                                    fprintf(stderr,
                                            "FAIL: %llu non-finite samples in %s scene=%s frame=%u\n",
                                            (unsigned long long) nonFinite, row.stage, sceneCfg.name, frame);
                                    anyFailure = true;
                                }
                                continue;
                            }
                            const PlaneStats st = ComparePlanes(*row.image, *row.truth, row.w, row.h);
                            if (st.nonFinite)
                            {
                                fprintf(stderr, "FAIL: %llu non-finite samples in %s scene=%s frame=%u\n",
                                        (unsigned long long) st.nonFinite, row.stage, sceneCfg.name, frame);
                                anyFailure = true;
                            }
                            art.metrics << sceneCfg.name << ',' << passName << ',' << frame << ',' << row.stage
                                        << ',' << st.rmse << ',' << st.bias << ',' << st.energyRatio << ','
                                        << st.detailRatio << ',' << st.psnr << ',' << st.nonFinite << ','
                                        << st.noiseStd << "\n";
                        }
                        if (noisy && sigma > 0.0f && !capturedScene)
                        {
                            // Fixture honesty: the noisy input must carry its declared
                            // RELATIVE noise. std(raw/jitteredTruth - 1) over non-trivial pixels
                            // equals sqrt(exp(sigma^2)-1) for the lognormal multiplier.
                            const size_t n = size_t(sceneCfg.renderW) * sceneCfg.renderH;
                            double maxLuma = 1e-9;
                            for (size_t i = 0; i < n; ++i)
                                maxLuma = std::max(maxLuma, Luma4(&fx.truthJittered[i * 4]));
                            double sum = 0, sumSq = 0;
                            size_t used = 0;
                            for (size_t i = 0; i < n; ++i)
                            {
                                const double truthLuma = Luma4(&fx.truthJittered[i * 4]);
                                if (truthLuma < 0.02 * maxLuma)
                                    continue;
                                const double d = (Luma4(&rawInput[i * 4]) - truthLuma) / truthLuma;
                                sum += d;
                                sumSq += d * d;
                                ++used;
                            }
                            if (used > 32)
                            {
                                const double meanRel = sum / double(used);
                                const double stdRel =
                                    sqrt(std::max(0.0, sumSq / double(used) - meanRel * meanRel));
                                const double expected = sqrt(exp(double(sigma) * double(sigma)) - 1.0);
                                if (stdRel < 0.5 * expected || stdRel > 1.8 * expected)
                                {
                                    fprintf(stderr,
                                            "FAIL: noisy input relative std %.5f vs expected %.5f (scene=%s): "
                                            "fixture noise model broken\n",
                                            stdRel, expected, sceneCfg.name);
                                    anyFailure = true;
                                }
                                if (fabs(meanRel) > 0.25 * expected)
                                {
                                    fprintf(stderr,
                                            "NOTE: noisy input carries %.5f mean relative bias vs expected std "
                                            "%.5f (scene=%s); recorded, not hidden\n",
                                            meanRel, expected, sceneCfg.name);
                                }
                            }
                        }

                        ReadFrame rf;
                        rf.frameId = frame;
                        rf.presr = std::move(preSR);
                        rf.sr = std::move(srOut);
                        if (nativeComparison)
                            rf.nativeSr = DownloadTexture(gpu, nativeRoute.srOutput.tex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                nativeRoute.srOutput.format, sceneCfg.TargetW(), sceneCfg.TargetH());
                        if (openrrEnabled)
                        {
                            rf.openrrSr = DownloadTexture(gpu, openrrRoute.output.tex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                openrrRoute.output.format, sceneCfg.TargetW(), sceneCfg.TargetH());
                            const auto st = ComparePlanes(rf.openrrSr, fx.truthTarget, sceneCfg.TargetW(), sceneCfg.TargetH());
                            anyFailure |= st.nonFinite != 0;
                            art.metrics << sceneCfg.name << ',' << passName << ',' << frame << ",openrr_sr," << st.rmse << ','
                                << st.bias << ',' << st.energyRatio << ',' << st.detailRatio << ',' << st.psnr << ','
                                << st.nonFinite << ',' << st.noiseStd << "\n";
                        }
                        rf.truthR = capturedScene ? fx.truthRender : fx.truthJittered;
                        rf.truthT = fx.truthTarget;
                        rf.hasTruth = !capturedScene || input.hasTruthTarget;

                        const bool dump = mf.dumpAllFrames ||
                                          std::find(std::begin(dumpFrames), std::end(dumpFrames), frame) !=
                                              std::end(dumpFrames);
                        if (dump)
                        {
                            const fs::path dir = art.outRoot / "frames" /
                                                 (std::string(sceneCfg.name) + "_" + passName + "_f" +
                                                  std::to_string(frame));
                            fs::create_directories(dir);
                            WritePfm(dir / "raw_input.pfm", rawInput.data(), sceneCfg.renderW, sceneCfg.renderH, 4);
                            WritePfm(dir / "lobe_diffuse.pfm", lobeDiffuse.data(), sceneCfg.renderW,
                                     sceneCfg.renderH, 4);
                            if (!nativeComparison)
                                WritePfm(dir / "lobe_specular.pfm", lobeSpecular.data(), sceneCfg.renderW, sceneCfg.renderH, 4);
                            WritePfm(dir / "presr.pfm", rf.presr.data(), sceneCfg.renderW, sceneCfg.renderH, 4);
                            WritePfm(dir / "sr.pfm", rf.sr.data(), sceneCfg.TargetW(), sceneCfg.TargetH(), 4);
                            if (!fx.truthJittered.empty())
                                WritePfm(dir / "truth_jittered.pfm", fx.truthJittered.data(), sceneCfg.renderW,
                                         sceneCfg.renderH, 4);
                            if (!fx.truthRender.empty())
                                WritePfm(dir / "truth_render.pfm", fx.truthRender.data(), sceneCfg.renderW,
                                         sceneCfg.renderH, 4);
                            if (!fx.truthTarget.empty())
                                WritePfm(dir / "truth_target.pfm", fx.truthTarget.data(), sceneCfg.TargetW(),
                                         sceneCfg.TargetH(), 4);
                        }
                        reads.push_back(std::move(rf));
                    }
                    hasHistory = true;
                }

                // Resolve this pass's timestamps before the next pass overwrites them.
                WaitFence(slotFence[0]);
                CHECK_HR(allocators[0]->Reset(), "allocator Reset");
                ID3D12GraphicsCommandList* cl = lists[0].Get();
                CHECK_HR(cl->Reset(allocators[0].Get(), nullptr), "list Reset");
                timer.Resolve(cl);
                if (nativeComparison) nativeTimer.Resolve(cl);
                if (openrrEnabled) openrrTimer.Resolve(cl);
                CHECK_HR(cl->Close(), "close");
                Execute(0, cl);
                WaitFence(slotFence[0]);
                WriteSegments(timer.Read(frameCount), sceneCfg, passName, frameCount);
                if (nativeComparison)
                {
                    const auto times = nativeTimer.Read(frameCount);
                    for (uint32_t f = 0; f < frameCount; ++f)
                        for (Stage stage : {StageUpload, StageDenoiser, StageComposition, StageUpscale})
                            art.segments << sceneCfg.name << ',' << passName << "_native," << f << ',' << StageName(stage)
                                << ',' << times[f * StageCount + stage] << "\n";
                }
                if (openrrEnabled)
                {
                    const auto times = openrrTimer.Read(frameCount);
                    for (uint32_t f = 0; f < frameCount; ++f)
                        art.segments << sceneCfg.name << ',' << passName << "_openrr," << f << ",denoiser,"
                            << times[f * StageCount + StageDenoiser] << "\n";
                }

                // Image-space truth-residual change, not motion-compensated surface flicker.
                if (reads.size() >= 2 && !capturedScene)
                {
                    const auto residualMad = [](const auto& a, const auto& at, const auto& b, const auto& bt) {
                        double sum = 0; const size_t pixels = a.size() / 4;
                        for (size_t p = 0; p < pixels; ++p)
                            sum += std::fabs((Luma4(&a[p * 4]) - Luma4(&at[p * 4])) -
                                             (Luma4(&b[p * 4]) - Luma4(&bt[p * 4])));
                        return sum / double(pixels);
                    };
                    double presrFlicker = 0, srFlicker = 0, nativeSrFlicker = 0, openrrSrFlicker = 0;
                    for (size_t i = 1; i < reads.size(); ++i)
                    {
                        presrFlicker += residualMad(reads[i - 1].presr, reads[i - 1].truthR, reads[i].presr, reads[i].truthR);
                        srFlicker += residualMad(reads[i - 1].sr, reads[i - 1].truthT, reads[i].sr, reads[i].truthT);
                        if (nativeComparison)
                            nativeSrFlicker += residualMad(reads[i - 1].nativeSr, reads[i - 1].truthT, reads[i].nativeSr, reads[i].truthT);
                        if (openrrEnabled)
                            openrrSrFlicker += residualMad(reads[i - 1].openrrSr, reads[i - 1].truthT, reads[i].openrrSr, reads[i].truthT);
                    }
                    art.temporal << sceneCfg.name << ',' << passName << ",presr_residual_temporal_mad_per_readback,"
                                 << presrFlicker / double(reads.size() - 1) << "\n";
                    art.temporal << sceneCfg.name << ',' << passName << ",sr_residual_temporal_mad_per_readback,"
                                 << srFlicker / double(reads.size() - 1) << "\n";
                    if (nativeComparison)
                        art.temporal << sceneCfg.name << ',' << passName << ",native_sr_residual_temporal_mad_per_readback,"
                                     << nativeSrFlicker / double(reads.size() - 1) << "\n";
                    if (openrrEnabled)
                        art.temporal << sceneCfg.name << ',' << passName << ",openrr_sr_residual_temporal_mad_per_readback,"
                                     << openrrSrFlicker / double(reads.size() - 1) << "\n";
                    const auto early = std::find_if(reads.begin(), reads.end(), [&](const auto& r) { return r.frameId >= sceneCfg.revealFrame; });
                    if (sceneCfg.occlusionReveal && early != reads.end() && early->frameId < reads.back().frameId)
                    {
                        // Whole-image reveal residual, event aligned; no reveal-ROI claim.
                        const double earlyError = ComparePlanes(early->presr, early->truthR, sceneCfg.renderW, sceneCfg.renderH).rmse;
                        const double lateError = ComparePlanes(reads.back().presr, reads.back().truthR, sceneCfg.renderW, sceneCfg.renderH).rmse;
                        art.temporal << sceneCfg.name << ',' << passName << ",presr_ghost_early_rmse," << earlyError << "\n";
                        art.temporal << sceneCfg.name << ',' << passName << ",presr_ghost_settled_rmse," << lateError << "\n";
                        art.temporal << sceneCfg.name << ',' << passName << ",presr_ghost_early_frame," << early->frameId << "\n";
                        art.temporal << sceneCfg.name << ',' << passName << ",presr_ghost_settled_frame," << reads.back().frameId << "\n";
                        art.temporal << sceneCfg.name << ',' << passName << ",presr_ghost_reveal_frame," << sceneCfg.revealFrame << "\n";
                    }
                }
            }
        }

        // ================= perf: bounded in-flight ring, diagnostics off =================
        if (mode == "perf")
        {
            StageTimer timer;
            timer.Init(gpu.device.Get(), gpu.queue.Get(), perSceneFrames);
            std::vector<uint64_t> slotTicket(ringSize, 0);
            bool hasHistory = false;
            for (uint32_t frame = 0; frame < perSceneFrames; ++frame)
            {
                const bool reset = frame == 0 ||
                                   std::find(sceneCfg.resetFrames.begin(), sceneCfg.resetFrames.end(), frame) !=
                                       sceneCfg.resetFrames.end();
                FrameInput input;
                input.cam = BuildCamera(sceneCfg, frame);
                input.fx = RenderFixture(scene, input.cam, frame, mf.seed, texW, texH, baseX, baseY,
                                         sceneCfg.noiseSigma, true);
                input.bases.colorBaseX = baseX;
                input.bases.colorBaseY = baseY;
                input.bases.depthBaseX = baseX;
                input.bases.depthBaseY = baseY;
                input.bases.mvBaseX = baseX;
                input.bases.mvBaseY = baseY;
                input.bases.normalsBaseX = baseX;
                input.bases.normalsBaseY = baseY;
                input.bases.roughBaseX = baseX;
                input.bases.roughBaseY = baseY;
                input.bases.albedoBaseX = baseX;
                input.bases.albedoBaseY = baseY;
                input.bases.hitBaseX = baseX;
                input.bases.hitBaseY = baseY;
                const FrameCamera& cam = input.cam;
                const FixtureFrame& fx = input.fx;
                const uint32_t slot = frame % ringSize;
                activeUploadSlot = slot;
                if (frame >= ringSize)
                {
                    WaitFence(slotFence[slot]);
                    if (!tracker.Wait(slotTicket[slot], mf.timeoutMs))
                        Die("perf: GpuBatch ticket did not retire");
                    ++trackerRetired;
                }
                CHECK_HR(allocators[slot]->Reset(), "allocator Reset");
                ID3D12GraphicsCommandList* cl = lists[slot].Get();
                CHECK_HR(cl->Reset(allocators[slot].Get(), nullptr), "list Reset");
                const uint64_t ticket = tracker.Record(cl);
                ++trackerRecorded;

                timer.Begin(cl, frame, StageUpload);
                CopyIn(cl, t.color, fx.color);
                CopyIn(cl, t.depth, fx.depth);
                CopyIn(cl, t.motion, fx.motion);
                if (!fx.srHardwareDepth.empty())
                {
                    CopyIn(cl, t.srDepth, fx.srHardwareDepth);
                    srRendererDepthEvidence.Plane(fx.srHardwareDepth);
                }
                CopyIn(cl, t.normals, fx.normals);
                CopyIn(cl, t.diffAlb, fx.diffAlbedo);
                CopyIn(cl, t.specAlb, fx.specAlbedo);
                if (sceneCfg.separateRoughness)
                    CopyIn(cl, t.roughness, fx.roughness);
                if (useExposureTexture)
                    CopyIn(cl, t.exposure, fx.exposure);
                const bool hitDistBound = sceneCfg.hitDistance && frame < 8;
                if (hitDistBound)
                    CopyIn(cl, t.hitDist, fx.hitDistance);
                timer.End(cl, frame, StageUpload);

                timer.Begin(cl, frame, StageConversion);
                FSRDConversionDesc conv;
                FillConversionDesc(conv, sceneCfg, cam, t, input.bases, hitDistBound, hasHistory && !reset,
                                   mf.specularSignal == "indirect");
                SetFrameIndexIfPresent(conv, frame);
                if (reset) InvalidateCompositionHistoryIfPresent(converter);
                ApplyOfflineConversion(conv, features, currentPlan, false, nullptr, 0, 0);
                if (!converter.DispatchConversion(cl, conv))
                    Die("perf DispatchConversion failed");
                ID3D12Resource* encodedSrDepth =
                    PrepareUpscalerDepthIfPresent(converter, cl, sceneCfg.nearPlane,
                                                  sceneCfg.infiniteFar ? FLT_MAX : sceneCfg.farPlane,
                                                  sceneCfg.infiniteFar, sceneCfg.reversedZ);
                if constexpr (HasUpscalerDepth_v<FSRDPreprocessor_Dx12>)
                {
                    if (encodedSrDepth == nullptr)
                        Die("perf: PrepareUpscalerDepthInput returned null after a successful conversion");
                }
                // History invalidation precedes conversion, never encoded-depth publication.
                timer.End(cl, frame, StageConversion);

                timer.Begin(cl, frame, StageDenoiser);
                ffxDispatchDescDenoiser dd = {};
                OfflineSignalChain signalChain;
                signalChain.Fill(converter, dd, features, currentPlan);
                FillDenoiserDesc(dd, cl, sceneCfg, cam, conv, reset, frame);
                CHECK_FFX(denoiser.api.Dispatch(&rrCtx, &dd.header), "perf RR dispatch");
                timer.End(cl, frame, StageDenoiser);

                timer.Begin(cl, frame, StageComposition);
                FSRDCompositionDesc comp = {};
                comp.DstTexSize = conv.RenderSize;
                comp.Flags = (mf.diffuseSignal == "indirect" ? uint32_t(FSRDCompFlags::DiffuseSignalIndirect)
                                                             : 0u) |
                             (mf.specularSignal == "indirect" ? uint32_t(FSRDCompFlags::SpecularSignalIndirect)
                                                              : 0u);
                SetBaselineCompositionFieldsIfPresent(comp, input.bases.colorBaseX, input.bases.colorBaseY,
                                                t.color.tex.Get());
                ApplyOfflineComposition(comp, features, currentPlan);
                if (!converter.DispatchComposition(cl, comp))
                    Die("perf DispatchComposition failed");
                timer.End(cl, frame, StageComposition);

                timer.Begin(cl, frame, StageUpscale);
                const UpscaleInputs srInputs = PrepareUpscaleInputs(gpu.device.Get(), cl, sceneCfg, t,
                    input.bases, encodedSrDepth);
                ffxDispatchDescUpscale sr;
                FillUpscaleDesc(sr, cl, sceneCfg, cam, t, converter.GetCompositionOutput(), useExposureTexture,
                                reset, srInputs);
                CHECK_FFX(upscaler.api.Dispatch(&srCtx, &sr.header), "perf SR dispatch");
                timer.End(cl, frame, StageUpscale);

                timer.Begin(cl, frame, StageReadback);
                if (frame + 1 == perSceneFrames)
                {
                    // Content sanity readback on the final frame only.
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition = {t.srOut.tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
                    cl->ResourceBarrier(1, &b);
                    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
                    src.pResource = t.srOut.tex.Get();
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dst.pResource = t.srOut.staging.Get();
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint = t.srOut.footprint;
                    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    cl->ResourceBarrier(1, &b);
                }
                timer.End(cl, frame, StageReadback);

                CHECK_HR(cl->Close(), "close");
                Execute(slot, cl);
                slotTicket[slot] = ticket;
                FinishCompositionHistoryIfPresent(converter, true);
                hasHistory = true;

                if (frame + 1 == perSceneFrames)
                {
                    const uint32_t activeSlots = (std::min)(perSceneFrames, ringSize);
                    for (uint32_t i = 0; i < activeSlots; ++i)
                    {
                        const uint32_t s = (perSceneFrames - activeSlots + i) % ringSize;
                        WaitFence(slotFence[s]);
                        if (!tracker.Wait(slotTicket[s], mf.timeoutMs))
                            Die("perf: GpuBatch ticket did not retire");
                        ++trackerRetired;
                    }
                    const std::vector<float> srOut = t.srOut.DownloadAsFloat(4);
                    uint64_t nonFinite = 0;
                    for (float v : srOut)
                        if (!std::isfinite(v))
                            ++nonFinite;
                    if (nonFinite)
                    {
                        fprintf(stderr, "FAIL: %llu non-finite SR samples on final perf frame (scene=%s)\n",
                                (unsigned long long) nonFinite, sceneCfg.name);
                        anyFailure = true;
                    }
                }
            }

            WaitFence(slotFence[0]);
            CHECK_HR(allocators[0]->Reset(), "allocator Reset");
            ID3D12GraphicsCommandList* cl = lists[0].Get();
            CHECK_HR(cl->Reset(allocators[0].Get(), nullptr), "list Reset");
            timer.Resolve(cl);
            CHECK_HR(cl->Close(), "close");
            Execute(0, cl);
            WaitFence(slotFence[0]);
            const std::vector<double> ms = timer.Read(perSceneFrames);
            WriteSegments(ms, sceneCfg, "perf", perSceneFrames);
            for (uint32_t s = 0; s < StageCount; ++s)
            {
                std::vector<double> v;
                for (uint32_t f = mf.warmupFrames; f < perSceneFrames; ++f)
                    v.push_back(ms[f * StageCount + s]);
                std::sort(v.begin(), v.end());
                if (!v.empty())
                    fprintf(stderr, "[perf] scene=%s stage=%s p50=%.4f ms p95=%.4f ms n=%zu in_flight=%u\n",
                            sceneCfg.name, StageName(Stage(s)), v[v.size() / 2], v[(v.size() - 1) * 95 / 100],
                            v.size(), ringSize);
            }
        }

        CHECK_FFX(upscaler.api.DestroyContext(&srCtx, nullptr), "SR DestroyContext");
        if (nativeComparison) CHECK_FFX(upscaler.api.DestroyContext(&nativeSrCtx, nullptr), "native SR DestroyContext");
        gpu.DrainInfoQueue("scene-teardown");
    }

    // ---- real DeferredRelease contract check ----
    {
        bool released = false;
        {
            ComPtr<ID3D12Resource> victim;
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width = 4096;
            d.Height = 1;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            CHECK_HR(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&victim)),
                     "DeferredRelease victim");
            ID3D12Resource* raw = victim.Detach();
            DeferredRelease::Enqueue([&released, raw]() {
                released = true;
                raw->Release();
            });
        }
        // Production retirement runs after MinPresents presents AND MinAge elapsed;
        // it cannot be forced, only waited for (bounded).
        const ULONGLONG start = GetTickCount64();
        for (int present = 0; present < 16 && !released; ++present)
        {
            DeferredRelease::Process();
            if (GetTickCount64() - start > 10000)
                break;
            Sleep(120);
        }
        if (!released)
        {
            fprintf(stderr, "FAIL: DeferredRelease entry did not run after bounded presents+age\n");
            anyFailure = true;
        }
        else
            fprintf(stderr, "DeferredRelease: entry retired after bounded presents+age\n");
    }

    CHECK_FFX(denoiser.api.DestroyContext(&rrCtx, nullptr), "RR DestroyContext");
    if (nativeComparison) CHECK_FFX(denoiser.api.DestroyContext(&nativeRrCtx, nullptr), "native RR DestroyContext");
    if (gpu.validationErrors || g_sdkErrors)
        anyFailure = true;

    art.metrics.close();
    art.temporal.close();
    art.segments.close();
    art.controls.close();

    // ---- run.json: identity, configuration, provider defaults, outcomes ----
    {
        std::ostringstream js;
        js << "{\n";
        js << "  \"label\": \"" << JsonEscape(mf.label) << "\",\n";
        js << "  \"closure\": \"" << JsonEscape(closureLabel.empty() ? std::string("tree") : closureLabel.string())
           << "\",\n";
        LARGE_INTEGER driver{};
        CHECK_HR(gpu.adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver), "adapter driver version");
        js << "  \"adapter_driver_version\": \"" << uint64_t(driver.QuadPart) << "\",\n";
        js << "  \"input_sha256\": \"" << inputEvidence.Finish() << "\",\n";
        js << "  \"input_controls_sha256\": \"" << inputControlEvidence.Finish() << "\",\n";
        js << "  \"effective_settings\": " << OfflineEffectiveSettings<FSRDConversionDesc, FSRDCompositionDesc>(features).dump() << ",\n";
        if (pairedDefault)
        {
            if (!pairedZeroReadbacks) Die("same-process additive zero equivalence has no GPU readbacks");
            js << "  \"additive_zero_paired_equivalence\": " << nlohmann::json({{"result", "BYTE_IDENTICAL"},
                {"actual_readbacks_compared", pairedZeroReadbacks},
                {"scope", "same-process production default-vs-explicit-zero conversion and composition; one actual RR dispatch copied identically to both independent composition histories; not independent RR determinism"}}).dump() << ",\n";
        }
        if (!zeroReference.empty())
        {
            const auto reference = LoadJsonFile(zeroReference / "run.json");
            if (reference.value("feature_case", "") != "additive-default" ||
                reference.at("effective_settings") != OfflineEffectiveSettings<FSRDConversionDesc, FSRDCompositionDesc>(features))
                Die("zero-equivalence reference must be additive-default with identical effective settings");
            const auto& previous = reference.at("feature_controls");
            if (previous.size() != featureControls.size()) Die("zero-equivalence frame count mismatch");
            size_t compared = 0;
            for (size_t i = 0; i < featureControls.size(); ++i)
            {
                const auto& now = featureControls[i]; const auto& before = previous[i];
                if (now.at("scene") != before.at("scene") || now.at("pass") != before.at("pass") || now.at("frame") != before.at("frame"))
                    Die("zero-equivalence input sequence mismatch");
                if (now.contains("presr_readback_sha256"))
                {
                    if (!before.contains("presr_readback_sha256") || now.at("presr_readback_sha256") != before.at("presr_readback_sha256"))
                        Die("additive zero changed actual composition readback bytes");
                    ++compared;
                }
            }
            if (!compared) Die("zero-equivalence has no actual readbacks");
            js << "  \"additive_zero_equivalence\": " << nlohmann::json({{"result", "BYTE_IDENTICAL"},
                {"actual_readbacks_compared", compared}, {"reference_run_sha256", Sha256File(zeroReference / "run.json")}}).dump() << ",\n";
        }
        js << "  \"fixture_controls\": " << nlohmann::json({{"guides", features.guides}, {"capture", features.capture},
            {"failure_recovery", features.failureRecovery}}).dump() << ",\n";
        js << "  \"derived_controls\": " << nlohmann::json({
            {"sr_depth_source", nativeComparison ? "shared-renderer-hardware-depth" :
                (HasUpscalerDepth_v<FSRDPreprocessor_Dx12> ? "production-encoded-hardware-depth" :
                    "per-scene independent renderer projection for linear RR inputs; authored hardware depth otherwise")},
            {"sr_inputs", srInputProvenance},
            {"sr_renderer_hardware_depth_sha256", srRendererDepthEvidence.Finish()},
            {"sr_near_far_sha256", derivedControlEvidence.Finish()}}).dump() << ",\n";
        js << "  \"converter_generation\": \"" << (HasOfflineFeatures<FSRDPreprocessor_Dx12> ? "current" : "frozen-2c2f619b") << "\",\n";
        js << "  \"feature_case\": " << nlohmann::json(features.name).dump() << ",\n";
        js << "  \"comparison_scope\": \"" << (nativeComparison ? "synthetic-native-vs-production-translated-diffuse-only" :
            (features.CandidateOnly() ? "candidate-only-feature" : "generation-specific-policy-not-identical-math")) << "\",\n";
        js << "  \"readback_every\": " << mf.readbackEvery << ",\n";
        js << "  \"metric_truth\": " << nlohmann::json({{"raw", fixtureDir.empty() ? "jittered-render-rgb" : "producer-or-none"},
            {"presr", fixtureDir.empty() ? "jittered-render-rgb" : "producer-or-none"},
            {"sr", nativeComparison ? "unjittered-target-rgb-16spp-spatial-box" : (fixtureDir.empty() ? "unjittered-target-rgb" : "producer-or-none")},
            {"lobe_diffuse", "invalid-final-rgb-reference"}, {"lobe_specular", "invalid-final-rgb-reference"}}).dump() << ",\n";
        js << "  \"metric_channels_contract\": \"rgb-radiance-v1;nonfinite=rgba\",\n";
        js << "  \"temporal_metrics_contract\": \"" << (fixtureDir.empty() ? "truth-residual-v1" : "NOT_ASSESSED") << "\",\n";
        js << "  \"feature_controls\": " << featureControls.dump() << ",\n";
        js << "  \"additive_capture_evidence\": " << captureEvidence.dump() << ",\n";
        js << "  \"fixture_geometry_contract\": \"camera-origin ray via finite interior NDC; row-vector CPU matrices; HLSL column-major storage; jitter subtraction matches production reconstruction; view-Z depth; real secondary mirror/cosine ray lengths, miss=0; analytic direct-light radiance, no physical indirect-light decomposition claimed\",\n";
        js << "  \"mode\": \"" << (nativeComparison ? "native-diffuse" : mode) << "\",\n";
        js << "  \"manifest\": \"" << JsonEscape(manifestPath.string()) << "\",\n";
        js << "  \"manifest_sha256\": \"" << Sha256File(manifestPath) << "\",\n";
        if (nativeComparison)
            js << "  \"native_metric_truth\": " << nlohmann::json({{"native_presr", "jittered-render-rgb"},
                {"native_sr", "unjittered-target-rgb-16spp-spatial-box"}, {"lobe_diffuse", "diagnostic-demodulated-not-final-rgb"},
                {"lobe_specular", "absent-not-dispatched-not-exported"}}).dump() << ",\n";
        if (nativeComparison)
            js << "  \"native_comparison\": " << nlohmann::json({
                {"schema", "native-diffuse-v2"}, {"input_class", "synthetic-analytic-Lambertian-not-path-traced"},
                {"scene_selection", nativeScene},
                {"scene", "constant-albedo finite flat rectangle first; separate finite checker rectangle and bounded diffuse spheres; no specular/emissive/ambient/environment"},
                {"geometry_bounds", "near=0.1 far=32; backdrop view-Z=12, half extent=14x9; every evaluated ray must hit within converter bypass and RR depth bounds"},
                {"miss_contract", "black radiance, zero albedo/motion, native signal alpha=-1, signed depth beyond far, neutral normal/rough1/material3; viewport misses fail the diagnostic"},
                {"native_signal_alpha", nativeAlpha == "65504" ? 65504 : 0}, {"history_mode", nativeHistory},
                {"rr_resource_extent", {maxRenderW, maxRenderH}},
                {"rr_resource_extent_contract", "both physical guide/signal extents and both context maxima equal actual render size; no display-sized headroom"},
                {"native_signal", "renderer E*max(NdotL,0)/pi times the same sampled noise multiplier as combined RGB; not divided RGB"},
                {"native_guides", "independent renderer signed view-Z R32F, world oct normal/rough1/material0 R10G10B10A2, UV/depth-delta RGBA16F"},
                {"translated_route", "production converter -> independent RR context -> production composition; Floor disabled; DirectDiffuse only"},
                {"native_route", "renderer native signals -> independent RR context -> independent GPU albedo remodulation"},
                {"albedo", "material quantized once to linear UNORM8 before shading; identical shared guide"},
                {"rounding", "shared pre-storage samples, not bit-identical encoded inputs: half(rho*D) versus rho*half(D)"},
                {"rr_pairing", "identical creation descriptor, six queried defaults explicitly applied to both contexts, identical reset schedule"},
                {"sr_pairing", "same loaded provider/configuration with independent contexts; common renderer hardware depth and pixel motion"},
                {"warmup_frames", mf.warmupFrames}, {"measured_frames", mf.measuredFrames},
                {"timing_scope", "serial validation/readback diagnostic GPU timestamps, not a throughput benchmark; native has no conversion stage; one-shot readbacks excluded"},
                {"reference", "independently evaluated noise-free analytic radiance; jittered render samples; unjittered target spatial box integral"},
                {"target_truth_samples_per_pixel", 16}, {"target_truth_filter", "4x4 stratified midpoint box; no noisy-image resampling"},
                {"quality_verdict", "not inferred from API success; compare native_presr/native_sr against translated presr/sr and clean truth"}}).dump() << ",\n";
        js << "  \"adapter\": \"" << JsonEscape(WideToUtf8(gpu.adapterDesc.Description)) << "\",\n";
        js << "  \"adapter_luid\": " << mf.adapterLuid << ",\n";
        js << "  \"adapter_vendor_id\": " << gpu.adapterDesc.VendorId << ",\n";
        js << "  \"dedicated_memory_mb\": " << (gpu.adapterDesc.DedicatedVideoMemory >> 20) << ",\n";
        js << "  \"debug_layer\": " << (gpu.debugLayer ? "true" : "false") << ",\n";
        js << "  \"denoiser_dll\": \"" << JsonEscape(WideToUtf8(denoiser.loadedPath.c_str())) << "\",\n";
        js << "  \"denoiser_requested\": \"" << JsonEscape(denoiser.requested.string()) << "\",\n";
        js << "  \"denoiser_sha256\": \"" << denoiser.sha256 << "\",\n";
        js << "  \"denoiser_size\": " << denoiser.size << ",\n";
        js << "  \"denoiser_file_version\": \"" << JsonEscape(denoiser.fileVersion) << "\",\n";
        js << "  \"upscaler_dll\": \"" << JsonEscape(WideToUtf8(upscaler.loadedPath.c_str())) << "\",\n";
        js << "  \"upscaler_sha256\": \"" << upscaler.sha256 << "\",\n";
        js << "  \"upscaler_size\": " << upscaler.size << ",\n";
        js << "  \"upscaler_file_version\": \"" << JsonEscape(upscaler.fileVersion) << "\",\n";
        js << "  \"rr_context\": {\"version\": " << FFX_DENOISER_VERSION << ", \"maxRenderSize\": [" << maxRenderW
           << ", " << maxRenderH << "], \"signalFlags\": " << rrCreate.signalFlags
           << ", \"createFlags\": " << rrCreate.flags << "},\n";
        js << "  \"denoiser_memory_usage\": " << nlohmann::json({
            {"predicted_queried", rrPredictedMemQueried},
            {"predicted_total_bytes", rrPredictedMem.totalUsageInBytes},
            {"predicted_aliasable_bytes", rrPredictedMem.aliasableUsageInBytes},
            {"actual_queried", rrActualMemQueried},
            {"actual_total_bytes", rrActualMem.totalUsageInBytes},
            {"actual_aliasable_bytes", rrActualMem.aliasableUsageInBytes}}).dump() << ",\n";
        js << "  \"diffuse_signal\": \"" << mf.diffuseSignal << "\", \"specular_signal\": \"" << mf.specularSignal
           << "\",\n";
        js << "  \"settings_source\": \"frozen generation: explicit canonical Config.h/feature controls pinned at 2c2f619b; current generation: explicit fixture preferences and production profile policy; six RR keys intentionally queried-provider defaults in both; distinct Floor algorithms/policies require explicit comparison policy, not identical-math claims\",\n";
        js << "  \"denoiser_defaults\": {\n";
        for (size_t i = 0; i < std::size(defaults); ++i)
            js << "    \"" << defaults[i].name << "\": {\"queried\": "
               << (defaults[i].queried ? "true" : "false") << ", \"value\": " << defaults[i].value << "}"
               << (i + 1 < std::size(defaults) ? "," : "") << "\n";
        js << "  },\n";
        js << "  \"seed\": " << mf.seed << ", \"frames_per_scene\": "
           << (fixtureDir.empty() ? perSceneFrames : capture.scene.frames)
           << ", \"in_flight\": " << ringSize << ",\n";
        js << "  \"validation_errors\": " << gpu.validationErrors << ", \"validation_warnings\": "
           << gpu.validationWarnings << ", \"sdk_errors\": " << g_sdkErrors
           << ", \"sdk_warnings\": " << g_sdkWarnings << ",\n";
        js << "  \"tracker_recorded\": " << trackerRecorded << ", \"tracker_retired\": " << trackerRetired << ",\n";
        js << "  \"scenes\": [";
        for (size_t i = 0; i < scenes.size(); ++i)
            js << "\"" << scenes[i].name << "\"" << (i + 1 < scenes.size() ? ", " : "");
        js << "],\n";
        js << "  \"motion_z_limitation\": " << nlohmann::json(nativeComparison
            ? "static first scene: exact zero renderer UV/depth-delta motion; no motion quality claim"
            : "product converter reconstructs camera-only signed depth motion; fixture XY is full reprojection; no per-object Z claim").dump() << ",\n";
        js << "  \"execution_result\": \"" << (anyFailure ? "FAIL" : "PASS") << "\",\n"
           << "  \"execution_result_scope\": \"execution only: API return codes, output finiteness, D3D12/SDK "
              "validation, GpuBatch ticket retirement, DeferredRelease retirement, fixture noise sanity. This is "
              "NOT a quality verdict; native-diffuse is a paired synthetic diagnostic, not ordinary generation "
              "comparison acceptance; other modes use compare_runs.py baseline-vs-candidate bounded metric deltas.\",\n"
           << "  \"feature_coverage\": \"Selected feature_case and per-frame feature_controls are authoritative; candidate-only cases never claim frozen-baseline parity. Composition graph equivalence is a separate shader fixture.\",\n"
           << "  \"input_truth\": \"" << (fixtureDir.empty()
               ? std::string("raw/presr use exact jittered sample truth; SR uses unjittered target truth; demodulated lobe metrics are diagnostic only")
               : std::string("captured engine chain; metrics only against the producer-published target PFM "
                             "truth, absent stages report images and finiteness only")) << "\",\n";
        if (!fixtureDir.empty())
        {
            js << "  \"capture_dir\": \"" << JsonEscape(fixtureDir.string()) << "\",\n";
            js << "  \"capture_provenance\": \"" << JsonEscape(capture.provenance) << "\",\n";
            js << "  \"capture_frames\": " << capture.frameMetadata.dump() << ",\n";
            js << "  \"capture_input_class\": \"clean-raster-engine-chain-not-noisy-RT\",\n";
            js << "  \"specular_guide_semantics\": \"Godot Fresnel F0; not hemispherical BRDF reflectance\",\n";
            js << "  \"replay_outputs\": \"replay/frameNNNN.raw; target linear RGBA16F, tight rows\",\n";
        }
        js << "  \"noise_model\": \""
           << (fixtureDir.empty()
                   ? "multiplicative lognormal, E=1 (positive unbiased), relative amplitude per scene"
                   : "none added; captured clean raster engine chain, not a noisy ray-tracing fixture")
           << "\"\n}\n";
        std::ofstream f(outDir / "run.json");
        f << js.str();
        f.close();
        if (!f)
            Die("cannot write run.json");
    }

    fprintf(stderr,
            "[%s] %s: validation_errors=%u validation_warnings=%u sdk_errors=%u sdk_warnings=%u tickets=%llu/%llu\n",
            mode.c_str(), anyFailure ? "FAIL" : "PASS", gpu.validationErrors, gpu.validationWarnings, g_sdkErrors,
            g_sdkWarnings, (unsigned long long) trackerRetired, (unsigned long long) trackerRecorded);
    return anyFailure ? 1 : 0;
}

int main(int argc, char** argv)
{
    try
    {
        return GuardedMain(argc, argv);
    }
    catch (const Fatal& e)
    {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "FAIL(std): %s\n", e.what());
        return 1;
    }
}
