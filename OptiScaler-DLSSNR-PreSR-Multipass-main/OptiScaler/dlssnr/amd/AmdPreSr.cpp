#include "AmdPreSr.h"
#include "AmdLayout.h"
#ifdef AMD_RETIRE_DIAGNOSTICS
#include "RetirementDiagnostics.h"
#endif
#include "RuntimeNotification.h"
#include "SubmissionState.h"
#include "ColorEncoding.h"
#include "AmdLookShader.h"
#include "RtgiNative.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <cmath>

using Microsoft::WRL::ComPtr;
namespace AmdPreSr
{
namespace
{
template <class T> T& At(HMODULE h, size_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(h) + rva); }
void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned>(hr)));
}
void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (!r || a == b)
        return;
    D3D12_RESOURCE_BARRIER v {};
    v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    c->ResourceBarrier(1, &v);
}
struct Packet
{
    ID3D12GraphicsCommandList* list;
    ID3D12Resource* colour;
    UINT colourState, pad14;
    ID3D12Resource* motion;
    UINT motionState, pad24;
    ID3D12Resource* depth;
    UINT depthState, pad34;
    ID3D12Resource* exposure;
    UINT exposureState;
    float scaleX, scaleY;
    UINT pad4c;
};
static_assert(sizeof(Packet) == 0x50 && offsetof(Packet, scaleX) == 0x44);
using InitFn = bool(__fastcall*)(void*, const std::string*);
using RecordFn = void(__fastcall*)(Packet*);
using NotifyFn = void(__fastcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using HipSetFn = int (*)(int);
constexpr char CopyShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; uint sourceW; uint sourceH; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=w || p.y>=h)return;
 if(w==sourceW && h==sourceH){dst[p.xy]=src.Load(int3(p.xy,0));return;}
 // Integrate the entire source pixel footprint. A single bilinear sample aliases
 // narrow emissive lines when the model runs far below the input resolution.
 float2 lo=float2(p.xy)*float2(sourceW,sourceH)/float2(w,h);
 float2 hi=float2(p.xy+1)*float2(sourceW,sourceH)/float2(w,h);
 int2 first=int2(floor(lo)); float4 sum=0;float total=0;
 [loop]for(int y=first.y;y<int(ceil(hi.y));++y)
 [loop]for(int x=first.x;x<int(ceil(hi.x));++x){
  float2 coverage=max(0,min(hi,float2(x+1,y+1))-max(lo,float2(x,y)));
  float weight=coverage.x*coverage.y;
  sum+=src.Load(int3(clamp(int2(x,y),0,int2(sourceW-1,sourceH-1)),0))*weight;total+=weight;
 }
 dst[p.xy]=sum/max(total,1e-6);
})";
constexpr char ResolveShader[] = R"(
Texture2D<float4> src:register(t0);
Texture2D<float4> baseline:register(t1);
Texture2D<float4> edited:register(t2);
RWTexture2D<float4> dst:register(u0);
cbuffer Extent:register(b0){uint w,h,lowW,lowH;};
float3 delta(int2 p){p=clamp(p,0,int2(lowW-1,lowH-1));return edited.Load(int3(p,0)).rgb-baseline.Load(int3(p,0)).rgb;}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){
 if(p.x>=w||p.y>=h)return;
 float2 q=(float2(p.xy)+.5)*float2(lowW,lowH)/float2(w,h)-.5;
 int2 a=int2(floor(q));float2 t=frac(q);
 float3 d=lerp(lerp(delta(a),delta(a+int2(1,0)),t.x),lerp(delta(a+int2(0,1)),delta(a+1),t.x),t.y);
 float4 c=src.Load(int3(p.xy,0));
 // A reduced neural pixel mixes surfaces and small emitters. Suppress its edit
 // where the original pixel disagrees with that footprint, rather than spreading
 // the edit blindly across high-contrast edges. No previous frame is reused.
 int2 hi=int2(lowW-1,lowH-1);
 float3 b=lerp(lerp(baseline.Load(int3(clamp(a,0,hi),0)).rgb,baseline.Load(int3(clamp(a+int2(1,0),0,hi),0)).rgb,t.x),
 lerp(baseline.Load(int3(clamp(a+int2(0,1),0,hi),0)).rgb,baseline.Load(int3(clamp(a+1,0,hi),0)).rgb,t.x),t.y);
 float3 magnitude=max(max(abs(c.rgb),abs(b)),1e-5);
 float mismatch=max(abs(c.r-b.r)/magnitude.r,max(abs(c.g-b.g)/magnitude.g,abs(c.b-b.b)/magnitude.b));
 float confidence=1-smoothstep(.15,.75,mismatch);
 // Keep extreme low-resolution edits bounded relative to the current footprint.
 float3 limit=.5*max(abs(b),abs(c.rgb));
 d=clamp(d,-limit,limit)*confidence;
 dst[p.xy]=float4(clamp(c.rgb+d,0,65504),c.a);
})";
constexpr char DepthShader[] = R"(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; uint sourceW; uint sourceH; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x<w && p.y<h) {
 uint2 q=min(uint2((float2(p.xy)+.5)*float2(sourceW,sourceH)/float2(w,h)),uint2(sourceW-1,sourceH-1));
 dst[p.xy]=src.Load(int3(q,0)); }
})";
constexpr char MotionShader[] = R"(
Texture2D<float2> src : register(t0);
RWTexture2D<float2> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; uint sourceW; uint sourceH; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=w || p.y>=h) return;
 uint2 q=min(uint2((float2(p.xy)+0.5)*float2(sourceW,sourceH)/float2(w,h)),uint2(sourceW-1,sourceH-1));
 // Keep the sampled vector unchanged; convert its pixel scale in the packet.
 dst[p.xy]=src.Load(int3(q,0));
})";
constexpr char ExposureShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; float preExposure; float exposureScale; };
[numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID) {
 float e=src.Load(int3(0,0,0)).r*exposureScale/preExposure;
 dst[uint2(0,0)]=isfinite(e) && e>0 ? e : 1.0;
})";
DXGI_FORMAT DepthReadFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_FLOAT:
        return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
