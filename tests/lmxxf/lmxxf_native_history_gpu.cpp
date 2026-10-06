#include "temporal_fixture.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h"
#include <memory>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
struct Recording {
    void* token=nullptr;ID3D12Resource* output=nullptr;
    bool historyRequested=false;
    ComPtr<ID3D12CommandAllocator> pa,ca;ComPtr<ID3D12GraphicsCommandList> p,c;
    ComPtr<ID3D12Fence> fence;UINT64 value=0;
};
int main(int argc,char** argv)try {
    Require(argc==3||(argc==4&&(!std::strcmp(argv[3],"--history-excludes-adaptive")||!std::strcmp(argv[3],"--history-900"))),"runtime DLL, modules directory, optional --history-excludes-adaptive or --history-900");
    const bool adaptive=argc==4&&!std::strcmp(argv[3],"--history-excludes-adaptive");
    const bool norm900=argc==4&&!std::strcmp(argv[3],"--history-900");
    if(norm900)for(auto kv:{std::pair{"DLSS5_FAST_NUMERIC","1"},{"DLSS5_NETWORK_FREE_RES","1"},{"DLSS5_SKIP_BLOCKS","none"}}){
        SetEnvironmentVariableA(kv.first,kv.second);_putenv_s(kv.first,kv.second);
    }
    const std::string decisions=std::string(argv[0])+".adaptive.csv";
    if(adaptive) {
        std::ofstream(decisions,std::ios::trunc).close();
        for(auto kv:{std::pair{"DLSS5_VIT_ADAPTIVE","1"},{"DLSS5_VIT_REUSE_PERIOD","16"},
                     {"DLSS5_VIT_REUSE_GLOBAL","1"},{"DLSS5_VIT_REUSE_LOCAL","50"},{"DLSS5_VIT_REUSE_IMAGE","1"}}) {
            SetEnvironmentVariableA(kv.first,kv.second);_putenv_s(kv.first,kv.second);
        }
        SetEnvironmentVariableA("DLSS5_VIT_ADAPTIVE_LOG",decisions.c_str());_putenv_s("DLSS5_VIT_ADAPTIVE_LOG",decisions.c_str());
    }
    const auto decisionCount=[&]() {
        size_t count=0;if(!adaptive)return count;
        std::ifstream f(decisions);Require(f.good(),"adaptive diagnostic file");
        std::string line;while(std::getline(f,line))if(!line.empty())++count;return count;
    };
    Gpu g(true);HMODULE dll=LoadLibraryA(argv[1]);Require(dll!=nullptr,"runtime load");
    auto get=reinterpret_cast<int32_t(*)(uint32_t,LmxxfNrApi*)>(GetProcAddress(dll,"LmxxfNrGetApi"));
    LmxxfNrApi api{};api.struct_size=sizeof(api);Require(get&&get(LMXXF_NR_ABI_VERSION,&api)==0,"current ABI");
    auto ok=[&](int32_t rc,const char* step){if(rc){char error[512]{};api.GetLastError(error,sizeof(error));throw std::runtime_error(std::string(step)+": "+error);}};
    std::string moduleArg=argv[2];std::wstring modules(moduleArg.begin(),moduleArg.end());
    LmxxfNrCreateInfo ci{};ci.struct_size=sizeof(ci);ci.device=g.device.Get();ci.queue=g.queue.Get();ci.assets_directory=modules.c_str();ci.flags=LMXXF_NR_CREATE_FLAG_RECORDING_LEASES;
    void* session=nullptr;ok(api.Create(&ci,&session),"create");ok(api.PrepareSession(session),"prepare session");
    const UINT w=norm900?1600:1280,h=norm900?900:720;
    auto colour=g.Texture(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT),motion=g.Texture(w,h,DXGI_FORMAT_R32G32_FLOAT),depth=g.Texture(w,h,DXGI_FORMAT_R32_FLOAT);
    std::vector<float> pixels(w*h*4,.4f),mv(w*h*2,0.f),z(w*h,.5f);
    for(UINT i=0;i<w*h;++i){pixels[i*4]=.25f+.15f*float(i%w)/w;pixels[i*4+3]=1;}
    g.Upload(colour.Get(),pixels);g.Upload(motion.Get(),mv);g.Upload(depth.Get(),z);
    ComPtr<ID3D12CommandQueue> other;D3D12_COMMAND_QUEUE_DESC qd{};Check(g.device->CreateCommandQueue(&qd,IID_PPV_ARGS(&other)),"second queue");
    std::vector<std::unique_ptr<Recording>> frames;
    auto record=[&](uint64_t sequence,bool history=true,bool guides=true,uint32_t flags=0){
        auto r=std::make_unique<Recording>();
        r->historyRequested=history;
        Check(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&r->pa)),"producer allocator");
        Check(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&r->ca)),"consumer allocator");
        Check(g.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,r->pa.Get(),nullptr,IID_PPV_ARGS(&r->p)),"producer");
        Check(g.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,r->ca.Get(),nullptr,IID_PPV_ARGS(&r->c)),"consumer");
        Check(g.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&r->fence)),"fence");
        LmxxfNrFrameInfo f{};f.struct_size=sizeof(f);f.frame_id=sequence;f.command_list=r->p.Get();f.color=colour.Get();f.color_width=w;f.color_height=h;f.color_state=ReadState;
        f.paper_white=f.model_scale=f.pre_exposure=f.exposure_scale=f.transfer_strength=f.color_strength=1;
        f.temporal_flags=(history?LMXXF_NR_TEMPORAL_MODEL_HISTORY:0)|LMXXF_NR_TEMPORAL_INPUTS_VALID|flags;
        f.motion=guides?motion.Get():nullptr;f.depth=depth.Get();f.motion_state=f.depth_state=ReadState;
        f.motion_width=w;f.motion_height=h;f.motion_scale_x=f.motion_scale_y=1;
        LmxxfNrJob job{};job.struct_size=sizeof(job);ok(api.PrepareFrame(session,&f,&job),"prepare");
        r->token=job.handle;r->output=static_cast<ID3D12Resource*>(job.private_output);
        ok(api.RecordInputs(session,r->token,r->p.Get()),"inputs");Check(r->p->Close(),"close producer");
        ok(api.RecordOutputs(session,r->token,r->c.Get()),"outputs");Check(r->c->Close(),"close consumer");
        auto* result=r.get();frames.push_back(std::move(r));return result;
    };
    auto status=[&](const char* expected){char text[1536]{};ok(api.GetStatus(session,text,sizeof(text)),"status");std::printf("%s\n",text);Require(std::strstr(text,expected)!=nullptr,expected);};
    auto run=[&](Recording* r,ID3D12CommandQueue* q,bool consumer=true){
        const auto before=decisionCount();
        ok(api.BeginRecordingExecution(session,r->token,q),"begin");
        ID3D12CommandList* p=r->p.Get();q->ExecuteCommandLists(1,&p);ok(api.EnqueueHip(session,r->token,q),"HIP");
        ID3D12CommandList* c=r->c.Get();if(consumer)q->ExecuteCommandLists(1,&c);
        HRESULT signal=q->Signal(r->fence.Get(),++r->value);
        ok(api.EndRecordingExecution(session,r->token,q,consumer?3:1,r->fence.Get(),r->value,signal),"end");
        Check(g.queue->Wait(r->fence.Get(),r->value),"readback dependency");
        if(adaptive) {
            const auto after=decisionCount();
            Require(r->historyRequested?after==before:after==before+1,
                    "requested history disables adaptive; history off restores its unchanged mode");
            Require(std::strcmp(std::getenv("DLSS5_VIT_ADAPTIVE"),"1")==0,"saved adaptive preference retained");
        }
        if(!consumer){g.Run([](auto*){});return uint64_t(0);} // No defined private output after a discarded consumer.
        auto values=g.Read(r->output);uint64_t hash=1469598103934665603ull;
        for(auto v:values){Require(std::isfinite(v),"finite output");uint32_t bits;std::memcpy(&bits,&v,4);hash=(hash^bits)*1099511628211ull;}
        return hash;
    };
    // Prepare multiple immutable jobs before executing any of them.
    auto* first=record(1);auto* second=record(2);auto* third=record(3);
    if(adaptive)Require(decisionCount()==0,"history warm-up also excludes adaptive");
    auto prime=run(first,g.queue.Get());status("history=priming");
    auto active=run(second,other.Get());status("history=active");
    Require(run(second,g.queue.Get())==prime,"same recording replay must reset seed/history");status("history=priming");
    Require(run(third,other.Get())==active,"next actual execution resumes history deterministically");status("history=active");
    ok(api.ResetHistory(session),"explicit reset");Require(run(third,g.queue.Get())==prime,"reset applies to already recorded commands");
    // Resetting/replaying the priming seed (0 -> 0) must also invalidate ViT.
    Require(run(third,g.queue.Get())==prime,"repeated priming execution");
    auto* cancelled=record(4);ok(api.BeginRecordingExecution(session,cancelled->token,other.Get()),"control-only begin");
    ok(api.EndRecordingExecution(session,cancelled->token,other.Get(),0,nullptr,0,S_OK),"control-only cancel");
    auto* after=record(5);Require(run(after,g.queue.Get())==prime,"cancel cannot publish history");
    auto* noGuides=record(6,true,false);status("history=missing-guides");run(noGuides,g.queue.Get());
    auto* off=record(7,false);run(off,g.queue.Get());status("history=off");
    if(adaptive) {
        run(off,other.Get());
        std::ifstream f(decisions);std::string line,last;while(std::getline(f,line))if(!line.empty())last=line;
        unsigned frame=0,used=0;Require(std::sscanf(last.c_str(),"%u,%u",&frame,&used)==2&&used==1,
            "unchanged input actually reuses ViT after turning history off");
    }
    Require(run(first,other.Get())==prime,"old chain replay after history-off rebuild");
    SetEnvironmentVariableA("DLSS5_MULTI_PASS","2");_putenv_s("DLSS5_MULTI_PASS","2");
    auto* multi=record(8);status("history=ready");auto multiPrime=run(multi,g.queue.Get());status("history=priming");status("passes=2");
    auto* multiNext=record(9);auto multiActive=run(multiNext,other.Get());status("history=active");
    Require(run(multi,g.queue.Get())==multiPrime,"multipass replay resets history deterministically");
    Require(run(multiNext,other.Get())==multiActive,"multipass history resumes after replay");
    Require(run(first,other.Get())==prime,"old single-pass chain retained across multipass rebuild");
    SetEnvironmentVariableA("DLSS5_MULTI_PASS","1");_putenv_s("DLSS5_MULTI_PASS","1");
    auto* restored=record(9);Require(run(restored,g.queue.Get())==prime,"single-pass restoration primes");
    if(adaptive) {
        Require(run(record(10),other.Get())==active,"history restoration uses full ViT without changing its preference");
        run(record(11),other.Get(),false);status("history=reset-after-discard");
        Require(run(record(12),g.queue.Get())==prime,"producer-only discard primes full ViT history");
        run(record(13),other.Get());
        Require(run(record(14,true,true,LMXXF_NR_TEMPORAL_RESET),g.queue.Get())==prime,"explicit frame reset");
        // Even maximally permissive gates cannot enable reuse while history is requested.
        for(auto key:{"DLSS5_VIT_REUSE_GLOBAL","DLSS5_VIT_REUSE_LOCAL","DLSS5_VIT_REUSE_IMAGE"}){SetEnvironmentVariableA(key,"1000000");_putenv_s(key,"1000000");}
        run(record(15),g.queue.Get());
    }
    auto collect=[&](){for(auto& r:frames){r->p.Reset();r->c.Reset();ok(api.InvalidateRecording(session,r->token),"invalidate");ok(api.CollectRecording(session,r->token),"collect");}frames.clear();};
    if(norm900){
        // Only keep one retired configuration at a time; each cycle exercises
        // network/auxiliary/history retirement, not unbounded retained test jobs.
        for(auto settings:{std::pair{3,1},std::pair{3,0},std::pair{2,0},std::pair{1,0}}){
            collect();auto passes=std::to_string(settings.first),predict=std::to_string(settings.second);
            SetEnvironmentVariableA("DLSS5_MULTI_PASS",passes.c_str());_putenv_s("DLSS5_MULTI_PASS",passes.c_str());
            SetEnvironmentVariableA("DLSS5_MULTI_PASS_PREDICT",predict.c_str());_putenv_s("DLSS5_MULTI_PASS_PREDICT",predict.c_str());
            const char* skin=settings.first==2?"1":"0";
            SetEnvironmentVariableA("DLSS5_MULTI_PASS_SKIN_PROTECT",skin);_putenv_s("DLSS5_MULTI_PASS_SKIN_PROTECT",skin);
            auto* a=record(20);auto hash=run(a,g.queue.Get());status("history=priming");
            auto* b=record(21);run(b,other.Get());status("history=active");
            Require(run(a,g.queue.Get())==hash,"pass/prediction/skin reset is deterministic");
            run(b,other.Get(),false);status("history=reset-after-discard");
            Require(run(record(22),g.queue.Get())==hash,"multipass discard cannot publish history");
        }
    }
    collect();
    frames.clear();ok(api.Destroy(session),"destroy");g.NoErrors();FreeLibrary(dll);
    std::printf("native history runtime GPU PASS prime=%016llx active=%016llx\n",static_cast<unsigned long long>(prime),static_cast<unsigned long long>(active));return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}

