#pragma once
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

struct NativeDrawObservation
{
    std::uint64_t listId;
    std::uintptr_t callerBegin, callerEnd;
    std::uint64_t count = 0;
};
inline thread_local NativeDrawObservation* g_nativeDrawObservation = nullptr;

class ScopedNativeDrawObservation
{
    NativeDrawObservation* previous_;
  public:
    NativeDrawObservation observation;
    ScopedNativeDrawObservation(std::uint64_t listId, std::uintptr_t begin, std::uintptr_t end)
        : previous_(g_nativeDrawObservation), observation { listId, begin, end }
    {
        g_nativeDrawObservation = &observation;
    }
    ~ScopedNativeDrawObservation() { g_nativeDrawObservation = previous_; }
    ScopedNativeDrawObservation(const ScopedNativeDrawObservation&) = delete;
    ScopedNativeDrawObservation& operator=(const ScopedNativeDrawObservation&) = delete;
};

inline void ObserveNativeDraw(std::uint64_t listId, std::uintptr_t returnAddress)
{
    auto* o = g_nativeDrawObservation;
    if (o && o->listId == listId && o->callerBegin && returnAddress >= o->callerBegin &&
        returnAddress < o->callerEnd)
        ++o->count;
}

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
