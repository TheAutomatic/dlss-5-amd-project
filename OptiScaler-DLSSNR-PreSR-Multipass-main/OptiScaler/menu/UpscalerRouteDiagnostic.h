#pragma once
#include <cstdint>

// Startup evidence only: absence of a feature is not proof that DLSS is unsupported.
// Called from the same presentation path as MenuCommon's frame counters.
class UpscalerRouteDiagnostic
{
public:
    enum class Event { None, MissingRoute, RouteObserved };

    Event Observe(uint64_t nowMs, bool nrEnabled, bool hasFeature)
    {
        if (seenRoute_)
            return Event::None;
        if (hasFeature)
        {
            seenRoute_ = true;
            return warned_ ? Event::RouteObserved : Event::None;
        }
        if (warned_)
            return Event::None;
        if (!nrEnabled)
        {
            started_ = false;
            activeMs_ = frames_ = 0;
            return Event::None;
        }
        if (started_ && nowMs >= lastMs_ && nowMs - lastMs_ <= 1000)
            activeMs_ += nowMs - lastMs_; // Exclude long loading/minimized gaps.
        started_ = true;
        lastMs_ = nowMs;
        ++frames_;
        if (activeMs_ < 30000 || frames_ < 120)
            return Event::None;
        warned_ = true;
        return Event::MissingRoute;
    }

private:
    uint64_t lastMs_ = 0, activeMs_ = 0, frames_ = 0;
    bool started_ = false, seenRoute_ = false, warned_ = false;
};
