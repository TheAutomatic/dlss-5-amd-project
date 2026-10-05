#include "temporal_fixture.h"
#include "queue_faults.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/TemporalControl.h"
int main()try {
    Gpu g;LmxxfRuntime::TemporalControl control;control.Create(g.device.Get());
    constexpr unsigned n=33;
    auto output=g.Buffer(n*256,D3D12_HEAP_TYPE_READBACK);
    std::vector<ComPtr<ID3D12CommandAllocator>> alloc(n);
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists(n);
    for(unsigned i=0;i<n;++i){
        Check(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc[i])),"allocator");
        Check(g.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc[i].Get(),nullptr,IID_PPV_ARGS(&lists[i])),"list");
        auto* cmd=lists[i].Get();
        LmxxfTemporal::Transition(cmd,control.Resource(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(output.Get(),i*256,control.Resource(),0,256);
        LmxxfTemporal::Transition(cmd,control.Resource(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        Check(cmd->Close(),"close");
    }
    ComPtr<ID3D12Fence> gate;Check(g.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"gate");
    Check(g.queue->Wait(gate.Get(),1),"block producer");
    std::vector<std::shared_ptr<LmxxfRuntime::RecordingCompletion>> job,chain;
    // Record order differs from execution order; immutable uploads must not be
    // overwritten while the GPU is blocked, even after the original CPU data dies.
    for(unsigned i=n;i-->0;){unsigned value=100+i;
        control.Submit(g.device.Get(),g.queue.Get(),&value,sizeof(value),job,chain);
        ID3D12CommandList* cmd=lists[i].Get();g.queue->ExecuteCommandLists(1,&cmd);
    }
    unsigned cancelled=999;
    control.Submit(g.device.Get(),g.queue.Get(),&cancelled,sizeof(cancelled),job,chain);
    Require(!job.back()->Complete(),"control-only cancellation still owns pending GPU work");
    Check(gate->Signal(1),"release gate");g.Run([](auto*){});
    void* data=nullptr;Check(output->Map(0,nullptr,&data),"map");
    for(unsigned i=0;i<n;++i)Require(static_cast<unsigned*>(data)[i*64]==100+i,"delayed immutable control readback");
    output->Unmap(0,nullptr);
    for(auto& c:job)Require(c->Complete(),"each update has its own completion");
    // Signal failure is not repaired by an unrelated later successful fence.
    auto* failing=new SignalFailQueue(g.queue.Get());bool threw=false;
    try{control.Submit(g.device.Get(),failing,&cancelled,sizeof(cancelled),job,chain);}catch(...){threw=true;}
    Require(threw&&!job.back()->Complete(),"failed Signal remains uncertified");
    g.Run([](auto*){});Require(!job.back()->Complete(),"later completion cannot certify failed Signal");
    failing->Release(); // Test explicitly drained real GPU work before destroying fixtures.
    g.NoErrors();std::puts("temporal control WARP PASS: blocked/reversed updates, cancellation and Signal failure");
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
