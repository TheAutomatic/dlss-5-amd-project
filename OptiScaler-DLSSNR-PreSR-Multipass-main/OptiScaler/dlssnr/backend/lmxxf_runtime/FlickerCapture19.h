#pragma once
// Explicit test19 build only. No additional ExecuteCommandLists or GPU wait.
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
#include "ReuseProbe18.h"
#include "DiagnosticOverlay.h"
#include "native_lab_paths.h"

namespace NrDiagnostic19 {
inline std::mutex mutex;
inline NrDiagnosticOverlay snapshot;
inline const void* owner=nullptr;
}

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
    static constexpr UINT kSlots=8,kEdge=64,kReferences=4,kMaxPixels=1600;
    // Four stages: 128 square + four 32 squares; then exposure and float4 guides.
    static constexpr UINT kStageBytes=32*1024+4*4096;
    static constexpr UINT kExposureOffset=4*kStageBytes;
    static constexpr UINT kGuideOffset=kExposureOffset+512;
    static constexpr UINT kBytes=kGuideOffset+64*1024;
    struct Tile {Record r{};UINT offset=0,pitch=0,bpp=0;};
    struct Slot {
        ID3D12Resource *buffer=nullptr,*guide=nullptr;
        ID3D12DescriptorHeap *heap=nullptr;
        std::vector<Tile> tiles;
        uint64_t fenceValue=0;
        bool output=false,poisoned=false,pixels=false;
        bool fullWanted=false;
        UINT mask=0,sourceFormat=0,viewFormat=0,observedRtv=0;
        ID3D12Resource* fullBuffer=nullptr;
        UINT64 fullCapacity=0;std::vector<Tile> fullTiles;
        Record metadata{};
        FrameInfo info{};
    };
    std::array<Slot,kSlots> slots{};
    ID3D12Fence *fence=nullptr;
    ID3D12RootSignature *guideRoot=nullptr;
    ID3D12PipelineState *guidePso=nullptr;
    uint64_t nextFence=0,start=0,now=0;
    UINT sequence=0,burst=0,captureId=0,mode=0,captureMode=0,dropped=0,cancelled=0,readFrames=0,pixelFrames=0,guideFrames=0;
    UINT duration=20000,burstStart=0,burstLimit=1600;
    uint64_t lastPixel=0; UINT fullRequested=0,missingMask=0,sourceFormat=0,viewFormat=0,observedRtv=0;
    bool budgetFailed=false;
    int current=-1;
    bool running=false,sealed=false,previousKey=false,guideFailed=false,eligible=true,initialized=false,controlsVisible=false;
    bool keys[5]{};bool requestedCapture=false;
    float centerX=.5f,centerY=.5f;
    bool screenYFlipped=false;
    uint64_t lastMove=0;
    Record meta{};FrameInfo frameInfo{};
    std::vector<unsigned char> bytes,fullBytes;
    ReuseProbe18 reuse;
    UINT fullFrames=0,fullTarget=0,fullErrors=0;
    static constexpr UINT64 fullBudget=256ull*1024*1024;
    void CopyFull(ID3D12GraphicsCommandList*c,ID3D12Resource*r,D3D12_RESOURCE_STATES state,UINT stage){
        if(current<0||!r||stage<1||stage>2||!slots[current].pixels)return;
        auto&s=slots[current];auto d=r->GetDesc();
        UINT64 pitch=(d.Width*8+255)&~255ull,one=pitch*d.Height;
        if(stage==1&&fullTarget==0)fullTarget=UINT((std::min)(uint64_t(8),(fullBudget-4096)/(2*(one+sizeof(Record)))));
        if(stage==1 && fullTarget && fullRequested<fullTarget){
            const UINT groups=(fullTarget+1)/2;
            const uint64_t due=uint64_t(duration)*15/100+(groups>1?uint64_t(duration)*75*(fullRequested/2)/(100*(groups-1)):0);
            if(now-start>=due){s.fullWanted=true;++fullRequested;}
        }
        if(!s.fullWanted)return;
        if(d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT||!one||one>fullBudget/2||d.Width>8192||d.Height>8192){++fullErrors;return;}
        if(!s.fullBuffer){
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=2*one;b.Height=1;b.DepthOrArraySize=b.MipLevels=1;b.SampleDesc.Count=1;b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if(FAILED(r->GetDevice(IID_PPV_ARGS(&fullDevice)))){++fullErrors;return;}
            HRESULT hr=fullDevice->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.fullBuffer));Drop(fullDevice);
            if(FAILED(hr)){++fullErrors;return;}s.fullCapacity=2*one;
        }
        if(2*one!=s.fullCapacity){++fullErrors;return;}
        Tile t{};t.r=meta;t.r.stage=stage;t.r.width=t.r.w=UINT(d.Width);t.r.height=t.r.h=d.Height;t.r.format=d.Format;t.bpp=8;t.pitch=UINT(pitch);t.offset=UINT((stage-1)*one);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=r;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=s.fullBuffer;to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=t.offset;to.PlacedFootprint.Footprint={d.Format,t.r.w,t.r.h,1,t.pitch};
        Barrier(c,r,state,D3D12_RESOURCE_STATE_COPY_SOURCE);c->CopyTextureRegion(&to,0,0,0,&from,nullptr);Barrier(c,r,D3D12_RESOURCE_STATE_COPY_SOURCE,state);s.fullTiles.push_back(t);
    }
    ID3D12Device*fullDevice=nullptr;
    std::ostringstream csv;
    std::wstring directory,path;
    std::string identity,captureSettings;
    std::shared_ptr<std::atomic<int>> saved=std::make_shared<std::atomic<int>>(0);
    std::thread writer;
    mutable std::mutex statusMutex;
    std::string status=" test19 F9=capture centered-ROI";
    template<class T> static void Drop(T *&p){if(p)p->Release();p=nullptr;}
    static void Barrier(ID3D12GraphicsCommandList *c,ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        if(a==b)return;D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,0,a,b};c->ResourceBarrier(1,&v);
    }
    static UINT Origin(float center,UINT extent,UINT edge){return UINT(std::clamp(int(std::lround(center*extent))-int(edge/2),0,int(extent-edge)));}
    void Publish(){
        {
            std::lock_guard<std::mutex> lock(NrDiagnostic19::mutex);
            auto&o=NrDiagnostic19::snapshot; o={}; NrDiagnostic19::owner=this;
            o.visible=controlsVisible; o.state=!eligible?5:running?(now-start<duration?1:2):saved->load()<0?4:saved->load()>0?3:sealed?2:0;
            o.elapsedMs=UINT((std::min)(uint64_t(duration),now>=start&&captureId?now-start:0));o.durationMs=duration;
            o.width=frameInfo.renderW;o.height=frameInfo.renderH;o.edge=kEdge;o.pixels=pixelFrames;o.fullPairs=fullFrames;
            o.centerX=o.width?float(RoiX(o.width))/o.width:centerX;
            const float sourceY=o.height?float(RoiY(o.height))/o.height:centerY;
            o.centerY=NrCaptureSourceY(sourceY,screenYFlipped);o.sourceYFlipped=screenYFlipped;
            o.missingMask=missingMask;o.sourceFormat=sourceFormat;o.viewFormat=viewFormat;o.updated=GetTickCount64();
        }
        const char *names[]={"original","soft","identity"};
        std::ostringstream s;s<<" test19="<<names[mode]<<" capture="<<captureId<<":";
        const int result=saved->load();
        s<<(running?(now-start<duration?"capturing":"writing"):result<0?"failed":result>0?"saved":sealed?"writing":"ready");
        s<<" roi="<<centerX<<","<<centerY<<" seconds="<<(running?(now-start)/1000:0)
         <<" reuse="<<reuse.completed<<"/"<<reuse.requested<<" reuseErrors="<<reuse.failed<<" pending="<<Pending()<<" slot="<<current<<" pixelFrames="<<pixelFrames<<" guides="<<guideFrames<<" missingMask="<<missingMask<<" dropped="<<dropped<<" cancelled="<<cancelled;
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
        hr=D3DCompile(shader,sizeof(shader)-1,"test19-guides",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);Drop(errors);if(FAILED(hr))return false;
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
    bool Pending()const{if(reuse.Pending())return true;for(const auto&s:slots)if(s.fenceValue)return true;return false;}
    void Seal(){
        if(sealed||!running||Pending()||current>=0)return;
        running=false;sealed=true;
        std::ostringstream report;report<<identity<<captureSettings<<"\ncapture="<<captureId<<"\nmode="<<captureMode<<"\nstart_tick="<<start
          <<"\nend_tick="<<now<<"\nread_frames="<<readFrames<<"\npixel_frames="<<pixelFrames<<"\nguide_frames="<<guideFrames
          <<"\ndropped_frames="<<dropped<<"\ncancelled_frames="<<cancelled<<"\ncomplete="<<(pixelFrames==burst&&burst>0&&guideFrames==pixelFrames&&dropped==0&&cancelled==0?1:0)<<"\n";
        report<<"full_frames="<<fullFrames<<"\nfull_target="<<fullTarget<<"\nfull_errors="<<fullErrors
              <<"\nreuse_requested="<<reuse.requested<<"\nreuse_completed="<<reuse.completed<<"\nreuse_errors="<<reuse.failed<<"\n";
        const bool healthy=!budgetFailed&&now-start>=duration&&pixelFrames==burst&&burst>0&&guideFrames==pixelFrames&&dropped==0&&cancelled==0&&fullTarget>0&&fullFrames==fullTarget&&fullErrors==0&&reuse.failed==0&&reuse.completed==reuse.requested;
        report<<"source_resource_format="<<sourceFormat<<"\nsource_view_format="<<viewFormat<<"\nobserved_game_rtv_format="<<observedRtv
              <<"\nmissing_stage_mask="<<missingMask<<"\npixel_requested="<<burst<<"\nsample_interval_ms=16\nroi_edge=64\nbudget_failed="<<budgetFailed<<"\n";
        report<<"diagnostics_complete="<<(healthy?1:0)<<"\n";
        auto reuseText=reuse.Text();auto state=saved;std::wstring destination=path;
        writer=std::thread([data=std::move(bytes),timeline=csv.str(),info=report.str(),full=std::move(fullBytes),reuseText=std::move(reuseText),healthy,destination,state](){
            auto write=[](const std::wstring&p,const void*data,size_t n){FILE*f=_wfopen(p.c_str(),L"wb");if(!f)return false;bool ok=fwrite(data,1,n,f)==n;bool closed=fclose(f)==0;return ok&&closed;};
            bool ok=write(destination,data.data(),data.size());ok=write(destination+L".csv",timeline.data(),timeline.size())&&ok;
            ok=write(destination+L".info.txt",info.data(),info.size())&&ok;ok=write(destination+L".full",full.data(),full.size())&&ok;ok=write(destination+L".reuse.csv",reuseText.data(),reuseText.size())&&ok;state->store(ok&&healthy?1:-1);
        });Publish();
    }
  public:
    explicit Capture(UINT ms=20000,UINT burstMs=0,UINT frames=1600):duration(ms),burstStart(burstMs),burstLimit((std::min)(frames,kMaxPixels)){}
    ~Capture(){if(writer.joinable())writer.join();std::lock_guard<std::mutex> lock(NrDiagnostic19::mutex);if(NrDiagnostic19::owner==this){NrDiagnostic19::snapshot={};NrDiagnostic19::owner=nullptr;}}
    Capture(const Capture&)=delete;Capture&operator=(const Capture&)=delete;
    void Configure(const std::wstring&shaders,const std::wstring&modules,const std::wstring&weights){
        // Observed on Aniimo's Unity color buffers: display top corresponds to texture bottom.
        // Keep this game-specific mapping out of the network and codec; it only selects the ROI.
        wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);
        const std::wstring exePath=executable;const auto slash=exePath.find_last_of(L"\\/");
        screenYFlipped=_wcsicmp(exePath.substr(slash==std::wstring::npos?0:slash+1).c_str(),L"Aniimo.exe")==0;
        if(!identity.empty())return;wchar_t tmp[MAX_PATH]{};GetTempPathW(MAX_PATH,tmp);
        directory=std::wstring(tmp)+L"Lmxxf-NR-test19";CreateDirectoryW(directory.c_str(),nullptr);
        HMODULE module=nullptr;wchar_t dll[MAX_PATH]{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&FileHash),&module);
        GetModuleFileNameW(module,dll,MAX_PATH);
        SYSTEMTIME utc{};GetSystemTime(&utc);
        std::ostringstream s;s<<"build=NR-test19\ndiagnostic_revision=21\ncodec_candidate=original-only\n";
        s<<"runtime="<<Utf8(dll)<<"\nruntime_sha256="<<FileHash(dll)<<"\nshader_dir="<<Utf8(shaders)<<"\nmodules="<<Utf8(modules)<<"\nweights_dir="<<Utf8(weights)<<"\n";
        for(const auto*name:{L"native_codec_encode.hlsl",L"native_codec_decode.hlsl"})s<<Utf8(name)<<"_sha256="<<FileHash(shaders+L"\\"+name)<<"\n";
        for(const auto*arch:{L"gfx1200",L"gfx1201"})s<<"c32_wave1_"<<Utf8(arch)<<"_sha256="<<FileHash(modules+L"\\"+arch+L"\\c32-wave1.hsaco")<<"\n";
        s<<"utc="<<utc.wYear<<"-"<<utc.wMonth<<"-"<<utc.wDay<<"T"<<utc.wHour<<":"<<utc.wMinute<<":"<<utc.wSecond<<"Z\nutc_tick="<<GetTickCount64()<<"\n";identity=s.str();
    }
    void SetFrameInfo(const FrameInfo&i){frameInfo=i;eligible=i.allowed;}
    void UpdateControls(){
        DWORD pid=0;HWND window=GetForegroundWindow();GetWindowThreadProcessId(window,&pid);
        if(pid!=GetCurrentProcessId()){std::fill(std::begin(keys),std::end(keys),false);return;}
        const int code[5]={0,0,0,VK_F10,VK_F9};
        for(int i=0;i<5;i++){
            bool down=code[i]&&(GetAsyncKeyState(code[i])&0x8000)!=0,pressed=down&&!keys[i];keys[i]=down;
            if(pressed)controlsVisible=true;
            if(pressed&&!running&&saved->load()!=0&&writer.joinable())writer.join();
            if(pressed&&!running&&(!sealed||saved->load()!=0)&&i<3)mode=UINT(i);
            if(pressed&&!running&&!Pending()&&i==3){POINT p{};RECT r{};if(GetCursorPos(&p)&&ScreenToClient(window,&p)&&GetClientRect(window,&r)&&r.right>0&&r.bottom>0)SelectScreenPoint(float(p.x)/r.right,float(p.y)/r.bottom);}
        }
        const auto tick=GetTickCount64();
        if((GetAsyncKeyState(VK_CONTROL)&0x8000)&&!running&&!Pending()&&tick-lastMove>=50){
            const int dx=((GetAsyncKeyState(VK_RIGHT)&0x8000)?1:0)-((GetAsyncKeyState(VK_LEFT)&0x8000)?1:0);
            const int dy=((GetAsyncKeyState(VK_DOWN)&0x8000)?1:0)-((GetAsyncKeyState(VK_UP)&0x8000)?1:0);
            if(dx||dy){SelectScreenPoint(centerX+dx*.01f,NrCaptureSourceY(centerY,screenYFlipped)+dy*.01f);lastMove=tick;}
        }
        Publish();
    }
    bool SelectScreenPoint(float x,float y){
        if(running||Pending()||!std::isfinite(x)||!std::isfinite(y))return false;
        centerX=std::clamp(x,0.f,1.f);centerY=NrCaptureSourceY(std::clamp(y,0.f,1.f),screenYFlipped);
        controlsVisible=true;return true;
    }
    UINT Mode()const{return mode;}
    bool SelectMode(UINT requested){if(requested!=0||running||Pending())return false;mode=requested;Publish();return true;}
    bool CaptureKey()const{return keys[4]||requestedCapture;}
    void RequestCapture(){requestedCapture=true;}
    UINT TestFlags(bool)const{return 0;} // All diagnostic UI is drawn by the host overlay.
    void RecordReuse(hip_probe::Api&a,void*stream,const void*state,unsigned actualMode){if(running&&now-start<duration&&current>=0)reuse.Record(a,stream,state,actualMode,meta.frame);}
    UINT RoiX(UINT extent)const{UINT edge=(std::min)(kEdge,extent);return Origin(centerX,extent,edge)+edge/2;}
    UINT RoiY(UINT extent)const{UINT edge=(std::min)(kEdge,extent);return Origin(centerY,extent,edge)+edge/2;}
    const wchar_t*Path()const{return path.c_str();}
    std::string Status()const{std::lock_guard<std::mutex> lock(statusMutex);return status;}
    void Begin(ID3D12Device*d,Record metadata,bool keyDown,uint64_t clock=0){
        Poll();now=clock?clock:GetTickCount64();++sequence;
        bool pressed=keyDown&&!previousKey;previousKey=keyDown;requestedCapture=false;
        if(pressed)controlsVisible=true;
        if(pressed&&!running&&eligible&&(!sealed||saved->load()!=0)){
            if(writer.joinable())writer.join();
            if(!Ensure(d)){saved->store(-1);Publish();return;}
            if(directory.empty()){wchar_t tmp[MAX_PATH]{};GetTempPathW(MAX_PATH,tmp);directory=std::wstring(tmp)+L"Lmxxf-NR-test19";CreateDirectoryW(directory.c_str(),nullptr);}
            ++captureId;captureMode=mode;start=now;burst=dropped=cancelled=readFrames=pixelFrames=guideFrames=0;
            wchar_t name[128]{};swprintf_s(name,L"\\capture-%lu-%llu-%u-mode%u.nrhl",GetCurrentProcessId(),GetTickCount64(),captureId,mode);path=directory+name;
            {std::ostringstream settings;settings<<"\nseed=1\nlegacy_f8_disabled=1\nroi_source_x="<<centerX<<"\nroi_source_y="<<centerY<<"\nroi_display_y="<<NrCaptureSourceY(centerY,screenYFlipped)<<"\nroi_source_y_flipped="<<screenYFlipped<<"\n";
            for(const char*key:{"DLSS5_VIT_ADAPTIVE","DLSS5_VIT_REUSE_PERIOD","DLSS5_VIT_REUSE_GLOBAL","DLSS5_VIT_REUSE_LOCAL","DLSS5_VIT_REUSE_IMAGE","DLSS5_VIT_REUSE_HOTKEY","DLSS5_SKIP_BLOCKS","DLSS5_HIP_PDL","DLSS5_HIP_GRAPH","DLSS5_HIP_FAST","DLSS5_HIP_WAVE_OWNED","DLSS5_VIT_BYTE_STREAM","DLSS5_TYPELESS_RGBA16"}){const char*v=std::getenv(key);settings<<key<<"="<<(v?v:"<compile-default>")<<"\n";}
            captureSettings=settings.str();}
            fullBytes={'N','R','F','F','V','1',0,0};fullFrames=fullTarget=fullErrors=fullRequested=missingMask=0;lastPixel=0;budgetFailed=false;reuse.Start();
            bytes={'N','R','H','L','V','2',0,0};bytes.reserve(size_t(burstLimit)*kBytes+4*1024*1024);
            csv.str("");csv.clear();csv<<"frame,tick,evaluate,mode,mask,render_w,render_h,motion_w,motion_h,fit_x,fit_y,fit_w,fit_h,jitter_x,jitter_y,mv_scale_x,mv_scale_y,temporal_flags,host_transfer,roi_x,roi_y,source_format,view_format,observed_rtv_format\n";
            saved->store(0);running=true;sealed=false;EnsureGuides(d);
        }
        if(!running){Publish();return;}
        if(!eligible||now-start>=duration||bytes.size()>320ull*1024*1024){Seal();Publish();return;}
        current=-1;
        for(UINT i=0;i<kSlots;i++)if(!slots[i].fenceValue&&!slots[i].poisoned){current=int(i);break;}
        if(current<0){++dropped;Publish();return;}
        auto &s=slots[current];s.tiles.clear();s.fullTiles.clear();s.output=false;s.mask=0;s.info=frameInfo;s.sourceFormat=sourceFormat;s.viewFormat=viewFormat;s.observedRtv=observedRtv;
        s.fullWanted=false;s.pixels=now-start>=burstStart&&(!lastPixel||now-lastPixel>=16)&&burst<burstLimit;
        if(s.pixels){++burst;lastPixel=now;}
        if(burst>=burstLimit&&now-start+16<duration){budgetFailed=true;}
        meta=metadata;meta.frame=sequence;meta.tick=now;s.metadata=meta;
        Publish();
    }
    void Copy(ID3D12GraphicsCommandList*c,ID3D12Resource*r,D3D12_RESOURCE_STATES state,UINT stage,UINT x,UINT y,UINT w,UINT h){
        CopyFull(c,r,state,stage);
        if(current<0||!r||stage>4)return;auto&s=slots[current];if(stage<4&&!s.pixels)return;
        auto d=r->GetDesc();bool exposure=stage==4;
        DXGI_FORMAT typed=NativeViewFormat(d.Format);
        if(stage==0){sourceFormat=d.Format;viewFormat=typed;UINT n=sizeof(observedRtv);observedRtv=0;r->GetPrivateData(NrObservedRtvFormatGuid,&n,&observedRtv);s.sourceFormat=sourceFormat;s.viewFormat=viewFormat;s.observedRtv=observedRtv;}
        // Copy compatible raw bits, but label them with the SAME interpretation as the codec SRV.
        d.Format=typed;
        UINT bpp=(d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT||d.Format==DXGI_FORMAT_R16G16B16A16_UNORM)?8:exposure&&d.Format==DXGI_FORMAT_R32_FLOAT?4:exposure&&d.Format==DXGI_FORMAT_R16_FLOAT?2:0;
        if(!bpp||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.SampleDesc.Count!=1||d.DepthOrArraySize!=1||d.MipLevels!=1||!w||!h||uint64_t(x)+w>d.Width||uint64_t(y)+h>d.Height){missingMask|=1u<<stage;return;}
        Barrier(c,r,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        for(UINT tile=0;tile<(exposure?1u:5u);tile++){
            Tile t{};t.r=meta;t.r.stage=stage;t.r.tile=tile;t.r.width=UINT(d.Width);t.r.height=d.Height;t.r.format=d.Format;
            UINT edge=exposure?1:tile?16:64;t.r.w=(std::min)(edge,w);t.r.h=(std::min)(edge,h);
            const float cx=tile?(tile==1||tile==3?.25f:.75f):centerX,cy=tile?(tile<3?.25f:.75f):centerY;
            t.r.x=x+Origin(cx,w,t.r.w);t.r.y=y+Origin(cy,h,t.r.h);t.bpp=bpp;
            t.offset=exposure?kExposureOffset:stage*kStageBytes+(tile?32*1024+(tile-1)*4096:0);
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
        UINT w=(std::min)(64u,i.renderW),h=(std::min)(64u,i.renderH),x=Origin(centerX,i.renderW,w),y=Origin(centerY,i.renderH,h);
        Barrier(c,motion,ms,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Barrier(c,depth,ds,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        c->SetDescriptorHeaps(1,&s.heap);c->SetComputeRootSignature(guideRoot);c->SetPipelineState(guidePso);auto gpu=s.heap->GetGPUDescriptorHandleForHeapStart();c->SetComputeRootDescriptorTable(0,gpu);gpu.ptr+=2ull*stride;c->SetComputeRootDescriptorTable(1,gpu);
        UINT words[]={x,y,w,h,i.renderW,i.renderH,i.motionW,i.motionH};c->SetComputeRoot32BitConstants(2,8,words,0);c->Dispatch((w+7)/8,(h+7)/8,1);
        Barrier(c,s.guide,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Tile t{};t.r=meta;t.r.stage=5;t.r.width=i.renderW;t.r.height=i.renderH;t.r.x=x;t.r.y=y;t.r.w=w;t.r.h=h;t.r.format=DXGI_FORMAT_R32G32B32A32_FLOAT;t.bpp=16;t.pitch=1024;t.offset=kGuideOffset;
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=s.guide;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=s.buffer;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Offset=t.offset;dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,w,h,1,1024};
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
        reuse.Poll();if(!fence)return;uint64_t done=fence->GetCompletedValue();if(done==UINT64_MAX)return;
        std::array<Slot*,kSlots> ordered{};for(UINT i=0;i<kSlots;i++)ordered[i]=&slots[i];
        std::sort(ordered.begin(),ordered.end(),[](const Slot*a,const Slot*b){return a->fenceValue<b->fenceValue;});
        for(auto*entry:ordered){auto&s=*entry;if(s.fenceValue&&done>=s.fenceValue){
            unsigned char*p=nullptr;D3D12_RANGE read{0,kBytes};
            if(SUCCEEDED(s.buffer->Map(0,&read,reinterpret_cast<void**>(&p)))){
                for(const auto&t:s.tiles){const auto*b=reinterpret_cast<const unsigned char*>(&t.r);bytes.insert(bytes.end(),b,b+sizeof(Record));for(UINT y=0;y<t.r.h;y++)bytes.insert(bytes.end(),p+t.offset+y*t.pitch,p+t.offset+y*t.pitch+t.r.w*t.bpp);}
                D3D12_RANGE none{0,0};s.buffer->Unmap(0,&none);
                {
                    const auto&r=s.metadata;const auto&i=s.info;
                    csv<<r.frame<<','<<r.tick<<','<<i.evaluate<<','<<captureMode<<','<<s.mask<<','<<i.renderW<<','<<i.renderH<<','<<i.motionW<<','<<i.motionH<<','<<i.fitX<<','<<i.fitY<<','<<i.fitW<<','<<i.fitH<<','<<i.jitterX<<','<<i.jitterY<<','<<i.scaleX<<','<<i.scaleY<<','<<i.temporalFlags<<','<<i.hostTransfer<<','<<centerX<<','<<centerY<<','<<s.sourceFormat<<','<<s.viewFormat<<','<<s.observedRtv<<'\n';
                }
                if(!s.fullTiles.empty()){
                    unsigned char*fp=nullptr;D3D12_RANGE fr{0,SIZE_T(s.fullCapacity)};
                    if(s.fullTiles.size()!=2||FAILED(s.fullBuffer->Map(0,&fr,reinterpret_cast<void**>(&fp))))++fullErrors;
                    else{
                        for(const auto&t:s.fullTiles){const auto*header=reinterpret_cast<const unsigned char*>(&t.r);fullBytes.insert(fullBytes.end(),header,header+sizeof(Record));for(UINT y=0;y<t.r.h;y++)fullBytes.insert(fullBytes.end(),fp+t.offset+y*t.pitch,fp+t.offset+y*t.pitch+t.r.w*8);}
                        D3D12_RANGE none{0,0};s.fullBuffer->Unmap(0,&none);++fullFrames;
                    }
                }
                ++readFrames;if(s.pixels&&(s.mask&15)==15)++pixelFrames;if(s.pixels&&(s.mask&32))++guideFrames;
                const UINT expected=(s.pixels?47u:0u)|((s.metadata.exposureSource==1||s.metadata.exposureSource==2)?16u:0u);
                if((s.mask&expected)!=expected){++dropped;missingMask|=expected&~s.mask;}
            }else ++dropped;s.fenceValue=0;
        }}
        if(running&&now-start>=duration)Seal();Publish();
    }
    void ReleaseAfterGpuIdle(){
        Poll();current=-1;if(running){now=start+duration;Seal();}
        reuse.ReleaseAfterGpuIdle();for(auto&s:slots){Drop(s.fullBuffer);Drop(s.buffer);Drop(s.guide);Drop(s.heap);}Drop(fence);Drop(guidePso);Drop(guideRoot);
    }
};
}
