#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/GraphicsSnapshot.h"
#include <cassert>
#include <iostream>

using namespace AmdPreSr::GraphicsSnap;

static void TestUnknownNeverAdmits()
{
    ListTracker t;
    auto a = CanAdmitGraphics(t);
    assert(!a.ok);
    assert(std::string(a.reason) == "unknown_generation");

    t.OnCreate(0x1000);
    a = CanAdmitGraphics(t);
    assert(!a.ok);
    assert(std::string(a.reason) == "pso_unknown_or_unset");

    GraphicsSnapshot frozen;
    assert(!TryFreeze(t, frozen));
}

static GraphicsSnapshot MakeAdmissible()
{
    GraphicsSnapshot s;
    s.graphics.SetSignature(0xA11);
    s.compute.SetSignature(0xC01);
    s.SetHeaps(0, nullptr);
    s.SetPso(0x9001);
    Viewport vp {};
    vp.width = 1;
    vp.height = 1;
    vp.maxDepth = 1;
    s.SetViewports(&vp, 1);
    ScissorRect sc { 0, 0, 1, 1 };
    s.SetScissors(&sc, 1);
    s.SetTopology(4); // TRIANGLELIST
    s.SetRenderTargets(0, nullptr, false, false, 0);
    s.SetPredication(0, 0, 0);
    return s;
}

static void TestHappyPathFreeze()
{
    ListTracker t;
    t.OnCreate(0x2000);
    t.snap = MakeAdmissible();
    auto a = CanAdmitGraphics(t);
    assert(a.ok);

    GraphicsSnapshot frozen;
    assert(TryFreeze(t, frozen));
    assert(frozen.pso == 0x9001);
    assert(frozen.graphics.signature == 0xA11);
    assert(frozen.viewportCount == 1);
    assert(frozen.om.state == BindState::KnownUnset);

    // Mutating the live tracker after freeze must not change the frozen copy.
    t.snap.SetPso(0xDEAD);
    assert(frozen.pso == 0x9001);
}

static void TestSignatureChangeClearsParams()
{
    RootDomain d;
    d.SetSignature(0xA);
    d.SetTable(0, 0x1111);
    d.SetGpuVa(1, RootEntryType::UAV, 0x2222);
    assert(d.entries[0].state == BindState::KnownValue);
    assert(d.entries[1].state == BindState::KnownValue);

    // Same signature again must preserve the actual game API state.
    d.SetSignature(0xA);
    assert(d.entries[0].table == 0x1111);
    assert(d.entries[1].gpuVa == 0x2222);

    // Different signature clears.
    d.SetSignature(0xA);
    d.SetTable(0, 0x1111);
    d.SetSignature(0xB);
    assert(d.entries[0].state == BindState::Unknown);
    assert(d.signature == 0xB);

    // Null signature is known-unset, still clears params.
    d.SetSignature(0xB);
    d.SetGpuVa(2, RootEntryType::CBV, 0x33);
    d.SetSignature(0);
    assert(d.signatureState == BindState::KnownUnset);
    assert(d.entries[2].state == BindState::Unknown);

    // Graphics and compute domains are independent.
    RootDomain g, c;
    g.SetSignature(0x600);
    c.SetSignature(0xC00);
    g.SetTable(0, 0x1);
    assert(c.entries[0].state == BindState::Unknown);
}

static void TestSameSignaturePreservesPartialConstants()
{
    RootDomain d;
    d.SetSignature(0x1);
    d.SetTable(0, 0x99);
    const uint32_t values[] = { 10, 20, 30, 40 };
    d.MergeConstants(1, values, 4, 0);
    d.SetSignature(0x1);
    d.SetConstant(1, 99, 2);
    assert(d.entries[0].table == 0x99);
    assert(d.entries[1].constants[0] == 10);
    assert(d.entries[1].constants[1] == 20);
    assert(d.entries[1].constants[2] == 99);
    assert(d.entries[1].constants[3] == 40);
    assert(d.signature == 0x1);
}

