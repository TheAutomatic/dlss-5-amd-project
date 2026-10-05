#pragma once
// Diagnostic build only. Records host decisions, including frames which never
// reach the runtime capture. No runtime calls or file writes on each Evaluate.
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "LmxxfJobLifecycle.h"

namespace DlssNr::Backend::NrHostTrace
{
inline const char *PhaseName(JobLifecycle::Phase phase)
{
    switch (phase)
    {
    case JobLifecycle::Phase::Idle: return "idle";
    case JobLifecycle::Phase::Recording: return "recording";
    case JobLifecycle::Phase::Armed: return "armed";
    case JobLifecycle::Phase::Submitting: return "submitting";
    case JobLifecycle::Phase::Enqueueing: return "enqueueing";
    case JobLifecycle::Phase::EnqueueCompleted: return "enqueue_completed";
    case JobLifecycle::Phase::Completing: return "completing";
    case JobLifecycle::Phase::Cancelling: return "cancelling";
    case JobLifecycle::Phase::Closing: return "closing";
    case JobLifecycle::Phase::Closed: return "closed";
    }
    return "unknown";
}

struct Entry
{
    uint64_t evaluate = 0, begin = 0, end = 0, token = 0;
    uint64_t enqueueCalls = 0, recovered = 0, submitFailures = 0;
    JobLifecycle::Phase phase = JobLifecycle::Phase::Idle;
    int32_t enqueueRc = 0;
    bool recorded = false, stopping = false, history = false, reset = false, inputs = false;
    DWORD thread = 0;
    std::array<char, 384> reason {};
};

class Capture
{
    static constexpr uint64_t kDuration = 22000;
    static constexpr size_t kMaxRows = 32768;
    std::mutex mutex_;
    bool previousKey_ = false, running_ = false;
    uint64_t start_ = 0;
    std::vector<Entry> rows_;
    std::thread writer_;
    std::shared_ptr<std::atomic<bool>> writerDone_ = std::make_shared<std::atomic<bool>>(true);

    void Save(uint64_t end, bool complete)
    {
        if (!running_) return;
        running_ = false;
        // A new capture cannot begin until the previous writer has completed.
        if (writer_.joinable()) writer_.join();
        const auto start = start_;
        const auto pid = GetCurrentProcessId();
        auto done = writerDone_;
        done->store(false);
        writer_ = std::thread([rows = std::move(rows_), start, end, complete, pid, done]() {
            wchar_t temp[MAX_PATH] {};
            const DWORD n = GetTempPathW(MAX_PATH, temp);
            if (!n || n >= MAX_PATH) { done->store(true); return; }
            const auto directory = std::wstring(temp) + L"Lmxxf-NR-test19";
            CreateDirectoryW(directory.c_str(), nullptr);
            const auto path = directory + L"/host-" + std::to_wstring(pid) + L"-" + std::to_wstring(start) + L".csv";
            FILE *file = _wfopen(path.c_str(), L"wb");
            if (file)
            {
                std::fprintf(file, "evaluate,begin_tick,end_tick,recorded,phase,token,stopping,history_requested,game_reset,inputs_valid,enqueue_calls,last_enqueue_rc,recovered,submit_failures,thread,reason\n");
                for (const auto &row : rows)
                {
                    // Status text is diagnostic data; quote and escape CSV cells.
                    std::fprintf(file, "%llu,%llu,%llu,%u,%s,%llu,%u,%u,%u,%u,%llu,%d,%llu,%llu,%lu,\"",
                        static_cast<unsigned long long>(row.evaluate), static_cast<unsigned long long>(row.begin),
                        static_cast<unsigned long long>(row.end), unsigned(row.recorded), PhaseName(row.phase),
                        static_cast<unsigned long long>(row.token), unsigned(row.stopping), unsigned(row.history),
                        unsigned(row.reset), unsigned(row.inputs), static_cast<unsigned long long>(row.enqueueCalls),
                        row.enqueueRc, static_cast<unsigned long long>(row.recovered),
                        static_cast<unsigned long long>(row.submitFailures), row.thread);
                    for (const char c : row.reason)
                    {
                        if (!c) break;
                        if (c == '\"') std::fputc('\"', file);
                        std::fputc(c == '\r' || c == '\n' ? ' ' : c, file);
                    }
                    std::fputs("\"\n", file);
                }
                const bool written = !std::ferror(file);
                const bool closed = std::fclose(file) == 0;
                file = _wfopen((path + L".info.txt").c_str(), L"wb");
                if (file)
                {
                    std::fprintf(file, "schema=host-evaluate-v1\nstart_tick=%llu\nend_tick=%llu\nrows=%zu\ncomplete=%u\n"
                        "recorded_means=NR_commands_recorded_not_presented\nphase_is=end_of_Evaluate_snapshot\n",
                        static_cast<unsigned long long>(start), static_cast<unsigned long long>(end), rows.size(),
                        unsigned(complete && written && closed));
                    std::fclose(file);
                }
            }
            done->store(true);
        });
    }

  public:
    ~Capture()
    {
        if (running_) Save(GetTickCount64(), false);
        if (writer_.joinable()) writer_.join();
    }

    void Observe(const Entry &entry, bool keyDown)
    {
        std::lock_guard lock(mutex_);
        const bool pressed = keyDown && !previousKey_;
        previousKey_ = keyDown;
        if (running_ && entry.end - start_ >= kDuration) Save(entry.end, true);
        if (pressed && !running_ && writerDone_->load())
        {
            if (writer_.joinable()) writer_.join();
            rows_.clear();
            rows_.reserve(kMaxRows);
            start_ = entry.begin;
            running_ = true;
        }
        if (!running_) return;
        rows_.push_back(entry);
        if (rows_.size() >= kMaxRows) Save(entry.end, false);
    }
};

inline void Observe(uint64_t evaluate, uint64_t beginTick, bool recorded, const char *reason,
                    const JobLifecycle::Observation &state, bool historyRequested, bool gameReset,
                    bool inputsValid, uint64_t enqueueCalls, int32_t lastEnqueueRc,
                    uint64_t recovered, uint64_t submitFailures)
{
    static Capture capture;
    Entry row;
    row.evaluate = evaluate; row.begin = beginTick; row.end = GetTickCount64();
    row.recorded = recorded; row.phase = state.phase;
    row.token = state.work ? state.work->token : 0; row.stopping = state.stopping;
    row.history = historyRequested; row.reset = gameReset; row.inputs = inputsValid;
    row.enqueueCalls = enqueueCalls; row.enqueueRc = lastEnqueueRc;
    row.recovered = recovered; row.submitFailures = submitFailures; row.thread = GetCurrentThreadId();
    std::snprintf(row.reason.data(), row.reason.size(), "%s", reason ? reason : "");
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    const bool down = foregroundPid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    capture.Observe(row, down);
}
} // namespace DlssNr::Backend::NrHostTrace
