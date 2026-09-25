#pragma once
#include "AmdLayout.h"
#include <windows.h>
#include <tlhelp32.h>
#include <detours/detours.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace AmdPreSr::RuntimeHostLoad
{
namespace Detail
{
struct Bootstrap
{
    const AmdLayout* layout;
    std::uintptr_t startRva;
    std::uintptr_t returnRva;
    // LEA R8, bootstrap; clear the three register arguments; CALL [CreateThread].
    std::array<unsigned char, 20> callBytes;
};

// Independently checked against each layout's pinned SHA. The following MOV
// overwrites RAX: none of these DllMain call sites uses the returned HANDLE.
inline constexpr Bootstrap bootstraps[] {
    { &kAmd0217, 0x8260, 0x6c0c,
      { 0x4c,0x8d,0x05,0x61,0x16,0,0,0x31,0xc9,0x31,0xd2,0x45,0x31,0xc9,0xff,0x15,0xe4,0xa4,0x07,0 } },
    { &kAmd03, 0x8400, 0x6cac,
      { 0x4c,0x8d,0x05,0x61,0x17,0,0,0x31,0xc9,0x31,0xd2,0x45,0x31,0xc9,0xff,0x15,0xcc,0x3b,0x08,0 } },
    { &kAmd031, 0x8630, 0x6da3,
      { 0x4c,0x8d,0x05,0x9a,0x18,0,0,0x31,0xc9,0x31,0xd2,0x45,0x31,0xc9,0xff,0x15,0xe5,0x69,0x08,0 } },
    // 0.3.2: same start/return RVAs and LEA displacement; only the CreateThread
    // IAT RIP displacement changes (0x8d788 → 0x8d748).
    { &kAmd032, 0x8630, 0x6da3,
      { 0x4c,0x8d,0x05,0x9a,0x18,0,0,0x31,0xc9,0x31,0xd2,0x45,0x31,0xc9,0xff,0x15,0xa5,0x69,0x08,0 } },
    // 0.3.3: start 0x8a10, return 0x6fa3, LEA disp 0x1a7a, CreateThread IAT 0x93740.
    { &kAmd033, 0x8a10, 0x6fa3,
      { 0x4c,0x8d,0x05,0x7a,0x1a,0,0,0x31,0xc9,0x31,0xd2,0x45,0x31,0xc9,0xff,0x15,0x9d,0xc7,0x08,0 } },
};
inline constexpr std::array<unsigned char, 16> startBytes {
    0x55,0x41,0x57,0x41,0x56,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x50,0x02,0x00
};

inline const Bootstrap& Select(const AmdLayout* layout)
{
    for (const auto& candidate : bootstraps)
        if (candidate.layout == layout)
            return candidate;
    throw std::runtime_error("AMD runtime has no verified bootstrap isolation contract");
}

enum class Match { Unrelated, Verified, Invalid };

// The hook also checks that both addresses belong to the same mapped image.
// If just one expected address matches, block it and reject the load: do not
// let a modified bootstrap run merely because its other anchor no longer fits.
inline Match MatchBootstrap(const Bootstrap& expected, std::uintptr_t image,
                            std::uintptr_t start, std::uintptr_t returnPc,
                            const unsigned char* call, const unsigned char* entry)
{
    const bool sameStart = start == image + expected.startRva;
    const bool sameReturn = returnPc == image + expected.returnRva;
    if (!sameStart && !sameReturn)
        return Match::Unrelated;
    if (!sameStart || !sameReturn || !call || !entry)
        return Match::Invalid;
    return std::memcmp(call, expected.callBytes.data(), expected.callBytes.size()) == 0 &&
                   std::memcmp(entry, startBytes.data(), startBytes.size()) == 0
               ? Match::Verified : Match::Invalid;
}

struct Loading
{
    const Bootstrap* expected;
    const wchar_t* expectedPath;
    int expectedPathLength;
    std::array<wchar_t, 32768> modulePath {};
    HMODULE suppressedBase = nullptr;
    unsigned int suppressedCount = 0;
    bool invalid = false;
    unsigned int tlsArmedCalls = 0;
    unsigned int callerQueryFailures = 0;
    unsigned int nonImageCallers = 0;
    unsigned int candidates = 0;
    unsigned int startMatches = 0;
    unsigned int returnMatches = 0;
    unsigned int pathMatches = 0;
    unsigned int pathFailures = 0;
    unsigned int invalidReasons = 0;
    unsigned int argumentMask = 0;
    DWORD candidatePathLength = 0;
    DWORD candidatePathError = 0;
    std::uintptr_t lastReturn = 0;
    std::uintptr_t lastStart = 0;
    std::uintptr_t candidateReturn = 0;
    std::uintptr_t candidateStart = 0;
    std::uintptr_t candidateBase = 0;
    std::uintptr_t candidateWorkerBase = 0;
    SIZE_T candidateStackSize = 0;
    DWORD candidateFlags = 0;
};

enum InvalidReason : unsigned int
{
    AddressMismatch = 1u << 0,
    CallBytesUnreadable = 1u << 1,
    CallBytesMismatch = 1u << 2,
    EntryBytesUnreadable = 1u << 3,
    EntryBytesMismatch = 1u << 4,
    DifferentImage = 1u << 5,
    UnexpectedArguments = 1u << 6,
    DifferentSuppressedImages = 1u << 7,
};

// Constant-initialized POD TLS. The hook never allocates, logs, reads a file,
// loads a library or takes a lock in this process while A's DllMain holds the loader lock.
inline thread_local Loading* loading = nullptr;
inline std::atomic<std::uint64_t> totalHookCalls { 0 };
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
using CreateThreadFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE,
                                      LPVOID, DWORD, LPDWORD);
inline CreateThreadFn createThreadOriginal = ::CreateThread;

inline bool Readable(const void* address, std::size_t count)
{
    MEMORY_BASIC_INFORMATION region {};
    if (!VirtualQuery(address, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const auto first = reinterpret_cast<std::uintptr_t>(address);
    const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    return first <= end && count <= end - first;
}

__declspec(noinline) inline HANDLE WINAPI CreateThreadFiltered(
    LPSECURITY_ATTRIBUTES attributes, SIZE_T stackSize, LPTHREAD_START_ROUTINE start,
    LPVOID parameter, DWORD flags, LPDWORD threadId)
{
    totalHookCalls.fetch_add(1, std::memory_order_relaxed);
    auto* context = loading;
    if (context)
    {
        ++context->tlsArmedCalls;
        const auto returnPc = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        context->lastReturn = returnPc;
        context->lastStart = reinterpret_cast<std::uintptr_t>(start);
        MEMORY_BASIC_INFORMATION caller {};
        const auto callerQuery = VirtualQuery(reinterpret_cast<const void*>(returnPc), &caller, sizeof(caller));
        if (!callerQuery)
            ++context->callerQueryFailures;
        else if (caller.Type != MEM_IMAGE)
            ++context->nonImageCallers;
        else
        {
            const auto image = reinterpret_cast<std::uintptr_t>(caller.AllocationBase);
            const auto entry = reinterpret_cast<std::uintptr_t>(start);
            const auto& expected = *context->expected;
            // Restrict inspection to a possible bootstrap. Other CRT/HIP
            // thread creation on this loading thread is forwarded unchanged.
            if (entry == image + expected.startRva || returnPc == image + expected.returnRva)
            {
                ++context->candidates;
                context->startMatches += entry == image + expected.startRva;
                context->returnMatches += returnPc == image + expected.returnRva;
                context->candidateReturn = returnPc;
                context->candidateStart = entry;
                context->candidateBase = image;
                context->candidateStackSize = stackSize;
                context->candidateFlags = flags;
                // GetModuleFileName reads the already-mapped module identity,
                // not the file. The fixed buffer was allocated before loading.
                // This extra check prevents an unrelated dependency with one
                // coincidentally equal RVA from being blocked in this TLS scope.
                const DWORD pathLength = GetModuleFileNameW(static_cast<HMODULE>(caller.AllocationBase),
                    context->modulePath.data(), static_cast<DWORD>(context->modulePath.size()));
                context->candidatePathLength = pathLength;
                context->candidatePathError = pathLength ? ERROR_SUCCESS : GetLastError();
                // Path spelling is diagnostic only. Xbox/WindowsApps aliases can
                // make GetModuleFileNameW disagree with the LoadLibrary path for
                // the same image. Forwarding here lets A's bootstrap start, then
                // Load() sees suppressedCount!=1 and NR never initializes.
                // RVA + call/prologue bytes + suppressedBase==module already bind
                // suppression to the module we are loading.
                if (!pathLength || pathLength >= context->modulePath.size() ||
                    CompareStringOrdinal(context->modulePath.data(), static_cast<int>(pathLength),
                                         context->expectedPath, context->expectedPathLength, TRUE) != CSTR_EQUAL)
                    ++context->pathFailures;
                else
                    ++context->pathMatches;
                const auto* callBytes = reinterpret_cast<const unsigned char*>(
                    image + expected.returnRva - expected.callBytes.size());
                const auto* entryBytes = reinterpret_cast<const unsigned char*>(image + expected.startRva);
                MEMORY_BASIC_INFORMATION worker {};
                const bool sameImage = VirtualQuery(reinterpret_cast<const void*>(entry), &worker, sizeof(worker)) &&
                                       worker.Type == MEM_IMAGE && worker.AllocationBase == caller.AllocationBase;
                context->candidateWorkerBase = reinterpret_cast<std::uintptr_t>(worker.AllocationBase);
                const bool callReadable = Readable(callBytes, expected.callBytes.size());
                const bool entryReadable = Readable(entryBytes, startBytes.size());
                const auto match = MatchBootstrap(expected, image, entry, returnPc,
                    callReadable ? callBytes : nullptr, entryReadable ? entryBytes : nullptr);
                if (entry != image + expected.startRva || returnPc != image + expected.returnRva)
                    context->invalidReasons |= AddressMismatch;
                if (!callReadable)
                    context->invalidReasons |= CallBytesUnreadable;
                else if (std::memcmp(callBytes, expected.callBytes.data(), expected.callBytes.size()) != 0)
                    context->invalidReasons |= CallBytesMismatch;
                if (!entryReadable)
                    context->invalidReasons |= EntryBytesUnreadable;
                else if (std::memcmp(entryBytes, startBytes.data(), startBytes.size()) != 0)
                    context->invalidReasons |= EntryBytesMismatch;
                if (!sameImage)
                    context->invalidReasons |= DifferentImage;
                context->argumentMask |= (attributes ? 1u : 0u) | (stackSize ? 2u : 0u) |
                    (parameter ? 4u : 0u) | (flags ? 8u : 0u) | (threadId ? 16u : 0u);
                if (context->argumentMask)
                    context->invalidReasons |= UnexpectedArguments;
                const auto module = static_cast<HMODULE>(caller.AllocationBase);
                if (context->suppressedBase && context->suppressedBase != module)
                    context->invalidReasons |= DifferentSuppressedImages;
                context->invalid |= !sameImage || match != Match::Verified || attributes || stackSize ||
                                    parameter || flags || threadId ||
                                    (context->suppressedBase && context->suppressedBase != module);
                context->suppressedBase = module;
                ++context->suppressedCount;
                // These three pinned callers ignore the result. Creating a
                // no-op thread instead would add needless DLL_THREAD_ATTACH.
                SetLastError(ERROR_ACCESS_DISABLED_BY_POLICY);
                return nullptr;
            }
        }
    }
    return createThreadOriginal(attributes, stackSize, start, parameter, flags, threadId);
}

struct ThreadHandles
{
    std::vector<HANDLE> values;
    ~ThreadHandles() { for (auto handle : values) CloseHandle(handle); }
};

inline void Install()
{
    static std::once_flag installed;
    std::call_once(installed, [] {
        // The process-wide forwarding hook and its trampoline stay installed.
        // Pin their owner so callbacks never target an unloaded OptiScaler/proxy DLL.
        HMODULE host {};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&CreateThreadFiltered), &host))
            throw std::runtime_error("Could not pin AMD runtime thread-filter owner");

        ThreadHandles threads;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Could not enumerate threads for AMD runtime isolation");
        THREADENTRY32 item {};
        item.dwSize = sizeof(item);
        const DWORD processId = GetCurrentProcessId();
        const DWORD currentId = GetCurrentThreadId();
        try
        {
            if (!Thread32First(snapshot, &item))
                throw std::runtime_error("Could not read thread snapshot for AMD runtime isolation");
            do
            {
                if (item.th32OwnerProcessID != processId || item.th32ThreadID == currentId)
                    continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                           THREAD_QUERY_INFORMATION, FALSE, item.th32ThreadID);
                if (!thread)
                {
                    if (GetLastError() == ERROR_INVALID_PARAMETER) // exited after the snapshot
                        continue;
                    throw std::runtime_error("Could not enlist a process thread for AMD runtime isolation");
                }
                try { threads.values.push_back(thread); }
                catch (...) { CloseHandle(thread); throw; }
            } while (Thread32Next(snapshot, &item));
            if (GetLastError() != ERROR_NO_MORE_FILES)
                throw std::runtime_error("Incomplete thread snapshot for AMD runtime isolation");
        }
        catch (...) { CloseHandle(snapshot); throw; }
        CloseHandle(snapshot);

        LONG error = DetourTransactionBegin();
        if (error != NO_ERROR)
            throw std::runtime_error("Could not begin AMD runtime isolation transaction: " + std::to_string(error));
        error = DetourUpdateThread(GetCurrentThread());
        for (auto thread : threads.values)
        {
            if (error != NO_ERROR)
                break;
            error = DetourUpdateThread(thread);
        }
        if (error == NO_ERROR)
            error = DetourAttach(reinterpret_cast<PVOID*>(&createThreadOriginal), CreateThreadFiltered);
        if (error == NO_ERROR)
            error = DetourTransactionCommit();
        else
            DetourTransactionAbort();
        if (error != NO_ERROR)
            throw std::runtime_error("Could not install AMD runtime bootstrap isolation: " + std::to_string(error));
    });
}

