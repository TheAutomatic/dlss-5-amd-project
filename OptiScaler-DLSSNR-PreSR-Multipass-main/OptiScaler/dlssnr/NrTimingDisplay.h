#pragma once
#include "NrPerformance.h"
#include <cmath>
#include <cstdio>
#include <string>
namespace DlssNr
{
inline std::string TimingValueText(const NrTimingSnapshot& snapshot, unsigned stage, uint64_t now, bool detailed = false)
{
    if (snapshot.version != NR_TIMING_VERSION || snapshot.struct_size != sizeof snapshot) return "N/A";
    if (stage == NR_GPU_NETWORK && !NR_NETWORK_TIMING_AVAILABLE) return "N/A (paused)";
    if (!snapshot.enabled) return "disabled";
    if (stage >= NR_TIMING_STAGE_COUNT) return "N/A";
    const auto& v = snapshot.stages[stage];
    if (!v.samples) return "N/A (waiting)";
    if (now < v.last_tick_ms || now - v.last_tick_ms > 2000) return "N/A (stale)";
    if (!std::isfinite(v.mean_ms) || !std::isfinite(v.last_ms) || !std::isfinite(v.max_ms) ||
        v.mean_ms < 0 || v.last_ms < 0 || v.max_ms < 0) return "N/A";
    char text[200];
    if (detailed)
        std::snprintf(text, sizeof text, "avg %.3f / last %.3f / max %.3f ms | n=%llu | age=%llu ms",
            v.mean_ms, v.last_ms, v.max_ms, static_cast<unsigned long long>(v.samples),
            static_cast<unsigned long long>(now - v.last_tick_ms));
    else std::snprintf(text, sizeof text, "%.2f ms", v.mean_ms);
    return text;
}
}
