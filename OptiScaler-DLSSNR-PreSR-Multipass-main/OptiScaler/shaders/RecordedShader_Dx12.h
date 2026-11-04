#pragma once
#include <dlssnr/submission/CommandListProxy.h>
#include <shaders/Shader_Dx12Utils.h>
#include <gpu_time/GpuTime_Dx12.h>
#include <wrl/client.h>
#include <algorithm>
#include <memory>
#include <vector>

// The recording owns its observer. Submitted observers additionally retain
// themselves until invalidation AND completion (or observed device removal).
namespace RecordedShader
{
using Microsoft::WRL::ComPtr;
namespace Submission = DlssNr::Submission;

inline bool Supported(ID3D12GraphicsCommandList* list) noexcept
{
    if (!list) return false;
    ComPtr<Submission::ILogicalCommandList> logical;
    ComPtr<Submission::IRecordingResources> resources;
    return SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&logical))) && logical &&
           SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&resources))) && resources && resources->CanAppendCompute();
}

struct Owner : Submission::RecordingObserver, std::enable_shared_from_this<Owner>
{
    struct Proof { ComPtr<ID3D12Fence> fence; UINT64 value = 0; };
    Submission::RecordingIdentity identity {};
    ComPtr<ID3D12Device> device;
    std::vector<Proof> proofs;
    bool invalidated = true, uncertain = false;
    std::shared_ptr<Owner> submitted;
    PTP_TIMER timer = nullptr;
    PTP_WAIT removalWait = nullptr;
    HANDLE removalEvent = nullptr;
    ComPtr<ID3D12Fence> removalFence;
    HMODULE module = nullptr;

    virtual ~Owner()
    {
        if (timer) CloseThreadpoolTimer(timer);
        if (removalWait) CloseThreadpoolWait(removalWait);
        if (removalEvent) CloseHandle(removalEvent);
        if (module) FreeLibrary(module);
    }
    virtual void OnRetired() noexcept {}
    bool Complete() noexcept
    {
        proofs.erase(std::remove_if(proofs.begin(), proofs.end(), [](const Proof& p) {
            const auto value = p.fence->GetCompletedValue();
            return value != UINT64_MAX && value >= p.value;
        }), proofs.end());
        // Device removal is not a successful fence proof, but makes execution impossible.
        return FAILED(device->GetDeviceRemovedReason()) || (!uncertain && proofs.empty());
    }
    bool Available() noexcept { return invalidated && Complete() && !submitted; }
    bool Attach(ID3D12GraphicsCommandList* list)
    {
        ComPtr<Submission::ILogicalCommandList> logical;
        ComPtr<Submission::IRecordingResources> resources;
        if (!Available() || !Supported(list) || FAILED(list->QueryInterface(IID_PPV_ARGS(&logical))) ||
            FAILED(list->QueryInterface(IID_PPV_ARGS(&resources)))) return false;
        identity = logical->Identity();
        invalidated = false;
        if (FAILED(resources->ObserveResources(shared_from_this()))) { invalidated = true; return false; }
        return true;
    }
    HRESULT BeforeExecute(const Submission::RecordingExecution& e) noexcept override
    {
        if (invalidated || !(identity == e.identity) || uncertain || !e.queue) return E_UNEXPECTED;
        Complete();
        // A replay may run on another queue. Serialize writes to intermediates and
        // timing queries without CPU waits or assuming first-submission retirement.
        for (const auto& p : proofs) {
            const HRESULT hr = e.queue->Wait(p.fence.Get(), p.value);
            if (FAILED(hr)) return hr;
        }
        try { proofs.reserve(proofs.size() + 1); }
        catch (...) { return E_OUTOFMEMORY; } // Before any GPU submission.
        return S_OK;
    }
    void ProducerSubmitted(const Submission::RecordingExecution&) noexcept override
    {
        uncertain = true;
        submitted = shared_from_this();
    }
    void Executed(const Submission::RecordingExecution& e) noexcept override
    {
        const bool submittedGpu = e.producerSubmitted || e.continuationSubmitted;
        if (!submittedGpu) return;
        submitted = shared_from_this();
        uncertain = true;
        if (FAILED(e.status) || !e.fence || !e.fenceValue || e.fenceValue == UINT64_MAX) return;
        auto it = std::find_if(proofs.begin(), proofs.end(), [&](const Proof& p) { return p.fence.Get() == e.fence; });
        if (it != proofs.end()) it->value = (std::max)(it->value, e.fenceValue);
        else proofs.push_back({e.fence, e.fenceValue}); // Capacity admitted in BeforeExecute.
        uncertain = false;
        if (invalidated) Retire();
    }
    static void Arm(PTP_TIMER timer) noexcept
    {
        LARGE_INTEGER due {}; due.QuadPart = -100000;
        FILETIME when {due.LowPart, DWORD(due.HighPart)};
        SetThreadpoolTimer(timer, &when, 0, 0);
    }
    static void CALLBACK Collect(PTP_CALLBACK_INSTANCE instance, void* context, PTP_TIMER timer)
    {
        std::lock_guard lock(Submission::RecordingMutex());
        auto* owner = static_cast<Owner*>(context);
        if (!owner->Complete()) { Arm(timer); return; }
        auto keep = std::move(owner->submitted);
        auto module = owner->module;
        owner->module = nullptr;
        owner->timer = nullptr;
        CloseThreadpoolTimer(timer);
        owner->OnRetired();
        keep.reset();
        if (module) FreeLibraryWhenCallbackReturns(instance, module);
    }
    static void CALLBACK Removed(PTP_CALLBACK_INSTANCE instance, void* context, PTP_WAIT wait, TP_WAIT_RESULT)
    {
        std::lock_guard lock(Submission::RecordingMutex());
        auto* owner = static_cast<Owner*>(context);
        // The private fence is never signaled by us. UINT64_MAX observes device
        // removal, not successful GPU completion; still require the device fact.
        if (SUCCEEDED(owner->device->GetDeviceRemovedReason())) return;
        auto keep = std::move(owner->submitted);
        auto module = owner->module;
        owner->module = nullptr;
        owner->removalWait = nullptr;
        CloseThreadpoolWait(wait);
        CloseHandle(owner->removalEvent);
        owner->removalEvent = nullptr;
        owner->removalFence.Reset();
        owner->OnRetired();
        keep.reset();
        if (module) FreeLibraryWhenCallbackReturns(instance, module);
    }
    void RetainUntilRemoval() noexcept
    {
        // A failed completion Signal can never become proof on a healthy
        // device, and replay is prohibited. Quarantine this self-owned closure
        // and module until device removal. One fence/event/one-shot wait per
        // owner is bounded storage: no timer, worker thread, or recurring poll.
        // Allocation/registration failure retains the same closure and module
        // indefinitely rather than freeing potentially GPU-live resources.
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removalFence)))) return;
        removalEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!removalEvent) { removalFence.Reset(); return; }
        removalWait = CreateThreadpoolWait(&Removed, this, nullptr);
        if (!removalWait || FAILED(removalFence->SetEventOnCompletion(UINT64_MAX, removalEvent))) {
            if (removalWait) CloseThreadpoolWait(removalWait);
            removalWait = nullptr;
            CloseHandle(removalEvent); removalEvent = nullptr;
            removalFence.Reset();
            return;
        }
        SetThreadpoolWait(removalWait, removalEvent, nullptr);
    }
    void Retire() noexcept
    {
        if (!submitted || timer || removalWait) return; // A callback owns the self-reference.
        if (Complete()) { auto keep = std::move(submitted); OnRetired(); return; }
        if (!module && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&Collect), &module)) return; // Keep self on failure, never force-retire.
        if (uncertain) { RetainUntilRemoval(); return; }
        timer = CreateThreadpoolTimer(&Collect, this, nullptr);
        if (!timer) return; // Keep both the closure and module if collection cannot be scheduled.
        Arm(timer);
    }
    void Invalidated(Submission::RecordingIdentity id) noexcept override
    {
        if (!(identity == id)) return;
        invalidated = true;
        Retire();
    }
};