// Only called after LoadLibrary has returned and the loading TLS is disarmed.
// String conversion, allocation, module-path queries and exception formatting
// deliberately stay out of the CreateThread hook / loader-lock scope.
inline std::string Utf8(const wchar_t* value, int length)
{
    if (!length)
        return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return "<conversion failed>";
    std::string result(required, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, length, result.data(), required, nullptr, nullptr);
    return result;
}

inline std::string FailureDetails(const Loading& context, HMODULE module, std::uint64_t callsBefore)
{
    std::wstring loadedPath(32768, L'\0');
    const DWORD loadedLength = GetModuleFileNameW(module, loadedPath.data(), static_cast<DWORD>(loadedPath.size()));
    const DWORD loadedPathError = loadedLength ? ERROR_SUCCESS : GetLastError();
    const auto candidateLength = (std::min)(context.candidatePathLength,
                                           static_cast<DWORD>(context.modulePath.size() - 1));
    std::ostringstream message;
    message << "AMD bootstrap isolation was not verified; initialization stopped, restart required"
            << " [invalid=" << context.invalid
            << " hookCallsDelta=" << totalHookCalls.load(std::memory_order_relaxed) - callsBefore
            << " tlsArmed=" << context.tlsArmedCalls
            << " callerQueryFailed=" << context.callerQueryFailures
            << " callerNotImage=" << context.nonImageCallers
            << " candidates=" << context.candidates
            << " startMatch=" << context.startMatches << " returnMatch=" << context.returnMatches
            << " pathMatch=" << context.pathMatches << " pathFailed=" << context.pathFailures
            << " suppressed=" << context.suppressedCount
            << " baseMatch=" << (context.suppressedBase == module)
            << " invalidMask=0x" << std::hex << context.invalidReasons
            << " argMask=0x" << context.argumentMask
            << " expectedStartRva=0x" << context.expected->startRva
            << " expectedReturnRva=0x" << context.expected->returnRva
            << " lastStart=0x" << context.lastStart << " lastReturn=0x" << context.lastReturn
            << " candidateStart=0x" << context.candidateStart
            << " candidateReturn=0x" << context.candidateReturn
            << " candidateBase=0x" << context.candidateBase
            << " workerBase=0x" << context.candidateWorkerBase
            << " suppressedBase=" << static_cast<const void*>(context.suppressedBase)
            << " loadedBase=" << static_cast<const void*>(module)
            << " stackSize=0x" << context.candidateStackSize << " flags=0x" << context.candidateFlags
            << std::dec << " candidatePathError=" << context.candidatePathError
            << " loadedPathError=" << loadedPathError
            << " expectedPath='" << Utf8(context.expectedPath, context.expectedPathLength)
            << "' candidatePath='" << Utf8(context.modulePath.data(), static_cast<int>(candidateLength))
            << "' loadedPath='" << Utf8(loadedPath.data(), static_cast<int>((std::min)(loadedLength,
                                                    static_cast<DWORD>(loadedPath.size() - 1)))) << "']";
    return message.str();
}
} // namespace Detail

