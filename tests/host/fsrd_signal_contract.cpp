// Consumer invariants adapted from burak113/OptiScaler 5a6adb10601fd16d62504febba850d8839e8f5fe
// fsrd_signal_policy_test.cpp. No copied routing implementation or Config stand-in.
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/upscalers/fsr31/FSRDSignalPolicy.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "fsrd_signal_contract:%d: %s\n", __LINE__, #condition); \
    std::exit(1); } } while (false)

using namespace FSRDSignals;
constexpr uint32_t DD = Bit(DirectDiffuse), DS = Bit(DirectSpecular);
constexpr uint32_t ID = Bit(IndirectDiffuse), IS = Bit(IndirectSpecular);

static void Expect(const Plan& plan, uint32_t mask, bool fix = false, bool copy = false, uint32_t notes = 0)
{
    CHECK(plan.mask == mask);
    CHECK(plan.albedoFix == fix && plan.diffuseCopy == copy);
    CHECK(plan.notes == notes);
}

static void GuideArrivalAndDropout()
{
    const Request request;
    CHECK(!request.estimate); // Primary depth cannot silently become title hit distance.
    const auto first = MakePlan(request, false, false);
    Expect(first, DD | DS);
    const auto specularArrives = MakePlan(request, true, false);
    Expect(specularArrives, DD | IS);
    const auto bothArrive = MakePlan(request, true, true);
    Expect(bothArrive, ID | IS);
    const auto specularDrops = MakePlan(request, false, true);
    Expect(specularDrops, ID | DS);
    const auto bothDrop = MakePlan(request, false, false);
    Expect(bothDrop, DD | DS);
    CHECK(first == bothDrop); // Guide presence is per-frame, never sticky.
    CHECK((bothArrive.mask & ~Feedable(bothArrive, false, false, true)) == IS);
    CHECK((bothArrive.mask & ~Feedable(bothArrive, false, true, false)) == ID);
    CHECK((bothArrive.mask & ~Feedable(bothArrive, false, false, false)) == (ID | IS));

    Request explicitIndirect;
    explicitIndirect.diffuse = explicitIndirect.specular = Indirect;
    Expect(MakePlan(explicitIndirect, false, false), DD | DS, false, false,
           DiffuseNeedsDistance | SpecularNeedsDistance);
    Expect(MakePlan(explicitIndirect, true, false), DD | IS, false, false, DiffuseNeedsDistance);
    Expect(MakePlan(explicitIndirect, false, true), ID | DS, false, false, SpecularNeedsDistance);
    Expect(MakePlan(explicitIndirect, true, true), ID | IS);
    explicitIndirect.estimate = true;
    const auto estimated = MakePlan(explicitIndirect, false, false);
    Expect(estimated, ID | IS);
    CHECK((estimated.mask & ~Feedable(estimated, true, false, false)) == 0);

    Request direct;
    direct.diffuse = direct.specular = Direct;
    Expect(MakePlan(direct, true, true), DD | DS); // Valid guides do not override explicit Direct.
    Request split;
    split.diffuse = split.specular = Split;
    Expect(MakePlan(split, true, true), All);
    Expect(MakePlan(split, true, false), DD | DS | IS, false, false, DiffuseNeedsDistance);
    Expect(MakePlan(split, false, true), DD | ID | DS, false, false, SpecularNeedsDistance);
    Expect(MakePlan(split, false, false), DD | DS, false, false,
           DiffuseNeedsDistance | SpecularNeedsDistance);
    split.estimate = true;
    Expect(MakePlan(split, false, false), All);
}

