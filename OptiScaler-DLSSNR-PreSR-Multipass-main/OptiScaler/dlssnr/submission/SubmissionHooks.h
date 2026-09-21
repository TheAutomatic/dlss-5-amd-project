#pragma once
#include "CommandListProxy.h"
#include "SubmissionTls.h"
#include <detours/detours.h>
#include <atomic>
#include <mutex>

// G1 Create/Execute wrap for lmxxf submission.
// Default: disarmed. Product must not call Arm until G1/P3 gates.
// Harness arms on a test device/queue, then uses ordinary CreateCommandList /
// ExecuteCommandLists so the Detours path matches the game.
namespace DlssNr::Submission::Hooks
{
using BetweenFn = void (*)(void *);

inline std::mutex g_mu;
inline std::atomic<bool> g_armed { false };
inline std::atomic<bool> g_expandEnabled { false };
// Product: ProxyWrap starts OFF; swapchain ctor enables it (Streamline-safe). Harness: SetProxyWrap(true) after Arm.
// Harnesses can SetProxyWrap(true) after Arm.
inline std::atomic<bool> g_proxyWrap { false };
inline std::atomic<bool> g_wrapOpenLists { false };
inline std::mutex g_executeMu;
inline BetweenFn g_between = nullptr;
inline void *g_betweenCtx = nullptr;

using PFN_CreateCommandList = HRESULT(WINAPI *)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE,
                                                ID3D12CommandAllocator *, ID3D12PipelineState *, REFIID, void **);
using PFN_CreateCommandList1 = HRESULT(WINAPI *)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE,
                                                 D3D12_COMMAND_LIST_FLAGS, REFIID, void **);
using PFN_ExecuteCommandLists = void(WINAPI *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);

inline PFN_CreateCommandList o_CreateCommandList = nullptr;
inline PFN_CreateCommandList1 o_CreateCommandList1 = nullptr;
inline PFN_ExecuteCommandLists o_ExecuteCommandLists = nullptr;

inline bool IsArmed() { return g_armed.load(std::memory_order_acquire); }
inline bool ExpandEnabled() { return g_expandEnabled.load(std::memory_order_acquire); }
inline bool ProxyWrapEnabled() { return g_proxyWrap.load(std::memory_order_acquire); }
inline void SetProxyWrap(bool on) { g_proxyWrap.store(on, std::memory_order_release); }
inline void SetWrapOpenLists(bool on) { g_wrapOpenLists.store(on, std::memory_order_release); }

inline void SetBetween(BetweenFn fn, void *ctx)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_between = fn;
    g_betweenCtx = ctx;
}

// Wrap a newly created DIRECT list as CommandListProxy. Non-DIRECT: pass through.
inline HRESULT WrapNewList(ID3D12Device *device, ID3D12CommandAllocator *alloc, ID3D12GraphicsCommandList *real,
                           REFIID riid, void **out, ID3D12PipelineState *initial = nullptr)
{
    if (!device || !alloc || !real || !out)
        return E_INVALIDARG;
    CommandListProxy *proxy = nullptr;
    const HRESULT hr = CommandListProxy::Create(device, alloc, real, &proxy, initial);
    if (FAILED(hr))
        return hr;
    const HRESULT qi = proxy->QueryInterface(riid, out);
    proxy->Release();
    return qi;
}

inline HRESULT WrapClosedList(ID3D12Device *device, ID3D12GraphicsCommandList *real, REFIID riid, void **out)
{
    if (!device || !real || !out)
        return E_INVALIDARG;
    CommandListProxy *proxy = nullptr;
    const HRESULT hr = CommandListProxy::CreateClosed(device, real, &proxy);
    if (FAILED(hr))
        return hr;
    const HRESULT qi = proxy->QueryInterface(riid, out);
    proxy->Release();
    return qi;
}

inline HRESULT CreateProxiedCommandList(ID3D12Device *device, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                        ID3D12CommandAllocator *alloc, ID3D12PipelineState *initial, REFIID riid,
                                        void **out)
{
    if (!device || !alloc || !out)
        return E_INVALIDARG;
    *out = nullptr;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return device->CreateCommandList(nodeMask, type, alloc, initial, riid, out);

    SuppressProxyWrap suppress;
    ID3D12GraphicsCommandList *real = nullptr;
    const HRESULT hr =
        device->CreateCommandList(nodeMask, type, alloc, initial, IID_PPV_ARGS(&real));
    if (FAILED(hr))
        return hr;
    const HRESULT wrap = WrapNewList(device, alloc, real, riid, out, initial);
    real->Release();
    return wrap;
}

