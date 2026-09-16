// Host-only tests. Does not load an author runtime or use a GPU.
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/RuntimeHostLoad.h"
#include <cstdio>

using namespace AmdPreSr::RuntimeHostLoad;
static void Expect(bool result, const char* text)
{
    if (!result) throw std::runtime_error(text);
}

static DWORD WINAPI Worker(void* data)
{
    InterlockedIncrement(static_cast<volatile LONG*>(data));
    return 42;
}

static void ForwardedThread()
{
    volatile LONG calls = 0;
    DWORD threadId = 0;
    HANDLE thread = CreateThread(nullptr, 0, Worker, const_cast<LONG*>(&calls), CREATE_SUSPENDED, &threadId);
    Expect(thread && threadId, "unrelated CreateThread must return its real HANDLE and thread ID");
    Expect(WaitForSingleObject(thread, 0) == WAIT_TIMEOUT && calls == 0, "CREATE_SUSPENDED must be preserved");
    Expect(ResumeThread(thread) == 1, "suspension count must be preserved");
    Expect(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "forwarded worker must finish");
    DWORD exitCode = 0;
    Expect(GetExitCodeThread(thread, &exitCode) && exitCode == 42 && calls == 1,
           "worker entry, argument, and exit code must be preserved");
    CloseHandle(thread);
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        constexpr std::uintptr_t image = 0x180000000;
        for (const auto& known : Detail::bootstraps)
        {
            const auto start = image + known.startRva;
            const auto ret = image + known.returnRva;
            Expect(&Detail::Select(known.layout) == &known, "known runtime contract selection");
            Expect(Detail::MatchBootstrap(known, image, start, ret, known.callBytes.data(), Detail::startBytes.data()) ==
                   Detail::Match::Verified, "exact known bootstrap must match");
            Expect(Detail::MatchBootstrap(known, image, start + 1, ret + 1, nullptr, nullptr) ==
                   Detail::Match::Unrelated, "unrelated worker must not inspect or require instruction bytes");
            Expect(Detail::MatchBootstrap(known, image, start + 1, ret, known.callBytes.data(), Detail::startBytes.data()) ==
                   Detail::Match::Invalid, "changed thread entry must reject a known bootstrap caller");
            Expect(Detail::MatchBootstrap(known, image, start, ret + 1, known.callBytes.data(), Detail::startBytes.data()) ==
                   Detail::Match::Invalid, "changed caller must reject a known bootstrap entry");
            auto badCall = known.callBytes;
            badCall.back() ^= 1;
            Expect(Detail::MatchBootstrap(known, image, start, ret, badCall.data(), Detail::startBytes.data()) ==
                   Detail::Match::Invalid, "changed call instruction must reject isolation");
            auto badEntry = Detail::startBytes;
            badEntry.front() ^= 1;
            Expect(Detail::MatchBootstrap(known, image, start, ret, known.callBytes.data(), badEntry.data()) ==
                   Detail::Match::Invalid, "changed bootstrap prologue must reject isolation");
            Expect(Detail::MatchBootstrap(known, image, start, ret, nullptr, Detail::startBytes.data()) ==
                   Detail::Match::Invalid, "unreadable call bytes must reject isolation");
        }
        bool unknownRejected = false;
        try { Detail::Select(nullptr); }
        catch (const std::runtime_error&) { unknownRejected = true; }
        Expect(unknownRejected, "unknown runtimes must never use guessed bootstrap offsets");

        std::fprintf(stderr, "Checking host thread forwarding...\n");
        Detail::Install();
        Detail::Install(); // a second pass does not stack another detour
        ForwardedThread();
        Detail::Loading active { &Detail::bootstraps[2], L"C:\\not-an-author-runtime.dll", 27 };
        Detail::loading = &active;
        ForwardedThread();
        Detail::loading = nullptr;
        Expect(!active.invalid && active.suppressedCount == 0 && !active.suppressedBase,
               "unrelated creation inside an armed loading scope must remain untouched");
        if (argc > 1)
        {
            std::fprintf(stderr, "Checking synthetic DLL loader interception...\n");
            if (argc > 2 && std::wcscmp(argv[2], L"--expect-isolation-failure") == 0)
            {
                bool diagnosed = false;
                try { Load(argv[1], &AmdPreSr::kAmd031); }
                catch (const std::runtime_error& error)
                {
                    const std::string message = error.what();
                    diagnosed = message.find("tlsArmed=0") != std::string::npos &&
                                message.find("suppressed=0") != std::string::npos &&
                                message.find("loadedPath='") != std::string::npos &&
                                message.find("invalidMask=0x0") != std::string::npos;
                    std::fprintf(stderr, "Expected loader diagnostic: %s\n", error.what());
                }
                Expect(diagnosed, "missing bootstrap must fail with counts, path and invalid-reason diagnostics");
                std::puts("PASS: absent-bootstrap diagnostics after LoadLibrary returned");
                return 0;
            }
            // Explicitly a generated test image, not an author runtime. The
            // production caller verifies SHA before calling this helper.
            Expect(Load(argv[1], &AmdPreSr::kAmd031) != nullptr,
                   "synthetic DllMain bootstrap must be suppressed before execution");
            bool duplicateRejected = false;
            try { Load(argv[1], &AmdPreSr::kAmd031); }
            catch (const std::runtime_error&) { duplicateRejected = true; }
            Expect(duplicateRejected, "an already-loaded image cannot bypass bootstrap verification");
            std::puts("PASS: synthetic DLL loader interception; mapped image identity; duplicate-load rejection");
        }
        std::puts("PASS: three pinned bootstrap filters; unknown rejection; unchanged host CreateThread forwarding");
        return 0;
    }
    catch (const std::exception& error)
    {
        Detail::loading = nullptr;
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
