#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../third_party/lmxxf/include/LmxxfNrApi.h"
#include <cassert>
#include <cstdio>
#include <string>

static void Require(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: lmxxf_nr_abi.exe <LmxxfNrRuntime.dll>\n");
        return 2;
    }

    const std::wstring path(argv[1], argv[1] + std::strlen(argv[1]));
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
    Require(caps.max_input_width == 1920 && caps.max_input_height == 1080, "max input");
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
    Require(api.Create(&info, &ctx) == LMXXF_NR_OK, "Create stub session");
    Require(ctx != nullptr, "session handle");

    char status[128] {};
    Require(api.GetStatus(ctx, status, sizeof status) == LMXXF_NR_OK, "GetStatus");
    Require(std::string(status).find("not wired") != std::string::npos, "status text");

    Require(api.PrepareSession(ctx) == LMXXF_NR_NOT_IMPLEMENTED, "PrepareSession not wired");
    Require(api.RecordInputs(ctx, nullptr, nullptr) == LMXXF_NR_NOT_IMPLEMENTED, "RecordInputs");
    Require(api.EnqueueHip(ctx, nullptr) == LMXXF_NR_NOT_IMPLEMENTED, "EnqueueHip");
    Require(api.RecordOutputs(ctx, nullptr, nullptr) == LMXXF_NR_NOT_IMPLEMENTED, "RecordOutputs");

    char err[256] {};
    Require(api.GetLastError(err, sizeof err) == LMXXF_NR_OK, "GetLastError");
    Require(err[0] != 0, "last error populated");

    Require(api.Destroy(ctx) == LMXXF_NR_OK, "Destroy");
    Require(FreeLibrary(dll), "FreeLibrary");
    std::printf("lmxxf_nr_abi: ok\n");
    return 0;
}
