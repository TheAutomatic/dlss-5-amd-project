#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfJobLifecycle.h"
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>

using DlssNr::Backend::ConsumeTemporalReset;
using DlssNr::Backend::JobLifecycle;
using Phase = JobLifecycle::Phase;
using namespace std::chrono_literals;

static void Require(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::abort();
    }
}

template<class T>
static T Await(std::future<T> &future, const char *message)
{
    Require(future.wait_for(5s) == std::future_status::ready, message);
    return future.get();
}

static uint64_t Arm(JobLifecycle &life, void *job, void *cmd)
{
    Require(life.BeginRecord(), "Record admitted from idle");
    const auto token = life.Publish(job, cmd);
    Require(token != 0, "Record publishes nonzero token");
    life.EndRecord();
    Require(life.Current() == Phase::Armed, "Record scope cannot clear published work");
    return token;
}

static void BlockedEnqueue()
{
    JobLifecycle life;
    int job, cmd, unrelated;
    const auto token = Arm(life, &job, &cmd);
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future();
    auto enqueue = std::async(std::launch::async, [&] {
        Require(life.BeginEnqueue(token), "enqueue claims armed work");
        Require(life.Owner() == std::this_thread::get_id(), "enqueue records owner");
        entered.set_value();
        Await(releaseFuture, "enqueue gate released");
        return life.EndEnqueue(token);
    });
    Await(enteredFuture, "enqueue reached gate");
    // Reproduce the old eight-Evaluate recovery window while HIP is in flight.
    for (int i = 0; i < 32; ++i)
    {
        Require(!life.BeginRecord(), "blocked enqueue cannot admit another Record");
        Require(!life.BeginCompletion(&cmd), "consuming an enqueue is not completion");
        Require(!life.BeginCompletion(&unrelated), "unrelated nested submission ignored");
        Require(!life.BeginCancelIfArmed(&cmd), "in-flight enqueue cannot be cancelled");
        Require(life.Active()->token == token, "in-flight ownership survives Evaluate attempts");
    }
    Require(!life.BeginEnqueue(token), "reentrant enqueue cannot claim twice");
    Require(!life.EndEnqueue(token + 1), "wrong token cannot finish enqueue");
    release.set_value();
    Require(Await(enqueue, "enqueue completed"), "enqueue publishes completion");
    Require(life.Current() == Phase::EnqueueCompleted, "HIP returned before outer retirement");
    Require(!life.BeginRecord(), "completed enqueue still owns consumer resources");
    Require(!life.BeginCancelIfArmed(&cmd), "completed enqueue cannot be cancelled");
    const auto work = life.BeginCompletion(&cmd);
    Require(work.has_value(), "outer submission claims retirement");
    Require(life.EndOperation(*work), "retirement releases job after cleanup");
}

static void ProducerSubmissionReservation()
{
    JobLifecycle life;
    int job, cmd, unrelated;
    const auto token = Arm(life, &job, &cmd);
    const auto expected = *life.Active();
    auto wrong = expected;
    wrong.cmd = &unrelated;
    Require(!life.BeginSubmission(wrong), "producer claim validates command identity");
    wrong = expected;
    wrong.job = &unrelated;
    Require(!life.BeginSubmission(wrong), "producer claim validates job identity");
    Require(life.BeginSubmission(expected), "outer batch reserves producer submission");
    const auto observed = life.Observe();
    Require(observed.phase == Phase::Submitting && observed.work && observed.work->token == token &&
            observed.owner == std::this_thread::get_id() && !observed.stopping,
            "one observation captures coherent submitting state");
    for (int i = 0; i < 32; ++i)
    {
        Require(!life.BeginRecord(), "producer submission blocks new Record");
        Require(!life.BeginCancelIfArmed(&cmd), "accepted submission cannot be reclaimed as discarded");
    }
    Require(!life.BeginSubmission(expected), "duplicate batch cannot claim producer twice");
    Require(!life.BeginCompletion(expected), "producer admission is not consumer completion");
    Require(!life.EndEnqueue(token), "producer admission is not an enqueue callback");
    Require(life.BeginEnqueue(token) && life.EndEnqueue(token), "between slot advances admitted submission");
    const auto work = life.BeginCompletion(expected);
    Require(work.has_value() && life.EndOperation(*work), "original batch completes after producer reservation");
    const auto idle = life.Observe();
    Require(idle.phase == Phase::Idle && !idle.work && idle.owner == std::thread::id{} && !idle.stopping,
            "idle observation drops old work and owner");

    const auto nextToken = Arm(life, &job, &cmd);
    Require(nextToken != token && !life.BeginSubmission(expected), "old batch cannot submit reused pointers");
    Require(life.Current() == Phase::Armed, "stale producer claim leaves new recording armed");
    const auto next = *life.Active();
    Require(life.BeginSubmission(next), "fresh batch claims reused pointers");
    Require(!life.BeginShutdown(), "shutdown cannot revoke accepted producer submission");
    const auto stopped = life.Observe();
    Require(stopped.phase == Phase::Submitting && stopped.work && stopped.work->token == nextToken &&
            stopped.owner == std::this_thread::get_id() && stopped.stopping,
            "stop observation retains producer owner and token");
    Require(life.BeginEnqueue(nextToken) && life.EndEnqueue(nextToken), "accepted submission finishes after stop");
    const auto finishing = life.BeginCompletion(next);
    Require(finishing.has_value() && life.EndOperation(*finishing), "stopped original batch retires");
    Require(life.BeginShutdown() && life.EndShutdown(), "shutdown follows completed producer submission");
}

