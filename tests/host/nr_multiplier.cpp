#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/framegen/InterpolationOverride.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/NrSessionActivity.h"
#include <cassert>
#include <climits>
#include <iostream>
#include <thread>

using namespace FrameGeneration;

int main()
{
    InterpolationOverride controller;
    InterpolationRequest request { 2, 3, 5, false, true, true };
    int calls = 0;
    int lastTarget = -1;
    auto apply = [&](int target) {
        ++calls;
        lastTarget = target;
        return InterpolationApplyResult::Success;
    };
    controller.Reset(5);
    assert(controller.Update(request, apply));
    assert(controller.Snapshot().applied == 2 && lastTarget == 2);
    request.nrRunning = true;
    assert(controller.Update(request, apply));
    assert(controller.Snapshot().overridden && lastTarget == 3 && request.baseline == 2);
    const int activeCalls = calls;
    for (unsigned frame = 0; frame < 1000; ++frame)
        assert(!controller.Update(request, apply));
    assert(calls == activeCalls);
    request.baseline = 1;
    assert(!controller.Update(request, apply));
    assert(controller.Snapshot().applied == 3 && request.nrOverride == 3);
    request.nrRunning = false;
    assert(controller.Update(request, apply));
    assert(lastTarget == 1 && !controller.Snapshot().overridden);
    request.nrRunning = true;
    assert(controller.Update(request, apply));
    request.nrOverride = 0;
    assert(controller.Update(request, apply));
    assert(lastTarget == 1 && !controller.Snapshot().overridden);

    request.nrOverride = 3;
    request.maximum = 2;
    assert(controller.Update(request, apply));
    assert(lastTarget == 2 && controller.Snapshot().limited);
    assert(request.baseline == 1 && request.nrOverride == 3);
    request.maximum = 5;
    assert(controller.Update(request, apply));
    assert(lastTarget == 3 && !controller.Snapshot().limited);

    request.liveSupported = false;
    const int oldProviderCalls = calls;
    assert(!controller.Update(request, apply));
    assert(calls == oldProviderCalls && !controller.Snapshot().overridden);
    request.liveSupported = true;
    request.enabled = false;
    assert(!controller.Update(request, apply));
    assert(!controller.Snapshot().overridden);
    request.enabled = true;
    assert(!controller.Update(request, apply));

    auto reject = [&](int) { ++calls; return InterpolationApplyResult::Rejected; };
    request.nrRunning = false;
    assert(!controller.Update(request, reject));
    assert(controller.Snapshot().failed && controller.Snapshot().applied == 3);
    const int rejectedCalls = calls;
    for (unsigned frame = 0; frame < 1000; ++frame)
        assert(!controller.Update(request, reject));
    assert(calls == rejectedCalls);
    request.baseline = 2;
    assert(controller.Update(request, apply));
    assert(!controller.Snapshot().failed && controller.Snapshot().applied == 2);
    request.nrRunning = true;
    assert(!controller.Update(request, reject));
    assert(controller.Snapshot().applied == 2);
    request.nrRunning = false;
    assert(!controller.Update(request, apply));
    request.nrRunning = true;
    assert(controller.Update(request, apply));
    request.nrRunning = false;
    assert(!controller.Update(request, reject));
    controller.Reset(5);
    assert(controller.Update(request, apply));
    assert(controller.Snapshot().applied == 2);

    request.nrRunning = true;
    auto fatal = [&](int) { ++calls; return InterpolationApplyResult::RequiresReinitialization; };
    assert(!controller.Update(request, fatal));
    assert(controller.Snapshot().requiresReinitialization && controller.Snapshot().applied == 2);
    request.nrOverride = 4;
    const int fatalCalls = calls;
    assert(!controller.Update(request, apply));
    assert(calls == fatalCalls);
    controller.Reset(5);
    assert(controller.Update(request, apply));
    assert(!controller.Snapshot().requiresReinitialization && controller.Snapshot().applied == 4);

    request.nrOverride = INT_MAX;
    assert(ResolveInterpolation(request).target == 5);
    request.maximum = 0;
    assert(ResolveInterpolation(request).target == 1);
    request.nrOverride = -1;
    assert(!ResolveInterpolation(request).overridden);

    DlssNr::NrSessionActivity activity;
    assert(!activity.IsRunning());
    const auto first = activity.Token();
    activity.Succeeded(first);
    assert(activity.IsRunning());
    activity.Succeeded(first);
    assert(activity.IsRunning());
    activity.Reset();
    activity.Succeeded(first);
    assert(!activity.IsRunning());
    activity.Succeeded(activity.Token());
    assert(activity.IsRunning());
    for (unsigned cycle = 0; cycle < 256; ++cycle)
    {
        const auto stale = activity.Token();
        std::thread callback([&] { activity.Succeeded(stale); });
        activity.Reset();
        callback.join();
        assert(!activity.IsRunning());
    }
    std::cout << "NR multiplier policy, retry limits and lifecycle epochs: PASS\n";
}
