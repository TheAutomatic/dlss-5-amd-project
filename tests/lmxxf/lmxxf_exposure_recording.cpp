#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfExposureMeter.h"

template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
static void Require(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void Check(HRESULT hr, const char* message) { Require(SUCCEEDED(hr), message); }
static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}
int main()
{
    Ptr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Ptr<IDXGIFactory4> factory; Ptr<IDXGIAdapter> warp; Ptr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
    Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    Ptr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd {};
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    Ptr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    UINT64 fenceValue = 0;
    auto submit = [&](ID3D12GraphicsCommandList* list) {
        ID3D12CommandList* commands[] = {list}; queue->ExecuteCommandLists(1, commands);
        Check(queue->Signal(fence.Get(), ++fenceValue), "signal");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(event != nullptr, "event");
        Check(fence->SetEventOnCompletion(fenceValue, event), "register event");
        Require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "wait"); CloseHandle(event);
        Require(fence->GetCompletedValue() != UINT64_MAX && SUCCEEDED(device->GetDeviceRemovedReason()), "completion");
    };
    auto buffer = [&](D3D12_HEAP_TYPE type, UINT64 bytes) {
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = type;
        D3D12_RESOURCE_DESC rd {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Ptr<ID3D12Resource> result;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
              type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
              nullptr, IID_PPV_ARGS(&result)), "buffer");
        return result;
    };
    constexpr unsigned count = 33;
    auto readback = buffer(D3D12_HEAP_TYPE_READBACK, count * 512);
    std::vector<Ptr<ID3D12CommandAllocator>> allocators(count);
    std::vector<Ptr<ID3D12GraphicsCommandList>> lists(count);
    std::vector<std::shared_ptr<LmxxfRuntime::ExposureRecording>> leases(count);
    LmxxfRuntime::ExposureMeter meter;
    Require(meter.Ensure(device.Get()), "meter setup");
    for (unsigned i = 0; i < count; ++i)
    {
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])), "allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
                                       IID_PPV_ARGS(&lists[i])), "list");
        D3D12_RESOURCE_DESC rd {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = rd.Height = 1; rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; rd.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Ptr<ID3D12Resource> colour;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&colour)), "colour");
        auto upload = buffer(D3D12_HEAP_TYPE_UPLOAD, 256);
        void* mapped = nullptr; Check(upload->Map(0, nullptr, &mapped), "upload map");
        const float luma = float(i + 1) / 8.0f;
        const float rgba[] = {luma, luma, luma, 1}; std::memcpy(mapped, rgba, sizeof rgba); upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {rd.Format, 1, 1, 1, 256};
        dst.pResource = colour.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        lists[i]->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(lists[i].Get(), colour.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Check(lists[i]->Close(), "upload close"); submit(lists[i].Get());
        Check(allocators[i]->Reset(), "upload allocator reset");
        Check(lists[i]->Reset(allocators[i].Get(), nullptr), "upload list reset");
        meter.Record(lists[i].Get(), device.Get(), colour.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 1, 1, &leases[i]);
        Barrier(lists[i].Get(), meter.value, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        src = {}; dst = {};
        src.pResource = meter.value; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = i * 512;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 256};
        lists[i]->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(lists[i].Get(), meter.value, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Check(lists[i]->Close(), "recording close");
        // The local colour reference dies here; the lease must retain it.
    }
    // Release the meter itself before delayed execution. Every dispatch dependency
    // must survive independently in the lease, including the PSO and root signature.
    meter.Release();
    float expected = 0;
    auto run = [&](unsigned i) {
        submit(lists[i].Get());
        const float encoded = std::pow(0.45f, 2.2f);
        const float target = (encoded / (1 - encoded)) / (float(i + 1) / 8);
        expected = expected > 0 ? std::exp(std::log(expected) * .75f + std::log(target) * .25f) : target;
        void* mapped = nullptr; D3D12_RANGE range {i * 512, i * 512 + 4};
        Check(readback->Map(0, &range, &mapped), "readback map");
        float actual; std::memcpy(&actual, static_cast<char*>(mapped) + i * 512, 4);
        D3D12_RANGE empty {}; readback->Unmap(0, &empty);
        if (!std::isfinite(actual) || std::fabs(actual - expected) > 0.0001f)
            std::fprintf(stderr, "recording %u: actual=%f expected=%f\n", i, actual, expected);
        Require(std::isfinite(actual) && std::fabs(actual - expected) <= .0001f, "immutable exposure binding output");
    };
    run(0); run(0);
    for (unsigned i = count; i-- > 0;) run(i);
    // Closed recordings are invalidated before their leases are dropped.
    lists.clear(); allocators.clear(); leases.clear();
    Ptr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device.As(&info)))
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T bytes = 0; info->GetMessage(i, nullptr, &bytes); std::vector<char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(info->GetMessage(i, message, &bytes), "debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) std::fprintf(stderr, "%s\n", message->pDescription);
            Require(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "D3D12 debug layer errors");
        }
    std::puts("exposure recording leases: PASS (33 delayed bindings, reverse execution, replay, owner teardown)");
}
