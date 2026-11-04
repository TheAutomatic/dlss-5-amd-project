#pragma once
#include <d3d12.h>
#include "../upscalers/fsr31/FSRDRetryPolicy.h"
#include <dlssnr/submission/GpuBatchTracker.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// One immutable query allocation per sampled evaluation, owned locally and by the
// recording observer. Only an explicit completed GpuBatch receipt permits mapping.
class FSRDStageTimings
{
  public:
    enum Stage : unsigned
    {
        Floor,
        Conversion,
        RayRegeneration,
        AlbedoRecovery,
        Composition,
        SuperResolution,
        Count
    };
    static constexpr const char* Names[] { "Floor",           "Conversion",  "AMD RR / ML",
                                           "Albedo Bleed Fix", "Composition", "Super Resolution" };
    struct Snapshot
    {
        std::array<double, Count> milliseconds {};
        unsigned validMask = 0;
        uint64_t sequence = 0;
        std::chrono::steady_clock::time_point completed {};
        bool available = true;
    };

  private:
    struct Shared
    {
        std::mutex mutex;
        Snapshot snapshot;
        std::atomic<uint64_t> generation { 0 };
    };
    struct Packet final : DlssNr::Submission::RecordingObserver
    {
        std::shared_ptr<Shared> owner;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        std::atomic<uint64_t> frequency { 0 };
        std::atomic<unsigned> submissions { 0 };
        uint64_t sequence = 0, generation = 0;
        unsigned started = 0, ended = 0;
        bool successful = false;
        DlssNr::Submission::GpuBatch::Tracker::Ticket ticket = DlssNr::Submission::GpuBatch::kInvalidTicket;
        DlssNr::Submission::GpuBatch::Tracker::CompletionReceiptPtr receipt;
        HRESULT BeforeExecute(const DlssNr::Submission::RecordingExecution& execution) noexcept override
        {
            ++submissions;
            uint64_t clock = 0;
            if (execution.queue && SUCCEEDED(execution.queue->GetTimestampFrequency(&clock)))
                frequency.store(clock);
            else
                frequency.store(0);
            return S_OK;
        }
        void Invalidated(DlssNr::Submission::RecordingIdentity) noexcept override {}

        explicit Packet(std::shared_ptr<Shared> state) : owner(std::move(state)) {}
        void Publish()
        {
            if (successful && ended && submissions.load() == 1 && frequency.load() &&
                owner->generation.load() == generation && device && readback &&
                SUCCEEDED(device->GetDeviceRemovedReason()))
            {
                D3D12_RANGE range { 0, Count * 2 * sizeof(uint64_t) };
                void* mapped = nullptr;
                if (SUCCEEDED(readback->Map(0, &range, &mapped)))
                {
                    const auto* values = static_cast<const uint64_t*>(mapped);
                    Snapshot sample;
                    sample.sequence = sequence;
                    sample.completed = std::chrono::steady_clock::now();
                    for (unsigned i = 0; i < Count; ++i)
                    {
                        if (!(ended & (1u << i)) || values[2 * i + 1] < values[2 * i])
                            continue;
                        const double ms = double(values[2 * i + 1] - values[2 * i]) * 1000.0 / double(frequency.load());
                        if (std::isfinite(ms))
                        {
                            sample.milliseconds[i] = ms;
                            sample.validMask |= 1u << i;
                        }
                    }
                    D3D12_RANGE written { 0, 0 };
                    readback->Unmap(0, &written);
                    std::lock_guard lock(owner->mutex);
                    if (sample.sequence > owner->snapshot.sequence && owner->generation.load() == generation)
                        owner->snapshot = sample;
                }
            }
        }
    };
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    std::shared_ptr<Packet> m_active;
    std::vector<std::shared_ptr<Packet>> m_pending;
    void Collect()
    {
        std::lock_guard recording(DlssNr::Submission::RecordingMutex());
        auto& tracker = DlssNr::Submission::GpuBatch::Tracker::Instance();
        using Outcome = DlssNr::Submission::GpuBatch::Tracker::CompletionReceipt::Outcome;
        for (auto it = m_pending.begin(); it != m_pending.end();)
        {
            auto& packet = **it;
            tracker.StatusOf(packet.ticket);
            const auto outcome = packet.receipt->outcome.load(std::memory_order_acquire);
            if (outcome == Outcome::Outstanding) { ++it; continue; }
            if (outcome == Outcome::Completed) packet.Publish();
            it = m_pending.erase(it);
        }
    }
    ID3D12GraphicsCommandList* m_list = nullptr;
    uint64_t m_sequence = 0;
    bool m_enabled = false;

