#pragma once
#include "GraphicsSnapshot.h"

// Ordered restore replay for a frozen GraphicsSnapshot.
// D3 and future host integration both walk this plan; no D3D12 types here.
namespace AmdPreSr::GraphicsSnap
{

enum class RestoreOp : std::uint8_t
{
    Nop,
    SetComputeRootSignature,
    SetGraphicsRootSignature,
    SetRootTable,
    SetRootGpuVa,
    SetRootConstants,
    SetPso,
    SetViewports,
    SetScissors,
    SetTopology,
    SetRenderTargets,
    SetPredicationDisabled,
};

struct RestoreCmd
{
    RestoreOp op = RestoreOp::Nop;
    bool graphics = false;
    std::uint32_t index = 0;
    std::uint64_t handle = 0;
    std::uint32_t count = 0;
    std::uint32_t destOffset = 0;
    std::uint32_t constants[kMaxRootConstants] {};
};

inline constexpr std::size_t kMaxRestoreOps = 1 + kMaxRootParams * 2 * 4 + 16;

struct RestorePlan
{
    RestoreCmd ops[kMaxRestoreOps] {};
    std::size_t count = 0;

    bool Push(const RestoreCmd& c)
    {
        if (count >= kMaxRestoreOps)
            return false;
        ops[count++] = c;
        return true;
    }
};

// Design §5.3 order: compute root/args → graphics root/args → PSO → RS/IA → OM → pred.
inline bool BuildRestorePlan(const GraphicsSnapshot& s, RestorePlan& out)
{
    out.count = 0;
    if (s.compute.signatureState != BindState::Unknown)
    {
        RestoreCmd c {};
        c.op = RestoreOp::SetComputeRootSignature;
        c.handle = s.compute.signature;
        if (!out.Push(c))
            return false;
    }
    if (s.graphics.signatureState != BindState::Unknown)
    {
        RestoreCmd c {};
        c.op = RestoreOp::SetGraphicsRootSignature;
        c.handle = s.graphics.signature;
        if (!out.Push(c))
            return false;
    }
    for (int domain = 0; domain < 2; ++domain)
    {
        const auto& d = domain ? s.graphics : s.compute;
        if (d.signatureState == BindState::Unknown)
            continue;
        for (std::uint32_t i = 0; i < kMaxRootParams; ++i)
        {
            const auto& e = d.entries[i];
            if (e.state != BindState::KnownValue)
                continue;
            RestoreCmd c {};
            c.graphics = domain != 0;
            c.index = i;
            switch (e.type)
            {
            case RootEntryType::Table:
                c.op = RestoreOp::SetRootTable;
                c.handle = e.table;
                break;
            case RootEntryType::CBV:
            case RootEntryType::SRV:
            case RootEntryType::UAV:
                c.op = RestoreOp::SetRootGpuVa;
                c.handle = e.gpuVa;
                break;
            case RootEntryType::Constant:
            case RootEntryType::Constants:
                c.op = RestoreOp::SetRootConstants;
                c.count = e.numConstants;
                c.destOffset = 0;
                for (std::uint32_t k = 0; k < e.numConstants && k < kMaxRootConstants; ++k)
                    c.constants[k] = e.constants[k];
                break;
            default:
                continue;
            }
            if (!out.Push(c))
                return false;
        }
    }
    if (s.psoState == BindState::KnownValue)
    {
        RestoreCmd c {};
        c.op = RestoreOp::SetPso;
        c.handle = s.pso;
        if (!out.Push(c))
            return false;
    }
    if (s.viewportState == BindState::KnownValue && s.viewportCount)
    {
        // Caller maps snapshot viewports; plan carries count only.
        RestoreCmd c {};
        c.op = RestoreOp::SetViewports;
        c.count = s.viewportCount;
        if (!out.Push(c))
            return false;
    }
    if (s.scissorState == BindState::KnownValue && s.scissorCount)
    {
        RestoreCmd c {};
        c.op = RestoreOp::SetScissors;
        c.count = s.scissorCount;
        if (!out.Push(c))
            return false;
    }
    if (s.topologyState == BindState::KnownValue)
    {
        RestoreCmd c {};
        c.op = RestoreOp::SetTopology;
        c.count = s.topology;
        if (!out.Push(c))
            return false;
    }
    if (s.om.state == BindState::KnownValue)
    {
        RestoreCmd c {};
        c.op = RestoreOp::SetRenderTargets;
        c.count = s.om.numRTVs;
        c.handle = s.om.hasDsv ? s.om.dsvHandle : 0;
        c.graphics = s.om.singleHandleRange;
        if (!out.Push(c))
            return false;
    }
    if (s.predication.IsDisabled())
    {
        RestoreCmd c {};
        c.op = RestoreOp::SetPredicationDisabled;
        if (!out.Push(c))
            return false;
    }
    return true;
}

} // namespace AmdPreSr::GraphicsSnap
