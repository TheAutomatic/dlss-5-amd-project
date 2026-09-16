#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/GraphicsTracker.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace AmdPreSr::GraphicsSnap;

static void TestDisabledByDefault()
{
    Tracker t;
    assert(!t.IsEnabled());
    t.OnCreate(0x1000);
    assert(!t.HasRecord(0x1000));
    t.ReportGraphicsRootSignature(0x1000, 0xA11);
    assert(!t.HasRecord(0x1000));
    assert(!t.CanAdmit(0x1000));
    GraphicsSnapshot frozen;
    assert(!t.TryFreeze(0x1000, frozen));
}

static void TestEnabledCapturesBindings()
{
    Tracker t;
    t.SetEnabled(true);
    t.OnCreate(0x2000);
    assert(t.HasRecord(0x2000));

    t.ReportGraphicsRootSignature(0x2000, 0xA11);
    t.ReportComputeRootSignature(0x2000, 0xC01);
    t.ReportPso(0x2000, 0x9001);
    Viewport vp {};
    vp.width = 1;
    vp.height = 1;
    vp.maxDepth = 1;
    t.ReportViewports(0x2000, &vp, 1);
    ScissorRect sc { 0, 0, 1, 1 };
    t.ReportScissors(0x2000, &sc, 1);
    t.ReportTopology(0x2000, 4);
    t.ReportRenderTargets(0x2000, 0, nullptr, false, false, 0);
    t.ReportPredication(0x2000, 0, 0, 0);

    assert(t.CanAdmit(0x2000));
    GraphicsSnapshot frozen;
    assert(t.TryFreeze(0x2000, frozen));
    assert(frozen.graphics.signature == 0xA11);
    assert(frozen.compute.signature == 0xC01);
    assert(frozen.pso == 0x9001);
}

static void TestRootParamsBothDomains()
{
    Tracker t;
    t.SetEnabled(true);
    t.OnCreate(0x3000);

    t.ReportGraphicsRootSignature(0x3000, 0xA11);
    t.ReportComputeRootSignature(0x3000, 0xC01);

    t.ReportRootTable(0x3000, true, 0, 0x1111);
    t.ReportRootGpuVa(0x3000, true, 2, RootEntryType::UAV, 0x2222);
    const uint32_t four[4] = { 1, 2, 3, 4 };
    t.ReportRootConstants(0x3000, true, 1, four, 4, 0);
    t.ReportRootConstant(0x3000, true, 1, 9, 2);

    t.ReportRootGpuVa(0x3000, false, 0, RootEntryType::CBV, 0xCCCC);

    GraphicsSnapshot frozen;
    // Not yet admissible (PSO/RS missing) but freeze still copies what we have via CanAdmit fail.
    assert(!t.TryFreeze(0x3000, frozen));

    // Domains must not clobber each other.
    t.ReportPso(0x3000, 0x9001);
    Viewport vp {};
    vp.width = 1;
    vp.height = 1;
    t.ReportViewports(0x3000, &vp, 1);
    ScissorRect sc { 0, 0, 1, 1 };
    t.ReportScissors(0x3000, &sc, 1);
    t.ReportTopology(0x3000, 4);
    t.ReportRenderTargets(0x3000, 0, nullptr, false, false, 0);
    t.ReportPredication(0x3000, 0, 0, 0);

    assert(t.TryFreeze(0x3000, frozen));
    assert(frozen.graphics.entries[0].table == 0x1111);
    assert(frozen.graphics.entries[1].constants[2] == 9);
    assert(frozen.graphics.entries[2].gpuVa == 0x2222);
    assert(frozen.compute.entries[0].gpuVa == 0xCCCC);
    assert(frozen.compute.entries[0].type == RootEntryType::CBV);
    assert(frozen.graphics.entries[0].state == BindState::KnownValue);
}

static void TestSignatureClearsParamsThroughReport()
{
    Tracker t;
    t.SetEnabled(true);
    t.OnCreate(0x4000);
    t.ReportGraphicsRootSignature(0x4000, 0xA);
    t.ReportRootTable(0x4000, true, 0, 0x99);

    t.ReportGraphicsRootSignature(0x4000, 0xB);
    GraphicsSnapshot frozen;
    // Incomplete, but we can inspect via a forced complete freeze path.
    t.ReportPso(0x4000, 1);
    Viewport vp {};
    vp.width = 1;
    vp.height = 1;
    t.ReportViewports(0x4000, &vp, 1);
    ScissorRect sc { 0, 0, 1, 1 };
    t.ReportScissors(0x4000, &sc, 1);
    t.ReportTopology(0x4000, 4);
    t.ReportRenderTargets(0x4000, 0, nullptr, false, false, 0);
    t.ReportPredication(0x4000, 0, 0, 0);
    assert(t.TryFreeze(0x4000, frozen));
    assert(frozen.graphics.signature == 0xB);
    assert(frozen.graphics.entries[0].state == BindState::Unknown);
}