static void LobeEnablesAndFourSignalFeedability()
{
    Request request;
    request.denoiseDiffuse = false;
    Expect(MakePlan(request, true, true), IS);
    request.denoiseDiffuse = true;
    request.denoiseSpecular = false;
    Expect(MakePlan(request, true, true), ID);
    request.denoiseDiffuse = false;
    Expect(MakePlan(request, true, true), ID | IS); // Both off normalizes to both on.

    request = {};
    request.albedoFix = true;
    Expect(MakePlan(request, false, false), DD | DS, false, false, FixNeedsDistance);
    Expect(MakePlan(request, false, true), ID | DS, false, false, FixNeedsDistance);
    // Full fix supplies diffuse-copy distance from primary view depth without enabling
    // the user's general estimate setting. Missing title diffuse distance is supported.
    const auto full = MakePlan(request, true, false);
    Expect(full, All, true, true);
    CHECK((full.mask & SpecularBits) == SpecularBits && (full.mask & DiffuseBits) != 0);
    CHECK((full.mask & ~Feedable(full, false, true, false)) == 0);
    CHECK((full.mask & ~Feedable(full, false, false, false)) == IS);
    CHECK((Feedable(full, false, false, false) & ID) != 0);
    CHECK((full.mask & ~Feedable(full, true, false, false)) == 0);
    Expect(MakePlan(request, false, false), DD | DS, false, false, FixNeedsDistance);
    Expect(MakePlan(request, true, false), All, true, true); // Recovers after dropout.

    request.fixDiffuse = false;
    Expect(MakePlan(request, true, false), DD | DS | IS, true);
    Expect(MakePlan(request, true, true), ID | DS | IS, true);
    request.diffuse = Split;
    const auto specularOnly = MakePlan(request, true, true);
    Expect(specularOnly, All, true);
    // Four slots alone do not imply a diffuse copy: ordinary split ID still needs a guide.
    CHECK((specularOnly.mask & ~Feedable(specularOnly, false, true, false)) == ID);
    CHECK((specularOnly.mask & ~Feedable(specularOnly, false, false, false)) == (ID | IS));
    Expect(MakePlan(request, true, false), DD | DS | IS, true, false, DiffuseNeedsDistance);

    request = {};
    request.albedoFix = true;
    request.denoiseDiffuse = false;
    Expect(MakePlan(request, true, true), IS, false, false, FixNeedsBothLobes);
    request.denoiseDiffuse = true;
    request.denoiseSpecular = false;
    Expect(MakePlan(request, true, true), ID, false, false, FixNeedsBothLobes);
    request.denoiseDiffuse = false;
    Expect(MakePlan(request, true, false), All, true, true);
    request = {};
    request.albedoFix = request.estimate = true;
    Expect(MakePlan(request, false, false), All, true, true);
}

static void LocalSignalTypeMigration()
{
    // Maps this product's own retired Diffuse/SpecularSignalType keys (0 Direct,
    // 1 Indirect, unset Auto) onto per-lobe routing. SignalCount/Signal1-4 were
    // never shipped here and have no migration path.
    const auto autoBoth = FromSignalTypes({}, -1, -1);
    CHECK(autoBoth.diffuse == Auto && autoBoth.specular == Auto);

    const auto directBoth = FromSignalTypes({}, 0, 0);
    CHECK(directBoth.diffuse == Direct && directBoth.specular == Direct);
    Expect(MakePlan(directBoth, true, true), DD | DS);

    const auto indirectBoth = FromSignalTypes({}, 1, 1);
    CHECK(indirectBoth.diffuse == Indirect && indirectBoth.specular == Indirect);
    Expect(MakePlan(indirectBoth, true, true), ID | IS);

    const auto mixed = FromSignalTypes({}, 0, 1);
    CHECK(mixed.diffuse == Direct && mixed.specular == Indirect);
    Expect(MakePlan(mixed, true, true), DD | IS);

    const auto unsetDiff = FromSignalTypes({}, -1, 1);
    CHECK(unsetDiff.diffuse == Auto && unsetDiff.specular == Indirect);
    Expect(MakePlan(unsetDiff, true, true), ID | IS);

    CHECK(RouteFromCode("auto") == Auto && RouteFromCode("direct") == Direct);
    CHECK(RouteFromCode("indirect") == Indirect && RouteFromCode("split") == Split);
    CHECK(RouteFromCode("both") == Auto);
}

