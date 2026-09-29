#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <cassert>
#include <condition_variable>
#include <future>
#include <iostream>
#include <thread>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfEvaluateCut.h"

namespace Cut = DlssNr::Backend::LmxxfCut;
using namespace DlssNr::Backend;

struct Session
{
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, finish = false, alive = true;
    unsigned calls = 0;
};

static int32_t Enqueue(void* context, void* job, void*)
{
    auto& s = *static_cast<Session*>(context);
    assert(job == context);
    std::unique_lock lock(s.mutex);
    assert(s.alive);
    ++s.calls;
    s.entered = true;
    s.cv.notify_all();
    s.cv.wait(lock, [&] { return s.finish; });
    assert(s.alive); // The teardown thread must not pass the callback lifetime barrier.
    return 0;
}

int main()
{
    assert(PrepareSubmissionAtStartup(Kind::Daniel, true, true));
    assert(!PrepareSubmissionAtStartup(Kind::Daniel, true, false));
    assert(!PrepareSubmissionAtStartup(Kind::Daniel, false, true));
    assert(PrepareSubmissionAtStartup(Kind::Lmxxf, false, true));

    for (unsigned cycle = 0; cycle < 32; ++cycle)
    {
        Session s;
        auto* list = reinterpret_cast<ID3D12CommandList*>(uintptr_t(0x1234));
        Cut::SetPendingEnqueue(&s, &s, Enqueue, nullptr, list);
        // No queue/COM operations occur: the callback is bound to a list identity.
        std::thread submit([&] { Cut::BetweenThunk(nullptr, list, nullptr); });
        {
            std::unique_lock lock(s.mutex);
            s.cv.wait(lock, [&] { return s.entered; });
        }
        std::promise<void> attempted, released;
        auto releasedFuture = released.get_future();
        std::thread off([&] {
            attempted.set_value();
            Cut::DisarmBetweenSlot();
            std::lock_guard lock(s.mutex);
            s.alive = false;
            released.set_value();
        });
        attempted.get_future().wait();
        assert(releasedFuture.wait_for(std::chrono::milliseconds(2)) == std::future_status::timeout);
        {
            std::lock_guard lock(s.mutex);
            s.finish = true;
            s.cv.notify_all();
        }
        submit.join();
        off.join();
        assert(!s.alive && s.calls == 1);
        // Execute may already have copied the thunk before it was disarmed.
        // A late call must not recover a stale session pointer.
        Cut::BetweenThunk(nullptr, list, nullptr);
        assert(s.calls == 1);
    }
    std::cout << "NR startup policy and in-flight/late callback teardown: PASS\n";
}
