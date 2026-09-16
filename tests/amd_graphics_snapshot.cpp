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
    assert(std::string(a.reason) == "graphics_root_unknown");

    GraphicsSnapshot frozen;
    assert(!TryFreeze(t, frozen));
}

static GraphicsSnapshot MakeAdmissible()
{
    GraphicsSnapshot s;
    s.graphics.SetSignature(0xA11);
    s.compute.SetSignature(0xC01);
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

    // Same signature again: params must survive.
    d.signature = 0xA;
    d.signatureState = BindState::KnownValue;
    // SetSignature always clears — production may special-case same-sig.
    // Model the D3D12 rule: switching signature clears; re-set of same also
    // goes through SetSignature here, so call the low-level clear only on change.
    d.ClearParams();
    d.signature = 0xA;
    d.signatureState = BindState::KnownValue;
    assert(d.entries[0].state == BindState::Unknown);

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

// Documented contract: SetSignature always drops arguments. Same-signature
// rebind that should keep params must be a no-op at the API layer; tests
// lock the clear-on-write behaviour so restore cannot assume survivors.
static void TestSetSignatureAlwaysInvalidates()
{
    RootDomain d;
    d.SetSignature(0x1);
    d.SetTable(0, 0x99);
    d.SetSignature(0x1); // same value, still a SetGraphicsRootSignature call
    assert(d.entries[0].state == BindState::Unknown);
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
    assert(std::string(a.reason) == "graphics_root_unknown");

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
    assert(t.snap.graphics.signatureState == BindState::Unknown);
    assert(t.snap.psoState == BindState::Unknown);
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

    // Non-empty OM records handles; single-handle range keeps only the base.
    const std::uint64_t handles[2] = { 0xAA, 0xBB };
    t.snap.SetRenderTargets(2, handles, false, true, 0xDD);
    assert(t.snap.om.numRTVs == 2);
    assert(t.snap.om.rtvHandles[0] == 0xAA);
    assert(t.snap.om.rtvHandles[1] == 0xBB);
    assert(t.snap.om.hasDsv);
    assert(t.snap.om.dsvHandle == 0xDD);
    assert(CanAdmitGraphics(t).ok);

    t.snap.SetRenderTargets(2, handles, true, false, 0);
    assert(t.snap.om.singleHandleRange);
    assert(t.snap.om.rtvHandles[0] == 0xAA);
    assert(t.snap.om.rtvHandles[1] == 0);
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
    TestSetSignatureAlwaysInvalidates();
    TestConstantMerge();
    TestGenerationAndReset();
    TestClearStateIsNotReset();
    TestPredicationGate();
    TestOmStates();
    TestPsoRequired();
    TestReleaseDropsRecord();
    TestFailedAdmissionFreeze();
    std::cout << "graphics-snapshot D1 scenarios passed\n";
    return 0;
}
