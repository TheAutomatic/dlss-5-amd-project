#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <memory>
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
// was provably discarded and never reached the GPU.
//
// Lifecycle observation (step 1 of the reuse-contract work): for wrapped lists, Record()
// subscribes an observer via IRecordingResources::ObserveResources. The proxy notifies
// Invalidated(identity) on Reset (old generation) and on final Release (destroy). An
// invalidated recording that was never submitted can never reach the GPU, so its entries
// are marked Discarded immediately. An invalidated recording that WAS submitted keeps its
// fence retirement: Reset/destroy does not cancel in-flight GPU work. The same rule
// applies to the generation mismatch observed in OnExecuted.
//
// Diagnostics: Reap() emits throttled gauges (tracked entries, native-identity entries,
// in-flight recordings, replays observed after first completion, observer invalidations)
// so field logs can separate record accumulation from actual GPU resource retention, and
// a replay counter that decides how far back slot reuse must guarantee replay safety.
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
    struct CompletionReceipt
    {
        enum class Outcome : uint8_t { Outstanding, Completed, Discarded };
        // A replay re-arms Outstanding until its latest batch completes. Capture
        // owners retain this receipt after the tracker reaps the recording entry.
        std::atomic<Outcome> outcome { Outcome::Outstanding };
    };
    using CompletionReceiptPtr = std::shared_ptr<CompletionReceipt>;

    // Attach immediately after Record and before submission/invalidation. Diagnostic
    // consumers alone allocate receipts; ordinary dispatch tickets allocate nothing.
    CompletionReceiptPtr ObserveCompletion(Ticket ticket)
    {
        std::lock_guard lock(mutex_);
        for (auto& entry : entries_)
        {
            if (entry.ticket != ticket)
                continue;
            if (!entry.receipt)
                entry.receipt = std::make_shared<CompletionReceipt>();
            if (entry.state == Entry::State::Completed)
                entry.receipt->outcome.store(CompletionReceipt::Outcome::Completed, std::memory_order_release);
            else if (entry.state == Entry::State::Discarded)
                entry.receipt->outcome.store(CompletionReceipt::Outcome::Discarded, std::memory_order_release);
            return entry.receipt;
        }
        return nullptr;
    }


    static Tracker& Instance()
    {
        static Tracker instance;
        return instance;
    }

    Ticket Record(ID3D12GraphicsCommandList* list)
    {
        if (!list)
            return kInvalidTicket;

        // Identity QI and observer registration happen outside mutex_: ObserveResources
        // takes the submission RecordingMutex, and Invalidated arrives under that same
        // mutex, so mutex_ must stay a leaf lock (no reverse-order acquisition).
        const ListIdentity identity = IdentityOf(list);
        if (identity.stableId != 0)
            ObserveList(list, identity);

        std::lock_guard lock(mutex_);
        // Always returns a valid ticket: callers treat kInvalidTicket as "nothing to
        // protect" (or throw, in RetiredSlotReservations::Commit). Recording lifetime
        // determines reclamation in Reap(); entry count must not weaken protection.
        Entry entry;
        entry.ticket = ++seq_;
        entry.list = list;
        entry.stableId = identity.stableId;
        entry.generation = identity.generation;
        entries_.push_back(entry);
        hasEntries_.store(true, std::memory_order_release);
        return entry.ticket;
    }

    // Call after the original ExecuteCommandLists returned.
    void OnExecuted(UINT num, ID3D12CommandList* const* lists, ID3D12CommandQueue* queue)
    {
        if (!queue || !lists || num == 0 || !hasEntries_.load(std::memory_order_acquire))
            return;

        ID3D12Fence* fence = nullptr;
        uint64_t value = 0;
        {
            std::lock_guard lock(mutex_);
            // Completed tickets may outlive the RR setting. Reap them before deciding
            // whether this batch needs a fence; never gate retirement on the INI switch.
            Reap();
            // Match any non-Discarded entry for the executed lists: a replay of an
            // already-Submitted/Completed list still needs a fresh fence so resources
            // used by the replay are not retired on the first execution's completion.
            const bool hasRecording = std::any_of(entries_.begin(), entries_.end(),
                [num, lists](const Entry& entry) {
                    return entry.state != Entry::State::Discarded &&
                           std::find(lists, lists + num,
                                     static_cast<ID3D12CommandList*>(entry.list)) != lists + num;
                });
            if (!hasRecording)
                return;
            fence = FenceFor(queue);
            if (!fence)
                return;
            value = ++seq_;
        }

        if (FAILED(queue->Signal(fence, value)))
            return;

        // Identity lookups (and the IUnknown Release inside them) take the submission
        // RecordingMutex via CommandListProxy::Release; snapshot them BEFORE locking
        // mutex_, because Invalidated arrives under RecordingMutex and waits on mutex_.
        std::vector<ListIdentity> identities(num);
        for (UINT i = 0; i < num; ++i)
            identities[i] = IdentityOf(static_cast<ID3D12GraphicsCommandList*>(lists[i]));

        std::lock_guard lock(mutex_);
        for (UINT i = 0; i < num; ++i)
        {
            auto* executed = static_cast<ID3D12GraphicsCommandList*>(lists[i]);
            const ListIdentity& identity = identities[i];
            for (auto& entry : entries_)
            {
                if (entry.list != executed)
                    continue;
                if (entry.stableId != 0 && identity.stableId == entry.stableId &&
                    identity.generation != entry.generation)
                {
                    // The list was reset and re-recorded: this execution carries a newer
                    // recording. An entry that never submitted can never reach the GPU
                    // (Reset erased its commands), so it is discarded. An entry that DID
                    // submit keeps its own fence retirement until completion — Reset does
                    // not cancel in-flight work — but is now invalidated: it can never be
                    // replayed, so Reap() reclaims it once its fence completes.
                    if (entry.state == Entry::State::Recorded)
                    {
                        entry.state = Entry::State::Discarded;
                        if (entry.receipt)
                            entry.receipt->outcome.store(CompletionReceipt::Outcome::Discarded,
                                                         std::memory_order_release);
                    }
                    else
                        entry.invalidated = true;
                    continue;
                }
                if (entry.stableId != 0 && identity.stableId != entry.stableId)
                {
                    // Address reuse delivered a different proxy on the same pointer:
                    // never re-arm an entry across identities.
                    continue;
                }
                if (entry.state == Entry::State::Completed && entry.stableId != 0)
                {
                    // A closed recording was submitted again after its first completion.
                    // Only identified (wrapped) recordings count: for them, same
                    // generation means the identical recording truly re-executed (a
                    // Reset would have advanced the generation). A native list reused
                    // by its owning pool re-submits the same pointer every frame after
                    // an invisible Reset, which is indistinguishable from a replay at
                    // this layer and must not pollute the datum. This counter decides
                    // how far back slot reuse has to guarantee replay safety.
                    ++replaysAfterCompletion_;
                    if (replaysAfterCompletion_ == 1)
                    {
                        LOG_WARN("GpuBatch: replay after first completion list=0x{:016X} stableId={} generation={}",
                                 (uint64_t)(uintptr_t)executed, identity.stableId, identity.generation);
                    }
                    else if (replaysAfterCompletion_ % 1024 == 0)
                    {
                        LOG_DEBUG("GpuBatch: replays after completion now {} (latest list=0x{:016X})",
                                  replaysAfterCompletion_.load(std::memory_order_relaxed),
                                  (uint64_t)(uintptr_t)executed);
                    }
                }
                // First submission: Record the fence. Replays (same list submitted again
                // before or after the first completed): overwrite with the latest fence
                // value, because the replay re-executes the same commands and the slot's
                // data must survive until the replay's batch completes.
                entry.state = Entry::State::Submitted;
                entry.fence = fence;
                entry.fenceValue = value;
                if (entry.receipt)
                    entry.receipt->outcome.store(CompletionReceipt::Outcome::Outstanding, std::memory_order_release);
            }
        }
        Reap();
    }

    // Field datum for the reuse-contract policy: how often a recording was submitted
    // again after its first completion. Exposed for diagnostics and tests.
    uint64_t ReplaysAfterCompletion() const
    {
        return replaysAfterCompletion_.load(std::memory_order_relaxed);
    }

    // Diagnostic: current tracked-entry count (diagrams reclamation health; never a
    // capacity control). Used by tests to observe invalidation-driven reclamation.
    size_t TrackedEntries() const
    {
        std::lock_guard lock(mutex_);
        return entries_.size();
    }

    // IRecordingResources observer callback (Reset old-generation, or list destroy).
    // Arrives under the submission RecordingMutex; takes only the leaf mutex_.
    void OnRecordingInvalidated(const RecordingIdentity& id) noexcept
    {
        std::lock_guard lock(mutex_);
        ++invalidationsObserved_;
        for (auto& entry : entries_)
        {
            if (entry.stableId != id.list || entry.generation != id.generation)
                continue;
            // Never-submitted: provably dead (commands erased / list destroyed),
            // discard immediately. Submitted: the batch may still be executing, so
            // fence retirement still governs, but the recording can never execute
            // again — mark invalidated so Reap() reclaims it once the fence completes.
            // Completed entries are marked so the same rule reclaims them.
            if (entry.state == Entry::State::Recorded)
            {
                entry.state = Entry::State::Discarded;
                if (entry.receipt)
                    entry.receipt->outcome.store(CompletionReceipt::Outcome::Discarded, std::memory_order_release);
            }
            else
                entry.invalidated = true;
        }
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
            if (entry.state == Entry::State::Submitted && entry.fence)
            {
                const uint64_t completed = entry.fence->GetCompletedValue();
                // UINT64_MAX means the device was removed: the fence will never complete
                // normally. Reporting Outstanding keeps the resources protected; the
                // caller's timeout path handles the degraded case.
                if (completed != UINT64_MAX && completed >= entry.fenceValue)
                {
                    entry.state = Entry::State::Completed;
                    if (entry.receipt)
                        entry.receipt->outcome.store(CompletionReceipt::Outcome::Completed, std::memory_order_release);
                    return Status::Retired;
                }
            }
            return Status::Outstanding;
        }
        // Discarded entries are reaped; a missing issued ticket no longer refers
        // to an executable recording. A ticket never issued this session is unknown;
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
    struct ListIdentity
    {
        uint64_t stableId = 0;   // proxy stableIdentity; 0 for native (unwrapped) lists
        uint64_t generation = 0;
    };

    // Forwards proxy Reset/destroy notifications into the tracker. Holds no reference
    // to the proxy (observer contract: the recording owns its observers).
    struct Observer final : RecordingObserver
    {
        void Invalidated(RecordingIdentity id) noexcept override
        {
            Tracker::Instance().OnRecordingInvalidated(id);
        }
    };

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
        uint64_t stableId = 0;    // 0: native list, no lifecycle observation
        uint64_t generation = kUnknownGeneration;
        State state = State::Recorded;
        // Set when lifecycle observation proved this recording can never execute
        // again (Reset advanced its generation, or the list was destroyed). Reclaim
        // eligibility, not a protection change: fence retirement still applies until
        // completion for entries that already submitted.
        bool invalidated = false;
        // Raw fence pointer owned by this tracker; fences are released only at exit, so
        // it outlives every entry that references it.
        ID3D12Fence* fence = nullptr;
        uint64_t fenceValue = 0;
        CompletionReceiptPtr receipt;
    };

    static ListIdentity IdentityOf(ID3D12GraphicsCommandList* list)
    {
        ILogicalCommandList* logical = nullptr;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&logical))) && logical)
        {
            const RecordingIdentity id = logical->Identity();
            logical->Release();
            return {id.list, id.generation};
        }
        return {}; // native list: no stable identity, no lifecycle observation
    }

    // Best-effort, race-tolerant: ObserveResources has no duplicate suppression, so a
    // concurrent double registration only adds an idempotent Invalidated fan-out.
    // Called outside mutex_ (ObserveResources takes the submission RecordingMutex).
    void ObserveList(ID3D12GraphicsCommandList* list, const ListIdentity& identity)
    {
        std::shared_ptr<RecordingObserver> observer;
        {
            // observer_ is created under mutex_ and only a copy escapes the lock:
            // concurrent Record() calls must not read/write the same shared_ptr
            // object unsynchronized.
            std::lock_guard lock(mutex_);
            const bool alreadyObserved = std::any_of(entries_.begin(), entries_.end(),
                [&identity](const Entry& entry) {
                    return entry.stableId == identity.stableId &&
                           entry.generation == identity.generation;
                });
            if (alreadyObserved)
                return;
            if (!observer_)
                observer_ = std::make_shared<Observer>();
            observer = observer_;
        }
        IRecordingResources* resources = nullptr;
        if (SUCCEEDED(list->QueryInterface(__uuidof(IRecordingResources),
                                           reinterpret_cast<void**>(&resources))) && resources)
        {
            const HRESULT hr = resources->ObserveResources(std::move(observer));
            resources->Release();
            (void)hr; // observation is an optimization; fence retirement still applies
        }
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
        // Promote Submitted entries whose fence completed (the same condition StatusOf
        // applies), so reclamation does not depend on the holder polling the ticket.
        for (auto& entry : entries_)
        {
            if (entry.state == Entry::State::Submitted && entry.fence)
            {
                const uint64_t completed = entry.fence->GetCompletedValue();
                if (completed != UINT64_MAX && completed >= entry.fenceValue)
                {
                    entry.state = Entry::State::Completed;
                    if (entry.receipt)
                        entry.receipt->outcome.store(CompletionReceipt::Outcome::Completed, std::memory_order_release);
                }
            }
        }
        // Discarded entries are provably dead. A Completed entry is reclaimable once
        // invalidated: its GPU work finished AND it can never execute again, so the
        // untracked-ticket fallback in StatusOf (Retired) matches its true state.
        // Completed-but-never-invalidated entries stay: the recording may still be
        // replayed and OnExecuted re-arms it with a fresh fence. Native entries
        // (stableId == 0) have no invalidation signal and remain tracked.
        std::erase_if(entries_, [](const Entry& entry) {
            return entry.state == Entry::State::Discarded ||
                   (entry.invalidated && entry.state == Entry::State::Completed);
        });

        // Field diagnostics, throttled to one line per minute: separates record
        // accumulation (native entries) from genuine in-flight work, and surfaces the
        // replay-after-completion count that the reuse contract will be tuned against.
        const uint64_t now = GetTickCount64();
        if (now - lastGaugeMs_ >= 60000 && !entries_.empty())
        {
            lastGaugeMs_ = now;
            size_t native = 0, inFlight = 0;
            for (const auto& entry : entries_)
            {
                if (entry.stableId == 0)
                    ++native;
                if (entry.state == Entry::State::Recorded || entry.state == Entry::State::Submitted)
                    ++inFlight;
            }
            // INFO (not DEBUG): the PBZ field session proved gauges are invisible at
            // the default LogLevel, and this line is the step-2 policy's data source.
            LOG_INFO("GpuBatch gauges: entries={} native={} inFlight={} replaysAfterCompletion={} invalidations={}",
                      entries_.size(), native, inFlight, replaysAfterCompletion_.load(std::memory_order_relaxed),
                      invalidationsObserved_.load(std::memory_order_relaxed));
        }

        // More than kMaxEntries Recorded/Submitted recordings in flight at once is a
        // caller leak (retirement is never polled), not a tracker capacity problem:
        // entries are kept (protection first) and the abuse is surfaced, throttled.
        if (entries_.size() > kMaxEntries)
        {
            ++overCapPolls_;
            if (overCapPolls_ == 1 || overCapPolls_ % 1024 == 0)
            {
                LOG_WARN("GpuBatch: {} recordings in flight exceeds cap {}; possible retirement-leak (keeping protection)",
                         entries_.size(), kMaxEntries);
            }
        }
        hasEntries_.store(!entries_.empty(), std::memory_order_release);
    }

    // Soft cap used only for the in-flight leak diagnostic; never a reclamation trigger.
    static constexpr size_t kMaxEntries = 1024;

    // Published under mutex_; the idle submission path needs only this atomic read.
    std::atomic<bool> hasEntries_ { false };
    std::shared_ptr<RecordingObserver> observer_; // shared with proxies via ObserveResources
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::vector<std::pair<ID3D12CommandQueue*, Microsoft::WRL::ComPtr<ID3D12Fence>>> fences_;
    uint64_t seq_ = 0;
    std::atomic<uint64_t> replaysAfterCompletion_ { 0 };
    std::atomic<uint64_t> invalidationsObserved_ { 0 };
    uint64_t overCapPolls_ = 0;
    uint64_t lastGaugeMs_ = 0;
};
} // namespace DlssNr::Submission::GpuBatch