static void BlockedRetireAndShutdown()
{
    JobLifecycle life;
    int job, cmd, unrelated;
    const auto token = Arm(life, &job, &cmd);
    Require(life.BeginEnqueue(token) && life.EndEnqueue(token), "enqueue before retirement");
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future();
    auto retire = std::async(std::launch::async, [&] {
        const auto work = life.BeginCompletion(&cmd);
        Require(work.has_value(), "retirement claimed");
        Require(life.Owner() == std::this_thread::get_id(), "retirement records owner");
        // Simulate same-thread Execute -> Submitted inside a runtime operation.
        Require(!life.BeginCompletion(&unrelated), "nested unrelated Submitted returns");
        Require(!life.BeginCompletion(&cmd), "nested duplicate Submitted returns");
        entered.set_value();
        Await(releaseFuture, "retirement gate released");
        return life.EndOperation(*work);
    });
    Await(enteredFuture, "retirement reached gate");
    Require(!life.BeginRecord(), "Retire retains ownership before clear");
    Require(!life.BeginShutdown(), "shutdown cannot destroy a retiring session");
    Require(life.Stopping(), "busy shutdown closes admission immediately");
    Require(life.Current() == Phase::Completing, "shutdown preserves retirement phase");
    Require(life.Active()->token == token, "shutdown preserves retirement identity");
    release.set_value();
    Require(Await(retire, "retirement completed"), "retirement can finish after stop request");
    Require(!life.BeginRecord(), "stopping remains latched after retirement");
    Require(life.BeginShutdown(), "quiescent shutdown can claim closing");
    Require(!life.BeginRecord() && !life.BeginShutdown(), "closing has one owner");
    Require(life.EndShutdown(), "shutdown publishes closed");
    Require(life.Current() == Phase::Closed && !life.Active(), "closed has no live work");
    Require(!life.BeginRecord() && !life.BeginShutdown() && !life.EndShutdown(), "closed cannot reopen");
}

static void RejectedProducerSubmission()
{
    JobLifecycle life;
    int job, cmd, unrelated;
    Arm(life, &job, &cmd);
    const auto original = *life.Active();
    Require(!life.RejectSubmission(original), "an armed recording has no admitted attempt to reject");
    Require(life.BeginSubmission(original), "first producer attempt admitted");
    auto wrong = original;
    wrong.job = &unrelated;
    Require(!life.RejectSubmission(wrong), "rejection validates job identity");
    wrong = original;
    wrong.cmd = &unrelated;
    Require(!life.RejectSubmission(wrong), "rejection validates command identity");
    Require(life.RejectSubmission(original), "proven pre-producer failure releases submission claim");
    const auto rejected = life.Observe();
    Require(rejected.phase == Phase::Armed && rejected.work && rejected.work->token == original.token &&
            rejected.owner == std::thread::id{}, "rejected attempt preserves armed work without an execution owner");
    Require(life.BeginSubmission(original), "same recording may retry a rejected submission");
    Require(life.RejectSubmission(original), "second proven pre-producer rejection releases retry");
    const auto discarded = life.BeginCancelIfArmed(&cmd);
    Require(discarded.has_value() && life.EndOperation(*discarded), "successful Reset may cancel after rejected submission");

    const auto token = Arm(life, &job, &cmd);
    const auto current = *life.Active();
    Require(token != original.token && life.BeginSubmission(current), "new generation begins its own attempt");
    Require(!life.RejectSubmission(original), "late old rejection cannot revoke a new generation");
    Require(life.Current() == Phase::Submitting, "old rejection leaves current producer reservation intact");
    Require(life.BeginEnqueue(token), "current generation enters HIP callback");
    Require(!life.RejectSubmission(current), "enqueueing is too late to reject producer submission");
    Require(life.EndEnqueue(token), "HIP callback completes");
    Require(!life.RejectSubmission(current), "enqueue completion cannot be rejected as pre-producer");
    const auto completing = life.BeginCompletion(current);
    Require(completing.has_value(), "current generation claims outer completion");
    Require(!life.RejectSubmission(current), "retirement cannot be revoked by rejection");
    Require(life.EndOperation(*completing), "current generation completes normally");
}

