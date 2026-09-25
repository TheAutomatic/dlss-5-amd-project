#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../third_party/lmxxf/Development/HIP/hip_reference_network.h"
#include "../third_party/lmxxf/src/native_device_identity.h"
// White-box coverage of the D3D12 backup clear without adding a production test hook.
#define private public
#include "../third_party/lmxxf/Development/HIP/hip_d3d12_bridge.h"
#undef private
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static void Require(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static void Check(HRESULT hr, const char *what)
{
    if (FAILED(hr))
    {
        std::fprintf(stderr, "FAIL: %s hr=%08lx\n", what, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

static void WaitQueue(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    ID3D12Fence *fence = nullptr;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    Check(queue->Signal(fence, 1), "signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "completion event");
    Check(fence->SetEventOnCompletion(1, event), "event on completion");
    Require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0 && fence->GetCompletedValue() >= 1,
            "queue completion");
    CloseHandle(event);
    fence->Release();
}

static ID3D12Resource *Buffer(ID3D12Device *device, D3D12_HEAP_TYPE type, UINT64 bytes,
                              D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *resource = nullptr;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&resource)), "buffer");
    return resource;
}

static void Transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                       D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}

int main(int argc, char **argv)
{
    if ((argc != 3 && argc != 4) ||
        (argc == 4 && std::strcmp(argv[3], "--probe-drain") != 0))
    {
        std::fprintf(stderr, "usage: lmxxf_bridge_zero_gpu.exe <weights_dir> <modules_dir> [--probe-drain]\n");
        return 2;
    }
    const bool probeDrain = argc == 4;
    bool drainReportedEarly = false;
    IDXGIFactory4 *factory = nullptr;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ID3D12Device *device = nullptr;
    for (UINT i = 0;; ++i)
    {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x1002)
            D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        adapter->Release();
        if (device)
            break;
    }
    factory->Release();
    Require(device != nullptr, "AMD D3D12 device");
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *sessionQueue = nullptr;
    ID3D12CommandQueue *producerQueue = nullptr;
    Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&sessionQueue)), "session queue");
    Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&producerQueue)), "producer queue");

    constexpr UINT width = 512, height = 512;
    constexpr UINT64 pixels = UINT64(width) * height;
    constexpr UINT64 outputBytes = pixels * 12;
    hip_reference::Options options;
    options.width = width;
    options.height = height;
    options.assets = argv[1];
    options.modules = argv[2];
    options.wmma = options.wave = options.pooled = true;
    options.fast_prefix = true;
    try
    {
        hip_reference::D3D12Bridge bridge;
        bridge.Create(sessionQueue, options, {});
        ID3D12Resource *input = Buffer(device, D3D12_HEAP_TYPE_DEFAULT, pixels * 16,
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12Resource *poison = Buffer(device, D3D12_HEAP_TYPE_UPLOAD, outputBytes,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource *readback = Buffer(device, D3D12_HEAP_TYPE_READBACK, outputBytes,
                                          D3D12_RESOURCE_STATE_COPY_DEST);
        void *mapped = nullptr;
        D3D12_RANGE noRead {0, 0};
        Check(poison->Map(0, &noRead, &mapped), "map poison");
        std::memset(mapped, 0x7f, static_cast<size_t>(outputBytes));
        poison->Unmap(0, nullptr);

        ID3D12CommandAllocator *allocator = nullptr;
        ID3D12GraphicsCommandList *list = nullptr;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
              "allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                        IID_PPV_ARGS(&list)), "command list");
        Transition(list, bridge.Output(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(bridge.Output(), 0, poison, 0, outputBytes);
        Transition(list, bridge.Output(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        bridge.RecordInputCopy(list, input);
        Check(list->Close(), "close producer");
        ID3D12CommandList *lists[] = {list};
        producerQueue->ExecuteCommandLists(1, lists);
        WaitQueue(device, producerQueue);

        const auto readOutput = [&](unsigned char expected) {
            Check(allocator->Reset(), "reset allocator");
            Check(list->Reset(allocator, nullptr), "reset command list");
            Transition(list, bridge.Output(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(readback, 0, bridge.Output(), 0, outputBytes);
            Transition(list, bridge.Output(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_COMMON);
            Check(list->Close(), "close readback");
            producerQueue->ExecuteCommandLists(1, lists);
            WaitQueue(device, producerQueue);
            void *data = nullptr;
            D3D12_RANGE range {0, static_cast<SIZE_T>(outputBytes)};
            Check(readback->Map(0, &range, &data), "map readback");
            const auto *bytes = static_cast<const unsigned char *>(data);
            bool match = true;
            for (UINT64 i = 0; i < outputBytes; ++i)
                if (bytes[i] != expected)
                {
                    std::fprintf(stderr, "byte %llu: got %u, expected %u\n",
                                 static_cast<unsigned long long>(i), bytes[i], expected);
                    match = false;
                    break;
                }
            readback->Unmap(0, nullptr);
            Require(match, "full output readback");
        };
        readOutput(0x7f);
        Require(bridge.ClearOutputD3D12(producerQueue), "D3D12 backup clear");
        readOutput(0);

        Check(allocator->Reset(), "reset poison allocator");
        Check(list->Reset(allocator, nullptr), "reset poison list");
        Transition(list, bridge.Output(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(bridge.Output(), 0, poison, 0, outputBytes);
        Transition(list, bridge.Output(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Check(list->Close(), "close second poison");
        producerQueue->ExecuteCommandLists(1, lists);
        WaitQueue(device, producerQueue);
        readOutput(0x7f);
        Require(bridge.ClearOutput(producerQueue), "cross-queue ClearOutput");
        readOutput(0);

        Check(allocator->Reset(), "reset readable allocator");
        Check(list->Reset(allocator, nullptr), "reset readable list");
        bridge.RecordOutputReadable(list);
        Check(list->Close(), "close readable");
        ID3D12Fence *block = nullptr;
        ID3D12Fence *tail = nullptr;
        if (probeDrain)
        {
            Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&block)), "block fence");
            Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tail)), "tail fence");
            Check(sessionQueue->Wait(block, 1), "block consumer queue");
        }
        sessionQueue->ExecuteCommandLists(1, lists);
        if (probeDrain)
            Check(sessionQueue->Signal(tail, 1), "tail signal");
        bridge.NotifyOutputSubmitted(sessionQueue);
        if (probeDrain)
        {
            HRESULT unblockHr = E_FAIL;
            std::thread unblocker([&] {
                Sleep(500);
                unblockHr = block->Signal(1);
            });
            const bool drained = bridge.WaitForSubmittedWork();
            drainReportedEarly = drained && tail->GetCompletedValue() < 1;
            unblocker.join();
            Check(unblockHr, "unblock consumer queue");
        }
        WaitQueue(device, sessionQueue);
        Require(bridge.WaitForSubmittedWork(), "bridge drain");
        if (tail)
            tail->Release();
        if (block)
            block->Release();
        list->Release();
        allocator->Release();
        readback->Release();
        poison->Release();
        input->Release();
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "FAIL: bridge exception: %s\n", e.what());
        return 1;
    }
    producerQueue->Release();
    sessionQueue->Release();
    device->Release();
    if (drainReportedEarly)
    {
        std::fputs("FAIL: WaitForSubmittedWork reported completion before fallback consumer finished\n", stderr);
        return 1;
    }
    std::puts("lmxxf_bridge_zero_gpu: PASS (D3D12 and HIP clear, full zero readback)");
    return 0;
}
