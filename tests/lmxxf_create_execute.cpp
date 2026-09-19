#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/SubmissionHooks.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

static void Check(HRESULT hr, const char *what)
{
    if (FAILED(hr))
    {
        std::fprintf(stderr, "FAIL: %s hr=%08lx\n", what, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

static void Require(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static ID3D12Device *MakeDevice()
{
    IDXGIFactory4 *factory = nullptr;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ID3D12Device *device = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        const bool skip = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (!skip && SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
        {
            adapter->Release();
            break;
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    return device;
}

static void WaitIdle(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    ID3D12Fence *fence = nullptr;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    Check(queue->Signal(fence, 1), "signal");
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(ev != nullptr, "event");
    Check(fence->SetEventOnCompletion(1, ev), "set event");
    WaitForSingleObject(ev, 10000);
    CloseHandle(ev);
    fence->Release();
}

static ID3D12Resource *MakeBuffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE heap,
                                  D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = nullptr;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)),
          "buffer");
    return r;
}

struct BetweenCounter
{
    int hits = 0;
};

static void BetweenHit(void *ctx)
{
    reinterpret_cast<BetweenCounter *>(ctx)->hits += 1;
}

static std::vector<uint8_t> RunHookedSplit(ID3D12Device *device, ID3D12CommandQueue *queue, bool doSplit,
                                           bool markRenderPassIneligible)
{
    constexpr UINT64 kBytes = 256;
    std::vector<uint8_t> pattern(kBytes);
    for (UINT i = 0; i < kBytes; ++i)
        pattern[i] = static_cast<uint8_t>(0xB0 + (i & 0xF));

    ID3D12Resource *upload = MakeBuffer(device, kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *gpu = MakeBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *readback = MakeBuffer(device, kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    void *map = nullptr;
    Check(upload->Map(0, nullptr, &map), "map upload");
    std::memcpy(map, pattern.data(), kBytes);
    upload->Unmap(0, nullptr);

    ID3D12CommandAllocator *alloc = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");

    // Create goes through Detour -> CommandListProxy when armed.
    ID3D12GraphicsCommandList *list = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)),
          "create list");

    DlssNr::Submission::ILogicalCommandList *logical = nullptr;
    Check(list->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                               reinterpret_cast<void **>(&logical)),
          "qi logical");

    if (markRenderPassIneligible)
    {
        ID3D12GraphicsCommandList4 *l4 = nullptr;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l4))))
        {
            // Empty begin/end would need real RTVs; just calling Begin with 0 is enough to mark.
            l4->BeginRenderPass(0, nullptr, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
            l4->EndRenderPass();
            l4->Release();
        }
        Require(logical->IsSplitIneligible(), "expected ineligible after render pass");
        const HRESULT splitHr = logical->SplitSegments();
        Require(FAILED(splitHr), "split must fail when ineligible");
    }

    D3D12_VIEWPORT vp {};
    vp.Width = 128.0f;
    vp.Height = 72.0f;
    vp.MaxDepth = 1.0f;
    list->RSSetViewports(1, &vp);
    Require(logical->CapturedViewportCount() == 1, "viewport captured");

    list->CopyBufferRegion(gpu, 0, upload, 0, kBytes / 2);
    if (doSplit && !markRenderPassIneligible)
    {
        Check(logical->SplitSegments(), "split");
        // Continuation seeded; capture still reports the producer viewport seed.
        Require(logical->CapturedViewportCount() == 1, "viewport kept after split");
    }
    list->CopyBufferRegion(gpu, kBytes / 2, upload, kBytes / 2, kBytes / 2);

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = gpu;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(readback, 0, gpu, 0, kBytes);
    Check(list->Close(), "close");

    ID3D12CommandList *cmd = list;
    queue->ExecuteCommandLists(1, &cmd); // Detour expands proxy + between
    WaitIdle(device, queue);

    logical->Release();
    list->Release();
    alloc->Release();

    D3D12_RANGE range { 0, kBytes };
    void *out = nullptr;
    Check(readback->Map(0, &range, &out), "map readback");
    std::vector<uint8_t> got(kBytes);
    std::memcpy(got.data(), out, kBytes);
    readback->Unmap(0, nullptr);
    upload->Release();
    gpu->Release();
    readback->Release();
    return got;
}


static void TestOpenSplitBarrierReject(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    ID3D12CommandAllocator *alloc = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
    ID3D12GraphicsCommandList *list = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)),
          "list");
    DlssNr::Submission::ILogicalCommandList *logical = nullptr;
    Check(list->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                               reinterpret_cast<void **>(&logical)),
          "logical");

    ID3D12Resource *buf = MakeBuffer(device, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    D3D12_RESOURCE_BARRIER begin {};
    begin.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    begin.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    begin.Transition.pResource = buf;
    begin.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    begin.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    begin.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &begin);
    const HRESULT splitHr = logical->SplitSegments();
    Require(FAILED(splitHr), "open split barrier must refuse split");
    Require(logical->IsSplitIneligible(), "marked ineligible");

    // Close without completing the split barrier pair would be illegal on a real queue;
    // discard by releasing without Execute.
    list->Close();
    logical->Release();
    list->Release();
    alloc->Release();
    buf->Release();
    (void)queue;
}
int main()
{
    ID3D12Device *device = MakeDevice();
    Require(device != nullptr, "D3D12 device");
    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");

    Check(DlssNr::Submission::Hooks::Arm(device, queue), "arm hooks");
    Require(DlssNr::Submission::Hooks::IsArmed(), "armed");

    BetweenCounter counter;
    DlssNr::Submission::Hooks::SetBetween(BetweenHit, &counter);

    const auto unsplit = RunHookedSplit(device, queue, false, false);
    Require(counter.hits == 0, "no between without split");

    const auto split = RunHookedSplit(device, queue, true, false);
    Require(counter.hits == 1, "between once after split execute");
    Require(unsplit == split, "hooked split matches unsplit");
    Require(unsplit[0] == 0xB0 && unsplit[15] == 0xBF, "pattern");

    // Ineligible: split refused; still execute as single list (no between increment).
    const int hitsBefore = counter.hits;
    const auto ineligible = RunHookedSplit(device, queue, true, true);
    Require(counter.hits == hitsBefore, "no between when split refused");
    Require(ineligible == unsplit, "ineligible passthrough matches");

    TestOpenSplitBarrierReject(device, queue);

    DlssNr::Submission::Hooks::Disarm();
    Require(!DlssNr::Submission::Hooks::IsArmed(), "disarmed");

    queue->Release();
    device->Release();
    std::printf("lmxxf_create_execute: ok (between_hits=%d)\n", counter.hits);
    return 0;
}