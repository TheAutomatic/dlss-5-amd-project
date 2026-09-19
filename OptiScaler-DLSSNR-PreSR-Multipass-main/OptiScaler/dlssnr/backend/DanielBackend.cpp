#include "pch.h"
#include "DanielBackend.h"

namespace DlssNr::Backend
{
DanielBackend::DanielBackend(ID3D12Device* device, ID3D12CommandQueue* queue,
                             const std::filesystem::path& directory)
    : inner(device, queue, directory)
{
}

ID3D12Resource* DanielBackend::Record(ID3D12GraphicsCommandList* cmd, const AmdPreSr::Frame& frame,
                                      const AmdPreSr::Settings& settings)
{
    return inner.Record(cmd, frame, settings);
}

int DanielBackend::PendingListIndex(UINT n, ID3D12CommandList* const* lists) const
{
    return inner.PendingListIndex(n, lists);
}

void DanielBackend::Submitting(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    inner.Submitting(q, n, lists);
}

void DanielBackend::TraceBoundary(const std::string& s) { inner.TraceBoundary(s); }

void DanielBackend::Submitted(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    inner.Submitted(q, n, lists);
}

bool DanielBackend::Shutdown() { return inner.Shutdown(); }

void DanielBackend::InvalidateHistory() { inner.InvalidateHistory(); }

std::string DanielBackend::Status() const { return inner.Status(); }

bool DanielBackend::GraphicsRestartNeeded(UINT activePasses) const
{
    return inner.GraphicsRestartNeeded(activePasses);
}
} // namespace DlssNr::Backend
