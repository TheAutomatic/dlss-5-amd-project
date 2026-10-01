#include "nr_effects_test_utils.h"

static Ptr<ID3D12Resource> Guide(ID3D12Device* d, ID3D12CommandQueue* q, DXGI_FORMAT format, float value)
{
    auto desc=Effects::Storage::Description(32,24);desc.Format=format;D3D12_HEAP_PROPERTIES hp {};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    Ptr<ID3D12Resource> r;Check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r)),"guide");
    Ptr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};Check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"guide heap");
    D3D12_UNORDERED_ACCESS_VIEW_DESC view {};view.Format=format;view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d->CreateUnorderedAccessView(r.Get(),nullptr,&view,heap->GetCPUDescriptorHandleForHeapStart());
    auto rec=NewRecording(d);auto* raw=heap.Get();rec.proxy->SetDescriptorHeaps(1,&raw);
    Effects::Barrier(rec.proxy.Get(),r.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const float values[]={value,value,value,value};rec.proxy->ClearUnorderedAccessViewFloat(heap->GetGPUDescriptorHandleForHeapStart(),heap->GetCPUDescriptorHandleForHeapStart(),r.Get(),values,0,nullptr);
    Effects::Barrier(rec.proxy.Get(),r.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Check(rec.proxy->Close(),"guide close");Check(rec.proxy->ExecuteOn(q),"guide submit");WaitQueue(d,q);return r;
}
int main()
{
    Ptr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    Ptr<IDXGIFactory4> factory;Ptr<IDXGIAdapter> adapter;Ptr<ID3D12Device> d;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");SelectEffectsAdapter(factory.Get(), &adapter);
    Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)),"device");
    Ptr<ID3D12CommandQueue> q,other;D3D12_COMMAND_QUEUE_DESC qd {};Check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)),"queue");Check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&other)),"other");
    auto original=Texture(d.Get(),32,24),result=Texture(d.Get(),32,24);
    g_patternExponentShift=0;UploadColorPattern(d.Get(),q.Get(),original.Get());g_patternExponentShift=1;UploadColorPattern(d.Get(),q.Get(),result.Get());g_patternExponentShift=0;
    auto motion=Guide(d.Get(),q.Get(),DXGI_FORMAT_R32G32_FLOAT,0),depth=Guide(d.Get(),q.Get(),DXGI_FORMAT_R32_FLOAT,.5f);
    Effects::Guides guides;guides.motion=motion.Get();guides.depth=depth.Get();guides.motionWidth=32;guides.motionHeight=24;
    auto record=[&](float intensity=1.f){auto r=NewRecording(d.Get());r.output=Effects::Record(r.proxy.Get(),original.Get(),result.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,32,24,intensity,true,guides,{true,.8f,4});Check(r.proxy->Close(),"close");return r;};
    auto& state=Effects::Global();
    {auto discarded=record();Require(discarded.output!=result.Get(),"stabilizer active");Require(!state.history,"Record must not publish history");}
    Effects::Poll();Require(NoLeases(),"discarded recording collected");
    auto first=record();auto firstLease=state.leases.back();Check(first.proxy->ExecuteOn(q.Get()),"first submit");Require(state.history==firstLease->storage,"submit publishes history");WaitQueue(d.Get(),q.Get());CheckPixels(d.Get(),q.Get(),first.output,1.f);
    auto second=record();auto secondLease=state.leases.back();Require(secondLease->previous==firstLease->storage,"captures submitted history");
    auto third=record();auto thirdLease=state.leases.back();Require(thirdLease->previous==firstLease->storage,"unsubmitted second cannot advance history");
    Ptr<ID3D12Fence> gate;Check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"gate");
    Check(q->Wait(gate.Get(),1),"hold queue");Check(third.proxy->ExecuteOn(q.Get()),"newer token first");
    Check(second.proxy->ExecuteOn(other.Get()),"cross queue history reader");
    Require(state.history==thirdLease->storage,"late old token cannot replace newer history");
    Ptr<ID3D12Fence> reached;Check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&reached)),"reached");Check(other->Signal(reached.Get(),1),"other marker");
    Require(reached->GetCompletedValue()==0,"history chain enqueues cross queue dependency");
    Check(gate->Signal(1),"release");WaitQueue(d.Get(),other.Get());
    Check(first.proxy->ExecuteOn(other.Get()),"old history writer replay");Require(state.history==thirdLease->storage,"replay never republishes history");WaitQueue(d.Get(),other.Get());
    auto oldEpoch=record();auto oldLease=state.leases.back();Effects::InvalidateHistory();
    Check(oldEpoch.proxy->ExecuteOn(q.Get()),"old epoch submit");Require(!state.history,"old epoch cannot resurrect reset history");WaitQueue(d.Get(),q.Get());
    auto fresh=record();Require(!state.leases.back()->previous,"reset starts without history");Check(fresh.proxy->ExecuteOn(q.Get()),"fresh");WaitQueue(d.Get(),q.Get());
    guides.preExposure=2;auto exposed=record();Require(!state.leases.back()->previous,"exposure jump rejects history");guides.preExposure=1;
    guides.reset=true;auto reset=record();Require(!state.leases.back()->previous,"scene reset rejects history");guides.reset=false;
    guides.depth=nullptr;auto missing=record();Require(missing.output==result.Get()&&!state.history,"missing depth identity bypass");
    auto blend=record(.5f);Require(blend.output!=result.Get(),"invalid guides still permit intensity");Check(blend.proxy->ExecuteOn(q.Get()),"fallback blend");WaitQueue(d.Get(),q.Get());CheckPixels(d.Get(),q.Get(),blend.output,.5f);guides.depth=depth.Get();
    auto zero=record(0);Require(zero.output==original.Get()&&!state.history,"zero intensity resets history");
    first.proxy.Reset();second.proxy.Reset();third.proxy.Reset();oldEpoch.proxy.Reset();fresh.proxy.Reset();exposed.proxy.Reset();reset.proxy.Reset();missing.proxy.Reset();blend.proxy.Reset();zero.proxy.Reset();
    firstLease.reset();secondLease.reset();thirdLease.reset();oldLease.reset();Effects::Reset();Effects::Poll();Require(NoLeases(),"all completed/abandoned recordings released");
    // Real pending execution survives invalidation. History publication is
    // independent of whether the application retains its command-list pointer.
    auto pending=record();Check(q->Wait(gate.Get(),2),"hold pending");Check(pending.proxy->ExecuteOn(q.Get()),"pending");
    Ptr<ID3D12CommandAllocator> allocator;Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"replacement");Check(pending.proxy->Reset(allocator.Get(),nullptr),"pending Reset");
    Effects::Reset();Effects::Poll();Require(!NoLeases(),"pending history pinned");Check(gate->Signal(2),"release pending");WaitQueue(d.Get(),q.Get());Effects::Poll();Require(NoLeases(),"completed history collected");Check(pending.proxy->Close(),"close replacement");pending.proxy.Reset();
    std::vector<Recording> held;for(unsigned i=0;i<16;++i)held.push_back(record());auto overflow=record();Require(overflow.output==result.Get(),"bounded texture budget");held.clear();overflow.proxy.Reset();Effects::Reset();Effects::Poll();Require(NoLeases(),"cap resources collected");
    // Inject notification failure after a real completed submission. No unsafe
    // GPU work is created by the fixture, but the production failure latch must
    // still prohibit reuse and must not publish the failed execution as history.
    auto failed=record();auto lease=state.leases.back();Check(failed.proxy->ExecuteOn(q.Get()),"failure fixture submit");WaitQueue(d.Get(),q.Get());Effects::InvalidateHistory();
    Submission::RecordingExecution facts {lease->identity,99,q.Get()};facts.producerSubmitted=true;facts.status=E_FAIL;lease->Executed(facts);
    Require(!state.history&&lease->unconfirmed&&lease->storage->pipeline->unconfirmed,"failed Signal retains and rejects history");Require(FAILED(lease->BeforeExecute(facts)),"failed Signal rejects replay");
    auto recovered=record();Require(recovered.output!=result.Get()&&state.pipeline!=lease->storage->pipeline,"new generation isolates unconfirmed history");
    // Fixture-only cleanup: the injected failure happened after WaitQueue.
    lease->unconfirmed=false;failed.proxy.Reset();recovered.proxy.Reset();lease.reset();Effects::Reset();Effects::Poll();Require(NoLeases(),"fixture cleanup");
    Ptr<ID3D12InfoQueue> info;if(SUCCEEDED(d.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<char> data(bytes);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());Check(info->GetMessage(i,m,&bytes),"debug message");if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)std::fprintf(stderr,"%s\n",m->pDescription);Require(m->Severity>D3D12_MESSAGE_SEVERITY_ERROR,"debug validation");}
    std::puts("NR stabilizer lifecycle: PASS (submission, replay, queue dependencies, resets, failures, budget)");
}