static void TestConstantMerge()
{
    RootDomain d;
    d.SetSignature(0x1);
    const std::uint32_t four[4] = { 1, 2, 3, 4 };
    assert(d.MergeConstants(1, four, 4, 0));
    assert(d.entries[1].numConstants == 4);
    assert(d.entries[1].constants[0] == 1);
    assert(d.entries[1].constants[3] == 4);

    d.SetConstant(1, 9, 2);
    assert(d.entries[1].numConstants == 4);
    assert(d.entries[1].constants[0] == 1);
    assert(d.entries[1].constants[1] == 2);
    assert(d.entries[1].constants[2] == 9);
    assert(d.entries[1].constants[3] == 4);

    // Sparse write must not clobber earlier dwords.
    d.SetConstant(1, 7, 0);
    assert(d.entries[1].constants[0] == 7);
    assert(d.entries[1].constants[2] == 9);

    // Switching that slot to a table drops constants.
    d.SetTable(1, 0xABCD);
    assert(d.entries[1].type == RootEntryType::Table);
    assert(d.entries[1].numConstants == 0);
    assert(d.entries[1].table == 0xABCD);

    // Out-of-range merge fails closed.
    assert(!d.MergeConstants(0, four, 4, kMaxRootConstants - 2));
    assert(d.entries[0].state == BindState::Unknown);
}

static void TestGenerationAndReset()
{
    ListTracker t;
    t.OnCreate(0x30);
    t.snap = MakeAdmissible();
    assert(CanAdmitGraphics(t).ok);

    // Failed Reset does not cancel the generation.
    assert(!t.OnReset(false));
    assert(t.generation == 1);
    assert(CanAdmitGraphics(t).ok);

    // Successful Reset: new generation, defaults, previous snapshot gone.
    assert(t.OnReset(true));
    assert(t.generation == 2);
    auto a = CanAdmitGraphics(t);
    assert(!a.ok);
    assert(std::string(a.reason) == "pso_unknown_or_unset");

    t.snap = MakeAdmissible();
    t.MarkIneligible(); // bundle/indirect
    a = CanAdmitGraphics(t);
    assert(!a.ok);
    assert(std::string(a.reason) == "ineligible_generation");

    t.OnReset(true);
    assert(!t.ineligible);
    t.snap = MakeAdmissible();
    assert(CanAdmitGraphics(t).ok);
}

static void TestClearStateIsNotReset()
{
    ListTracker t;
    t.OnCreate(0x40);
    t.snap = MakeAdmissible();
    t.snap.renderPassActive = true;
    t.snap.queryActive = true;
    t.MarkIneligible();

    t.OnClearState();
    // Bindings cleared...
    assert(t.snap.graphics.signatureState == BindState::KnownUnset);
    assert(t.snap.compute.signatureState == BindState::KnownUnset);
    assert(t.snap.heapState == BindState::KnownUnset);
    assert(t.snap.psoState == BindState::KnownUnset);
    // ...but RP/query/ineligible markers are not Reset.
    assert(t.snap.renderPassActive);
    assert(t.snap.queryActive);
    assert(t.ineligible);
    assert(t.generation == 1);

    auto a = CanAdmitGraphics(t);
    assert(!a.ok);
}

static void TestPredicationGate()
{
    ListTracker t;
    t.OnCreate(0x50);
    t.snap = MakeAdmissible();

    t.snap.SetPredication(0xBEEF, 0, 0);
    auto a = CanAdmitGraphics(t);
    assert(!a.ok);
    assert(std::string(a.reason) == "predication_active_or_unknown");

    t.snap.SetPredication(0, 0, 0);
    assert(CanAdmitGraphics(t).ok);
    assert(t.snap.predication.IsDisabled());

    t.snap.predication.state = BindState::Unknown;
    a = CanAdmitGraphics(t);
    assert(!a.ok);
    assert(std::string(a.reason) == "predication_active_or_unknown");
}

