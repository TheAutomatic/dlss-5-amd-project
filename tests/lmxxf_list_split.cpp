#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#define LOG_WARN(...) ((void)0)
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/LogicalList.h"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/CommandListProxy.h"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/ResourceStateBook.h"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/RootBindState.h"
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

    // A completed timestamp can be resolved after the cut on the same queue.
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
        qhd.Count = 1;
        Check(device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)), "query heap");
        ID3D12Resource *result = MakeBuffer(device, 8, D3D12_HEAP_TYPE_READBACK,
                                             D3D12_RESOURCE_STATE_COPY_DEST);
        px->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        Require(!px->IsSplitIneligible(), "timestamp permits split");
        Check(px->SplitSegments(), "split after timestamp");
        px->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, result, 0);
        Check(px->ExecuteOn(queue), "execute timestamp split");
        WaitIdle(device, queue);
        void *mapped = nullptr;
        D3D12_RANGE range {0, 8};
        Check(result->Map(0, &range, &mapped), "map timestamp result");
        Require(*static_cast<const UINT64 *>(mapped) != 0, "timestamp resolved across split");
        result->Unmap(0, nullptr);
        px->Release();
        raw->Release();
        qh->Release();
        result->Release();
        a->Release();
    }

    // An active Begin/End query cannot cross the cut; closing it restores admission.
    {
        ID3D12CommandAllocator *a = nullptr;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "open_q_a");
        ID3D12GraphicsCommandList *raw = nullptr;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&raw)),
              "open_q_raw");
        DlssNr::Submission::CommandListProxy *px = nullptr;
        Check(DlssNr::Submission::CommandListProxy::Create(device, a, raw, &px), "open_q_proxy");
        ID3D12QueryHeap *qh = nullptr;
        D3D12_QUERY_HEAP_DESC qhd {};
        qhd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        qhd.Count = 1;
        Check(device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)), "occlusion query heap");
        px->BeginQuery(qh, D3D12_QUERY_TYPE_OCCLUSION, 0);
        Require(px->IsSplitIneligible(), "open query blocks split");
        Require(std::strcmp(px->SplitRejectionReason(), "open_query") == 0, "open query reason");
        Require(FAILED(px->SplitSegments()), "split refused during query");
        px->EndQuery(qh, D3D12_QUERY_TYPE_OCCLUSION, 0);
        Require(!px->IsSplitIneligible(), "completed query permits split");
        Check(px->SplitSegments(), "split after completed query");
        Check(px->ExecuteOn(queue), "execute completed query split");
        WaitIdle(device, queue);
        px->Release();
        raw->Release();
        qh->Release();
        a->Release();
    }


    // Plan D: IA capture survives into Split seed; Execute decay on book.
    {
        ID3D12CommandAllocator *a = nullptr;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "d_ia_a");
        ID3D12GraphicsCommandList *raw = nullptr;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&raw)), "d_ia_raw");
        DlssNr::Submission::CommandListProxy *px = nullptr;
        Check(DlssNr::Submission::CommandListProxy::Create(device, a, raw, &px), "d_ia_proxy");

        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *vbRes = nullptr;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&vbRes)),
              "d_ia_vb");
        D3D12_VERTEX_BUFFER_VIEW vbv {};
        vbv.BufferLocation = vbRes->GetGPUVirtualAddress();
        vbv.SizeInBytes = 64;
        vbv.StrideInBytes = 16;
        px->IASetVertexBuffers(0, 1, &vbv);
        D3D12_INDEX_BUFFER_VIEW ibv {};
        ibv.BufferLocation = vbRes->GetGPUVirtualAddress() + 64;
        ibv.SizeInBytes = 32;
        ibv.Format = DXGI_FORMAT_R16_UINT;
        px->IASetIndexBuffer(&ibv);
        Require(px->CapturedVbSlotCount() == 1, "vb slot captured before split");
        Require(px->CapturedIbBound() == TRUE, "ib captured before split");
        Check(px->SplitSegments(), "split with IA seed");
        Require(px->CapturedVbSlotCount() == 1, "vb still captured after split");
        Require(px->CapturedIbBound() == TRUE, "ib still captured after split");
        px->Close();
        px->Release();
        raw->Release();
        vbRes->Release();
        a->Release();
    }

    {
        DlssNr::Submission::ResourceStateBook book;
        // Fake resource pointer keys (map identity only; never dereferenced).
        ID3D12Resource *psr = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(0x1000));
        ID3D12Resource *rt = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(0x2000));
        D3D12_RESOURCE_BARRIER bars[2] {};
        bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[0].Transition.pResource = psr;
        bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bars[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[1].Transition.pResource = rt;
        bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bars[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        const char *why = nullptr;
        Require(book.OnBarriers(2, bars, &why), "book accepts transitions");
        book.ApplyExecuteDecay();
        D3D12_RESOURCE_STATES s = D3D12_RESOURCE_STATE_COMMON;
        Require(book.TryGet(psr, &s) && s == D3D12_RESOURCE_STATE_COMMON, "PSR decays to COMMON");
        Require(book.TryGet(rt, &s) && s == D3D12_RESOURCE_STATE_RENDER_TARGET, "RT survives Execute");
    }


    // Plan D residual: root CBV + sample positions capture.
    {
        ID3D12CommandAllocator *a = nullptr;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "d_root_a");
        ID3D12GraphicsCommandList *raw = nullptr;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&raw)), "d_root_raw");
        DlssNr::Submission::CommandListProxy *px = nullptr;
        Check(DlssNr::Submission::CommandListProxy::Create(device, a, raw, &px), "d_root_proxy");

        // Minimal empty root signature (allow root CBV at param 0).
        D3D12_ROOT_PARAMETER1 rp {};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp.Descriptor.ShaderRegister = 0;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC vs {};
        vs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        vs.Desc_1_1.NumParameters = 1;
        vs.Desc_1_1.pParameters = &rp;
        ID3DBlob *blob = nullptr;
        ID3DBlob *err = nullptr;
        Check(D3D12SerializeVersionedRootSignature(&vs, &blob, &err), "serialize rs");
        ID3D12RootSignature *rs = nullptr;
        Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)),
              "create rs");
        blob->Release();
        if (err)
            err->Release();
        px->SetGraphicsRootSignature(rs);
        px->SetGraphicsRootConstantBufferView(0, 0x1000);
        Require(px->CapturedGfxRootCount() == 1, "gfx root CBV captured");

        D3D12_SAMPLE_POSITION sp[4] {};
        sp[0] = { -4, -4 };
        sp[1] = { 4, -4 };
        sp[2] = { -4, 4 };
        sp[3] = { 4, 4 };
        px->SetSamplePositions(4, 1, sp);
        Require(px->CapturedSamplePositions() == TRUE, "sample positions captured");
        Check(px->SplitSegments(), "split with root+sample seed");
        Require(px->CapturedGfxRootCount() == 1, "gfx root still after split");
        px->Close();
        px->Release();
        raw->Release();
        rs->Release();
        a->Release();
    }

    // RootBindState unit: overflow fails closed via MarkSplitIneligible path is index>=64.
    {
        DlssNr::Submission::RootBindState rb;
        D3D12_GPU_DESCRIPTOR_HANDLE h {};
        h.ptr = 1;
        Require(rb.OnTable(0, h), "table 0 ok");
        Require(!rb.OnTable(64, h), "table 64 overflow");
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
