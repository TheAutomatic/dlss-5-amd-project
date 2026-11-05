#pragma once
#include "RecordingLifecycle.h"
#include <atomic>
#include <wrl/client.h>

namespace DlssNr::Submission::DepthProvenance
{
// Diagnostic only: a COM identity reference prevents target address reuse. No
// claims about shader writes: descriptor tables/raw escaped lists are not decoded.
inline std::atomic<ID3D12Resource*> target {nullptr};
inline std::atomic<unsigned> budget {0};
inline Microsoft::WRL::ComPtr<ID3D12Resource> retained;
inline const void* owner = nullptr; // Protected by mutex; disarm only the owning probe.
inline std::recursive_mutex mutex;
inline bool Matches(ID3D12Resource* r) { return r && target.load(std::memory_order_acquire) == r; }
inline bool Admit()
{
    unsigned left = budget.load(std::memory_order_relaxed);
    while (left && !budget.compare_exchange_weak(left,left-1,std::memory_order_relaxed)) {}
    return left != 0;
}
inline void Event(const char* operation, RecordingIdentity id, ID3D12Resource* r, UINT64 a=0, UINT64 b=0)
{
    if (!Matches(r)) return;
    std::lock_guard lock(mutex);
    if (!Matches(r) || !Admit()) return; // Recheck while the target identity is retained.
    LOG_WARN("[RR_DEPTH_TRACE] op={} list={} generation={} resource={:p} a={:#x} b={:#x} thread={}",
             operation,id.list,id.generation,static_cast<void*>(r),a,b,GetCurrentThreadId());
}
struct Submission final : RecordingObserver
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    void Executed(const RecordingExecution& e) noexcept override
    {
        if (!Matches(resource.Get())) return;
        std::lock_guard lock(mutex);
        if (!Matches(resource.Get()) || !Admit()) return;
        LOG_WARN("[RR_DEPTH_TRACE] op=execute list={} generation={} serial={} queue={:p} submitted={} "
                 "continuation={} status={:#x} fence={:p} value={}",e.identity.list,e.identity.generation,
                 e.serial,static_cast<void*>(e.queue),e.producerSubmitted,e.continuationSubmitted,
                 UINT(e.status),static_cast<void*>(e.fence),e.fenceValue);
    }
    void Invalidated(RecordingIdentity id) noexcept override { Event("recording_invalidated",id,resource.Get()); }
};
inline void Arm(ID3D12Resource* r, const void* probe)
{
    std::lock_guard lock(mutex);
    if (!r || retained) return; // One target allocation at a time, bounded.
    retained=r;
    owner=probe;
    budget.store(384,std::memory_order_relaxed);
    target.store(r,std::memory_order_release);
    const auto d=r->GetDesc();
    LOG_WARN("[RR_DEPTH_TRACE] armed resource={:p} extent={}x{} array={} mips={} format={} flags={:#x}; "
             "subresource0 only; earlier commands, native escaped lists, shader descriptor writes and other queues' "
             "Signal/Wait are not inferred from missing events",static_cast<void*>(r),d.Width,d.Height,
             d.DepthOrArraySize,d.MipLevels,UINT(d.Format),UINT(d.Flags));
}
inline void Disarm(const void* probe)
{
    if (!target.load(std::memory_order_acquire)) return;
    std::lock_guard lock(mutex);
    if (owner != probe) return;
    // Event/Executed recheck under this mutex before using the retained identity.
    target.store(nullptr,std::memory_order_release);
    budget.store(0,std::memory_order_relaxed);
    owner=nullptr;
    retained.Reset();
    // Recording observers retain their own resources independently of this target.
}
}
