#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void Require(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static std::wstring Widen(const char *s)
{
    return std::wstring(s, s + std::strlen(s));
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: lmxxf_nr_abi.exe <LmxxfNrRuntime.dll> [modules_dir]\n");
        return 2;
    }

    const std::wstring path = Widen(argv[1]);
    HMODULE dll = LoadLibraryW(path.c_str());
    Require(dll != nullptr, "LoadLibraryW");

    auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi *)>(
        GetProcAddress(dll, "LmxxfNrGetApi"));
    Require(getApi != nullptr, "GetProcAddress LmxxfNrGetApi");
    auto getTiming = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrTimingApi*)>(
        GetProcAddress(dll, "LmxxfNrGetTimingApi"));
    Require(getTiming != nullptr, "timing extension export");
    struct { LmxxfNrTimingApi api; uint64_t canary; } telemetry {};
    telemetry.canary = 0x123456789abcdef0ull;
    telemetry.api.struct_size = sizeof telemetry.api;
    Require(getTiming(99, &telemetry.api) == LMXXF_NR_UNSUPPORTED_ABI, "unknown timing ABI");
    --telemetry.api.struct_size;
    Require(getTiming(NR_TIMING_VERSION, &telemetry.api) == LMXXF_NR_INVALID_ARGUMENT, "short timing table");
    telemetry.api.struct_size = sizeof telemetry.api;
    Require(getTiming(NR_TIMING_VERSION, &telemetry.api) == LMXXF_NR_OK, "timing negotiation");
    Require(telemetry.canary == 0x123456789abcdef0ull, "timing table bounds");
    Require(telemetry.api.SetEnabled(nullptr, 1) == LMXXF_NR_INVALID_ARGUMENT, "timing requires session");

    LmxxfNrApi rejected {}; rejected.struct_size = sizeof rejected;
    Require(getApi(3, &rejected) == LMXXF_NR_UNSUPPORTED_ABI, "ABI3 rejected; install complete package");
    static_assert(sizeof(LmxxfNrJob) == 32, "v4 first-pass job output");
    Require(getApi(2, &rejected) == LMXXF_NR_UNSUPPORTED_ABI, "ABI2 rejected; install complete package");
    Require(getApi(1, &rejected) == LMXXF_NR_UNSUPPORTED_ABI, "old ABI rejected; install complete package");
    rejected.struct_size = 168;
    Require(getApi(LMXXF_NR_ABI_VERSION, &rejected) == LMXXF_NR_INVALID_ARGUMENT, "old package table rejected");
    static_assert(sizeof(LmxxfNrTimings) == 24);
    LmxxfNrApi api {};
    api.struct_size = sizeof(api);
    Require(getApi(99, &api) == LMXXF_NR_UNSUPPORTED_ABI, "unsupported abi");

    api = {};
    api.struct_size = sizeof(api);
    Require(getApi(LMXXF_NR_ABI_VERSION, &api) == LMXXF_NR_OK, "GetApi");
    Require(api.abi_version == LMXXF_NR_ABI_VERSION, "abi_version");
    Require(api.QueryCapabilities && api.Create && api.Destroy, "required pointers");
    Require(api.RecordInputs && api.EnqueueHip && api.RecordOutputs, "record/enqueue pointers");
    Require(api.GetLastError && api.GetStatus && api.GetTimings, "error and network timing pointers");
    Require(api.BeginRecordingExecution && api.EndRecordingExecution &&
            api.InvalidateRecording && api.CollectRecording, "v2 recording pointers");

    LmxxfNrCapabilities caps {};
    caps.struct_size = sizeof(caps);
    Require(api.QueryCapabilities(&caps) == LMXXF_NR_OK, "QueryCapabilities");
    {
        const char *fit = std::getenv("DLSS5_FIT_LARGE");
        const bool fitLarge = fit && fit[0] == '1' && !fit[1];
        if (fitLarge)
            Require(caps.max_input_width == 16384 && caps.max_input_height == 16384, "max input fit-large");
        else
            Require(caps.max_input_width == 1920 && caps.max_input_height == 1080, "max input");
    }
    Require(caps.history_supported == 1 && caps.overlap_supported == 0, "native history capability; no overlap");
    Require(caps.graph_supported == 0, "graph off");
    Require(caps.gfx1201_target == 1, "gfx1201 target");

    void *ctx = reinterpret_cast<void *>(1);
    LmxxfNrCreateInfo info {};
    info.struct_size = sizeof(info);
    Require(api.Create(&info, &ctx) == LMXXF_NR_INVALID_ARGUMENT, "Create without device");
    Require(ctx == nullptr, "Create failure clears context");

    info.device = reinterpret_cast<void *>(0x100);
    info.queue = reinterpret_cast<void *>(0x200);
    /* assets_directory is now required (P2 step 2). */
    Require(api.Create(&info, &ctx) == LMXXF_NR_INVALID_ARGUMENT, "Create without assets_directory");
    Require(ctx == nullptr, "Create without assets clears context");

    auto resolveArch = reinterpret_cast<int32_t (*)(const wchar_t *, const char *, wchar_t *, uint32_t, char *, uint32_t)>(
        GetProcAddress(dll, "LmxxfNrResolveArchModules"));
    Require(resolveArch != nullptr, "GetProcAddress LmxxfNrResolveArchModules");

    if (argc >= 3)
    {
        const std::wstring modules = Widen(argv[2]);

        // Pure path resolution tests
        wchar_t outPath[MAX_PATH] {};
        char errBuf[256] {};
        Require(resolveArch(modules.c_str(), "gfx1200", outPath, MAX_PATH, errBuf, sizeof(errBuf)) == LMXXF_NR_OK,
                "ResolveArchModules gfx1200");
        Require(std::wstring(outPath).find(L"gfx1200") != std::wstring::npos, "resolved gfx1200 path");

        Require(resolveArch(modules.c_str(), "gfx1201", outPath, MAX_PATH, errBuf, sizeof(errBuf)) == LMXXF_NR_OK,
                "ResolveArchModules gfx1201");
        Require(std::wstring(outPath).find(L"gfx1201") != std::wstring::npos, "resolved gfx1201 path");

        Require(resolveArch(modules.c_str(), "gfx1100", outPath, MAX_PATH, errBuf, sizeof(errBuf)) == LMXXF_NR_UNAVAILABLE,
                "ResolveArchModules rejects unsupported arch");
        Require(std::strstr(errBuf, "unsupported HIP architecture") != nullptr,
                "ResolveArchModules error diagnostics");

        const std::wstring leafDir = modules + L"\\gfx1201";
        Require(resolveArch(leafDir.c_str(), "gfx1201", outPath, MAX_PATH, errBuf, sizeof(errBuf)) == LMXXF_NR_OK,
                "ResolveArchModules flat leaf");
        Require(std::wstring(outPath) == leafDir, "ResolveArchModules leaf identity");

        // Dual-arch Create & Status
        info.assets_directory = modules.c_str();
        Require(api.Create(&info, &ctx) == LMXXF_NR_OK, "Create with dual-arch modules directory");
        Require(ctx != nullptr, "session handle with modules");
        LmxxfNrTimings net {}; net.struct_size = sizeof net;
        Require(api.GetTimings(ctx, &net) == LMXXF_NR_OK && !net.valid, "network unavailable without HIP");
        --net.struct_size;
        Require(api.GetTimings(ctx, &net) == LMXXF_NR_INVALID_ARGUMENT, "short network timing payload");
        NrTimingSnapshot timing {}; timing.struct_size = sizeof timing;
        Require(telemetry.api.GetSnapshot(ctx, &timing) == LMXXF_NR_OK && !timing.enabled, "timing defaults off");
        Require(telemetry.api.SetEnabled(ctx, 1) == LMXXF_NR_OK, "enable telemetry without GPU");
        Require(telemetry.api.GetSnapshot(ctx, &timing) == LMXXF_NR_OK && timing.enabled, "read enabled telemetry");
        Require(!timing.stages[NR_GPU_NETWORK].samples, "no invented GPU duration");
        --timing.struct_size;
        Require(telemetry.api.GetSnapshot(ctx, &timing) == LMXXF_NR_INVALID_ARGUMENT, "short timing snapshot");
        Require(telemetry.api.SetEnabled(ctx, 2) == LMXXF_NR_INVALID_ARGUMENT, "invalid timing switch");

        char status[256] {};
        Require(api.GetStatus(ctx, status, sizeof status) == LMXXF_NR_OK, "GetStatus modules");
        std::string st(status);
        Require(st.find("modules_ok=80") != std::string::npos, "status reports modules_ok=80");
        Require(st.find("arch=unknown") != std::string::npos, "status reports arch=unknown before bridge");
        Require(st.find("pdl=0/0(unknown)") != std::string::npos, "status reports pdl=0/0(unknown) before bridge");
        Require(st.find("hip=0") != std::string::npos, "status hip still 0");

        Require(api.PrepareSession(ctx) == LMXXF_NR_INVALID_ARGUMENT,
                "PrepareSession rejects non-D3D12 placeholder device");
        Require(api.RecordInputs(ctx, nullptr, nullptr) == LMXXF_NR_NOT_IMPLEMENTED,
                "RecordInputs stays unwired without PrepareSession");
        Require(api.EnqueueHip(ctx, nullptr, nullptr) == LMXXF_NR_NOT_IMPLEMENTED, "EnqueueHip stays unwired");
        Require(api.RecordOutputs(ctx, nullptr, nullptr) == LMXXF_NR_NOT_IMPLEMENTED,
                "RecordOutputs stays unwired");

        char err[256] {};
        Require(api.GetLastError(err, sizeof err) == LMXXF_NR_OK, "GetLastError after not-wired");
        Require(err[0] != 0, "last error populated");

        Require(api.Destroy(ctx) == LMXXF_NR_OK, "Destroy modules session");

        // Leaf / flat directory Create & Status
        info.assets_directory = leafDir.c_str();
        Require(api.Create(&info, &ctx) == LMXXF_NR_OK, "Create with leaf modules directory");
        Require(ctx != nullptr, "session handle with leaf modules");
        Require(api.GetStatus(ctx, status, sizeof status) == LMXXF_NR_OK, "GetStatus leaf modules");
        st = status;
        Require(st.find("modules_ok=40") != std::string::npos, "status reports modules_ok=40 for leaf");
        Require(st.find("arch=unknown") != std::string::npos, "status reports arch=unknown for leaf");
        Require(st.find("pdl=0/0(unknown)") != std::string::npos, "status reports pdl=0/0(unknown) for leaf");
        Require(api.Destroy(ctx) == LMXXF_NR_OK, "Destroy leaf session");
    }
    else
    {
        std::printf("note: no modules_dir; skipping module-path Create checks\n");
    }

    Require(FreeLibrary(dll), "FreeLibrary");
    std::printf("lmxxf_nr_abi: ok\n");
    return 0;
}
