#pragma once
#include "GraphicsInvocation.h"
#include <windows.h>
#include <d3d12.h>
#include <tlhelp32.h>
#include <detours/detours.h>
#include <intrin.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <new>
#include <vector>

// Diagnostic coverage for the actual command-list implementation handed to A.
// An installation failure affects observation only, never graphics admission.
namespace AmdPreSr::GraphicsSnap::NativeWaitHooks
{
struct Coverage
{
    std::uintptr_t drawTarget = 0, dispatchTarget = 0;
    LONG drawError = ERROR_NOT_READY, dispatchError = ERROR_NOT_READY;
    bool drawCovered = false, dispatchCovered = false;
};

namespace Detail
{
using DrawFn = void(WINAPI*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DispatchFn = void(WINAPI*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
inline constexpr std::size_t Capacity = 8;

template <typename Fn> struct Slot
{
    std::uintptr_t target = 0;
    Fn original = nullptr;
    LONG error = ERROR_NOT_READY;
    bool covered = false;
};
inline std::array<Slot<DrawFn>, Capacity> drawSlots {};
inline std::array<Slot<DispatchFn>, Capacity> dispatchSlots {};
inline std::mutex installMutex;

// Every patched address has its own forwarding function and trampoline.
// The callbacks never allocate, log, lock, or change the command arguments.
template <std::size_t Index>
__declspec(noinline) inline void WINAPI Draw(ID3D12GraphicsCommandList* list, UINT vertices,
                                            UINT instances, UINT firstVertex, UINT firstInstance)
{
    ObserveNativeDraw(reinterpret_cast<std::uintptr_t>(list),
                      reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    drawSlots[Index].original(list, vertices, instances, firstVertex, firstInstance);
}

template <std::size_t Index>
__declspec(noinline) inline void WINAPI Dispatch(ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z)
{
    ObserveNativeDispatch(reinterpret_cast<std::uintptr_t>(list),
                          reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    dispatchSlots[Index].original(list, x, y, z);
}

inline constexpr std::array<DrawFn, Capacity> drawHooks {
    Draw<0>, Draw<1>, Draw<2>, Draw<3>, Draw<4>, Draw<5>, Draw<6>, Draw<7>
};
inline constexpr std::array<DispatchFn, Capacity> dispatchHooks {
    Dispatch<0>, Dispatch<1>, Dispatch<2>, Dispatch<3>,
    Dispatch<4>, Dispatch<5>, Dispatch<6>, Dispatch<7>
};

struct Handle
{
    HANDLE value;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct ThreadHandles
{
    std::vector<HANDLE> values;
    ~ThreadHandles() { for (auto value : values) CloseHandle(value); }
};

// Finish enumeration and allocation before Detours suspends other threads.
inline LONG CollectThreads(ThreadHandles& threads)
{
    Handle snapshot { CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) };
    if (snapshot.value == INVALID_HANDLE_VALUE)
        return static_cast<LONG>(GetLastError());
    THREADENTRY32 item {};
    item.dwSize = sizeof(item);
    if (!Thread32First(snapshot.value, &item))
        return static_cast<LONG>(GetLastError());
    const DWORD process = GetCurrentProcessId(), current = GetCurrentThreadId();
    do
    {
        if (item.th32OwnerProcessID != process || item.th32ThreadID == current)
            continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                   THREAD_QUERY_INFORMATION, FALSE, item.th32ThreadID);
        if (!thread)
        {
            const auto error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) // exited after enumeration
                continue;
            return static_cast<LONG>(error);
        }
        try { threads.values.push_back(thread); }
        catch (...) { CloseHandle(thread); throw; }
    } while (Thread32Next(snapshot.value, &item));
    const auto error = GetLastError();
    return error == ERROR_NO_MORE_FILES ? NO_ERROR : static_cast<LONG>(error);
}

struct Transaction
{
    bool owned = false;
    ~Transaction() { if (owned) DetourTransactionAbort(); }
};

template <typename Fn> inline LONG Install(Slot<Fn>& slot, Fn hook)
{
    // These process-wide hooks intentionally remain installed. Their owner
    // must outlive every command list that can call the forwarding function.
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(hook), &owner))
        return static_cast<LONG>(GetLastError());

    ThreadHandles threads;
    LONG error = CollectThreads(threads);
    if (error != NO_ERROR)
        return error;
    Transaction transaction;
    error = DetourTransactionBegin();
    if (error != NO_ERROR)
        return error; // Never abort a transaction owned by another caller.
    transaction.owned = true;
    error = DetourUpdateThread(GetCurrentThread());
    for (auto thread : threads.values)
    {
        if (error != NO_ERROR)
            break;
        error = DetourUpdateThread(thread);
    }
    if (error == NO_ERROR)
        error = DetourAttach(reinterpret_cast<PVOID*>(&slot.original), reinterpret_cast<PVOID>(hook));
    if (error != NO_ERROR)
        return error;
    error = DetourTransactionCommit();
    transaction.owned = false;
    return error;
}

struct Status { LONG error; bool covered; };

template <typename Fn>
inline Status EnsureTarget(std::array<Slot<Fn>, Capacity>& slots,
                           const std::array<Fn, Capacity>& hooks, std::uintptr_t target) noexcept
{
    if (!target)
        return { ERROR_INVALID_ADDRESS, false };
    for (const auto& slot : slots)
        if (slot.target == target)
            return { slot.error, slot.covered };
    for (std::size_t index = 0; index < slots.size(); ++index)
    {
        auto& slot = slots[index];
        if (slot.target)
            continue;
        slot.target = target; // Cache failures too; never retry every frame.
        slot.original = reinterpret_cast<Fn>(target);
        try { slot.error = Install(slot, hooks[index]); }
        catch (const std::bad_alloc&) { slot.error = ERROR_NOT_ENOUGH_MEMORY; }
        catch (...) { slot.error = ERROR_GEN_FAILURE; }
        slot.covered = slot.error == NO_ERROR;
        return { slot.error, slot.covered };
    }
    return { ERROR_TOO_MANY_CMDS, false };
}
} // namespace Detail

// Call with a live borrowed list before taking the backend mutex or calling A.
// verifiedEarlyDrawTarget is the pre-detour entry of a successfully committed
// early Draw hook, not its (post-detour) trampoline address.
inline Coverage Ensure(ID3D12GraphicsCommandList* list, std::uintptr_t verifiedEarlyDrawTarget) noexcept
{
    Coverage result;
    if (!list)
    {
        result.drawError = result.dispatchError = ERROR_INVALID_PARAMETER;
        return result;
    }
    auto** vtable = *reinterpret_cast<void***>(list);
    result.drawTarget = reinterpret_cast<std::uintptr_t>(vtable[12]);
    result.dispatchTarget = reinterpret_cast<std::uintptr_t>(vtable[14]);
    try
    {
        std::lock_guard<std::mutex> lock(Detail::installMutex);
        if (verifiedEarlyDrawTarget && result.drawTarget == verifiedEarlyDrawTarget)
        {
            result.drawError = NO_ERROR;
            result.drawCovered = true;
        }
        else
        {
            const auto draw = Detail::EnsureTarget(Detail::drawSlots, Detail::drawHooks, result.drawTarget);
            result.drawError = draw.error;
            result.drawCovered = draw.covered;
        }
        const auto dispatch = Detail::EnsureTarget(Detail::dispatchSlots, Detail::dispatchHooks,
                                                   result.dispatchTarget);
        result.dispatchError = dispatch.error;
        result.dispatchCovered = dispatch.covered;
    }
    catch (...)
    {
        if (!result.drawCovered) result.drawError = ERROR_GEN_FAILURE;
        if (!result.dispatchCovered) result.dispatchError = ERROR_GEN_FAILURE;
    }
    return result;
}
} // namespace AmdPreSr::GraphicsSnap::NativeWaitHooks
