#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/GraphicsTracker.h"
#include <cassert>
#include <iostream>
#include <string>
#include <thread>

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

    // A successful Reset defines a generation even without Create.
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
    assert(t.CanAdmit(0x6001));
    assert(!t.OnReset(0x6002, false));
    assert(!t.HasRecord(0x6002));

    // A late Reset also repairs a record made by an earlier lazy observation.
    assert(t.OnReset(0x6000, true, 0xBEEF));
    uint32_t generation = 0;
    bool generationKnown = false;
    GraphicsSnapshot copy;
    assert(t.CopyState(0x6000, copy, generation, generationKnown));
    assert(generationKnown && generation == 1 && copy.pso == 0xBEEF);
    assert(copy.compute.signatureState == BindState::KnownUnset);
    assert(copy.graphics.signatureState == BindState::KnownUnset);
    assert(copy.heapState == BindState::KnownUnset && copy.heapCount == 0);
}

static void CompleteGameBindings(Tracker& t, uint64_t list)
{
    t.ReportGraphicsRootSignature(list, 0xABC);
    t.ReportComputeRootSignature(list, 0xDEF);
    t.ReportPso(list, 0x9001);
    const Viewport vp { 0, 0, 1280, 720, 0, 1 };
    const ScissorRect sc { 0, 0, 1280, 720 };
    t.ReportViewports(list, &vp, 1);
    t.ReportScissors(list, &sc, 1);
    t.ReportTopology(list, 4);
    t.ReportRenderTargets(list, 0, nullptr, false, false, 0);
    t.ReportPredication(list, 0, 0, 0);
}

static void TestQueryPairsAndRenderPass()
{
    Tracker t;
    t.SetEnabled(true);
    t.OnCreate(1);
    CompleteGameBindings(t, 1);
    t.OnBeginQuery(1, 100, 0, 3);
    t.OnBeginQuery(1, 200, 0, 3);
    t.OnEndQuery(1, 100, 2, 3, true); // End-only timestamp closes neither.
    assert(std::string(t.AdmitReason(1)) == "query_active");
    t.OnEndQuery(1, 100, 0, 3);
    assert(std::string(t.AdmitReason(1)) == "query_active");
    t.OnEndQuery(1, 200, 0, 4); // Wrong index cannot close it.
    assert(!t.CanAdmit(1));
    t.OnClearState(1, 0x9002);
    CompleteGameBindings(t, 1);
    assert(!t.CanAdmit(1)); // ClearState is not EndQuery.
    t.OnEndQuery(1, 200, 0, 3);
    assert(t.CanAdmit(1));

    t.OnBeginRenderPass(1, true);
    assert(t.IsRenderPassUnsafe(1));
    assert(std::string(t.AdmitReason(1)) == "render_pass");
    t.OnEndRenderPass(1);
    assert(t.IsRenderPassUnsafe(1));
    assert(std::string(t.AdmitReason(1)) == "render_pass_suspended");
    t.OnBeginRenderPass(1, false, true);
    t.OnEndRenderPass(1);
    assert(!t.IsRenderPassUnsafe(1));
    assert(std::string(t.AdmitReason(1)) == "om_unknown");
    t.ReportRenderTargets(1, 0, nullptr, false, false, 0);
    assert(t.CanAdmit(1));
    t.OnBeginQuery(1, 100, 0, 3);
    assert(t.OnReset(1, true));
    CompleteGameBindings(t, 1);
    assert(t.CanAdmit(1));
}

static void TestNestedListSuppressionAndIndirectPostState()
{
    Tracker t;
    t.SetEnabled(true);
    t.OnCreate(1, 1);
    t.OnCreate(2, 2);
    CompleteGameBindings(t, 1);
    CompleteGameBindings(t, 2);
    const uint32_t words[] = { 10, 20, 30, 40 };
    t.ReportRootConstants(1, true, 0, words, 4, 0);
    t.ReportRootGpuVa(1, true, 1, RootEntryType::CBV, 0x123);
    t.ReportRootTable(1, true, 2, 0x456);
    t.ReportGraphicsRootSignature(1, 0xABC);
    t.ReportIndirectRootConstants(1, true, 0, 1, 2);
    t.ReportIndirectRootGpuVa(1, true, 1, RootEntryType::CBV);

    {
        ScopedCaptureSuppression outer(t, 1);
        t.ReportPso(1, 0xBAD);
        {
            ScopedCaptureSuppression inner(t, 1);
            t.ReportRootTable(1, true, 2, 0xBAD);
            t.ReportPso(2, 0x222); // Other list is not suppressed.
            t.ReportPso(1, 0x111, true); // Direct restore reports bypass.
        }
        t.ReportPso(1, 0xBAD); // Outer scope still suppresses.
        t.OnBeginQuery(1, 10, 0, 1);
        t.OnBeginRenderPass(1);
    }
    GraphicsSnapshot copy;
    assert(t.TryFreeze(1, copy));
    assert(copy.pso == 0x111);
    assert(copy.graphics.entries[0].constants[0] == 10);
    assert(copy.graphics.entries[0].constants[1] == 0);
    assert(copy.graphics.entries[0].constants[2] == 0);
    assert(copy.graphics.entries[0].constants[3] == 40);
    assert(copy.graphics.entries[1].gpuVa == 0);
    assert(copy.graphics.entries[1].type == RootEntryType::CBV);
    assert(copy.graphics.entries[2].table == 0x456);
    assert(t.TryFreeze(2, copy) && copy.pso == 0x222);
    t.ReportPso(1, 0x333);
    assert(t.TryFreeze(1, copy) && copy.pso == 0x333);
    t.MarkIneligible(1, IneligibleWhy::Indirect);
    assert(std::string(t.AdmitReason(1)) == "execute_indirect");
}

