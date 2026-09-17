#pragma once
#include <atomic>
#include <cstdint>

// CPU recording diagnostics, scoped to one NR invocation/native Record.
// A Draw observed here is not proof of GPU execution or completion.
namespace AmdPreSr::GraphicsSnap
{
struct InvocationState
{
    std::uint64_t listId = 0;
    std::uint32_t listType = 0, generation = 0;
    bool requested = false, generationKnown = false;
    bool predDisabled = false, renderPassIdle = false, psoReady = false;
    bool admitted = false, frozen = false, pinned = false, planned = false, armed = false;
    bool commandsRecorded = false;
    const char* outcome = "not_recorded";
    const char* reason = "no_envelope";
    char gates[192] {};
};

inline thread_local InvocationState* g_graphicsInvocation = nullptr;
class ScopedGraphicsInvocation
{
    InvocationState* previous_;
  public:
    InvocationState state;
    explicit ScopedGraphicsInvocation(std::uint64_t listId)
        : previous_(g_graphicsInvocation)
    {
        state.listId = listId;
        g_graphicsInvocation = &state;
    }
    ~ScopedGraphicsInvocation() { g_graphicsInvocation = previous_; }
    ScopedGraphicsInvocation(const ScopedGraphicsInvocation&) = delete;
    ScopedGraphicsInvocation& operator=(const ScopedGraphicsInvocation&) = delete;
};

inline InvocationState* GraphicsInvocationFor(std::uint64_t listId)
{
    return g_graphicsInvocation && g_graphicsInvocation->listId == listId ? g_graphicsInvocation : nullptr;
}

struct NativeDispatchCallSites
{
    std::uintptr_t init = 0, fallback = 0, slices = 0, finish = 0;
};

struct NativeDrawObservation
{
    std::uint64_t listId;
    std::uintptr_t callerBegin, callerEnd;
    std::uint64_t count = 0;
    // Layered diagnostics: which filter dropped a Draw.
    std::uint64_t hookHits = 0;
    std::uint64_t sameList = 0;
    std::uint64_t callerMatched = 0;
    std::uint64_t mismatchList = 0;
    std::uintptr_t mismatchReturn = 0;
    NativeDispatchCallSites dispatchSites {};
    std::uint64_t dispatchHook = 0, dispatchSameList = 0, dispatchWait = 0;
    std::uint64_t dispatchInit = 0, dispatchFallback = 0, dispatchSlices = 0, dispatchFinish = 0;
    std::uintptr_t dispatchMismatchReturn = 0;
};
inline thread_local NativeDrawObservation* g_nativeDrawObservation = nullptr;

class ScopedNativeDrawObservation
{
    NativeDrawObservation* previous_;
  public:
    NativeDrawObservation observation;
    ScopedNativeDrawObservation(std::uint64_t listId, std::uintptr_t begin, std::uintptr_t end,
                               NativeDispatchCallSites sites = {})
        : previous_(g_nativeDrawObservation), observation { listId, begin, end }
    {
        observation.dispatchSites = sites;
        g_nativeDrawObservation = &observation;
    }
    ~ScopedNativeDrawObservation() { g_nativeDrawObservation = previous_; }
    ScopedNativeDrawObservation(const ScopedNativeDrawObservation&) = delete;
    ScopedNativeDrawObservation& operator=(const ScopedNativeDrawObservation&) = delete;
};

inline void ObserveNativeDraw(std::uint64_t listId, std::uintptr_t returnAddress)
{
    auto* o = g_nativeDrawObservation;
    if (!o)
        return;
    ++o->hookHits;
    if (o->listId != listId)
    {
        if (!o->mismatchReturn)
        {
            o->mismatchList = listId;
            o->mismatchReturn = returnAddress;
        }
        return;
    }
    ++o->sameList;
    if (!o->callerBegin || returnAddress < o->callerBegin || returnAddress >= o->callerEnd)
    {
        if (!o->mismatchReturn)
        {
            o->mismatchList = listId;
            o->mismatchReturn = returnAddress;
        }
        return;
    }
    ++o->callerMatched;
    ++o->count;
}

inline void ObserveNativeDispatch(std::uint64_t listId, std::uintptr_t returnAddress)
{
    auto* o = g_nativeDrawObservation;
    if (!o) return;
    ++o->dispatchHook;
    if (o->listId != listId) return;
    ++o->dispatchSameList;
    if (!o->callerBegin || returnAddress < o->callerBegin || returnAddress >= o->callerEnd)
    {
        if (!o->dispatchMismatchReturn) o->dispatchMismatchReturn = returnAddress;
        return;
    }
    ++o->dispatchWait;
    // Graphics wait has init/finish Dispatch calls too. Only the two spin
    // call sites establish that the helper recorded a compute wait.
    if (returnAddress == o->dispatchSites.init) ++o->dispatchInit;
    else if (returnAddress == o->dispatchSites.fallback) ++o->dispatchFallback;
    else if (returnAddress == o->dispatchSites.slices) ++o->dispatchSlices;
    else if (returnAddress == o->dispatchSites.finish) ++o->dispatchFinish;
}

// A creates its graphics PSO during staging initialization. A pass first
// recorded in compute can therefore need a restart before graphics is usable.
// Publish only the missing-PSO bits: the menu must not take the recording lock
// or read runtime memory. Unused/new passes must not request a restart early.
class GraphicsRestartState
{
    std::atomic<std::uint32_t> missingPso_ { 0 };
  public:
    void OnRecorded(std::uint32_t pass, bool hasGraphicsPso)
    {
        if (pass >= 3) return;
        const auto bit = std::uint32_t(1) << pass;
        if (hasGraphicsPso)
            missingPso_.fetch_and(~bit, std::memory_order_relaxed);
        else
            missingPso_.fetch_or(bit, std::memory_order_relaxed);
    }
    bool NeedsRestart(std::uint32_t activePasses) const
    {
        if (activePasses > 3) activePasses = 3;
        const auto activeMask = (std::uint32_t(1) << activePasses) - 1;
        return (missingPso_.load(std::memory_order_relaxed) & activeMask) != 0;
    }
};

// Give graphics-first sessions a bounded opportunity before creating compute
// staging. This skips NR, never blocks the game/render thread. Once Record has
// run, normal per-frame fallback applies; missing PSO remains visible in logs.
class GraphicsStartupGate
{
    bool waiting_ = false, recorded_ = false;
    std::uint64_t started_ = 0;
  public:
    bool ShouldDefer(bool requested, bool armed, std::uint64_t now, std::uint64_t waitMs = 2000)
    {
        if (!requested || armed || recorded_)
            return false;
        if (!waiting_)
        {
            waiting_ = true;
            started_ = now;
        }
        return now - started_ < waitMs;
    }
    void MarkRecorded() { recorded_ = true; }
    bool HasRecorded() const { return recorded_; }
};
}
