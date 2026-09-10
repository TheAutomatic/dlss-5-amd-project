#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/RuntimeNotification.h"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/RuntimeHash.h"
#include <bcrypt.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#pragma comment(lib, "bcrypt.lib")

using Notify = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static unsigned executes = 0;
static void STDMETHODCALLTYPE CountExecute(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) { ++executes; }
template <class T> T& At(HMODULE module, size_t rva)
{
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(module) + rva);
}

int wmain(int argc, wchar_t** argv)
{
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
    unsigned char hash[32] {};
    BCRYPT_ALG_HANDLE alg {};
    assert(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0);
    assert(BCryptHash(alg, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()), hash, sizeof(hash)) == 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    assert(bytes.size() == 7248384 && std::memcmp(hash, AmdRuntimeSha256, sizeof(hash)) == 0);

    // No DllMain, imports, hooks, threads or HIP initialization. Only execute
    // the audited Notify path with native job -1, which makes no imported calls.
    auto module = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    assert(module);
    const auto call = reinterpret_cast<const unsigned char*>(module) + 0x9183;
    assert(call[0] == 0xff && call[1] == 0x15);
    std::int32_t displacement = 0;
    std::memcpy(&displacement, call + 2, sizeof(displacement));
    assert(0x9189 + displacement == 0x8daf8);
    auto notify = reinterpret_cast<Notify>(reinterpret_cast<uintptr_t>(module) + 0x9170);
    auto queue = reinterpret_cast<ID3D12CommandQueue*>(0x1234);
    auto list = reinterpret_cast<ID3D12CommandList*>(0x5678);
    auto onePass = [&](Notify callback)
    {
        At<void*>(module, 0x8cee8) = nullptr;
        At<ID3D12CommandQueue*>(module, 0x8cef0) = queue;
        At<ID3D12CommandList*>(module, 0x8d908) = list;
        At<int>(module, 0x8d910) = -1;
        At<void*>(module, 0x8d8c8) = nullptr;
        At<Notify>(module, 0x8daf8) = callback;
        notify(queue, 1, &list);
        assert(At<ID3D12CommandList*>(module, 0x8d908) == nullptr);
    };
    for (unsigned passes = 1; passes <= 3; ++passes)
    {
        executes = 0;
        for (unsigned i = 0; i < passes; ++i) onePass(CountExecute);
        assert(executes == passes); // Reproduces the old bridge's extra submissions.
        executes = 0;
        CountExecute(queue, 1, &list); // The bridge's single real submission.
        for (unsigned i = 0; i < passes; ++i) onePass(AmdPreSr::AlreadySubmitted);
        assert(executes == 1);
        std::cout << passes << " passes: original=" << passes << ", corrected=1; native pending consumed\n";
    }
    FreeLibrary(module);
}
