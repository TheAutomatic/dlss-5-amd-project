#pragma once
#include "lmxxf_runtime/LmxxfNrApi.h"
#include <string_view>

namespace DlssNr::Backend
{
enum class MochizukiPrepareOutcome { Ready, Building, Settling, VramWait, Failed };

inline MochizukiPrepareOutcome ClassifyMochizukiPrepare(int32_t result, std::string_view reason)
{
    if (result == LMXXF_NR_OK)
        return MochizukiPrepareOutcome::Ready;
    // Admission can refuse the network or its candidate frame buffers. The
    // latter wraps the admission message and uses the runtime's OOM build hold.
    // Do not permanently close either recovery path in host.
    // Other UNAVAILABLE errors (e.g. unsupported formats) remain real failures.
    constexpr std::string_view admission = "insufficient VRAM for ";
    const bool candidateAdmission = reason.starts_with("candidate frame buffers failed at ") &&
        reason.find(": insufficient VRAM for ") != std::string_view::npos;
    if (result == LMXXF_NR_UNAVAILABLE && (reason.starts_with(admission) || candidateAdmission))
        return MochizukiPrepareOutcome::VramWait;
    if (reason.find("building the network") != std::string_view::npos)
        return MochizukiPrepareOutcome::Building;
    if (reason.find("input resolution is settling") != std::string_view::npos)
        return MochizukiPrepareOutcome::Settling;
    return MochizukiPrepareOutcome::Failed;
}

class MochizukiPrepareFailureGate
{
    unsigned failures_ = 0;
  public:
    static constexpr unsigned kLimit = 50;
    bool Open() const { return failures_ >= kLimit; }
    unsigned Failures() const { return failures_; }
    void Reset() { failures_ = 0; }
    void Observe(MochizukiPrepareOutcome outcome)
    {
        if (outcome != MochizukiPrepareOutcome::Failed)
            Reset();
        else if (!Open())
            ++failures_;
    }
};
} // namespace DlssNr::Backend