static void TestOmStates()
{
    ListTracker t;
    t.OnCreate(0x60);
    t.snap = MakeAdmissible();

    // Unknown OM blocks admission.
    t.snap.om.state = BindState::Unknown;
    auto a = CanAdmitGraphics(t);
    assert(!a.ok);
    assert(std::string(a.reason) == "om_unknown");

    // Empty OM is known-unset and allowed (A clears RTV/DSV itself).
    t.snap.SetRenderTargets(0, nullptr, false, false, 0);
    assert(t.snap.om.state == BindState::KnownUnset);
    assert(CanAdmitGraphics(t).ok);

    // Internal OM is always explicit; all handles must be present and nonzero.
    const std::uint64_t handles[2] = { 0xAA, 0xBB };
    t.snap.SetRenderTargets(2, handles, false, true, 0xDD);
    assert(t.snap.om.numRTVs == 2);
    assert(t.snap.om.rtvHandles[0] == 0xAA);
    assert(t.snap.om.rtvHandles[1] == 0xBB);
    assert(t.snap.om.hasDsv);
    assert(t.snap.om.dsvHandle == 0xDD);
    assert(CanAdmitGraphics(t).ok);

    t.snap.SetRenderTargets(2, handles, true, false, 0);
    assert(t.snap.om.state == BindState::Unknown);
    assert(!CanAdmitGraphics(t).ok);
    const uint64_t incomplete[] = { 0xAA, 0 };
    t.snap.SetRenderTargets(2, incomplete, false, false, 0);
    assert(t.snap.om.state == BindState::Unknown);
    t.snap.SetRenderTargets(1, nullptr, false, false, 0);
    assert(t.snap.om.state == BindState::Unknown);
    t.snap.SetRenderTargets(0, nullptr, false, true, 0);
    assert(t.snap.om.state == BindState::Unknown);
}

static void TestInitialPsoAndOwnerLifetime()
{
    ListTracker t;
    t.OnCreate(0x100, 0xABC);
    assert(t.snap.pso == 0xABC);
    assert(t.snap.psoState == BindState::KnownValue);
    assert(!t.OnReset(false, 0xDEF));
    assert(t.snap.pso == 0xABC);
    assert(t.OnReset(true, 0xDEF));
    assert(t.snap.pso == 0xDEF);
    t.OnClearState(0x123);
    assert(t.snap.pso == 0x123);
    assert(t.snap.om.state == BindState::KnownUnset);

    t.snap = MakeAdmissible();
    auto descriptorBlock = std::make_shared<int>(7);
    std::weak_ptr<int> weak = descriptorBlock;
    const uint64_t rtvs[] = { 100, 200 };
    t.snap.SetRenderTargets(2, rtvs, false, true, 300, descriptorBlock);
    descriptorBlock.reset();
    GraphicsSnapshot frozen;
    assert(TryFreeze(t, frozen));
    t.OnReset(true);
    assert(!weak.expired()); // Frozen owner, not a global cursor, protects it.
    assert(frozen.om.rtvHandles[1] == 200);
    frozen = {};
    assert(weak.expired());
}

static void TestPsoRequired()
{
    ListTracker t;
    t.OnCreate(0x70);
    t.snap = MakeAdmissible();
    t.snap.SetPso(0);
    auto a = CanAdmitGraphics(t);
    assert(!a.ok);
    assert(std::string(a.reason) == "pso_unknown_or_unset");

    t.snap.psoState = BindState::Unknown;
    a = CanAdmitGraphics(t);
    assert(!a.ok);
    assert(std::string(a.reason) == "pso_unknown_or_unset");
}

static void TestApiDefaultsAndUnknownBindings()
{
    ListTracker t;
    t.OnCreate(0x71, 0x9001);
    assert(t.snap.graphics.signatureState == BindState::KnownUnset);
    assert(t.snap.compute.signatureState == BindState::KnownUnset);
    assert(t.snap.graphics.signature == 0 && t.snap.compute.signature == 0);
    assert(t.snap.heapState == BindState::KnownUnset && t.snap.heapCount == 0);

    t.snap = MakeAdmissible();
    t.snap.compute.signatureState = BindState::Unknown;
    assert(std::string(CanAdmitGraphics(t).reason) == "compute_root_unknown");
    t.snap.compute.SetSignature(0);
    assert(CanAdmitGraphics(t).ok);
    t.snap.heapState = BindState::Unknown;
    assert(std::string(CanAdmitGraphics(t).reason) == "descriptor_heaps_unknown");
    t.snap.SetHeaps(0, nullptr);
    assert(CanAdmitGraphics(t).ok);
    t.snap.graphics.signatureState = BindState::Unknown;
    assert(std::string(CanAdmitGraphics(t).reason) == "graphics_root_unknown");

    assert(t.OnReset(true, 0x9002));
    assert(t.snap.graphics.signatureState == BindState::KnownUnset);
    assert(t.snap.compute.signatureState == BindState::KnownUnset);
    assert(t.snap.heapState == BindState::KnownUnset);
}