static void DiscardedSubmissionGeneration()
{
    JobLifecycle life;
    int job, cmd;
    auto generation = std::make_shared<DlssNr::Submission::ListGenerationToken>();
    Require(life.BeginRecord(), "recording with generation admitted");
    const auto oldToken = life.Publish(&job, &cmd, {generation, 0});
    Require(oldToken != 0, "published work carries recording generation");
    life.EndRecord();
    const auto oldWork = *life.Active();
    Require(oldWork.generation.token == generation && !oldWork.generation.Discarded(),
            "active work retains its original generation credential");
    // The game resets a discarded NR list, then submits its new unsplit contents
    // without another NR Record. Pointer equality must not claim the old job.
    generation->generation.fetch_add(1, std::memory_order_release);
    Require(oldWork.generation.Discarded(), "successful Reset discards original recording");
    Require(!life.BeginSubmission(oldWork), "new list generation cannot submit discarded NR job");
    Require(!life.BeginEnqueue(oldToken), "direct between entry cannot revive discarded recording");
    Require(life.Current() == Phase::Armed, "discarded submission stays cancellable instead of stuck Submitting");
    const auto discarded = life.BeginCancelIfArmed(&cmd);
    Require(discarded.has_value() && life.EndOperation(*discarded), "next Record can reclaim verified discard");

    Require(life.BeginRecord(), "Record recovers after discarded list submission");
    const auto token = life.Publish(&job, &cmd, {generation, 1});
    life.EndRecord();
    const auto current = *life.Active();
    Require(token != oldToken && life.BeginSubmission(current), "new generation can submit normally");
    Require(life.BeginEnqueue(token) && life.EndEnqueue(token), "new generation executes HIP");
    const auto completion = life.BeginCompletion(current);
    Require(completion.has_value() && life.EndOperation(*completion), "new generation completes normally");

    Require(life.BeginRecord(), "record before final proxy release");
    const auto releasedToken = life.Publish(&job, &cmd, {generation, 1});
    life.EndRecord();
    const auto released = *life.Active();
    generation->destroyed.store(true, std::memory_order_release);
    Require(!life.BeginSubmission(released) && !life.BeginEnqueue(releasedToken),
            "reused proxy address cannot revive a destroyed recording");
    const auto cancelled = life.BeginCancelIfArmed(&cmd);
    Require(cancelled.has_value() && life.EndOperation(*cancelled), "final release remains a discard credential");
}

