#pragma once
#include "../dlssnr/NrGpuHistory.h"
#include "MenuUi.h"
#include <cfloat>
#include <cstdio>

namespace MenuNrGpuHistory
{
inline const char* MetricLabel(DlssNr::GpuHistoryMetric metric)
{
    switch (metric)
    {
    case DlssNr::GpuHistoryMetric::Network: return "NR network GPU";
    case DlssNr::GpuHistoryMetric::MochizukiVulkanCore: return "NR Mochizuki Vulkan core GPU";
    default: return "NR GPU";
    }
}

inline void Draw(const DlssNr::GpuHistory& history)
{
    MenuUi::Text(history.HasMeasurement() ? MetricLabel(history.Metric()) : "NR GPU");
    char value[48] = "N/A";
    if (history.HasMeasurement()) std::snprintf(value, sizeof value, "%.2f ms", history.Last());
    if (history.HasMeasurement())
        ImGui::PlotLines("##nr_gpu_history", [](void* data, int index) {
            return static_cast<const DlssNr::GpuHistory*>(data)->At(static_cast<size_t>(index));
        }, const_cast<DlssNr::GpuHistory*>(&history), static_cast<int>(history.Size()), 0, value,
            0.0f, FLT_MAX, {ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight()});
    else
        MenuUi::TextDisabled("N/A");
    MenuUi::TextWrapped("Sampled completed-execution history; not every frame");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::BeginTooltip();
        // Fixed wrap width (MenuUi::SetTooltip convention): a fresh tooltip window's
        // content width is ~0, so TextWrapped alone folds the text into a one-character
        // vertical strip (field-reported tall-narrow tooltip).
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
        MenuUi::TextWrapped("GPU timestamps only. Cached snapshots may skip executions; repeated UI polls add no points. Missing or stale measurements show N/A, never CPU wall time.");
        if (history.Metric() == DlssNr::GpuHistoryMetric::MochizukiVulkanCore)
            MenuUi::TextWrapped("Vulkan core total, including core input/composition; excludes bridge copies and shared effects.");
        else if (history.Metric() == DlssNr::GpuHistoryMetric::Network)
            MenuUi::TextWrapped("Network execution only; not whole NR or frame latency.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
}
