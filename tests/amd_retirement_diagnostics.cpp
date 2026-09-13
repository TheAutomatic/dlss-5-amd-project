// Exercise the actual C++ writer; its log is also consumed by retirement_stats.py.
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/RetirementDiagnostics.h"
#include <cassert>
#include <memory>
#include <string>

int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::filesystem::path directory(argv[1]);
    const auto log = directory / L"amd_presr.log";
    const auto offset = std::filesystem::exists(log) ? std::filesystem::file_size(log) : 0;
    auto recorder = std::make_unique<AmdPreSr::RetirementDiagnostics>();
    using Scope = AmdPreSr::RetirementDiagnostics::Scope;
    recorder->BeginRecord(false); // NR-off must not start a capture.
    {
        Scope s(*recorder, directory, "test", "Record");
        s.event.outcome = "other_skip";
    }
    recorder->BeginRecord(true);
    {
        Scope s(*recorder, directory, "test", "Record");
        {
            Scope retirement(*recorder, directory, "test", "Record", &s.event);
            retirement.event.pending = 42;
            retirement.event.submitted = true;
            retirement.event.passes = 1;
            retirement.event.jobs[0] = 10;
            retirement.event.done[0] = 9;
            retirement.event.nativeDone = false;
            retirement.event.target = 11;
            retirement.event.gpuBefore = retirement.event.gpuAfter = 10;
        }
        s.event.outcome = "pending_skip"; // External scope must not emit early.
    }
    recorder->Flush(directory, "test", "shutdown");
    recorder->Flush(directory, "test", "shutdown"); // Must not duplicate session.
    std::ifstream file(log, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(offset));
    std::string text((std::istreambuf_iterator<char>(file)), {});
    assert(text.find("source=Record attempt=2 pending=42") != std::string::npos);
    assert(text.find("outcome=pending_skip") != std::string::npos);
    assert(text.find("rows=1 reason=shutdown") != std::string::npos);
    assert(text.find("source=Record attempt=1 ") == std::string::npos);
    const auto begin = text.find("[AMD-S4-BEGIN]");
    const auto end = text.find("[AMD-S4-END]");
    assert(begin != std::string::npos && end != std::string::npos);
    assert(text.find("[AMD-S4-BEGIN]", begin + 1) == std::string::npos);
    assert(text.find("[AMD-S4-END]", end + 1) == std::string::npos);
}
