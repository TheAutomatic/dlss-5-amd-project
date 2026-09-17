#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/GraphicsRestore.h"
#include <cassert>
#include <iostream>

using namespace AmdPreSr::GraphicsSnap;

static GraphicsSnapshot MakeGameLike()
{
    GraphicsSnapshot s;
    s.SetHeaps(0, nullptr);
    s.compute.SetSignature(0xC01);
    s.graphics.SetSignature(0xA11);
    s.graphics.SetTable(0, 0x1111);
    const std::uint32_t four[4] = { 1, 2, 3, 4 };
    s.graphics.MergeConstants(1, four, 4, 0);
    s.SetPso(0x9001);
    Viewport vp {};
    vp.width = 1280;
    vp.height = 720;
    vp.maxDepth = 1;
    s.SetViewports(&vp, 1);
    ScissorRect sc { 0, 0, 1280, 720 };
    s.SetScissors(&sc, 1);
    s.SetTopology(4);
    s.SetRenderTargets(0, nullptr, false, false, 0);
    s.SetPredication(0, 0, 0);
    return s;
}

static void TestPlanOrderAndContent()
{
    auto s = MakeGameLike();
    RestorePlan plan;
    assert(BuildRestorePlan(s, plan));
    assert(plan.count >= 6);

    // Compute root before graphics root.
    std::size_t iCompute = plan.count, iGraphics = plan.count, iPso = plan.count, iVs = plan.count, iPred = plan.count;
    for (std::size_t i = 0; i < plan.count; ++i)
    {
        switch (plan.ops[i].op)
        {
        case RestoreOp::SetComputeRootSignature:
            iCompute = i;
            break;
        case RestoreOp::SetGraphicsRootSignature:
            iGraphics = i;
            break;
        case RestoreOp::SetPso:
            iPso = i;
            break;
        case RestoreOp::SetViewports:
            iVs = i;
            break;
        case RestoreOp::SetPredicationDisabled:
            iPred = i;
            break;
        default:
            break;
        }
    }
    assert(iCompute < iGraphics);
    assert(iGraphics < iPso);
    assert(iPso < iVs);
    assert(iVs < iPred);

    bool sawTable = false, sawConsts = false;
    for (std::size_t i = 0; i < plan.count; ++i)
    {
        const auto& c = plan.ops[i];
        if (c.op == RestoreOp::SetRootTable && c.graphics && c.index == 0)
        {
            sawTable = true;
            assert(c.handle == 0x1111);
        }
        if (c.op == RestoreOp::SetRootConstants && c.graphics && c.index == 1)
        {
            sawConsts = true;
            assert(c.count == 4);
            assert(c.constants[2] == 3);
        }
    }
    assert(sawTable && sawConsts);
}

static void TestAlikeDirtThenPlan()
{
    // Simulate A: overwrite viewport/OM/pred; freeze must still plan the game values.
    auto frozen = MakeGameLike();
    auto dirted = frozen;
    Viewport tiny { 0, 0, 1, 1, 0, 1 };
    dirted.SetViewports(&tiny, 1);
    dirted.SetRenderTargets(0, nullptr, false, false, 0);
    dirted.SetPredication(0xBEEF, 0, 0);

    RestorePlan plan;
    assert(BuildRestorePlan(frozen, plan));
    for (std::size_t i = 0; i < plan.count; ++i)
        assert(plan.ops[i].op != RestoreOp::SetPredicationDisabled || frozen.predication.IsDisabled());
    // Dirted snapshot would refuse admission / plan differently.
    RestorePlan dirtyPlan;
    assert(BuildRestorePlan(dirted, dirtyPlan));
    bool dirtyHasPredOff = false;
    for (std::size_t i = 0; i < dirtyPlan.count; ++i)
        if (dirtyPlan.ops[i].op == RestoreOp::SetPredicationDisabled)
            dirtyHasPredOff = true;
    assert(!dirtyHasPredOff);
}

static void TestIncompleteRootsSkipped()
{
    GraphicsSnapshot s;
    s.graphics.SetSignature(0xA);
    // No PSO/RS — plan still builds; those ops omitted (admission would have refused freeze).
    RestorePlan plan;
    assert(BuildRestorePlan(s, plan));
    for (std::size_t i = 0; i < plan.count; ++i)
        assert(plan.ops[i].op != RestoreOp::SetPso && plan.ops[i].op != RestoreOp::SetViewports);
}

