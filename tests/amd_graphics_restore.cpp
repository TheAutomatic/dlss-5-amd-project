#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/GraphicsRestore.h"
#include <cassert>
#include <iostream>

using namespace AmdPreSr::GraphicsSnap;

static GraphicsSnapshot MakeGameLike()
{
    GraphicsSnapshot s;
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

int main()
{
    TestPlanOrderAndContent();
    TestAlikeDirtThenPlan();
    TestIncompleteRootsSkipped();
    std::cout << "graphics-restore plan scenarios passed\n";
    return 0;
}
