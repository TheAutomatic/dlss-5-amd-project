#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <mutex>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "CommandListProxy.h"

// Retirement of command-list-recorded GPU work (constant upload slots, whole feature
// contexts) keyed to the queue batch that actually executed the recording.
//
// Producers call Record() when they record into a command list, and Wait() before they
// overwrite or free what that recording references. The queue's ExecuteCommandLists hook
// calls OnExecuted() after the original call: stamps whose list (and generation, for
// wrapped lists) matches the executed batch are upgraded with the fence value signalled on
// that queue, and a completed fence value proves the batch's GPU work finished.
//
// A recording whose list was reset and re-recorded before any execution can never run; its
// stamp is retired as discarded at that point. Stamps that never see an execution (lists
// never submitted, or the queue hook not armed) are force-retired by age or timeout so a
// producer degrades with a logged warning instead of deadlocking.
namespace DlssNr::Submission::GpuBatch
{
inline constexpr uint64_t kUnknownGeneration = 0;

class Tracker
{
  public:
    static Tracker& Instance()
    {
        static Tracker instance;
        return instance;
    }

    struct Stamp
    {
        enum class State : uint8_t
        {
            Recorded,
            Submitted,
            Retired,
        };

        ID3D12GraphicsCommandList* list = nullptr;
        uint64_t generation = kUnknownGeneration;
        State state = State::Retired; // a default-constructed stamp claims nothing
        uint64_t fenceValue = 0;
        // Raw fence pointer owned by this tracker; it outlives every stamp that references
        // it (fences are released only at process exit).
        ID3D12Fence* fence = nullptr;
        uint64_t recordedAtSeq = 0;
    };

    Stamp Record(ID3D12GraphicsCommandList* list)
    {
        Stamp stamp;
        stamp.list = list;
        stamp.generation = GenerationOf(list);
        stamp.state = Stamp::State::Recorded;

        std::lock_guard lock(mutex_);
        stamp.recordedAtSeq = ++seq_;
        outstanding_.push_back(stamp);
        return stamp;
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
            for (auto& stamp : outstanding_)
            {
                if (stamp.state != Stamp::State::Recorded || stamp.list != executed)
                    continue;
                if (stamp.generation != kUnknownGeneration)
                {
                    const uint64_t generation = GenerationOf(executed);
                    if (generation != stamp.generation)
                    {
                        // The list was reset and re-recorded: this execution carries a newer
                        // recording, so ours was discarded and will never touch the GPU.
                        stamp.state = Stamp::State::Retired;
                        continue;
                    }
                }
                stamp.state = Stamp::State::Submitted;
                stamp.fence = fence;
                stamp.fenceValue = value;
            }
        }
        PruneRetired();
    }

    static bool Retired(const Stamp& stamp)
    {
        if (stamp.state == Stamp::State::Retired)
            return true;
        if (stamp.state != Stamp::State::Submitted || !stamp.fence)
            return false;
        return stamp.fence->GetCompletedValue() >= stamp.fenceValue;
    }

    // Blocks until the stamp no longer references pending GPU work: by fence completion,
    // by discard (recording never executed and many later batches exist), or by timeout.
    // The forced paths log once each; they are the degradation boundary, not the norm.
    void Wait(Stamp& stamp, uint64_t timeoutMs, const char* what)
    {
        if (Retired(stamp))
            return;

        const uint64_t deadline = NowMs() + timeoutMs;
        while (!Retired(stamp))
        {
            if (stamp.state == Stamp::State::Recorded && AgeOf(stamp) > 64)
            {
                Force(stamp, what, "recording never observed on an execution queue");
                return;
            }
            if (NowMs() >= deadline)
            {
                Force(stamp, what, "retirement timed out");
                return;
            }
            Sleep(1);
        }
    }

  private:
    static uint64_t GenerationOf(ID3D12GraphicsCommandList* list)
    {
        if (!list)
            return kUnknownGeneration;
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

    void PruneRetired()
    {
        std::erase_if(outstanding_, [](const Stamp& stamp) {
            return stamp.state == Stamp::State::Retired ||
                   (stamp.state == Stamp::State::Submitted && stamp.fence &&
                    stamp.fence->GetCompletedValue() >= stamp.fenceValue);
        });
    }

    uint64_t AgeOf(const Stamp& stamp)
    {
        std::lock_guard lock(mutex_);
        return seq_ > stamp.recordedAtSeq ? seq_ - stamp.recordedAtSeq : 0;
    }

    void Force(Stamp& stamp, const char* what, const char* reason)
    {
        std::lock_guard lock(mutex_);
        if (stamp.state == Stamp::State::Retired)
            return;
        static bool loggedDiscard = false;
        static bool loggedTimeout = false;
        const bool timeout = std::strstr(reason, "timed out") != nullptr;
        bool& logged = timeout ? loggedTimeout : loggedDiscard;
        if (!logged)
        {
            logged = true;
            LOG_WARN("GpuBatch: forced retirement of a {} ({})", what, reason);
        }
        stamp.state = Stamp::State::Retired;
        stamp.fence = nullptr;
    }

    static uint64_t NowMs()
    {
        return GetTickCount64();
    }

    mutable std::mutex mutex_;
    std::vector<Stamp> outstanding_;
    std::vector<std::pair<ID3D12CommandQueue*, Microsoft::WRL::ComPtr<ID3D12Fence>>> fences_;
    uint64_t seq_ = 0;
};
} // namespace DlssNr::Submission::GpuBatch