struct Intermediate final : Owner
{
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

// Resource-local weak metadata lets another helper retain the intermediate's
// reuse lease, not just its COM allocation. No resource/owner cycle or registry.
inline constexpr GUID IntermediateKey = {0x6de5b942, 0x14c0, 0x4fad, {0x97, 0x25, 0x4f, 0x37, 0x8a, 0xa4, 0x72, 0x01}};
struct IntermediateLink final : IUnknown
{
    std::atomic<ULONG> refs {1};
    std::weak_ptr<Intermediate> owner;
    explicit IntermediateLink(const std::shared_ptr<Intermediate>& value) : owner(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs; if (!n) delete this; return n; }
};
inline std::shared_ptr<Intermediate> IntermediateOf(ID3D12Resource* resource)
{
    IntermediateLink* link = nullptr;
    UINT size = sizeof(link);
    if (FAILED(resource->GetPrivateData(IntermediateKey, &size, &link)) || !link) return {};
    auto owner = link->owner.lock();
    link->Release(); // GetPrivateData AddRefs stored IUnknown interfaces.
    return owner;
}

struct Dispatch final : Owner
{
    FrameDescriptorHeap heap;
    ComPtr<ID3D12Resource> constants;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    std::vector<ComPtr<ID3D12Resource>> resources;
    std::vector<std::shared_ptr<Intermediate>> intermediates;
    std::unique_ptr<GpuTime_Dx12> timing;
    Dispatch() { heap.SetDeferredRetirement(false); resources.reserve(4); intermediates.reserve(4); }
    void OnRetired() noexcept override { resources.clear(); intermediates.clear(); }
};
} // namespace RecordedShader
