#pragma once
#include "Host.h"

namespace DlssNr::Backend
{
class DanielBackend final : public Host
{
    AmdPreSr::Backend inner;

  public:
    DanielBackend(ID3D12Device*, ID3D12CommandQueue*, const std::filesystem::path& directory);
    ID3D12Resource* Record(ID3D12GraphicsCommandList*, const AmdPreSr::Frame&,
                           const AmdPreSr::Settings&) override;
    int PendingListIndex(UINT, ID3D12CommandList* const*) const override;
    void Submitting(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) override;
    void TraceBoundary(const std::string&) override;
    void Submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) override;
    bool Shutdown() override;
    void InvalidateHistory() override;
    std::string Status() const override;
    bool GraphicsRestartNeeded(UINT activePasses) const override;
};
} // namespace DlssNr::Backend
