#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/SubmissionHooks.h"
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfColorProbe.h"

using Microsoft::WRL::ComPtr;
using namespace DlssNr::Submission;
namespace H = DlssNr::Submission::Hooks;
static void Require(bool value, const char *what)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
static void Check(HRESULT hr, const char *what)
{
    if (FAILED(hr)) { std::fprintf(stderr, "FAIL: %s hr=%08lx\n", what, static_cast<unsigned long>(hr)); std::exit(1); }
}
static void Barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *r,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    cmd->ResourceBarrier(1, &b);
}
static UINT tracedCalls = 0, tracedLists = 0;
static void WINAPI TraceRaw(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *lists)
{
    ++tracedCalls;
    tracedLists += n;
    for (UINT i = 0; i < n; ++i)
    {
        ComPtr<ILogicalCommandList> logical;
        Require(FAILED(lists[i]->QueryInterface(IID_PPV_ARGS(&logical))), "native ECL never receives a proxy");
    }
    g_rawExecuteCommandLists(q, n, lists);
}
int main()
{
    using namespace DlssNr::Backend::LmxxfProbe;
    Require(NeedsOpenListProxy(ParseMode("proxy-original")) && NeedsOpenListProxy(ParseMode("split-original")) &&
            !NeedsOpenListProxy(ParseMode("off")) && !NeedsOpenListProxy(ParseMode("staging-current")), "opt-in modes");
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
    Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC qd {};
    ComPtr<ID3D12CommandQueue> queue;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "event");
    UINT64 serial = 0;
    auto wait = [&] {
        Check(queue->Signal(fence.Get(), ++serial), "signal");
        Check(fence->SetEventOnCompletion(serial, event), "completion event");
        Require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU completion timeout");
        const UINT64 done = fence->GetCompletedValue();
        Require(done != UINT64_MAX && done >= serial, "GPU completion value");
        Check(device->GetDeviceRemovedReason(), "device healthy");
    };
    Check(H::Arm(device.Get(), queue.Get()), "hook Create/ECL");
    H::SetProxyWrap(true);
    auto allocator = [&] {
        ComPtr<ID3D12CommandAllocator> a;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "allocator");
        return a;
    };
    auto a = allocator();
    ComPtr<ID3D12GraphicsCommandList> unwrapped;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&unwrapped)), "default Create");
    ComPtr<ILogicalCommandList> logical;
    Require(FAILED(unwrapped.As(&logical)), "open lists remain native by default");
    Check(unwrapped->Close(), "default close");
    H::SetWrapOpenLists(true);

    // Ordinary Create hook, unsplit mixed batch: exactly one native Execute containing both lists.
    auto b = allocator();
    ComPtr<ID3D12GraphicsCommandList> cmd;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, b.Get(), nullptr, IID_PPV_ARGS(&cmd)), "hooked Create");
    Check(cmd.As(&logical), "open-list proxy");
    ComPtr<IUnknown> face1, face2;
    Check(cmd.As(&face1), "list identity");
    Check(logical.As(&face2), "logical identity");
    Require(face1.Get() == face2.Get(), "COM identity");
    Check(cmd->Close(), "unsplit close");
    ID3D12CommandList *mixed[] = {unwrapped.Get(), cmd.Get()};
    H::ExecuteExpanded(queue.Get(), 2, mixed, nullptr, nullptr, TraceRaw);
    Require(tracedCalls == 1 && tracedLists == 2, "unsplit batch preserved");
    wait();

    // Texture producer writes a different pattern each generation; consumer is after the cut.
    for (DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT})
    {
        D3D12_RESOURCE_DESC td {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 16; td.Height = 8; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = format; td.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&texture)), "texture");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT rows = 0; UINT64 rowBytes = 0, total = 0;
        device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
        auto buffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, UINT64 bytes) {
            D3D12_RESOURCE_DESC bd {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes; bd.Height = 1;
            bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES bh {}; bh.Type = type;
            ComPtr<ID3D12Resource> r;
            Check(device->CreateCommittedResource(&bh, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&r)), "buffer");
            return r;
        };
        auto upload = buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, total);
        auto readback = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, total);
        std::vector<unsigned char> expected(static_cast<size_t>(rowBytes * rows));
        for (unsigned token = 0; token < 4; ++token)
        {
            for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<unsigned char>(i * 19 + token * 67);
            unsigned char *p = nullptr;
            D3D12_RANGE noRead {0, 0};
            Check(upload->Map(0, &noRead, reinterpret_cast<void **>(&p)), "upload map");
            for (UINT y = 0; y < rows; ++y)
                std::memcpy(p + footprint.Offset + y * footprint.Footprint.RowPitch, expected.data() + y * rowBytes, static_cast<size_t>(rowBytes));
            upload->Unmap(0, nullptr);
            Check(b->Reset(), "allocator reset after fence");
            Check(cmd->Reset(b.Get(), nullptr), "proxy reset");
            D3D12_VIEWPORT vp {0, 0, 16, 8, 0, 1};
            cmd->RSSetViewports(1, &vp);
            D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
            dst.pResource = texture.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprint;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Barrier(cmd.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Check(logical->SplitSegments(), "same-frame cut");
            Require(logical->IsSplitIneligible(), "second cut refused");
            dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
            src.pResource = texture.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Barrier(cmd.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            Check(cmd->Close(), "continuation close");
            Require(FAILED(cmd->Reset(nullptr, nullptr)), "failed Reset preserves closed generation");
            Require(logical->CapturedViewportCount() == 1, "failed Reset preserves captured bindings");
            ID3D12CommandList *batch[] = {cmd.Get()};
            queue->ExecuteCommandLists(1, batch); // Real Detours route, internal calls must bypass it.
            wait();
            D3D12_RANGE all {0, static_cast<SIZE_T>(total)};
            Check(readback->Map(0, &all, reinterpret_cast<void **>(&p)), "readback map");
            for (UINT y = 0; y < rows; ++y)
                Require(std::memcmp(p + footprint.Offset + y * footprint.Footprint.RowPitch,
                                    expected.data() + y * rowBytes, static_cast<size_t>(rowBytes)) == 0, "CURRENT generation pixel bytes");
            D3D12_RANGE noWrite {0, 0}; readback->Unmap(0, &noWrite);
            // Replay must retain both segments. Caller has waited for the previous execution.
            queue->ExecuteCommandLists(1, batch);
            wait();
        }
    }
    Require(g_splitSubmissions.load() == 16 && g_continuationSubmissions.load() == 16 &&
            g_submissionFailures.load() == 0, "actual producer/continuation submit counts");

    // Initial PSO comes from CreateCommandList, with no explicit SetPipelineState.
    // Root UAV/constants are set before the cut and consumed by a real Dispatch after it.
    {
        const char shader[] = "RWByteAddressBuffer output : register(u0); cbuffer C : register(b0) { uint value; }; [numthreads(1,1,1)] void main() { output.Store(0, value); }";
        ComPtr<ID3DBlob> code, error, serialized;
        Check(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &error), "compile seed shader");
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = 1;
        D3D12_ROOT_SIGNATURE_DESC rd {}; rd.NumParameters = 2; rd.pParameters = params;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error), "serialize root");
        ComPtr<ID3D12RootSignature> root;
        Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root.Get(); pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        ComPtr<ID3D12PipelineState> pso;
        Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "PSO");
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 256; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> gpu, cpu;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&gpu)), "seed UAV");
        hp.Type = D3D12_HEAP_TYPE_READBACK; bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&cpu)), "seed readback");
        auto seedAlloc = allocator();
        ComPtr<ID3D12GraphicsCommandList> seedCmd;
        ComPtr<ILogicalCommandList> seedLogical;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, seedAlloc.Get(), pso.Get(), IID_PPV_ARGS(&seedCmd)), "Create with initial PSO");
        Check(seedCmd.As(&seedLogical), "seed proxy");
        seedCmd->SetComputeRootSignature(root.Get());
        seedCmd->SetComputeRootUnorderedAccessView(0, gpu->GetGPUVirtualAddress());
        seedCmd->SetComputeRoot32BitConstant(1, 0x1234abcd, 0);
        Check(seedLogical->SplitSegments(), "seed cut");
        seedCmd->Dispatch(1, 1, 1);
        Barrier(seedCmd.Get(), gpu.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        seedCmd->CopyBufferRegion(cpu.Get(), 0, gpu.Get(), 0, sizeof(UINT));
        Check(seedCmd->Close(), "seed close");
        ID3D12CommandList *batch[] = {seedCmd.Get()};
        queue->ExecuteCommandLists(1, batch);
        wait();
        UINT *data = nullptr;
        D3D12_RANGE range {0, sizeof(UINT)};
        Check(cpu->Map(0, &range, reinterpret_cast<void **>(&data)), "seed map");
        Require(*data == 0x1234abcd, "initial PSO + UAV + root constants restored to continuation");
        D3D12_RANGE noWrite {0, 0}; cpu->Unmap(0, &noWrite);
    }

    // Closed CreateCommandList1 and explicit helper remain single-layer proxies with open wrapping enabled.
    ComPtr<ID3D12Device4> dev4;
    Check(device.As(&dev4), "device4");
    ComPtr<ID3D12GraphicsCommandList> closed;
    Check(dev4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&closed)), "CL1");
    Check(closed->Reset(a.Get(), nullptr), "CL1 first reset");
    Check(closed->Close(), "CL1 close");
    ComPtr<ID3D12GraphicsCommandList> explicitProxy;
    auto c = allocator();
    Check(H::CreateProxiedCommandList(device.Get(), 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.Get(), nullptr,
                                     IID_PPV_ARGS(&explicitProxy)), "explicit proxy helper");
    Check(explicitProxy->Close(), "explicit close");
    ID3D12CommandList *tail[] = {closed.Get(), explicitProxy.Get()};
    H::ExecuteExpanded(queue.Get(), 2, tail, nullptr, nullptr, TraceRaw);
    Require(tracedCalls == 2 && tracedLists == 4, "CL1/helper native unwrapping");
    wait();
    H::Disarm();
    Require(g_rawExecuteCommandLists == nullptr, "detach clears trampoline");
    CloseHandle(event);
    std::printf("lmxxf_same_frame_boundary: PASS (WARP; SDR8/FP16 current pixels, 16 split submits/replays, initial PSO/root/UAV dispatch, mixed batch, failed Reset, CL1/helper)\n");
}
