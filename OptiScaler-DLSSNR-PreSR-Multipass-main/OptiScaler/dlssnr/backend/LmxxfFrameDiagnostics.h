#pragma once
#include <cstdint>

namespace DlssNr::Backend
{
// Counts host Evaluate outcomes, not displayed frames or successful HIP execution.
// Owned by one backend, accessed only under recordMutex. No per-frame allocations.
struct FrameDiagnostics
{
    uint64_t calls = 0, recorded = 0, original = 0, switches = 0;
    uint64_t streak = 0, maxRecorded = 0, maxOriginal = 0;
    uint64_t lastReportMs = 0;
    bool lastRecorded = false;

    bool Observe(bool nr, uint64_t now)
    {
        const bool changed = calls && nr != lastRecorded;
        if (changed) ++switches;
        streak = calls && !changed ? streak + 1 : 1;
        ++calls;
        if (nr) { ++recorded; if (streak > maxRecorded) maxRecorded = streak; }
        else { ++original; if (streak > maxOriginal) maxOriginal = streak; }
        lastRecorded = nr;
        // First eight transitions expose short alternating sequences; then bound
        // logging to one aggregate report per five seconds, even at high FPS.
        if (calls == 1 || (changed && switches <= 8) || now - lastReportMs >= 5000)
        {
            lastReportMs = now;
            return true;
        }
        return false;
    }
};
}
