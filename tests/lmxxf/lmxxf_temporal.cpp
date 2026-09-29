#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include <cstdio>
#include <limits>
#include <cmath>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/TemporalHistory.h"
using Microsoft::WRL::ComPtr;
using LmxxfTemporal::Check;
constexpr auto ReadState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static void Require(bool condition,const char *message) {if(!condition) throw std::runtime_error(message);}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info;
    HANDLE event{}; UINT64 value=0;
    Gpu()
    {
        ComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
        ComPtr<IDXGIAdapter> warp; Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP");
        Check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
        device.As(&info);
        D3D12_COMMAND_QUEUE_DESC q{}; Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
        Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
        Check(list->Close(),"initial close");
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(event!=nullptr,"event");
    }
    ~Gpu(){CloseHandle(event);}
    template<class F> void Run(F fn)
    {
        Check(allocator->Reset(),"allocator reset"); Check(list->Reset(allocator.Get(),nullptr),"list reset");
        fn(list.Get()); Check(list->Close(),"close");
        ID3D12CommandList *lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
        Check(queue->Signal(fence.Get(),++value),"signal");
        Check(fence->SetEventOnCompletion(value,event),"arm event");
        Require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0 && fence->GetCompletedValue()>=value,"GPU completion");
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 bytes,D3D12_HEAP_TYPE heap=D3D12_HEAP_TYPE_DEFAULT)
    {
        D3D12_HEAP_PROPERTIES hp{};hp.Type=heap;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;
        d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(heap==D3D12_HEAP_TYPE_DEFAULT)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        auto state=heap==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:heap==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:ReadState;
        ComPtr<ID3D12Resource> r;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"buffer");return r;
    }
    ComPtr<ID3D12Resource> Texture(UINT w,UINT h,DXGI_FORMAT format)
    {
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
        d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;
        ComPtr<ID3D12Resource> r;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,ReadState,nullptr,IID_PPV_ARGS(&r)),"texture");return r;
    }
    void Upload(ID3D12Resource *r,const std::vector<float> &data)
    {
        auto d=r->GetDesc(); const bool texture=d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        UINT64 bytes=data.size()*sizeof(float),rowBytes=bytes;UINT rows=1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        if(texture)device->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&rowBytes,&bytes);
        Require(data.size()*sizeof(float)>=rowBytes*rows,"upload source size");
        auto up=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);
        void *ptr=nullptr;Check(up->Map(0,nullptr,&ptr),"upload map");
        for(UINT y=0;y<rows;++y)std::memcpy(static_cast<char*>(ptr)+y*(texture?fp.Footprint.RowPitch:rowBytes),
                                         reinterpret_cast<const char*>(data.data())+y*rowBytes,size_t(rowBytes));
        up->Unmap(0,nullptr);
        Run([&](auto *cmd){
            LmxxfTemporal::Transition(cmd,r,ReadState,D3D12_RESOURCE_STATE_COPY_DEST);
            if(texture){D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=r;src.pResource=up.Get();
                src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}
            else cmd->CopyBufferRegion(r,0,up.Get(),0,bytes);
            LmxxfTemporal::Transition(cmd,r,D3D12_RESOURCE_STATE_COPY_DEST,ReadState);
        });
    }
    std::vector<float> Read(ID3D12Resource *r)
    {
        UINT64 bytes=r->GetDesc().Width;auto rb=Buffer(bytes,D3D12_HEAP_TYPE_READBACK);
        Run([&](auto *cmd){LmxxfTemporal::Transition(cmd,r,ReadState,D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(rb.Get(),0,r,0,bytes);LmxxfTemporal::Transition(cmd,r,D3D12_RESOURCE_STATE_COPY_SOURCE,ReadState);});
        std::vector<float> data(size_t(bytes/4));void *ptr=nullptr;Check(rb->Map(0,nullptr,&ptr),"read map");
        std::memcpy(data.data(),ptr,size_t(bytes));rb->Unmap(0,nullptr);return data;
    }
    void NoErrors()
    {
        if(!info)return;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);
            std::vector<char> buf(bytes);auto *m=reinterpret_cast<D3D12_MESSAGE*>(buf.data());info->GetMessage(i,m,&bytes);
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"D3D12: %s\n",m->pDescription);throw std::runtime_error("D3D12 validation error");}}
    }
};

