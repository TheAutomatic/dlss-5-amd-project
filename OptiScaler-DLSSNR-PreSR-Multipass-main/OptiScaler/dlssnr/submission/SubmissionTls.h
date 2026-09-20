#pragma once
#include <d3d12.h>

namespace DlssNr::Submission
{
// When non-zero, CreateCommandList hooks must not wrap (continuation alloc inside Split).
inline thread_local int g_suppressProxyWrap = 0;

struct SuppressProxyWrap
{
    SuppressProxyWrap() { ++g_suppressProxyWrap; }
    ~SuppressProxyWrap() { --g_suppressProxyWrap; }
    SuppressProxyWrap(const SuppressProxyWrap &) = delete;
    SuppressProxyWrap &operator=(const SuppressProxyWrap &) = delete;
};

// True ID3D12CommandQueue::ExecuteCommandLists before any of our Detours.
// LogicalList must use this for producer/continuation submits so AmdBridge::Submitted
// does not re-enter and ClearPendingEnqueue before the HIP between-slot runs.
using PFN_RawExecuteCommandLists = void(WINAPI *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
inline PFN_RawExecuteCommandLists g_rawExecuteCommandLists = nullptr;

inline void NoteRawExecuteCommandLists(PFN_RawExecuteCommandLists fn)
{
    if (fn && !g_rawExecuteCommandLists)
        g_rawExecuteCommandLists = fn;
}

inline thread_local int g_logicalExecuteDepth = 0;

struct LogicalExecuteScope
{
    LogicalExecuteScope() { ++g_logicalExecuteDepth; }
    ~LogicalExecuteScope() { --g_logicalExecuteDepth; }
    LogicalExecuteScope(const LogicalExecuteScope &) = delete;
    LogicalExecuteScope &operator=(const LogicalExecuteScope &) = delete;
};

inline bool InsideLogicalExecute() { return g_logicalExecuteDepth > 0; }
} // namespace DlssNr::Submission
