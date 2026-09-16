#pragma once
#include "GraphicsRestore.h"
#include <d3d12.h>

// Execute a RestorePlan on a live command list. Handles in the snapshot are the
// real COM pointers recorded by the tracker.
namespace AmdPreSr::GraphicsSnap
{

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
            // Tracker stored GPU VA; CBV/SRV/UAV share the same setter family by type.
            // We only distinguish table vs VA in the plan; VA goes to the matching UAV setter
            // when the snapshot recorded UAV (the only VA type A's graphics path uses).
            if (c.graphics)
                cmd->SetGraphicsRootUnorderedAccessView(c.index, c.handle);
            else
                cmd->SetComputeRootUnorderedAccessView(c.index, c.handle);
            break;
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
            if (c.count == 0 && c.handle == 0)
                cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
            else if (c.count == 1)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE rtv { snap.om.rtvHandles[0] };
                if (snap.om.hasDsv)
                {
                    D3D12_CPU_DESCRIPTOR_HANDLE dsv { snap.om.dsvHandle };
                    cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                }
                else
                    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
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
