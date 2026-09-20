#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/LogicalList.h"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/CommandListProxy.h"
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

static std::vector<uint8_t> RunCopy(ID3D12Device *device, ID3D12CommandQueue *queue, bool doSplit, bool useProxy)
{
    constexpr UINT64 kBytes = 256;
    std::vector<uint8_t> pattern(kBytes);
    for (UINT i = 0; i < kBytes; ++i)
        pattern[i] = static_cast<uint8_t>(0xA0 + (i & 0xF));

    ID3D12Resource *upload = MakeBuffer(device, kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *gpu = MakeBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *readback = MakeBuffer(device, kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    void *map = nullptr;
    Check(upload->Map(0, nullptr, &map), "map upload");
    std::memcpy(map, pattern.data(), kBytes);
    upload->Unmap(0, nullptr);

    ID3D12CommandAllocator *alloc = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
    ID3D12GraphicsCommandList *list = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)), "list");

    ID3D12GraphicsCommandList *rec = list;
    DlssNr::Submission::CommandListProxy *proxy = nullptr;
    DlssNr::Submission::ILogicalCommandList *logical = nullptr;
    DlssNr::Submission::LogicalList book;
    if (useProxy)
    {
        Check(DlssNr::Submission::CommandListProxy::Create(device, alloc, list, &proxy), "proxy");
        rec = proxy;
        Check(proxy->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                    reinterpret_cast<void **>(&logical)),
              "logical qi");
        // List1..10 must QI on the proxy (full forward). Producer may or may not support each IID.
        static const IID kListIids[] = {
            __uuidof(ID3D12GraphicsCommandList1),  __uuidof(ID3D12GraphicsCommandList2),
            __uuidof(ID3D12GraphicsCommandList3),  __uuidof(ID3D12GraphicsCommandList4),
            __uuidof(ID3D12GraphicsCommandList5),  __uuidof(ID3D12GraphicsCommandList6),
            __uuidof(ID3D12GraphicsCommandList7),  __uuidof(ID3D12GraphicsCommandList8),
            __uuidof(ID3D12GraphicsCommandList9),  __uuidof(ID3D12GraphicsCommandList10),
        };
        for (const IID &iid : kListIids)
        {
            IUnknown *unk = nullptr;
            Require(proxy->QueryInterface(iid, reinterpret_cast<void **>(&unk)) == S_OK, "listN qi");
            Require(unk != nullptr, "listN ptr");
            unk->Release();
        }
        // Optional producer capability: OMSetDepthBounds via List1 (no-op if GPU rejects; must not crash).
        ID3D12GraphicsCommandList1 *l1 = nullptr;
        if (SUCCEEDED(proxy->QueryInterface(IID_PPV_ARGS(&l1))))
        {
            l1->OMSetDepthBounds(0.0f, 1.0f);
            l1->Release();
        }
    }
    else
        Check(book.BindProducer(device, alloc, list), "bind");

    rec->CopyBufferRegion(gpu, 0, upload, 0, kBytes);
    if (doSplit)
    {
        if (useProxy)
            Check(logical->SplitSegments(), "proxy split");
        else
        {
            Check(book.Split(), "split");
            rec = book.Current();
            Require(rec != list, "current is not producer");
        }
    }
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = gpu;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rec->ResourceBarrier(1, &b);
    rec->CopyBufferRegion(readback, 0, gpu, 0, kBytes);
    if (useProxy)
        Check(logical->ExecuteOn(queue), "proxy execute");
    else
        Check(book.Execute(queue), "execute");
    WaitIdle(device, queue);

    if (useProxy)
    {
        Check(proxy->Reset(alloc, nullptr), "proxy reset");
        logical->Release();
        proxy->Release();
    }
    else
    {
        Check(book.Reset(alloc, nullptr), "reset");
        Require(book.Generation() == 2, "generation 2");
        Require(!book.WasSplit(), "reset clears split");
    }

    D3D12_RANGE range { 0, kBytes };
    void *out = nullptr;
    Check(readback->Map(0, &range, &out), "map readback");
    std::vector<uint8_t> got(kBytes);
    std::memcpy(got.data(), out, kBytes);
    readback->Unmap(0, nullptr);

    list->Release();
    alloc->Release();
    upload->Release();
    gpu->Release();
    readback->Release();
    return got;
}

int main()
{
    ID3D12Device *device = MakeDevice();
    Require(device != nullptr, "D3D12 device");
    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");

    const auto unsplit = RunCopy(device, queue, false, false);
    const auto split = RunCopy(device, queue, true, false);
    const auto proxied = RunCopy(device, queue, true, true);
    Require(unsplit == split, "split passthrough matches unsplit");
    Require(unsplit == proxied, "COM proxy split matches unsplit");

    // Admission: query on producer ? Split ineligible (ordinary SR path).
    {
        ID3D12CommandAllocator *a = nullptr;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "adm_a");
        ID3D12GraphicsCommandList *raw = nullptr;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&raw)), "adm_raw");
        DlssNr::Submission::CommandListProxy *px = nullptr;
        Check(DlssNr::Submission::CommandListProxy::Create(device, a, raw, &px), "adm_proxy");
        ID3D12QueryHeap *qh = nullptr;
        D3D12_QUERY_HEAP_DESC qhd {};
        qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhd.Count = 2;
        Check(device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)), "query heap");
        px->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        Require(px->IsSplitIneligible(), "query makes split ineligible");
        Require(FAILED(px->SplitSegments()), "split refused after query");
        px->Close();
        px->Release();
        raw->Release();
        qh->Release();
        a->Release();
    }

    Require(unsplit[0] == 0xA0 && unsplit[15] == 0xAF, "pattern");

    // Create -> Close -> Reset (never Execute) must succeed.
    {
        ID3D12CommandAllocator *a1 = nullptr;
        ID3D12CommandAllocator *a2 = nullptr;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a1)), "a1");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a2)), "a2");
        ID3D12GraphicsCommandList *raw = nullptr;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a1, nullptr, IID_PPV_ARGS(&raw)), "raw");
        DlssNr::Submission::CommandListProxy *proxy = nullptr;
        Check(DlssNr::Submission::CommandListProxy::Create(device, a1, raw, &proxy), "proxy2");
        Check(proxy->Close(), "close never-exec");
        Check(proxy->Reset(a2, nullptr), "reset after close never-exec");
        proxy->Release();
        raw->Release();
        a1->Release();
        a2->Release();
    }

    queue->Release();
    device->Release();
    std::printf("lmxxf_list_split: ok\n");
    return 0;
}
