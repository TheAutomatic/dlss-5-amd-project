#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h"
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

    LmxxfNrApi api {};
    api.struct_size = sizeof(api);
    Require(getApi(99, &api) == LMXXF_NR_UNSUPPORTED_ABI, "unsupported abi");

    api = {};
    api.struct_size = sizeof(api);
    Require(getApi(LMXXF_NR_ABI_VERSION, &api) == LMXXF_NR_OK, "GetApi");
    Require(api.abi_version == LMXXF_NR_ABI_VERSION, "abi_version");
    Require(api.QueryCapabilities && api.Create && api.Destroy, "required pointers");
    Require(api.RecordInputs && api.EnqueueHip && api.RecordOutputs, "record/enqueue pointers");
    Require(api.GetLastError && api.GetStatus, "error pointers");

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
    Require(caps.history_supported == 0 && caps.overlap_supported == 0, "history/overlap off");
    Require(caps.graph_supported == 0, "graph off");
    Require(caps.hip_ready == 0, "HIP not wired");
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

    if (argc >= 3)
    {
        const std::wstring modules = Widen(argv[2]);
        info.assets_directory = modules.c_str();
        Require(api.Create(&info, &ctx) == LMXXF_NR_OK, "Create with modules directory");
        Require(ctx != nullptr, "session handle with modules");

        char status[256] {};
        Require(api.GetStatus(ctx, status, sizeof status) == LMXXF_NR_OK, "GetStatus modules");
        const std::string st(status);
        Require(st.find("modules_ok=") != std::string::npos, "status reports modules_ok");
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
    }
    else
    {
        std::printf("note: no modules_dir; skipping module-path Create checks\n");
    }

    Require(FreeLibrary(dll), "FreeLibrary");
    std::printf("lmxxf_nr_abi: ok\n");
    return 0;
}