inline HRESULT WINAPI hkCreateCommandList(ID3D12Device *device, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                          ID3D12CommandAllocator *alloc, ID3D12PipelineState *initial, REFIID riid,
                                          void **out)
{
    // Opt-in only for proxy-original/split-original until real-game boundary validation.
    if (!IsArmed() || !ProxyWrapEnabled() || !g_wrapOpenLists.load(std::memory_order_acquire) ||
        g_suppressProxyWrap || type != D3D12_COMMAND_LIST_TYPE_DIRECT || !out)
        return o_CreateCommandList(device, nodeMask, type, alloc, initial, riid, out);
    ID3D12GraphicsCommandList *real = nullptr;
    const HRESULT hr = o_CreateCommandList(device, nodeMask, type, alloc, initial, IID_PPV_ARGS(&real));
    if (FAILED(hr))
        return hr;
    const HRESULT wrap = WrapNewList(device, alloc, real, riid, out, initial);
    real->Release();
    if (FAILED(wrap))
        *out = nullptr;
    return wrap;
}

inline HRESULT WINAPI hkCreateCommandList1(ID3D12Device *device, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                           D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **out)
{
    if (!IsArmed() || !ProxyWrapEnabled() || g_suppressProxyWrap || type != D3D12_COMMAND_LIST_TYPE_DIRECT || !o_CreateCommandList1)
        return o_CreateCommandList1 ? o_CreateCommandList1(device, nodeMask, type, flags, riid, out)
                                    : E_NOINTERFACE;

    // Create closed real list, wrap as proxy; allocator binds on first Reset.
    ID3D12GraphicsCommandList *real = nullptr;
    const HRESULT hr =
        o_CreateCommandList1(device, nodeMask, type, flags, IID_PPV_ARGS(&real));
    if (FAILED(hr))
        return hr;
    const HRESULT wrap = WrapClosedList(device, real, riid, out);
    real->Release();
    if (FAILED(wrap) && out)
        *out = nullptr;
    return wrap;
}

// Expand proxies in a batch: for each ILogicalCommandList, ExecuteOnWithBetween;
// non-proxies submitted via original ExecuteCommandLists in contiguous runs.
inline void ExecuteExpanded(ID3D12CommandQueue *queue, UINT num, ID3D12CommandList *const *lists,
                            BetweenFn between, void *betweenCtx, PFN_ExecuteCommandLists rawExec)
{
    if (!queue || !lists || !rawExec)
        return;
    // Keep all ordinary/unsplit lists in their original contiguous batch. Splitting
    // every unsplit proxy into separate Executes would itself change resource decay.
    std::lock_guard<std::mutex> submitLock(g_executeMu);
    std::vector<ID3D12CommandList *> run;
    run.reserve(num);
    const auto flush = [&]() {
        if (!run.empty())
        {
            rawExec(queue, static_cast<UINT>(run.size()), run.data());
            run.clear();
        }
    };
    for (UINT i = 0; i < num; ++i)
    {
        ILogicalCommandList *logical = nullptr;
        if (FAILED(lists[i]->QueryInterface(__uuidof(ILogicalCommandList),
                                           reinterpret_cast<void **>(&logical))) || !logical)
        {
            run.push_back(lists[i]);
            continue;
        }
        if (auto *native = logical->UnsplitNativeList())
        {
            run.push_back(native);
            g_unsplitProxySubmissions.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            flush();
            if (FAILED(logical->ExecuteOnWithBetween(queue, between, betweenCtx)))
                g_submissionFailures.fetch_add(1, std::memory_order_relaxed);
        }
        logical->Release();
    }
    flush();
}

inline void WINAPI hkExecuteCommandLists(ID3D12CommandQueue *queue, UINT num, ID3D12CommandList *const *lists)
{
    if (!IsArmed())
    {
        o_ExecuteCommandLists(queue, num, lists);
        return;
    }
    BetweenFn between = nullptr;
    void *ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        between = g_between;
        ctx = g_betweenCtx;
    }
    ExecuteExpanded(queue, num, lists, between, ctx, o_ExecuteCommandLists);
}

