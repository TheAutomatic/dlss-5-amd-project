// SPDX-License-Identifier: MIT
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <detours.h>
#include <wrl/client.h>

#include "../submission/CommandListProxy.h"
#include "../submission/GpuBatchTracker.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace openrr_asi {

// Tracks D3D12 command list state for BOTH:
// 1. Direct ELF route (lightweight 4-slot vtable tracking: SetDescriptorHeaps, SetComputeRootSignature, Reset, Close
//    with zero proxy wrapper on game command lists, restoring compute heaps + root signature after OnrRrRecord).
// 2. Vulkan Interop Bridge route (wraps DIRECT command lists in DlssNr::Submission::CommandListProxy when enabled,
//    expanding producer/continuation segments in ExecuteCommandLists with a mutex-protected Between callback).
class D3d12Bridge {
public:
    using BetweenCallback = int32_t(*)(void* owner, void* job, ID3D12CommandQueue* queue);

    static D3d12Bridge& Instance() {
        static auto* inst = new D3d12Bridge();
        return *inst;
    }

    void SetEnableSplitWrap(bool enable) {
        enableSplitWrap_.store(enable, std::memory_order_release);
    }

    void AttachToDevice(ID3D12Device* device) {
        if (!device) return;
        std::lock_guard lock(hookMu_);
        if (hooked_) return;

        // Slots copied verbatim from the shipping product, not computed:
        //   SubmissionHooks.h:373-379 -> ID3D12Device::CreateCommandList = devVt[12],
        //                                ID3D12CommandQueue::ExecuteCommandLists = queueVt[10],
        //                                ID3D12Device4::CreateCommandList1 = vt4[51]
        //   D3D12_Hooks.cpp:1867-1871  -> ID3D12GraphicsCommandList::Reset = pVTable[10],
        //                                ClearState = 11, DrawInstanced = 12, ExecuteBundle = 27
        // hence Close = 9, SetDescriptorHeaps = 28, SetComputeRootSignature = 29.
        void** devVtbl = *reinterpret_cast<void***>(device);
        o_CreateCommandList_ = reinterpret_cast<PFN_CreateCommandList>(devVtbl[12]);

        Microsoft::WRL::ComPtr<ID3D12Device4> dev4;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev4))) && dev4) {
            void** dev4Vtbl = *reinterpret_cast<void***>(dev4.Get());
            o_CreateCommandList1_ = reinterpret_cast<PFN_CreateCommandList1>(dev4Vtbl[51]);
        }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> tmpQueue;
        if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&tmpQueue))) && tmpQueue) {
            void** qVtbl = *reinterpret_cast<void***>(tmpQueue.Get());
            o_ExecuteCommandLists_ = reinterpret_cast<PFN_ExecuteCommandLists>(qVtbl[10]);
        }

        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> tmpAlloc;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> tmpList;
        if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tmpAlloc))) &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tmpAlloc.Get(), nullptr, IID_PPV_ARGS(&tmpList)))) {
            void** clVtbl = *reinterpret_cast<void***>(tmpList.Get());
            o_Close_ = reinterpret_cast<PFN_Close>(clVtbl[9]);
            o_Reset_ = reinterpret_cast<PFN_Reset>(clVtbl[10]);
            o_SetDescriptorHeaps_ = reinterpret_cast<PFN_SetDescriptorHeaps>(clVtbl[28]);
            o_SetComputeRootSignature_ = reinterpret_cast<PFN_SetComputeRootSignature>(clVtbl[29]);
            tmpList->Close();
        }

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        if (o_CreateCommandList_)
            DetourAttach(reinterpret_cast<PVOID*>(&o_CreateCommandList_), &HkCreateCommandList);
        if (o_CreateCommandList1_)
            DetourAttach(reinterpret_cast<PVOID*>(&o_CreateCommandList1_), &HkCreateCommandList1);
        if (o_ExecuteCommandLists_)
            DetourAttach(reinterpret_cast<PVOID*>(&o_ExecuteCommandLists_), &HkExecuteCommandLists);
        if (o_Close_)
            DetourAttach(reinterpret_cast<PVOID*>(&o_Close_), &HkClose);
        if (o_Reset_)
            DetourAttach(reinterpret_cast<PVOID*>(&o_Reset_), &HkReset);
        if (o_SetDescriptorHeaps_)
            DetourAttach(reinterpret_cast<PVOID*>(&o_SetDescriptorHeaps_), &HkSetDescriptorHeaps);
        if (o_SetComputeRootSignature_)
            DetourAttach(reinterpret_cast<PVOID*>(&o_SetComputeRootSignature_), &HkSetComputeRootSignature);

        if (DetourTransactionCommit() == NO_ERROR) {
            hooked_ = true;
        }
    }

    struct ScopedPrivateComputeBindings {
        ID3D12GraphicsCommandList* cl = nullptr;
        Microsoft::WRL::ComPtr<DlssNr::Submission::IRecordingResources> proxyRec;
        ID3D12DescriptorHeap* savedCbvSrvUav = nullptr;
        ID3D12DescriptorHeap* savedSampler = nullptr;
        ID3D12RootSignature* savedComputeSig = nullptr;
        bool hadState = false;

        explicit ScopedPrivateComputeBindings(ID3D12GraphicsCommandList* list) : cl(list) {
            if (!cl) return;
            ++s_privateDepth_;
            if (SUCCEEDED(cl->QueryInterface(__uuidof(DlssNr::Submission::IRecordingResources),
                                             reinterpret_cast<void**>(proxyRec.GetAddressOf()))) && proxyRec) {
                proxyRec->BeginPrivateBindings();
                return;
            }
            auto& self = D3d12Bridge::Instance();
            std::lock_guard lock(self.stateMu_);
            auto it = self.listState_.find(cl);
            if (it != self.listState_.end()) {
                savedCbvSrvUav = it->second.cbvSrvUavHeap;
                savedSampler = it->second.samplerHeap;
                savedComputeSig = it->second.computeRootSig;
                hadState = true;
            }
        }

        ~ScopedPrivateComputeBindings() {
            if (!cl) return;
            if (proxyRec) {
                proxyRec->EndPrivateBindings();
                --s_privateDepth_;
                return;
            }
            auto& self = D3d12Bridge::Instance();
            if (hadState) {
                ID3D12DescriptorHeap* heaps[2]{};
                UINT numHeaps = 0;
                if (savedCbvSrvUav) heaps[numHeaps++] = savedCbvSrvUav;
                if (savedSampler) heaps[numHeaps++] = savedSampler;
                if (numHeaps > 0 && self.o_SetDescriptorHeaps_)
                    self.o_SetDescriptorHeaps_(cl, numHeaps, heaps);
                if (savedComputeSig && self.o_SetComputeRootSignature_)
                    self.o_SetComputeRootSignature_(cl, savedComputeSig);
            }
            --s_privateDepth_;
        }
    };

    uint64_t RecordTicket(ID3D12GraphicsCommandList* cl) {
        return DlssNr::Submission::GpuBatch::Tracker::Instance().Record(cl);
    }

    bool IsTicketRetired(uint64_t ticket) {
        return DlssNr::Submission::GpuBatch::Tracker::Instance().Retired(ticket);
    }

    void ArmPendingBetween(void* owner, void* job, BetweenCallback cb, ID3D12CommandList* targetList, std::recursive_mutex* ownerMutex) {
        std::lock_guard lock(betweenMu_);
        pendingOwner_ = owner;
        pendingJob_ = job;
        pendingCb_ = cb;
        pendingList_ = targetList;
        pendingOwnerMutex_ = ownerMutex;
    }

    bool CancelPendingBetween(void* owner, void* job, ID3D12CommandList* targetList) {
        std::lock_guard lock(betweenMu_);
        if (pendingOwner_ == owner && pendingJob_ == job && pendingList_ == targetList) {
            pendingOwner_ = nullptr;
            pendingJob_ = nullptr;
            pendingCb_ = nullptr;
            pendingList_ = nullptr;
            pendingOwnerMutex_ = nullptr;
            return true;
        }
        return false;
    }

    int32_t ConsumeLastEnqueueRc() {
        return lastEnqueueRc_.exchange(0, std::memory_order_acq_rel);
    }

