#pragma once
#include <shaders/RecordedShader_Dx12.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// At most four spaced captures per feature, only on rejected camera input.
// Observed depth extrema never become authoritative camera clipping planes.
class FSRDRawDepthProbe
{
    struct Capture final : RecordedShader::Owner
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> source, readback;
        UINT width = 0, height = 0, pitch = 0;
        bool observedSubmission = false;
        void Executed(const DlssNr::Submission::RecordingExecution& e) noexcept override
        {
            Owner::Executed(e);
            observedSubmission |= e.producerSubmitted || e.continuationSubmitted;
        }
    };
    std::shared_ptr<Capture> pending;
    unsigned attempts = 0;
    ULONGLONG nextAttempt = 0;
public:
    ~FSRDRawDepthProbe() { Disarm(); }
    void Disarm() { DlssNr::Submission::DepthProvenance::Disarm(this); }
    void Poll(bool diagnosticsEnabled)
    {
        if (!diagnosticsEnabled) Disarm();
        if (!pending) return;
        std::lock_guard lock(DlssNr::Submission::RecordingMutex());
        if (!pending || (pending->invalidated && !pending->observedSubmission && !pending->uncertain)) { pending.reset(); return; }
        if (!pending->observedSubmission || pending->uncertain || !pending->Complete()) return;
        auto c = std::move(pending);
        // Disabling skips CPU readback analysis, never drops an uncompleted capture.
        // The recording/self-owned observer still protects replay and GPU lifetime.
        if (!diagnosticsEnabled) return;
        if (FAILED(c->device->GetDeviceRemovedReason())) return;
        void* mapped = nullptr;
        D3D12_RANGE range {0, SIZE_T(c->pitch) * c->height};
        if (FAILED(c->readback->Map(0, &range, &mapped)))
        { LOG_ERROR("[RR_RAW_DEPTH] readback Map failed"); return; }
        std::vector<float> positive;
        positive.reserve(size_t(c->width) * c->height);
        size_t zero = 0, negative = 0, invalid = 0, belowTen = 0, nan = 0, infinity = 0;
        uint32_t firstBits = 0, centerBits = 0;
        for (UINT y = 0; y < c->height; ++y)
            for (UINT x = 0; x < c->width; ++x)
            {
                float v;
                std::memcpy(&v, static_cast<const char*>(mapped) + size_t(y) * c->pitch + x * 4, 4);
                uint32_t bits;
                std::memcpy(&bits, &v, sizeof(bits));
                if (x == 0 && y == 0) firstBits = bits;
                if (x == c->width/2 && y == c->height/2) centerBits = bits;
                if (!std::isfinite(v)) { ++invalid; if (std::isnan(v)) ++nan; else ++infinity; continue; }
                if (v == 0) { ++zero; continue; }
                if (v < 0) ++negative;
                const float distance = std::abs(v);
                positive.push_back(distance);
                if (distance < 10) ++belowTen;
            }
        D3D12_RANGE written {0,0};
        c->readback->Unmap(0, &written);
        std::sort(positive.begin(), positive.end());
        const auto percentile = [&](double q) { return positive.empty() ? 0.0f : positive[size_t(q * (positive.size()-1))]; };
        LOG_WARN("[RR_RAW_DEPTH] BEFORE CLAMP {}x{} finiteNonzero={} zero={} negative={} nonfinite={} "
                 "absMin={} p01={} p10={} p50={} p90={} p99={} absMax={} absBelow10={} fractionBelow10={}; "
                 "all finite nonzero samples, sky/sentinel not classified; not authoritative cameraNear",
                 c->width, c->height, positive.size(), zero, negative, invalid,
                 percentile(0), percentile(.01), percentile(.1), percentile(.5), percentile(.9), percentile(.99),
                 percentile(1), belowTen, positive.empty() ? 0.0 : double(belowTen)/positive.size());
        LOG_WARN("[RR_RAW_DEPTH] sample={} nan={} infinity={} firstBits={:#010x} centerBits={:#010x}; "
                 "next sample is bounded and delayed, no validity inferred from sentinels",
                 attempts, nan, infinity, firstBits, centerBits);
    }
    void Record(ID3D12GraphicsCommandList* list, ID3D12Resource* source, UINT x, UINT y, UINT width, UINT height,
                bool hardwareDepth, bool taggedLinear)
    {
        if (pending || attempts >= 4 || GetTickCount64() < nextAttempt || !list || !source || !width || !height) return;
        ++attempts;
        const ULONGLONG delays[] {500, 1500, 3000, 0};
        nextAttempt = GetTickCount64() + delays[attempts-1];
        const auto desc = source->GetDesc();
        DlssNr::Submission::DepthProvenance::Arm(source, this);
        Microsoft::WRL::ComPtr<DlssNr::Submission::ILogicalCommandList> logical;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&logical))))
            DlssNr::Submission::DepthProvenance::Event("evaluate_copy",logical->Identity(),source,attempts);
        LOG_WARN("[RR_RAW_DEPTH] effectiveHardware={} taggedLinear={} sourceFormat={} sourceFlags={:#x}; "
                 "raw values are not interpreted as physical clipping metadata",
                 hardwareDepth, taggedLinear, UINT(desc.Format), UINT(desc.Flags));
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Format != DXGI_FORMAT_R32_FLOAT ||
            desc.SampleDesc.Count != 1 || UINT64(x)+width > desc.Width || UINT64(y)+height > desc.Height ||
            UINT64(width)*height > 16777216)
        { LOG_WARN("[RR_RAW_DEPTH] unsupported bounded copy format={} extent={}x{}", UINT(desc.Format), width, height); return; }
        auto c = std::make_shared<Capture>();
        if (FAILED(source->GetDevice(IID_PPV_ARGS(&c->device)))) return;
        c->source = source; c->width = width; c->height = height;
        c->pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT-1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT-1);
        D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer {}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = UINT64(c->pitch)*height; buffer.Height = 1; buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(c->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&c->readback))))
        { LOG_WARN("[RR_RAW_DEPTH] allocation failed"); return; }
        Microsoft::WRL::ComPtr<DlssNr::Submission::IRecordingResources> recorder;
        if (FAILED(list->QueryInterface(IID_PPV_ARGS(&recorder)))) return;
        auto trace = std::make_shared<DlssNr::Submission::DepthProvenance::Submission>();
        trace->resource = source;
        if (FAILED(recorder->ObserveResources(trace)))
            LOG_WARN("[RR_DEPTH_TRACE] execution observer unavailable");
        ID3D12Resource* resources[] { source };
        D3D12_RESOURCE_STATES states[] { D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        const HRESULT stateHr = recorder->BeginDirectState(1, resources, states);
        if (FAILED(stateHr))
        { LOG_WARN("[RR_RAW_DEPTH] state unavailable: {}", recorder->InlineRejectionReason()); return; }
        if (!c->Attach(list)) { recorder->EndDirectState(); LOG_WARN("[RR_RAW_DEPTH] recording observer admission unavailable"); return; }
        D3D12_RESOURCE_BARRIER barrier {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, states[0], D3D12_RESOURCE_STATE_COPY_SOURCE};
        if (states[0] != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = source; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = c->readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT,width,height,1,c->pitch};
        D3D12_BOX region {x,y,0,x+width,y+height,1};
        list->CopyTextureRegion(&dst,0,0,0,&src,&region);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        if (states[0] != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &barrier);
        recorder->EndDirectState();
        pending = std::move(c);
        LOG_WARN("[RR_RAW_DEPTH] queued one pre-clamp copy origin=[{},{}] extent={}x{} state={:#x}", x,y,width,height,UINT(states[0]));
    }
};
