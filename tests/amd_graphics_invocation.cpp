#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/GraphicsInvocation.h"
#include <cassert>
#include <iostream>
#include <thread>

using namespace AmdPreSr::GraphicsSnap;

static void TestStartupGraceIsBounded()
{
    GraphicsStartupGate gate;
    // The deadline starts on the first requested but unarmed invocation.
    assert(!gate.ShouldDefer(false, false, 100));
    assert(!gate.ShouldDefer(true, true, 200));
    assert(gate.ShouldDefer(true, false, 1000));
    assert(gate.ShouldDefer(true, false, 2999));
    assert(!gate.ShouldDefer(true, false, 3000));
    assert(!gate.ShouldDefer(true, false, 100000));
    assert(!gate.HasRecorded()); // Expiry alone is not native Record.

    // Disabling/re-enabling the request must not restart the grace period.
    assert(!gate.ShouldDefer(false, false, 100001));
    assert(!gate.ShouldDefer(true, false, 100002));

    GraphicsStartupGate zeroOrigin;
    assert(zeroOrigin.ShouldDefer(true, false, 0));
    assert(!zeroOrigin.ShouldDefer(true, false, 2000));

    GraphicsStartupGate noGrace;
    assert(!noGrace.ShouldDefer(true, false, 42, 0));
}

static void TestReadyOrRecordedDoesNotDelay()
{
    GraphicsStartupGate gate;
    assert(gate.ShouldDefer(true, false, 100));
    assert(!gate.ShouldDefer(true, true, 101));
    // Admission without a native Record does not finish startup by itself.
    assert(gate.ShouldDefer(true, false, 102));
    gate.MarkRecorded();
    assert(gate.HasRecorded());
    assert(!gate.ShouldDefer(true, false, 103));
    assert(!gate.ShouldDefer(true, false, 100000));

    GraphicsStartupGate alreadyRecorded;
    alreadyRecorded.MarkRecorded();
    assert(!alreadyRecorded.ShouldDefer(true, false, 0));
}

static void TestInvocationScopesMatchOnlyCurrentList()
{
    assert(GraphicsInvocationFor(0xA) == nullptr);
    {
        ScopedGraphicsInvocation outer(0xA);
        outer.state.armed = true;
        assert(GraphicsInvocationFor(0xA) == &outer.state);
        assert(GraphicsInvocationFor(0xB) == nullptr);
        {
            ScopedGraphicsInvocation otherList(0xB);
            assert(GraphicsInvocationFor(0xA) == nullptr);
            assert(GraphicsInvocationFor(0xB) == &otherList.state);
            assert(!otherList.state.armed);
        }
        assert(GraphicsInvocationFor(0xA) == &outer.state);
        assert(outer.state.armed);
        {
            ScopedGraphicsInvocation sameList(0xA);
            assert(GraphicsInvocationFor(0xA) == &sameList.state);
            sameList.state.commandsRecorded = true;
        }
        assert(GraphicsInvocationFor(0xA) == &outer.state);
        assert(!outer.state.commandsRecorded);
    }
    assert(GraphicsInvocationFor(0xA) == nullptr);
}

static void TestNativeDrawRequiresListAndVerifiedCaller()
{
    ObserveNativeDraw(0xA, 0x1100); // No observation scope is harmless.
    {
        ScopedNativeDrawObservation outer(0xA, 0x1000, 0x1200);
        ObserveNativeDraw(0xB, 0x1100);
        ObserveNativeDraw(0xA, 0x0FFF);
        ObserveNativeDraw(0xA, 0x1200); // The helper range is half-open.
        assert(outer.observation.count == 0);
        ObserveNativeDraw(0xA, 0x1000);
        ObserveNativeDraw(0xA, 0x11FF);
        assert(outer.observation.count == 2);
        {
            ScopedNativeDrawObservation inner(0xB, 0x2000, 0x2100);
            ObserveNativeDraw(0xA, 0x1000);
            ObserveNativeDraw(0xB, 0x2000);
            assert(inner.observation.count == 1);
            assert(outer.observation.count == 2);
        }
        ObserveNativeDraw(0xA, 0x1100);
        assert(outer.observation.count == 3);
        {
            // An unavailable runtime helper address cannot report a draw.
            ScopedNativeDrawObservation unknownRange(0xA, 0, 0x1200);
            ObserveNativeDraw(0xA, 0x1100);
            assert(unknownRange.observation.count == 0);
        }
    }
    assert(g_nativeDrawObservation == nullptr);
}

static void TestScopesDoNotLeakBetweenRecordingThreads()
{
    ScopedGraphicsInvocation parent(0xA);
    ScopedNativeDrawObservation parentDraw(0xA, 0x1000, 0x1200);
    std::thread recordingThread([] {
        assert(GraphicsInvocationFor(0xA) == nullptr);
        assert(g_nativeDrawObservation == nullptr);
        ScopedGraphicsInvocation child(0xA);
        child.state.armed = true;
        ScopedNativeDrawObservation childDraw(0xA, 0x1000, 0x1200);
        ObserveNativeDraw(0xA, 0x1100);
        assert(childDraw.observation.count == 1);
    });
    recordingThread.join();
    assert(GraphicsInvocationFor(0xA) == &parent.state);
    assert(!parent.state.armed);
    assert(parentDraw.observation.count == 0);
}

int main()
{
    TestStartupGraceIsBounded();
    TestReadyOrRecordedDoesNotDelay();
    TestInvocationScopesMatchOnlyCurrentList();
    TestNativeDrawRequiresListAndVerifiedCaller();
    TestScopesDoNotLeakBetweenRecordingThreads();
    std::cout << "graphics-invocation scenarios passed\n";
    return 0;
}
