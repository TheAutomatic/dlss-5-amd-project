#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/fsrd_preprocess/RetiredSlotReservations.h"
#include <cassert>
#include <cstdint>
#include <set>

int main()
{
    FSRD::RetiredSlotReservations<uint64_t> pool;
    std::set<uint64_t> retired;
    size_t payloads = 3, growths = 0;
    auto grow = [&](size_t required) {
        while (payloads < required) { payloads += 3; ++growths; }
        return payloads;
    };
    auto done = [&](uint64_t ticket) { return retired.contains(ticket); };
    // Existing initial page is adopted, not duplicated. Four pending recordings
    // must get distinct payload addresses even though the old ring had 3 slots.
    for (uint64_t ticket = 1; ticket <= 4; ++ticket)
    {
        pool.Reserve(1, done, grow);
        assert(pool.Commit(ticket) == ticket - 1);
    }
    assert(payloads == 6 && growths == 1);
    // Reuse a retired non-head slot while the oldest slot remains outstanding.
    retired.insert(2);
    pool.Reserve(1, done, grow);
    assert(pool.Commit(5) == 1);
    assert(payloads == 6 && growths == 1);

    // Reserve five filter dispatches together; no ticket is committed merely
    // by reserving, and abandoning an unused reservation must not pin capacity.
    FSRD::RetiredSlotReservations<uint64_t> filter;
    filter.Reserve(5, done, grow);
    filter.Reserve(5, done, grow);
    for (uint64_t i = 0; i < 5; ++i)
        assert(filter.Commit(100 + i) == i);
    assert(!filter.HasReserved());
    try { filter.Commit(999); assert(false); } catch (const std::runtime_error&) {}

    // Grow failure after finding some free slots publishes no partial reservation.
    bool failed = false;
    try {
        filter.Reserve(5, done, [](size_t) -> size_t {
            throw std::runtime_error("allocation/capacity failure");
        });
    } catch (const std::runtime_error&) { failed = true; }
    assert(failed && !filter.HasReserved());
    // Previously committed tickets remain protected after that failure.
    retired.insert(102);
    filter.Reserve(1, done, grow);
    assert(filter.Commit(200) == 2);

    // Zero tickets cannot consume a reservation or make a live slot reusable.
    filter.Reserve(1, done, grow);
    try { filter.Commit(0); assert(false); } catch (const std::runtime_error&) {}
    assert(filter.HasReserved());
    filter.Commit(201);

    // Model a loading burst with no completions, then verify reuse without growth.
    FSRD::RetiredSlotReservations<uint64_t> burst;
    size_t burstCapacity = 0;
    auto burstGrow = [&](size_t required) { return burstCapacity = required; };
    for (uint64_t frame = 1; frame <= 32; ++frame)
    {
        burst.Reserve(5, [](auto) { return false; }, burstGrow);
        for (unsigned pass = 0; pass < 5; ++pass)
            assert(burst.Commit(frame * 5 + pass) == (frame - 1) * 5 + pass);
    }
    assert(burstCapacity == 160);
    burst.Reserve(5, [](auto) { return true; }, [](size_t) -> size_t { assert(false); return 0; });
    for (unsigned pass = 0; pass < 5; ++pass)
        assert(burst.Commit(1000 + pass) == pass);
}
