#pragma once
#include <d3d12.h>
#include <cstdint>
#include <mutex>

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
} // namespace DlssNr::Submission
