#pragma once
// Explicit test17 build only. No additional ExecuteCommandLists or GPU wait.
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <vector>
#include <string>
#include <thread>
#include <memory>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "TemporalHistory.h"

namespace HighlightDiagnostics {
#pragma pack(push, 1)
struct Record {
    uint32_t frame, stage, tile, width, height, x, y, w, h, format, exposureSource, debugView;
    float pre, scale, paper, transfer, color;
    uint64_t tick;
};
#pragma pack(pop)
static_assert(sizeof(Record)==76, "NRHL record");
inline std::string Utf8(const std::wstring &s) {
    if(s.empty())return {};
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);
    std::string out(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),out.data(),n,nullptr,nullptr);return out;
}
inline std::string FileHash(const std::wstring &path) {
    FILE *f=_wfopen(path.c_str(),L"rb");if(!f)return "unavailable";
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;DWORD size=0,got=0;
    std::string answer="unavailable";std::vector<unsigned char> object;unsigned char digest[32],block[65536];
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0 &&
       BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),4,&got,0)>=0){
        object.resize(size);
        if(BCryptCreateHash(alg,&hash,object.data(),size,nullptr,0,0)>=0){
            bool ok=true;size_t n;
            while((n=fread(block,1,sizeof(block),f)))if(BCryptHashData(hash,block,ULONG(n),0)<0){ok=false;break;}
            if(ok&&!ferror(f)&&BCryptFinishHash(hash,digest,32,0)>=0){
                static const char hex[]="0123456789abcdef";answer.clear();
                for(auto b:digest){answer+=hex[b>>4];answer+=hex[b&15];}
            }
        }
    }
    if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);fclose(f);return answer;
}
struct FrameInfo {
    uint64_t evaluate=0;
    UINT renderW=0,renderH=0,motionW=0,motionH=0,fitX=0,fitY=0,fitW=0,fitH=0,temporalFlags=0;
    float jitterX=0,jitterY=0,scaleX=0,scaleY=0,hostTransfer=1;
    bool allowed=true;
};
class Capture {
    static constexpr UINT kSlots=8,kEdge=128,kReferences=4,kMaxPixels=128;
    // Four stages: 128 square + four 32 squares; then exposure and float4 guides.
    static constexpr UINT kStageBytes=128*1024+4*8192;
    static constexpr UINT kExposureOffset=4*kStageBytes;
    static constexpr UINT kGuideOffset=kExposureOffset+512;
    static constexpr UINT kBytes=kGuideOffset+128*2048;
    struct Tile {Record r{};UINT offset=0,pitch=0,bpp=0;};
    struct Slot {
        ID3D12Resource *buffer=nullptr,*guide=nullptr;
        ID3D12DescriptorHeap *heap=nullptr;
        std::vector<Tile> tiles;
        uint64_t fenceValue=0;
        bool output=false,poisoned=false,pixels=false;
        UINT mask=0;
        Record metadata{};
        FrameInfo info{};
    };
    std::array<Slot,kSlots> slots{};
    ID3D12Fence *fence=nullptr;
    ID3D12RootSignature *guideRoot=nullptr;
    ID3D12PipelineState *guidePso=nullptr;
    uint64_t nextFence=0,start=0,now=0;
    UINT sequence=0,burst=0,captureId=0,mode=0,captureMode=0,dropped=0,cancelled=0,readFrames=0,pixelFrames=0,guideFrames=0;
    UINT duration=20000,burstStart=5000,burstLimit=128;
    int current=-1;
    bool running=false,sealed=false,previousKey=false,guideFailed=false,eligible=true,initialized=false,controlsVisible=false;
    bool keys[5]{};
    float centerX=.5f,centerY=.5f;
    Record meta{};FrameInfo frameInfo{};
    std::vector<unsigned char> bytes;
    std::ostringstream csv;
    std::wstring directory,path;
    std::string identity;
    std::shared_ptr<std::atomic<int>> saved=std::make_shared<std::atomic<int>>(0);
    std::thread writer;
    mutable std::mutex statusMutex;
    std::string status=" test17=original F6=original F7=soft F8=identity F9=capture F10=pick-ROI";
    template<class T> static void Drop(T *&p){if(p)p->Release();p=nullptr;}
    static void Barrier(ID3D12GraphicsCommandList *c,ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        if(a==b)return;D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,0,a,b};c->ResourceBarrier(1,&v);
    }
    static UINT Origin(float center,UINT extent,UINT edge){return UINT(std::clamp(int(std::lround(center*extent))-int(edge/2),0,int(extent-edge)));}
    void Publish(){
        const char *names[]={"original","soft","identity"};
        std::ostringstream s;s<<" test17="<<names[mode]<<" capture="<<captureId<<":";
        const int result=saved->load();
        s<<(running?"capturing":result<0?"failed":result>0?"saved":sealed?"writing":"ready");
        s<<" roi="<<centerX<<","<<centerY<<" seconds="<<(running?(now-start)/1000:0)
         <<" pixelFrames="<<pixelFrames<<" guides="<<guideFrames<<" dropped="<<dropped<<" cancelled="<<cancelled;
        if(!eligible)s<<" NEED-HISTORY-OFF-SMOOTHING-0-DEBUG-OFF";
        if(!path.empty())s<<" file="<<Utf8(path);
        std::lock_guard<std::mutex> lock(statusMutex);status=s.str();
    }
    bool Ensure(ID3D12Device *d){
        if(fence)return initialized;
        if(FAILED(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))return false;
        for(auto &s:slots){
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=kBytes;rd.Height=1;
            rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if(FAILED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.buffer))))return false;
            s.tiles.reserve(22);
        }initialized=true;return true;
    }
    bool EnsureGuides(ID3D12Device *d){
        if(guideFailed)return false;if(guidePso)return true;guideFailed=true;
        static const char shader[]=R"(
Texture2D<float2> Motion:register(t0); Texture2D<float> Depth:register(t1);
RWTexture2D<float4> Result:register(u0);
cbuffer C:register(b0){uint2 origin;uint2 extent;uint2 renderSize;uint2 motionSize;};
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=extent))return;
 uint2 p=origin+id.xy;
 uint2 m=min(uint2((float2(p)+.5)*float2(motionSize)/float2(renderSize)),motionSize-1);
 Result[id.xy]=float4(Motion.Load(int3(m,0)),Depth.Load(int3(p,0)),1);
})";
        D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
        D3D12_ROOT_PARAMETER params[3]{};
        for(UINT i=0;i<2;i++){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]};}
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,8};
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=3;desc.pParameters=params;
        ID3DBlob *blob=nullptr,*errors=nullptr;
        HRESULT hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);Drop(errors);
        if(FAILED(hr))return false;hr=d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&guideRoot));Drop(blob);if(FAILED(hr))return false;
        hr=D3DCompile(shader,sizeof(shader)-1,"test17-guides",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);Drop(errors);if(FAILED(hr))return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=guideRoot;pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
        hr=d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&guidePso));Drop(blob);if(FAILED(hr))return false;
        for(auto &s:slots){
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=rd.Height=128;rd.DepthOrArraySize=rd.MipLevels=1;
            rd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;rd.SampleDesc.Count=1;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if(FAILED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&s.guide))))return false;
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            if(FAILED(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.heap))))return false;
        }guideFailed=false;return true;
    }
    bool Pending()const{for(const auto&s:slots)if(s.fenceValue)return true;return false;}
    void Seal(){
        if(sealed||!running||Pending()||current>=0)return;
        running=false;sealed=true;
        std::ostringstream report;report<<identity<<"\ncapture="<<captureId<<"\nmode="<<captureMode<<"\nstart_tick="<<start
          <<"\nend_tick="<<now<<"\nread_frames="<<readFrames<<"\npixel_frames="<<pixelFrames<<"\nguide_frames="<<guideFrames
          <<"\ndropped_frames="<<dropped<<"\ncancelled_frames="<<cancelled<<"\ncomplete="<<(pixelFrames==burstLimit&&guideFrames==pixelFrames&&dropped==0&&cancelled==0?1:0)<<"\n";
        auto state=saved;std::wstring destination=path;
        writer=std::thread([data=std::move(bytes),timeline=csv.str(),info=report.str(),destination,state](){
            auto write=[](const std::wstring&p,const void*data,size_t n){FILE*f=_wfopen(p.c_str(),L"wb");if(!f)return false;bool ok=fwrite(data,1,n,f)==n;bool closed=fclose(f)==0;return ok&&closed;};
            bool ok=write(destination,data.data(),data.size());ok=write(destination+L".csv",timeline.data(),timeline.size())&&ok;
            ok=write(destination+L".info.txt",info.data(),info.size())&&ok;state->store(ok?1:-1);
        });Publish();
    }
  public:
    explicit Capture(UINT ms=20000,UINT burstMs=5000,UINT frames=128):duration(ms),burstStart(burstMs),burstLimit((std::min)(frames,kMaxPixels)){}
    ~Capture(){if(writer.joinable())writer.join();}
    Capture(const Capture&)=delete;Capture&operator=(const Capture&)=delete;
    void Configure(const std::wstring&shaders,const std::wstring&modules,const std::wstring&weights){
        if(!identity.empty())return;wchar_t tmp[MAX_PATH]{};GetTempPathW(MAX_PATH,tmp);
        directory=std::wstring(tmp)+L"Lmxxf-WuWa-test17";CreateDirectoryW(directory.c_str(),nullptr);
        HMODULE module=nullptr;wchar_t dll[MAX_PATH]{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&FileHash),&module);
        GetModuleFileNameW(module,dll,MAX_PATH);
        SYSTEMTIME utc{};GetSystemTime(&utc);
        std::ostringstream s;s<<"build=WuWa-test17\ncodec_candidate=rational-shoulder-C1\n";
        s<<"runtime="<<Utf8(dll)<<"\nruntime_sha256="<<FileHash(dll)<<"\nshader_dir="<<Utf8(shaders)<<"\nmodules="<<Utf8(modules)<<"\nweights_dir="<<Utf8(weights)<<"\n";
        for(const auto*name:{L"native_codec_encode.hlsl",L"native_codec_decode.hlsl"})s<<Utf8(name)<<"_sha256="<<FileHash(shaders+L"\\"+name)<<"\n";
        s<<"utc="<<utc.wYear<<"-"<<utc.wMonth<<"-"<<utc.wDay<<"T"<<utc.wHour<<":"<<utc.wMinute<<":"<<utc.wSecond<<"Z\nutc_tick="<<GetTickCount64()<<"\n";identity=s.str();
    }
    void SetFrameInfo(const FrameInfo&i){frameInfo=i;eligible=i.allowed;}
    void UpdateControls(){
        DWORD pid=0;HWND window=GetForegroundWindow();GetWindowThreadProcessId(window,&pid);
        if(pid!=GetCurrentProcessId()){std::fill(std::begin(keys),std::end(keys),false);return;}
        const int code[5]={VK_F6,VK_F7,VK_F8,VK_F10,VK_F9};
        for(int i=0;i<5;i++){
            bool down=(GetAsyncKeyState(code[i])&0x8000)!=0,pressed=down&&!keys[i];keys[i]=down;
            if(pressed)controlsVisible=true;
            if(pressed&&!running&&saved->load()!=0&&writer.joinable())writer.join();
            if(pressed&&!running&&(!sealed||saved->load()!=0)&&i<3)mode=UINT(i);
            if(pressed&&!running&&i==3){POINT p{};RECT r{};if(GetCursorPos(&p)&&ScreenToClient(window,&p)&&GetClientRect(window,&r)&&r.right>0&&r.bottom>0){centerX=std::clamp(float(p.x)/r.right,0.f,1.f);centerY=std::clamp(float(p.y)/r.bottom,0.f,1.f);}}
        }Publish();
    }
    UINT Mode()const{return mode;}
    bool SelectMode(UINT requested){if(requested>2||running||Pending())return false;mode=requested;Publish();return true;}
    bool CaptureKey()const{return keys[4];}
    UINT TestFlags(bool display)const{
        if(!eligible)return 0;
        UINT flags=mode==1?0x20000u:mode==2?0x40000u:0;
        if(display&&controlsVisible)flags|=0x80000u|(mode<<20)|(UINT(running?(std::min)(uint64_t(20),(now-start)/1000):0)<<24);
        return flags;
    }
    UINT RoiX(UINT extent)const{UINT edge=(std::min)(128u,extent);return Origin(centerX,extent,edge)+edge/2;}
    UINT RoiY(UINT extent)const{UINT edge=(std::min)(128u,extent);return Origin(centerY,extent,edge)+edge/2;}
    const wchar_t*Path()const{return path.c_str();}
    std::string Status()const{std::lock_guard<std::mutex> lock(statusMutex);return status;}
    void Begin(ID3D12Device*d,Record metadata,bool keyDown,uint64_t clock=0){
        Poll();now=clock?clock:GetTickCount64();++sequence;
        bool pressed=keyDown&&!previousKey;previousKey=keyDown;
        if(pressed&&!running&&eligible&&(!sealed||saved->load()!=0)){
            if(writer.joinable())writer.join();
            if(!Ensure(d)){saved->store(-1);Publish();return;}
            if(directory.empty()){wchar_t tmp[MAX_PATH]{};GetTempPathW(MAX_PATH,tmp);directory=std::wstring(tmp)+L"Lmxxf-WuWa-test17";CreateDirectoryW(directory.c_str(),nullptr);}
            ++captureId;captureMode=mode;start=now;burst=dropped=cancelled=readFrames=pixelFrames=guideFrames=0;
            wchar_t name[128]{};swprintf_s(name,L"\\capture-%lu-%llu-%u-mode%u.nrhl",GetCurrentProcessId(),GetTickCount64(),captureId,mode);path=directory+name;
            bytes={'N','R','H','L','V','2',0,0};bytes.reserve(size_t(burstLimit)*kBytes+4*1024*1024);
            csv.str("");csv.clear();csv<<"frame,tick,evaluate,mode,mask,render_w,render_h,motion_w,motion_h,fit_x,fit_y,fit_w,fit_h,jitter_x,jitter_y,mv_scale_x,mv_scale_y,temporal_flags,host_transfer,roi_x,roi_y\n";
            saved->store(0);running=true;sealed=false;EnsureGuides(d);
        }
        if(!running){Publish();return;}
        if(!eligible||now-start>=duration||bytes.size()>160ull*1024*1024){Seal();Publish();return;}
        current=-1;
        for(UINT i=0;i<kSlots;i++)if(!slots[i].fenceValue&&!slots[i].poisoned){current=int(i);break;}
        if(current<0){++dropped;Publish();return;}
        auto &s=slots[current];s.tiles.clear();s.output=false;s.mask=0;s.info=frameInfo;
        s.pixels=now-start>=burstStart&&burst<burstLimit;if(s.pixels)++burst;
        meta=metadata;meta.frame=sequence;meta.tick=now;s.metadata=meta;
        Publish();
    }
    void Copy(ID3D12GraphicsCommandList*c,ID3D12Resource*r,D3D12_RESOURCE_STATES state,UINT stage,UINT x,UINT y,UINT w,UINT h){
        if(current<0||!r||stage>4)return;auto&s=slots[current];if(stage<4&&!s.pixels)return;
        auto d=r->GetDesc();bool exposure=stage==4;
        UINT bpp=d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:exposure&&d.Format==DXGI_FORMAT_R32_FLOAT?4:exposure&&d.Format==DXGI_FORMAT_R16_FLOAT?2:0;
        if(!bpp||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.SampleDesc.Count!=1||d.DepthOrArraySize!=1||d.MipLevels!=1||!w||!h||uint64_t(x)+w>d.Width||uint64_t(y)+h>d.Height)return;
        Barrier(c,r,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        for(UINT tile=0;tile<(exposure?1u:5u);tile++){
            Tile t{};t.r=meta;t.r.stage=stage;t.r.tile=tile;t.r.width=UINT(d.Width);t.r.height=d.Height;t.r.format=d.Format;
            UINT edge=exposure?1:tile?32:128;t.r.w=(std::min)(edge,w);t.r.h=(std::min)(edge,h);
            const float cx=tile?(tile==1||tile==3?.25f:.75f):centerX,cy=tile?(tile<3?.25f:.75f):centerY;
            t.r.x=x+Origin(cx,w,t.r.w);t.r.y=y+Origin(cy,h,t.r.h);t.bpp=bpp;
            t.offset=exposure?kExposureOffset:stage*kStageBytes+(tile?128*1024+(tile-1)*8192:0);
            t.pitch=(t.r.w*bpp+255)&~255u;
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=r;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource=s.buffer;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Offset=t.offset;dst.PlacedFootprint.Footprint={d.Format,t.r.w,t.r.h,1,t.pitch};
            D3D12_BOX box{t.r.x,t.r.y,0,t.r.x+t.r.w,t.r.y+t.r.h,1};c->CopyTextureRegion(&dst,0,0,0,&src,&box);s.tiles.push_back(t);
        }s.mask|=1u<<stage;Barrier(c,r,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
    }
    void Guides(ID3D12GraphicsCommandList*c,ID3D12Device*d,ID3D12Resource*motion,ID3D12Resource*depth,D3D12_RESOURCE_STATES ms,D3D12_RESOURCE_STATES ds){
        if(current<0||!slots[current].pixels||!motion||!depth||guideFailed||!guidePso)return;
        auto&s=slots[current];auto md=motion->GetDesc(),dd=depth->GetDesc();const auto&i=s.info;
        DXGI_FORMAT mf=LmxxfTemporal::MotionFormat(md.Format),df=LmxxfTemporal::DepthFormat(dd.Format);
        if(!s.heap||!s.guide||LmxxfTemporal::TextureIssue(md)||LmxxfTemporal::TextureIssue(dd)||mf==DXGI_FORMAT_UNKNOWN||df==DXGI_FORMAT_UNKNOWN||!i.renderW||!i.renderH||!i.motionW||!i.motionH||i.renderW>dd.Width||i.renderH>dd.Height||i.motionW>md.Width||i.motionH>md.Height)return;
        UINT stride=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);auto cpu=s.heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;
        sv.Format=mf;d->CreateShaderResourceView(motion,&sv,cpu);cpu.ptr+=stride;sv.Format=df;d->CreateShaderResourceView(depth,&sv,cpu);cpu.ptr+=stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d->CreateUnorderedAccessView(s.guide,nullptr,&uv,cpu);
        UINT w=(std::min)(128u,i.renderW),h=(std::min)(128u,i.renderH),x=Origin(centerX,i.renderW,w),y=Origin(centerY,i.renderH,h);
        Barrier(c,motion,ms,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Barrier(c,depth,ds,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        c->SetDescriptorHeaps(1,&s.heap);c->SetComputeRootSignature(guideRoot);c->SetPipelineState(guidePso);auto gpu=s.heap->GetGPUDescriptorHandleForHeapStart();c->SetComputeRootDescriptorTable(0,gpu);gpu.ptr+=2ull*stride;c->SetComputeRootDescriptorTable(1,gpu);
        UINT words[]={x,y,w,h,i.renderW,i.renderH,i.motionW,i.motionH};c->SetComputeRoot32BitConstants(2,8,words,0);c->Dispatch((w+7)/8,(h+7)/8,1);
        Barrier(c,s.guide,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Tile t{};t.r=meta;t.r.stage=5;t.r.width=i.renderW;t.r.height=i.renderH;t.r.x=x;t.r.y=y;t.r.w=w;t.r.h=h;t.r.format=DXGI_FORMAT_R32G32B32A32_FLOAT;t.bpp=16;t.pitch=2048;t.offset=kGuideOffset;
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=s.guide;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=s.buffer;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Offset=t.offset;dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,w,h,1,2048};
        D3D12_BOX box{0,0,0,w,h,1};c->CopyTextureRegion(&dst,0,0,0,&src,&box);s.tiles.push_back(t);s.mask|=32;
        Barrier(c,s.guide,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);Barrier(c,motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,ms);Barrier(c,depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,ds);
    }
    void OutputsRecorded(){if(current>=0)slots[current].output=true;}
    void Submitted(ID3D12CommandQueue*q){
        if(current<0)return;auto&s=slots[current];
        if(s.output&&q&&SUCCEEDED(q->Signal(fence,++nextFence)))s.fenceValue=nextFence;
        else{s.poisoned=true;++cancelled;}current=-1;
    }
    void Cancel(){if(current>=0){slots[current].poisoned=true;++cancelled;current=-1;}}
    void Poll(){
        if(!fence)return;uint64_t done=fence->GetCompletedValue();if(done==UINT64_MAX)return;
        std::array<Slot*,kSlots> ordered{};for(UINT i=0;i<kSlots;i++)ordered[i]=&slots[i];
        std::sort(ordered.begin(),ordered.end(),[](const Slot*a,const Slot*b){return a->fenceValue<b->fenceValue;});
        for(auto*entry:ordered){auto&s=*entry;if(s.fenceValue&&done>=s.fenceValue){
            unsigned char*p=nullptr;D3D12_RANGE read{0,kBytes};
            if(SUCCEEDED(s.buffer->Map(0,&read,reinterpret_cast<void**>(&p)))){
                for(const auto&t:s.tiles){const auto*b=reinterpret_cast<const unsigned char*>(&t.r);bytes.insert(bytes.end(),b,b+sizeof(Record));for(UINT y=0;y<t.r.h;y++)bytes.insert(bytes.end(),p+t.offset+y*t.pitch,p+t.offset+y*t.pitch+t.r.w*t.bpp);}
                D3D12_RANGE none{0,0};s.buffer->Unmap(0,&none);
                {
                    const auto&r=s.metadata;const auto&i=s.info;
                    csv<<r.frame<<','<<r.tick<<','<<i.evaluate<<','<<captureMode<<','<<s.mask<<','<<i.renderW<<','<<i.renderH<<','<<i.motionW<<','<<i.motionH<<','<<i.fitX<<','<<i.fitY<<','<<i.fitW<<','<<i.fitH<<','<<i.jitterX<<','<<i.jitterY<<','<<i.scaleX<<','<<i.scaleY<<','<<i.temporalFlags<<','<<i.hostTransfer<<','<<centerX<<','<<centerY<<'\n';
                }
                ++readFrames;if(s.pixels&&(s.mask&15)==15)++pixelFrames;if(s.pixels&&(s.mask&32))++guideFrames;
                const UINT expected=(s.pixels?47u:0u)|((s.metadata.exposureSource==1||s.metadata.exposureSource==2)?16u:0u);
                if((s.mask&expected)!=expected)++dropped;
            }else ++dropped;s.fenceValue=0;
        }}
        if(running&&now-start>=duration)Seal();Publish();
    }
    void ReleaseAfterGpuIdle(){
        Poll();current=-1;if(running){now=start+duration;Seal();}
        for(auto&s:slots){Drop(s.buffer);Drop(s.guide);Drop(s.heap);}Drop(fence);Drop(guidePso);Drop(guideRoot);
    }
};
}
