#pragma once
#include <cstddef>
#include <cstdint>

namespace DlssNr::Person
{
// Keep GPU readbacks in flight while the one CPU inference request runs.
// A single end-to-end request leaves the GPU capture idle for several frames
// and makes source age sawtooth across the mask's expiry window.
struct CaptureSchedule
{
    static constexpr size_t MaxPending = 4;
    static constexpr uint64_t IntervalMs = 50; // At most 20 captures/s, independent of display FPS.
    bool started = false;
    uint64_t last = 0, submittedFrame = 0;
    bool Newer(uint64_t frame) const { return frame > submittedFrame; }
    void Submitted(uint64_t frame) { submittedFrame = frame; }
    bool Request(uint64_t now, size_t pending, bool available)
    {
        if (!available || pending >= MaxPending || (started && now - last < IntervalMs)) return false;
        started = true; last = now; return true;
    }
};
}