static void TokensAndCancellation()
{
    JobLifecycle life;
    int job, cmd, unrelated;
    Require(!life.BeginEnqueue(0) && !life.EndEnqueue(0), "zero token is never active");
    Require(!life.EndOperation({}) && !life.EndShutdown(), "idle cannot be completed as work");
    const auto oldToken = Arm(life, &job, &cmd);
    Require(!life.BeginCompletion(&cmd), "armed work has no submission evidence");
    Require(!life.BeginCancelIfArmed(&unrelated), "discard requires matching command list");
    const auto oldWork = life.BeginCancelIfArmed(&cmd);
    Require(oldWork.has_value(), "confirmed discard can claim armed work");
    Require(!life.BeginEnqueue(oldToken) && !life.BeginRecord(), "cancellation owns resources until cleanup");
    Require(!life.BeginCancelIfArmed(&cmd), "discard cannot claim twice");
    Require(life.EndOperation(*oldWork), "cancel cleanup finishes");

    // The actual runtime reuses both pointers; only the token distinguishes jobs.
    const auto newToken = Arm(life, &job, &cmd);
    Require(newToken != oldToken, "reused addresses receive a fresh token");
    Require(!life.BeginEnqueue(oldToken), "stale enqueue cannot claim new work");
    Require(life.BeginEnqueue(newToken) && life.EndEnqueue(newToken), "new token can enqueue");
    Require(!life.BeginCompletion(*oldWork), "late old Submitted cannot claim reused pointers");
    Require(life.Current() == Phase::EnqueueCompleted, "rejected old Submitted preserves new completion");
    const auto expectedNew = *life.Active();
    auto wrongExpected = expectedNew;
    wrongExpected.cmd = &unrelated;
    Require(!life.BeginCompletion(wrongExpected), "completion claim validates command identity");
    wrongExpected = expectedNew;
    wrongExpected.job = &unrelated;
    Require(!life.BeginCompletion(wrongExpected), "completion claim validates job identity");
    const auto newWork = life.BeginCompletion(expectedNew);
    Require(newWork.has_value(), "new job can retire");
    Require(!life.EndOperation(*oldWork), "late old cleanup cannot clear new retirement");
    auto wrongIdentity = *newWork;
    wrongIdentity.cmd = &unrelated;
    Require(!life.EndOperation(wrongIdentity), "cleanup validates command identity");
    wrongIdentity = *newWork;
    wrongIdentity.job = &unrelated;
    Require(!life.EndOperation(wrongIdentity), "cleanup validates job identity");
    Require(life.Active()->token == newToken, "rejected stale cleanup retains new job");
    Require(life.EndOperation(*newWork), "correct new cleanup succeeds");
    Require(!life.Active() && life.BeginRecord(), "new Record allowed after full cleanup");
    life.EndRecord();
}

static void RecordingReentryAndStop()
{
    JobLifecycle life;
    int job, cmd, unrelated;
    Require(life.BeginRecord(), "initial Record owns runtime");
    Require(life.Owner() == std::this_thread::get_id(), "Record records owner");
    Require(!life.BeginCompletion(&unrelated), "runtime internal Submitted during Record returns");
    Require(!life.BeginShutdown(), "reentrant shutdown does not wait for itself");
    Require(life.Current() == Phase::Recording, "stop request preserves admitted Record");
    Require(life.Publish(nullptr, &cmd) == 0 && life.Publish(&job, nullptr) == 0, "null work is not published");
    const auto token = life.Publish(&job, &cmd);
    Require(token != 0, "already admitted Record can publish after stop");
    life.EndRecord();
    Require(!life.BeginShutdown(), "unsubmitted armed work cannot be destroyed");
    Require(life.BeginEnqueue(token) && life.EndEnqueue(token), "stop still allows existing work to submit");
    const auto work = life.BeginCompletion(&cmd);
    Require(work.has_value() && life.EndOperation(*work), "existing submission can retire after stop");
    Require(!life.BeginRecord() && life.BeginShutdown() && life.EndShutdown(), "stop excludes only new work");

    JobLifecycle aborted;
    Require(aborted.BeginRecord() && !aborted.BeginShutdown(), "stop may arrive before failed Record");
    aborted.EndRecord();
    Require(aborted.BeginShutdown() && aborted.EndShutdown(), "failed Record releases shutdown reservation");
}

static void ResetConsumption()
{
    std::atomic<bool> pending {true};
    Require(ConsumeTemporalReset(true, pending), "game and host reset apply together");
    Require(!pending.load(), "game reset must still consume pending host reset");
    Require(!ConsumeTemporalReset(false, pending), "combined reset does not repeat next frame");
    pending.store(true);
    Require(ConsumeTemporalReset(false, pending), "host reset works independently");
    Require(ConsumeTemporalReset(true, pending), "game reset works independently");
    pending.store(true);
    Require(ConsumeTemporalReset(false, pending), "later host reset is preserved");
    Require(!ConsumeTemporalReset(false, pending), "later reset consumed once");
}

int main()
{
    BlockedEnqueue();
    ProducerSubmissionReservation();
    BlockedRetireAndShutdown();
    RejectedProducerSubmission();
    DiscardedSubmissionGeneration();
    TokensAndCancellation();
    RecordingReentryAndStop();
    ResetConsumption();
    std::cout << "lmxxf job lifecycle: PASS (CPU ownership, reentry, stop, tokens, reset)\n";
}