static void TestObserverSuppressedRestoreBypasses()
{
    Tracker t;
    t.SetEnabled(true);
    t.OnCreate(0x5000);
    t.ReportGraphicsRootSignature(0x5000, 0xA11);
    t.ReportPso(0x5000, 0x9001);
    Viewport vp {};
    vp.width = 1;
    vp.height = 1;
    t.ReportViewports(0x5000, &vp, 1);
    ScissorRect sc { 0, 0, 1, 1 };
    t.ReportScissors(0x5000, &sc, 1);
    t.ReportTopology(0x5000, 4);
    t.ReportRenderTargets(0x5000, 0, nullptr, false, false, 0);
    t.ReportPredication(0x5000, 0, 0, 0);

    t.PushSuppress(0x5000);
    // Observer path: suppressed, must not dirty the snapshot.
    t.ReportPso(0x5000, 0xDEAD);
    t.ReportGraphicsRootSignature(0x5000, 0xBAD);

    // Restore bridge: must update even while suppressed.
    t.ReportGraphicsRootSignature(0x5000, 0xA11, /*fromRestore=*/true);
    t.ReportPso(0x5000, 0x9001, /*fromRestore=*/true);
    t.ReportRootGpuVa(0x5000, true, 0, RootEntryType::UAV, 0x55, true);
    t.PopSuppress(0x5000);

    assert(t.CanAdmit(0x5000));
    GraphicsSnapshot frozen;
    assert(t.TryFreeze(0x5000, frozen));
    assert(frozen.pso == 0x9001);
    assert(frozen.graphics.signature == 0xA11);
    assert(frozen.graphics.entries[0].gpuVa == 0x55);
}

static void TestUnknownGenerationLazyCreate()
{
    Tracker t;
    t.SetEnabled(true);
    // Report without Create: unknown generation, admission must fail.
    t.ReportGraphicsRootSignature(0x6000, 0xA11);
    assert(t.HasRecord(0x6000));
    assert(!t.CanAdmit(0x6000));

    // Reset without Create: still unknown generation.
    assert(t.OnReset(0x6001, true));
    t.ReportPso(0x6001, 1);
    Viewport vp {};
    vp.width = 1;
    vp.height = 1;
    t.ReportViewports(0x6001, &vp, 1);
    ScissorRect sc { 0, 0, 1, 1 };
    t.ReportScissors(0x6001, &sc, 1);
    t.ReportTopology(0x6001, 4);
    t.ReportRenderTargets(0x6001, 0, nullptr, false, false, 0);
    t.ReportPredication(0x6001, 0, 0, 0);
    t.ReportGraphicsRootSignature(0x6001, 1);
    assert(!t.CanAdmit(0x6001));
}

static void TestLifecycle()
{
    Tracker t;
    t.SetEnabled(true);
    t.OnCreate(0x7000);
    t.ReportGraphicsRootSignature(0x7000, 0xA11);
    t.ReportPso(0x7000, 1);
    Viewport vp {};
    vp.width = 1;
    vp.height = 1;
    t.ReportViewports(0x7000, &vp, 1);
    ScissorRect sc { 0, 0, 1, 1 };
    t.ReportScissors(0x7000, &sc, 1);
    t.ReportTopology(0x7000, 4);
    t.ReportRenderTargets(0x7000, 0, nullptr, false, false, 0);
    t.ReportPredication(0x7000, 0, 0, 0);
    assert(t.CanAdmit(0x7000));

    // Failed Reset keeps the generation.
    assert(!t.OnReset(0x7000, false));
    assert(t.CanAdmit(0x7000));

    // ClearState drops bindings but the record survives.
    t.OnClearState(0x7000);
    assert(t.HasRecord(0x7000));
    assert(!t.CanAdmit(0x7000));

    t.ReportGraphicsRootSignature(0x7000, 0xA11);
    t.ReportPso(0x7000, 1);
    t.ReportViewports(0x7000, &vp, 1);
    t.ReportScissors(0x7000, &sc, 1);
    t.ReportTopology(0x7000, 4);
    t.ReportRenderTargets(0x7000, 0, nullptr, false, false, 0);
    t.ReportPredication(0x7000, 0, 0, 0);
    assert(t.CanAdmit(0x7000));

    // Release drops the record entirely.
    t.OnRelease(0x7000);
    assert(!t.HasRecord(0x7000));
    assert(!t.CanAdmit(0x7000));
}

static void TestMarkIneligible()
{
    Tracker t;
    t.SetEnabled(true);
    t.OnCreate(0x8000);
    t.ReportGraphicsRootSignature(0x8000, 1);
    t.ReportPso(0x8000, 1);
    Viewport vp {};
    vp.width = 1;
    vp.height = 1;
    t.ReportViewports(0x8000, &vp, 1);
    ScissorRect sc { 0, 0, 1, 1 };
    t.ReportScissors(0x8000, &sc, 1);
    t.ReportTopology(0x8000, 4);
    t.ReportRenderTargets(0x8000, 0, nullptr, false, false, 0);
    t.ReportPredication(0x8000, 0, 0, 0);
    assert(t.CanAdmit(0x8000));

    t.MarkIneligible(0x8000);
    assert(!t.CanAdmit(0x8000));
}

int main()
{
    TestDisabledByDefault();
    TestEnabledCapturesBindings();
    TestRootParamsBothDomains();
    TestSignatureClearsParamsThroughReport();
    TestObserverSuppressedRestoreBypasses();
    TestUnknownGenerationLazyCreate();
    TestLifecycle();
    TestMarkIneligible();
    std::cout << "graphics-tracker D2 scenarios passed\n";
    return 0;
}