static void TestOmFailureAndFrozenOwnership()
{
    Tracker t;
    t.SetEnabled(true);
    t.OnCreate(1);
    t.OnCreate(2);
    CompleteGameBindings(t, 1);
    CompleteGameBindings(t, 2);
    auto owner1 = std::make_shared<int>(1);
    auto owner2 = std::make_shared<int>(2);
    std::weak_ptr<int> weak1 = owner1, weak2 = owner2;
    const uint64_t rtvs1[] = { 0x11, 0x12 };
    const uint64_t rtvs2[] = { 0x21, 0x22 };
    t.ReportRenderTargets(1, 2, rtvs1, false, false, 0, false, owner1);
    t.ReportRenderTargets(2, 2, rtvs2, false, false, 0, false, owner2);
    owner1.reset();
    owner2.reset();
    GraphicsSnapshot frozen;
    assert(t.TryFreeze(1, frozen));
    t.OnReset(1, true);
    assert(!weak1.expired() && !weak2.expired());
    t.ReportOmUnknown(2);
    assert(weak2.expired() && !weak1.expired());
    assert(std::string(t.AdmitReason(2)) == "om_unknown");
    frozen = {};
    assert(weak1.expired());
}

static void TestConcurrentReportAndRelease()
{
    Tracker t;
    t.SetEnabled(true);
    std::thread writer([&] {
        for (uint64_t i = 1; i != 2000; ++i)
        {
            t.ReportGraphicsRootSignature(1, i);
            t.ReportRootTable(1, true, 0, i);
            t.ReportPso(1, i);
        }
    });
    std::thread lifecycle([&] {
        GraphicsSnapshot copy;
        uint32_t gen = 0;
        bool known = false;
        for (unsigned i = 0; i != 2000; ++i)
        {
            t.OnCreate(1);
            t.CopyState(1, copy, gen, known);
            t.OnRelease(1);
        }
    });
    writer.join();
    lifecycle.join();
    t.OnCreate(1);
    CompleteGameBindings(t, 1);
    assert(t.CanAdmit(1));
}

static void TestHeapReportsPreserveSameSetAndInvalidateChangedSet()
{
    Tracker t;
    t.SetEnabled(true);
    t.OnCreate(1);
    CompleteGameBindings(t, 1);
    const uint64_t heaps[] = { 0x100, 0x200 };
    const uint64_t reordered[] = { 0x200, 0x100 };
    const uint64_t other[] = { 0x300 };
    t.ReportHeaps(1, 2, heaps);
    t.ReportRootTable(1, true, 0, 0xABC);
    t.ReportRootTable(1, false, 0, 0xDEF);
    t.ReportRootGpuVa(1, true, 1, RootEntryType::CBV, 0x123);
    t.ReportRootConstant(1, false, 1, 42, 0);
    {
        ScopedCaptureSuppression scope(t, 1);
        t.ReportHeaps(1, 1, other); // Internal temporary heap is not game state.
        t.ReportHeaps(1, 2, reordered, true); // Direct restore of same set.
    }
    GraphicsSnapshot frozen;
    assert(t.TryFreeze(1, frozen));
    assert(frozen.graphics.entries[0].table == 0xABC);
    assert(frozen.compute.entries[0].table == 0xDEF);
    t.ReportHeaps(1, 1, other);
    assert(t.TryFreeze(1, frozen));
    assert(frozen.graphics.entries[0].state == BindState::Unknown);
    assert(frozen.compute.entries[0].state == BindState::Unknown);
    assert(frozen.graphics.entries[1].gpuVa == 0x123);
    assert(frozen.compute.entries[1].constants[0] == 42);
    t.ReportHeaps(1, 1, nullptr);
    assert(std::string(t.AdmitReason(1)) == "descriptor_heaps_unknown");
    t.ReportHeaps(1, 0, nullptr);
    assert(t.CanAdmit(1));
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
    TestQueryPairsAndRenderPass();
    TestNestedListSuppressionAndIndirectPostState();
    TestOmFailureAndFrozenOwnership();
    TestConcurrentReportAndRelease();
    TestHeapReportsPreserveSameSetAndInvalidateChangedSet();
    std::cout << "graphics-tracker D2 scenarios passed\n";
    return 0;
}
