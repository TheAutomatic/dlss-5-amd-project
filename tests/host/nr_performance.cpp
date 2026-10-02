#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/NrPerformanceStore.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/NrTimingDisplay.h"
#include <cassert>
#include <limits>
#include <thread>
#include <vector>
#include <iostream>
int main()
{
    DlssNr::PerformanceStore store;
    store.Record(NR_CPU_PREPARE, 1, 1);
    assert(!store.Read().enabled && !store.Read().stages[0].samples);
    store.SetEnabled(true);
    store.Record(NR_CPU_PREPARE, 1000, 1);
    for (int i = 0; i < 120; ++i) store.Record(NR_CPU_PREPARE, 1, 2 + i, 42);
    auto a = store.Read(), b = store.Read();
    assert(a.stages[0].samples == 121 && a.stages[0].mean_ms == 1 && a.stages[0].max_ms == 1);
    assert(b.stages[0].samples == a.stages[0].samples && b.stages[0].frame_id == 42);
    assert(!a.stages[NR_GPU_NETWORK].samples);
    store.Record(NR_GPU_NETWORK, std::numeric_limits<double>::quiet_NaN(), 0);
    store.Record(NR_GPU_NETWORK, -1, 0);
    assert(store.Read().dropped == 2 && !store.Read().stages[NR_GPU_NETWORK].samples);
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i)
        workers.emplace_back([&] { for (int j = 0; j < 1000; ++j) { store.Record(NR_CPU_ENQUEUE, 2, j); store.Read(); } });
    for (auto& t : workers) t.join();
    assert(store.Read().stages[NR_CPU_ENQUEUE].samples == 4000);
    store.SetEnabled(false);
    const auto oldEpoch = store.Epoch();
    store.SetEnabled(true);
    store.Record(NR_GPU_NETWORK, 3, 200, 5, 6, oldEpoch);
    assert(!store.Read().stages[0].samples && !store.Read().dropped);
    assert(!store.Read().stages[NR_GPU_NETWORK].samples);
    store.Record(NR_GPU_DECODE, 2.5, 500, 42, 8);
    const auto display = store.Read();
    assert(DlssNr::TimingValueText(display, NR_GPU_NETWORK, 600) == "N/A (waiting)");
    assert(DlssNr::TimingValueText(display, NR_GPU_DECODE, 600) == "2.50 ms");
    assert(DlssNr::TimingValueText(display, NR_GPU_DECODE, 2600) == "N/A (stale)");
    assert(DlssNr::TimingValueText(display, NR_GPU_DECODE, 499) == "N/A (stale)");
    assert(DlssNr::TimingValueText(display, NR_GPU_ENCODE, 600) == "N/A (waiting)");
    assert(DlssNr::TimingValueText({}, NR_GPU_DECODE, 600) == "N/A");
    store.SetEnabled(false);
    assert(DlssNr::TimingValueText(store.Read(), NR_GPU_DECODE, 600) == "disabled");
    store.SetEnabled(true);
    for (int i = 0; i < 8; ++i) store.Record(NR_GPU_NETWORK, .001, 999);
    assert(!store.Read().stages[NR_GPU_NETWORK].samples && store.Read().dropped == 8);
    for (double ms : {9.0, 9.2, 0.1, 9.1, 9.3}) store.Record(NR_GPU_NETWORK, ms, 1000);
    auto median = store.Read();
    assert(median.stages[NR_GPU_NETWORK].mean_ms == 9.1 && (median.reserved & 1u));
    assert(DlssNr::TimingValueText(median, NR_GPU_NETWORK, 1001, true).find("median") == 0);
    median.reserved |= 2u;
    assert(DlssNr::TimingValueText(median, NR_GPU_NETWORK, 1001) == "N/A (PDL)");
    store.ResetStage(NR_GPU_NETWORK);
    assert(!store.Read().stages[NR_GPU_NETWORK].samples);
    std::cout << "nr performance: PASS\n";
}
