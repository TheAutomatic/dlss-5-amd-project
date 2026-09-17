// CPU-only production replay check. Requires the Windows SDK, not a device/GPU.
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/GraphicsRestoreDx12.h"
#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>

using namespace AmdPreSr::GraphicsSnap;

namespace
{
// A local x64 COM ABI stub avoids implementing the other 57 command-list methods.
// These are the base ID3D12GraphicsCommandList vtable indices, including IUnknown.
// Only the three RS/IA operations selected below may reach this fake object.
static_assert(sizeof(void*) == 8, "This command-list ABI fixture is x64 only");
constexpr std::size_t kTopologyIndex = 20;
constexpr std::size_t kViewportsIndex = 21;
constexpr std::size_t kScissorsIndex = 22;

struct CommandListCalls
{
    void** vtable;
    unsigned viewportCalls = 0;
    unsigned scissorCalls = 0;
    unsigned topologyCalls = 0;
    UINT viewportCount = 99;
    UINT scissorCount = 99;
    bool viewportsNull = false;
    bool scissorsNull = false;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
};

void STDMETHODCALLTYPE UnexpectedCall(ID3D12GraphicsCommandList*)
{
    // Do not let an accidental additional replay operation pass silently.
    std::abort();
}

void STDMETHODCALLTYPE SetViewports(ID3D12GraphicsCommandList* self, UINT count, const D3D12_VIEWPORT* values)
{
    auto& calls = *reinterpret_cast<CommandListCalls*>(self);
    ++calls.viewportCalls;
    calls.viewportCount = count;
    calls.viewportsNull = values == nullptr;
}

void STDMETHODCALLTYPE SetScissors(ID3D12GraphicsCommandList* self, UINT count, const D3D12_RECT* values)
{
    auto& calls = *reinterpret_cast<CommandListCalls*>(self);
    ++calls.scissorCalls;
    calls.scissorCount = count;
    calls.scissorsNull = values == nullptr;
}

void STDMETHODCALLTYPE SetTopology(ID3D12GraphicsCommandList* self, D3D12_PRIMITIVE_TOPOLOGY topology)
{
    auto& calls = *reinterpret_cast<CommandListCalls*>(self);
    ++calls.topologyCalls;
    calls.topology = topology;
}

void CheckEmptyRsIaReplay(const GraphicsSnapshot& snapshot)
{
    assert(snapshot.viewportState == BindState::KnownUnset && snapshot.viewportCount == 0);
    assert(snapshot.scissorState == BindState::KnownUnset && snapshot.scissorCount == 0);
    assert(snapshot.topologyState == BindState::KnownUnset && snapshot.topology == 0);

    RestorePlan fullPlan;
    assert(BuildRestorePlan(snapshot, fullPlan));
    RestorePlan rsIaPlan;
    for (std::size_t i = 0; i < fullPlan.count; ++i)
    {
        const auto& op = fullPlan.ops[i];
        if (op.op == RestoreOp::SetViewports || op.op == RestoreOp::SetScissors ||
            op.op == RestoreOp::SetTopology)
        {
            assert(op.count == 0);
            assert(rsIaPlan.Push(op));
        }
    }
    assert(rsIaPlan.count == 3);

    std::array<void*, 60> vtable;
    vtable.fill(reinterpret_cast<void*>(&UnexpectedCall));
    vtable[kTopologyIndex] = reinterpret_cast<void*>(&SetTopology);
    vtable[kViewportsIndex] = reinterpret_cast<void*>(&SetViewports);
    vtable[kScissorsIndex] = reinterpret_cast<void*>(&SetScissors);
    CommandListCalls calls { vtable.data() };

    // Execute the production adapter, rather than a second test-only replay loop.
    ApplyRestorePlan(reinterpret_cast<ID3D12GraphicsCommandList*>(&calls), snapshot, rsIaPlan);
    assert(calls.viewportCalls == 1 && calls.viewportCount == 0 && calls.viewportsNull);
    assert(calls.scissorCalls == 1 && calls.scissorCount == 0 && calls.scissorsNull);
    assert(calls.topologyCalls == 1 && calls.topology == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);
}

void SetNonemptyRsIa(ListTracker& tracker)
{
    const Viewport viewport { 0, 0, 1280, 720, 0, 1 };
    const ScissorRect scissor { 0, 0, 1280, 720 };
    tracker.snap.SetViewports(&viewport, 1);
    tracker.snap.SetScissors(&scissor, 1);
    tracker.snap.SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}
} // namespace

int main()
{
    ListTracker tracker;
    tracker.OnCreate(1);
    CheckEmptyRsIaReplay(tracker.snap);

    SetNonemptyRsIa(tracker);
    assert(tracker.OnReset(true));
    CheckEmptyRsIaReplay(tracker.snap);

    SetNonemptyRsIa(tracker);
    tracker.OnClearState();
    CheckEmptyRsIaReplay(tracker.snap);

    std::cout << "graphics-restore DX12 empty RS/IA CPU replay scenarios passed\n";
    return 0;
}
