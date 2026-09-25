#pragma once
#include <d3d12.h>
#include <atomic>
#include <cstdint>

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

// Detours trampoline captured AFTER a successful attachment transaction.
// LogicalList must use this for producer/continuation submits so AmdBridge::Submitted
// does not re-enter and ClearPendingEnqueue before the HIP between-slot runs.
using PFN_RawExecuteCommandLists = void(WINAPI *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
inline PFN_RawExecuteCommandLists g_rawExecuteCommandLists = nullptr;

inline void NoteRawExecuteCommandLists(PFN_RawExecuteCommandLists fn)
{
    g_rawExecuteCommandLists = fn;
}

// Submission counts, not GPU completion or engine frame IDs. No per-frame logging here.
inline std::atomic<uint64_t> g_splitSubmissions { 0 };
inline std::atomic<uint64_t> g_continuationSubmissions { 0 };
inline std::atomic<uint64_t> g_submissionFailures { 0 };
inline std::atomic<uint64_t> g_unsplitProxySubmissions { 0 };

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
