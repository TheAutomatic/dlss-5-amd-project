#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfColorProbe.h"

using Microsoft::WRL::ComPtr;
using namespace DlssNr::Backend::LmxxfProbe;
static void Require(bool ok, const char *what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
static void Check(HRESULT hr, const char *what)
{
    if (FAILED(hr)) { std::fprintf(stderr, "FAIL: %s hr=%08lx\n", what, static_cast<unsigned long>(hr)); std::exit(1); }
}
static void Barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *r,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    cmd->ResourceBarrier(1, &b);
}
int main(int argc, char **argv)
{
    Require(ParseMode("off") == Mode::Off && ParseMode("original") == Mode::Original &&
            ParseMode("copy-current") == Mode::CopyCurrent && ParseMode("typo") == Mode::Invalid,
            "diagnostic mode parser");
    ComPtr<ID3D12Debug> debug;
    const bool debugOn = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (debugOn) debug->EnableDebugLayer();
    std::printf("debug_layer=%s\n", debugOn ? "enabled" : "unavailable");
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter1> adapter;
    if (argc > 1 && std::strcmp(argv[1], "hardware") == 0)
    {
        for (UINT i = 0; ; ++i)
        {
            Check(factory->EnumAdapters1(i, &adapter), "hardware adapter");
            DXGI_ADAPTER_DESC1 d {};
            Check(adapter->GetDesc1(&d), "adapter desc");
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
            adapter.Reset();
        }
    }
    else
        Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    DXGI_ADAPTER_DESC1 ad {};
    Check(adapter->GetDesc1(&ad), "adapter desc");
    std::printf("adapter_vendor=%04x software=%u\n", ad.VendorId, unsigned(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE));
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    ComPtr<ID3D12InfoQueue> info;
    device.As(&info);
    D3D12_COMMAND_QUEUE_DESC qd {};
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    ComPtr<ID3D12CommandAllocator> alloc;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
    ComPtr<ID3D12GraphicsCommandList> cmd;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                   IID_PPV_ARGS(&cmd)), "list");
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "event");
    UINT64 submitted = 0;
    auto execute = [&] {
        ID3D12CommandList *batch[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, batch);
        Check(queue->Signal(fence.Get(), ++submitted), "signal");
        Check(fence->SetEventOnCompletion(submitted, event), "event on completion");
        Require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU wait");
        Require(fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= submitted, "real completion");
        Check(device->GetDeviceRemovedReason(), "device still healthy");
    };
    auto reset = [&] {
        Check(alloc->Reset(), "allocator reset after idle/discard");
        Check(cmd->Reset(alloc.Get(), nullptr), "list reset");
    };

    D3D12_RESOURCE_DESC td {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 16; td.Height = 8; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> source;
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, read, nullptr,
                                          IID_PPV_ARGS(&source)), "source");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    auto makeBuffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = type;
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> resource;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr,
                                              IID_PPV_ARGS(&resource)), "buffer");
        return resource;
    };
    auto upload = makeBuffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = makeBuffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto readback2 = makeBuffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    std::array<unsigned char, 16 * 8 * 8> expected {};
    auto fill = [&](unsigned token) {
        for (size_t i = 0; i < expected.size(); ++i)
            expected[i] = static_cast<unsigned char>((i * 17 + token * 29) & 255);
        unsigned char *p = nullptr;
        D3D12_RANGE noRead {0, 0};
        Check(upload->Map(0, &noRead, reinterpret_cast<void **>(&p)), "upload map");
        for (UINT y = 0; y < rows; ++y)
            std::memcpy(p + footprint.Offset + y * footprint.Footprint.RowPitch, expected.data() + y * rowBytes,
                        static_cast<size_t>(rowBytes));
        upload->Unmap(0, nullptr);
    };
    auto verify = [&](ID3D12Resource *buffer) {
        unsigned char *p = nullptr;
        D3D12_RANGE range {0, static_cast<SIZE_T>(total)};
        Check(buffer->Map(0, &range, reinterpret_cast<void **>(&p)), "readback map");
        for (UINT y = 0; y < rows; ++y)
            Require(std::memcmp(p + footprint.Offset + y * footprint.Footprint.RowPitch,
                                expected.data() + y * rowBytes, static_cast<size_t>(rowBytes)) == 0,
                    "same-frame bytes including allocation padding");
        D3D12_RANGE noWrite {0, 0}; buffer->Unmap(0, &noWrite);
    };
    auto recordProducer = [&] {
        Barrier(cmd.Get(), source.Get(), read, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst {}; dst.pResource = source.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src {}; src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprint;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(cmd.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
    };
    auto recordConsumer = [&](ID3D12Resource *texture, ID3D12Resource *buffer) {
        Barrier(cmd.Get(), texture, read, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src {}; src.pResource = texture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst {}; dst.pResource = buffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(cmd.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, read);
    };
    ColorCopy probe;
    // Record then discard: first-use bookkeeping must not claim GPU execution.
    auto *first = probe.Record(device.Get(), cmd.Get(), source.Get(), read, 13, 7);
    Require(first != nullptr && first != source.Get() && probe.EntryCount() == 1, "private output");
    Require(fence->GetCompletedValue() == 0, "record is not submission");
    Check(cmd->Close(), "close discarded list"); reset();
    const D3D12_RESOURCE_STATES states[] = {read, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE};
    for (unsigned frame = 1; frame <= 3; ++frame)
    {
        fill(frame);
        recordProducer();
        const auto state = states[frame - 1];
        Barrier(cmd.Get(), source.Get(), read, state);
        auto *output = probe.Record(device.Get(), cmd.Get(), source.Get(), state, 13, 7);
        Require(output == first && probe.EntryCount() == 1, "reuse per list without growth");
        Barrier(cmd.Get(), source.Get(), state, read);
        recordConsumer(output, readback.Get());
        // A second consumer after a second Evaluate site must remain in order.
        auto *second = probe.Record(device.Get(), cmd.Get(), source.Get(), read, 16, 8);
        Require(second == output, "same-list second Evaluate");
        recordConsumer(second, readback2.Get());
        Check(cmd->Close(), "close"); execute(); verify(readback.Get()); verify(readback2.Get());
        // Legitimate closed-list replay: changed producer bytes, no new Record call.
        fill(frame + 20); execute(); verify(readback.Get()); verify(readback2.Get());
        reset();
    }
    Require(!probe.Record(device.Get(), cmd.Get(), source.Get(), read, 17, 8), "reject oversized valid extent");
    Require(!probe.Record(device.Get(), cmd.Get(), source.Get(), read, 0, 8), "reject empty valid extent");
    Require(!probe.Record(device.Get(), cmd.Get(), source.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, 16, 8), "reject unknown state");
    Require(!probe.Record(device.Get(), nullptr, source.Get(), read, 16, 8), "reject null list");
    ComPtr<ID3D12Resource> changed;
    td.Width = 8; td.Height = 4;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, read, nullptr,
                                          IID_PPV_ARGS(&changed)), "resize source");
    auto *resized = probe.Record(device.Get(), cmd.Get(), changed.Get(), read, 8, 4);
    Require(resized && resized != first && probe.EntryCount() == 2, "resize retains previous recorded output");
    Check(cmd->Close(), "close unsubmitted resize"); reset();
    Require(probe.Record(device.Get(), cmd.Get(), source.Get(), read, 13, 7) == first, "return to old extent");
    Check(cmd->Close(), "close discarded probe"); reset(); Check(cmd->Close(), "discard old recording");

    // Distinct lists cannot share outputs. Exhaustion rejects instead of freeing pins.
    std::vector<ComPtr<ID3D12CommandAllocator>> allocs;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
    for (unsigned i = 0; i < 31; ++i)
    {
        ComPtr<ID3D12CommandAllocator> a;
        ComPtr<ID3D12GraphicsCommandList> l;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "pool allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l)), "pool list");
        auto *out = probe.Record(device.Get(), l.Get(), source.Get(), read, 13, 7);
        if (i < 30) Require(out && out != first, "different-list private output");
        else Require(!out && std::strcmp(probe.Reason(), "pin_budget_exhausted") == 0, "bounded pin budget");
        Check(l->Close(), "pool close");
        // Discard, never submit, then discard the empty generation too.
        Check(a->Reset(), "unsubmitted allocator reset");
        Check(l->Reset(a.Get(), nullptr), "unsubmitted list reset");
        Check(l->Close(), "empty list close");
        allocs.push_back(a); lists.push_back(l);
    }
    Require(probe.EntryCount() == 32 && probe.AllocatedBytes() <= 128ull * 1024 * 1024, "bounded retained storage");
    probe.ReleaseAfterGpuIdleAndDiscard();
    Require(probe.EntryCount() == 0, "explicit proven-idle teardown");
    if (info)
    {
        const UINT64 n = info->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i)
        {
            SIZE_T size = 0; info->GetMessage(i, nullptr, &size);
            std::vector<unsigned char> storage(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            Check(info->GetMessage(i, message, &size), "debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                std::fprintf(stderr, "D3D12 ERROR: %s\n", message->pDescription);
                Require(false, "no debug-layer errors");
            }
        }
    }
    CloseHandle(event);
    std::printf("PASS: current-frame pixels, padding, producer/consumer order, replay, discard, resize, per-list isolation, admission and pin cap; submissions=%llu\n",
                static_cast<unsigned long long>(submitted));
    return 0;
}
