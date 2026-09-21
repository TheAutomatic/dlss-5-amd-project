#pragma once
#include "Host.h"
#include "LmxxfEvaluateCut.h"
#include "LmxxfColorProbe.h"
#include "LmxxfStagingProbe.h"
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
    LmxxfProbe::Mode diagnostic = LmxxfProbe::Mode::Off;
    LmxxfProbe::ColorCopy colorProbe;
    LmxxfProbe::StagingProbe stagingProbe;
    uint64_t probeEvaluateId = 0; // Host ordinal, NOT an engine frame ID or GPU completion.
    uint64_t evaluateSequence_ = 0; // Monotonic sequence across all Evaluate calls including bypassed.
    uint64_t boundaryProxyHits = 0;
    uint64_t boundaryCuts = 0;
    uint64_t boundaryRejects = 0;
    ID3D12Resource *RecordDiagnostic(ID3D12GraphicsCommandList *, const AmdPreSr::Frame &);

    bool EnsureRuntime();
    ID3D12Resource *FinishRecord(ID3D12GraphicsCommandList *recordCmd, void *jobHandle, void *privateOutput);
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
