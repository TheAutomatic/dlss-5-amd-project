#pragma once
#include <algorithm>
#include <cstdint>

namespace FSRD
{
enum class RRResult : uint8_t
{
    Success,
    RetryableInputFailure,
    NeedsRecreation,
    UnsupportedProvider,
    DeviceLost
};

// Input failures retain the existing context. Probe again on the next frame,
// then back off to at most one attempt per 61 evaluations if the inputs stay bad.
// Context/provider/device failures require an explicit successful recreation.
class RRRetryPolicy
{
  public:
    static constexpr uint32_t MaxSkippedFrames = 60;

    RRResult Result() const noexcept { return _result; }
    uint32_t SkippedFramesRemaining() const noexcept { return _skippedFrames; }
    bool IsLatched() const noexcept
    {
        return _result != RRResult::Success && _result != RRResult::RetryableInputFailure;
    }

    bool CanAttempt() noexcept
    {
        if (IsLatched())
            return false;
        if (_skippedFrames != 0)
        {
            --_skippedFrames;
            return false;
        }
        return true;
    }

    void Record(RRResult result) noexcept
    {
        // A later bad input (or a successful SR fallback) must not clear a latch.
        if (IsLatched() && result != RRResult::DeviceLost)
            return;
        _result = result;
        if (result == RRResult::RetryableInputFailure)
        {
            _inputFailures = (std::min)(_inputFailures + 1u, 7u);
            _skippedFrames = (std::min)((1u << (_inputFailures - 1u)) - 1u, MaxSkippedFrames);
        }
        else
        {
            _inputFailures = 0;
            _skippedFrames = 0;
        }
    }

    void Reset() noexcept
    {
        _result = RRResult::Success;
        _inputFailures = 0;
        _skippedFrames = 0;
    }

  private:
    RRResult _result = RRResult::Success;
    uint32_t _inputFailures = 0;
    uint32_t _skippedFrames = 0;
};
} // namespace FSRD
