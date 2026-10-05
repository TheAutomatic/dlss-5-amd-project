#pragma once
#include <atomic>
#include <cstdint>

namespace DlssNr
{
class NrSessionActivity
{
    std::atomic<uint64_t> state { 0 };

  public:
    uint64_t Token() const { return state.load(std::memory_order_acquire) & ~uint64_t { 1 }; }
    bool IsRunning() const { return (state.load(std::memory_order_acquire) & 1) != 0; }

    void Reset()
    {
        auto previous = state.load(std::memory_order_acquire);
        while (!state.compare_exchange_weak(previous, (previous + 2) & ~uint64_t { 1 },
                                             std::memory_order_acq_rel)) {}
    }

    void Succeeded(uint64_t token)
    {
        state.compare_exchange_strong(token, token | 1, std::memory_order_acq_rel);
    }
};
}
