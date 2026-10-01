#pragma once
#include "NrPerformance.h"
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <algorithm>

namespace DlssNr
{
class PerformanceStore
{
    static constexpr size_t kWindow = 120;
    struct Stage
    {
        NrTimingValue value {};
        std::array<double, kWindow> history {};
        size_t cursor = 0, size = 0;
    };
    mutable std::mutex lock;
    std::atomic<bool> enabled {false};
    std::atomic<uint64_t> epoch {1};
    std::array<Stage, NR_TIMING_STAGE_COUNT> stages {};
    uint64_t dropped = 0;
public:
    bool Enabled() const { return enabled.load(std::memory_order_relaxed); }
    uint64_t Epoch() const { return epoch.load(std::memory_order_relaxed); }
    void SetEnabled(bool value)
    {
        std::lock_guard guard(lock);
        if (enabled.load() == value) return;
        stages = {};
        dropped = 0;
        ++epoch;
        enabled.store(value);
    }
    void Record(unsigned stage, double ms, uint64_t tick, uint64_t frame = 0, uint64_t execution = 0, uint64_t sampleEpoch = 0)
    {
        if (!Enabled()) return;
        std::lock_guard guard(lock);
        if (!enabled.load()) return;
        if (sampleEpoch && sampleEpoch != epoch.load()) return;
        if (stage >= stages.size() || !std::isfinite(ms) || ms < 0) { ++dropped; return; }
        auto& s = stages[stage];
        s.history[s.cursor] = ms;
        s.cursor = (s.cursor + 1) % kWindow;
        s.size = (std::min)(s.size + 1, kWindow);
        ++s.value.samples;
        s.value.last_ms = ms;
        s.value.last_tick_ms = tick;
        s.value.frame_id = frame;
        s.value.execution_id = execution;
    }
    void Drop(uint64_t count)
    {
        if (!count || !Enabled()) return;
        std::lock_guard guard(lock);
        if (enabled.load()) dropped += count;
    }
    NrTimingSnapshot Read() const
    {
        std::lock_guard guard(lock);
        NrTimingSnapshot out {};
        out.struct_size = sizeof out;
        out.version = NR_TIMING_VERSION;
        out.enabled = enabled.load() ? 1u : 0u;
        out.dropped = dropped;
        for (size_t i = 0; i < stages.size(); ++i)
        {
            const auto& s = stages[i];
            out.stages[i] = s.value;
            double sum = 0, peak = 0;
            for (size_t j = 0; j < s.size; ++j) { sum += s.history[j]; peak = (std::max)(peak, s.history[j]); }
            out.stages[i].mean_ms = s.size ? sum / s.size : 0;
            out.stages[i].max_ms = peak;
        }
        return out;
    }
};
}
