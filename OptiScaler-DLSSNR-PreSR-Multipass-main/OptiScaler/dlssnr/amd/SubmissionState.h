#pragma once
#include <cstdint>
#include <limits>

namespace AmdPreSr
{
// CPU bookkeeping only. An elapsed timeout never cancels recorded GPU work.
struct SubmissionState
{
    bool recorded = false;
    bool submitted = false;
    bool stallReported = false;
    std::uint64_t recordedAt = 0;
    std::uint64_t submittedAt = 0;

    void Record(std::uint64_t now) { *this = {}; recorded = true; recordedAt = now; }
    void Submit(std::uint64_t now) { submitted = true; submittedAt = now; stallReported = false; }
    // A exposes one pending list/job per instance, even with multiple jobs in flight.
    bool BlocksRecord() const { return recorded && !submitted; }
    bool CanRetire(bool nativeDone, std::uint64_t completed, std::uint64_t target) const
    {
        // D3D12 reports UINT64_MAX on device removal, not successful completion.
        return recorded && submitted && target != 0 && nativeDone && completed != (std::numeric_limits<std::uint64_t>::max)() &&
               completed >= target;
    }
    bool ReportStall(std::uint64_t now)
    {
        const auto start = submitted ? submittedAt : recordedAt;
        if (!recorded || stallReported || now < start || now - start <= 5000)
            return false;
        stallReported = true;
        return true;
    }
    // A timeout does not cancel a command list. Keep it owned and match a late
    // submission; recovering a truly discarded list requires generation/cancel evidence.
};
}
