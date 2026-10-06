#pragma once
#include <string>
#include <string_view>

namespace DlssNr
{
struct NrStatusSummary
{
    std::string state;
    std::string historyIssue;
};

inline std::string HistoryIssueText(std::string_view reason)
{
    static constexpr struct { std::string_view reason; const char* text; } issues[] = {
        {"diagnostic-view", "Diagnostic view is enabled"},
        {"unsupported-passes", "Only one NR pass is supported"},
        {"unsupported-post-layout", "The current model layout is unsupported"},
        {"unknown-motion-contract", "Motion vector metadata is unavailable"},
        {"missing-guides", "Motion vectors or depth are unavailable"},
        {"unsupported-guide-format", "Motion vector or depth format is unsupported"},
        {"guide-extent", "Motion vector or depth dimensions do not match"},
        {"invalid-motion-scalars", "Motion vector scale or jitter is invalid"},
        {"guide-device", "Motion vectors or depth belong to another device"},
        {"unsupported-lifecycle", "The current execution path is unsupported"},
    };
    for (const auto& issue : issues)
        if (reason == issue.reason) return issue.text;
    return std::string(reason);
}

// Display only: compact formats owned by our hosts/runtimes. Unknown messages
// (including bridge bypasses, failures and diagnostic NO NR modes) stay visible.
// Never infer activity from a successful recording or an old timing sample.
inline NrStatusSummary SummarizeNrStatus(std::string_view detail, bool running)
{
    const char* activity = running ? "NR active" : "NR waiting for frame submission";
    NrStatusSummary result {std::string(detail), {}};
    if (detail.starts_with("lmxxf:") || detail.starts_with("lmxxf diagnostic:"))
    {
        const auto split = detail.find(" | lmxxf arch=");
        if (split != std::string_view::npos)
        {
            result.state = detail.substr(0, split);
            const auto runtime = detail.substr(split + 3);
            const auto history = runtime.find(" history=");
            if (history != std::string_view::npos)
            {
                auto value = runtime.substr(history + 9);
                value = value.substr(0, value.find(' '));
                if (value != "off" && value != "ready" && value != "active" && value != "priming" &&
                    value != "reset-after-discard" && value != "reset-requested")
                    result.historyIssue = HistoryIssueText(value);
            }
        }
        if (result.state == "lmxxf: recording ready") result.state = activity;
        else if (result.state == "lmxxf: session ready" ||
                 result.state == "lmxxf: constructed (session not ready)")
            result.state = "NR waiting for input";
        return result;
    }

    // Mochizuki's normal status includes dimensions, timing and frame counters.
    // Its pass-capacity fallback has a different format and must remain visible.
    if (detail.starts_with("mochizuki ") && detail.size() > 10 &&
        detail[10] >= '0' && detail[10] <= '9' &&
        detail.find(", network ") != std::string_view::npos && detail.ends_with(" frames") &&
        detail.find(';') == std::string_view::npos && detail.find(": requested ") == std::string_view::npos)
    {
        result.state = activity;
        return result;
    }

    if (detail.starts_with("AMD runtime "))
    {
        const auto split = detail.find(" | ");
        if (split == std::string_view::npos) return result;
        detail.remove_prefix(split + 3);
    }
    // Only remove the host's known cumulative telemetry suffix. Retain any
    // effect failure between the current status and the counters.
    detail = detail.substr(0, detail.find(" | completed frames="));
    const auto split = detail.find(" | ");
    const auto state = detail.substr(0, split);
    if (state.starts_with("Completed AMD NR passes=") || state.starts_with("Recorded pre-SR ") ||
        state.starts_with("AMD Record ok: n=") || state.starts_with("Neural submission: lists="))
    {
        result.state = activity;
        if (split != std::string_view::npos) result.state += detail.substr(split);
    }
    else result.state = detail;
    return result;
}
}
