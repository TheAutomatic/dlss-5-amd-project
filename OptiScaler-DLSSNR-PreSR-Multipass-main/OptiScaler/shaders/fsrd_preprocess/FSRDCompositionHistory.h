#pragma once
#include <shaders/RecordedShader_Dx12.h>

namespace FSRD::CompositionHistory
{
// RecordingMutex guards publication, recording admission, and invalidation.
struct Generation
{
    RecordedShader::ComPtr<ID3D12Resource> decisions, metadata;
    std::vector<RecordedShader::Owner::Proof> accesses;
    DlssNr::Submission::RecordingIdentity writer {};
    // Retirement uncertainty quarantines storage; only writer uncertainty makes
    // the contents unreadable. A reader's failed Signal must not erase good data.
    bool produced = false, uncertain = false, contentUncertain = false, discarded = false;
    uint64_t admittedEpoch = 0;
    size_t reservedAccesses = 0;

    bool CanRead(DlssNr::Submission::RecordingIdentity) const noexcept
    {
        // Recording retains a candidate, not a promise that its writer executes.
        return writer.list && !contentUncertain && !discarded;
    }
    bool CanRead(ID3D12GraphicsCommandList* list) const noexcept
    {
        return list && CanRead(DlssNr::Submission::RecordingIdentity {});
    }
    bool OrderedBefore(DlssNr::Submission::RecordingIdentity reader) const noexcept
    {
        return CanRead(reader) && (produced || writer == reader ||
            (admittedEpoch && admittedEpoch == DlssNr::Submission::g_recordingAdmissionEpoch));
    }
};

struct Use : RecordedShader::Owner
{
    enum class Selection { Pending, History, NoHistory };
    std::shared_ptr<Generation> generation;
    bool write = false, submittedOnce = false, accessReserved = false;
    Selection selection = Selection::Pending;
    // Only private dispatch slots, retained for the entire recording lifetime,
    // can implement this. An absent patch target is an error, never permission
    // to execute a speculative SRV or silently ignore an admission HRESULT.
    virtual bool CanFinalizeNoHistory() const noexcept { return false; }
    virtual void FinalizeNoHistory() noexcept {}
    HRESULT BeforeExecute(const DlssNr::Submission::RecordingExecution& e) noexcept override
    {
        const HRESULT admitted = Owner::BeforeExecute(e);
        if (FAILED(admitted)) return admitted;
        auto& g = *generation;
        if (write)
        {
            if (g.uncertain || g.contentUncertain || g.discarded) return E_UNEXPECTED;
        }
        else if (selection != Selection::NoHistory)
        {
            if (g.OrderedBefore(e.identity)) selection = Selection::History;
            else
            {
                // A preceding observer may have aborted the whole native batch.
                // Until actual submission even a History choice is still private.
                if (submittedOnce || !CanFinalizeNoHistory()) return E_UNEXPECTED;
                FinalizeNoHistory();
                selection = Selection::NoHistory;
            }
        }
        if (!write && selection == Selection::NoHistory) return S_OK;
        try { g.accesses.reserve(g.accesses.size() + g.reservedAccesses + 1); }
        catch (...) { return E_OUTOFMEMORY; }
        for (const auto& proof : g.accesses)
        {
            const auto done = proof.fence->GetCompletedValue();
            if (done != UINT64_MAX && done >= proof.value) continue;
            const HRESULT hr = e.queue->Wait(proof.fence.Get(), proof.value);
            if (FAILED(hr)) return hr;
        }
        if (!accessReserved) { ++g.reservedAccesses; accessReserved = true; }
        // Published only after every fallible writer admission operation; the
        // scope is one contiguous native batch and cannot authorize a later call.
        if (write) g.admittedEpoch = DlssNr::Submission::g_recordingAdmissionEpoch;
        return S_OK;
    }
    void ProducerSubmitted(const DlssNr::Submission::RecordingExecution& e) noexcept override
    {
        Owner::ProducerSubmitted(e);
        submittedOnce = true;
        if (write)
        {
            // Replay overwrites previously good contents too. Clear trust before
            // completion Signal so failure cannot expose the old produced flag.
            generation->produced = false;
            generation->contentUncertain = true;
        }
    }
    void Executed(const DlssNr::Submission::RecordingExecution& e) noexcept override
    {
        if (accessReserved) { --generation->reservedAccesses; accessReserved = false; }
        Owner::Executed(e);
        if (!e.producerSubmitted && !e.continuationSubmitted) return;
        submittedOnce = true;
        if (!write && selection == Selection::NoHistory) return;
        auto& g = *generation;
        if (FAILED(e.status) || !e.fence || !e.fenceValue || e.fenceValue == UINT64_MAX)
        {
            g.uncertain = true;
            if (write) { g.produced = false; g.contentUncertain = true; }
            return;
        }
        const auto found = std::find_if(g.accesses.begin(), g.accesses.end(),
            [&](const auto& p) { return p.fence.Get() == e.fence; });
        if (found == g.accesses.end()) g.accesses.push_back({ e.fence, e.fenceValue });
        else found->value = (std::max)(found->value, e.fenceValue);
        if (write) { g.produced = true; g.contentUncertain = false; }
    }
    void Invalidated(DlssNr::Submission::RecordingIdentity id) noexcept override
    {
        if (write && identity == id && !submittedOnce) generation->discarded = true;
        Owner::Invalidated(id);
    }
};
} // namespace FSRD::CompositionHistory
