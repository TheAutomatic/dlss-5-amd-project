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

    // Private proxied list when Evaluate cmd is not ILogicalCommandList (yysls CreateCommandList).
    ID3D12CommandAllocator *privAlloc = nullptr;
    ID3D12GraphicsCommandList *privCmd = nullptr;
    ID3D12Fence *privFence = nullptr;
    HANDLE privFenceEvent = nullptr;
    UINT64 privFenceValue = 0;

    // 1-frame Color capture: copy THIS frame onto the game cmd; NR reads the PREVIOUS
    // capture so Encode never races unsubmitted Color producers on the same list.
    ID3D12Resource *colorRing[2] {};
    bool colorRingReady[2] {};
    UINT colorRingWrite = 0;
    UINT colorRingW = 0;
    UINT colorRingH = 0;
    DXGI_FORMAT colorRingFmt = DXGI_FORMAT_UNKNOWN;

    bool EnsureRuntime();
    bool EnsurePrivateList();
    void ReleasePrivateList();
    void ReleaseColorRing();
    bool EnsureColorRing(ID3D12Resource *color);
    void ScheduleColorCapture(ID3D12GraphicsCommandList *gameCmd, ID3D12Resource *color,
                              D3D12_RESOURCE_STATES colorState, UINT slot);
    ID3D12Resource *FinishRecord(ID3D12GraphicsCommandList *recordCmd, void *jobHandle, void *privateOutput,
                                 bool executeNow);
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
