#pragma once
#include <cstdint>
#include <limits>

namespace AmdPreSr
{
// CPU bookkeeping only. An elapsed timeout never cancels recorded GPU work.
struct SubmissionState
{
    bool submitted = false;
    bool stallReported = false;
    std::uint64_t recordedAt = 0;
    std::uint64_t submittedAt = 0;

    void Record(std::uint64_t now) { *this = {}; recordedAt = now; }
    void Submit(std::uint64_t now) { submitted = true; submittedAt = now; stallReported = false; }
    bool CanRetire(bool nativeDone, std::uint64_t completed, std::uint64_t target) const
    {
        // D3D12 reports UINT64_MAX on device removal, not successful completion.
        return submitted && nativeDone && completed != (std::numeric_limits<std::uint64_t>::max)() &&
               completed >= target;
    }
    bool ReportStall(std::uint64_t now)
    {
        const auto start = submitted ? submittedAt : recordedAt;
        if (stallReported || now < start || now - start <= 5000)
            return false;
        stallReported = true;
        return true;
    }
    // Recorded but never submitted (game dropped the list, NR toggled off, …).
    // After 5s this slot would otherwise stay occupied forever and block Record.
    bool AbandonUnsubmitted(std::uint64_t now) const
    {
        if (submitted || recordedAt == 0 || now < recordedAt)
            return false;
        return now - recordedAt > 5000;
    }
};
}
