#pragma once
#include <atomic>
#include <cstdint>

// A single snapshot for the whole resource set; reset cannot split a pair lookup.
class FrameResourceReadiness
{
    std::atomic<uint32_t> bits {0};
public:
    void Reset() noexcept { bits.store(0, std::memory_order_release); }
    void Mark(uint32_t type) noexcept
    {
        if (type < 32) bits.fetch_or(uint32_t{1} << type, std::memory_order_release);
    }
    bool Contains(uint32_t type) const noexcept
    {
        return type < 32 && ContainsMask(uint32_t{1} << type);
    }
    bool ContainsMask(uint32_t mask) const noexcept
    {
        return (bits.load(std::memory_order_acquire) & mask) == mask;
    }
};
