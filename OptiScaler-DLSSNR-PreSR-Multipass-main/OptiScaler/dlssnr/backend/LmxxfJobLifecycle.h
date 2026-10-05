#pragma once
#include "../submission/ListGenerationToken.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace DlssNr::Backend
{
// CPU ownership of the single runtime job. No transition waits, invokes a
// callback, or proves GPU completion. The caller performs work between claims.
class JobLifecycle
{
  public:
    enum class Phase { Idle, Recording, Armed, Submitting, Enqueueing, EnqueueCompleted, Completing, Cancelling, Closing, Closed };
    struct Work
    {
        uint64_t token = 0;
        void *job = nullptr;
        void *cmd = nullptr;
        Submission::ListGenerationSnapshot generation {};
    };
    struct Observation
    {
        Phase phase = Phase::Idle;
        std::optional<Work> work;
        std::thread::id owner;
        bool stopping = false;
    };

    bool BeginRecord()
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || phase_ != Phase::Idle) return false;
        phase_ = Phase::Recording;
        owner_ = std::this_thread::get_id();
        return true;
    }

    void EndRecord()
    {
        std::lock_guard lock(mutex_);
        if (phase_ == Phase::Recording) SetIdle();
    }

    uint64_t Publish(void *job, void *cmd, Submission::ListGenerationSnapshot generation = {})
    {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::Recording || !job || !cmd) return 0;
        // A shutdown request cannot revoke an already admitted Record: its
        // commands may already reference runtime resources and need retirement.
        if (++nextToken_ == 0) ++nextToken_;
        work_ = {nextToken_, job, cmd, std::move(generation)};
        phase_ = Phase::Armed;
        owner_ = {};
        return work_.token;
    }

    // Claim before producer submission, so a concurrent discard cannot cancel
    // the job during Execute's producer-to-Between interval.
    bool BeginSubmission(const Work &expected)
    {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::Armed || !Matches(expected.token) ||
            work_.job != expected.job || work_.cmd != expected.cmd || work_.generation.Discarded()) return false;
        phase_ = Phase::Submitting;
        owner_ = std::this_thread::get_id();
        return true;
    }

    // Only an explicit failed Execute attempt that submitted no producer may
    // revoke admission. A missing Between callback alone does not prove this.
    bool RejectSubmission(const Work &expected)
    {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::Submitting || !Matches(expected.token) ||
            work_.job != expected.job || work_.cmd != expected.cmd) return false;
        phase_ = Phase::Armed;
        owner_ = {};
        return true;
    }

    bool BeginEnqueue(uint64_t token)
    {
        std::lock_guard lock(mutex_);
        // Direct CPU harnesses may enter the between slot without an outer
        // submission wrapper; product callers first claim Submitting.
        if ((phase_ != Phase::Armed && phase_ != Phase::Submitting) || !Matches(token)) return false;
        if (phase_ == Phase::Armed && work_.generation.Discarded()) return false;
        phase_ = Phase::Enqueueing;
        owner_ = std::this_thread::get_id();
        return true;
    }

    bool EndEnqueue(uint64_t token)
    {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::Enqueueing || !Matches(token)) return false;
        phase_ = Phase::EnqueueCompleted;
        owner_ = {};
        return true;
    }

    // Only the outer submission, after its continuation submit, may claim this.
    // EnqueueCompleted alone is not evidence that the consumer was submitted.
    std::optional<Work> BeginCompletion(const Work &expected)
    {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::EnqueueCompleted || !Matches(expected.token) ||
            work_.job != expected.job || work_.cmd != expected.cmd) return std::nullopt;
        phase_ = Phase::Completing;
        owner_ = std::this_thread::get_id();
        return work_;
    }

    // Legacy test callers only. Product submission captures Work before Execute
    // and uses the token-aware overload so a late callback cannot claim reused pointers.
    std::optional<Work> BeginCompletion(void *cmd)
    {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::EnqueueCompleted || !cmd || work_.cmd != cmd) return std::nullopt;
        phase_ = Phase::Completing;
        owner_ = std::this_thread::get_id();
        return work_;
    }

    // The caller must first establish that this list generation was discarded.
    // Elapsed time and skipped Evaluate calls are not cancellation credentials.
    std::optional<Work> BeginCancelIfArmed(void *cmd)
    {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::Armed || !cmd || work_.cmd != cmd) return std::nullopt;
        phase_ = Phase::Cancelling;
        owner_ = std::this_thread::get_id();
        return work_;
    }

    bool EndOperation(const Work &work)
    {
        std::lock_guard lock(mutex_);
        if ((phase_ != Phase::Completing && phase_ != Phase::Cancelling) ||
            !Matches(work.token) || work_.job != work.job || work_.cmd != work.cmd) return false;
        SetIdle();
        return true;
    }

    bool BeginShutdown()
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        if (phase_ != Phase::Idle) return false;
        phase_ = Phase::Closing;
        owner_ = std::this_thread::get_id();
        return true;
    }

    bool EndShutdown()
    {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::Closing) return false;
        phase_ = Phase::Closed;
        owner_ = {};
        return true;
    }

    Phase Current() const
    {
        std::lock_guard lock(mutex_);
        return phase_;
    }

    bool Stopping() const
    {
        std::lock_guard lock(mutex_);
        return stopping_;
    }

    Observation Observe() const
    {
        std::lock_guard lock(mutex_);
        return {phase_, work_.token ? std::optional<Work>(work_) : std::nullopt, owner_, stopping_};
    }

    // An observation only. Mutating callers must still claim the transition.
    std::optional<Work> Active() const
    {
        std::lock_guard lock(mutex_);
        if (!work_.token) return std::nullopt;
        return work_;
    }

    std::thread::id Owner() const
    {
        std::lock_guard lock(mutex_);
        return owner_;
    }

  private:
    bool Matches(uint64_t token) const { return token != 0 && work_.token == token; }
    void SetIdle()
    {
        work_ = {};
        phase_ = Phase::Idle;
        owner_ = {};
    }

    mutable std::mutex mutex_;
    Phase phase_ = Phase::Idle;
    Work work_ {};
    uint64_t nextToken_ = 0;
    bool stopping_ = false;
    std::thread::id owner_ {};
};

inline bool ConsumeTemporalReset(bool gameReset, std::atomic<bool> &pending)
{
    const bool requested = pending.exchange(false, std::memory_order_acq_rel);
    return gameReset || requested;
}
} // namespace DlssNr::Backend
