#pragma once
#include "../amd/AmdPreSr.h"
#include "../NrPerformance.h"
#include <string>
#include <atomic>

namespace DlssNr::Backend
{
// Methods AmdBridge already calls on the live Daniel host. New ABI fields for lmxxf
// are not added here; this increment only isolates the call sites.
class Host
{
    inline static std::atomic<uint64_t> nextTimingRuntimeId {1};
    std::atomic<uint64_t> timingRuntimeId {0};
  protected:
    // Advance while clearing cached timing on session/measurement epoch changes.
    void AdvanceTimingRuntimeId()
    { timingRuntimeId.store(nextTimingRuntimeId.fetch_add(1, std::memory_order_relaxed), std::memory_order_release); }
  public:
    virtual ~Host() = default;
    virtual ID3D12Resource* Record(ID3D12GraphicsCommandList*, const AmdPreSr::Frame&,
                                   const AmdPreSr::Settings&) = 0;
    // Optional same-frame first pass. The returned textures remain owned by the
    // recording observer until invalidation plus GPU completion.
    virtual ID3D12Resource* RecordLayers(ID3D12GraphicsCommandList* cmd, const AmdPreSr::Frame& frame,
                                        const AmdPreSr::Settings& settings, ID3D12Resource** first)
    { if(first)*first=nullptr; return Record(cmd,frame,settings); }
    virtual int PendingListIndex(UINT, ID3D12CommandList* const*) const = 0;
    virtual void Submitting(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) = 0;
    virtual void TraceBoundary(const std::string&) = 0;
    virtual void Submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) = 0;
    virtual bool Shutdown() = 0;
    // Request session-buffer release after outstanding submissions complete.
    // PollRelease also runs while disabled. A later Record can rebuild.
    // Fault latches (recoveryDisabled / failed) are NOT set here.
    virtual void ReleaseSession() = 0;
    // Same release, with the reason for the release log (release_reason=...).
    // Backends that log override this; the default ignores the reason.
    virtual void ReleaseSession(const char* reason) { (void)reason; ReleaseSession(); }
    virtual bool PollRelease() = 0;
    // Backend switch / re-enable: clear compute-first history so new wait can re-init.
    virtual void ResetGraphicsWaitState() = 0;
    virtual void InvalidateHistory() = 0;
    virtual std::string Status() const = 0;
    virtual NrTimingSnapshot Timing() const { return {}; }
    uint64_t TimingRuntimeId() const { return timingRuntimeId.load(std::memory_order_acquire); }
    virtual bool IsRunning() const = 0;
    virtual bool GraphicsRestartNeeded(UINT activePasses) const = 0;
};
} // namespace DlssNr::Backend
