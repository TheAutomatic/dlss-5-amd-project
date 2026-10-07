#pragma once
#include <Windows.h>
#include <detours/detours.h>
#include <atomic>

namespace HookInit
{
enum class Stage { Ready, Busy, Resolve, Begin, UpdateThread, Attach, Commit };
struct Result { Stage stage; LONG error; };
inline const char* StageName(Stage stage)
{
    switch (stage)
    {
    case Stage::Begin: return "DetourTransactionBegin";
    case Stage::UpdateThread: return "DetourUpdateThread";
    case Stage::Attach: return "DetourAttachEx";
    case Stage::Commit: return "DetourTransactionCommit";
    default: return "initialization";
    }
}
struct DetoursApi
{
    LONG Begin() { return DetourTransactionBegin(); }
    LONG UpdateThread() { return DetourUpdateThread(GetCurrentThread()); }
    LONG Attach(PVOID* target, PVOID hook, PDETOUR_TRAMPOLINE* trampoline)
    {
        return DetourAttachEx(target, hook, trampoline, nullptr, nullptr);
    }
    LONG Commit() { return DetourTransactionCommit(); }
    void Abort() { DetourTransactionAbort(); }
};

// One permanent hook on a pinned module. The owner must outlive the hook.
// Loading can re-enter Init or run under loader lock: never wait for another
// initializer. Unavailable modules and failed transactions remain retryable.
template<class Fn> class RetryableDetour
{
    enum class State { Idle, Installing, Ready };
    std::atomic<State> state {State::Idle};
    std::atomic<Fn> original {nullptr};
public:
    // Only the installed detour calls Original; a prepared trampoline is also
    // visible during Commit so the newly patched entry can forward immediately.
    Fn Original() const { return original.load(std::memory_order_acquire); }
    bool Installed() const { return state.load(std::memory_order_acquire) == State::Ready; }

    template<class Resolve, class Api = DetoursApi>
    Result TryInstall(Resolve resolve, Fn hook, Api api = {})
    {
        State expected = State::Idle;
        if (!state.compare_exchange_strong(expected, State::Installing, std::memory_order_acq_rel))
            return expected == State::Ready ? Result {Stage::Ready, NO_ERROR}
                                           : Result {Stage::Busy, ERROR_BUSY};
        struct Attempt
        {
            RetryableDetour& owner;
            bool success = false;
            ~Attempt()
            {
                if (!success) owner.original.store(nullptr, std::memory_order_release);
                owner.state.store(success ? State::Ready : State::Idle, std::memory_order_release);
            }
        } attempt {*this};

        // Only Detours reads/writes this transaction-local pointer. Never give
        // Detours an atomic's storage or a variable concurrently read by a hook.
        Fn target = resolve();
        if (!target) return {Stage::Resolve, ERROR_PROC_NOT_FOUND};
        LONG error = api.Begin();
        if (error != NO_ERROR)
        {
            // Detours rejects an existing owner with ERROR_INVALID_OPERATION.
            // Other Begin failures occur after it acquires ownership (making
            // trampoline pages writable); release that failed transaction.
            if (error != ERROR_INVALID_OPERATION) api.Abort();
            return {Stage::Begin, error};
        }
        struct Transaction
        {
            Api& api;
            bool owned = true;
            ~Transaction() { if (owned) api.Abort(); }
        } transaction {api};
        error = api.UpdateThread();
        if (error != NO_ERROR) return {Stage::UpdateThread, error};
        PDETOUR_TRAMPOLINE trampoline = nullptr;
        error = api.Attach(reinterpret_cast<PVOID*>(&target), reinterpret_cast<PVOID>(hook), &trampoline);
        if (error != NO_ERROR) return {Stage::Attach, error};
        if (!trampoline) return {Stage::Attach, ERROR_INVALID_ADDRESS};

        // AttachEx prepares the callable trampoline before Commit redirects the
        // entry. Publish first: another thread can enter our hook during Commit,
        // before this initializer returns. Never publish the soon-to-be-hooked
        // entry address itself (forwarding to it would recurse).
        original.store(reinterpret_cast<Fn>(trampoline), std::memory_order_release);
        error = api.Commit();
        transaction.owned = false; // Commit ends/rolls back our transaction, even on failure.
        if (error != NO_ERROR) return {Stage::Commit, error};
        attempt.success = true;
        return {Stage::Commit, NO_ERROR};
    }
};
} // namespace HookInit