std::string Layout(ID3D12Resource* resource)
{
    auto d = resource->GetDesc();
    return std::to_string(d.Width) + "x" + std::to_string(d.Height) + " format=" + std::to_string(d.Format) +
           " flags=" + std::to_string(d.Flags) + " samples=" + std::to_string(d.SampleDesc.Count) +
           " array=" + std::to_string(d.DepthOrArraySize) + " dimension=" + std::to_string(d.Dimension);
}
const AmdLayout* IdentifyRuntime(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
    BCRYPT_ALG_HANDLE alg {};
    unsigned char digest[32] {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return nullptr;
    auto result = BCryptHash(alg, nullptr, 0, data.data(), static_cast<ULONG>(data.size()), digest, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (result < 0)
        return nullptr;
    for (auto layout : kAmdLayouts)
        if (data.size() == layout->size && std::memcmp(digest, layout->sha256, 32) == 0)
            return layout;
    return nullptr;
}
DXGI_FORMAT ReadFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return f;
    }
}
} // namespace
struct Backend::Impl
{
    std::unique_ptr<ColorEncoding> decode, encode;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> scaleBaseline, scaleOutput;
    ComPtr<ID3D12PipelineState> resolvePipeline;
    ComPtr<ID3D12Resource> motionCrop, depthCrop;
    std::unique_ptr<RtgiNative> rtgi;
    bool rtgiFailed = false;
    std::string rtgiStatus;
    ComPtr<ID3D12Resource> lookColour;
    ComPtr<ID3D12PipelineState> lookPipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12PipelineState> depthPipeline;
    ComPtr<ID3D12PipelineState> motionPipeline, exposurePipeline;
    UINT lastMotionWidth = 0, lastMotionHeight = 0;
    UINT lastInputWidth=0,lastInputHeight=0;
    bool hadExposure = false;
    std::array<HMODULE, 3> runtime {};
    std::array<UINT, 3> observedTimeouts {};
    std::filesystem::path directory;
    std::string status = "AMD pre-SR: not initialized";
    std::atomic<bool> failed { false };
    std::atomic<bool> resetRequested { true };
    Settings lastSettings {};
    bool haveSettings = false;
    UINT64 frames = 0, serial = 0;
    UINT64 lastSubmitted = 0, lastCompleted = 0, completedFrames = 0;
    UINT64 pendingSkips = 0, fenceSkips = 0, fenceRecoveries = 0;
    UINT64 retryAfter = 0, timeoutEvents = 0;
    bool resetAfterTimeout = false;
    // Per-job state and the GPU resources that job borrows. A is a single worker
    // on one HIP stream, so two slots never run concurrently: the extra slot only
    // lets the CPU record the next frame while the previous job is still
    // retiring, instead of blocking the render thread in Submitted.
    // kSlots == 1 reproduces the original one-frame-outstanding behaviour exactly.
#ifdef AMD_MULTISLOT
    static constexpr UINT kSlots = 2;
#else
    static constexpr UINT kSlots = 1;
#endif
    struct Slot
    {
        std::atomic<ID3D12CommandList*> pending { nullptr };
        std::atomic<UINT64> completion { 0 };
        std::array<UINT, 3> jobs {};
        SubmissionState submission;
        // A reads this and writes its correction back into it (in place), so no
        // two outstanding jobs may share one.
        ComPtr<ID3D12Resource> colour;
        ComPtr<ID3D12Resource> exposureCopy;
    };
    std::array<Slot, kSlots> slots;
    UINT activeSlot = 0;
    UINT skipWaits = 0;
    // A joins its workers and clears the abort buffer while it rebuilds staging,
    // which it does after a resize, a re-created upscaler context or an INI
    // change. While that is in flight the extra slot must not be used to skip
    // the Submitted wait - doing so hung the game (exports/design-multislot.md
    // section 4b).
    //
    // A timer cannot guard this: the rebuild happens on whichever later Record
    // A chooses, so any window simply expires first and the crash follows. A
    // publishes its own decision as a sticky byte instead - set when it detects
    // the change, cleared only after it has drained the queue and joined its
    // workers - and a rebuild happens on exactly those calls that read 1 at
    // entry. Reading it is therefore the real guard. See
    // exports/a03-staging-state.md.
    bool NativeRebuilding() const
    {
        if (!L || !L->recreate) return true; // unknown layout: assume the worst
        for (UINT i = 0; i < runtime.size(); ++i)
            if (auto h = runtime[i])
                if (At<volatile uint8_t>(h, L->recreate) != 0) return true;
        return false;
    }
    // True while any slot still owns the resources its job borrowed.
    bool AnySlotBusy() const
    {
        for (size_t k = 0; k < slots.size(); ++k)
            if (slots[k].pending.load(std::memory_order_acquire)) return true;
        return false;
    }
    // Highest fence value any slot is still waiting on.
    UINT64 LatestCompletion() const
    {
        UINT64 value = 0;
        for (size_t k = 0; k < slots.size(); ++k)
            value = (std::max)(value, slots[k].completion.load());
        return value;
    }
    bool deviceLostReported = false;
    UINT width = 0, height = 0, activePasses = 0, lastPasses = 0;
    HipSetFn hipSet = nullptr;
    int hipDevice = -1;
    const AmdLayout* L = nullptr;
    std::mutex lock;
#ifdef AMD_RETIRE_DIAGNOSTICS
    RetirementDiagnostics diagnostics;
#endif
    void Log(const std::string& s)
    {
        status = s;
        std::ofstream out(directory / L"amd_presr.log", std::ios::app);
        out << GetTickCount64() << " " << s << '\n';
    }
    void TraceBoundary(const std::string& reason)
    {
        const auto gpu = fence ? fence->GetCompletedValue() : 0;
        const auto removed = device->GetDeviceRemovedReason();
          // Report the first slot with work outstanding; with one slot that
          // is the only one, so the line keeps the original format.
          const Slot* traced = &slots[0];
          for (size_t k = 0; k < slots.size(); ++k)
              if (slots[k].pending.load(std::memory_order_acquire)) { traced = &slots[k]; break; }
        Log("AMD boundary: " + reason + " pending=" +
            std::to_string(reinterpret_cast<uintptr_t>(traced->pending.load())) +
            " submitted=" + std::to_string(traced->submission.submitted) +
            " recordedAt=" + std::to_string(traced->submission.recordedAt) +
            " submittedAt=" + std::to_string(traced->submission.submittedAt) +
            " fence=" + std::to_string(gpu) + "/" + std::to_string(traced->completion.load()) +
            " deviceHR=" + std::to_string(static_cast<UINT>(removed)) +
            " NR=" + std::to_string(width) + "x" + std::to_string(height));
        for (UINT i = 0; i < runtime.size(); ++i)
            if (auto h = runtime[i])
                Log("AMD boundary pass " + std::to_string(i + 1) + " native=" +
                    std::to_string(At<UINT>(h, L->jobDone)) + "/" + std::to_string(traced->jobs[i]) +
                    " nativePending=" + std::to_string(reinterpret_cast<uintptr_t>(At<void*>(h, L->pendingList))) +
                    " timeouts=" + std::to_string(At<UINT>(h, L->timeoutCount)));
        if (FAILED(removed) && !deviceLostReported)
        {
            deviceLostReported = true;
            failed = true;
            // Read whatever DRED the game/OS collected. Do not change device
            // creation settings or globally enable a debug layer in the game.
            ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
            if (SUCCEEDED(device.As(&dred)))
            {
                D3D12_DRED_PAGE_FAULT_OUTPUT1 fault {};
                const auto hr = dred->GetPageFaultAllocationOutput1(&fault);
                Log("AMD DRED page fault: hr=" + std::to_string(static_cast<UINT>(hr)) +
                    " VA=" + std::to_string(fault.PageFaultVA));
                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs {};
                const auto bh = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
                Log("AMD DRED breadcrumbs: hr=" + std::to_string(static_cast<UINT>(bh)));
                UINT count = 0;
                for (auto node = breadcrumbs.pHeadAutoBreadcrumbNode; node && count++ < 16; node = node->pNext)
                    Log("AMD DRED list=" + std::to_string(reinterpret_cast<uintptr_t>(node->pCommandList)) +
                        " progress=" + std::to_string(node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0) +
                        "/" + std::to_string(node->BreadcrumbCount));
            }
        }
    }
    // Retire one slot if its job has finished. Called with `lock` held. Keep
    // every borrowed resource alive until BOTH native inference and the actual
    // D3D12 submission have retired.
    void RetireSlot(UINT k, bool waitForGpu, const char* source
#ifdef AMD_RETIRE_DIAGNOSTICS
                    , RetirementDiagnostics::Event* sample
#endif
                    )
    {
        Slot& sl = slots[k];
        if (!sl.pending.load(std::memory_order_acquire))
            return;
        bool nativeDone = true;
        bool timedOut = false;
        for (UINT i = 0; i < activePasses; ++i)
        {
            const auto done = static_cast<UINT>(InterlockedCompareExchange(
                reinterpret_cast<volatile LONG*>(&At<UINT>(runtime[i], L->jobDone)), 0, 0));
            nativeDone &= sl.jobs[i] != 0 && done >= sl.jobs[i];
#ifdef AMD_RETIRE_DIAGNOSTICS
            sample->done[i] = done;
#endif
            timedOut |= At<UINT>(runtime[i], L->timeoutCount) > observedTimeouts[i];
        }
        auto gpuDone = fence->GetCompletedValue();
        if (gpuDone == UINT64_MAX && !deviceLostReported)
            TraceBoundary("device removed while retiring");
        const auto target = sl.completion.load();
#ifdef AMD_RETIRE_DIAGNOSTICS
        sample->nativeDone = nativeDone; // Exactly the value passed to CanRetire.
        sample->gpuBefore = gpuDone;
        sample->target = target;
#endif
        // Preserve the existing short recording-thread wait, but never block
        // on a list that the game has not submitted yet, or from Status().
        if (waitForGpu && sl.submission.submitted && nativeDone && gpuDone < target)
        {
#ifdef AMD_RETIRE_DIAGNOSTICS
            const auto waitStart = RetirementDiagnostics::Clock();
#endif
            const auto start = GetTickCount64();
            while (gpuDone < target && GetTickCount64() - start < 16)
            {
                Sleep(1);
                gpuDone = fence->GetCompletedValue();
            }
#ifdef AMD_RETIRE_DIAGNOSTICS
            sample->waitMs = diagnostics.Milliseconds(RetirementDiagnostics::Clock() - waitStart);
#endif
        }
#ifdef AMD_RETIRE_DIAGNOSTICS
        sample->gpuAfter = gpuDone;
        sample->retired = sl.submission.CanRetire(nativeDone, gpuDone, target);
#endif
        if (sl.submission.CanRetire(nativeDone, gpuDone, target))
        {
            sl.pending.store(nullptr, std::memory_order_release);
            sl.submission = {};
            lastSubmitted = GetTickCount64();
            if (!failed && activePasses && !timedOut)
            {
                ++completedFrames;
                lastCompleted = lastSubmitted;
                status = "Completed AMD pre-SR passes=" + std::to_string(activePasses) + " at " +
                         std::to_string(width) + "x" + std::to_string(height);
                if (completedFrames <= 3 || completedFrames % 120 == 0)
                    Log(status);
            }
            return;
        }
        if (sl.submission.ReportStall(GetTickCount64()))
        {
            Log("AMD submission stalled >5s; retaining list/resources until completion. submitted=" +
                std::to_string(sl.submission.submitted) + " nativeDone=" + std::to_string(nativeDone) +
                " passes=" + std::to_string(activePasses) + " fence=" + std::to_string(gpuDone) +
                "/" + std::to_string(target));
        }
    }
    void RetireSubmission(bool waitForGpu = false, const char* source = "Unknown"
#ifdef AMD_RETIRE_DIAGNOSTICS
                          , RetirementDiagnostics::Event* recordEvent = nullptr
#endif
                          )
    {
#ifdef AMD_RETIRE_DIAGNOSTICS
        RetirementDiagnostics::Scope timing(diagnostics, directory, L ? L->name : "uninitialized", source, recordEvent);
        auto& sample = timing.event;
        sample.passes = activePasses;
        sample.width = width;
        sample.height = height;
        sample.everyFrame = haveSettings && lastSettings.everyFrame;
        // Sample the first slot with work outstanding. With kSlots == 1 that is
        // the only slot, so the recorded diagnostic matches the original.
        UINT sampled = kSlots;
        for (UINT k = 0; k < kSlots; ++k)
            if (slots[k].pending.load(std::memory_order_acquire))
            {
                sampled = k;
                break;
            }
        if (sampled < kSlots)
        {
            sample.pending = reinterpret_cast<uintptr_t>(slots[sampled].pending.load(std::memory_order_acquire));
            sample.submitted = slots[sampled].submission.submitted;
            sample.recordedAt = slots[sampled].submission.recordedAt;
            sample.submittedAt = slots[sampled].submission.submittedAt;
            sample.jobs = slots[sampled].jobs;
            sample.target = slots[sampled].completion.load();
            for (UINT k = 0; k < kSlots; ++k)
                RetireSlot(k, waitForGpu, source, k == sampled ? &sample : nullptr);
        }
#else
        for (UINT k = 0; k < kSlots; ++k)
            RetireSlot(k, waitForGpu, source);
#endif
    }
    // Execute has already happened. Wait only for HIP job-done, not the D3D12
    // fence: that fence covers FSR and the rest of the batch and was stalling
    // ExecuteCommandLists down to ~30 FPS. A's GPU inline still serializes NR
    // before FSR on the list. Record may still skip if the fence is in flight.
    void WaitAfterSubmitIfEveryFrame(UINT k)
    {
        if (!haveSettings || !lastSettings.everyFrame)
            return;
        // The wait existed for one reason: with a single slot, the next Record
        // would skip unless this frame's job had already retired. An extra slot
        // is exactly what removes that need, so with two slots the render thread
        // must not block here - blocking is the cost this whole change removes.
        // Not while A is rebuilding, though: that is when the wait is load-bearing.
        if (kSlots > 1 && !NativeRebuilding())
        {
            // Throttled trace of the fast path, so a run shows whether it was
            // taken and how far the native counter had progressed.
            if (++skipWaits <= 3 || skipWaits % 300 == 0)
                Log("AMD wait skipped (not rebuilding); count=" + std::to_string(skipWaits) +
                    " nativeDone=" + std::to_string(L && runtime[0] ? At<UINT>(runtime[0], L->jobDone) : 0) +
                    " job=" + std::to_string(slots[k].jobs[0]));
            return;
        }
#ifdef AMD_RETIRE_DIAGNOSTICS
        // Observational only: no wait behaviour is changed here.
        RetirementDiagnostics::Scope timing(diagnostics, directory, L ? L->name : "uninitialized", "EfWaitLoop");
        auto& sample = timing.event;
        sample.everyFrame = true;
        sample.passes = activePasses;
        sample.width = width;
        sample.height = height;
        sample.jobs = slots[k].jobs;
        sample.target = slots[k].completion.load();
        const auto waitEntry = RetirementDiagnostics::Clock();
#endif
        unsigned iterations = 0;
#ifdef AMD_RETIRE_DIAGNOSTICS
        bool nativeAtEntry = true;   // first poll result: did we wait at all?
#endif
        const auto start = GetTickCount64();
        while (GetTickCount64() - start < 80)
        {
            bool nativeDone = true;
            for (UINT i = 0; i < activePasses; ++i)
            {
                if (!runtime[i] || slots[k].jobs[i] == 0)
                {
                    nativeDone = false;
                    break;
                }
                const auto done = static_cast<UINT>(InterlockedCompareExchange(
                    reinterpret_cast<volatile LONG*>(&At<UINT>(runtime[i], L->jobDone)), 0, 0));
                if (done < slots[k].jobs[i])
                {
                    nativeDone = false;
                    break;
                }
            }
#ifdef AMD_RETIRE_DIAGNOSTICS
            if (iterations == 0)
                nativeAtEntry = nativeDone;
#endif
            if (nativeDone)
            {
                RetireSubmission(false, "EveryFrameWait");
#ifdef AMD_RETIRE_DIAGNOSTICS
                sample.outcome = "done";
#endif
                break;
            }
            ++iterations;
            Sleep(1);
        }
#ifdef AMD_RETIRE_DIAGNOSTICS
        if (sample.outcome == std::string_view("poll"))
            sample.outcome = "budget";   // fell out of the 80 ms loop without finishing
        sample.waitIterations = iterations;
        sample.waitedBeforeDone = !nativeAtEntry;
        sample.gpuAfter = fence ? fence->GetCompletedValue() : 0;
        sample.waitMs = diagnostics.Milliseconds(RetirementDiagnostics::Clock() - waitEntry);
#endif
    }
    void InitHip()
    {
        if (hipSet)
            return;
        HMODULE hip = LoadLibraryExW(L"amdhip64_7.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!hip)
            throw std::runtime_error("Cannot load amdhip64_7.dll; Windows error=" + std::to_string(GetLastError()) +
                                     ". Install the compatible AMD HIP 7 runtime; HIP 6 alone is insufficient.");
        wchar_t hipPath[MAX_PATH] {};
        GetModuleFileNameW(hip, hipPath, MAX_PATH);
        Log("HIP runtime: " + std::filesystem::path(hipPath).string());
        auto count = reinterpret_cast<int (*)(int*)>(GetProcAddress(hip, "hipGetDeviceCount"));
        auto props = reinterpret_cast<int (*)(void*, int)>(GetProcAddress(hip, "hipGetDevicePropertiesR0600"));
        hipSet = reinterpret_cast<HipSetFn>(GetProcAddress(hip, "hipSetDevice"));
        if (!count || !props || !hipSet)
            throw std::runtime_error("HIP R0600 API unavailable");
        int n = 0;
        int countResult = count(&n);
        if (countResult != 0 || n == 0)
            throw std::runtime_error("HIP device enumeration failed: code=" + std::to_string(countResult) +
                                     " devices=" + std::to_string(n));
        auto luid = device->GetAdapterLuid();
        for (int i = 0; i < n; ++i)
        {
            // R0600 prefix: name[256], uuid[16], luid[8]. Oversized aligned storage.
            alignas(16) std::array<unsigned char, 8192> p {};
            int propResult = props(p.data(), i);
            Log("HIP candidate " + std::to_string(i) + " code=" + std::to_string(propResult) +
                " name=" + std::string(reinterpret_cast<char*>(p.data())));
            if (propResult == 0 && std::memcmp(p.data() + 272, &luid, 8) == 0)
            {
                hipDevice = i;
                Log("HIP adapter: " + std::string(reinterpret_cast<char*>(p.data())));
                break;
            }
        }
        if (hipDevice < 0 || hipSet(hipDevice) != 0)
            throw std::runtime_error("No HIP adapter matches D3D12 LUID");
    }
    void InitPass(UINT i)
    {
        if (runtime[i])
            return;
        InitHip();
        auto path = directory / (L"dlssnr_amd_pass" + std::to_wstring(i + 1) + L".dll");
        auto identified = IdentifyRuntime(path);
        if (!identified)
            throw std::runtime_error("Private AMD runtime hash mismatch: pass " + std::to_string(i + 1));
        if (L && L != identified)
            throw std::runtime_error("Mixed AMD runtime versions across passes");
        L = identified;
        Log(std::string("AMD runtime ") + L->name);
        auto weights = directory / L"dlssnr_on_amd_weights.bin";
        if (!std::filesystem::exists(weights))
            throw std::runtime_error("dlssnr_on_amd_weights.bin is required");
        HMODULE h =
            LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!h)
            throw std::runtime_error("Private AMD runtime LoadLibrary failed: " + std::to_string(GetLastError()));
        HMODULE pinned {};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(h), &pinned);
        // Retain module even on failure: CRT registered HIP kernels; no unsafe unloading.
        runtime[i] = h;
        // The hash above fixes this private module's import layout. Older games
        // ship a 2013 D3DCompiler that rejects the FP16 typed UAV load shader.
        // Bind only this module's compiler import; leave the game's DLL intact.
        static HMODULE systemCompiler = [] {
            wchar_t systemPath[MAX_PATH] {};
            auto length = GetSystemDirectoryW(systemPath, MAX_PATH);
            if (!length || length >= MAX_PATH) return HMODULE(nullptr);
            auto path = std::filesystem::path(systemPath) / L"d3dcompiler_47.dll";
            return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }();
        auto compile = systemCompiler ? GetProcAddress(systemCompiler, "D3DCompile") : nullptr;
        if (!compile) throw std::runtime_error("System D3DCompile unavailable for AMD neural shaders");
        if (L->d3dCompileIat)
        {
            auto import = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(h) + L->d3dCompileIat);
            DWORD previousProtection = 0;
            if (!VirtualProtect(import, sizeof(void*), PAGE_READWRITE, &previousProtection))
                throw std::runtime_error("Could not bind private AMD shader compiler");
            InterlockedExchangePointer(import, reinterpret_cast<void*>(compile));
            DWORD unused = 0;
            if (!VirtualProtect(import, sizeof(void*), previousProtection, &unused))
                throw std::runtime_error("Could not restore private AMD import protection");
            Log("Private AMD shaders use System32 D3DCompiler; game compiler preserved");
        }
        else
            Log("AMD runtime has no D3DCompile import; using engine default");
        // All passes notify after the bridge's single real submission.
        At<NotifyFn>(h, L->trampoline) = AlreadySubmitted;

        At<ID3D12Device*>(h, L->device) = device.Get();
        device->AddRef();
        At<ID3D12CommandQueue*>(h, L->queue) = queue.Get();
        queue->AddRef();
        At<int>(h, L->hipOrdinal) = hipDevice;
        At<uint8_t>(h, L->configuredInline) = 1;
        At<uint8_t>(h, L->interop) = 1;
        At<uint8_t>(h, L->enabled) = 1;
        At<uint8_t>(h, L->fsrInputs) = 1;
        At<uint8_t>(h, L->depthPresent) = 1;
        At<int>(h, L->tonemap) = -1;
        std::string file = weights.string();
        if (hipSet(hipDevice) != 0 || !reinterpret_cast<InitFn>(reinterpret_cast<uintptr_t>(h) + L->init)(
                                          reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(h) + L->engine), &file))
            throw std::runtime_error("AMD engine initialization failed");
        At<uint8_t>(h, L->initDone) = 1;
        Log("Initialized independent AMD pass " + std::to_string(i + 1));
    }
    void InitShader()
    {
        if (root)
            return;
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 };
        D3D12_ROOT_PARAMETER params[3] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 24 };
        D3D12_DESCRIPTOR_RANGE residualRange { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1, 0, 0 };
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = { 1, &residualRange };
        D3D12_ROOT_SIGNATURE_DESC desc { 3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, error;
        Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
              "Root signature serialize");
        Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
              "Root signature create");
        Check(D3DCompile(CopyShader, sizeof(CopyShader), "AMD active crop", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "Crop shader compile");
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps {};
        ps.pRootSignature = root.Get();
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pipeline)), "Crop pipeline");
        Check(D3DCompile(DepthShader, sizeof(DepthShader), "AMD depth conversion", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error),
              "Depth shader compile");
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&depthPipeline)), "Depth pipeline");
        Check(D3DCompile(MotionShader, sizeof(MotionShader), "AMD motion resample", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error), "Motion shader compile");
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&motionPipeline)), "Motion pipeline");
        Check(D3DCompile(ExposureShader, sizeof(ExposureShader), "AMD exposure conversion", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error), "Exposure shader compile");
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&exposurePipeline)), "Exposure pipeline");
        Check(D3DCompile(ResolveShader, sizeof(ResolveShader), "AMD residual resolve", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error), "Resolve compile");
        ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        Check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&resolvePipeline)), "Resolve pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 14,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "Crop heap");
    }
};
Backend::Backend(ID3D12Device* d, ID3D12CommandQueue* q, const std::filesystem::path& dir) : p(new Impl)
{
    p->device = d;
    p->queue = q;
    p->directory = dir;
    // The tail of this line identifies the build. Four earlier rounds were
    // analysed without it and the logs could not be told apart.
#ifdef AMD_MULTISLOT
    static constexpr const char* kBuildTag = " [s9-refusaldiag slots=2 state-gated wait]";
#else
    static constexpr const char* kBuildTag = " [s9-refusaldiag slots=1]";
#endif
    p->Log("AMD submission revision 20260910-r1: one Execute, post-submit Notify, native+GPU retirement" +
           std::string(kBuildTag));
    try
    {
        Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p->fence)), "Completion fence");
    }
    catch (const std::exception& e)
    {
        p->failed = true;
        p->Log(e.what());
    }
}
ID3D12Resource* Backend::Record(ID3D12GraphicsCommandList* cmd, const Frame& incoming, const Settings& cfg)
{
    std::lock_guard guard(p->lock);
    Frame f=incoming;
#ifdef AMD_RETIRE_DIAGNOSTICS
    p->diagnostics.BeginRecord(p->frames != 0);
    RetirementDiagnostics::Scope timing(p->diagnostics, p->directory, p->L ? p->L->name : "uninitialized", "Record");
    timing.event.outcome = "other_skip";
    p->RetireSubmission(true, "Record", &timing.event);
#else
    p->RetireSubmission(true);
#endif
    if (p->failed || !cmd || !f.colour || !f.motion || !f.depth)
        return nullptr;
    const auto deviceStatus = p->device->GetDeviceRemovedReason();
    if (FAILED(deviceStatus))
    {
        p->failed = true;
        p->Log("AMD stopped: D3D12 device lost, HRESULT=" + std::to_string(static_cast<UINT>(deviceStatus)));
        p->TraceBoundary("Record device removed");
        return nullptr;
    }
    // Pick the slot for this frame. With one slot this is the original
    // behaviour: that slot must have retired or the frame is skipped. With two,
    // the second slot lets the CPU keep recording while the previous job is
    // still retiring, instead of blocking the render thread in Submitted.
    Impl::Slot* sl = nullptr;
    for (size_t k = 0; k < p->slots.size(); ++k)
        if (!p->slots[k].pending.load(std::memory_order_acquire))
        {
            sl = &p->slots[k];
            p->activeSlot = static_cast<UINT>(k);
            break;
        }
    if (!sl)
    {
#ifdef AMD_RETIRE_DIAGNOSTICS
        timing.event.outcome = "pending_skip";
#endif
        // Do not wait here. Execute/Submitted needs this lock to Notify HIP.
        // Every-frame waits after Execute in Submitted instead.
        if (++p->pendingSkips <= 3 || p->pendingSkips % 120 == 0)
            p->Log("AMD skipped: no free neural slot; count=" + std::to_string(p->pendingSkips));
        return nullptr;
    }
    const auto completion = sl->completion.load();
#ifdef AMD_RETIRE_DIAGNOSTICS
    const auto extraGpuBefore = p->fence->GetCompletedValue();
    if (extraGpuBefore < completion)
#else
    if (p->fence->GetCompletedValue() < completion)
#endif
    {
#ifdef AMD_RETIRE_DIAGNOSTICS
        timing.event.extraWaited = true;
        timing.event.extraGpuBefore = extraGpuBefore;
        timing.event.extraTarget = completion;
        const auto waitStart = RetirementDiagnostics::Clock();
#endif
        // Only wait for already submitted GPU work. Never wait here for an
        // unsubmitted list: its submission may depend on the recording thread.
        // A short scheduling delay used to bypass the effect for a whole frame.
        const auto start = GetTickCount64();
        while (p->fence->GetCompletedValue() < completion && GetTickCount64() - start < 16)
            Sleep(1);
#ifdef AMD_RETIRE_DIAGNOSTICS
        timing.event.extraWaitMs = p->diagnostics.Milliseconds(RetirementDiagnostics::Clock() - waitStart);
        const auto extraGpuAfter = p->fence->GetCompletedValue();
        timing.event.extraGpuAfter = extraGpuAfter;
        if (extraGpuAfter < completion)
#else
        if (p->fence->GetCompletedValue() < completion)
#endif
        {
#ifdef AMD_RETIRE_DIAGNOSTICS
            timing.event.outcome = "fence_skip";
#endif
            if (++p->fenceSkips)
                p->Log("AMD skipped: submitted GPU work still in flight after 16 ms; count=" + std::to_string(p->fenceSkips));
            return nullptr;
        }
        if (++p->fenceRecoveries <= 3 || p->fenceRecoveries % 120 == 0)
            p->Log("AMD continuity: prior GPU work retired after short wait; count=" + std::to_string(p->fenceRecoveries));
    }
    bool timedOut = false;
    for (UINT i = 0; i < p->runtime.size(); ++i)
        if (auto h = p->runtime[i])
        {
            UINT count = static_cast<UINT>(
                InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&At<UINT>(h, p->L->timeoutCount)), 0, 0));
            if (count > p->observedTimeouts[i])
            {
                p->timeoutEvents += count - p->observedTimeouts[i];
                timedOut = true;
            }
            // The native count resets when staging is recreated.
            p->observedTimeouts[i] = count;
        }
    if (timedOut)
    {
        p->retryAfter = GetTickCount64() + 1000;
        p->resetAfterTimeout = true;
        p->Log("AMD timeout: native fallback may reuse the previous residual; retry in 1s with fresh history. Events=" +
               std::to_string(p->timeoutEvents));
    }
    if (GetTickCount64() < p->retryAfter)
        return nullptr;
    try
    {
        // Reject transient/dummy guides before any GPU commands or native jobs.
        // A later valid frame must be allowed to recover without restarting.
        const auto cd=f.colour->GetDesc();
        const UINT iw=f.width?f.width:UINT(cd.Width), ih=f.height?f.height:cd.Height;
        for(auto guide : {f.motion,f.depth}) {
            auto gd=guide->GetDesc();
            if(gd.Width<iw || gd.Height<ih || gd.SampleDesc.Count!=1 || gd.DepthOrArraySize!=1 ||
               gd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
                const std::string reason="AMD neural: waiting for valid full-size guides; received "+Layout(guide);
                if(p->status!=reason)p->Log(reason);
                p->resetRequested=true;
                return nullptr;
            }
        }
        auto desc = f.colour->GetDesc();
        UINT w = f.width ? f.width : static_cast<UINT>(desc.Width), h = f.height ? f.height : desc.Height;
        if (p->frames == 0 || p->lastInputWidth != w || p->lastInputHeight != h)
        {
            p->Log("Input active=" + std::to_string(w) + "x" + std::to_string(h) + " colour=" + Layout(f.colour));
            p->Log("Input motion=" + Layout(f.motion) + " depth=" + Layout(f.depth));
        }
        if (!w || !h || w > desc.Width || h > desc.Height || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 ||
            desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
            throw std::runtime_error("Unsupported active colour extent/layout");
        // Display-resolution vectors are resampled, never cropped as if they
        // belonged to the render-resolution pixel grid.
        for (auto guide : { f.motion, f.depth })
        {
            auto gd = guide->GetDesc();
            if (gd.Width < w || gd.Height < h || gd.SampleDesc.Count != 1 || gd.DepthOrArraySize != 1 ||
                gd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                throw std::runtime_error(std::string("Unsupported AMD pre-SR ") +
                                         (guide == f.motion ? "motion: " : "depth: ") + Layout(guide));
        }
        const UINT inputW=w, inputH=h;
        const float scale=std::isfinite(cfg.modelScale)?std::clamp(cfg.modelScale,.25f,1.f):1.f;
        w=(std::min)(inputW,(std::max)(32u,UINT(std::lround(inputW*scale))));
        h=(std::min)(inputH,(std::max)(32u,UINT(std::lround(inputH*scale))));
        const bool scaled=w!=inputW||h!=inputH;
        const UINT mvW=f.motionWidth?f.motionWidth:inputW, mvH=f.motionHeight?f.motionHeight:inputH;
        const auto depthDesc = f.depth->GetDesc();
        // The private AMD runtime already accepts typeless/depth-stencil guides
        // and stages only the colour-sized active region. Preparing another
        // crop/conversion on the game's command list duplicates that work and
        // invalidates some UE 4.26 command lists (Stellar Blade reports
        // E_INVALIDARG from Close). Pass the original guides through instead.
        const bool convertDepth = scaled;
        if (scaled && (depthDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            throw std::runtime_error("NR scale: depth is not shader readable; use 100%");
        if (scaled && DepthReadFormat(depthDesc.Format)==DXGI_FORMAT_UNKNOWN)
            throw std::runtime_error("NR scale: unsupported depth view; use 100%");
        const bool resampleMotion = mvW != w || mvH != h;
        const auto motionDesc = f.motion->GetDesc();
        if (resampleMotion && (f.motionWidth > motionDesc.Width || f.motionHeight > motionDesc.Height))
            throw std::runtime_error("Display motion extent exceeds its allocation");
        if (resampleMotion && motionDesc.Format != DXGI_FORMAT_R16G16_FLOAT &&
            motionDesc.Format != DXGI_FORMAT_R32G32_FLOAT && motionDesc.Format != DXGI_FORMAT_R16G16_SNORM &&
            motionDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && motionDesc.Format != DXGI_FORMAT_R32G32B32A32_FLOAT)
            throw std::runtime_error("Unsupported display motion format: " + Layout(f.motion));
        ID3D12Resource* exposureSource = nullptr;
        if (f.exposure)
        {
            const auto ed = f.exposure->GetDesc();
            if (ed.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && ed.SampleDesc.Count == 1 &&
                ed.DepthOrArraySize == 1 && !(ed.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
                (ed.Format == DXGI_FORMAT_R32_FLOAT || ed.Format == DXGI_FORMAT_R32G32_FLOAT ||
                 ed.Format == DXGI_FORMAT_R32G32B32A32_FLOAT || ed.Format == DXGI_FORMAT_R16_FLOAT ||
                 ed.Format == DXGI_FORMAT_R16G16B16A16_FLOAT))
                exposureSource = f.exposure;
        }
        if (f.motion->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
            throw std::runtime_error("Unsupported depth-stencil motion buffer: " + Layout(f.motion));
        p->activePasses = std::clamp(cfg.passes, 1u, 3u);
        bool passChange = p->lastPasses != p->activePasses;
        p->lastPasses = p->activePasses;
        for (UINT i = 0; i < p->activePasses; ++i)
            p->InitPass(i);
        const AmdLayout* L = p->L;
        if (!L)
            return nullptr;
        p->InitShader();
        bool resize = p->width != w || p->height != h;
        if (resize)
        {
            sl->colour.Reset();
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = w;
            rd.Height = h;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            Check(p->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&sl->colour)),
                  "Active FP16 texture");
            p->width = w;
            p->height = h;
        }
        const bool convertEncoding = cfg.encoding == 2 || cfg.encoding == 3;
        if (convertEncoding) {
            if(!p->decode) p->decode=std::make_unique<ColorEncoding>(p->device.Get());
            if(!p->encode) p->encode=std::make_unique<ColorEncoding>(p->device.Get());
            f.colour=p->decode->Run(cmd,f.colour,f.colourState,inputW,inputH,cfg.encoding,false);
            f.colourState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        auto prepareGuide = [&](ID3D12Resource* source, ComPtr<ID3D12Resource>& crop)
        {
            auto rd = source->GetDesc();
            if (rd.Width == w && rd.Height == h)
                return source;
            if (!crop || crop->GetDesc().Width != w || crop->GetDesc().Height != h ||
                crop->GetDesc().Format != rd.Format)
            {
                crop.Reset();
                rd.Width = w;
                rd.Height = h;
                rd.MipLevels = 1;
                rd.Flags = D3D12_RESOURCE_FLAG_NONE;
                D3D12_HEAP_PROPERTIES hp {};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                Check(p->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                         IID_PPV_ARGS(&crop)),
                      "Guide crop");
            }
            return crop.Get();
        };
        auto motion = f.motion;
        auto depth = f.depth;
        const auto& look = cfg.look;
        const bool applyLook = look.enabled && (look.mix > 0 || look.tone > 0 || look.inspect != 0);
        auto createScratch = [&](ComPtr<ID3D12Resource>& resource, UINT sw, UINT sh, DXGI_FORMAT format)
        {
            if (resource && resource->GetDesc().Width == sw && resource->GetDesc().Height == sh) return;
            resource.Reset();
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = sw; rd.Height = sh; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = format; rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            Check(p->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&resource)), "Guide scratch");
        };
        if (scaled) {
            createScratch(p->scaleBaseline,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT);
            createScratch(p->scaleOutput,inputW,inputH,DXGI_FORMAT_R16G16B16A16_FLOAT);
            createScratch(p->depthCrop,w,h,DXGI_FORMAT_R32_FLOAT);
            depth=p->depthCrop.Get();
        }
        if (resampleMotion)
        {
            createScratch(p->motionCrop, w, h, DXGI_FORMAT_R16G16_FLOAT);
            motion = p->motionCrop.Get();
        }
        if (exposureSource) createScratch(sl->exposureCopy, 1, 1, DXGI_FORMAT_R32_FLOAT);
        if (applyLook)
        {
            createScratch(p->lookColour, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (!p->lookPipeline)
            {
                ComPtr<ID3DBlob> blob, error;
                auto hr = D3DCompile(AmdLookShader, sizeof(AmdLookShader), "AMD integrated appearance", nullptr,
                    nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error);
                if (FAILED(hr) && error) p->Log(static_cast<const char*>(error->GetBufferPointer()));
                Check(hr, "Appearance shader compile");
                D3D12_COMPUTE_PIPELINE_STATE_DESC ps {};
                ps.pRootSignature = p->root.Get();
                ps.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
                Check(p->device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&p->lookPipeline)), "Appearance pipeline");
            }
        }
        const bool guideChange = p->lastInputWidth != inputW || p->lastInputHeight != inputH ||
                                 p->lastMotionWidth != f.motionWidth || p->lastMotionHeight != f.motionHeight ||
                                 p->hadExposure != (exposureSource != nullptr);
        if (resize || guideChange || p->frames == 0)
            p->Log("Guide mapping: motion=" + std::to_string(f.motionWidth) + "x" + std::to_string(f.motionHeight) +
                   " resampled=" + std::to_string(resampleMotion) + " exposure=" +
                   (exposureSource ? Layout(exposureSource) : "auto") +
                   " preExposure=" + std::to_string(f.preExposure) + " tone=" + std::to_string(cfg.tone));
        auto cpu = p->heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = ReadFormat(f.colour->GetDesc().Format);
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        p->device->CreateShaderResourceView(f.colour, &srv, cpu);
        cpu.ptr += p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p->device->CreateUnorderedAccessView(sl->colour.Get(), nullptr, &uav, cpu);
        auto guideDescriptors = [&](UINT slot, ID3D12Resource* source, ID3D12Resource* target, DXGI_FORMAT format)
        {
            auto handle = p->heap->GetCPUDescriptorHandleForHeapStart();
            auto stride = p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            handle.ptr += slot * stride;
            auto guideSrv = srv;
            guideSrv.Format = ReadFormat(source->GetDesc().Format);
            p->device->CreateShaderResourceView(source, &guideSrv, handle);
            handle.ptr += stride;
            auto guideUav = uav; guideUav.Format = format;
            p->device->CreateUnorderedAccessView(target, nullptr, &guideUav, handle);
        };
        if (resampleMotion) guideDescriptors(4, f.motion, motion, DXGI_FORMAT_R16G16_FLOAT);
        if (exposureSource) guideDescriptors(6, exposureSource, sl->exposureCopy.Get(), DXGI_FORMAT_R32_FLOAT);
        if (applyLook) guideDescriptors(8, sl->colour.Get(), p->lookColour.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (convertDepth)
        {
            // Distinct descriptor slots: overwriting the colour descriptors here
            // would change the earlier dispatch when the GPU consumes the list.
            cpu.ptr += p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            srv.Format = DepthReadFormat(depthDesc.Format);
            p->device->CreateShaderResourceView(f.depth, &srv, cpu);
            cpu.ptr += p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            uav.Format = DXGI_FORMAT_R32_FLOAT;
            p->device->CreateUnorderedAccessView(depth, nullptr, &uav, cpu);
        }
        Barrier(cmd, f.colour, f.colourState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, sl->colour.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(p->root.Get());
        cmd->SetPipelineState(p->pipeline.Get());
        auto heap = p->heap.Get();
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        UINT dims[] { w, h, inputW, inputH };
        cmd->SetComputeRoot32BitConstants(1, 4, dims, 0);
        cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Barrier(cmd, sl->colour.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, f.colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.colourState);
        Barrier(cmd, f.motion, f.motionState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, f.depth, f.depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, exposureSource, f.exposureState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto copyGuide = [&](ID3D12Resource* source, ID3D12Resource* dest)
        {
            if (source == dest)
                return;
            Barrier(cmd, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cmd, dest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION from {}, to {};
            from.pResource = source;
            to.pResource = dest;
            D3D12_BOX box { 0, 0, 0, w, h, 1 };
            cmd->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            Barrier(cmd, dest, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        };
        if (resampleMotion)
        {
            Barrier(cmd, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetPipelineState(p->motionPipeline.Get());
            auto table = p->heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += 4 * p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            cmd->SetComputeRootDescriptorTable(0, table);
            UINT motionDims[] { w, h, mvW, mvH };
            cmd->SetComputeRoot32BitConstants(1, 4, motionDims, 0);
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            Barrier(cmd, motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (convertDepth)
        {
            Barrier(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetPipelineState(p->depthPipeline.Get());
            cmd->SetComputeRoot32BitConstants(1,4,dims,0);
            auto table = p->heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += 2 * p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            cmd->SetComputeRootDescriptorTable(0, table);
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            Barrier(cmd, depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        else
            copyGuide(f.depth, depth);
        if (exposureSource)
        {
            auto exposure = sl->exposureCopy.Get();
            Barrier(cmd, exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetPipelineState(p->exposurePipeline.Get());
            auto table = p->heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += 6 * p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            cmd->SetComputeRootDescriptorTable(0, table);
            struct { UINT w, h; float preExposure, exposureScale; } constants {
                1, 1, std::isfinite(f.preExposure) && f.preExposure > 0 ? f.preExposure : 1,
                std::isfinite(f.exposureScale) && f.exposureScale > 0 ? f.exposureScale : 1 };
            cmd->SetComputeRoot32BitConstants(1, 4, &constants, 0);
            cmd->Dispatch(1, 1, 1);
            Barrier(cmd, exposure, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        UINT accepted = 0;
        if (scaled) copyGuide(sl->colour.Get(),p->scaleBaseline.Get());
        const bool settingsChanged = cfg.encoding != p->lastSettings.encoding || cfg.toneChannels != p->lastSettings.toneChannels || cfg.modelScale != p->lastSettings.modelScale || !p->haveSettings || cfg.tone != p->lastSettings.tone ||
                                     cfg.structure != p->lastSettings.structure || cfg.skin != p->lastSettings.skin ||
                                     cfg.everyFrame != p->lastSettings.everyFrame;
        const bool explicitReset = p->resetRequested.exchange(false);
        const bool gap = p->lastSubmitted && GetTickCount64() - p->lastSubmitted > 250;
        if (f.reset || resize || guideChange || passChange || p->resetAfterTimeout || settingsChanged || explicitReset || gap)
        {
            p->Log("AMD history reset: frame=" + std::to_string(p->frames) +
                   " game=" + std::to_string(f.reset) + " resize=" + std::to_string(resize) +
                   " guides=" + std::to_string(guideChange) + " passes=" + std::to_string(passChange) +
                   " timeout=" + std::to_string(p->resetAfterTimeout) + " settings=" + std::to_string(settingsChanged) +
                   " explicit=" + std::to_string(explicitReset) + " gap=" + std::to_string(gap));
        }
        for (UINT i = 0; i < p->activePasses; ++i)
        {
            auto r = p->runtime[i];
            // 0x8d9bd is A 0.2.17 Temporal. Default on (skip-frame path).
            // Every-frame mode matches author 0.3: skip history inputs, do not
            // clear history-valid (0x8d018) each frame.
            At<uint8_t>(r, L->temporal) = cfg.everyFrame ? 0 : 1;
            // Engine +0x120 is the history-valid flag, +0x118 is the current
            // borrowed history view. Clear only at a quiescent frame boundary.
            if (f.reset || resize || guideChange || passChange || p->resetAfterTimeout || settingsChanged || explicitReset || gap)
            {
                At<uint8_t>(r, L->historyValid) = 0;
                At<void*>(r, L->historyView) = nullptr;
            }
            At<UINT>(r, L->depthInverted) = f.depthInverted;
            At<uint8_t>(r, L->explicitDepth) = 1; // explicit depth convention, no heuristic
            At<float>(r, L->tone) = i == 0 ? cfg.tone : 0;
            At<float>(r, L->structure) = cfg.structure;
            At<float>(r, L->skin) = cfg.skin;
            At<UINT>(r, L->toneChannels)=cfg.toneChannels?1u:0u;
            At<UINT>(r, L->charMask) = 1; // Enable native semantic character-mask channel.
            // The old shader ceiling expired at high render resolutions even
            // when inference finished well inside the native host watchdog.
            // Scale the spin allowance with pixels, but retain a hard ceiling
            // in the private shader if notification is lost. This is an
            // iteration allowance, not a portable millisecond conversion.
            At<UINT>(r, L->watchdog) = static_cast<UINT>(std::clamp<UINT64>(
                262144 + (UINT64(w) * h + 1) / 2, 262144, 2097152));
            Packet packet {};
            packet.list = cmd;
            packet.colour = sl->colour.Get();
            packet.colourState = 4;
            packet.motion = motion;
            packet.motionState = 4;
            packet.depth = depth;
            packet.depthState = 4;
            packet.exposure = exposureSource ? sl->exposureCopy.Get() : nullptr;
            packet.exposureState = 4;
            packet.scaleX = f.motionScaleX * (resampleMotion ? float(w) / mvW : 1.0f);
            packet.scaleY = f.motionScaleY * (resampleMotion ? float(h) / mvH : 1.0f);
            // Snapshot the two things that can make Record refuse, so the log
            // can say which one it was. A non-null native pending list means A
            // is still holding an earlier list; the recreate byte means A is
            // rebuilding staging and will not attach anything this call.
            const void* pendingBefore = At<ID3D12CommandList*>(r, L->pendingList);
            const unsigned recreateBefore = L->recreate ? At<volatile uint8_t>(r, L->recreate) : 0;
            reinterpret_cast<RecordFn>(reinterpret_cast<uintptr_t>(r) + L->record)(&packet);
            sl->jobs[i] = At<UINT>(r, L->jobId);
            // Staging recreation resets the native job counter. After a resize,
            // job 1 can follow job 1, so counter equality does not mean rejection.
            // The native pending-list pointer is the actual submission contract.
            const bool recorded = At<ID3D12CommandList*>(r, L->pendingList) == cmd;
            if (recorded)
            {
                // Runtime 0.2.17 owns the abort word in its HIP flags buffer.
                // Native staging rebuilds join workers and clear that buffer.
                ++accepted;
            }
            if (At<uint8_t>(r, L->nativeFailure))
            {
                p->failed = true;
                p->Log("AMD pass native failure: " + std::to_string(i + 1) + " job=" + std::to_string(sl->jobs[i]));
                break;
            }
            if (!recorded)
            {
                // A refused the list. Two very different reasons, and the log
                // has to say which: an earlier list still in nativePending
                // means A is simply busy, while recreate == 1 means A is
                // rebuilding staging. Everything downstream depends on which.
                p->Log("AMD Record refused: job=" + std::to_string(sl->jobs[i]) +
                       " nativePending_before=" + std::to_string(reinterpret_cast<uintptr_t>(pendingBefore)) +
                       " nativePending_after=" + std::to_string(reinterpret_cast<uintptr_t>(At<ID3D12CommandList*>(r, L->pendingList))) +
                       " recreate_before=" + std::to_string(recreateBefore) +
                       " recreate_after=" + std::to_string(L->recreate ? At<volatile uint8_t>(r, L->recreate) : 0) +
                       " cmd=" + std::to_string(reinterpret_cast<uintptr_t>(cmd)) +
                       " slot=" + std::to_string(static_cast<UINT>(sl - &p->slots[0])) +
                       " busy=" + std::to_string(p->AnySlotBusy() ? 1 : 0));
                break;
            }
        }
        Barrier(cmd, f.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.motionState);
        Barrier(cmd, f.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.depthState);
        Barrier(cmd, exposureSource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.exposureState);
        p->activePasses = accepted;
#ifdef AMD_RETIRE_DIAGNOSTICS
        timing.event.accepted = accepted;
#endif
        if (accepted)
        {
            ++p->frames;
        }
        // Even accepted == 0 has B's copy/conversion/barrier commands recorded.
        // Track that list until its D3D fence completes before reusing resources.
        sl->submission.Record(GetTickCount64());
        sl->pending.store(cmd, std::memory_order_release);
        if (p->failed)
            return nullptr;
        if (applyLook)
        {
            auto bounded = [](float v, float lo, float hi, float fallback) {
                return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
            };
            struct Constants
            {
                UINT w, h, appearance, inspect;
                float mix, material, shape, lighting, skin, softness, specular, rollOff;
                float colour, shadow, halo, flat, tone, exposureEV, contrast, saturation;
                float compression, preExposure;
                UINT detectSkin, reserved;
            } c {
                w, h, (std::min)(look.appearance, 3u), (std::min)(look.inspect, 3u),
                bounded(look.mix,0,1,1), bounded(look.materialDetail,0,2,1.15f),
                bounded(look.shapeDefinition,0,2,1.2f), bounded(look.localLighting,0,2,1.15f),
                bounded(look.skinDetail,0,2,1.1f), bounded(look.skinSoftness,0,1,.486f),
                bounded(look.specularControl,0,1,.58f), bounded(look.highlightRollOff,0,1,.9f),
                bounded(look.colourSeparation,0,1,0), bounded(look.shadowDepth,0,1,.2f),
                bounded(look.antiHalo,0,1,.901f), bounded(look.flatAreaProtection,0,1,0),
                bounded(look.tone,0,1,0), bounded(look.exposureEV,-3,3,1),
                bounded(look.contrast,.5f,1.5f,1), bounded(look.saturation,0,2,1),
                bounded(look.highlightCompression,0,1,0),
                std::isfinite(f.preExposure) && f.preExposure > 0 ? f.preExposure : 1, look.detectSkin, 0
            };
            static_assert(sizeof(Constants) == 24 * sizeof(UINT));
            Barrier(cmd, p->lookColour.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetComputeRootSignature(p->root.Get());
            cmd->SetPipelineState(p->lookPipeline.Get());
            cmd->SetDescriptorHeaps(1, &heap);
            auto table = p->heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += 8 * p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            cmd->SetComputeRootDescriptorTable(0, table);
            cmd->SetComputeRoot32BitConstants(1, 24, &c, 0);
            cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            Barrier(cmd, p->lookColour.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        auto finalColour = applyLook ? p->lookColour.Get() : sl->colour.Get();
        if (cfg.rtgi.enabled && !p->rtgiFailed)
        {
            try
            {
                if (!p->rtgi) p->rtgi = std::make_unique<RtgiNative>(p->device.Get(), p->directory / L"experimental_lighting");
                Frame rtgiFrame = f;
                rtgiFrame.colour = finalColour;
                rtgiFrame.width=w;rtgiFrame.height=h;
                rtgiFrame.depth=depth;
                rtgiFrame.depthState=scaled?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:f.depthState;
                rtgiFrame.colourState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                rtgiFrame.motion = motion;
                rtgiFrame.motionState = motion == f.motion ? f.motionState : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                rtgiFrame.motionScaleX *= resampleMotion ? float(w) / mvW : 1.0f;
                rtgiFrame.motionScaleY *= resampleMotion ? float(h) / mvH : 1.0f;
                rtgiFrame.reset |= resize || guideChange || passChange || p->resetAfterTimeout || explicitReset || gap;
                finalColour = p->rtgi->Record(cmd, rtgiFrame, cfg.rtgi);
                p->rtgiStatus = "Experimental effect active";
            }
            catch (const std::exception& e)
            {
                // Retain resources referenced by any already recorded commands.
                // A failed optional effect must not disable the neural backend.
                p->rtgiFailed = true;
                p->rtgiStatus = e.what();
                p->Log(p->rtgiStatus);
            }
        }
        else if (!cfg.rtgi.enabled)
        {
            if (p->rtgi) p->rtgi->ResetHistory();
            p->rtgiStatus.clear();
        }
        if (scaled) {
            guideDescriptors(10,f.colour,p->scaleOutput.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
            auto handle=p->heap->GetCPUDescriptorHandleForHeapStart();
            auto stride=p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            handle.ptr+=12*stride;
            auto v=srv;v.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            p->device->CreateShaderResourceView(p->scaleBaseline.Get(),&v,handle);
            handle.ptr+=stride;p->device->CreateShaderResourceView(finalColour,&v,handle);
            Barrier(cmd,f.colour,f.colourState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd,p->scaleOutput.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetComputeRootSignature(p->root.Get());cmd->SetDescriptorHeaps(1,&heap);
            cmd->SetPipelineState(p->resolvePipeline.Get());
            auto table=heap->GetGPUDescriptorHandleForHeapStart();table.ptr+=10*stride;cmd->SetComputeRootDescriptorTable(0,table);
            table.ptr+=2*stride;cmd->SetComputeRootDescriptorTable(2,table);
            UINT rc[]{inputW,inputH,w,h};cmd->SetComputeRoot32BitConstants(1,4,rc,0);
            cmd->Dispatch((inputW+7)/8,(inputH+7)/8,1);
            Barrier(cmd,p->scaleOutput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd,f.colour,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,f.colourState);
            finalColour=p->scaleOutput.Get();
        }
        p->resetAfterTimeout = false;
        p->lastSettings = cfg;
        p->haveSettings = true;
        p->lastInputWidth=inputW;p->lastInputHeight=inputH;
        p->lastMotionWidth = f.motionWidth;
        p->lastMotionHeight = f.motionHeight;
        p->hadExposure = exposureSource != nullptr;
        if (p->frames <= 3 || resize)
            p->Log("Recorded pre-SR " + std::to_string(w) + "x" + std::to_string(h) +
                   " passes=" + std::to_string(p->activePasses));
        if(convertEncoding) finalColour=p->encode->Run(cmd,finalColour,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,inputW,inputH,cfg.encoding,true);
#ifdef AMD_RETIRE_DIAGNOSTICS
        timing.event.outcome = "recorded";
#endif
        return finalColour;
    }
    catch (const std::exception& e)
    {
        p->failed = true;
        p->Log(e.what());
        return nullptr;
    }
}
int Backend::PendingListIndex(UINT count, ID3D12CommandList* const* lists) const
{
    if (!lists) return -1;
    for (size_t s = 0; s < p->slots.size(); ++s)
    {
        auto pending = p->slots[s].pending.load(std::memory_order_acquire);
        if (!pending) continue;
        for (UINT i = 0; i < count; ++i)
            if (lists[i] == pending) return static_cast<int>(i);
    }
    return -1;
}
void Backend::TraceBoundary(const std::string& reason)
{
    std::lock_guard guard(p->lock);
    p->TraceBoundary(reason);
}
void Backend::Submitting(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists)
{
    if (!queue)
        return;
    // Which slot is this batch submitting? Usually the one Record just filled.
    UINT slot = static_cast<UINT>(p->slots.size());
    ID3D12CommandList* pending = nullptr;
    for (size_t k = 0; k < p->slots.size() && !pending; ++k)
    {
        auto candidate = p->slots[k].pending.load(std::memory_order_acquire);
        if (!candidate)
            continue;
        bool found = false;
        for (UINT i = 0; i < n; ++i)
            found |= lists[i] == candidate;
        if (found)
        {
            slot = static_cast<UINT>(k);
            pending = candidate;
        }
    }
    if (!pending)
        return;
    std::lock_guard guard(p->lock);
    const AmdLayout* L = p->L;
    if (!L)
        return;
    auto& sl = p->slots[slot];
    if (sl.pending.load() != pending || sl.submission.submitted)
        return;
    if (p->frames <= 3)
        p->Log("Neural submission: lists=" + std::to_string(n) + " queueType=" +
               std::to_string(static_cast<UINT>(queue->GetDesc().Type)));
    // Match the recorded list, not the swapchain's presentation queue. FG can
    // replace the latter, and the renderer may also migrate between queues.
    // A slot is only reused once its own completion fence has retired, so the
    // runtime resources it borrows are free by then.
    if (queue != p->queue.Get())
    {
        for (auto h : p->runtime)
            if (h)
            {
                auto old = At<ID3D12CommandQueue*>(h, L->queue);
                queue->AddRef();
                At<ID3D12CommandQueue*>(h, L->queue) = queue;
                if (old)
                    old->Release();
            }
        p->queue = queue;
        p->Log("Render submission queue changed; AMD pre-SR remains enabled");
    }
    // Bind the real queue here, but only wake HIP after ExecuteCommandLists.
    // A capture-wait kernel launched before D3D12 submission can occupy the GPU
    // while the capture it depends on is still queued on the CPU.

}
void Backend::Submitted(ID3D12CommandQueue* queue, UINT n, ID3D12CommandList* const* lists)
{
    // Fallback for callers using the original post-submit API.
    Submitting(queue, n, lists);
    if (!queue)
        return;
    UINT slot = static_cast<UINT>(p->slots.size());
    ID3D12CommandList* pending = nullptr;
    for (size_t k = 0; k < p->slots.size() && !pending; ++k)
    {
        auto candidate = p->slots[k].pending.load(std::memory_order_acquire);
        if (!candidate)
            continue;
        bool found = false;
        for (UINT i = 0; i < n; ++i)
            found |= lists[i] == candidate;
        if (found)
        {
            slot = static_cast<UINT>(k);
            pending = candidate;
        }
    }
    if (!pending)
        return;
    std::lock_guard guard(p->lock);
    const AmdLayout* L = p->L;
    if (!L)
        return;
    auto& sl = p->slots[slot];
    if (sl.pending.load() != pending)
        return;
    if (sl.submission.submitted) return;
    if (p->activePasses == 1)
    {
        auto h = p->runtime[0];
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(h) + L->notify)(queue, n, lists);
        auto value = ++p->serial;
        if (FAILED(queue->Signal(p->fence.Get(), value)))
        {
            p->failed = true;
            p->Log("D3D12 completion Signal failed; resources retained");
            return;
        }
        sl.completion.store(value);
        sl.submission.Submit(GetTickCount64());
        p->WaitAfterSubmitIfEveryFrame(slot);
        return;
    }
    for (UINT i = 0; i < p->activePasses; ++i)
    {
        auto h = p->runtime[i];
        reinterpret_cast<NotifyFn>(reinterpret_cast<uintptr_t>(h) + L->notify)(queue, n, lists);
        // All runtimes use HIP stream 0. Publish the next pass only once the previous
        // worker finished; otherwise its capture-wait kernel could block the first pass.
        auto start = GetTickCount64();
        while (static_cast<UINT>(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&At<UINT>(h, L->jobDone)), 0,
                                                            0)) < sl.jobs[i])
        {
            if (GetTickCount64() - start > 5000)
            {
                p->failed = true;
                p->Log("HIP completion timeout pass " + std::to_string(i + 1));
                break;
            }
            Sleep(1);
        }
    }
    UINT64 value = ++p->serial;
    if (FAILED(queue->Signal(p->fence.Get(), value)))
    {
        p->failed = true;
        p->Log("D3D12 completion Signal failed");
        return;
    }
    sl.completion.store(value);
    sl.submission.Submit(GetTickCount64());
    p->WaitAfterSubmitIfEveryFrame(slot);
    p->RetireSubmission(false, "Submitted");
}
std::string Backend::Status() const
{
    std::lock_guard guard(p->lock);
    const AmdLayout* L = p->L;
    p->RetireSubmission(false, "Status");
    auto reportedTimeouts = p->timeoutEvents;
    for (UINT i = 0; L && i < p->runtime.size(); ++i)
        if (p->runtime[i])
        {
            auto count = At<UINT>(p->runtime[i], L->timeoutCount);
            if (count > p->observedTimeouts[i])
                reportedTimeouts += count - p->observedTimeouts[i];
        }
    if (!p->failed && p->lastSubmitted)
        return p->status + (p->rtgiStatus.empty() ? "" : " | " + p->rtgiStatus) + " | completed frames=" + std::to_string(p->completedFrames) +
               (p->lastCompleted ? " last completion " + std::to_string((GetTickCount64() - p->lastCompleted) / 1000) + "s ago" : " no successful completion") +
               " | timeout events=" + std::to_string(reportedTimeouts) +
               " | skipped pending/GPU=" + std::to_string(p->pendingSkips) + "/" + std::to_string(p->fenceSkips);
    return p->status;
}
UINT64 Backend::RecordedFrames() const { return p->frames; }
void Backend::InvalidateHistory() { p->resetRequested.store(true); }
bool Backend::Ready()
{
    std::lock_guard guard(p->lock);
    if (!p->fence) return false;
    p->RetireSubmission(false, "Ready");
    const auto completed = p->fence->GetCompletedValue();
    return !p->failed && !p->AnySlotBusy() && completed != UINT64_MAX && completed >= p->LatestCompletion();
}
bool Backend::Shutdown()
{
    std::lock_guard guard(p->lock);
    const AmdLayout* L = p->L;
    if (!p->fence) return false;
    p->RetireSubmission(false, "Shutdown");
#ifdef AMD_RETIRE_DIAGNOSTICS
    p->diagnostics.Flush(p->directory, L ? L->name : "uninitialized", "shutdown");
#endif
    const auto completed = p->fence->GetCompletedValue();
    if (p->AnySlotBusy() || completed == UINT64_MAX || completed < p->LatestCompletion())
        return false;
    for (auto h : p->runtime)
        if (h && L)
        {
            if (p->hipSet)
                p->hipSet(p->hipDevice);
            reinterpret_cast<void (*)()>(reinterpret_cast<uintptr_t>(h) + L->shutdown)();
        }
    p->failed = true;
    p->Log("Workers stopped outside loader lock");
    return true;
}
} // namespace AmdPreSr
