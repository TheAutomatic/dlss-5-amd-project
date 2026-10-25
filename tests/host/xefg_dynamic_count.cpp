#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/framegen/xefg/XeFG_DynamicCount.h"
#include <cassert>
#include <cstdio>

int main()
{
    for (bool disable1x : {false, true})
    {
        XeFG_DynamicCount controller;
        XeFG_DynamicCount::Sample sample;
        sample.maxCount = 2;
        sample.minCount = 0;
        sample.currentCount = 0;
        sample.targetFps = 120;
        sample.frameMs = 40;
        bool triedUp = false, wentBack = false;
        for (int frame = 0; frame < 100; ++frame)
        {
            sample.nowMs = frame * 50.;
            const int count = controller.Update(sample);
            assert(count >= sample.minCount && count <= sample.maxCount);
            if (!triedUp && count == 1)
            {
                triedUp = true;
                sample.frameMs = 80; // Failed 1X->2X trial: generated frames cost too much.
                sample.minCount = disable1x ? 1 : 0;
            }
            else if (triedUp && count == 0)
                wentBack = true;
            sample.currentCount = count;
        }
        assert(triedUp);
        assert(wentBack != disable1x);
    }

    // Tighten bounds mid-experiment and during a loading sample.
    for (int start = 0; start <= 5; ++start)
    for (int max = 1; max <= 5; ++max)
    for (int min = 0; min <= 1; ++min)
    {
        XeFG_DynamicCount controller;
        XeFG_DynamicCount::Sample sample;
        sample.targetFps = 120;
        sample.frameMs = 50;
        sample.maxCount = 5;
        sample.minCount = 0;
        sample.currentCount = start;
        for (int frame = 0; frame < 120; ++frame)
        {
            if (frame == 16)
            {
                sample.maxCount = max;
                sample.minCount = min;
                sample.frameMs = 300;
            }
            if (frame == 17)
                sample.frameMs = 80;
            sample.nowMs = frame * 50.;
            const int count = controller.Update(sample);
            assert(count >= sample.minCount && count <= sample.maxCount);
            sample.currentCount = count;
        }
    }

    // An isolated hitch (2.5x the running average) is skipped: the count doesn't move on it.
    {
        XeFG_DynamicCount controller;
        XeFG_DynamicCount::Sample sample;
        sample.targetFps = 120;
        sample.frameMs = 40;
        sample.maxCount = 3;
        sample.minCount = 1;
        sample.currentCount = 1;
        int count = 1;
        for (int frame = 0; frame < 40; ++frame)
        {
            sample.nowMs = frame * 40.;
            count = controller.Update(sample);
            sample.currentCount = count;
        }
        sample.frameMs = 120; // 3x the average: a hitch
        sample.nowMs += 40.;
        const int hitched = controller.Update(sample);
        assert(hitched == count);
        sample.frameMs = 40;
        sample.nowMs += 40.;
        assert(controller.Update(sample) >= sample.minCount);
    }
    std::puts("XeFG dynamic count: isolated hitch skipped PASS");
    std::puts("XeFG dynamic count: in-flight limits and failed-up recovery PASS");
}
