#include "nr_effects_test_utils.h"
int main()
{
    Ptr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    Ptr<IDXGIFactory4> factory;Ptr<IDXGIAdapter> adapter;Ptr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP");
    Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
    Ptr<ID3D12CommandQueue> queue,other;D3D12_COMMAND_QUEUE_DESC qd {};
    Check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");Check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&other)),"other queue");
    constexpr UINT width=32,height=24;
    auto original=Texture(device.Get(),width,height), result=Texture(device.Get(),width,height);
    g_patternExponentShift=0;UploadColorPattern(device.Get(),queue.Get(),original.Get());
    g_patternExponentShift=1;UploadColorPattern(device.Get(),queue.Get(),result.Get());g_patternExponentShift=0;
    auto record=[&](float intensity){auto r=NewRecording(device.Get());
        r.output=Effects::Record(r.proxy.Get(),original.Get(),result.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,width,height,intensity,true);
        Check(r.proxy->Close(),"close effects");return r;};
    {auto zero=record(0),one=record(1),invalid=record(std::numeric_limits<float>::quiet_NaN());
     Require(zero.output==original.Get()&&one.output==result.Get()&&invalid.output==result.Get(),"identity bypass pointers");
     Require(Effects::Global().storage.empty()&&NoLeases(),"identity path has no GPU effect allocations");}
    auto half=record(.5f),full=record(2.f);
    Require(half.output!=result.Get()&&full.output!=result.Get()&&half.output!=full.output,"distinct closed recording resources");
    Effects::Reset(); // Active owner gone, closed lists retain exact dependencies.
    Check(half.proxy->ExecuteOn(queue.Get()),"delayed execute");WaitQueue(device.Get(),queue.Get());CheckPixels(device.Get(),queue.Get(),half.output,.5f);
    Check(full.proxy->ExecuteOn(other.Get()),"second output");WaitQueue(device.Get(),other.Get());CheckPixels(device.Get(),other.Get(),full.output,2.f);
    Check(half.proxy->ExecuteOn(other.Get()),"cross-queue replay");WaitQueue(device.Get(),other.Get());CheckPixels(device.Get(),other.Get(),half.output,.5f);
    Ptr<ID3D12Fence> gate;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"gate");
    Check(other->Wait(gate.Get(),1),"hold replay");Check(half.proxy->ExecuteOn(other.Get()),"pending replay");
    Ptr<ID3D12CommandAllocator> replacement;Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&replacement)),"replacement");
    Check(half.proxy->Reset(replacement.Get(),nullptr),"Reset pending recording");
    Effects::Poll();Require(!NoLeases(),"pending output retained after Reset");
    Check(gate->Signal(1),"release replay");WaitQueue(device.Get(),other.Get());
    Check(half.proxy->Close(),"close replacement");half.proxy.Reset();full.proxy.Reset();Effects::Poll();
    Require(NoLeases(),"invalidated completed effects collected");
    std::vector<Recording> held;for(unsigned i=0;i<16;++i)held.push_back(record(.5f));
    auto overflow=record(.5f);Require(overflow.output==result.Get(),"resource cap bypass");held.clear();overflow.proxy.Reset();Effects::Reset();Effects::Poll();
    Require(NoLeases(),"unsubmitted resources released");
    g_patternExponentShift=4;UploadColorPattern(device.Get(),queue.Get(),original.Get());
    g_patternExponentShift=5;UploadColorPattern(device.Get(),queue.Get(),result.Get());g_patternExponentShift=4;
    auto hdr=record(.25f);Check(hdr.proxy->ExecuteOn(queue.Get()),"HDR attenuation");WaitQueue(device.Get(),queue.Get());CheckPixels(device.Get(),queue.Get(),hdr.output,.25f);
    hdr.proxy.Reset();Effects::Reset();
    g_patternExponentShift=6;UploadColorPattern(device.Get(),queue.Get(),result.Get());g_patternExponentShift=4;
    auto guarded=record(2.f);Check(guarded.proxy->ExecuteOn(queue.Get()),"guarded amplification");WaitQueue(device.Get(),queue.Get());CheckPixels(device.Get(),queue.Get(),guarded.output,5.f);
    guarded.proxy.Reset();Effects::Reset();g_patternExponentShift=0;
    Ptr<ID3D12InfoQueue> info;if(SUCCEEDED(device.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i){
        SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<char> storage(bytes);auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(info->GetMessage(i,m,&bytes),"debug message");if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)std::fprintf(stderr,"%s\n",m->pDescription);
        Require(m->Severity>D3D12_MESSAGE_SEVERITY_ERROR,"debug validation");}
    std::puts("NR output effects: PASS (identity, numerical blend, replay, pending Reset, cap)");
}