private:
    struct ListState {
        ID3D12DescriptorHeap* cbvSrvUavHeap = nullptr;
        ID3D12DescriptorHeap* samplerHeap = nullptr;
        ID3D12RootSignature* computeRootSig = nullptr;
    };

    using PFN_CreateCommandList = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE,
                                                              ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
    using PFN_CreateCommandList1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE,
                                                               D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
    using PFN_ExecuteCommandLists = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    using PFN_Close = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
    using PFN_Reset = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
    using PFN_SetDescriptorHeaps = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
    using PFN_SetComputeRootSignature = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);

    static inline thread_local int s_privateDepth_ = 0;
    static inline thread_local int s_internalCreateDepth_ = 0;

    static HRESULT STDMETHODCALLTYPE HkCreateCommandList(ID3D12Device* dev, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                                         ID3D12CommandAllocator* alloc, ID3D12PipelineState* initial,
                                                         REFIID riid, void** ppList) {
        auto& self = Instance();
        if (!self.enableSplitWrap_.load(std::memory_order_acquire) || s_internalCreateDepth_ > 0 ||
            DlssNr::Submission::InInternalCreate() || type != D3D12_COMMAND_LIST_TYPE_DIRECT || !alloc || !ppList) {
            return self.o_CreateCommandList_(dev, nodeMask, type, alloc, initial, riid, ppList);
        }

        ++s_internalCreateDepth_;
        ID3D12GraphicsCommandList* real = nullptr;
        HRESULT hr = self.o_CreateCommandList_(dev, nodeMask, type, alloc, initial, IID_PPV_ARGS(&real));
        --s_internalCreateDepth_;
        if (FAILED(hr) || !real) return hr;

        DlssNr::Submission::CommandListProxy* proxy = nullptr;
        hr = DlssNr::Submission::CommandListProxy::Create(dev, alloc, real, &proxy, initial);
        real->Release();
        if (FAILED(hr) || !proxy) return hr;

        hr = proxy->QueryInterface(riid, ppList);
        proxy->Release();
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE HkCreateCommandList1(ID3D12Device4* dev4, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                                          D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** ppList) {
        auto& self = Instance();
        if (!self.enableSplitWrap_.load(std::memory_order_acquire) || s_internalCreateDepth_ > 0 ||
            DlssNr::Submission::InInternalCreate() || type != D3D12_COMMAND_LIST_TYPE_DIRECT || !ppList) {
            return self.o_CreateCommandList1_(dev4, nodeMask, type, flags, riid, ppList);
        }

        ++s_internalCreateDepth_;
        ID3D12GraphicsCommandList* real = nullptr;
        HRESULT hr = self.o_CreateCommandList1_(dev4, nodeMask, type, flags, IID_PPV_ARGS(&real));
        --s_internalCreateDepth_;
        if (FAILED(hr) || !real) return hr;

        DlssNr::Submission::CommandListProxy* proxy = nullptr;
        hr = DlssNr::Submission::CommandListProxy::CreateClosed(dev4, real, &proxy);
        real->Release();
        if (FAILED(hr) || !proxy) return hr;

        hr = proxy->QueryInterface(riid, ppList);
        proxy->Release();
        return hr;
    }

    struct BetweenThunkCtx {
        D3d12Bridge* self;
        ID3D12CommandList* list;
    };

    static void StaticBetweenCallback(ID3D12CommandQueue* queue, void* rawCtx) {
        auto* ctx = static_cast<BetweenThunkCtx*>(rawCtx);
        ctx->self->RunPendingBetween(queue, ctx->list);
    }

    void RunPendingBetween(ID3D12CommandQueue* queue, ID3D12CommandList* list) {
        void* owner = nullptr;
        void* job = nullptr;
        BetweenCallback cb = nullptr;
        std::recursive_mutex* ownerMu = nullptr;
        {
            std::lock_guard lock(betweenMu_);
            if (pendingList_ != list || !pendingCb_ || !pendingOwner_ || !pendingJob_)
                return;
            owner = pendingOwner_;
            job = pendingJob_;
            cb = pendingCb_;
            ownerMu = pendingOwnerMutex_;
            pendingOwner_ = nullptr;
            pendingJob_ = nullptr;
            pendingCb_ = nullptr;
            pendingList_ = nullptr;
            pendingOwnerMutex_ = nullptr;
        }
        int32_t rc = 0;
        if (ownerMu) {
            std::lock_guard ownerLock(*ownerMu);
            rc = cb(owner, job, queue);
        } else {
            rc = cb(owner, job, queue);
        }
        if (rc != 0)
            lastEnqueueRc_.store(rc, std::memory_order_release);
    }

    static void STDMETHODCALLTYPE HkExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
        auto& self = Instance();
        if (!queue || !lists || count == 0 || DlssNr::Submission::InInternalExecute()) {
            if (self.o_ExecuteCommandLists_)
                self.o_ExecuteCommandLists_(queue, count, lists);
            return;
        }

        // Fast check whether any list in the batch is a Split proxy.
        bool anyProxy = false;
        for (UINT i = 0; i < count; ++i) {
            if (!lists[i]) continue;
            DlssNr::Submission::ILogicalCommandList* logical = nullptr;
            if (SUCCEEDED(lists[i]->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                                   reinterpret_cast<void**>(&logical))) && logical) {
                logical->Release();
                anyProxy = true;
                break;
            }
        }

        if (!anyProxy) {
            self.o_ExecuteCommandLists_(queue, count, lists);
            DlssNr::Submission::GpuBatch::Tracker::Instance().OnExecuted(queue, count, lists);
            return;
        }

        DlssNr::Submission::ScopedInternalExecute guard;
        std::vector<ID3D12CommandList*> batch;
        batch.reserve(count);
        auto flushBatch = [&]() {
            if (!batch.empty()) {
                self.o_ExecuteCommandLists_(queue, static_cast<UINT>(batch.size()), batch.data());
                batch.clear();
            }
        };

        for (UINT i = 0; i < count; ++i) {
            ID3D12CommandList* item = lists[i];
            DlssNr::Submission::ILogicalCommandList* logical = nullptr;
            if (item && SUCCEEDED(item->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                                       reinterpret_cast<void**>(&logical))) && logical) {
                if (ID3D12CommandList* unsplit = logical->UnsplitHardwareList()) {
                    batch.push_back(unsplit);
                } else {
                    flushBatch();
                    BetweenThunkCtx bctx{&self, item};
                    logical->ExecuteOnWithBetween(queue, &StaticBetweenCallback, &bctx);
                }
                logical->Release();
            } else if (item) {
                batch.push_back(item);
            }
        }
        flushBatch();
        DlssNr::Submission::GpuBatch::Tracker::Instance().OnExecuted(queue, count, lists);
    }

    static HRESULT STDMETHODCALLTYPE HkClose(ID3D12GraphicsCommandList* cl) {
        auto& self = Instance();
        {
            std::lock_guard lock(self.stateMu_);
            self.listState_.erase(cl);
        }
        return self.o_Close_(cl);
    }

    static HRESULT STDMETHODCALLTYPE HkReset(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso) {
        auto& self = Instance();
        {
            std::lock_guard lock(self.stateMu_);
            self.listState_[cl] = ListState{};
        }
        return self.o_Reset_(cl, alloc, pso);
    }

    static void STDMETHODCALLTYPE HkSetDescriptorHeaps(ID3D12GraphicsCommandList* cl, UINT count, ID3D12DescriptorHeap* const* heaps) {
        auto& self = Instance();
        if (s_privateDepth_ == 0 && cl) {
            ListState st{};
            for (UINT i = 0; i < count; ++i) {
                if (!heaps[i]) continue;
                const auto type = heaps[i]->GetDesc().Type;
                if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                    st.cbvSrvUavHeap = heaps[i];
                else if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
                    st.samplerHeap = heaps[i];
            }
            std::lock_guard lock(self.stateMu_);
            auto& cur = self.listState_[cl];
            cur.cbvSrvUavHeap = st.cbvSrvUavHeap;
            cur.samplerHeap = st.samplerHeap;
        }
        self.o_SetDescriptorHeaps_(cl, count, heaps);
    }

    static void STDMETHODCALLTYPE HkSetComputeRootSignature(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* sig) {
        auto& self = Instance();
        if (s_privateDepth_ == 0 && cl) {
            std::lock_guard lock(self.stateMu_);
            self.listState_[cl].computeRootSig = sig;
        }
        self.o_SetComputeRootSignature_(cl, sig);
    }

    std::mutex hookMu_;
    bool hooked_ = false;
    std::atomic<bool> enableSplitWrap_{false};

    PFN_CreateCommandList o_CreateCommandList_ = nullptr;
    PFN_CreateCommandList1 o_CreateCommandList1_ = nullptr;
    PFN_ExecuteCommandLists o_ExecuteCommandLists_ = nullptr;
    PFN_Close o_Close_ = nullptr;
    PFN_Reset o_Reset_ = nullptr;
    PFN_SetDescriptorHeaps o_SetDescriptorHeaps_ = nullptr;
    PFN_SetComputeRootSignature o_SetComputeRootSignature_ = nullptr;

    std::mutex stateMu_;
    std::unordered_map<ID3D12GraphicsCommandList*, ListState> listState_;

    std::mutex betweenMu_;
    void* pendingOwner_ = nullptr;
    void* pendingJob_ = nullptr;
    BetweenCallback pendingCb_ = nullptr;
    ID3D12CommandList* pendingList_ = nullptr;
    std::recursive_mutex* pendingOwnerMutex_ = nullptr;
    std::atomic<int32_t> lastEnqueueRc_{0};
};

} // namespace openrr_asi