static void TestHeapChangesInvalidateOnlyTables()
{
    GraphicsSnapshot s;
    s.compute.SetSignature(0xC);
    s.graphics.SetSignature(0xA);
    const uint64_t heaps[] = { 0x100, 0x200 };
    const uint64_t reordered[] = { 0x200, 0x100 };
    const uint64_t changed[] = { 0x100, 0x300 };
    s.SetHeaps(2, heaps);
    for (auto* domain : { &s.compute, &s.graphics })
    {
        domain->SetTable(0, 0xABC);
        domain->SetGpuVa(1, RootEntryType::CBV, 0x123);
        domain->SetConstant(2, 42, 3);
    }
    s.SetHeaps(2, heaps);
    assert(s.compute.entries[0].table == 0xABC);
    s.SetHeaps(2, reordered);
    assert(s.compute.entries[0].state == BindState::KnownValue);
    assert(s.graphics.entries[0].state == BindState::KnownValue);
    s.SetHeaps(2, changed);
    for (auto* domain : { &s.compute, &s.graphics })
    {
        assert(domain->entries[0].state == BindState::Unknown);
        assert(domain->entries[1].gpuVa == 0x123);
        assert(domain->entries[1].state == BindState::KnownValue);
        assert(domain->entries[2].constants[3] == 42);
        assert(domain->entries[2].knownConstants == (uint64_t { 1 } << 3));
        domain->SetTable(0, 0xDEF);
    }
    s.SetHeaps(0, nullptr);
    assert(s.heapState == BindState::KnownUnset && s.heapCount == 0);
    assert(s.compute.entries[0].state == BindState::Unknown);
    assert(s.graphics.entries[0].state == BindState::Unknown);
    s.SetHeaps(1, nullptr); // Invalid observation must not masquerade as empty.
    assert(s.heapState == BindState::Unknown);
    const uint64_t invalid[] = { 0 };
    s.SetHeaps(1, invalid);
    assert(s.heapState == BindState::Unknown);
}

static void TestReleaseDropsRecord()
{
    ListTracker t;
    t.OnCreate(0x80);
    t.snap = MakeAdmissible();
    assert(t.live);
    t.OnRelease();
    assert(!t.live);
    assert(!t.generationKnown);
    assert(!CanAdmitGraphics(t).ok);

    // Address reuse must start clean.
    t.OnCreate(0x80);
    assert(t.generation == 1);
    assert(CanAdmitGraphics(t).ok == false);
}

static void TestFailedAdmissionFreeze()
{
    ListTracker t;
    t.OnCreate(0x90);
    // Incomplete snapshot
    t.snap.graphics.SetSignature(1);
    GraphicsSnapshot out;
    out.pso = 0x1234;
    assert(!TryFreeze(t, out));
    assert(out.pso == 0x1234); // freeze failure leaves dest untouched
}

int main()
{
    TestUnknownNeverAdmits();
    TestHappyPathFreeze();
    TestSignatureChangeClearsParams();
    TestSameSignaturePreservesPartialConstants();
    TestConstantMerge();
    TestGenerationAndReset();
    TestClearStateIsNotReset();
    TestPredicationGate();
    TestOmStates();
    TestPsoRequired();
    TestApiDefaultsAndUnknownBindings();
    TestHeapChangesInvalidateOnlyTables();
    TestReleaseDropsRecord();
    TestFailedAdmissionFreeze();
    TestInitialPsoAndOwnerLifetime();
    std::cout << "graphics-snapshot D1 scenarios passed\n";
    return 0;
}