static void RuntimeReportPreservesDropout()
{
    Request request;
    request.albedoFix = true;
    const Plan full = MakePlan(request, true, false);
    // A selected full-fix plan and whether that fix actually ran are distinct.
    // The packed status must not silently turn inactive/dropout into active.
    for (unsigned bits = 0; bits < 16; ++bits)
    {
        const Status original{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0, full};
        const auto restored = Status::Unpack(original.Pack());
        CHECK(restored.observed == original.observed);
        CHECK(restored.specularGuide == original.specularGuide && restored.diffuseGuide == original.diffuseGuide);
        CHECK(restored.albedoFixActive == original.albedoFixActive && restored.plan == full);
    }
    const Plan paused = MakePlan(request, false, false);
    const auto report = Status::Unpack(Status{true, false, false, false, paused}.Pack());
    CHECK(report.plan == paused && !report.albedoFixActive);
    CHECK(report.plan.notes == FixNeedsDistance);
}

static void CurrentGuideContinuityConsumers()
{
    // Exercise the same state carried through production deferred recreation,
    // not a test-only routing loop using independent guide booleans.
    for (int route : {int(Auto), int(Indirect), int(Split)})
    {
        Request request;
        request.diffuse = request.specular = route;
        Guides guides;
        guides.Observe(true, true);
        auto plan = guides.Resolve(request);
        CHECK((plan.mask & (ID | IS)) == (ID | IS));
        CHECK((plan.mask & ~guides.FeedableMask(plan, request.estimate)) == 0);
        guides.Observe(false, true);
        CHECK(guides.seenSpecular && guides.seenDiffuse && !guides.specular && guides.diffuse);
        auto replacement = guides;
        plan = replacement.Resolve(request);
        CHECK((plan.mask & IS) == 0 && (plan.mask & DS) != 0);
        CHECK((plan.mask & ~replacement.FeedableMask(plan, false)) == 0);
        auto status = Status::Unpack(Status{true, replacement.specular, replacement.diffuse, false, plan}.Pack());
        CHECK(!status.specularGuide && status.diffuseGuide);
        replacement.Observe(false, false);
        guides = replacement; // The next replacement must not plan from ever-seen bits.
        plan = guides.Resolve(request);
        CHECK(plan.mask == (DD | DS));
        CHECK((plan.mask & ~guides.FeedableMask(plan, false)) == 0);
        CHECK(guides.seenSpecular && guides.seenDiffuse);
        if (route != Auto) CHECK(plan.notes == (DiffuseNeedsDistance | SpecularNeedsDistance));
        guides.Observe(true, true);
        plan = guides.Resolve(request);
        CHECK((plan.mask & (ID | IS)) == (ID | IS));
        CHECK((plan.mask & ~guides.FeedableMask(plan, false)) == 0);
    }

    Request fix;
    fix.albedoFix = true;
    Guides guides;
    guides.Observe(true, false);
    CHECK(guides.Resolve(fix).diffuseCopy);
    guides.Observe(false, false);
    CHECK(guides.seenSpecular && !guides.specular);
    auto replacement = guides;
    const auto fallback = replacement.Resolve(fix);
    Expect(fallback, DD | DS, false, false, FixNeedsDistance);
    CHECK((fallback.mask & ~replacement.FeedableMask(fallback, false)) == 0);
    replacement.Observe(true, false);
    CHECK(replacement.Resolve(fix).diffuseCopy);
    fix.estimate = true;
    replacement.Observe(false, false);
    const auto estimated = replacement.Resolve(fix);
    CHECK(estimated.diffuseCopy && (estimated.mask & ~replacement.FeedableMask(estimated, true)) == 0);
}

int main()
{
    GuideArrivalAndDropout();
    LobeEnablesAndFourSignalFeedability();
    LocalSignalTypeMigration();
    RuntimeReportPreservesDropout();
    CurrentGuideContinuityConsumers();
    std::puts("fsrd_signal_contract: PASS (production route, guide and feedability CPU behavior)");
}
