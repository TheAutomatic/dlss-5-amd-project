#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vulkan/vulkan.h>

// One reservation per actual execution, never per network callback. The owning
// ProductRuntime already remains alive until its last Vulkan submission completes
// (and is quarantined on device loss), so its query pool has the same lifetime.
class MochizukiStageTiming
{
public:
    static constexpr uint32_t kSlots = 8;
    static constexpr uint32_t kPasses = 30;
    static constexpr uint32_t kQueries = 2 + 2 * kPasses;
    static constexpr uint32_t kUnavailable = kSlots;
    struct Sample { float total, network; };

    static bool Decode(const uint64_t* stamps, uint32_t passes, uint64_t mask,
                       double period, Sample& out)
    {
        if (passes > kPasses || !(period > 0)) return false;
        const uint64_t total = (stamps[1] - stamps[0]) & mask;
        if (!total) return false;
        uint64_t network = 0;
        for (uint32_t p = 0; p < passes; ++p) {
            const uint64_t delta = (stamps[3 + 2 * p] - stamps[2 + 2 * p]) & mask;
            if (delta > total || network > total - delta) return false;
            network += delta;
        }
        out = {float(double(total) * period / 1e6), float(double(network) * period / 1e6)};
        return std::isfinite(out.total) && std::isfinite(out.network) && out.total > 0;
    }

    void Create(VkDevice device, double period, uint32_t graphicsBits, uint32_t computeBits)
    {
        device_ = device; period_ = period;
        bits_[0] = graphicsBits; bits_[1] = computeBits;
        if (!(period > 0) || (!graphicsBits && !computeBits)) return;
        VkQueryPoolCreateInfo qi {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kSlots * kQueries;
        if (vkCreateQueryPool(device, &qi, nullptr, &pool_) != VK_SUCCESS) pool_ = VK_NULL_HANDLE;
    }
    ~MochizukiStageTiming() { if (pool_) vkDestroyQueryPool(device_, pool_, nullptr); }
    MochizukiStageTiming() = default;
    MochizukiStageTiming(const MochizukiStageTiming&) = delete;
    MochizukiStageTiming& operator=(const MochizukiStageTiming&) = delete;
    void DiscardSamples() noexcept
    {
        // Preserve reservations until Vulkan completion, but exclude a previous route's samples.
        for (auto& slot : slots_) if (slot.reserved) slot.discard = true;
    }

    template<class Accept> void Harvest(uint64_t completed, Accept accept)
    {
        if (!pool_ || completed == UINT64_MAX) return;
        // Read oldest first, even after a skipped timing frame or ring wrap.
        for (;;) {
            uint32_t oldest = kUnavailable;
            for (uint32_t i = 0; i < kSlots; ++i)
                if (slots_[i].finished && (oldest == kUnavailable || slots_[i].finished < slots_[oldest].finished))
                    oldest = i;
            if (oldest == kUnavailable || slots_[oldest].finished > completed) return;
            auto& slot = slots_[oldest];
            uint64_t stamps[kQueries] {};
            const uint32_t n = 2 + 2 * slot.passes;
            const VkResult result = vkGetQueryPoolResults(device_, pool_, oldest * kQueries, n,
                n * sizeof(uint64_t), stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (result == VK_NOT_READY) return; // Never wait, and never reset an unread slot.
            Sample sample {};
            if (!slot.discard && result == VK_SUCCESS && Decode(stamps, slot.passes, slot.mask, period_, sample)) accept(sample);
            slot = {};
        }
    }

    // A direct recipe replays its recorded timestamp writes without Begin().
    // Keep its mask/pass metadata and consume only the submission marker.
    template<class Accept> void HarvestRecording(uint32_t index, uint64_t completed, Accept accept)
    {
        if (!pool_ || index >= kSlots || completed == UINT64_MAX) return;
        auto& slot = slots_[index];
        if (!slot.finished || slot.finished > completed) return;
        struct Query { uint64_t stamp, available; } queries[kQueries] {};
        uint64_t stamps[kQueries] {};
        const uint32_t n = 2 + 2 * slot.passes;
        const auto result = vkGetQueryPoolResults(device_, pool_, index * kQueries, n,
            n * sizeof(Query), queries, sizeof(Query),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS) return;
        for (uint32_t i = 0; i < n; ++i) {
            if (!queries[i].available) return;
            stamps[i] = queries[i].stamp;
        }
        Sample sample {};
        if (!slot.discard && Decode(stamps, slot.passes, slot.mask, period_, sample)) accept(sample);
        slot.finished = 0;
    }
    uint32_t Begin(VkCommandBuffer cmd, bool compute)
    {
        const uint32_t bits = bits_[compute ? 1 : 0];
        if (!pool_ || !bits) return kUnavailable;
        for (uint32_t n = 0; n < kSlots; ++n) {
            const uint32_t i = (next_ + n) % kSlots;
            auto& slot = slots_[i];
            if (slot.reserved) continue;
            slot.reserved = true;
            slot.mask = bits < 64 ? (uint64_t(1) << bits) - 1 : ~uint64_t(0);
            next_ = (i + 1) % kSlots;
            vkCmdResetQueryPool(cmd, pool_, i * kQueries, kQueries);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_, i * kQueries);
            return i;
        }
        return kUnavailable; // GPU lag: drop measurement rather than overwrite or stall.
    }
    void BeginNetwork(VkCommandBuffer cmd, uint32_t slot)
    {
        if (slot < kSlots && slots_[slot].passes < kPasses)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, slot * kQueries + 2 + 2 * slots_[slot].passes);
    }
    void EndNetwork(VkCommandBuffer cmd, uint32_t slot)
    {
        if (slot < kSlots && slots_[slot].passes < kPasses)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, slot * kQueries + 3 + 2 * slots_[slot].passes++);
    }
    void End(VkCommandBuffer cmd, uint32_t slot)
    {
        if (slot < kSlots) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, slot * kQueries + 1);
    }
    void Submitted(uint32_t slot, uint64_t finished) noexcept
    {
        if (slot < kSlots) slots_[slot].finished = finished;
    }
    void CancelUnsubmitted(uint32_t slot) noexcept
    {
        if (slot < kSlots && !slots_[slot].finished) slots_[slot] = {};
    }
private:
    struct Slot { uint64_t finished = 0, mask = 0; uint32_t passes = 0; bool reserved = false, discard = false; };
    Slot slots_[kSlots] {};
    uint32_t next_ = 0, bits_[2] {};
    VkDevice device_ {};
    VkQueryPool pool_ {};
    double period_ = 0;
};
