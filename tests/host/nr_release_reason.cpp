// Contract: a release reason handed to Host::ReleaseSession(const char*) reaches the
// backend that logs it (mochizuki release_reason=...), and backends that never
// implemented it keep working through the reason-ignoring default.
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/Host.h"
#include <cassert>
#include <cstdio>
#include <string>

namespace
{
struct MinimalHost final : DlssNr::Backend::Host
{
    int released = 0;
    std::string lastReason {"<none>"};
    bool reasonSeen = false;

    ID3D12Resource* Record(ID3D12GraphicsCommandList*, const AmdPreSr::Frame&,
                           const AmdPreSr::Settings&) override { return nullptr; }
    int PendingListIndex(UINT, ID3D12CommandList* const*) const override { return -1; }
    void Submitting(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) override {}
    void TraceBoundary(const std::string&) override {}
    void Submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) override {}
    bool Shutdown() override { return true; }
    void ReleaseSession() override { ++released; }
    // MochizukiBackend's shape: the reason-aware overload is the one that logs.
    void ReleaseSession(const char* reason) override
    {
        reasonSeen = true;
        lastReason = reason && *reason ? reason : "unspecified";
        ReleaseSession();
    }
    bool PollRelease() override { return true; }
    void ResetGraphicsWaitState() override {}
    void InvalidateHistory() override {}
    std::string Status() const override { return {}; }
    bool IsRunning() const override { return false; }
    bool GraphicsRestartNeeded(UINT) const override { return false; }
};

struct PlainHost final : DlssNr::Backend::Host
{
    int released = 0;
    ID3D12Resource* Record(ID3D12GraphicsCommandList*, const AmdPreSr::Frame&,
                           const AmdPreSr::Settings&) override { return nullptr; }
    int PendingListIndex(UINT, ID3D12CommandList* const*) const override { return -1; }
    void Submitting(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) override {}
    void TraceBoundary(const std::string&) override {}
    void Submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) override {}
    bool Shutdown() override { return true; }
    void ReleaseSession() override { ++released; }
    bool PollRelease() override { return true; }
    void ResetGraphicsWaitState() override {}
    void InvalidateHistory() override {}
    std::string Status() const override { return {}; }
    bool IsRunning() const override { return false; }
    bool GraphicsRestartNeeded(UINT) const override { return false; }
};
}

int main()
{
    using DlssNr::Backend::Host;
    {
        MinimalHost host;
        Host& h = host;
        h.ReleaseSession("menu_toggle");
        assert(host.released == 1 && host.reasonSeen && host.lastReason == "menu_toggle");
        h.ReleaseSession("backend_switch");
        assert(host.released == 2 && host.lastReason == "backend_switch");
        h.ReleaseSession("shutdown");
        assert(host.released == 3 && host.lastReason == "shutdown");
        // A null/empty reason (the plain ReleaseSession() delegation) still releases.
        h.ReleaseSession(nullptr);
        assert(host.released == 4 && host.lastReason == "unspecified");
    }
    {
        // Backends without the overload keep the reason-ignoring default.
        PlainHost host;
        Host& h = host;
        h.ReleaseSession("menu_toggle");
        assert(host.released == 1);
        h.ReleaseSession();
        assert(host.released == 2);
    }
    std::puts("nr_release_reason: PASS");
    return 0;
}
