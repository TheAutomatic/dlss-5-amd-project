#pragma once
#include <d3d12.h>
#include <cstdint>
#include <mutex>
#include <memory>
#include <vector>

namespace DlssNr::Submission
{
// One lock order for Record, Execute, successful Reset and final Release.
// Observers may transfer ownership here, but must never wait for GPU completion.
inline std::recursive_mutex& RecordingMutex()
{
    static std::recursive_mutex mutex;
    return mutex;
}

struct RecordingIdentity
{
    uint64_t list = 0;
    uint64_t generation = 0;
    bool operator==(const RecordingIdentity& other) const
    {
        return list == other.list && generation == other.generation;
    }
};

struct RecordingExecution
{
    RecordingIdentity identity;
    uint64_t serial = 0;
    ID3D12CommandQueue* queue = nullptr;
    bool producerSubmitted = false;
    bool continuationSubmitted = false;
    HRESULT status = E_PENDING;
    ID3D12Fence* fence = nullptr; // Borrowed only during the callback.
    UINT64 fenceValue = 0;
};

// A recording owns its observer, never the reverse: no COM reference to the
// proxy is allowed here. Old generations cannot be revived by address reuse.
struct RecordingObserver
{
    virtual ~RecordingObserver() = default;
    virtual HRESULT BeforeExecute(const RecordingExecution&) noexcept { return S_OK; }
    virtual void ProducerSubmitted(const RecordingExecution&) noexcept {}
    virtual void Between(const RecordingExecution&) noexcept {}
    virtual void Executed(const RecordingExecution&) noexcept {}
    virtual void Invalidated(RecordingIdentity) noexcept = 0;
};
// One NR job remains exclusive. Independent post-processing owners share its
// submission facts without replacing that job or owning the proxy itself.
struct RecordingObservers final : RecordingObserver
{
    std::shared_ptr<RecordingObserver> job;
    std::vector<std::shared_ptr<RecordingObserver>> resources;
    HRESULT BeforeExecute(const RecordingExecution& e) noexcept override
    {
        if (job) { const auto hr = job->BeforeExecute(e); if (FAILED(hr)) return hr; }
        for (auto& r : resources) { const auto hr = r->BeforeExecute(e); if (FAILED(hr)) return hr; }
        return S_OK;
    }
    void ProducerSubmitted(const RecordingExecution& e) noexcept override
    { if (job) job->ProducerSubmitted(e); for (auto& r : resources) r->ProducerSubmitted(e); }
    void Between(const RecordingExecution& e) noexcept override
    { if (job) job->Between(e); for (auto& r : resources) r->Between(e); }
    void Executed(const RecordingExecution& e) noexcept override
    { if (job) job->Executed(e); for (auto& r : resources) r->Executed(e); }
    void Invalidated(RecordingIdentity id) noexcept override
    { if (job) job->Invalidated(id); for (auto& r : resources) r->Invalidated(id); }
};
} // namespace DlssNr::Submission
