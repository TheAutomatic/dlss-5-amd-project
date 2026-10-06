#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/NrStatusDisplay.h"
#include <cassert>
#include <iostream>

int main()
{
    using DlssNr::SummarizeNrStatus;
    const std::string ready = "lmxxf: recording ready | lmxxf arch=gfx1201 match=uuid hip=1 history=";
    for (const char* state : {"off", "ready", "active", "priming", "reset-after-discard", "reset-requested"}) {
        auto summary = SummarizeNrStatus(ready + state + " net_gpu_ms=8.5 perf=v3", true);
        assert(summary.state == "NR active" && summary.historyIssue.empty());
    }
    assert(SummarizeNrStatus(ready + "active", false).state == "NR waiting for frame submission");
    assert(SummarizeNrStatus(ready + "unsupported-passes net_gpu_ms=8", true).historyIssue ==
           "Only one NR pass is supported");
    assert(SummarizeNrStatus(ready + "future-history-fallback perf=v4", true).historyIssue ==
           "future-history-fallback");
    const std::string failed = "lmxxf: RecordOutputs failed";
    assert(SummarizeNrStatus(failed + " | lmxxf arch=gfx1201 history=off", true).state == failed);
    const std::string diagnostic = "lmxxf diagnostic: hip-passthrough (HIP round trip; NO NR)";
    assert(SummarizeNrStatus(diagnostic + " | lmxxf arch=gfx1201 history=diagnostic-view", true).state == diagnostic);
    const std::string mochi = "mochizuki 1920x1080 (model 1920x1080, 2 passes), network 8.50 ms (p95 9.10), 123 frames";
    assert(SummarizeNrStatus(mochi, true).state == "NR active");
    assert(SummarizeNrStatus(mochi, false).state == "NR waiting for frame submission");
    const std::string counters = " | completed frames=42 last completion 0s ago | timeout events=0 | skipped pending/GPU=0/0";
    assert(SummarizeNrStatus("AMD runtime 0.6.0 | Completed AMD NR passes=1 at 1920x1080" + counters, true).state == "NR active");
    assert(SummarizeNrStatus("AMD runtime 0.6.0 | Recorded pre-SR 1920x1080 passes=1" + counters, false).state == "NR waiting for frame submission");
    assert(SummarizeNrStatus("AMD runtime 0.6.0 | AMD submission stalled >5s" + counters, true).state == "AMD submission stalled >5s");
    assert(SummarizeNrStatus("AMD runtime 0.6.0 | Completed AMD NR passes=1 at 1920x1080 | effect failed" + counters, true).state.find("effect failed") != std::string::npos);
    // These are user-actionable messages, even when older work was submitted.
    for (const char* message : {
        "SR -> NR: invalid output extent; keeping the SR output",
        "AMD NR: waiting for a DirectX 12 SR frame",
        "AMD NR: warming up after an upscaler/resource change",
        "lmxxf: weights missing. Install native-game-tiled-assets beside OptiScaler, then restart.",
        "lmxxf: recording ready | lmxxf poisoned (fatal error) history=off",
        "lmxxf: same-frame boundary unavailable (original Color; NO NR)",
        "mochizuki: model missing; run the Mochizuki model installer",
        "mochizuki 1920x1080: requested 3 passes, using 2 (capacity 2); allocation failed",
        "mochizuki failed (see mochizuki_nr.log)",
        "mochizuki building the network",
        "New backend error: device removed"}) {
        assert(SummarizeNrStatus(message, true).state == message);
        assert(SummarizeNrStatus(message, false).state == message);
    }
    std::cout << "nr status display: PASS\n";
}
