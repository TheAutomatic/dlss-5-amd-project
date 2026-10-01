#pragma once
#include "Host.h"
#include "../DiagnosticLog.h"
#include "LmxxfEvaluateCut.h"
#include <memory>
#include "LmxxfColorProbe.h"
#include "LmxxfStagingProbe.h"
#include <filesystem>
#include <mutex>

namespace DlssNr::Backend
{
namespace LmxxfRecording { struct SessionOwner; }
// Last lmxxf colour job height seen in Record (0 = unknown). Menus use this to
// gray NR% tiers that are taller than the current input.
unsigned LastLmxxfColorHeight();

// Full Host for lmxxf. Constructed only when ActiveKind==Lmxxf (requires LmxxfWired()).
// Record: PrepareFrame -> recording observer -> Inputs -> Split -> Outputs.
class LmxxfBackend final : public Host
{
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    std::filesystem::path directory;
    void *runtimeDll = nullptr; // HMODULE
    void *session = nullptr;
    std::shared_ptr<LmxxfRecording::SessionOwner> sessionOwner;
    bool sessionReady = false;
    uint64_t frameId = 0;
    std::string status { "lmxxf: idle" };
    // Function table copied from LmxxfNrGetApi (opaque here to keep header free of C ABI).
    struct Api;
    Api *api = nullptr;
    LmxxfProbe::Mode diagnostic = LmxxfProbe::Mode::Off;
    LmxxfProbe::ColorCopy colorProbe;
    LmxxfProbe::StagingProbe stagingProbe;
    uint64_t probeEvaluateId = 0; // Host ordinal, NOT an engine frame ID or GPU completion.
    uint64_t evaluateSequence_ = 0; // Monotonic sequence across all Evaluate calls including bypassed.
    uint64_t boundaryProxyHits = 0;
    uint64_t boundaryCuts = 0;
    uint64_t boundaryRejects = 0;
    // Zero-output recoveries reported through the between slot (EnqueueHip OK + diagnostic).
    uint64_t seenRecoveries = 0;
    uint64_t seenEnqueueCalls = 0;
    uint32_t consecutiveRecoveries = 0;
    bool recoveryDisabled = false;
    // Record calls to skip before retrying a failed Create/PrepareSession.
    uint32_t sessionFailures = 0;
    uint32_t sessionRetryIn = 0;
    // Set once a runtime has rejected the current LmxxfNrFrameInfo size: it predates the
    // exposure fields, so send the ABI v1 size and run without exposure.
    bool frameInfoV1 = false;
    // PrepareFrame failure accounting: first error is fully logged; later poison repeats are quiet.
    unsigned prepareFrameFailLogs = 0;
    unsigned prepareFramePoisonLogs = 0;
    uint64_t loggedSubmissionFailures = 0;
    uint64_t lastAnomalyTime = 0;
    Diagnostics::RepeatGate anomalyLog;
    ID3D12Resource *RecordDiagnostic(ID3D12GraphicsCommandList *, const AmdPreSr::Frame &,
                                     const AmdPreSr::Settings &);

    bool EnsureRuntime();
    ID3D12Resource *FinishRecord(ID3D12GraphicsCommandList *recordCmd, void *jobHandle, void *privateOutput);
    bool EnsureSession();
    void NoteSessionFailure();
    bool NoteEnqueueRecoveries();
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
    // NR off drops the active owner. Old closed recordings retain their own
    // sessions until invalidation and actual GPU completion.
    void ReleaseSession() override;
    bool PollRelease() override;
    void ResetGraphicsWaitState() override;
    void InvalidateHistory() override;
    std::string Status() const override;
    bool GraphicsRestartNeeded(UINT activePasses) const override;
};
} // namespace DlssNr::Backend
