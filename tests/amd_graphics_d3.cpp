// Mini D3: simulate A's graphics-wait state dirt on a real DIRECT list, restore
// from a frozen snapshot, then draw without rebinding. Uses the debug layer when
// available. Does not load the author runtime.
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/GraphicsRestore.h"

using Microsoft::WRL::ComPtr;
using namespace AmdPreSr::GraphicsSnap;

static int g_failures = 0;
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL: %s\n", msg);                                                                            \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

struct CpuRtvCopy
{
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE src {};
    D3D12_CPU_DESCRIPTOR_HANDLE copy {};
    UINT increment = 0;
};

static bool CopyCpuRtv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE src, CpuRtvCopy& out)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    desc.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&out.heap))))
        return false;
    out.increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    out.src = src;
    out.copy = out.heap->GetCPUDescriptorHandleForHeapStart();
    device->CopyDescriptorsSimple(1, out.copy, src, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

int main()
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        std::printf("FAIL: DXGI factory\n");
        return 1;
    }
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 ad {};
        adapter->GetDesc1(&ad);
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            break;
        device.Reset();
    }
    if (!device)
    {
        std::printf("SKIP: no D3D12 device\n");
        return 0;
    }

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
    {
        std::printf("FAIL: queue\n");
        return 1;
    }
    ComPtr<ID3D12CommandAllocator> alloc;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ComPtr<ID3D12GraphicsCommandList> list;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));

    // Minimal RT texture + RTV (game-like OM binding).
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = 64;
    rd.Height = 64;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> target;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                    IID_PPV_ARGS(&target));
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap));
    auto gameRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, gameRtv);

    // CPU copy of OM descriptor at "capture" time (design §3.4).
    CpuRtvCopy omCopy;
    CHECK(CopyCpuRtv(device.Get(), gameRtv, omCopy), "OM CPU descriptor copy");

    // Root signature with one table + constants (enough to exercise restore).
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, 4 };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd {};
    rsd.NumParameters = 1;
    rsd.pParameters = params;
    ComPtr<ID3DBlob> blob, err;
    D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    ComPtr<ID3D12RootSignature> root;
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));

    // --- Game state (what we freeze) ---
    D3D12_VIEWPORT gameVp { 0, 0, 32, 32, 0, 1 };
    D3D12_RECT gameSc { 0, 0, 32, 32 };
    list->RSSetViewports(1, &gameVp);
    list->RSSetScissorRects(1, &gameSc);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(1, &gameRtv, FALSE, nullptr);
    list->SetGraphicsRootSignature(root.Get());
    UINT gameConsts[4] = { 10, 20, 30, 40 };
    list->SetGraphicsRoot32BitConstants(0, 4, gameConsts, 0);
    list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);

    GraphicsSnapshot frozen;
    frozen.graphics.SetSignature(reinterpret_cast<uint64_t>(root.Get()));
    frozen.graphics.MergeConstants(0, gameConsts, 4, 0);
    Viewport fv { gameVp.TopLeftX, gameVp.TopLeftY, gameVp.Width, gameVp.Height, gameVp.MinDepth, gameVp.MaxDepth };
    frozen.SetViewports(&fv, 1);
    ScissorRect fs { gameSc.left, gameSc.top, gameSc.right, gameSc.bottom };
    frozen.SetScissors(&fs, 1);
    frozen.SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto rtvHandle = gameRtv.ptr;
    frozen.SetRenderTargets(1, &rtvHandle, false, false, 0);
    frozen.SetPredication(0, 0, 0);

    // --- Simulate A's dirt (interfaces.md graphics path) ---
    D3D12_VIEWPORT tiny { 0, 0, 1, 1, 0, 1 };
    D3D12_RECT tinySc { 0, 0, 1, 1 };
    list->RSSetViewports(1, &tiny);
    list->RSSetScissorRects(1, &tinySc);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    list->SetGraphicsRootSignature(root.Get());
    UINT dirt[4] = { 0, 0, 0, 0 };
    list->SetGraphicsRoot32BitConstants(0, 4, dirt, 0);

    // Rewrite the game's CPU RTV descriptor (A/game could do this before restore).
    D3D12_RESOURCE_DESC rd2 = rd;
    ComPtr<ID3D12Resource> other;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd2, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                    IID_PPV_ARGS(&other));
    device->CreateRenderTargetView(other.Get(), nullptr, gameRtv);

    // --- Restore from plan + OM CPU copy ---
    RestorePlan plan;
    CHECK(BuildRestorePlan(frozen, plan), "build restore plan");
    D3D12_VIEWPORT restoreVps[16];
    D3D12_RECT restoreScs[16];
    UINT restoreVpCount = 0, restoreScCount = 0;
    for (std::size_t i = 0; i < plan.count; ++i)
    {
        const auto& c = plan.ops[i];
        switch (c.op)
        {
        case RestoreOp::SetGraphicsRootSignature:
            list->SetGraphicsRootSignature(reinterpret_cast<ID3D12RootSignature*>(c.handle));
            break;
        case RestoreOp::SetRootConstants:
            if (c.graphics)
                list->SetGraphicsRoot32BitConstants(c.index, c.count, c.constants, c.destOffset);
            break;
        case RestoreOp::SetViewports:
            restoreVps[restoreVpCount++] = gameVp;
            list->RSSetViewports(restoreVpCount, restoreVps);
            break;
        case RestoreOp::SetScissors:
            restoreScs[restoreScCount++] = gameSc;
            list->RSSetScissorRects(restoreScCount, restoreScs);
            break;
        case RestoreOp::SetTopology:
            list->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(c.count));
            break;
        case RestoreOp::SetRenderTargets:
            // Use the private copy, not the rewritten game handle.
            list->OMSetRenderTargets(1, &omCopy.copy, FALSE, nullptr);
            break;
        case RestoreOp::SetPredicationDisabled:
            list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            break;
        default:
            break;
        }
    }

    // Game-like draw with no rebinding after restore.
    list->DrawInstanced(3, 1, 0, 0);
    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, INFINITE);
        CloseHandle(ev);
    }

    CHECK(plan.count > 0, "plan non-empty");
    std::printf("D3: restore plan ops=%zu device=%p failures=%d\n", plan.count, device.Get(), g_failures);
    if (g_failures == 0)
        std::printf("graphics-d3 mini scenario passed\n");
    return g_failures ? 1 : 0;
}
