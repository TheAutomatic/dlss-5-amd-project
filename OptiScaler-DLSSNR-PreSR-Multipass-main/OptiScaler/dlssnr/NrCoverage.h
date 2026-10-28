#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace DlssNr::Coverage
{
// Calls, recordings and executions are different units; none is a Present count.
enum class Outcome : unsigned {
    Recorded, ProxyMissing, Rejected, InputRejected, SessionUnavailable,
    BuildWait, PrepareFailed, Busy, RecordFailed, HeldOff, InvalidInput,
    Diagnostic, BridgeBypass, BackendBypass, Count
};
inline constexpr unsigned kOutcomes = unsigned(Outcome::Count);
inline constexpr const char* kNames[kOutcomes] = {
    "recorded", "proxy_missing", "rejected", "input_rejected", "session_unavailable",
    "build_wait", "prepare_failed", "busy", "record_failed", "held_off", "invalid_input",
    "diagnostic", "bridge_bypass", "backend_bypass"
};
inline constexpr unsigned kReasons = 8;
inline constexpr const char* kReasonNames[kReasons] = {
    "predication", "enhanced_barrier", "query", "render_pass", "native_interface",
    "resource_state", "already_split", "other"
};
inline unsigned RejectReason(const char* text) noexcept {
    if (!text) return kReasons - 1;
    if (std::strstr(text, "predication")) return 0;
    if (std::strstr(text, "enhanced")) return 1;
    if (std::strstr(text, "query")) return 2;
    if (std::strstr(text, "render_pass")) return 3;
    if (std::strstr(text, "native_interface")) return 4;
    if (std::strstr(text, "state") || std::strstr(text, "layout") || std::strstr(text, "barrier")) return 5;
    if (std::strstr(text, "already_split")) return 6;
    return kReasons - 1;
}
struct Snapshot {
    std::array<std::array<uint64_t, kOutcomes>, 2> calls {};
    std::array<std::array<uint64_t, kReasons>, 2> rejections {};
    uint64_t executions = 0, executionFailed = 0, splitAccepted = 0, enqueues = 0, enqueueFailed = 0;
    uint64_t directAccepted = 0;
    uint64_t inlineApplied = 0, inlineMissed = 0;
};
struct Counters {
    std::atomic<bool> prepared {false};
    std::atomic<uint64_t> offered {0};
    std::array<std::array<std::atomic<uint64_t>, kOutcomes>, 2> calls {};
    std::array<std::array<std::atomic<uint64_t>, kReasons>, 2> rejections {};
    std::atomic<uint64_t> executions {0}, executionFailed {0}, splitAccepted {0}, enqueues {0}, enqueueFailed {0};
    std::atomic<uint64_t> directAccepted {0};
    std::atomic<uint64_t> inlineApplied {0}, inlineMissed {0};
    uint64_t Begin() noexcept { return offered.fetch_add(1, std::memory_order_relaxed) + 1; }
    void Finish(bool postReady, Outcome outcome, const char* reason = nullptr) noexcept {
        calls[postReady ? 1 : 0][unsigned(outcome)].fetch_add(1, std::memory_order_relaxed);
        if (outcome == Outcome::Rejected || outcome == Outcome::InputRejected || outcome == Outcome::BridgeBypass ||
            outcome == Outcome::PrepareFailed)
            rejections[postReady ? 1 : 0][RejectReason(reason)].fetch_add(1, std::memory_order_relaxed);
    }
    Snapshot Read() const noexcept {
        Snapshot s;
        for (unsigned p = 0; p < 2; ++p)
            for (unsigned i = 0; i < kOutcomes; ++i)
                s.calls[p][i] = calls[p][i].load(std::memory_order_relaxed);
        for (unsigned p = 0; p < 2; ++p)
            for (unsigned i = 0; i < kReasons; ++i)
                s.rejections[p][i] = rejections[p][i].load(std::memory_order_relaxed);
        s.executions = executions.load(std::memory_order_relaxed);
        s.executionFailed = executionFailed.load(std::memory_order_relaxed);
        s.splitAccepted = splitAccepted.load(std::memory_order_relaxed);
        s.directAccepted = directAccepted.load(std::memory_order_relaxed);
        s.enqueues = enqueues.load(std::memory_order_relaxed);
        s.enqueueFailed = enqueueFailed.load(std::memory_order_relaxed);
        s.inlineApplied = inlineApplied.load(std::memory_order_relaxed);
        s.inlineMissed = inlineMissed.load(std::memory_order_relaxed);
        return s;
    }
};
inline Snapshot Delta(const Snapshot& now, const Snapshot& before) noexcept {
    Snapshot d;
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned i = 0; i < kOutcomes; ++i)
            d.calls[p][i] = now.calls[p][i] - before.calls[p][i];
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned i = 0; i < kReasons; ++i)
            d.rejections[p][i] = now.rejections[p][i] - before.rejections[p][i];
    d.executions = now.executions - before.executions;
    d.executionFailed = now.executionFailed - before.executionFailed;
    d.splitAccepted = now.splitAccepted - before.splitAccepted;
    d.directAccepted = now.directAccepted - before.directAccepted;
    d.enqueues = now.enqueues - before.enqueues;
    d.enqueueFailed = now.enqueueFailed - before.enqueueFailed;
    d.inlineApplied = now.inlineApplied - before.inlineApplied;
    d.inlineMissed = now.inlineMissed - before.inlineMissed;
    return d;
}
}
