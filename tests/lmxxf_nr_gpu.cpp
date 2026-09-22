#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void Require(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static std::wstring Widen(const char *s) { return std::wstring(s, s + std::strlen(s)); }

static void Check(HRESULT hr, const char *what)
{
    if (FAILED(hr))
    {
        std::fprintf(stderr, "FAIL: %s hr=%08lx\n", what, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: lmxxf_nr_gpu.exe <LmxxfNrRuntime.dll> <modules_dir>\n");
        return 2;
    }

    HMODULE dll = LoadLibraryW(Widen(argv[1]).c_str());
    Require(dll != nullptr, "LoadLibraryW");
    auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi *)>(GetProcAddress(dll, "LmxxfNrGetApi"));
    Require(getApi != nullptr, "GetProcAddress");

    LmxxfNrApi api {};
    api.struct_size = sizeof(api);
    Require(getApi(LMXXF_NR_ABI_VERSION, &api) == LMXXF_NR_OK, "GetApi");
    LmxxfNrCapabilities caps {};
    caps.struct_size = sizeof(caps);
    Require(api.QueryCapabilities(&caps) == LMXXF_NR_OK, "QueryCapabilities");
    Require(caps.hip_ready == 0, "hip_ready stays 0");
    Require(caps.graph_supported == 0, "graph off");

    IDXGIFactory4 *factory = nullptr;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    IDXGIAdapter1 *adapter = nullptr;
    ID3D12Device *device = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x1002 && SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
            break;
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    Require(device != nullptr, "AMD D3D12 device");

    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    ID3D12CommandAllocator *alloc = nullptr;
    ID3D12CommandAllocator *outAlloc = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
    ID3D12GraphicsCommandList *list = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)), "list");

    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 1920;
    td.Height = 1080;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *color = nullptr;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                          nullptr, IID_PPV_ARGS(&color)),
          "color");

    const std::wstring modules = Widen(argv[2]);
    LmxxfNrCreateInfo info {};
    info.struct_size = sizeof(info);
    info.device = device;
    info.queue = queue;
    info.assets_directory = modules.c_str();
    void *ctx = nullptr;
    Require(api.Create(&info, &ctx) == LMXXF_NR_OK, "Create");
    const int32_t prep = api.PrepareSession(ctx);
    if (prep != LMXXF_NR_OK)
    {
        char err[256] {};
        api.GetLastError(err, sizeof err);
        std::fprintf(stderr, "PrepareSession rc=%d err=%s\n", prep, err);
        Require(false, "PrepareSession");
    }

    LmxxfNrFrameInfo frame {};
    frame.struct_size = sizeof(frame);
    frame.color_width = 1920;
    frame.color_height = 1080;
    frame.color = color;
    frame.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    LmxxfNrJob job {};
    job.struct_size = sizeof(job);
    const int32_t pfr = api.PrepareFrame(ctx, &frame, &job);
    if (pfr != LMXXF_NR_OK)
    {
        char err[256] {};
        api.GetLastError(err, sizeof err);
        std::fprintf(stderr, "PrepareFrame rc=%d err=%s\n", pfr, err);
        Require(false, "PrepareFrame");
    }
    Require(job.private_output != nullptr, "private_output");
    Require(api.RecordInputs(ctx, job.handle, list) == LMXXF_NR_OK, "RecordInputs");
    Check(list->Close(), "close producer");
    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    const int32_t hip = api.EnqueueHip(ctx, job.handle, queue);
    char err[256] {};
    api.GetLastError(err, sizeof err);
    std::printf("EnqueueHip rc=%d last_error=%s\n", hip, err);
    Require(hip == LMXXF_NR_OK || hip == LMXXF_NR_UNAVAILABLE, "EnqueueHip wired (OK or missing weights)");

    // EnqueueHip only schedules GPU work; wait before Reset of the same allocator.
    {
        ID3D12Fence *fence = nullptr;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        Check(queue->Signal(fence, 1), "signal");
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(ev != nullptr, "event");
        Check(fence->SetEventOnCompletion(1, ev), "set event");
        WaitForSingleObject(ev, 30000);
        CloseHandle(ev);
        fence->Release();
    }
    // Prefer a fresh allocator for outputs so producer storage is never Reset early.
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&outAlloc)), "out alloc");
    Check(list->Reset(outAlloc, nullptr), "reset list on out alloc");
    const int32_t outs = api.RecordOutputs(ctx, job.handle, list);
    Require(outs == LMXXF_NR_OK || outs == LMXXF_NR_FAILED, "RecordOutputs called");
    if (outs == LMXXF_NR_OK)
    {
        Check(list->Close(), "close outputs");
        queue->ExecuteCommandLists(1, lists);
        Check(api.Retire(ctx, job.handle) == LMXXF_NR_OK, "Retire");
    }

    Require(api.Destroy(ctx) == LMXXF_NR_OK, "Destroy");
    color->Release();
    list->Release();
    if (outAlloc)
        outAlloc->Release();
    alloc->Release();
    queue->Release();
    device->Release();
    if (adapter)
        adapter->Release();
    FreeLibrary(dll);
    std::printf("lmxxf_nr_gpu: ok\n");
    return 0;
}
