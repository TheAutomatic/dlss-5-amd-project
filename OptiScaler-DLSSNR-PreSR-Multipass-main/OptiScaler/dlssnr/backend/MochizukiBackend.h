#pragma once
#include "Host.h"
#include "mochizuki_runtime/MochizukiNrControls.h"
#include <filesystem>
#include <memory>

namespace DlssNr::Backend
{
class MochizukiBackend final : public Host
{
    struct Impl;
    std::unique_ptr<Impl> p;
public:
    MochizukiBackend(ID3D12Device*, ID3D12CommandQueue*, const std::filesystem::path&);
    ~MochizukiBackend() override;
    ID3D12Resource* Record(ID3D12GraphicsCommandList*, const AmdPreSr::Frame&, const AmdPreSr::Settings&) override;
    int PendingListIndex(UINT, ID3D12CommandList* const*) const override { return -1; }
    void Submitting(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) override {}
    void Submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) override;
    void TraceBoundary(const std::string&) override {}
    bool Shutdown() override;
    void ReleaseSession() override;
    bool PollRelease() override;
    void ResetGraphicsWaitState() override {}
    void InvalidateHistory() override;
    std::string Status() const override;
    bool IsRunning() const override;
    NrTimingSnapshot Timing() const override;
    MochizukiNrBuildProgress BuildProgress() const;
    MochizukiNrInfo TimingDetails() const;
    bool GraphicsRestartNeeded(UINT) const override { return false; }
};
}