static void TestCbvSrvNotForcedToUav()
{
    GraphicsSnapshot s;
    s.graphics.SetSignature(0xA);
    s.graphics.SetGpuVa(0, RootEntryType::CBV, 0xC0);
    s.graphics.SetGpuVa(1, RootEntryType::SRV, 0x51);
    s.graphics.SetGpuVa(2, RootEntryType::UAV, 0x52);
    RestorePlan plan;
    assert(BuildRestorePlan(s, plan));
    bool sawCbv = false, sawSrv = false, sawUav = false;
    for (std::size_t i = 0; i < plan.count; ++i)
    {
        const auto& c = plan.ops[i];
        if (c.op != RestoreOp::SetRootGpuVa)
            continue;
        if (c.index == 0)
        {
            assert(c.gpuVaType == RootEntryType::CBV);
            sawCbv = true;
        }
        if (c.index == 1)
        {
            assert(c.gpuVaType == RootEntryType::SRV);
            sawSrv = true;
        }
        if (c.index == 2)
        {
            assert(c.gpuVaType == RootEntryType::UAV);
            sawUav = true;
        }
    }
    assert(sawCbv && sawSrv && sawUav);
}

static void TestSparseConstantsRestoreOnlyObservedRanges()
{
    GraphicsSnapshot s;
    s.graphics.SetSignature(1);
    s.graphics.SetConstant(0, 12, 1);
    s.graphics.SetConstant(0, 34, 3);
    s.graphics.SetSignature(1); // Rebinding must retain both nonadjacent DWORDs.
    RestorePlan plan;
    assert(BuildRestorePlan(s, plan));
    unsigned ranges = 0;
    for (size_t i = 0; i < plan.count; ++i)
    {
        const auto& c = plan.ops[i];
        if (c.op != RestoreOp::SetRootConstants)
            continue;
        assert(c.count == 1);
        assert(c.destOffset == (ranges == 0 ? 1u : 3u));
        assert(c.constants[0] == (ranges == 0 ? 12u : 34u));
        ++ranges;
    }
    assert(ranges == 2);
}

static void TestKnownEmptyBindingsAreRestored()
{
    ListTracker t;
    t.OnCreate(1);
    RestorePlan plan;
    assert(BuildRestorePlan(t.snap, plan));
    assert(plan.count >= 4);
    assert(plan.ops[0].op == RestoreOp::SetDescriptorHeaps);
    assert(plan.ops[0].count == 0);
    bool computeNull = false, graphicsNull = false, emptyOm = false;
    unsigned emptyViewports = 0, emptyScissors = 0, undefinedTopology = 0;
    for (size_t i = 0; i < plan.count; ++i)
    {
        const auto& c = plan.ops[i];
        if (c.op == RestoreOp::SetComputeRootSignature)
            computeNull = c.handle == 0;
        if (c.op == RestoreOp::SetGraphicsRootSignature)
            graphicsNull = c.handle == 0;
        if (c.op == RestoreOp::SetRenderTargets)
            emptyOm = c.count == 0 && c.handle == 0;
        if (c.op == RestoreOp::SetViewports)
        {
            assert(c.count == 0);
            ++emptyViewports;
        }
        if (c.op == RestoreOp::SetScissors)
        {
            assert(c.count == 0);
            ++emptyScissors;
        }
        if (c.op == RestoreOp::SetTopology)
        {
            assert(c.count == 0); // D3D_PRIMITIVE_TOPOLOGY_UNDEFINED
            ++undefinedTopology;
        }
    }
    assert(computeNull && graphicsNull && emptyOm);
    assert(emptyViewports == 1 && emptyScissors == 1 && undefinedTopology == 1);

    GraphicsSnapshot unknown;
    assert(BuildRestorePlan(unknown, plan));
    assert(plan.count == 0); // Unobserved is different from deliberately empty.
}

static void TestHeapSwitchDoesNotReplayStaleTables()
{
    auto s = MakeGameLike();
    const uint64_t heaps[] = { 0x123 };
    s.SetHeaps(1, heaps);
    s.compute.SetTable(0, 0x1000);
    s.graphics.SetTable(0, 0x2000);
    s.compute.SetGpuVa(1, RootEntryType::SRV, 0x3000);
    s.SetHeaps(0, nullptr);
    RestorePlan plan;
    assert(BuildRestorePlan(s, plan));
    bool sawVa = false, sawConstants = false;
    for (size_t i = 0; i < plan.count; ++i)
    {
        assert(plan.ops[i].op != RestoreOp::SetRootTable);
        sawVa |= plan.ops[i].op == RestoreOp::SetRootGpuVa;
        sawConstants |= plan.ops[i].op == RestoreOp::SetRootConstants;
    }
    assert(sawVa && sawConstants);
}

int main()
{
    TestPlanOrderAndContent();
    TestAlikeDirtThenPlan();
    TestIncompleteRootsSkipped();
    TestCbvSrvNotForcedToUav();
    TestSparseConstantsRestoreOnlyObservedRanges();
    TestKnownEmptyBindingsAreRestored();
    TestHeapSwitchDoesNotReplayStaleTables();
    std::cout << "graphics-restore plan scenarios passed\n";
    return 0;
}
