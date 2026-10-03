#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/framegen/FrameResourceReadiness.h"
#include <cassert>
#include <thread>
#include <cstdio>
int main()
{
    FrameResourceReadiness ready;
    assert(!ready.ContainsMask(3));
    ready.Mark(0); assert(!ready.ContainsMask(3));
    ready.Mark(1); assert(ready.ContainsMask(3));
    ready.Reset(); ready.Mark(1); assert(!ready.ContainsMask(3));
    ready.Mark(32); assert(!ready.Contains(32));
    // Reset and reads race in the reported failure. No container is erased while read.
    std::thread reset([&] { for (int i=0;i<100000;++i) ready.Reset(); });
    std::thread depth([&] { for (int i=0;i<100000;++i) ready.Mark(0); });
    std::thread velocity([&] { for (int i=0;i<100000;++i) ready.Mark(1); });
    for (int i=0;i<100000;++i) (void)ready.ContainsMask(3);
    reset.join(); depth.join(); velocity.join();
    ready.Reset(); assert(!ready.ContainsMask(3));
    // Concurrent publication must not lose either flag.
    for (int i=0;i<200;++i) {
        ready.Reset();
        std::thread a([&] { ready.Mark(0); });
        std::thread b([&] { ready.Mark(1); });
        a.join(); b.join(); assert(ready.ContainsMask(3));
    }
    std::puts("FG_RESOURCE_READINESS_OK");
}
