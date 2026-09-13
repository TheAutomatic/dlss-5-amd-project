#pragma once

// Only the isolated Section 4 diagnostic build includes this recorder.
// All calls are made under Backend::Impl::lock. No GPU work or waits are added.
#include <Windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string_view>

namespace AmdPreSr
{
class RetirementDiagnostics
{
public:
    struct Event
    {
        const char* source = "Unknown";
        const char* outcome = "poll";
        int64_t qpc = 0;
        uint64_t attempt = 0, pending = 0, recordedAt = 0, submittedAt = 0;
        uint64_t gpuBefore = 0, gpuAfter = 0, target = 0;
        uint64_t extraGpuBefore = 0, extraGpuAfter = 0, extraTarget = 0;
        std::array<unsigned, 3> done {}, jobs {};
        unsigned passes = 0, accepted = 0, width = 0, height = 0;
        bool submitted = false, nativeDone = true, retired = false, everyFrame = false, extraWaited = false;
        double waitMs = 0, extraWaitMs = 0;
        // Every-frame wait-loop accounting (source "EfWaitLoop").
        unsigned waitIterations = 0;
        bool waitedBeforeDone = false;
    };

    static int64_t Clock()
    {
        LARGE_INTEGER value;
        QueryPerformanceCounter(&value);
        return value.QuadPart;
    }
    double Milliseconds(int64_t ticks) const { return 1000.0 * ticks / frequency; }

    // Start after the first accepted native record; an NR-off menu cannot consume
    // the capture. One bounded window per Backend/process, no per-frame I/O.
    void BeginRecord(bool hasRecordedNativeWork)
    {
        ++attempt;
        if (!start && !finished && hasRecordedNativeWork)
        {
            start = Clock();
            capture = GetTickCount64();
        }
    }

    struct Scope
    {
        RetirementDiagnostics& owner;
        const std::filesystem::path& directory;
        const char* runtime;
        Event local;
        Event& event;
        bool emit;
        Scope(RetirementDiagnostics& d, const std::filesystem::path& path, const char* version,
              const char* source, Event* external = nullptr)
            : owner(d), directory(path), runtime(version), event(external ? *external : local), emit(!external)
        {
            event.source = source;
            event.attempt = d.attempt;
            if (!event.qpc) event.qpc = Clock();
        }
        ~Scope() noexcept
        {
            if (emit) owner.Add(event, directory, runtime);
        }
    };

    RetirementDiagnostics()
    {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        frequency = value.QuadPart;
    }

    void Flush(const std::filesystem::path& directory, const char* runtime, const char* reason) noexcept
    {
        if (!start || finished) return;
        finished = true;
        const auto elapsed = Milliseconds(Clock() - start);
        try
        {
            std::ofstream out(directory / L"amd_presr.log", std::ios::app);
            out << std::fixed << std::setprecision(3);
            out << "[AMD-S4-BEGIN] capture=" << capture << " runtime=" << runtime
                << " window_ms=30000 schema=1\n";
            for (size_t i = 0; i < count; ++i)
            {
                const auto& e = events[i];
                out << "[AMD-S4] capture=" << capture << " t_ms=" << Milliseconds(e.qpc - start)
                    << " source=" << e.source << " attempt=" << e.attempt
                    << " pending=" << e.pending << " submitted=" << e.submitted << " passes=" << e.passes
                    << " native=" << e.nativeDone;
                for (size_t pass = 0; pass < e.done.size(); ++pass)
                    out << " done" << pass << '=' << e.done[pass] << " job" << pass << '=' << e.jobs[pass];
                out << " gpu_before=" << e.gpuBefore << " gpu_after=" << e.gpuAfter << " target=" << e.target
                    << " retired=" << e.retired << " wait_ms=" << e.waitMs
                    << " extra_wait_ms=" << e.extraWaitMs << " extra_gpu_before=" << e.extraGpuBefore
                    << " extra_gpu_after=" << e.extraGpuAfter << " extra_target=" << e.extraTarget
                    << " extra_waited=" << e.extraWaited << " outcome=" << e.outcome << " accepted=" << e.accepted
                    << " width=" << e.width << " height=" << e.height << " every_frame=" << e.everyFrame
                    << " recorded_at=" << e.recordedAt << " submitted_at=" << e.submittedAt
                    << " wait_iters=" << e.waitIterations << " waited_before_done=" << e.waitedBeforeDone
                    << '\n';
            }
            out << "[AMD-S4-END] capture=" << capture << " rows=" << count << " reason=" << reason
                << " elapsed_ms=" << elapsed << '\n';
        }
        catch (...) { /* Diagnostics must never change game error handling. */ }
    }

private:
    void Add(const Event& event, const std::filesystem::path& directory, const char* runtime) noexcept
    {
        if (!start || finished) return;
        // Exclude the event that triggers the flush, and thus the flush cost,
        // from the measured interval. A late callback ends an idle capture too.
        if (Milliseconds(event.qpc - start) >= 30000)
        {
            Flush(directory, runtime, "window");
            return;
        }
        if (event.pending || event.source == std::string_view("Record") ||
            event.source == std::string_view("EfWaitLoop"))
            events[count++] = event;
        if (count == events.size()) Flush(directory, runtime, "capacity");
    }
    std::array<Event, 16384> events {};
    size_t count = 0;
    uint64_t attempt = 0, capture = 0;
    int64_t start = 0, frequency = 1;
    bool finished = false;
};
} // namespace AmdPreSr