int main()
try
{
    Gpu g; constexpr UINT w=8,h=8,n=w*h;
    auto motion=g.Texture(w,h,DXGI_FORMAT_R32G32_FLOAT),depth=g.Texture(w,h,DXGI_FORMAT_R32_FLOAT);
    auto raw=g.Buffer(n*16),output=g.Buffer(n*12);
    std::vector<float> pixels(n*4,.4f),model(n*3,.55f),vectors(n*2,0),depths(n,.5f);
    for(UINT i=0;i<n;++i){pixels[i*4+3]=1;for(UINT c=0;c<3;++c)model[i*3+c]+=.001f*float(i);}
    g.Upload(raw.Get(),pixels);g.Upload(output.Get(),model);g.Upload(motion.Get(),vectors);g.Upload(depth.Get(),depths);
    LmxxfTemporal::History history;history.Create(g.device.Get(),w,h,h);history.PrepareBinding(g.device.Get(),motion.Get(),depth.Get());
    LmxxfTemporal::Parameters p{};p.width=w;p.height=h;p.processingHeight=h;p.viewWidth=w;p.viewHeight=h;
    p.renderWidth=w;p.renderHeight=h;p.motionWidth=w;p.motionHeight=h;p.scaleX=1.f/w;p.scaleY=1.f/h;
    const auto inputs=[&]{g.Run([&](auto *cmd){history.RecordInputs(cmd,raw.Get(),output.Get(),motion.Get(),depth.Get(),ReadState,ReadState,p);});};
    const auto finish=[&]{g.Run([&](auto *cmd){history.RecordOutputs(cmd,raw.Get(),output.Get(),depth.Get(),ReadState,p);});};
    const auto rejected=[&](const char *reason){auto v=g.Read(history.Warped());for(UINT i=0;i<n;++i)
        Require(v[i*4+3]==0 && std::abs(v[i*4]-.4f)<1e-6f,reason);};
    inputs();rejected("first frame must use current input");finish();
    auto unchanged=g.Read(output.Get());Require(unchanged==model,"smoothing off must be bit-exact");
    p.useHistory=1;inputs();auto v=g.Read(history.Warped());
    for(UINT i=0;i<n;++i)Require(v[i*4+3]==1 && std::abs(v[i*4]-model[i*3])<1e-6f,"identity reprojection");
    // Known one-pixel translation, then a jitter-only shift with the same result.
    for(UINT i=0;i<n;++i)vectors[i*2]=1;
    g.Upload(motion.Get(),vectors);inputs();v=g.Read(history.Warped());
    Require(std::abs(v[3*4]-model[4*3])<1e-6f && v[7*4+3]==0,"motion pixels and out-of-bounds rejection");
    std::fill(vectors.begin(),vectors.end(),0);g.Upload(motion.Get(),vectors);p.jitterX=1.f/w;
    inputs();v=g.Read(history.Warped());Require(std::abs(v[3*4]-model[4*3])<1e-6f,"jitter coordinate adjustment");p.jitterX=0;
    // Render vectors and display vectors encode the same displacement in UV.
    auto displayMotion=g.Texture(w*2,h*2,DXGI_FORMAT_R32G32_FLOAT);std::vector<float> displayVectors(n*8,0);
    for(UINT i=0;i<n*4;++i)displayVectors[i*2]=2;
    g.Upload(displayMotion.Get(),displayVectors);history.PrepareBinding(g.device.Get(),displayMotion.Get(),depth.Get());
    p.motionWidth=w*2;p.motionHeight=h*2;p.scaleX=1.f/(w*2);p.scaleY=1.f/(h*2);
    g.Run([&](auto *cmd){history.RecordInputs(cmd,raw.Get(),output.Get(),displayMotion.Get(),depth.Get(),ReadState,ReadState,p);});
    v=g.Read(history.Warped());Require(std::abs(v[3*4]-model[4*3])<1e-6f,"display-grid motion normalization");
    history.PrepareBinding(g.device.Get(),motion.Get(),depth.Get());p.motionWidth=w;p.motionHeight=h;p.scaleX=1.f/w;p.scaleY=1.f/h;
    // Half-pixel jitter: interpolation is expected, not nearest-frame hold.
    p.jitterX=.5f/w;inputs();v=g.Read(history.Warped());
    Require(std::abs(v[3*4]-(model[3*3]+model[4*3])*.5f)<1e-6f,"fractional reprojection");p.jitterX=0;
    std::fill(depths.begin(),depths.end(),.9f);g.Upload(depth.Get(),depths);inputs();rejected("disocclusion depth guard");
    std::fill(depths.begin(),depths.end(),.5f);g.Upload(depth.Get(),depths);
    for(UINT i=0;i<n;++i)vectors[i*2]=std::numeric_limits<float>::quiet_NaN();
    g.Upload(motion.Get(),vectors);inputs();rejected("NaN motion rejection");
    std::fill(vectors.begin(),vectors.end(),0);g.Upload(motion.Get(),vectors);
    // Change the raw input, independently of the model output: reject history.
    for(UINT i=0;i<n;++i)for(UINT c=0;c<3;++c)pixels[i*4+c]=.1f;
    g.Upload(raw.Get(),pixels);inputs();v=g.Read(history.Warped());Require(v[3]==0 && std::abs(v[0]-.1f)<1e-6f,"raw disagreement");
    for(UINT i=0;i<n;++i)for(UINT c=0;c<3;++c)pixels[i*4+c]=.4f;
    g.Upload(raw.Get(),pixels);inputs();
    std::vector<float> next=model;for(auto &f:next)f+=.01f;g.Upload(output.Get(),next);p.smoothStrength=.5f;
    finish();auto smoothed=g.Read(output.Get());const float weight=.5f*(1-.01f/(8.f/255.f));
    Require(std::abs(smoothed[0]-(next[0]+weight*(model[0]-next[0])))<2e-6f,"bounded output smoothing");
    // Black history must not become a self-sustaining dark trail.
    p.smoothStrength=0;g.Upload(output.Get(),std::vector<float>(n*3,.001f));finish();inputs();rejected("black history guard");
    // Invalid network output is not replaced by a previous bright frame.
    g.Upload(output.Get(),model);finish();inputs();p.smoothStrength=.5f;
    g.Upload(output.Get(),std::vector<float>(n*3,0));finish();auto zero=g.Read(output.Get());
    for(auto f:zero)Require(f==0,"zero fallback stays zero");inputs();rejected("zero history invalid");
    p.useHistory=0;inputs();rejected("reset ignores previous frame");
    // A fitted viewport has padding; never sample history from that padding.
    p.viewX=2;p.viewWidth=4;g.Upload(output.Get(),model);finish();p.useHistory=1;inputs();v=g.Read(history.Warped());
    Require(v[3]==0 && v[2*4+3]==1 && v[6*4+3]==0,"fitted viewport padding");
    g.NoErrors();std::puts("temporal WARP: PASS (identity, motion, display grid, jitter, depth/raw/black guards, smoothing, zero recovery, reset, fit viewport)");return 0;
}
catch(const std::exception &e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
