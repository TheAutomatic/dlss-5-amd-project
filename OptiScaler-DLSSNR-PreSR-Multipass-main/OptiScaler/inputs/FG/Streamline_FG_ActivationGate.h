#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

class StreamlineFGActivationGate
{
  public:
    enum class Action
    {
        None,
        Activate,
        Deactivate,
    };

    struct Diagnostics
    {
        bool requested = false;
        bool armed = false;
        bool active = false;
        bool hasPresentBoundary = false;
        bool hasConstants = false;
        bool hasInterpolationSize = false;
        bool reset = false;
        bool lastEvaluateHadDepth = false;
        bool lastEvaluateHadMotionVectors = false;
        uint32_t readyFrameCount = 0;
        uint32_t lastEvaluatedFrameId = UINT32_MAX;
    };

  private:
    enum class State
    {
        Disabled,
        Armed,
        Active,
    };

    struct FrameReadiness
    {
        uint32_t frameId = UINT32_MAX;
        bool hasDepth = false;
        bool hasMotionVectors = false;
    };

    std::mutex _mutex;
    // Callback metadata only; this does not own or index the renderer's resource buffers.
    std::array<FrameReadiness, 4> _frames {};
    std::atomic<bool> _enabled;
    std::atomic<bool> _requested { false };
    State _state = State::Disabled;
    bool _hasPresentBoundary = false;

    // Streamline may reuse these across frame tokens after returning from UI scenes.
    bool _hasConstants = false;
    bool _hasInterpolationSize = false;
    bool _reset = false;
    uint32_t _readyFrameCount = 0;
    uint32_t _lastReadyFrameId = UINT32_MAX;
    uint32_t _lastEvaluatedFrameId = UINT32_MAX;
    bool _lastEvaluateHadDepth = false;
    bool _lastEvaluateHadMotionVectors = false;

    FrameReadiness& FrameData(uint32_t frameId)
    {
        auto& frame = _frames[frameId % _frames.size()];
        if (frame.frameId != frameId)
        {
            frame = {};
            frame.frameId = frameId;
        }
        return frame;
    }

    void ResetProgress()
    {
        _readyFrameCount = 0;
        _lastReadyFrameId = UINT32_MAX;
        _frames = {};
    }

  public:
    explicit StreamlineFGActivationGate(bool enabled = true) : _enabled(enabled) {}

    void SetEnabled(bool enabled)
    {
        // The default-off input path does not take a gate mutex every frame.
        if (_enabled.load(std::memory_order_acquire) == enabled)
            return;

        std::scoped_lock lock(_mutex);
        if (_enabled.load(std::memory_order_relaxed) == enabled)
            return;

        _enabled.store(enabled, std::memory_order_release);
        _state = enabled && _requested.load(std::memory_order_relaxed) ? State::Armed : State::Disabled;
        _hasPresentBoundary = false;
        ResetProgress();
    }

    void SetRequested(bool requested)
    {
        // Some games repeat the same options/constants every frame, including while the gate is off.
        if (_requested.load(std::memory_order_acquire) == requested)
            return;

        std::scoped_lock lock(_mutex);
        if (_requested.load(std::memory_order_relaxed) == requested)
            return;

        _requested.store(requested, std::memory_order_release);
        _state = requested && _enabled.load(std::memory_order_relaxed) ? State::Armed : State::Disabled;
        _hasPresentBoundary = false;
        ResetProgress();
    }

    void ReportConstants(uint32_t, bool reset)
    {
        std::scoped_lock lock(_mutex);
        _hasConstants = true;
        _reset = reset;
        if (reset)
            ResetProgress();
    }

    void ReportDepth(uint32_t frameId, uint64_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return;
        std::scoped_lock lock(_mutex);
        FrameData(frameId).hasDepth = true;
    }

    void ReportMotionVectors(uint32_t frameId, uint64_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return;
        std::scoped_lock lock(_mutex);
        FrameData(frameId).hasMotionVectors = true;
    }

    void ReportInterpolationSize(uint64_t width, uint32_t height)
    {
        std::scoped_lock lock(_mutex);
        _hasInterpolationSize = width > 0 && height > 0;
    }

