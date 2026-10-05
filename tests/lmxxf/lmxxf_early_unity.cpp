#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#define LOG_WARN(...) ((void)0)
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/SubmissionHooks.h"

namespace H = DlssNr::Submission::Hooks;
namespace O = DlssNr::Submission::CreationDiagnostic;
using DlssNr::Submission::ILogicalCommandList;
using CreateFn = HRESULT(WINAPI *)(ID3D12Device *, ID3D12CommandAllocator *,
                                  D3D12_COMMAND_LIST_TYPE, BOOL, ID3D12GraphicsCommandList **);

static void Require(bool ok, const char *what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
static void Check(HRESULT hr, const char *what)
{
    if (FAILED(hr)) { std::fprintf(stderr, "FAIL: %s (%08lx)\n", what, hr); std::exit(1); }
}
static bool IsLogical(ID3D12GraphicsCommandList *list)
{
    ILogicalCommandList *logical = nullptr;
    const bool ok = SUCCEEDED(list->QueryInterface(__uuidof(ILogicalCommandList),
                                                   reinterpret_cast<void **>(&logical)));
    if (logical) logical->Release();
    return ok;
}
static void WaitIdle(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    ID3D12Fence *fence = nullptr;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "event");
    Check(queue->Signal(fence, 1), "signal");
    Check(fence->SetEventOnCompletion(1, event), "event completion");
    Require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU completion");
    Require(fence->GetCompletedValue() == 1, "fence completed without device removal");
    CloseHandle(event); fence->Release();
}
static ID3D12Resource *Buffer(ID3D12Device *device, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp {}; hp.Type = heap;
    D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *resource = nullptr;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state,
                                         nullptr, IID_PPV_ARGS(&resource)), "buffer");
    return resource;
}
static void Between(ID3D12CommandQueue *, ID3D12CommandList *list, void *context)
{
    Require(list != nullptr, "between list");
    ++*static_cast<int *>(context);
}
static void ExerciseRetained(ID3D12Device *device, ID3D12CommandQueue *queue,
                             ID3D12CommandAllocator *allocator, ID3D12GraphicsCommandList *list)
{
    ILogicalCommandList *logical = nullptr;
    Check(list->QueryInterface(__uuidof(ILogicalCommandList), reinterpret_cast<void **>(&logical)), "logical");
    IUnknown *identity = nullptr, *logicalIdentity = nullptr;
    Check(list->QueryInterface(IID_PPV_ARGS(&identity)), "identity");
    Check(logical->QueryInterface(IID_PPV_ARGS(&logicalIdentity)), "logical identity");
    Require(identity == logicalIdentity, "COM identity");
    identity->Release(); logicalIdentity->Release();
    auto *upload = Buffer(device, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto *readback = Buffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    int hits = 0;
    H::SetBetween(Between, &hits);
    for (unsigned pass = 0; pass != 2; ++pass)
    {
        unsigned char expected[256];
        for (unsigned i = 0; i != 256; ++i) expected[i] = static_cast<unsigned char>(i ^ (pass * 73));
        void *mapped = nullptr;
        Check(upload->Map(0, nullptr, &mapped), "upload map");
        std::memcpy(mapped, expected, sizeof(expected)); upload->Unmap(0, nullptr);
        Require(logical->CapturedViewportCount() == 0, "clean state after creation/reset");
        const auto generation = logical->RecordingGeneration();
        D3D12_VIEWPORT viewport {0, 0, 128, 72, 0, 1};
        list->RSSetViewports(1, &viewport);
        list->CopyBufferRegion(readback, 0, upload, 0, 128);
        Check(logical->SplitSegments(), "split retained list");
        Require(logical->CapturedViewportCount() == 1, "viewport preserved across split");
        list->CopyBufferRegion(readback, 128, upload, 128, 128);
        Check(list->Close(), "close");
        ID3D12CommandList *batch[] {list}; queue->ExecuteCommandLists(1, batch);
        WaitIdle(device, queue);
        D3D12_RANGE range {0, 256};
        Check(readback->Map(0, &range, &mapped), "readback map");
        Require(std::memcmp(mapped, expected, sizeof(expected)) == 0, "split output equals original pattern");
        readback->Unmap(0, nullptr);
        Require(hits == static_cast<int>(pass + 1), "one between callback per submission");
        Check(allocator->Reset(), "allocator reset");
        Check(list->Reset(allocator, nullptr), "retained list reset");
        Require(generation.Discarded(), "old generation retired by reset");
    }
    Check(list->Close(), "final close"); H::SetBetween(nullptr, nullptr);
    logical->Release(); upload->Release(); readback->Release();
}

int wmain(int argc, wchar_t **argv)
{
    Require(argc == 3, "two fixture DLL paths required");
    HMODULE unityModule = LoadLibraryW(argv[1]), otherModule = LoadLibraryW(argv[2]);
    Require(unityModule && otherModule && unityModule != otherModule, "load distinct fixtures");
    auto unity = reinterpret_cast<CreateFn>(GetProcAddress(unityModule, "CreateFixtureList"));
    auto other = reinterpret_cast<CreateFn>(GetProcAddress(otherModule, "CreateFixtureList"));
    Require(unity && other, "fixture exports");
    ID3D12Device *device = nullptr;
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12 device");
    D3D12_COMMAND_QUEUE_DESC qd {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    Check(H::Arm(device, queue), "arm create and execution hooks before admission");
    H::SetWrapOpenLists(true);
    ID3D12CommandAllocator *allocator = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    ID3D12GraphicsCommandList *old = nullptr;
    Check(unity(device, allocator, D3D12_COMMAND_LIST_TYPE_DIRECT, FALSE, &old), "old boot list");
    Require(!IsLogical(old), "old behavior reproduces raw Unity boot list");
    H::SetEarlyUnityPlayerWrap(true);
    Require(!IsLogical(old), "late admission cannot repair retained native object");
    Check(old->Close(), "close old"); old->Release();
    Require(!H::ShouldWrapCreate(nullptr), "null caller excluded");

    for (auto creator : {other, unity})
    {
        for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY})
        {
            for (BOOL closed : {FALSE, TRUE})
            {
                ID3D12CommandAllocator *a = nullptr;
                Check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&a)), "typed allocator");
                ID3D12GraphicsCommandList *list = nullptr;
                Check(creator(device, a, type, closed, &list), "fixture create");
                const bool expected = creator == unity && type == D3D12_COMMAND_LIST_TYPE_DIRECT;
                Require(IsLogical(list) == expected, "only Unity DIRECT admitted");
                O::Stamp stamp;
                Require(O::Read(list, stamp) && stamp.wrapped == (expected ? 1u : 0u) &&
                            stamp.api == static_cast<unsigned>(closed), "creation provenance");
                Require((stamp.gates & O::EarlyUnityPlayerWrap) && !(stamp.gates & O::ProxyWrap), "early module gate only");
                Require(std::strcmp(stamp.callerModule, creator == unity ? "UnityPlayer.dll" : "OtherEngine.dll") == 0, "real DLL caller recorded");
                if (expected)
                {
                    if (closed) Check(list->Reset(a, nullptr), "first reset of closed list");
                    H::SetEarlyUnityPlayerWrap(false); H::SetProxyWrap(true);
                    ExerciseRetained(device, queue, a, list);
                    H::SetProxyWrap(false); H::SetEarlyUnityPlayerWrap(true);
                }
                else if (!closed) Check(list->Close(), "close excluded list");
                list->Release(); a->Release();
            }
        }
    }
    ID3D12GraphicsCommandList *native = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                   IID_PPV_ARGS(&native)), "EXE caller");
    Require(!IsLogical(native), "Unity admission does not admit EXE caller");
    Check(native->Close(), "EXE close"); native->Release();
    H::SetWrapOpenLists(false);
    Check(unity(device, allocator, D3D12_COMMAND_LIST_TYPE_DIRECT, FALSE, &native), "open-list gate");
    Require(!IsLogical(native), "open-list opt-out preserved");
    Check(native->Close(), "open-list close"); native->Release(); H::SetWrapOpenLists(true);
    {
        DlssNr::Submission::SuppressProxyWrap suppress;
        Check(unity(device, allocator, D3D12_COMMAND_LIST_TYPE_DIRECT, FALSE, &native), "suppressed create");
        Require(!IsLogical(native), "internal creation suppression preserved");
        Check(native->Close(), "suppressed close"); native->Release();
    }
    H::SetEarlyUnityPlayerWrap(false);
    Check(unity(device, allocator, D3D12_COMMAND_LIST_TYPE_DIRECT, FALSE, &native), "disabled create");
    Require(!IsLogical(native), "explicit early disable preserved");
    Check(native->Close(), "disabled close"); native->Release();
    H::SetEarlyUnityPlayerWrap(true); H::Disarm();
    Require(!H::g_earlyUnityPlayerWrap.load(), "disarm resets module gate");
    allocator->Release(); queue->Release(); device->Release();
    FreeLibrary(otherModule); FreeLibrary(unityModule);
    std::puts("lmxxf_early_unity: PASS (real DLL callers, retained open/closed lists, split GPU readback, reset, exclusions)");
    return 0;
}
