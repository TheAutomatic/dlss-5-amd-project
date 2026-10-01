#include "../lmxxf/lmxxf_gpu_test_utils.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/effects/NrOutputEffects.h"
#include <limits>
using namespace DlssNr;
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
static float Half(UINT16 h)
{
    const float sign = h & 0x8000 ? -1.f : 1.f;
    const unsigned exponent = (h >> 10) & 31, fraction = h & 1023;
    return sign * (exponent ? std::ldexp(1.f + float(fraction)/1024.f, int(exponent)-15) : std::ldexp(float(fraction), -24));
}
static Ptr<ID3D12Resource> Texture(ID3D12Device* device, UINT w, UINT h)
{
    D3D12_RESOURCE_DESC desc {}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=w; desc.Height=h; desc.DepthOrArraySize=desc.MipLevels=1; desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp {}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    Ptr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                         nullptr,IID_PPV_ARGS(&resource)),"texture"); return resource;
}
struct Recording
{
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<Submission::CommandListProxy> proxy;
    ID3D12Resource* output = nullptr;
};
static Recording NewRecording(ID3D12Device* device)
{
    Recording out; Ptr<ID3D12GraphicsCommandList> real;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&out.allocator)),"allocator");
    Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,out.allocator.Get(),nullptr,IID_PPV_ARGS(&real)),"list");
    Submission::CommandListProxy* proxy=nullptr;
    Check(Submission::CommandListProxy::Create(device,out.allocator.Get(),real.Get(),&proxy),"proxy"); out.proxy.Attach(proxy);
    return out;
}
static void CheckPixels(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* output, float intensity)
{
    auto td=output->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {}; UINT64 bytes=0;
    device->GetCopyableFootprints(&td,0,1,0,&fp,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC desc {}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes;
    desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp {};hp.Type=D3D12_HEAP_TYPE_READBACK;Ptr<ID3D12Resource> readback;
    Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
    Ptr<ID3D12CommandAllocator> allocator;Ptr<ID3D12GraphicsCommandList> list;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"copy allocator");
    Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"copy list");
    Effects::Barrier(list.Get(),output,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src {},dst {};src.pResource=output;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Effects::Barrier(list.Get(),output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Check(list->Close(),"copy close");ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);WaitQueue(device,queue);
    void* data=nullptr;D3D12_RANGE range {0,SIZE_T(bytes)};Check(readback->Map(0,&range,&data),"map");
    std::vector<unsigned char> expected(size_t(td.Width)*8);
    for(UINT y=0;y<td.Height;++y){FillRow(expected.data(),y,UINT(td.Width),td.Format);
        const auto* actual=reinterpret_cast<const UINT16*>(static_cast<unsigned char*>(data)+size_t(y)*fp.Footprint.RowPitch);
        const auto* base=reinterpret_cast<const UINT16*>(expected.data());
        for(UINT x=0;x<td.Width;++x)for(UINT c=0;c<4;++c){
            const float wanted=Half(base[x*4+c])*(c==3?1.f:1.f+intensity);
            Require(std::fabs(Half(actual[x*4+c])-wanted)<=(std::max)(.001f,std::fabs(wanted)*.001f),"numerical blend/alpha");
        }
    }
    D3D12_RANGE written {0,0};readback->Unmap(0,&written);
}
static bool NoLeases() { std::lock_guard lock(Submission::RecordingMutex()); return Effects::Global().leases.empty(); }
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