inline unsigned int lastPathFailures = 0;
inline unsigned int LastPathFailures() { return lastPathFailures; }

// Call only after IdentifyRuntime has verified the file's full SHA/size against
// layout. A must not already be loaded. Its normal loader/CRT/HIP initializers
// and DllMain settings run; only its verified native-hook bootstrap is blocked.
// Patching A's IAT after LoadLibrary returns would race the bootstrap thread.
inline HMODULE Load(const wchar_t* path, const AmdLayout* layout)
{
    const auto& expected = Detail::Select(layout);
    if (Detail::loading)
        throw std::runtime_error("Nested AMD runtime loading is unsupported");
    Detail::Install();
    if (GetModuleHandleW(path))
        throw std::runtime_error("AMD runtime was loaded before bootstrap isolation; restart the game");
    const DWORD required = GetFullPathNameW(path, 0, nullptr, nullptr);
    if (!required || required > 32768)
        throw std::runtime_error("Could not resolve the AMD runtime path for isolation");
    std::wstring fullPath(required, L'\0');
    const DWORD length = GetFullPathNameW(path, required, fullPath.data(), nullptr);
    if (!length || length >= required)
        throw std::runtime_error("Could not resolve the AMD runtime path for isolation");
    fullPath.resize(length);
    Detail::Loading context { &expected, fullPath.c_str(), static_cast<int>(fullPath.size()) };
    const auto callsBefore = Detail::totalHookCalls.load(std::memory_order_relaxed);
    Detail::loading = &context;
    HMODULE module = LoadLibraryExW(fullPath.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const DWORD loadError = GetLastError();
    Detail::loading = nullptr;
    if (!module)
        throw std::runtime_error("Private AMD runtime LoadLibrary failed: " + std::to_string(loadError));

    // Never unload a runtime whose CRT may have registered HIP kernels, even
    // when verification fails. A missing interception is not a safe rollback.
    HMODULE pinned {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(module), &pinned))
        throw std::runtime_error("Could not pin loaded AMD runtime; initialization stopped");
    lastPathFailures = context.pathFailures;
    if (context.invalid || context.suppressedCount != 1 || context.suppressedBase != module)
        throw std::runtime_error(Detail::FailureDetails(context, module, callsBefore));
    return module;
}
} // namespace AmdPreSr::RuntimeHostLoad
