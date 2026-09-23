#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#define LOG_WARN(...) ((void)0)
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfEvaluateCut.h"
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

static int32_t FakeEnqueue(void *session, void *job, void *queue)
{
    (void)session;
    (void)job;
    (void)queue;
    return 0;
}

int main()
{
    ID3D12Device *device = MakeDevice();
    Require(device != nullptr, "device");
    ID3D12Device4 *device4 = nullptr;
    Check(device->QueryInterface(IID_PPV_ARGS(&device4)), "Device4");
    Check(DlssNr::Submission::Hooks::ArmCreate(device), "ArmCreate");
    DlssNr::Submission::Hooks::SetProxyWrap(true);

    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");

    ID3D12GraphicsCommandList *list = nullptr;
    Check(device4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                      IID_PPV_ARGS(&list)),
          "CreateCommandList1");

    DlssNr::Submission::ILogicalCommandList *logical = nullptr;
    Require(SUCCEEDED(list->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                            reinterpret_cast<void **>(&logical))) &&
                logical,
            "List1 returns proxy");
    logical->Release();

    ID3D12Device *devFromList = nullptr;
    Check(list->GetDevice(IID_PPV_ARGS(&devFromList)), "GetDevice before Reset");
    Require(devFromList == device, "GetDevice identity");
    devFromList->Release();

    // Closed before Reset: Split must fail (not recording).
    Require(FAILED(DlssNr::Backend::LmxxfCut::TrySplitAtEvaluate(list)), "split before Reset fails");

    ID3D12CommandAllocator *alloc = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
    Check(list->Reset(alloc, nullptr), "Reset binds allocator");

    Check(DlssNr::Backend::LmxxfCut::TrySplitAtEvaluate(list), "split after Reset");
    auto &pending = DlssNr::Backend::LmxxfCut::Pending();
    pending.betweenHits.store(0);
    DlssNr::Backend::LmxxfCut::SetPendingEnqueue(reinterpret_cast<void *>(1), reinterpret_cast<void *>(2),
                                                 &FakeEnqueue, list);
    DlssNr::Backend::LmxxfCut::ArmBetweenSlot();
    Check(list->Close(), "close continuation");

    ID3D12CommandList *batch[] = {list};
    const auto between = DlssNr::Submission::Hooks::GetBetween();
    DlssNr::Submission::Hooks::ExecuteExpanded(
        queue, 1, batch, between.fn, between.ctx,
        [](ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *c) { q->ExecuteCommandLists(n, c); });
    Require(pending.betweenHits.load() == 1, "between after List1 wrap");

    DlssNr::Backend::LmxxfCut::DisarmBetweenSlot();
    DlssNr::Submission::Hooks::Disarm();
    list->Release();
    alloc->Release();
    queue->Release();
    device4->Release();
    device->Release();
    std::printf("lmxxf_list1_wrap: ok\n");
    return 0;
}
