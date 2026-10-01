#pragma once
#include <windows.h>
#include <d3d12.h>
#include <memory>
#include <array>
#include <cstring>
#include "LmxxfRecordingLease.h"
#include "../../NrPerformanceStore.h"

namespace LmxxfRuntime
{
// A query heap belongs to one immutable recording. GPU replay is ordered by
// its existing chain completion contract. CPU reads happen only before a new
// execution or after the latest submitted execution's completion certificate.
class RecordingGpuTiming
{
    ID3D12QueryHeap* queries = nullptr;
    ID3D12Resource* readback = nullptr;
    std::shared_ptr<RecordingCompletion> completion;
    unsigned recorded = 0, submitted = 0;
    unsigned stages[2] {NR_GPU_ENCODE, NR_GPU_DECODE};
    uint64_t frame = 0, execution = 0, epoch = 0, frequency = 0;
public:
    ~RecordingGpuTiming() { if (readback) readback->Release(); if (queries) queries->Release(); }
    static std::shared_ptr<RecordingGpuTiming> Create(ID3D12Device* device, unsigned first = NR_GPU_ENCODE, unsigned second = NR_GPU_DECODE) noexcept
    {
        try {
            auto out = std::make_shared<RecordingGpuTiming>();
            out->stages[0] = first; out->stages[1] = second;
            D3D12_QUERY_HEAP_DESC query {};
            query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            query.Count = 4;
            if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&out->queries)))) return {};
            D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 4 * sizeof(uint64_t);
            desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out->readback)))) return {};
            return out;
        } catch (...) { return {}; }
    }
    // Only the pool may call this after every recording owner has released us.
    void ResetRecording() { completion.reset(); recorded = submitted = 0; }
    void Begin(ID3D12GraphicsCommandList* list, unsigned stage)
    {
        list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, stage * 2);
    }
    void End(ID3D12GraphicsCommandList* list, unsigned stage)
    {
        list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, stage * 2 + 1);
        list->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, stage * 2, 2, readback, stage * 16);
        recorded |= 1u << stage;
    }
    void Collect(DlssNr::PerformanceStore& store)
    {
        if (!completion || !completion->Complete()) return;
        void* mapped = nullptr;
        D3D12_RANGE range {0, 4 * sizeof(uint64_t)};
        if (frequency && SUCCEEDED(readback->Map(0, &range, &mapped))) {
            uint64_t ticks[4]; std::memcpy(ticks, mapped, sizeof ticks);
            D3D12_RANGE written {0, 0}; readback->Unmap(0, &written);
            for (unsigned i = 0; i < 2; ++i) if (submitted & (1u << i)) {
                if (ticks[i*2+1] >= ticks[i*2])
                    store.Record(stages[i],
                        double(ticks[i*2+1] - ticks[i*2]) * 1000.0 / double(frequency),
                        GetTickCount64(), frame, execution, epoch);
                else store.Drop(1);
            }
        } else store.Drop(1);
        completion.reset(); submitted = 0;
    }
    void BeforeExecution(DlssNr::PerformanceStore& store)
    {
        Collect(store);
        // A replay may overwrite the readback before the CPU can collect it.
        // Drop the old measurement before submission rather than racing its write.
        if (completion) { store.Drop(1); completion.reset(); submitted = 0; }
    }
    void Submitted(std::shared_ptr<RecordingCompletion> proof, bool consumer,
                   uint64_t frameId, uint64_t executionId, uint64_t enableEpoch)
    {
        frequency = 0;
        if (FAILED(proof->queue->GetTimestampFrequency(&frequency))) frequency = 0;
        frame = frameId; execution = executionId; epoch = enableEpoch;
        submitted = recorded & (consumer ? 3u : 1u);
        completion = std::move(proof);
    }
};

class RecordingGpuTimingPool
{
    std::array<std::shared_ptr<RecordingGpuTiming>, 16> slots {};
public:
    std::shared_ptr<RecordingGpuTiming> Acquire(ID3D12Device* device) noexcept
    {
        for (auto& slot : slots) {
            if (!slot) { slot = RecordingGpuTiming::Create(device); if (!slot) return {}; }
            if (slot && slot.use_count() == 1) {
                slot->ResetRecording();
                return slot;
            }
        }
        return {};
    }
};
}
