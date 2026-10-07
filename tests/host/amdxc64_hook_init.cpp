// Real Detours on synthetic functions; no driver, D3D device or game is loaded.
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/hooks/RetryableDetour.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

using Fn = int(__cdecl*)(int);
using HookInit::Stage;
static std::array<HookInit::RetryableDetour<Fn>, 9> hooks;
static void Require(bool ok, const char* text)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); }
}
template<int I> __declspec(noinline) int Target(int value)
{
    volatile int bias = I + 10;
    return value + bias;
}
template<int I> __declspec(noinline) int Hook(int value)
{
    auto original = hooks[I].Original();
    Require(original != nullptr, "callback always has a published trampoline");
    return original(value) + 1000;
}
enum class Fail { None, Begin, Update, Attach, Commit };
struct Stats { int begin = 0, update = 0, attach = 0, commit = 0, abort = 0; };
struct Api : HookInit::DetoursApi
{
    Stats* stats;
    Fail fail = Fail::None;
    void (*afterCommit)() = nullptr;
    LONG Begin()
    {
        ++stats->begin;
        const auto rc = DetoursApi::Begin();
        return rc == NO_ERROR && fail == Fail::Begin ? ERROR_ACCESS_DENIED : rc;
    }
    LONG UpdateThread()
    {
        ++stats->update;
        return fail == Fail::Update ? ERROR_ACCESS_DENIED : DetoursApi::UpdateThread();
    }
    LONG Attach(PVOID* target, PVOID hook, PDETOUR_TRAMPOLINE* trampoline)
    {
        ++stats->attach;
        const auto rc = DetoursApi::Attach(target, hook, trampoline);
        // Fail with a real pending attachment, so rollback must free it.
        return rc == NO_ERROR && fail == Fail::Attach ? ERROR_INVALID_BLOCK : rc;
    }
    LONG Commit()
    {
        ++stats->commit;
        if (fail == Fail::Commit)
        {
            // Poison the real pending transaction, then let real Commit roll
            // back the already prepared attachment and reclaim its trampoline.
            PVOID missing = nullptr;
            Require(DetourAttach(&missing, reinterpret_cast<PVOID>(&Hook<3>)) == ERROR_INVALID_HANDLE,
                    "inject pending transaction error");
            return DetoursApi::Commit();
        }
        const auto rc = DetoursApi::Commit();
        if (rc == NO_ERROR && afterCommit) afterCommit();
        return rc;
    }
    void Abort() { ++stats->abort; DetoursApi::Abort(); }
};
template<int I> void Verify()
{
    Require(hooks[I].Installed(), "successful hook marked ready");
    auto target = &Target<I>;
    Require(target(7) == 1017 + I, "hook forwards exactly once");
    Require(hooks[I].Original()(7) == 17 + I, "trampoline bypasses hook");
}
static void CommitWindow()
{
    Require(!hooks[0].Installed(), "probe is before ready publication");
    auto target = &Target<0>;
    Require(target(5) == 1015, "newly patched entry works before Commit returns to caller");
    const auto nested = hooks[0].TryInstall([] { return &Target<0>; }, &Hook<0>);
    Require(nested.stage == Stage::Busy, "commit re-entry cannot attach twice");
}
template<int I> void FailureRetry(Fail fail, Stage stage)
{
    Stats stats;
    const auto failed = hooks[I].TryInstall([] { return &Target<I>; }, &Hook<I>, Api {{}, &stats, fail});
    Require(failed.stage == stage && failed.error != NO_ERROR, "failure stage preserved");
    Require(!hooks[I].Installed() && !hooks[I].Original(), "failed attempt not published as ready");
    auto target = &Target<I>;
    Require(target(7) == 17 + I, "failed transaction leaves target unchanged");
    Require(stats.abort == (fail == Fail::Commit ? 0 : 1), "only an owned live transaction is aborted");
    if (fail == Fail::Begin) Require(stats.update == 0 && stats.attach == 0 && stats.commit == 0,
                                    "failed Begin is cleaned up before retry");
    if (fail == Fail::Update) Require(stats.attach == 0 && stats.commit == 0, "stop after UpdateThread error");
    if (fail == Fail::Attach) Require(stats.commit == 0, "stop after Attach error");
    const auto retried = hooks[I].TryInstall([] { return &Target<I>; }, &Hook<I>);
    Require(retried.error == NO_ERROR, "transaction failure is retryable");
    Verify<I>();
}
static Fn foreignOriginal = Target<6>;
__declspec(noinline) static int ForeignHook(int value) { return foreignOriginal(value) + 2000; }

