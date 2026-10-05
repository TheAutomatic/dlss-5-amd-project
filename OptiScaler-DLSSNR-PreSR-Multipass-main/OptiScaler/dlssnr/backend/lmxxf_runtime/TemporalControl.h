#pragma once
#include "LmxxfRecordingLease.h"
#include "TemporalSupport.h"
#include <wrl/client.h>
#include <cstring>

namespace LmxxfRuntime {
// Stable GPU address read by replayable game lists. Each update has an immutable
// upload and its own completion proof, even when no game producer follows it.
class TemporalControl {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Slot {
        Ptr<ID3D12Resource> upload;
        Ptr<ID3D12CommandAllocator> allocator;
        Ptr<ID3D12GraphicsCommandList> list;
        Ptr<ID3D12Fence> fence;
        std::shared_ptr<RecordingCompletion> completion;
        UINT64 value=0;
    };
    Ptr<ID3D12Resource> control;
    std::vector<std::unique_ptr<Slot>> slots;
    static Ptr<ID3D12Resource> Buffer(ID3D12Device* device,D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES hp{};hp.Type=type;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width=256;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
        d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Ptr<ID3D12Resource> result;
        LmxxfTemporal::Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,
            type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
            nullptr,IID_PPV_ARGS(&result)),"temporal control buffer");
        return result;
    }
public:
    void Create(ID3D12Device* device){control=Buffer(device,D3D12_HEAP_TYPE_DEFAULT);}
    D3D12_GPU_VIRTUAL_ADDRESS Address()const{return control->GetGPUVirtualAddress();}
    ID3D12Resource* Resource()const{return control.Get();}
    // Caller marks the recording/chain unconfirmed before entering this method,
    // and clears that flag only on success. It retains us after a failed Signal.
    void Submit(ID3D12Device* device,ID3D12CommandQueue* queue,const void* data,size_t size,
                std::vector<std::shared_ptr<RecordingCompletion>>& job,
                std::vector<std::shared_ptr<RecordingCompletion>>& chain) {
        using LmxxfTemporal::Check;
        if(size>256)throw std::runtime_error("temporal control capacity");
        Slot* s=nullptr;
        for(auto& candidate:slots)if(!candidate->completion||candidate->completion->Complete()){
            s=candidate.get();break;
        }
        if(!s){
            auto candidate=std::make_unique<Slot>();candidate->upload=Buffer(device,D3D12_HEAP_TYPE_UPLOAD);
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&candidate->allocator)),"control allocator");
            Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,candidate->allocator.Get(),nullptr,IID_PPV_ARGS(&candidate->list)),"control list");
            Check(candidate->list->Close(),"control initial close");
            Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&candidate->fence)),"control fence");
            s=candidate.get();slots.push_back(std::move(candidate));
        }
        if(s->value>=UINT64_MAX-1)throw std::runtime_error("temporal control fence exhausted");
        void* mapped=nullptr;D3D12_RANGE read{0,0};
        Check(s->upload->Map(0,&read,&mapped),"control map");
        std::memset(mapped,0,256);std::memcpy(mapped,data,size);s->upload->Unmap(0,nullptr);
        Check(s->allocator->Reset(),"control allocator reset");
        Check(s->list->Reset(s->allocator.Get(),nullptr),"control list reset");
        LmxxfTemporal::Transition(s->list.Get(),control.Get(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,D3D12_RESOURCE_STATE_COPY_DEST);
        s->list->CopyBufferRegion(control.Get(),0,s->upload.Get(),0,256);
        LmxxfTemporal::Transition(s->list.Get(),control.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        Check(s->list->Close(),"control close");
        auto certificate=std::make_shared<RecordingCompletion>(s->fence.Get(),queue,++s->value);
        // All allocation can fail before submission, never after GPU ownership begins.
        job.push_back(certificate);chain.push_back(certificate);s->completion=certificate;
        ID3D12CommandList* list=s->list.Get();queue->ExecuteCommandLists(1,&list);
        Check(queue->Signal(s->fence.Get(),s->value),"control signal");
    }
};
}
