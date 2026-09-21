#pragma once
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <mutex>
#include "LmxxfColorProbe.h"

namespace DlssNr::Backend::LmxxfProbe
{
// Deterministic 4-slot ring buffer for frame-age isolation diagnostic.
// Invariants:
// - Slot allocation is bounded: exactly 4 capture slots + 4 output slots (~30-60 MB total).
// - No asynchronous fence polling needed: 4 slots ensure the GPU has retired slot N
//   long before slot (N + 4) is written (D3D12 swapchain latency is 2-3 frames max).
// - staging-current: Color N -> Capture[N % 4] -> Output[N % 4] -> FSR (age = 0).
// - staging-previous: Color N -> Capture[N % 4]; Output[N % 4] <- Capture[(N - 1) % 4] -> FSR (age = 1).
//   If slot (N - 1) is not from the same epoch or sequence != N - 1, it cleanly flags 'priming'.
class StagingProbe
{
  public:
    struct RecordResult
    {
        ID3D12Resource *output = nullptr;
        const char *reason = "idle";
        uint64_t epoch = 0;
        uint64_t currentSequence = 0;
        uint64_t sourceSequence = 0;
        uint32_t age = 0;  // 0 = current frame, 1 = previous frame
        bool priming = false;
    };

    struct Stats
    {
        uint64_t requested = 0, recorded = 0, submitted = 0, consumed = 0, fallback = 0;
        uint64_t currentWindowStart = 0;
        uint64_t currentWindowLength = 0;
        uint64_t bestWindowLength = 0;
        uint64_t primingFrames = 0;
    };

    StagingProbe() = default;
    StagingProbe(const StagingProbe &) = delete;
    StagingProbe &operator=(const StagingProbe &) = delete;
    ~StagingProbe()
    {
        for (auto &s : captures_) FreeSlot(s);
        for (auto &s : outputs_) FreeSlot(s);
    }

