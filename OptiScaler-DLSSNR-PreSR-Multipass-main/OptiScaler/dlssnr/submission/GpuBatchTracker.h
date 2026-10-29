#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <mutex>
#include <cstdint>
#include <utility>
#include <vector>
#include "CommandListProxy.h"

// Retirement of command-list-recorded GPU work (constant upload slots, whole feature
// contexts) keyed to the queue batch that actually executed the recording.
//
// Producers call Record() when they record into a command list and receive a stable
// Ticket. All state lives inside the tracker: the queue's ExecuteCommandLists hook calls
// OnExecuted() after the original call, matches recorded tickets by list identity (and
// generation, for wrapped lists), and upgrades them with the fence value signalled on that
// queue. A ticket is Retired only when its batch's fence completed, or when the recording
// was provably discarded (its list was reset and re-recorded before executing).
//
// There is deliberately no forced retirement: an unsubmitted or unfinished recording keeps
// its resources protected forever. Callers that cannot proceed (ring slot busy, teardown
// with work outstanding) must fail or defer, never overwrite.
namespace DlssNr::Submission::GpuBatch
{
inline constexpr uint64_t kUnknownGeneration = 0;
inline constexpr uint64_t kInvalidTicket = 0;

class Tracker
{
  public:
    using Ticket = uint64_t;

    enum class Status : uint8_t
    {
        Outstanding, // recorded, not yet seen on an execution queue (or not yet completed)
        Retired,     // batch completed, or recording provably discarded
        Unknown,     // ticket never issued (or already reaped and forgotten)
    };

    static Tracker& Instance()
    {
        static Tracker instance;
        return instance;
    }

    Ticket Record(ID3D12GraphicsCommandList* list)
    {
        if (!list)
            return kInvalidTicket;

        std::lock_guard lock(mutex_);
        Entry entry;
        entry.ticket = ++seq_;
        entry.list = list;
        entry.generation = GenerationOf(list);
        entries_.push_back(entry);
        return entry.ticket;
    }

    // Call after the original ExecuteCommandLists returned.
    void OnExecuted(UINT num, ID3D12CommandList* const* lists, ID3D12CommandQueue* queue)
    {
        if (!queue || !lists)
            return;

        ID3D12Fence* fence = nullptr;
        uint64_t value = 0;
        {
            std::lock_guard lock(mutex_);
            fence = FenceFor(queue);
            if (!fence)
                return;
            value = ++seq_;
        }

        if (FAILED(queue->Signal(fence, value)))
            return;

        std::lock_guard lock(mutex_);
        for (UINT i = 0; i < num; ++i)
        {
            auto* executed = static_cast<ID3D12GraphicsCommandList*>(lists[i]);
            for (auto& entry : entries_)
            {
                if (entry.state != Entry::State::Recorded || entry.list != executed)
                    continue;
                if (entry.generation != kUnknownGeneration)
                {
                    const uint64_t generation = GenerationOf(executed);
                    if (generation != entry.generation)
                    {
                        // The list was reset and re-recorded: this execution carries a newer
                        // recording, so ours was discarded and will never touch the GPU.
                        entry.state = Entry::State::Discarded;
                        continue;
                    }
                }
                entry.state = Entry::State::Submitted;
                entry.fence = fence;
                entry.fenceValue = value;
            }
        }
        Reap();
    }

    Status StatusOf(Ticket ticket)
    {
        if (ticket == kInvalidTicket)
            return Status::Retired; // nothing was recorded; nothing is protected

        std::lock_guard lock(mutex_);
        for (auto& entry : entries_)
        {
            if (entry.ticket != ticket)
                continue;
            if (entry.state == Entry::State::Discarded || entry.state == Entry::State::Completed)
                return Status::Retired;
            if (entry.state == Entry::State::Submitted && entry.fence &&
                entry.fence->GetCompletedValue() >= entry.fenceValue)
            {
                entry.state = Entry::State::Completed;
                return Status::Retired;
            }
            return Status::Outstanding;
        }
        // Completed entries are reaped; a ticket that is no longer tracked completed
        // earlier. Only an invalid ticket (never issued this session) is unknown, and
        // treating it as retired would silently drop protection, so report it.
        return ticket <= seq_ ? Status::Retired : Status::Unknown;
    }

    bool Retired(Ticket ticket)
    {
        return StatusOf(ticket) == Status::Retired;
    }

    // Polls until the ticket retires or the budget is exhausted. Never force-retires:
    // a false return leaves the recording's resources protected and the caller must
    // fail or defer the operation that wanted to reuse them.
    bool Wait(Ticket ticket, uint64_t budgetMs)
    {
        const uint64_t deadline = GetTickCount64() + budgetMs;
        while (!Retired(ticket))
        {
            if (GetTickCount64() >= deadline)
                return false;
            Sleep(1);
        }
        return true;
    }

  private:
    struct Entry
    {
        enum class State : uint8_t
        {
            Recorded,
            Submitted,
            Completed,
            Discarded,
        };

        Ticket ticket = kInvalidTicket;
        ID3D12GraphicsCommandList* list = nullptr;
        uint64_t generation = kUnknownGeneration;
        State state = State::Recorded;
        // Raw fence pointer owned by this tracker; fences are released only at exit, so
        // it outlives every entry that references it.
        ID3D12Fence* fence = nullptr;
        uint64_t fenceValue = 0;
    };

    static uint64_t GenerationOf(ID3D12GraphicsCommandList* list)
    {
        ILogicalCommandList* logical = nullptr;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&logical))) && logical)
        {
            const uint64_t generation = logical->Identity().generation;
            logical->Release();
            return generation;
        }
        return kUnknownGeneration;
    }

    ID3D12Fence* FenceFor(ID3D12CommandQueue* queue)
    {
        for (auto& entry : fences_)
            if (entry.first == queue)
                return entry.second.Get();

        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        ID3D12Device* device = nullptr;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
            return nullptr;
        const HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        device->Release();
        if (FAILED(hr))
            return nullptr;
        fences_.emplace_back(queue, std::move(fence));
        return fences_.back().second.Get();
    }

    void Reap()
    {
        std::erase_if(entries_, [](const Entry& entry) {
            return entry.state == Entry::State::Discarded ||
                   entry.state == Entry::State::Completed ||
                   (entry.state == Entry::State::Submitted && entry.fence &&
                    entry.fence->GetCompletedValue() >= entry.fenceValue);
        });
    }

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::vector<std::pair<ID3D12CommandQueue*, Microsoft::WRL::ComPtr<ID3D12Fence>>> fences_;
    uint64_t seq_ = 0;
};
} // namespace DlssNr::Submission::GpuBatch