  public:
    // Recording ownership comes from the local command-list observer, not a foreign lease.
    void BeginFrame(ID3D12Device* device, ID3D12GraphicsCommandList* list, bool enabled)
    {
        FinishFrame(false);
        Collect();
        if (enabled != m_enabled)
        {
            m_enabled = enabled;
            ++m_shared->generation;
            std::lock_guard lock(m_shared->mutex);
            m_shared->snapshot = {};
        }
        const uint64_t sequence = ++m_sequence;
        if (!enabled || !device || !list || (sequence - 1) % 16 || m_pending.size() >= 4)
            return;
        auto packet = std::make_shared<Packet>(m_shared);
        packet->device = device;
        packet->sequence = sequence;
        packet->generation = m_shared->generation.load();
        D3D12_QUERY_HEAP_DESC queryDesc { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, Count * 2, 0 };
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = Count * 2 * sizeof(uint64_t);
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&packet->queries))) ||
            FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&packet->readback))))
        {
            std::lock_guard lock(m_shared->mutex);
            m_shared->snapshot.available = false;
            return;
        }
        auto& tracker = DlssNr::Submission::GpuBatch::Tracker::Instance();
        packet->ticket = tracker.Record(list);
        packet->receipt = tracker.ObserveCompletion(packet->ticket);
        Microsoft::WRL::ComPtr<DlssNr::Submission::IRecordingResources> resources;
        if (!packet->receipt || FAILED(list->QueryInterface(IID_PPV_ARGS(&resources))) ||
            FAILED(resources->ObserveResources(packet)))
        {
            std::lock_guard lock(m_shared->mutex);
            m_shared->snapshot.available = false;
            return;
        }
        m_pending.push_back(packet);
        m_list = list;
        m_active = std::move(packet);
    }
    bool Begin(Stage stage)
    {
        if (!m_active || (m_active->started & (1u << stage)))
            return false;
        m_list->EndQuery(m_active->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * stage);
        m_active->started |= 1u << stage;
        return true;
    }
    void End(Stage stage)
    {
        if (!m_active || !(m_active->started & (1u << stage)))
            return;
        m_list->EndQuery(m_active->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * stage + 1);
        m_list->ResolveQueryData(m_active->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * stage, 2,
                                 m_active->readback.Get(), 2 * stage * sizeof(uint64_t));
        m_active->ended |= 1u << stage;
    }
    void FinishFrame(bool successful)
    {
        if (m_active)
            m_active->successful = successful;
        m_active.reset();
        m_list = nullptr;
        Collect();
    }
    Snapshot GetSnapshot() const
    {
        std::lock_guard lock(m_shared->mutex);
        return m_shared->snapshot;
    }
    struct Scope
    {
        FSRDStageTimings* timer;
        Stage stage;
        bool started;
        Scope(FSRDStageTimings* owner, Stage value) : timer(owner), stage(value), started(owner && owner->Begin(value))
        {
        }
        ~Scope()
        {
            if (started)
                timer->End(stage);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
};

struct FSRDRuntimeSnapshot
{
    enum Step { Inputs, Floor, Conversion, RayRegeneration, AlbedoRecovery, Composition, SuperResolution, Output, StepCount };
    enum Status { NotRun, Running, Passed, Failed, Disabled };
    static constexpr const char* StepNames[] = { "Input validation", "Floor", "Conversion", "Ray Regeneration",
        "Albedo Bleed Fix", "Composition", "FSR Super Resolution", "Output" };
    enum Input { Color, Depth, Motion, Normals, Roughness, DiffuseAlbedo, SpecularAlbedo,
                 SpecularDistance, DiffuseDistance, Bias, Emissive, LinearDepth, Responsivity,
                 AmbientOcclusion, Exposure, InputCount };
    static constexpr const char* InputNames[] = { "Color / radiance", "Depth", "Motion vectors", "Normals",
        "Roughness", "Diffuse albedo", "Specular albedo", "Specular hit distance", "Diffuse hit distance",
        "Current-color bias", "Emissive", "Title linear depth", "Responsivity", "Ambient occlusion", "Exposure" };
    static constexpr uint32_t Bit(Input input) { return 1u << input; }
    std::array<Status, StepCount> steps {};
    Step activeStep = Inputs;
    uint32_t received = 0, prepared = 0, submitted = 0;
    bool rayReconstruction = false, nativeAvailable = false, nativeActive = false, fallback = false;
    bool nativeRRPreferred = false, gameNativeRequested = false, rrValidated = false;
    bool rrDispatched = false, success = false;
    uint64_t frame = 0;
    FSRD::RRResult recoveryResult = FSRD::RRResult::Success;
    uint32_t retryFramesRemaining = 0;
    std::string failure;
    std::chrono::steady_clock::time_point updated {};
    uint32_t signalStatus = 0;
    FSRDStageTimings::Snapshot timings;
    void Begin(Step step) { activeStep = step; steps[step] = Running; }
    void Complete(Step step) { steps[step] = Passed; }
};