    RecordResult Record(Mode mode, ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
                        ID3D12Resource *color, D3D12_RESOURCE_STATES colorState,
                        UINT validWidth, UINT validHeight, uint64_t evaluateSequence)
    {
        std::lock_guard lock(mu_);
        ++stats_.requested;
        RecordResult r {};
        r.epoch = epoch_;
        r.currentSequence = evaluateSequence;

        // ── Validate input ──
        if (!device || !cmd || !color || cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return Fallback(r, "invalid_input");
        const auto desc = color->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
            return Fallback(r, "unsupported_dimension");
        if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
            desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
            desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
            desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
            desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM &&
            desc.Format != DXGI_FORMAT_R11G11B10_FLOAT &&
            desc.Format != DXGI_FORMAT_R16G16B16A16_UNORM)
            return Fallback(r, "unsupported_format");
        if (!validWidth || !validHeight || validWidth > desc.Width || validHeight > desc.Height)
            return Fallback(r, "invalid_extent");

        const size_t capIdx = evaluateSequence % kRingSlots;
        const size_t outIdx = evaluateSequence % kRingSlots;
        Slot &cap = captures_[capIdx];

        // ── Ensure capture texture allocated with matching extent/format ──
        if (!EnsureSlot(device, cap, static_cast<UINT>(desc.Width), desc.Height, desc.Format))
            return Fallback(r, "capture_alloc_failed");

        // ── Step 1: Copy current frame Color → Capture[N % 4] ──
        D3D12_RESOURCE_BARRIER b[4] {};
        UINT nBarriers = 0;
        if (colorState != D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            b[nBarriers].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[nBarriers].Transition = {color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                       colorState, D3D12_RESOURCE_STATE_COPY_SOURCE};
            ++nBarriers;
        }
        b[nBarriers].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[nBarriers].Transition = {cap.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   cap.currentState, D3D12_RESOURCE_STATE_COPY_DEST};
        ++nBarriers;
        cmd->ResourceBarrier(nBarriers, b);
        cmd->CopyResource(cap.texture, color);

        // Restore color to caller state; set capture to NON_PIXEL_SHADER_RESOURCE
        nBarriers = 0;
        if (colorState != D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            b[nBarriers].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[nBarriers].Transition = {color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE, colorState};
            ++nBarriers;
        }
        b[nBarriers].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[nBarriers].Transition = {cap.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        ++nBarriers;
        cmd->ResourceBarrier(nBarriers, b);
        cap.currentState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cap.epoch = epoch_;
        cap.sequence = evaluateSequence;
        ++stats_.recorded;

        // ── Step 2: Select source for output ──
        Slot *source = &cap; // staging-current: use current capture (age = 0)
        if (mode == Mode::StagingPrevious)
        {
            if (evaluateSequence <= 1)
            {
                // First frame has no previous capture; prime.
                r.priming = true;
                r.reason = "priming";
                ++stats_.primingFrames;
                return r;
            }
            const size_t prevIdx = (evaluateSequence - 1) % kRingSlots;
            Slot &prevCap = captures_[prevIdx];
            if (!prevCap.texture || prevCap.epoch != epoch_ || prevCap.sequence != evaluateSequence - 1)
            {
                // Sequence gap or epoch mismatch; prime.
                r.priming = true;
                r.reason = "priming";
                ++stats_.primingFrames;
                return r;
            }
            source = &prevCap; // staging-previous: use previous capture (age = 1)
        }

        // ── Step 3: Copy selected source → Output[N % 4] ──
        Slot &out = outputs_[outIdx];
        if (!EnsureSlot(device, out, static_cast<UINT>(desc.Width), desc.Height, desc.Format))
            return Fallback(r, "output_alloc_failed");

        nBarriers = 0;
        if (source->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            b[nBarriers].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[nBarriers].Transition = {source->texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                       source->currentState, D3D12_RESOURCE_STATE_COPY_SOURCE};
            ++nBarriers;
        }
        b[nBarriers].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[nBarriers].Transition = {out.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   out.currentState, D3D12_RESOURCE_STATE_COPY_DEST};
        ++nBarriers;
        cmd->ResourceBarrier(nBarriers, b);
        cmd->CopyResource(out.texture, source->texture);

        // Restore source to NON_PIXEL_SHADER_RESOURCE; set output to NON_PIXEL_SHADER_RESOURCE for FSR
        nBarriers = 0;
        if (source->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            b[nBarriers].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[nBarriers].Transition = {source->texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            ++nBarriers;
        }
        source->currentState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b[nBarriers].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[nBarriers].Transition = {out.texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        ++nBarriers;
        cmd->ResourceBarrier(nBarriers, b);
        out.currentState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        out.epoch = epoch_;
        out.sequence = source->sequence;

        // ── Update window stats ──
        if (stats_.currentWindowLength == 0)
            stats_.currentWindowStart = evaluateSequence;
        ++stats_.currentWindowLength;
        if (stats_.currentWindowLength > stats_.bestWindowLength)
            stats_.bestWindowLength = stats_.currentWindowLength;

        r.output = out.texture;
        r.reason = (mode == Mode::StagingCurrent) ? "staging_current_ok" : "staging_previous_ok";
        r.sourceSequence = source->sequence;
        r.age = (source == &cap) ? 0u : 1u;
        return r;
    }

    void InvalidateEpoch()
    {
        std::lock_guard lock(mu_);
        ++epoch_;
        stats_.currentWindowStart = 0;
        stats_.currentWindowLength = 0;
    }

    Stats GetStats() const { std::lock_guard lock(mu_); return stats_; }
    size_t CaptureCount() const
    {
        std::lock_guard lock(mu_);
        size_t n = 0;
        for (const auto &s : captures_) if (s.texture) ++n;
        return n;
    }
    size_t OutputCount() const
    {
        std::lock_guard lock(mu_);
        size_t n = 0;
        for (const auto &o : outputs_) if (o.texture) ++n;
        return n;
    }
    UINT64 AllocatedBytes() const { std::lock_guard lock(mu_); return allocBytes_; }

  private:
    struct Slot
    {
        ID3D12Resource *texture = nullptr;
        UINT width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
        uint64_t epoch = 0;
        uint64_t sequence = 0;
    };

    static constexpr size_t kRingSlots = 4;

    std::array<Slot, kRingSlots> captures_ {};
    std::array<Slot, kRingSlots> outputs_ {};
    mutable std::mutex mu_;
    uint64_t epoch_ = 1;
    UINT64 allocBytes_ = 0;
    Stats stats_ {};

    RecordResult Fallback(RecordResult &r, const char *reason)
    {
        r.reason = reason;
        ++stats_.fallback;
        stats_.currentWindowStart = 0;
        stats_.currentWindowLength = 0;
        return r;
    }

    bool EnsureSlot(ID3D12Device *dev, Slot &slot, UINT w, UINT h, DXGI_FORMAT fmt)
    {
        if (slot.texture && slot.width == w && slot.height == h && slot.format == fmt)
            return true;

        FreeSlot(slot);

        D3D12_RESOURCE_DESC td {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;

        const auto alloc = dev->GetResourceAllocationInfo(0, 1, &td);
        if (alloc.SizeInBytes == UINT64_MAX)
            return false;

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.texture))))
            return false;

        slot.width = w;
        slot.height = h;
        slot.format = fmt;
        slot.currentState = D3D12_RESOURCE_STATE_COMMON;
        slot.epoch = 0;
        slot.sequence = 0;
        allocBytes_ += alloc.SizeInBytes;
        return true;
    }

    void FreeSlot(Slot &slot)
    {
        if (!slot.texture) return;
        D3D12_RESOURCE_DESC d = slot.texture->GetDesc();
        ID3D12Device *dev = nullptr;
        if (SUCCEEDED(slot.texture->GetDevice(IID_PPV_ARGS(&dev))))
        {
            const auto alloc = dev->GetResourceAllocationInfo(0, 1, &d);
            if (alloc.SizeInBytes != UINT64_MAX && allocBytes_ >= alloc.SizeInBytes)
                allocBytes_ -= alloc.SizeInBytes;
            dev->Release();
        }
        slot.texture->Release();
        slot.texture = nullptr;
        slot.width = 0;
        slot.height = 0;
        slot.format = DXGI_FORMAT_UNKNOWN;
        slot.currentState = D3D12_RESOURCE_STATE_COMMON;
        slot.epoch = 0;
        slot.sequence = 0;
    }
};

} // namespace DlssNr::Backend::LmxxfProbe
