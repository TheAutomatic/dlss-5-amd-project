#define WIN32_LEAN_AND_MEAN
#define LMXXF_NR_FLICKER_TEST
#define LMXXF_NR_FLICKER_TEST18
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include "third_party/lmxxf/src/native_game_codec.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/FlickerCapture19.h"
using Microsoft::WRL::ComPtr;
static void Require(bool v,const char*s){if(!v){fprintf(stderr,"FAIL: %s\n",s);exit(1);}}
static void Check(HRESULT h,const char*s){if(FAILED(h)){fprintf(stderr,"FAIL: %s %08lx\n",s,(unsigned long)h);exit(1);}}
static void Transition(ID3D12GraphicsCommandList*c,ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&x);}
static float Half(UINT16 h){unsigned e=(h>>10)&31,m=h&1023;return (h&32768?-1.f:1.f)*(e?std::ldexp(1.f+m/1024.f,int(e)-15):std::ldexp(float(m),-24));}
int main(int argc,char**argv){
    ComPtr<ID3D12Debug>debug;bool debugging=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));if(debugging)debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4>factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");ComPtr<IDXGIAdapter1>adapter;
    bool hardware=argc>1&&strcmp(argv[1],"hardware")==0;
    if(hardware){for(UINT n=0;;n++){Check(factory->EnumAdapters1(n,&adapter),"adapter");DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);if(desc.VendorId==0x1002&&!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))break;adapter.Reset();}}
    else Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"warp");
    ComPtr<ID3D12Device>d;Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)),"device");ComPtr<ID3D12InfoQueue>info;d.As(&info);
    ComPtr<ID3D12CommandQueue>q;D3D12_COMMAND_QUEUE_DESC qd{};Check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)),"queue");
    ComPtr<ID3D12CommandAllocator>a;Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"allocator");
    ComPtr<ID3D12GraphicsCommandList>c;Check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&c)),"list");c->Close();
    ComPtr<ID3D12Fence>fence;Check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");UINT64 serial=0;HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    auto reset=[&](){Check(a->Reset(),"reset allocator");Check(c->Reset(a.Get(),nullptr),"reset list");};
    auto wait=[&](){Check(q->Signal(fence.Get(),++serial),"signal");Check(fence->SetEventOnCompletion(serial,event),"event");Require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"wait");};
    auto execute=[&](){Check(c->Close(),"close");ID3D12CommandList*lists[]={c.Get()};q->ExecuteCommandLists(1,lists);wait();};
    constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto texture=[&](DXGI_FORMAT format,UINT w,UINT h,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE){
        ComPtr<ID3D12Resource>r;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=w;rd.Height=h;rd.DepthOrArraySize=rd.MipLevels=1;rd.Format=format;rd.SampleDesc.Count=1;rd.Flags=flags;
        Check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,state,nullptr,IID_PPV_ARGS(&r)),"texture");return r;
    };
    auto buffer=[&](UINT64 size,D3D12_HEAP_TYPE type){ComPtr<ID3D12Resource>r;D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=size;rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;Check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,type==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&r)),"buffer");return r;};
    auto original=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,1920,1080),neural=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,1920,1080);
    auto upload=buffer(1920ull*1080*8,D3D12_HEAP_TYPE_UPLOAD);unsigned char*p=nullptr;Check(upload->Map(0,nullptr,reinterpret_cast<void**>(&p)),"upload map");
    UINT16 levels[]={0,0x3000,0x3800,0x3a00,0x3c00,0x4000,0x4600,0x4c00};
    for(UINT y=0;y<1080;y++)for(UINT x=0;x<1920;x++){UINT16 rgba[]={levels[x/240],levels[x/240],levels[x/240],0x3c00};memcpy(p+(y*1920+x)*8,rgba,8);}upload->Unmap(0,nullptr);
    reset();Transition(c.Get(),original.Get(),read,D3D12_RESOURCE_STATE_COPY_DEST);D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,1920,1080,1,1920*8};dst.pResource=original.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;c->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Transition(c.Get(),original.Get(),D3D12_RESOURCE_STATE_COPY_DEST,read);execute();
    NativeGameCodec encode,decode;encode.Create(d.Get(),{original.Get()},L"third_party\\lmxxf\\shaders");decode.Create(d.Get(),{encode.Output(),neural.Get(),original.Get()},L"third_party\\lmxxf\\shaders");
    auto readback=buffer(1920ull*1080*8,D3D12_HEAP_TYPE_READBACK);
    auto values=[&](ID3D12Resource*r){reset();Transition(c.Get(),r,read,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION s{},t{};s.pResource=r;s.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;t.pResource=readback.Get();t.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;t.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,1920,1080,1,1920*8};c->CopyTextureRegion(&t,0,0,0,&s,nullptr);Transition(c.Get(),r,D3D12_RESOURCE_STATE_COPY_SOURCE,read);execute();unsigned char*b=nullptr;Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&b)),"read map");std::vector<float>v;for(UINT x=120;x<1920;x+=240){UINT16 h;memcpy(&h,b+(500*1920+x)*8,2);v.push_back(Half(h));}D3D12_RANGE none{0,0};readback->Unmap(0,&none);return v;};
    std::vector<float> baseline;
    for(UINT mode=0;mode<3;mode++){
        NativeCodecParameters par;par.test_flags=mode==1?0x20000:mode==2?0x40000:0;
        reset();encode.Record(c.Get(),{read},1,par);Transition(c.Get(),encode.Output(),read,D3D12_RESOURCE_STATE_COPY_SOURCE);Transition(c.Get(),neural.Get(),read,D3D12_RESOURCE_STATE_COPY_DEST);c->CopyResource(neural.Get(),encode.Output());Transition(c.Get(),encode.Output(),D3D12_RESOURCE_STATE_COPY_SOURCE,read);Transition(c.Get(),neural.Get(),D3D12_RESOURCE_STATE_COPY_DEST,read);decode.Record(c.Get(),{read,read,read},1,par);execute();
        auto e=values(encode.Output()),out=values(decode.Output());
        for(UINT i=0;i<8;i++){Require(std::isfinite(e[i])&&e[i]>=0&&e[i]<=1,"working domain");Require(std::abs(out[i]-Half(levels[i]))<.016,"equal proxy/neural preserves original");if(mode==2)Require(out[i]==Half(levels[i]),"identity exact original");}
        if(mode==0)baseline=e;
        if(mode==1){for(UINT i=0;i<4;i++)Require(e[i]==baseline[i],"candidate preserves toe");Require(e[5]<e[6]&&e[6]<e[7],"candidate keeps highlight separation");Require(baseline[5]==baseline[6],"baseline quantized highlights");}
    }
    auto motion=texture(DXGI_FORMAT_R16G16_FLOAT,1920,1080);
    Check(upload->Map(0,nullptr,reinterpret_cast<void**>(&p)),"motion map");for(UINT i=0;i<1920*1080;i++){UINT16 m[]={0x3400,0xb800};memcpy(p+i*4,m,4);}upload->Unmap(0,nullptr);
    reset();Transition(c.Get(),motion.Get(),read,D3D12_RESOURCE_STATE_COPY_DEST);src.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16_FLOAT,1920,1080,1,1920*4};dst.pResource=motion.Get();c->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Transition(c.Get(),motion.Get(),D3D12_RESOURCE_STATE_COPY_DEST,read);execute();
    auto depth=texture(DXGI_FORMAT_R32G8X24_TYPELESS,1920,1080,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ComPtr<ID3D12DescriptorHeap>dsvHeap;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};Check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsvHeap)),"DSV heap");D3D12_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;dv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;d->CreateDepthStencilView(depth.Get(),&dv,dsvHeap->GetCPUDescriptorHandleForHeapStart());reset();c->ClearDepthStencilView(dsvHeap->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,.75f,0,0,nullptr);Transition(c.Get(),depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,read);execute();
    auto exposure=texture(DXGI_FORMAT_R32_FLOAT,1,1);
    auto typeless=texture(DXGI_FORMAT_R16G16B16A16_TYPELESS,1920,1080);
    reset();Transition(c.Get(),original.Get(),read,D3D12_RESOURCE_STATE_COPY_SOURCE);Transition(c.Get(),typeless.Get(),read,D3D12_RESOURCE_STATE_COPY_DEST);
    c->CopyResource(typeless.Get(),original.Get());Transition(c.Get(),original.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,read);Transition(c.Get(),typeless.Get(),D3D12_RESOURCE_STATE_COPY_DEST,read);execute();
    HighlightDiagnostics::Capture capture(2000,0,32);HighlightDiagnostics::FrameInfo fi{};fi.renderW=fi.motionW=fi.fitW=1920;fi.renderH=fi.motionH=fi.fitH=1080;capture.SetFrameInfo(fi);
    capture.Configure(L"third_party\\lmxxf\\shaders",L"third_party\\lmxxf\\modules",L"");
    const bool flipped=argc>2&&strcmp(argv[2],"flipped")==0;
    Require(NrCaptureSourceY(.25f,true)==.75f&&NrCaptureSourceY(.25f,false)==.25f,"display/source orientation");
    HighlightDiagnostics::Record meta{};meta.pre=meta.scale=meta.paper=meta.transfer=meta.color=1;meta.exposureSource=2;
    for(UINT run=0;run<3;run++){
        Require(capture.SelectScreenPoint(.7f,.2f),"select off-center target while idle");
        const UINT expectedX=1312,expectedY=flipped?832:184;
        NativeTypelessRgba16AsFloat()=run!=2;
        UINT observed=run==2?DXGI_FORMAT_R16G16B16A16_UNORM:DXGI_FORMAT_R16G16B16A16_FLOAT;
        typeless->SetPrivateData(NrObservedRtvFormatGuid,sizeof(observed),&observed);
        Require(capture.TestFlags(true)==0 && capture.TestFlags(false)==0,"no UI or mode flags enter codec");
        for(UINT frame=0;frame<22;frame++){
            reset();capture.Begin(d.Get(),meta,frame==0,1000+3000*run+100*frame);
            if(frame<20)Require(!capture.SelectScreenPoint(.1f,.9f),"target locked during capture");
            capture.Guides(c.Get(),d.Get(),motion.Get(),depth.Get(),read,read);
            for(UINT stage=0;stage<4;stage++)capture.Copy(c.Get(),stage==0&&run?typeless.Get():original.Get(),read,stage,0,0,1920,1080);
            capture.Copy(c.Get(),exposure.Get(),read,4,0,0,1,1);capture.OutputsRecorded();
            if(false){capture.Cancel();Check(c->Close(),"discard close");continue;}
            execute();capture.Submitted(q.Get());wait();capture.Poll();
        }
        for(UINT i=0;i<1000&&capture.Status().find(":writing")!=std::string::npos;i++){Sleep(1);capture.Poll();}
        Require(capture.Status().find(":saved")!=std::string::npos,"repeat capture saved");
        FILE*f=_wfopen(capture.Path(),L"rb");Require(f!=nullptr,"open capture");char magic[8];Require(fread(magic,1,8,f)==8&&!memcmp(magic,"NRHLV2\0\0",8),"v2 magic");
        HighlightDiagnostics::Record r{};UINT guides=0,originalRecords=0;uint64_t first=0,last=0;
        while(fread(&r,sizeof(r),1,f)==1){UINT bpp=r.format==2?16:r.format==41?4:8;std::vector<unsigned char>b(r.w*r.h*bpp);Require(fread(b.data(),1,b.size(),f)==b.size(),"complete payload");if(r.stage==0){Require(r.format==(run==2?11u:10u),"typeless capture follows codec view");++originalRecords;}
        if((r.stage<4&&r.tile==0)||r.stage==5)Require(r.x==expectedX&&r.y==expectedY,"selected target agrees across color/guide stages");
        if(r.stage==5){if(!first)first=r.tick;last=r.tick;float g[4];memcpy(g,b.data(),16);Require(g[0]==.25f&&g[1]==-.5f&&g[2]==.75f&&g[3]==1,"real typed depth/motion bytes");++guides;}}
        fclose(f);Require(guides==20u && originalRecords==100u,"all stages across full duration");
        Require(last-first==1900,"ROI extends through last tenth of recording");
        {std::lock_guard<std::mutex>lock(NrDiagnostic19::mutex);const auto&o=NrDiagnostic19::snapshot;Require(o.state==3&&o.edge==64&&o.pixels==20&&o.missingMask==0,"host snapshot reflects complete capture");}
        std::wstring path=capture.Path();
        FILE*ff=_wfopen((path+L".full").c_str(),L"rb");Require(ff!=nullptr,"full frame file");
        Require(fread(magic,1,8,ff)==8&&!memcmp(magic,"NRFFV1\0\0",8),"full magic");
        UINT fullRecords=0;uint64_t firstFull=0,lastFull=0;
        while(fread(&r,sizeof(r),1,ff)==1){if(!firstFull)firstFull=r.tick;lastFull=r.tick;Require(r.w==1920&&r.h==1080&&(r.stage==1||r.stage==2),"full geometry");std::vector<UINT16>v(size_t(r.w)*r.h*4);Require(fread(v.data(),2,v.size(),ff)==v.size(),"full payload");Require(v[0]==0&&v[3]==0x3c00&&v[(1920-1)*4]==0x4c00,"full pixels exact");++fullRecords;}
        fclose(ff);Require(lastFull-firstFull>=1500,"full pairs include late capture");Require(fullRecords==16,"eight complete input/output pairs");
        DeleteFileW((path+L".full").c_str());DeleteFileW((path+L".reuse.csv").c_str());
        DeleteFileW(path.c_str());DeleteFileW((path+L".csv").c_str());DeleteFileW((path+L".info.txt").c_str());
    }
    Require(capture.SelectScreenPoint(-1.f,2.f),"clamp edge selection");
    Require(capture.RoiX(1920)==32&&capture.RoiY(1080)==(flipped?32u:1048u),"whole ROI remains in bounds");
    capture.ReleaseAfterGpuIdle();
    if(info){for(UINT64 i=0;i<info->GetNumStoredMessages();i++){SIZE_T n=0;info->GetMessage(i,nullptr,&n);std::vector<char>b(n);auto*m=reinterpret_cast<D3D12_MESSAGE*>(b.data());info->GetMessage(i,m,&n);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){fprintf(stderr,"D3D12: %s\n",m->pDescription);Require(false,"debug layer clean");}}}
    CloseHandle(event);printf("PASS: test19 %s codec modes, bounded highlights, original preservation, typed guides, repeat capture, cancellation; debug=%u\n",hardware?"hardware":"WARP",debugging);return 0;
}
