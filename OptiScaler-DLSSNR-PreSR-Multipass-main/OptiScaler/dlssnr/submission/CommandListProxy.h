#pragma once
#include "LogicalList.h"
#include "ContinuationState.h"
#include "ResourceStateBook.h"
#include "QueryStateBook.h"
#include "SubmissionPolicy.h"
#include "EnhancedBarrierBook.h"
#include <cstring>
#include <algorithm>
#include <atomic>
#include <memory>
#include <wrl/client.h>
// Enhanced-barrier policy (host sets from LmxxfAllowEnhancedBarriers; no Config.h include here).
// Explicit opt-in: interop only normalizes observed, uniform enhanced input layouts.

#ifndef LOG_WARN
#define LOG_WARN(...) ((void)0)
#endif
#ifndef LOG_DEBUG
#define LOG_DEBUG(...) ((void)0)
#endif

// COM proxy for ID3D12GraphicsCommandList1..10 (inherits List10).
// QI accepts List1..List10 + base. Newer methods QI the live producer; if unsupported, fail-closed (no-op / E_UNEXPECTED).
// Create/Execute wrap helpers live in SubmissionHooks.h (armed only by harness / future P3).
namespace DlssNr::Submission
{
MIDL_INTERFACE("b3c0e9a1-4d2f-4c77-9a18-6f2d8e1b4c01")
ILogicalCommandList : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SplitSegments(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteOn(ID3D12CommandQueue *queue) = 0;
    // HIP / NR slot between producer and continuation Executes. Pass nullptr for no-op.
    virtual HRESULT STDMETHODCALLTYPE ExecuteOnWithBetween(ID3D12CommandQueue *queue, BetweenCallback between,
                                                           void *betweenCtx) = 0;
    virtual bool STDMETHODCALLTYPE IsSplitIneligible(void) = 0;
    // Harness: viewport count captured for continuation seed (0 if never set).
    virtual UINT STDMETHODCALLTYPE CapturedViewportCount(void) = 0;
    // Harness: plan D continuation IA capture.
    virtual BOOL STDMETHODCALLTYPE CapturedIbBound(void) = 0;
    virtual UINT STDMETHODCALLTYPE CapturedVbSlotCount(void) = 0;
    virtual UINT STDMETHODCALLTYPE CapturedGfxRootCount(void) = 0;
    virtual BOOL STDMETHODCALLTYPE CapturedSamplePositions(void) = 0;
    // Borrowed pointer, only for unsplit lists: preserve the caller's Execute batch.
    virtual ID3D12CommandList *STDMETHODCALLTYPE UnsplitNativeList(void) = 0;
    virtual const char *STDMETHODCALLTYPE SplitRejectionReason(void) = 0;
    virtual ID3D12GraphicsCommandList *STDMETHODCALLTYPE RecordingNativeList(void) = 0;
    virtual RecordingIdentity STDMETHODCALLTYPE Identity(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE ObserveRecording(std::shared_ptr<RecordingObserver> observer) = 0;
    virtual void STDMETHODCALLTYPE TraceOpenQueries() = 0;
    virtual HRESULT STDMETHODCALLTYPE PrepareUnsplitBatch(ID3D12CommandQueue* queue) = 0;
    virtual void STDMETHODCALLTYPE CompleteUnsplitBatch(ID3D12CommandQueue* queue, HRESULT submissionStatus) = 0;
};

MIDL_INTERFACE("8e54c39a-4a7e-4c2e-9c8f-63fa758a610e")
IRecordingResources : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE ObserveResources(std::shared_ptr<RecordingObserver> observer) = 0;
    virtual bool STDMETHODCALLTYPE InContinuation() = 0;
    virtual bool STDMETHODCALLTYPE CanAppendCompute() = 0;
    // Which of CanAppendCompute's conditions failed; only meaningful after it returned false.
    virtual const char* STDMETHODCALLTYPE AppendComputeRejection() = 0;
    virtual bool STDMETHODCALLTYPE CanRecordInline() = 0;
    virtual const char* STDMETHODCALLTYPE InlineRejectionReason() = 0;
    virtual HRESULT STDMETHODCALLTYPE BeginInlineState(UINT count, ID3D12Resource** resources,
                                                        D3D12_RESOURCE_STATES* states) = 0;
    virtual void STDMETHODCALLTYPE EndInlineState() = 0;
    // A lane that records private bindings of its own - the mochizuki exposure meter's compute pass - brackets that
    // call with these. Tracked setters record into a scratch snapshot instead, and the end puts the game's bindings
    // back on the native list, so the snapshot the continuation is seeded from is never the lane's. Unlike
    // BeginInlineState this never rejects, scans sources or moves layouts: it only redirects bookkeeping.
    virtual void STDMETHODCALLTYPE BeginPrivateBindings() = 0;
    virtual void STDMETHODCALLTYPE EndPrivateBindings() = 0;
    virtual HRESULT STDMETHODCALLTYPE BeginSplitState(UINT count, ID3D12Resource** resources,
                                                       D3D12_RESOURCE_STATES* states) = 0;
    virtual void STDMETHODCALLTYPE TraceNrInputState(const char* backend, uint64_t frame, const char* role,
                                                     ID3D12Resource* resource, D3D12_RESOURCE_STATES expected) = 0;
    // Restore caller heaps only, then synchronize tracking without replaying native compute bindings.
    virtual void STDMETHODCALLTYPE InvalidateDirectComputeBindings() = 0;
    virtual HRESULT STDMETHODCALLTYPE BeginDirectState(UINT count, ID3D12Resource** resources,
                                                       D3D12_RESOURCE_STATES* states) = 0;
    virtual void STDMETHODCALLTYPE EndDirectState() = 0;
};
// D3D12Core's private presentation face, not a GraphicsCommandList version or XeLL IID.
// Microsoft D3D12Core.pdb F597AC0490C849B7992AAAA0485E83EB1: GUID RVA 0x1CAA20;
// CGraphicsCommandList COM-map RVA 0x1C93F0 returns +0x300, matching the
// ID3D12CommandListPresent_RS5 RTTI base and its IUnknown/Blt/Present vtable.
inline constexpr GUID IID_CommandListPresentRS5 =
    {0xB0B9E70E, 0x59CE, 0x4415, {0x81, 0x16, 0xC8, 0xC8, 0x26, 0xB1, 0xE5, 0x99}};
// Only opaque pointers cross this ABI; the runtime owns these private structures.
struct SUBRESOURCE_BLT_MAP;
struct D3D12_PRESENTSURFACE;
struct D3D12_PRESENT;
MIDL_INTERFACE("b0b9e70e-59ce-4415-8116-c8c826b1e599")
ID3D12CommandListPresent_RS5 : public IUnknown
{
    virtual void STDMETHODCALLTYPE Blt(ID3D12Resource* destination, const RECT* destinationRect,
                                      const RECT* destinationSubRect, UINT destinationSubresource,
                                      ID3D12Resource* source, const RECT* sourceRect,
                                      const SUBRESOURCE_BLT_MAP* subresources, UINT subresourceCount,
                                      UINT flags) = 0;
    virtual void STDMETHODCALLTYPE Present(ID3D12CommandQueue* queue, const D3D12_PRESENTSURFACE* surfaces,
                                          UINT surfaceCount, ID3D12Resource* resource, const RECT* rect,
                                          UINT flags, UINT interval, UINT presentCount, INT* result,
                                          D3D12_PRESENT* present) = 0;
};


class CommandListProxy final : public ID3D12GraphicsCommandList10, public ILogicalCommandList, public IRecordingResources,
                               public ID3D12CommandListPresent_RS5
{
    std::atomic<ULONG> refs { 1 };
    inline static std::atomic<uint64_t> nextIdentity { 0 };
    const uint64_t stableIdentity = ++nextIdentity;
    uint64_t executionSerial = 0;
    std::shared_ptr<RecordingObservers> recordingObserver;
    std::shared_ptr<RecordingObservers> batchObserver;
    RecordingExecution batchExecution {};
    LogicalList logical;
    ContinuationState contState;
    ResourceStateBook resBook;
    QueryStateBook queryBook;
    bool splitIneligible = false;
    bool renderPassActive = false, enhancedBarriersSeen = false;
    bool presentCommandsUsed = false;
    std::atomic<bool> rawInterfaceEscaped { false }; // Lifetime-wide; an alias can outlive Reset.
    const char *splitIneligibleReason = nullptr;
    bool inlineUnsafe = false, inlineStateActive = false;
    bool directStateActive = false, predicationActive = false;
    // A lane's own recording may bind private state (see BeginPrivateBindings). While this is set the tracked setters
    // write into inlineState, so the game's snapshot stays what the continuation is seeded from, while the native list
    // still receives the private bindings the dispatch needs.
    bool privateBindings = false;
    const char* inlineUnsafeReason = nullptr;
    const char* inlineRejection = "eligible";
    ContinuationState inlineState;
    ContinuationState& TrackedState() { return (inlineStateActive || privateBindings) ? inlineState : contState; }
    EnhancedBarrierBook enhancedBook;
    std::vector<std::pair<ID3D12Resource*, EnhancedBarrierBook::State>> inlineLayouts;

    void TransitionEnhanced(ID3D12GraphicsCommandList7* list, ID3D12Resource* resource,
                            EnhancedBarrierBook::State state, bool toCommon)
    {
        D3D12_BARRIER_GROUP group {};
        group.NumBarriers = 1;
        D3D12_TEXTURE_BARRIER texture {};
        D3D12_BUFFER_BARRIER buffer {};
        if (state.texture) {
            texture.SyncBefore = texture.SyncAfter = D3D12_BARRIER_SYNC_ALL;
            texture.AccessBefore = toCommon ? state.access : D3D12_BARRIER_ACCESS_COMMON;
            texture.AccessAfter = toCommon ? D3D12_BARRIER_ACCESS_COMMON : state.access;
            texture.LayoutBefore = toCommon ? state.layout : D3D12_BARRIER_LAYOUT_COMMON;
            texture.LayoutAfter = toCommon ? D3D12_BARRIER_LAYOUT_COMMON : state.layout;
            texture.pResource = resource;
            texture.Subresources.IndexOrFirstMipLevel = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            group.Type = D3D12_BARRIER_TYPE_TEXTURE; group.pTextureBarriers = &texture;
        } else {
            buffer.SyncBefore = buffer.SyncAfter = D3D12_BARRIER_SYNC_ALL;
            buffer.AccessBefore = toCommon ? state.access : D3D12_BARRIER_ACCESS_COMMON;
            buffer.AccessAfter = toCommon ? D3D12_BARRIER_ACCESS_COMMON : state.access;
            buffer.pResource = resource; buffer.Size = UINT64_MAX;
            group.Type = D3D12_BARRIER_TYPE_BUFFER; group.pBufferBarriers = &buffer;
        }
        list->Barrier(1, &group);
    }

    void MarkSplitIneligible(const char *reason)
    {
        splitIneligible = true;
        if (reason && std::strcmp(reason, "enhanced_barrier") != 0) {
            inlineUnsafe = true; if (!inlineUnsafeReason) inlineUnsafeReason = reason;
        }
        if (!splitIneligibleReason)
        {
            splitIneligibleReason = reason;
            static std::atomic<uint32_t> s_warnCount{0};
            const uint32_t c = s_warnCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (c <= 10 ||
                (c <= 100 && (c % 20 == 0)) ||
                (c <= 1000 && (c % 100 == 0)) ||
                (c % 1000 == 0))
            {
                LOG_WARN("CommandListProxy {:p} MarkSplitIneligible: {} (event #{})",
                         (void*)this, reason ? reason : "unknown", c);
            }
        }
    }


    ID3D12GraphicsCommandList *Cur() const { return logical.Current(); }

    template <typename TIface>
    TIface *CurAs() const
    {
        ID3D12GraphicsCommandList *c = Cur();
        if (!c)
            return nullptr;
        TIface *p = nullptr;
        if (FAILED(c->QueryInterface(__uuidof(TIface), reinterpret_cast<void **>(&p))))
            return nullptr;
        return p; // caller must Release
    }

  public:
    RecordingIdentity STDMETHODCALLTYPE Identity() override { return {stableIdentity, logical.Generation()}; }
    HRESULT STDMETHODCALLTYPE ObserveRecording(std::shared_ptr<RecordingObserver> observer) override
    {
        std::lock_guard lifetime(RecordingMutex());
        if (!observer || (recordingObserver && recordingObserver->job)) return E_UNEXPECTED;
        try {
            if (!recordingObserver) recordingObserver = std::make_shared<RecordingObservers>();
            recordingObserver->job = std::move(observer);
            return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }
    bool STDMETHODCALLTYPE CanAppendCompute() override
    {
        return !rawInterfaceEscaped && !presentCommandsUsed && !renderPassActive && !enhancedBarriersSeen;
    }
    const char* STDMETHODCALLTYPE AppendComputeRejection() override
    {
        if (rawInterfaceEscaped) return "raw interface escaped";
        if (presentCommandsUsed) return "present commands used";
        if (renderPassActive) return "render pass active";
        if (enhancedBarriersSeen) return "enhanced barriers seen";
        return "eligible";
    }
    bool STDMETHODCALLTYPE CanRecordInline() override
    {
        if (rawInterfaceEscaped) inlineRejection = "native_interface_escaped";
        else if (renderPassActive) inlineRejection = "render_pass";
        else if (inlineUnsafe) inlineRejection = inlineUnsafeReason ? inlineUnsafeReason : "unsupported_state";
        else if (logical.WasSplit()) inlineRejection = "already_split";
        else if (!resBook.CanSplit(nullptr)) inlineRejection = "open_split_barrier";
        else if (enhancedBarriersSeen && !AllowEnhancedBarriers()) inlineRejection = "enhanced_barriers_not_allowed";
        else { inlineRejection = "eligible"; return true; }
        return false;
    }
    const char* STDMETHODCALLTYPE InlineRejectionReason() override { return inlineRejection; }
    void STDMETHODCALLTYPE TraceNrInputState(const char* backend, uint64_t frame, const char* role,
                                             ID3D12Resource* resource, D3D12_RESOURCE_STATES expected) override
    {
        if (!resource) return;
        const auto id = Identity();
        D3D12_RESOURCE_STATES legacy {};
        const bool hasLegacy = resBook.TryGet(resource, &legacy);
        const auto* observed = enhancedBook.Find(resource);
        const EnhancedBarrierBook::State state = observed ? *observed : EnhancedBarrierBook::State {};
        (void)backend; (void)frame; (void)role; (void)expected; (void)hasLegacy; // No-logger harness builds.
        LOG_WARN("{} NR input: frame={} proxy={:p} native={:p} listId={} generation={} role={} resource={:p} inputRange=all expectedLegacy={:X} legacyObserved={} lastLegacy={:X} enhancedObserved={} known={} invalidated={} legacyContract={} lastLayout={} lastAccess={:X} observedRange={}/{} arrays={}/{} planes={}/{} bufferOffset={} bufferSize={}",
                 backend, frame, static_cast<void*>(this), static_cast<void*>(Cur()), id.list, id.generation,
                 role, static_cast<void*>(resource), unsigned(expected), hasLegacy, unsigned(legacy),
                 observed != nullptr, state.known, state.invalidated, state.legacy, int(state.layout), unsigned(state.access),
                 state.range.IndexOrFirstMipLevel, state.range.NumMipLevels, state.range.FirstArraySlice,
                 state.range.NumArraySlices, state.range.FirstPlane, state.range.NumPlanes, state.offset, state.size);
    }
    HRESULT STDMETHODCALLTYPE BeginInlineState(UINT count, ID3D12Resource** resources,
                                               D3D12_RESOURCE_STATES* states) override
    {
        if (!CanRecordInline()) return E_UNEXPECTED;
        return BeginNrState(count, resources, states, false);
    }
    HRESULT STDMETHODCALLTYPE BeginSplitState(UINT count, ID3D12Resource** resources,
                                              D3D12_RESOURCE_STATES* states) override
    {
        if (IsSplitIneligible()) { inlineRejection = SplitRejectionReason(); return E_UNEXPECTED; }
        if (enhancedBarriersSeen && !AllowEnhancedBarriers()) {
            inlineRejection = "enhanced_barriers_not_allowed"; return E_UNEXPECTED;
        }
        return BeginNrState(count, resources, states, true);
    }
    HRESULT STDMETHODCALLTYPE BeginDirectState(UINT count, ID3D12Resource** resources,
                                               D3D12_RESOURCE_STATES* states) override
    {
        // Open queries are legal in an unsplit recording. Active render passes and
        // predication are not: raw Vulkan ignores the caller's conditional execution.
        if (renderPassActive) inlineRejection = "render_pass";
        else if (predicationActive) inlineRejection = "predication";
        else if (presentCommandsUsed) inlineRejection = "private_present_commands";
        else if (!resBook.CanSplit(nullptr)) inlineRejection = "open_split_barrier";
        else if (privateBindings || inlineStateActive || directStateActive) inlineRejection = "nr_state_already_active";
        else {
            const auto hr = BeginNrState(count, resources, states, false);
            if (FAILED(hr)) return hr;
            // Prefer an observed whole-resource legacy state; unobserved inputs
            // retain the incoming Frame contract, including prior-list transitions.
            for (UINT i = 0; i < count; ++i) {
                if (!resources[i]) continue;
                const auto* observed = enhancedBook.Find(resources[i]);
                if (!observed || observed->legacy) resBook.TryGet(resources[i], &states[i]);
            }
            directStateActive = true;
            return S_OK;
        }
        return E_UNEXPECTED;
    }
    void STDMETHODCALLTYPE EndDirectState() override
    {
        if (!directStateActive) return;
        auto* list7 = inlineLayouts.empty() ? nullptr : CurAs<ID3D12GraphicsCommandList7>();
        for (const auto& [resource, state] : inlineLayouts) {
            // Runtime restores the COMMON layout provided by BeginNrState.
            TransitionEnhanced(list7, resource, state, false);
            enhancedBook.Restore(resource, state);
            resBook.Forget(resource);
        }
        if (list7) list7->Release();
        inlineLayouts.clear();
        inlineState.Reset();
        inlineStateActive = directStateActive = false;
        // Never ApplyTo or reset native compute bindings here: the runtime's
        // null PSO/root and partial ClearUAV invalidation must survive this scope.
    }
private:
    HRESULT BeginNrState(UINT count, ID3D12Resource** resources, D3D12_RESOURCE_STATES* states, bool split)
    {
        if (inlineStateActive) { inlineRejection = "nr_state_already_active"; return E_UNEXPECTED; }
        if (count && (!resources || !states)) { inlineRejection = "invalid_nr_inputs"; return E_INVALIDARG; }
        inlineRejection = "eligible";
        inlineLayouts.clear();
        // Preserve the pre-existing split cost for ordinary legacy games: no
        // source scan, private binding snapshot or additional restore commands.
        if (split && !enhancedBarriersSeen) return S_OK;
        // Validate every source before writing any command or changing caller states.
        for (UINT i = 0; i < count; ++i) {
            if (!resources[i]) continue;
            if (const auto* state = enhancedBook.Find(resources[i])) {
                if (!state->known) {
                    inlineRejection = state->invalidated ? "source_layout_invalidated" : "enhanced_layout_partial_or_pending";
                    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                }
                if (state->legacy) continue;
                if (std::none_of(inlineLayouts.begin(), inlineLayouts.end(), [&](const auto& p) {
                        return p.first == resources[i]; })) inlineLayouts.emplace_back(resources[i], *state);
            }
            // An untouched input retains the caller's Frame/INI legacy contract,
            // including a state established by a previously submitted list. A
            // GLOBAL barrier changes no texture layout. Never infer COMMON here.
        }
        if (split && inlineLayouts.empty()) return S_OK;
        auto* list7 = inlineLayouts.empty() ? nullptr : CurAs<ID3D12GraphicsCommandList7>();
        if (!inlineLayouts.empty() && !list7) { inlineRejection = "enhanced_interface_unavailable"; return E_NOINTERFACE; }
        inlineState.Reset();
        inlineStateActive = true;
        for (const auto& [resource, state] : inlineLayouts) {
            TransitionEnhanced(list7, resource, state, true);
            enhancedBook.Forget(resource);
            resBook.Forget(resource);
            for (UINT i = 0; i < count; ++i) if (resources[i] == resource) states[i] = D3D12_RESOURCE_STATE_COMMON;
        }
        if (list7) list7->Release();
        return S_OK;
    }
public:
    void STDMETHODCALLTYPE EndInlineState() override
    {
        if (!inlineStateActive) return;
        Cur()->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        auto* list7 = inlineLayouts.empty() ? nullptr : CurAs<ID3D12GraphicsCommandList7>();
        for (const auto& [resource, state] : inlineLayouts) {
            D3D12_RESOURCE_STATES legacy = D3D12_RESOURCE_STATE_COMMON;
            resBook.TryGet(resource, &legacy);
            if (legacy != D3D12_RESOURCE_STATE_COMMON) {
                D3D12_RESOURCE_BARRIER b {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, legacy, D3D12_RESOURCE_STATE_COMMON};
                Cur()->ResourceBarrier(1, &b);
            }
            TransitionEnhanced(list7, resource, state, false);
            enhancedBook.Restore(resource, state);
            resBook.Forget(resource);
        }
        if (list7) list7->Release();
        inlineLayouts.clear();
        // Apply to the native list: our private bindings must not overwrite the game snapshot.
        contState.ApplyTo(Cur());
        inlineState.Reset();
        inlineStateActive = false;
    }
    void STDMETHODCALLTYPE BeginPrivateBindings() override
    {
        if (inlineStateActive || privateBindings) return; // an inline scope already keeps them out of the snapshot
        inlineState.Reset();
        privateBindings = true;
    }
    void STDMETHODCALLTYPE EndPrivateBindings() override
    {
        if (!privateBindings) return;
        privateBindings = false;
        // Apply to the native list: the private bindings must not stay where the game's were. The game snapshot itself
        // was never written to, so the continuation is still seeded from the game's bindings.
        if (auto* c = Cur()) contState.ApplyTo(c);
        inlineState.Reset();
    }
    void STDMETHODCALLTYPE InvalidateDirectComputeBindings() override
    {
        // The runtime's native exposure meter can change descriptor heaps. Restore
        // only those: replaying PSO/root would overwrite its null setters and the
        // partial ClearUAV meta invalidation needed for subsequent Vulkan rebinds.
        if (auto* c = Cur(); c && contState.hasHeaps)
            c->SetDescriptorHeaps(contState.numHeaps, contState.heaps);
        // Raw Vulkan changed the compute bind point; discard its old snapshots.
        contState.OnPso(nullptr);
        contState.OnComputeRoot(nullptr);
        contState.computeRoots.Reset();
        inlineState.OnPso(nullptr);
        inlineState.OnComputeRoot(nullptr);
        inlineState.computeRoots.Reset();
    }
    void STDMETHODCALLTYPE TraceOpenQueries() override
    {
        static std::atomic<uint32_t> traces {0};
        if (queryBook.CanSplit()) return;
        const auto n = ++traces;
        if (n > 5 && n % 1000) return;
        const auto id = Identity();
        LOG_WARN("NR open queries: list={} generation={} count={} sample={}",
                 id.list, id.generation, queryBook.OpenCount(), n);
        queryBook.ForEachOpen([&](ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
            (void)heap; (void)type; (void)index;
            LOG_WARN("NR open query: heap={:p} type={} index={}", (void*)heap, unsigned(type), index);
        });
    }
    bool STDMETHODCALLTYPE InContinuation() override { return logical.WasSplit(); }
    HRESULT STDMETHODCALLTYPE ObserveResources(std::shared_ptr<RecordingObserver> observer) override
    {
        std::lock_guard lifetime(RecordingMutex());
        if (!observer) return E_INVALIDARG;
        try {
            if (!recordingObserver) recordingObserver = std::make_shared<RecordingObservers>();
            recordingObserver->resources.push_back(std::move(observer));
            return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }
    ID3D12GraphicsCommandList *STDMETHODCALLTYPE RecordingNativeList() override { return Cur(); }
    static HRESULT Create(ID3D12Device *device, ID3D12CommandAllocator *alloc, ID3D12GraphicsCommandList *real,
                          CommandListProxy **out, ID3D12PipelineState *initial = nullptr)
    {
        if (!device || !alloc || !real || !out)
            return E_INVALIDARG;
        // Never wrap our own proxy: on Proton, vkd3d's CreateCommandList can internally reach the
        // hooked CreateCommandList1, so a "real" list may already be a proxy from the inner hook.
        // Double-wrapping hands the inner proxy back to vkd3d as if it were native (crash).
        {
            ILogicalCommandList *already = nullptr;
            if (SUCCEEDED(real->QueryInterface(__uuidof(ILogicalCommandList), reinterpret_cast<void **>(&already))) &&
                already != nullptr)
            {
                already->Release();
                return E_NOINTERFACE;
            }
        }
        auto *p = new CommandListProxy();
        const HRESULT hr = p->logical.BindProducer(device, alloc, real);
        if (FAILED(hr))
        {
            delete p;
            return hr;
        }
        if (initial)
            p->TrackedState().OnPso(initial);
        *out = p;
        return S_OK;
    }

    // CreateCommandList1: real list is closed; allocator bound on first Reset.
    static HRESULT CreateClosed(ID3D12Device *device, ID3D12GraphicsCommandList *real, CommandListProxy **out)
    {
        if (!device || !real || !out)
            return E_INVALIDARG;
        // Same guard as Create: refuse to wrap an object that is already one of our proxies.
        {
            ILogicalCommandList *already = nullptr;
            if (SUCCEEDED(real->QueryInterface(__uuidof(ILogicalCommandList), reinterpret_cast<void **>(&already))) &&
                already != nullptr)
            {
                already->Release();
                return E_NOINTERFACE;
            }
        }
        auto *p = new CommandListProxy();
        const HRESULT hr = p->logical.BindClosedProducer(device, real);
        if (FAILED(hr))
        {
            delete p;
            return hr;
        }
        *out = p;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        // COM identity: IUnknown / list IIDs / our logical face stay on the proxy.
        if (riid == IID_IUnknown || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList) ||
            riid == __uuidof(ID3D12GraphicsCommandList1) || riid == __uuidof(ID3D12GraphicsCommandList2) ||
            riid == __uuidof(ID3D12GraphicsCommandList3) || riid == __uuidof(ID3D12GraphicsCommandList4) ||
            riid == __uuidof(ID3D12GraphicsCommandList5) || riid == __uuidof(ID3D12GraphicsCommandList6) ||
            riid == __uuidof(ID3D12GraphicsCommandList7) || riid == __uuidof(ID3D12GraphicsCommandList8) ||
            riid == __uuidof(ID3D12GraphicsCommandList9) || riid == __uuidof(ID3D12GraphicsCommandList10))
        {
            // Do not advertise interface versions unsupported by the native list.
            IUnknown *supported = nullptr;
            if (!Cur() || FAILED(Cur()->QueryInterface(riid, reinterpret_cast<void **>(&supported))))
                return E_NOINTERFACE;
            supported->Release();
            *ppv = static_cast<ID3D12GraphicsCommandList10 *>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IRecordingResources))
        {
            *ppv = static_cast<IRecordingResources *>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(ILogicalCommandList))
        {
            *ppv = static_cast<ILogicalCommandList *>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_CommandListPresentRS5)
        {
            auto* supported = CurAs<ID3D12CommandListPresent_RS5>();
            if (!supported) return E_NOINTERFACE;
            supported->Release();
            // Capability probing must not hand out a native alias that can survive Reset.
            *ppv = static_cast<ID3D12CommandListPresent_RS5*>(this);
            AddRef();
            return S_OK;
        }
        // Streamline / debug / vendor QIs: forward to the live producer/continuation list
        // instead of E_NOINTERFACE (燕云 crash with ArmCreate + wrap).
        if (auto *cur = logical.Current())
        {
            // Unknown interfaces cannot be redirected to a continuation. Before a cut,
            // keep compatibility but permanently refuse splitting this object.
            if (logical.WasSplit())
                return E_NOINTERFACE;
            const HRESULT hr = cur->QueryInterface(riid, ppv);
            // Observation-only interfaces cannot record commands: handing one out does not
            // let the caller bypass the recording bookkeeping rawInterfaceEscaped protects.
            // ResTrack asks every tracked command list for ID3DDestructionNotifier
            // (resource_tracking/ResTrack_dx12.cpp), and with FGInput=Upscaler (OptiFG) it
            // tracks exactly the lists NR needs — treating that QI as an escape rejected
            // every later recording for the rest of the process (Forza Horizon 6 + XeFG:
            // recorded=0, reject_causes=[native_interface_escaped], reported 2026-10-25).
            // The escape verdict must follow the interface's CAPABILITY (can it record
            // commands?), not its novelty.
            const bool observationOnly = riid == __uuidof(ID3DDestructionNotifier);
            if (SUCCEEDED(hr) && !observationOnly)
            {
                rawInterfaceEscaped = true;
                static std::atomic<uint32_t> s_escapeWarnCount{0};
                const uint32_t c = s_escapeWarnCount.fetch_add(1, std::memory_order_relaxed) + 1;
                // First WARN per event class is enough for users; the counter stays at Debug.
                if (c == 1)
                {
                    LOG_WARN("CommandListProxy {:p} rawInterfaceEscaped: riid={:08X}",
                             (void*)this, riid.Data1);
                }
                else
                {
                    LOG_DEBUG("CommandListProxy {:p} rawInterfaceEscaped: riid={:08X} (event #{})",
                              (void*)this, riid.Data1, c);
                }
            }
            return hr;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        std::unique_lock lifetime(RecordingMutex());
        const ULONG n = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!n)
        {
            if (recordingObserver) recordingObserver->Invalidated(Identity());
            recordingObserver.reset();
            lifetime.unlock(); // LogicalList destruction may wait; never hold the submission lock.
            delete this;
        }
        return n;
    }

    void STDMETHODCALLTYPE Blt(ID3D12Resource* destination, const RECT* destinationRect,
                              const RECT* destinationSubRect, UINT destinationSubresource,
                              ID3D12Resource* source, const RECT* sourceRect,
                              const SUBRESOURCE_BLT_MAP* subresources, UINT subresourceCount, UINT flags) override
    {
        // QI-only probes are harmless; an actual opaque command invalidates tracked state
        // for this recording. Unlike a raw escaped alias, this proxy face follows Reset.
        presentCommandsUsed = true;
        MarkSplitIneligible("private_present_commands");
        if (auto* current = CurAs<ID3D12CommandListPresent_RS5>())
        {
            current->Blt(destination, destinationRect, destinationSubRect, destinationSubresource,
                         source, sourceRect, subresources, subresourceCount, flags);
            current->Release();
        }
    }
    void STDMETHODCALLTYPE Present(ID3D12CommandQueue* queue, const D3D12_PRESENTSURFACE* surfaces,
                                  UINT surfaceCount, ID3D12Resource* resource, const RECT* rect,
                                  UINT flags, UINT interval, UINT presentCount, INT* result,
                                  D3D12_PRESENT* present) override
    {
        presentCommandsUsed = true;
        MarkSplitIneligible("private_present_commands");
        if (auto* current = CurAs<ID3D12CommandListPresent_RS5>())
        {
            current->Present(queue, surfaces, surfaceCount, resource, rect, flags, interval, presentCount,
                             result, present);
            current->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size, void *data) override
    {
        return logical.Current() ? logical.Current()->GetPrivateData(guid, size, data) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void *data) override
    {
        return logical.Current() ? logical.Current()->SetPrivateData(guid, size, data) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override
    {
        return logical.Current() ? logical.Current()->SetPrivateDataInterface(guid, data) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override
    {
        return logical.Current() ? logical.Current()->SetName(name) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override
    {
        return logical.Current() ? logical.Current()->GetDevice(riid, device) : E_UNEXPECTED;
    }
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return D3D12_COMMAND_LIST_TYPE_DIRECT; }

    HRESULT STDMETHODCALLTYPE Close() override { return logical.Close(); }
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *alloc, ID3D12PipelineState *initial) override
    {
        std::lock_guard lifetime(RecordingMutex());
        const auto oldIdentity = Identity();
        const HRESULT hr = logical.Reset(alloc, initial);
        if (FAILED(hr))
            return hr;
        if (recordingObserver) recordingObserver->Invalidated(oldIdentity);
        recordingObserver.reset();
        TrackedState().Reset();
        resBook.Reset();
        queryBook.Reset();
        splitIneligible = false;
        presentCommandsUsed = false;
        renderPassActive = enhancedBarriersSeen = false;
        splitIneligibleReason = nullptr;
        inlineUnsafe = inlineStateActive = privateBindings = false;
        directStateActive = predicationActive = false;
        inlineUnsafeReason = nullptr; inlineRejection = "eligible";
        enhancedBook.Reset(); inlineLayouts.clear();
        if (initial)
            TrackedState().OnPso(initial);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SplitSegments() override
    {
        if (IsSplitIneligible())
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        const char *why = nullptr;
        if (!resBook.CanSplit(&why))
        {
            MarkSplitIneligible(why ? why : "resource_state");
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        // A private NR scope restores the game snapshot after the cut, including
        // DXR state and any enhanced layouts. Certify those interfaces on the
        // continuation BEFORE anything is appended to the producer.
        HRESULT hr = logical.PrepareSplit(TrackedState().stateObject != nullptr || contState.stateObject != nullptr,
                                          !inlineLayouts.empty());
        if (FAILED(hr))
            return hr;
        // Opt-in query splice (NrSplitQuerySplice): end every pair still open at the cut
        // on the producer half, then reopen it at the head of the continuation. The book
        // keeps the entries open so the game's own EndQuery (recorded after the cut)
        // closes the reopened pair normally on the continuation. The resolved result
        // covers only the post-split half of the original scope. Recorded only after
        // PrepareSplit succeeded, so allocation/interface failures never leave an early
        // EndQuery behind on a still-usable producer; past this point only Close can
        // fail, which already leaves that list unusable for the game.
        size_t splicedQueries = 0;
        if (!queryBook.CanSplit() && SplitQuerySpliceAllowed())
        {
            if (auto *producer = logical.Current())
                queryBook.ForEachOpen([&](ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) {
                    producer->EndQuery(heap, type, index);
                    ++splicedQueries;
                });
        }
        hr = logical.CommitSplit();
        if (FAILED(hr))
            return hr;
        // Seed continuation with captured producer bindings (IA/SO/VRS included).
        TrackedState().ApplyTo(logical.Current());
        if (splicedQueries)
        {
            if (auto *cont = logical.Current())
                queryBook.ForEachOpen([&](ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) {
                    cont->BeginQuery(heap, type, index);
                });
            static std::atomic<uint32_t> s_spliceCount{0};
            const uint32_t c = s_spliceCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (c <= 10 ||
                (c <= 100 && (c % 20 == 0)) ||
                (c <= 1000 && (c % 100 == 0)) ||
                (c % 1000 == 0))
            {
                LOG_WARN("CommandListProxy {:p} split query splice: {} open pair(s) re-based across the cut;"
                         " results cover the post-split half (event #{})",
                         (void*)this, splicedQueries, c);
            }
        }
        // Producer Execute introduces a promotion/decay boundary: keep tracked
        // resources but apply Microsoft decay so continuation starts from the
        // post-Execute expectation (plan D / M3), not a wipe.
        resBook.ApplyExecuteDecay();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ExecuteOn(ID3D12CommandQueue *queue) override { return ExecuteOnWithBetween(queue, nullptr, nullptr); }
    HRESULT STDMETHODCALLTYPE ExecuteOnWithBetween(ID3D12CommandQueue *queue, BetweenCallback between,
                                                   void *betweenCtx) override
    {
        std::lock_guard lifetime(RecordingMutex());
        RecordingExecution facts {Identity(), ++executionSerial, queue};
        const auto observer = recordingObserver;
        const HRESULT hr = logical.Execute(queue, between, betweenCtx, &facts, observer.get());
        facts.status = hr;
        if (observer) observer->Executed(facts);
        return hr;
    }
    bool STDMETHODCALLTYPE IsSplitIneligible() override
    {
        // Open queries refuse a cut without the opt-in splice; with it, each open pair
        // is re-based across the cut (see SplitSegments) and both halves stay legal.
        return rawInterfaceEscaped || splitIneligible || logical.WasSplit() ||
               (!queryBook.CanSplit() && !SplitQuerySpliceAllowed()) ||
               !resBook.CanSplit(nullptr);
    }
    ID3D12CommandList *STDMETHODCALLTYPE UnsplitNativeList() override
    {
        return logical.WasSplit() ? nullptr : logical.Current();
    }
    HRESULT STDMETHODCALLTYPE PrepareUnsplitBatch(ID3D12CommandQueue* queue) override
    {
        if (!recordingObserver) return S_FALSE;
        if (batchObserver) return E_UNEXPECTED;
        batchExecution = {Identity(), ++executionSerial, queue};
        auto observer = recordingObserver;
        const auto hr = logical.PrepareUnsplitBatch(queue, batchExecution, observer.get());
        if (SUCCEEDED(hr)) batchObserver = std::move(observer);
        else { batchExecution.status = hr; observer->Executed(batchExecution); }
        return hr;
    }
    void STDMETHODCALLTYPE CompleteUnsplitBatch(ID3D12CommandQueue* queue, HRESULT submissionStatus) override
    {
        if (!batchObserver) return;
        auto observer = std::move(batchObserver);
        batchExecution.status = SUCCEEDED(submissionStatus) ?
            logical.CompleteUnsplitBatch(queue, batchExecution, observer.get()) : submissionStatus;
        observer->Executed(batchExecution);
    }
    const char *STDMETHODCALLTYPE SplitRejectionReason() override
    {
        if (rawInterfaceEscaped) return "native_interface_escaped";
        if (logical.WasSplit()) return "already_split";
        if (splitIneligibleReason) return splitIneligibleReason;
        if (!queryBook.CanSplit() && !SplitQuerySpliceAllowed()) return "open_query";
        const char *reason = nullptr;
        resBook.CanSplit(&reason);
        return reason ? reason : "eligible";
    }
    const char *SplitIneligibleReason() const { return splitIneligibleReason; }
    UINT STDMETHODCALLTYPE CapturedViewportCount() override
    {
        return TrackedState().hasViewports ? TrackedState().numViewports : 0;
    }
    BOOL STDMETHODCALLTYPE CapturedIbBound() override { return TrackedState().hasIb ? TRUE : FALSE; }
    UINT STDMETHODCALLTYPE CapturedVbSlotCount() override { return TrackedState().CapturedVbSlotCount(); }
    UINT STDMETHODCALLTYPE CapturedGfxRootCount() override { return TrackedState().gfxRoots.BoundCount(); }
    BOOL STDMETHODCALLTYPE CapturedSamplePositions() override
    {
        return TrackedState().hasSamplePositions ? TRUE : FALSE;
    }

    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *p) override
    {
        TrackedState().Reset();
        if (p)
            TrackedState().OnPso(p);
        if (auto *c = Cur())
            c->ClearState(p);
    }
    void STDMETHODCALLTYPE DrawInstanced(UINT a, UINT b, UINT d, UINT e) override
    {
        if (auto *c = Cur())
            c->DrawInstanced(a, b, d, e);
    }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT a, UINT b, UINT d, INT e, UINT f) override
    {
        if (auto *c = Cur())
            c->DrawIndexedInstanced(a, b, d, e, f);
    }
    void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override
    {
        if (auto *c = Cur())
            c->Dispatch(x, y, z);
    }
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *d, UINT64 o, ID3D12Resource *s, UINT64 so, UINT64 n) override
    {
        if (auto *c = Cur())
            c->CopyBufferRegion(d, o, s, so, n);
    }
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *d, UINT x, UINT y, UINT z,
                                             const D3D12_TEXTURE_COPY_LOCATION *s, const D3D12_BOX *b) override
    {
        if (auto *c = Cur())
            c->CopyTextureRegion(d, x, y, z, s, b);
    }
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource *d, ID3D12Resource *s) override
    {
        if (auto *c = Cur())
            c->CopyResource(d, s);
    }
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource *t, const D3D12_TILED_RESOURCE_COORDINATE *c0,
                                     const D3D12_TILE_REGION_SIZE *sz, ID3D12Resource *buf, UINT64 off,
                                     D3D12_TILE_COPY_FLAGS flags) override
    {
        if (auto *c = Cur())
            c->CopyTiles(t, c0, sz, buf, off, flags);
    }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *d, UINT ds, ID3D12Resource *s, UINT ss,
                                              DXGI_FORMAT f) override
    {
        if (auto *c = Cur())
            c->ResolveSubresource(d, ds, s, ss, f);
    }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY t) override
    {
        TrackedState().OnTopology(t);
        if (auto *c = Cur())
            c->IASetPrimitiveTopology(t);
    }
    void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D12_VIEWPORT *v) override
    {
        // Modelled: an explicit clear (n == 0) and 1..16 viewports. Anything else we cannot
        // reproduce on the continuation, and replaying a stale or invented viewport would be
        // worse than declining to split - same rule as the graphics path's "Unknown is not
        // auto-admitted, never fabricate viewport/topology".
        if (n > 16 || (n > 0 && !v))
            MarkSplitIneligible("viewport_unmodelled");
        else
            TrackedState().OnViewports(n, v);
        if (auto *c = Cur())
            c->RSSetViewports(n, v);
    }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D12_RECT *r) override
    {
        if (n > 16 || (n > 0 && !r))
            MarkSplitIneligible("scissor_unmodelled");
        else
            TrackedState().OnScissors(n, r);
        if (auto *c = Cur())
            c->RSSetScissorRects(n, r);
    }
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT f[4]) override
    {
        TrackedState().OnBlend(f);
        if (auto *c = Cur())
            c->OMSetBlendFactor(f);
    }
    void STDMETHODCALLTYPE OMSetStencilRef(UINT s) override
    {
        TrackedState().OnStencil(s);
        if (auto *c = Cur())
            c->OMSetStencilRef(s);
    }
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState *p) override
    {
        TrackedState().OnPso(p);
        if (auto *c = Cur())
            c->SetPipelineState(p);
    }
    void STDMETHODCALLTYPE ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER *b) override
    {
        if (enhancedBarriersSeen)
            for (UINT i = 0; b && i < n; ++i) enhancedBook.OnLegacyBarrier(b[i]);
        const char *why = nullptr;
        if (!resBook.OnBarriers(n, b, &why))
            MarkSplitIneligible(why ? why : "barrier");
        if (auto *c = Cur())
            c->ResourceBarrier(n, b);
    }
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *l) override
    {
        MarkSplitIneligible("bundle_state_not_captured");
        if (auto *c = Cur())
            c->ExecuteBundle(l);
    }
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap *const *h) override
    {
        TrackedState().OnHeaps(n, h);
        if (auto *c = Cur())
            c->SetDescriptorHeaps(n, h);
    }
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *s) override
    {
        TrackedState().OnComputeRoot(s);
        if (auto *c = Cur())
            c->SetComputeRootSignature(s);
    }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *s) override
    {
        TrackedState().OnGfxRoot(s);
        if (auto *c = Cur())
            c->SetGraphicsRootSignature(s);
    }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) override
    {
        if (!TrackedState().computeRoots.OnTable(i, h))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetComputeRootDescriptorTable(i, h);
    }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) override
    {
        if (!TrackedState().gfxRoots.OnTable(i, h))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetGraphicsRootDescriptorTable(i, h);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT i, UINT v, UINT o) override
    {
        if (!TrackedState().computeRoots.OnSingleConstant(i, v, o))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetComputeRoot32BitConstant(i, v, o);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT i, UINT v, UINT o) override
    {
        if (!TrackedState().gfxRoots.OnSingleConstant(i, v, o))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetGraphicsRoot32BitConstant(i, v, o);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT i, UINT n, const void *s, UINT o) override
    {
        if (!TrackedState().computeRoots.OnConstants(i, n, s, o))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetComputeRoot32BitConstants(i, n, s, o);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT i, UINT n, const void *s, UINT o) override
    {
        if (!TrackedState().gfxRoots.OnConstants(i, n, s, o))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetGraphicsRoot32BitConstants(i, n, s, o);
    }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (!TrackedState().computeRoots.OnGpuVa(i, RootBindState::EntryType::CBV, a))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetComputeRootConstantBufferView(i, a);
    }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (!TrackedState().gfxRoots.OnGpuVa(i, RootBindState::EntryType::CBV, a))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetGraphicsRootConstantBufferView(i, a);
    }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (!TrackedState().computeRoots.OnGpuVa(i, RootBindState::EntryType::SRV, a))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetComputeRootShaderResourceView(i, a);
    }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (!TrackedState().gfxRoots.OnGpuVa(i, RootBindState::EntryType::SRV, a))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetGraphicsRootShaderResourceView(i, a);
    }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (!TrackedState().computeRoots.OnGpuVa(i, RootBindState::EntryType::UAV, a))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetComputeRootUnorderedAccessView(i, a);
    }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (!TrackedState().gfxRoots.OnGpuVa(i, RootBindState::EntryType::UAV, a))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetGraphicsRootUnorderedAccessView(i, a);
    }
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *v) override
    {
        TrackedState().OnIndexBuffer(v);
        if (auto *c = Cur())
            c->IASetIndexBuffer(v);
    }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT s, UINT n, const D3D12_VERTEX_BUFFER_VIEW *v) override
    {
        TrackedState().OnVertexBuffers(s, n, v);
        if (auto *c = Cur())
            c->IASetVertexBuffers(s, n, v);
    }
    void STDMETHODCALLTYPE SOSetTargets(UINT s, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *v) override
    {
        TrackedState().OnSoTargets(s, n, v);
        if (auto *c = Cur())
            c->SOSetTargets(s, n, v);
    }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rt, BOOL single,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE *ds) override
    {
        TrackedState().OnOm(n, rt, single, ds);
        if (auto *c = Cur())
            c->OMSetRenderTargets(n, rt, single, ds);
    }
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s,
                                                 UINT n, const D3D12_RECT *r) override
    {
        if (auto *c = Cur())
            c->ClearDepthStencilView(h, f, d, s, n, r);
    }
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT c4[4], UINT n,
                                                 const D3D12_RECT *r) override
    {
        if (auto *c = Cur())
            c->ClearRenderTargetView(h, c4, n, r);
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                                        ID3D12Resource *res, const UINT v[4], UINT n,
                                                        const D3D12_RECT *r) override
    {
        if (auto *c = Cur())
            c->ClearUnorderedAccessViewUint(g, cpu, res, v, n, r);
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                                         ID3D12Resource *res, const FLOAT v[4], UINT n,
                                                         const D3D12_RECT *r) override
    {
        if (auto *c = Cur())
            c->ClearUnorderedAccessViewFloat(g, cpu, res, v, n, r);
    }
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *r, const D3D12_DISCARD_REGION *region) override
    {
        if (auto *c = Cur())
            c->DiscardResource(r, region);
    }
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *h, D3D12_QUERY_TYPE t, UINT i) override
    {
        if (!queryBook.Begin(h, t, i))
            MarkSplitIneligible("query_invalid_scope");
        if (auto *c = Cur())
            c->BeginQuery(h, t, i);
    }
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *h, D3D12_QUERY_TYPE t, UINT i) override
    {
        if (!queryBook.End(h, t, i))
            MarkSplitIneligible("query_invalid_scope");
        if (auto *c = Cur())
            c->EndQuery(h, t, i);
    }
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *h, D3D12_QUERY_TYPE t, UINT s, UINT n, ID3D12Resource *d,
                                            UINT64 o) override
    {
        if (auto *c = Cur())
            c->ResolveQueryData(h, t, s, n, d, o);
    }
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource *b, UINT64 o, D3D12_PREDICATION_OP op) override
    {
        predicationActive = b != nullptr;
        if (!inlineStateActive) MarkSplitIneligible("predication");
        if (auto *c = Cur())
            c->SetPredication(b, o, op);
    }
    void STDMETHODCALLTYPE SetMarker(UINT m, const void *p, UINT s) override
    {
        if (auto *c = Cur())
            c->SetMarker(m, p, s);
    }
    void STDMETHODCALLTYPE BeginEvent(UINT m, const void *p, UINT s) override
    {
        if (auto *c = Cur())
            c->BeginEvent(m, p, s);
    }
    void STDMETHODCALLTYPE EndEvent() override
    {
        if (auto *c = Cur())
            c->EndEvent();
    }
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *sig, UINT n, ID3D12Resource *arg, UINT64 ao,
                                           ID3D12Resource *cnt, UINT64 co) override
    {
        if (auto *c = Cur())
            c->ExecuteIndirect(sig, n, arg, ao, cnt, co);
    }

    // --- ID3D12GraphicsCommandList1 ---
    void STDMETHODCALLTYPE AtomicCopyBufferUINT(ID3D12Resource *dst, UINT64 dstOff, ID3D12Resource *src, UINT64 srcOff,
                                                UINT deps, ID3D12Resource *const *depRes,
                                                const D3D12_SUBRESOURCE_RANGE_UINT64 *depRanges) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList1>())
        {
            c->AtomicCopyBufferUINT(dst, dstOff, src, srcOff, deps, depRes, depRanges);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE AtomicCopyBufferUINT64(ID3D12Resource *dst, UINT64 dstOff, ID3D12Resource *src, UINT64 srcOff,
                                                  UINT deps, ID3D12Resource *const *depRes,
                                                  const D3D12_SUBRESOURCE_RANGE_UINT64 *depRanges) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList1>())
        {
            c->AtomicCopyBufferUINT64(dst, dstOff, src, srcOff, deps, depRes, depRanges);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT mn, FLOAT mx) override
    {
        TrackedState().OnDepthBounds(mn, mx);
        if (auto *c = CurAs<ID3D12GraphicsCommandList1>())
        {
            c->OMSetDepthBounds(mn, mx);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE SetSamplePositions(UINT samplesPerPixel, UINT numPixels,
                                              D3D12_SAMPLE_POSITION *positions) override
    {
        if (ContinuationState::SamplePositionsOverflow(samplesPerPixel, numPixels))
            MarkSplitIneligible("sample_positions");
        else
            TrackedState().OnSamplePositions(samplesPerPixel, numPixels, positions);
        if (auto *c = CurAs<ID3D12GraphicsCommandList1>())
        {
            c->SetSamplePositions(samplesPerPixel, numPixels, positions);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE ResolveSubresourceRegion(ID3D12Resource *dst, UINT dstSub, UINT dstX, UINT dstY,
                                                    ID3D12Resource *src, UINT srcSub, D3D12_RECT *srcRect,
                                                    DXGI_FORMAT fmt, D3D12_RESOLVE_MODE mode) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList1>())
        {
            c->ResolveSubresourceRegion(dst, dstSub, dstX, dstY, src, srcSub, srcRect, fmt, mode);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE SetViewInstanceMask(UINT mask) override
    {
        TrackedState().OnViewInstanceMask(mask);
        if (auto *c = CurAs<ID3D12GraphicsCommandList1>())
        {
            c->SetViewInstanceMask(mask);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList2 ---
    void STDMETHODCALLTYPE WriteBufferImmediate(UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *params,
                                                const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList2>())
        {
            c->WriteBufferImmediate(count, params, modes);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList3 ---
    void STDMETHODCALLTYPE SetProtectedResourceSession(ID3D12ProtectedResourceSession *session) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList3>())
        {
            c->SetProtectedResourceSession(session);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList4 ---
    void STDMETHODCALLTYPE BeginRenderPass(UINT numRTs, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *rts,
                                           const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *ds,
                                           D3D12_RENDER_PASS_FLAGS flags) override
    {
        MarkSplitIneligible("render_pass");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            renderPassActive = true;
            c->BeginRenderPass(numRTs, rts, ds, flags);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE EndRenderPass() override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->EndRenderPass();
            renderPassActive = false;
            c->Release();
        }
    }
    void STDMETHODCALLTYPE InitializeMetaCommand(ID3D12MetaCommand *cmd, const void *initData, SIZE_T initSize) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->InitializeMetaCommand(cmd, initData, initSize);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE ExecuteMetaCommand(ID3D12MetaCommand *cmd, const void *execData, SIZE_T execSize) override
    {
        MarkSplitIneligible("meta_command");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->ExecuteMetaCommand(cmd, execData, execSize);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc, UINT numPost,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *post) override
    {
        MarkSplitIneligible("rtas");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->BuildRaytracingAccelerationStructure(desc, numPost, post);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc, UINT numSrc,
        const D3D12_GPU_VIRTUAL_ADDRESS *src) override
    {
        MarkSplitIneligible("rtas");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->EmitRaytracingAccelerationStructurePostbuildInfo(desc, numSrc, src);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS dst,
                                                               D3D12_GPU_VIRTUAL_ADDRESS src,
                                                               D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) override
    {
        MarkSplitIneligible("rtas");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->CopyRaytracingAccelerationStructure(dst, src, mode);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject *stateObject) override
    {
        if (!stateObject) MarkSplitIneligible("null_state_object");
        TrackedState().OnStateObject(stateObject);
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->SetPipelineState1(stateObject);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC *desc) override
    {
        // DispatchRays is already recorded on the producer. Its shader tables and
        // resource barriers remain there; the continuation restores pipeline/root
        // bindings, never replays the dispatch. Keep AS-build and other guards.
        if (!desc) MarkSplitIneligible("null_dispatch_rays");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->DispatchRays(desc);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList5 ---
    void STDMETHODCALLTYPE RSSetShadingRate(D3D12_SHADING_RATE base,
                                            const D3D12_SHADING_RATE_COMBINER *combiners) override
    {
        TrackedState().OnShadingRate(base, combiners);
        if (auto *c = CurAs<ID3D12GraphicsCommandList5>())
        {
            c->RSSetShadingRate(base, combiners);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE RSSetShadingRateImage(ID3D12Resource *image) override
    {
        TrackedState().OnShadingRateImage(image);
        if (auto *c = CurAs<ID3D12GraphicsCommandList5>())
        {
            c->RSSetShadingRateImage(image);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList6 ---
    void STDMETHODCALLTYPE DispatchMesh(UINT x, UINT y, UINT z) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList6>())
        {
            c->DispatchMesh(x, y, z);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList7 ---
    void STDMETHODCALLTYPE Barrier(UINT32 numGroups, const D3D12_BARRIER_GROUP *groups) override
    {
        // Explicit opt-in still applies. Observed enhanced NR inputs require a
        // known layout and COMMON round trip; untouched inputs keep the legacy contract.
        if (!AllowEnhancedBarriers())
            MarkSplitIneligible("enhanced_barrier");
        if (auto *c = CurAs<ID3D12GraphicsCommandList7>())
        {
            enhancedBarriersSeen = true;
            for (UINT32 g = 0; groups && g < numGroups; ++g) {
                const auto& group = groups[g];
                for (UINT32 i = 0; i < group.NumBarriers; ++i) {
                    D3D12_BARRIER_SYNC before {}, after {};
                    if (group.Type == D3D12_BARRIER_TYPE_TEXTURE) {
                        const auto& b = group.pTextureBarriers[i];
                        resBook.Forget(b.pResource);
                        before = b.SyncBefore; after = b.SyncAfter;
                    } else if (group.Type == D3D12_BARRIER_TYPE_BUFFER) {
                        const auto& b = group.pBufferBarriers[i];
                        resBook.Forget(b.pResource);
                        before = b.SyncBefore; after = b.SyncAfter;
                    } else if (group.Type == D3D12_BARRIER_TYPE_GLOBAL) {
                        before = group.pGlobalBarriers[i].SyncBefore;
                        after = group.pGlobalBarriers[i].SyncAfter;
                    }
                    // A full range-aware split tracker is not available. Refuse
                    // this recording even if a later group closes the split.
                    if (before == D3D12_BARRIER_SYNC_SPLIT || after == D3D12_BARRIER_SYNC_SPLIT)
                        MarkSplitIneligible("enhanced_split_barrier");
                }
            }
            enhancedBook.Observe(numGroups, groups);
            c->Barrier(numGroups, groups);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList8 ---
    void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT front, UINT back) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList8>())
        {
            c->OMSetFrontAndBackStencilRef(front, back);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList9 ---
    void STDMETHODCALLTYPE RSSetDepthBias(FLOAT bias, FLOAT clamp, FLOAT slope) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList9>())
        {
            c->RSSetDepthBias(bias, clamp, slope);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE value) override
    {
        TrackedState().OnStripCut(value);
        if (auto *c = CurAs<ID3D12GraphicsCommandList9>())
        {
            c->IASetIndexBufferStripCutValue(value);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList10 ---
    void STDMETHODCALLTYPE SetProgram(const D3D12_SET_PROGRAM_DESC *desc) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList10>())
        {
            c->SetProgram(desc);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE DispatchGraph(const D3D12_DISPATCH_GRAPH_DESC *desc) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList10>())
        {
            c->DispatchGraph(desc);
            c->Release();
        }
    }
};
// ReShade and similar outer layers may forward our private IID while exposing
// their own standard command-list/device identity. Recover our logical proxy,
// not its native producer (which changes at a split). Keep the returned COM
// reference alive for the entire Evaluate call. No private interface: no change.
inline Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> QueryLogicalCommandList(
    IUnknown* list, bool* outRequiresOuterUnwrap = nullptr) noexcept
{
    if (outRequiresOuterUnwrap)
        *outRequiresOuterUnwrap = false;
    Microsoft::WRL::ComPtr<ILogicalCommandList> logical;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> proxy;
    if (!list) return proxy;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&logical))) && logical) {
        // Either list is our proxy or a transparent wrapper (e.g. ReShade) whose
        // _orig delegates to it: recording through the caller's outer object keeps
        // outer dispatch/bind callbacks alive and still follows Cur() across splits.
        logical.As(&proxy);
        return proxy;
    }
    // Streamline's native-object QI is also used by Util::CheckForRealObject.
    // Some wrappers do not forward private IIDs. Only accept a recovered object
    // that actually exposes our logical face; never manufacture a proxy around
    // an already-recorded native list or redirect it by pointer similarity.
    static constexpr GUID streamlineNative = {0xADEC44E2, 0x61F0, 0x45C3,
        {0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF}};
    Microsoft::WRL::ComPtr<IUnknown> native;
    if (SUCCEEDED(list->QueryInterface(streamlineNative, reinterpret_cast<void**>(native.GetAddressOf()))) && native &&
        SUCCEEDED(native.As(&logical)) && logical) {
        // The outer wrapper is opaque (Streamline interposer style) and may record
        // to a stale native producer after NR splits; the caller must unwrap it.
        if (outRequiresOuterUnwrap)
            *outRequiresOuterUnwrap = true;
        logical.As(&proxy);
    }
    return proxy;
}

// Borrowed for the duration of recording. The caller owns the proxy/list.
inline ID3D12GraphicsCommandList* GraphicsRecordingList(ID3D12GraphicsCommandList* list)
{
    if (!list) return nullptr;
    ILogicalCommandList* logical = nullptr;
    if (FAILED(list->QueryInterface(__uuidof(ILogicalCommandList), reinterpret_cast<void**>(&logical))))
        return list;
    auto* native = logical->RecordingNativeList();
    logical->Release();
    return native;
}
} // namespace DlssNr::Submission
