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
    // Admission refuses before allocating, and the runtime's Hold retries this
    // key every 10 seconds. Do not permanently close that recovery path in host.
    // Other UNAVAILABLE errors (e.g. unsupported formats) remain real failures.
    if (result == LMXXF_NR_UNAVAILABLE && reason.starts_with("insufficient VRAM for "))
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