    void ReportEvaluate(uint32_t frameId)
    {
        std::scoped_lock lock(_mutex);
        _lastEvaluatedFrameId = frameId;

        if (!_enabled.load(std::memory_order_relaxed) || _state != State::Armed ||
            !_requested.load(std::memory_order_relaxed) || !_hasPresentBoundary)
        {
            _lastEvaluateHadDepth = false;
            _lastEvaluateHadMotionVectors = false;
            ResetProgress();
            return;
        }

        // Evaluate and Present may both complete the same frame.
        if (_readyFrameCount > 0 && frameId == _lastReadyFrameId)
            return;

        auto& frame = FrameData(frameId);
        _lastEvaluateHadDepth = frame.hasDepth;
        _lastEvaluateHadMotionVectors = frame.hasMotionVectors;
        if (!_hasConstants || !frame.hasDepth || !frame.hasMotionVectors || !_hasInterpolationSize || _reset)
        {
            ResetProgress();
            return;
        }

        if (_readyFrameCount > 0 && frameId != _lastReadyFrameId + 1)
            _readyFrameCount = 0;

        _lastReadyFrameId = frameId;
        if (_readyFrameCount < UINT32_MAX)
            ++_readyFrameCount;
    }

    void MarkPresentBoundary()
    {
        std::scoped_lock lock(_mutex);
        _hasPresentBoundary = true;
    }

    void ReportPresent(uint32_t frameId)
    {
        // FG-only games may never evaluate an upscaler or send RenderSubmitEnd.
        ReportEvaluate(frameId);
        MarkPresentBoundary();
    }

    Action Evaluate(bool featureEnabled, bool activationBlocked, bool fgActive, bool fgPaused,
                    uint32_t requiredReadyFrames)
    {
        std::scoped_lock lock(_mutex);
        if (!_enabled.load(std::memory_order_relaxed))
            return Action::None;

        if (!featureEnabled || !_requested.load(std::memory_order_relaxed))
        {
            _state = State::Disabled;
            _hasPresentBoundary = false;
            ResetProgress();
            return fgActive ? Action::Deactivate : Action::None;
        }

        // A host FG off/on transition need not change the game's last request.
        if (_state == State::Disabled)
        {
            _state = State::Armed;
            _hasPresentBoundary = false;
            ResetProgress();
        }

        if (activationBlocked || fgPaused)
        {
            if (!fgActive)
                _state = State::Armed;
            ResetProgress();
            return Action::None;
        }

        if (fgActive)
        {
            _state = State::Active;
            return Action::None;
        }

        if (_state == State::Active)
        {
            _state = State::Armed;
            _hasPresentBoundary = false;
            ResetProgress();
        }

        if (_state == State::Armed && _readyFrameCount >= requiredReadyFrames)
            return Action::Activate;
        return Action::None;
    }

    bool ConfirmActivation(bool featureEnabled, bool activationBlocked, bool fgPaused, uint32_t requiredReadyFrames)
    {
        std::scoped_lock lock(_mutex);
        if (!_enabled.load(std::memory_order_relaxed) || !featureEnabled ||
            !_requested.load(std::memory_order_relaxed) || activationBlocked || fgPaused || _state != State::Armed ||
            _readyFrameCount < requiredReadyFrames)
            return false;

        _state = State::Active;
        return true;
    }

    void ReportActivationResult(bool active)
    {
        std::scoped_lock lock(_mutex);
        if (!_enabled.load(std::memory_order_relaxed) || !_requested.load(std::memory_order_relaxed))
        {
            _state = State::Disabled;
            ResetProgress();
            return;
        }
        if (active)
        {
            _state = State::Active;
            return;
        }
        _state = State::Armed;
        _hasPresentBoundary = false;
        ResetProgress();
    }

    uint32_t ReadyFrameCount()
    {
        std::scoped_lock lock(_mutex);
        return _readyFrameCount;
    }

    Diagnostics GetDiagnostics()
    {
        std::scoped_lock lock(_mutex);
        return {
            .requested = _requested.load(std::memory_order_relaxed),
            .armed = _state == State::Armed,
            .active = _state == State::Active,
            .hasPresentBoundary = _hasPresentBoundary,
            .hasConstants = _hasConstants,
            .hasInterpolationSize = _hasInterpolationSize,
            .reset = _reset,
            .lastEvaluateHadDepth = _lastEvaluateHadDepth,
            .lastEvaluateHadMotionVectors = _lastEvaluateHadMotionVectors,
            .readyFrameCount = _readyFrameCount,
            .lastEvaluatedFrameId = _lastEvaluatedFrameId,
        };
    }
};
