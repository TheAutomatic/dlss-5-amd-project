#pragma once
#include "../amd/AmdPreSr.h"
#include <string>

namespace DlssNr::Backend
{
// Methods AmdBridge already calls on the live Daniel host. New ABI fields for lmxxf
// are not added here; this increment only isolates the call sites.
class Host
{
  public:
    virtual ~Host() = default;
    virtual ID3D12Resource* Record(ID3D12GraphicsCommandList*, const AmdPreSr::Frame&,
                                   const AmdPreSr::Settings&) = 0;
    virtual int PendingListIndex(UINT, ID3D12CommandList* const*) const = 0;
    virtual void Submitting(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) = 0;
    virtual void TraceBoundary(const std::string&) = 0;
    virtual void Submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) = 0;
    virtual bool Shutdown() = 0;
    // Request session-buffer release after outstanding submissions complete.
    // PollRelease also runs while disabled. A later Record can rebuild.
    // Fault latches (recoveryDisabled / failed) are NOT set here.
    virtual void ReleaseSession() = 0;
    virtual bool PollRelease() = 0;
    // Backend switch / re-enable: clear compute-first history so new wait can re-init.
    virtual void ResetGraphicsWaitState() = 0;
    virtual void InvalidateHistory() = 0;
    virtual std::string Status() const = 0;
    virtual bool GraphicsRestartNeeded(UINT activePasses) const = 0;
};
} // namespace DlssNr::Backend
