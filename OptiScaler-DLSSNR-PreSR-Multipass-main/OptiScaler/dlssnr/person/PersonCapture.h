#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace DlssNr::Person
{
inline uint64_t CaptureClock()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// Keep GPU readbacks in flight while the one CPU inference request runs.
// A single end-to-end request leaves the GPU capture idle for several frames
// and makes source age sawtooth across the mask's expiry window.
struct CaptureSchedule
{
    static constexpr size_t MaxPending = 4;
    static constexpr uint64_t IntervalMs = 50; // 20 Hz target, quantized to available render frames.
    bool started = false;
    uint64_t next = 0, submittedFrame = 0;
    bool Newer(uint64_t frame) const { return frame > submittedFrame; }
    void Submitted(uint64_t frame) { submittedFrame = frame; }
    bool Request(uint64_t now, size_t pending, bool available)
    {
        if (!available || pending >= MaxPending || (started && now < next)) return false;
        // Keep the scheduled phase instead of adding 50 ms to the last actual
        // capture. Otherwise a 40 ms render interval turns a 20 Hz target into
        // 12.5 Hz. Skip missed slots after stalls; never queue catch-up work.
        next = now + (started ? IntervalMs - (now - next) % IntervalMs : IntervalMs);
        started = true; return true;
    }
};
}
