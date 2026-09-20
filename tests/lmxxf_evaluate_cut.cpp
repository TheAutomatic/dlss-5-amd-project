#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfEvaluateCut.h"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/SubmissionHooks.h"
#include <cstdio>
#include <cstdint>

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
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
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

static int32_t FakeEnqueue(void *session, void *job)
{
    Require(session == reinterpret_cast<void *>(0x1111), "session");
    Require(job == reinterpret_cast<void *>(0x2222), "job");
    return 0;
}

int main()
{
    ID3D12Device *device = MakeDevice();
    Require(device != nullptr, "device");
    Check(DlssNr::Submission::Hooks::ArmCreate(device), "ArmCreate");

    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    // Expand path used by AmdBridge when armed.
    DlssNr::Submission::Hooks::g_expandEnabled.store(true, std::memory_order_release);

    // Non-proxy: S_FALSE
    {
        ID3D12CommandAllocator *a = nullptr;
        ID3D12GraphicsCommandList *raw = nullptr;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "a0");
        {
            DlssNr::Submission::SuppressProxyWrap suppress;
            Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&raw)), "raw");
        }
        Require(DlssNr::Backend::LmxxfCut::TrySplitAtEvaluate(raw) == S_FALSE, "non-proxy S_FALSE");
        raw->Close();
        raw->Release();
        a->Release();
    }

    ID3D12CommandAllocator *alloc = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
    ID3D12GraphicsCommandList *list = nullptr;
    // Must be a proxied logical list: product CreateCommandList is not wrapped (CL1-only).
    Check(DlssNr::Submission::Hooks::CreateProxiedCommandList(
              device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)),
          "CreateProxiedCommandList");
    Require(list != nullptr, "proxy list");

    // Evaluate cut: Split + pending EnqueueHip + ArmBetween
    const HRESULT splitHr = DlssNr::Backend::LmxxfCut::TrySplitAtEvaluate(list);
    Require(splitHr == S_OK, "TrySplitAtEvaluate must return S_OK on proxy (not S_FALSE)");
    auto &pending = DlssNr::Backend::LmxxfCut::Pending();
    pending.betweenHits.store(0);
    pending.enqueueCalls.store(0);
    pending.skippedHits.store(0);
    pending.lastEnqueueRc.store(-1);
    DlssNr::Backend::LmxxfCut::SetPendingEnqueue(reinterpret_cast<void *>(0x1111), reinterpret_cast<void *>(0x2222),
                                                 &FakeEnqueue);
    DlssNr::Backend::LmxxfCut::ArmBetweenSlot();

    // Record a trivial clear on continuation side after split
    list->Close();
    ID3D12CommandList *batch[] = {list};
    DlssNr::Submission::Hooks::ExecuteExpanded(queue, 1, batch, DlssNr::Submission::Hooks::g_between,
                                               DlssNr::Submission::Hooks::g_betweenCtx,
                                               [](ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *c) {
                                                   q->ExecuteCommandLists(n, c);
                                               });

    Require(pending.betweenHits.load() == 1, "betweenHits");
    Require(pending.enqueueCalls.load() == 1, "enqueueCalls must be 1 (real Enqueue)");
    Require(pending.skippedHits.load() == 0, "skippedHits must be 0");
    Require(pending.lastEnqueueRc.load() == 0, "EnqueueHip rc");

    DlssNr::Backend::LmxxfCut::DisarmBetweenSlot();
    DlssNr::Submission::Hooks::Disarm();
    list->Release();
    alloc->Release();
    queue->Release();
    device->Release();
    std::printf("lmxxf_evaluate_cut: ok (between→FakeEnqueue rc=0)\n");
    return 0;
}