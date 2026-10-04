#pragma once
#include <algorithm>
#include <mutex>
#include <optional>

namespace FrameGeneration
{
enum class InterpolationApplyResult { Success, Rejected, RequiresReinitialization };

struct InterpolationRequest
{
    int baseline = 1;
    int nrOverride = 0;
    int maximum = 1;
    bool nrRunning = false;
    bool liveSupported = false;
    bool enabled = false;
    bool operator==(const InterpolationRequest&) const = default;
};

struct InterpolationStatus
{
    InterpolationRequest request;
    int requested = 1;
    int target = 1;
    int applied = -1;
    bool overridden = false;
    bool limited = false;
    bool failed = false;
    bool requiresReinitialization = false;
};

inline InterpolationStatus ResolveInterpolation(const InterpolationRequest& request)
{
    InterpolationStatus status;
    status.request = request;
    status.overridden = request.enabled && request.liveSupported && request.nrRunning && request.nrOverride > 0;
    status.requested = status.overridden ? request.nrOverride : request.baseline;
    status.target = std::clamp(status.requested, 1, (std::max)(1, request.maximum));
    status.limited = status.requested != status.target;
    return status;
}

class InterpolationOverride
{
    mutable std::mutex mutex;
    InterpolationStatus status;
    std::optional<InterpolationRequest> attempted;

  public:
    void Reset(int applied = -1)
    {
        std::lock_guard guard(mutex);
        status = {};
        status.applied = applied;
        attempted.reset();
    }

    template <typename Apply> bool Update(const InterpolationRequest& request, Apply&& apply)
    {
        std::lock_guard guard(mutex);
        const bool changed = !attempted || *attempted != request;
        auto next = ResolveInterpolation(request);
        next.applied = status.applied;
        next.failed = status.requiresReinitialization || (!changed && status.failed);
        next.requiresReinitialization = status.requiresReinitialization;
        status = next;
        if (!changed)
            return false;
        attempted = request;
        if (status.requiresReinitialization || !request.enabled || !request.liveSupported ||
            status.target == status.applied)
            return false;
        const auto result = apply(status.target);
        if (result != InterpolationApplyResult::Success)
        {
            status.failed = true;
            status.requiresReinitialization = result == InterpolationApplyResult::RequiresReinitialization;
            return false;
        }
        status.applied = status.target;
        return true;
    }

    InterpolationStatus Snapshot() const
    {
        std::lock_guard guard(mutex);
        return status;
    }
};
}
