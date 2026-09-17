#pragma once
#include "GraphicsRestore.h"
#include <d3d12.h>
#include <mutex>
#include <wrl/client.h>

// Execute a RestorePlan on a live command list. Handles in the snapshot are the
// real COM pointers recorded by the tracker.
namespace AmdPreSr::GraphicsSnap
{

// Frozen OM CPU copies for one NR invocation. The capture ring may be reused
// while restore is pending; these slots are only rewritten at the next pin.
struct FrozenOm
{
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsv;
    UINT rtvCap = 0;
    UINT dsvCap = 0;
};

inline std::mutex g_frozenOmMutex;
inline FrozenOm g_frozenOm;

// Copy snap.om CPU descriptors into a private heap and retarget handles there.
// Call after a successful freeze, before A dirties the list.
inline bool PinOmForRestore(ID3D12Device* device, GraphicsSnapshot& snap)
{
    if (!device || snap.om.state != BindState::KnownValue)
        return true;
    std::lock_guard<std::mutex> lock(g_frozenOmMutex);
    const UINT needRtv = snap.om.numRTVs ? snap.om.numRTVs : 1;
    if (!g_frozenOm.rtv || g_frozenOm.rtvCap < needRtv)
    {
        D3D12_DESCRIPTOR_HEAP_DESC d {};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        d.NumDescriptors = needRtv < 8 ? 8 : needRtv;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        if (FAILED(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap))))
            return false;
        g_frozenOm.rtv = heap;
        g_frozenOm.rtvCap = d.NumDescriptors;
    }
    if (snap.om.hasDsv)
    {
        if (!g_frozenOm.dsv || g_frozenOm.dsvCap < 1)
        {
            D3D12_DESCRIPTOR_HEAP_DESC d {};
            d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            d.NumDescriptors = 1;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
            if (FAILED(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap))))
                return false;
            g_frozenOm.dsv = heap;
            g_frozenOm.dsvCap = 1;
        }
    }
    const UINT incR = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtvBase = g_frozenOm.rtv->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < snap.om.numRTVs; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE src { snap.om.rtvHandles[i] };
        D3D12_CPU_DESCRIPTOR_HANDLE dst = rtvBase;
        dst.ptr += static_cast<SIZE_T>(i) * incR;
        device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        snap.om.rtvHandles[i] = dst.ptr;
    }
    if (snap.om.hasDsv)
    {
        const UINT incD = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_CPU_DESCRIPTOR_HANDLE src { snap.om.dsvHandle };
        auto dst = g_frozenOm.dsv->GetCPUDescriptorHandleForHeapStart();
        device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        snap.om.dsvHandle = dst.ptr;
    }
    return true;
}

inline void ApplyRestorePlan(ID3D12GraphicsCommandList* cmd, const GraphicsSnapshot& snap, const RestorePlan& plan)
{
    if (!cmd)
        return;
    D3D12_VIEWPORT vps[kMaxViewports];
    D3D12_RECT scissors[kMaxScissors];
    UINT vpCount = 0, scCount = 0;
    for (std::size_t i = 0; i < plan.count; ++i)
    {
        const auto& c = plan.ops[i];
        switch (c.op)
        {
        case RestoreOp::SetDescriptorHeaps:
            if (snap.heapState == BindState::KnownValue && snap.heapCount && snap.heapCount <= 2)
            {
                ID3D12DescriptorHeap* hs[2] {};
                for (UINT k = 0; k < snap.heapCount; ++k)
                    hs[k] = reinterpret_cast<ID3D12DescriptorHeap*>(snap.heaps[k]);
                cmd->SetDescriptorHeaps(snap.heapCount, hs);
            }
            break;
        case RestoreOp::SetComputeRootSignature:
            cmd->SetComputeRootSignature(reinterpret_cast<ID3D12RootSignature*>(c.handle));
            break;
        case RestoreOp::SetGraphicsRootSignature:
            cmd->SetGraphicsRootSignature(reinterpret_cast<ID3D12RootSignature*>(c.handle));
            break;
        case RestoreOp::SetRootTable:
        {
            const D3D12_GPU_DESCRIPTOR_HANDLE h { c.handle };
            if (c.graphics)
                cmd->SetGraphicsRootDescriptorTable(c.index, h);
            else
                cmd->SetComputeRootDescriptorTable(c.index, h);
            break;
        }
        case RestoreOp::SetRootGpuVa:
        {
            const auto va = c.handle;
            if (c.graphics)
            {
                if (c.gpuVaType == RootEntryType::CBV)
                    cmd->SetGraphicsRootConstantBufferView(c.index, va);
                else if (c.gpuVaType == RootEntryType::SRV)
                    cmd->SetGraphicsRootShaderResourceView(c.index, va);
                else
                    cmd->SetGraphicsRootUnorderedAccessView(c.index, va);
            }
            else
            {
                if (c.gpuVaType == RootEntryType::CBV)
                    cmd->SetComputeRootConstantBufferView(c.index, va);
                else if (c.gpuVaType == RootEntryType::SRV)
                    cmd->SetComputeRootShaderResourceView(c.index, va);
                else
                    cmd->SetComputeRootUnorderedAccessView(c.index, va);
            }
            break;
        }
        case RestoreOp::SetRootConstants:
            if (c.graphics)
                cmd->SetGraphicsRoot32BitConstants(c.index, c.count, c.constants, c.destOffset);
            else
                cmd->SetComputeRoot32BitConstants(c.index, c.count, c.constants, c.destOffset);
            break;
        case RestoreOp::SetPso:
            cmd->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(c.handle));
            break;
        case RestoreOp::SetViewports:
            vpCount = c.count;
            if (vpCount <= kMaxViewports && snap.viewportCount == vpCount)
            {
                for (UINT k = 0; k < vpCount; ++k)
                {
                    const auto& v = snap.viewports[k];
                    vps[k] = { v.topLeftX, v.topLeftY, v.width, v.height, v.minDepth, v.maxDepth };
                }
                cmd->RSSetViewports(vpCount, vps);
            }
            break;
        case RestoreOp::SetScissors:
            scCount = c.count;
            if (scCount <= kMaxScissors && snap.scissorCount == scCount)
            {
                for (UINT k = 0; k < scCount; ++k)
                {
                    const auto& r = snap.scissors[k];
                    scissors[k] = { r.left, r.top, r.right, r.bottom };
                }
                cmd->RSSetScissorRects(scCount, scissors);
            }
            break;
        case RestoreOp::SetTopology:
            cmd->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(c.count));
            break;
        case RestoreOp::SetRenderTargets:
            if (snap.om.state != BindState::KnownValue)
                break;
            if (snap.om.numRTVs == 0 && !snap.om.hasDsv)
                cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
            else if (snap.om.numRTVs <= kMaxRTVs)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kMaxRTVs] {};
                for (UINT k = 0; k < snap.om.numRTVs; ++k)
                    rtvs[k].ptr = snap.om.rtvHandles[k];
                D3D12_CPU_DESCRIPTOR_HANDLE dsv { snap.om.dsvHandle };
                cmd->OMSetRenderTargets(snap.om.numRTVs, snap.om.numRTVs ? rtvs : nullptr, FALSE,
                                        snap.om.hasDsv ? &dsv : nullptr);
            }
            break;
        case RestoreOp::SetPredicationDisabled:
            cmd->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            break;
        default:
            break;
        }
    }
}

} // namespace AmdPreSr::GraphicsSnap