inline HRESULT Arm(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    if (!device || !queue)
        return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_armed.load(std::memory_order_relaxed))
        return S_FALSE;

    void **devVt = *reinterpret_cast<void ***>(device);
    void **queueVt = *reinterpret_cast<void ***>(queue);
    // ID3D12Device::CreateCommandList is vtable slot 12 (same as D3D12_Hooks).
    o_CreateCommandList = reinterpret_cast<PFN_CreateCommandList>(devVt[12]);
    // ID3D12CommandQueue::ExecuteCommandLists is vtable slot 10.
    o_ExecuteCommandLists = reinterpret_cast<PFN_ExecuteCommandLists>(queueVt[10]);

    // Optional CreateCommandList1 on ID3D12Device4 (slot 51) — may be absent.
    ID3D12Device4 *dev4 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev4))))
    {
        void **vt4 = *reinterpret_cast<void ***>(dev4);
        o_CreateCommandList1 = reinterpret_cast<PFN_CreateCommandList1>(vt4[51]);
        dev4->Release();
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (o_CreateCommandList)
        DetourAttach(reinterpret_cast<PVOID *>(&o_CreateCommandList), hkCreateCommandList);
    if (o_CreateCommandList1)
        DetourAttach(reinterpret_cast<PVOID *>(&o_CreateCommandList1), hkCreateCommandList1);
    if (o_ExecuteCommandLists)
        DetourAttach(reinterpret_cast<PVOID *>(&o_ExecuteCommandLists), hkExecuteCommandLists);
    const LONG err = DetourTransactionCommit();
    if (err != NO_ERROR)
    {
        o_CreateCommandList = nullptr;
        o_CreateCommandList1 = nullptr;
        o_ExecuteCommandLists = nullptr;
        return HRESULT_FROM_WIN32(err);
    }
    NoteRawExecuteCommandLists(o_ExecuteCommandLists);
    g_armed.store(true, std::memory_order_release);
    g_expandEnabled.store(true, std::memory_order_release);
    return S_OK;
}

// Product path: wrap CreateCommandList only. Execute expand rides AmdBridge's existing
// queue ExecuteCommandLists hook (no second Detour). Mutual exclusion vs graphics tracker.
inline HRESULT ArmCreate(ID3D12Device *device)
{
    if (!device)
        return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_armed.load(std::memory_order_relaxed))
    {
        g_expandEnabled.store(true, std::memory_order_release);
        return S_FALSE;
    }
    void **devVt = *reinterpret_cast<void ***>(device);
    o_CreateCommandList = reinterpret_cast<PFN_CreateCommandList>(devVt[12]);
    ID3D12Device4 *dev4 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev4))))
    {
        void **vt4 = *reinterpret_cast<void ***>(dev4);
        o_CreateCommandList1 = reinterpret_cast<PFN_CreateCommandList1>(vt4[51]);
        dev4->Release();
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (o_CreateCommandList)
        DetourAttach(reinterpret_cast<PVOID *>(&o_CreateCommandList), hkCreateCommandList);
    if (o_CreateCommandList1)
        DetourAttach(reinterpret_cast<PVOID *>(&o_CreateCommandList1), hkCreateCommandList1);
    const LONG err = DetourTransactionCommit();
    if (err != NO_ERROR)
    {
        o_CreateCommandList = nullptr;
        o_CreateCommandList1 = nullptr;
        return HRESULT_FROM_WIN32(err);
    }
    g_armed.store(true, std::memory_order_release);
    g_expandEnabled.store(true, std::memory_order_release);
    return S_OK;
}

inline void Disarm()
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_armed.load(std::memory_order_relaxed))
        return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (o_CreateCommandList)
        DetourDetach(reinterpret_cast<PVOID *>(&o_CreateCommandList), hkCreateCommandList);
    if (o_CreateCommandList1)
        DetourDetach(reinterpret_cast<PVOID *>(&o_CreateCommandList1), hkCreateCommandList1);
    if (o_ExecuteCommandLists)
        DetourDetach(reinterpret_cast<PVOID *>(&o_ExecuteCommandLists), hkExecuteCommandLists);
    if (DetourTransactionCommit() != NO_ERROR)
        return;
    if (o_ExecuteCommandLists)
        NoteRawExecuteCommandLists(nullptr);
    o_CreateCommandList = nullptr;
    o_CreateCommandList1 = nullptr;
    o_ExecuteCommandLists = nullptr;
    g_between = nullptr;
    g_betweenCtx = nullptr;
    g_armed.store(false, std::memory_order_release);
    g_expandEnabled.store(false, std::memory_order_release);
    g_proxyWrap.store(false, std::memory_order_release);
    g_wrapOpenLists.store(false, std::memory_order_release);
}
} // namespace DlssNr::Submission::Hooks
