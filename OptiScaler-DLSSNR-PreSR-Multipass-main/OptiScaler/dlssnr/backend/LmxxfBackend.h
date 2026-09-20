#pragma once
#include "Host.h"
#include "LmxxfEvaluateCut.h"
#include <filesystem>

namespace DlssNr::Backend
{
// Full Host for lmxxf. Constructed only when ActiveKind==Lmxxf (requires LmxxfWired()).
// Record: PrepareFrame → RecordInputs → Split → RecordOutputs → SetPendingEnqueue(EnqueueHip).
class LmxxfBackend final : public Host
{
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    std::filesystem::path directory;
    void *runtimeDll = nullptr; // HMODULE
    void *session = nullptr;
    bool sessionReady = false;
    uint64_t frameId = 0;
    std::string status { "lmxxf: idle" };
    // Function table copied from LmxxfNrGetApi (opaque here to keep header free of C ABI).
    struct Api;
    Api *api = nullptr;
    void *pendingJob = nullptr;

    bool EnsureRuntime();
    bool EnsureSession();
    void SetStatus(const char *s);

  public:
    LmxxfBackend(ID3D12Device *device, ID3D12CommandQueue *queue, const std::filesystem::path &directory);
    ~LmxxfBackend() override;
    LmxxfBackend(const LmxxfBackend &) = delete;
    LmxxfBackend &operator=(const LmxxfBackend &) = delete;

    ID3D12Resource *Record(ID3D12GraphicsCommandList *, const AmdPreSr::Frame &,
                           const AmdPreSr::Settings &) override;
    int PendingListIndex(UINT, ID3D12CommandList *const *) const override;
    void Submitting(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *) override;
    void TraceBoundary(const std::string &) override;
    void Submitted(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *) override;
    bool Shutdown() override;
    void InvalidateHistory() override;
    std::string Status() const override;
    bool GraphicsRestartNeeded(UINT activePasses) const override;
};
} // namespace DlssNr::Backend