int main()
{
    Stats first;
    const auto unavailable = hooks[0].TryInstall([]() -> Fn { return nullptr; }, &Hook<0>, Api {{}, &first});
    Require(unavailable.stage == Stage::Resolve && first.begin == 0, "missing module opens no transaction");
    Require(!hooks[0].Installed() && !hooks[0].Original(), "missing module remains retryable");
    const auto ready = hooks[0].TryInstall([] { return &Target<0>; }, &Hook<0>,
                                         Api {{}, &first, Fail::None, &CommitWindow});
    Require(ready.error == NO_ERROR && first.commit == 1, "late module attaches once");
    Verify<0>();
    const auto again = hooks[0].TryInstall([]() -> Fn {
        Require(false, "ready hook must not resolve or rewrite its pointer"); return nullptr;
    }, &Hook<0>, Api {{}, &first});
    Require(again.stage == Stage::Ready && first.begin == 1, "duplicate Init is inert");
    FailureRetry<1>(Fail::Update, Stage::UpdateThread);
    FailureRetry<2>(Fail::Attach, Stage::Attach);
    FailureRetry<3>(Fail::Commit, Stage::Commit);
    FailureRetry<8>(Fail::Begin, Stage::Begin);

    // Neither same-thread nesting nor another thread may join, commit or abort
    // a transaction owned by an unrelated hook. Its pending patch must survive.
    Require(DetourTransactionBegin() == NO_ERROR, "foreign begin");
    Require(DetourUpdateThread(GetCurrentThread()) == NO_ERROR, "foreign update");
    Require(DetourAttach(reinterpret_cast<PVOID*>(&foreignOriginal), reinterpret_cast<PVOID>(&ForeignHook)) == NO_ERROR,
            "foreign attach");
    Stats foreign;
    auto rejected = [&] {
        const auto result = hooks[4].TryInstall([] { return &Target<4>; }, &Hook<4>, Api {{}, &foreign});
        Require(result.stage == Stage::Begin && result.error == ERROR_INVALID_OPERATION,
                "foreign transaction rejected at Begin");
    };
    rejected();
    std::thread other(rejected); other.join();
    Require(foreign.begin == 2 && foreign.update == 0 && foreign.attach == 0 &&
            foreign.commit == 0 && foreign.abort == 0, "foreign transaction is untouched");
    Require(DetourTransactionCommit() == NO_ERROR, "foreign owner can still commit");
    auto foreignTarget = &Target<6>;
    Require(foreignTarget(7) == 2023, "foreign attachment preserved");
    Require(hooks[4].TryInstall([] { return &Target<4>; }, &Hook<4>).error == NO_ERROR,
            "retry after foreign transaction succeeds");
    Verify<4>();

    try
    {
        hooks[5].TryInstall([]() -> Fn { throw std::runtime_error("load failed"); }, &Hook<5>);
        Require(false, "resolver exception expected");
    }
    catch (const std::runtime_error&) {}
    Require(hooks[5].TryInstall([] { return &Target<5>; }, &Hook<5>).error == NO_ERROR,
            "unwinding releases initializer ownership");
    Verify<5>();

    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE proceed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Require(entered && proceed, "race events");
    Stats race;
    std::thread owner([&] {
        const auto result = hooks[7].TryInstall([&] {
            const auto recursive = hooks[7].TryInstall([] { return &Target<7>; }, &Hook<7>);
            Require(recursive.stage == Stage::Busy, "loader re-entry returns without waiting");
            SetEvent(entered);
            Require(WaitForSingleObject(proceed, 10000) == WAIT_OBJECT_0, "race completion gate");
            return &Target<7>;
        }, &Hook<7>, Api {{}, &race});
        Require(result.error == NO_ERROR, "initializing thread finishes");
    });
    Require(WaitForSingleObject(entered, 10000) == WAIT_OBJECT_0, "owner enters resolver");
    std::vector<std::thread> contenders;
    for (int i = 0; i < 12; ++i) contenders.emplace_back([] {
        for (int j = 0; j < 100; ++j)
            Require(hooks[7].TryInstall([] { return &Target<7>; }, &Hook<7>).stage == Stage::Busy,
                    "contending Init never blocks or changes state");
    });
    for (auto& thread : contenders) thread.join();
    SetEvent(proceed); owner.join();
    CloseHandle(entered); CloseHandle(proceed);
    Require(race.begin == 1 && race.attach == 1 && race.commit == 1 && race.abort == 0, "one attachment for all callers");
    contenders.clear();
    for (int i = 0; i < 12; ++i) contenders.emplace_back([] {
        for (int j = 0; j < 1000; ++j)
        {
            Verify<7>();
            Require(hooks[7].TryInstall([] { return &Target<7>; }, &Hook<7>).stage == Stage::Ready,
                    "initialized concurrent calls cannot clear the trampoline");
        }
    });
    for (auto& thread : contenders) thread.join();
    std::puts("amdxc64 hook init: PASS (real Detours, concurrent/reentrant Init, retry, foreign transactions, commit-window forwarding)");
}
