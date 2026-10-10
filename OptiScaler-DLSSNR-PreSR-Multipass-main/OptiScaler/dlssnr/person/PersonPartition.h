#pragma once
#include "PersonInference.h"
#include "PersonShader.h"
#include "PersonSettings.h"
#include "PersonDiagnostics.h"
#include "PersonCapture.h"
#include "../effects/NrOutputEffects.h"
#include "../backend/lmxxf_runtime/TemporalControl.h"
#include "../backend/lmxxf_runtime/LmxxfShaderCompiler.h"

namespace DlssNr::Person
{
using Microsoft::WRL::ComPtr;
using Completion = LmxxfRuntime::RecordingCompletion;
inline void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("person D3D12 preparation failed");}
inline ComPtr<ID3D12Resource> Buffer(ID3D12Device* d,UINT64 bytes,D3D12_HEAP_TYPE type,bool uav=false){
 D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
 desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 if(uav)desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 ComPtr<ID3D12Resource> r;Check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,
 type==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&r)));return r;
}
inline ComPtr<ID3D12Resource> Texture(ID3D12Device*d,UINT w,UINT h,DXGI_FORMAT format){
 auto desc=Effects::Storage::Description(w,h);desc.Format=format;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
 ComPtr<ID3D12Resource> r;Check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r)));return r;
}
struct Pipeline {
 ComPtr<ID3D12Device> device;
 ComPtr<ID3D12RootSignature> root;
 ComPtr<ID3D12PipelineState> capture,warp,compose;
 LmxxfRuntime::TemporalControl control;
 std::vector<std::shared_ptr<Completion>> completions;
 std::shared_ptr<Completion> chain;
 bool unconfirmed=false;
 static std::shared_ptr<Pipeline> Create(ID3D12Device*d){
  auto p=std::make_shared<Pipeline>();p->device=d;p->control.Create(d);
  D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,33,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,4,0,0,33}};
  D3D12_ROOT_PARAMETER params[3]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
  params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,24};
  params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[2].Descriptor.ShaderRegister=1;
  D3D12_ROOT_SIGNATURE_DESC desc{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
  Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
  Check(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&p->root)));
  auto shader=[&](const char*entry,ComPtr<ID3D12PipelineState>& out){
   blob.Reset();error.Reset();Check(NativeCompileShaderBlob(Shader,strlen(Shader),"person partition",nullptr,nullptr,entry,&blob,&error,"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,::LmxxfCompiler()));
   D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=p->root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
   Check(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&out)));
  };
  shader("capture_main",p->capture);shader("warp_main",p->warp);shader("compose_main",p->compose);return p;
 }
};
struct Guide {
 ComPtr<ID3D12Resource> texture,mask;
 uint64_t frame=0,epoch=0,tick=0;float jitterX=0,jitterY=0;
 bool maskAccepted=false;
};
struct Storage {
 std::shared_ptr<Pipeline> pipeline;
 std::shared_ptr<Guide> guide;
 ComPtr<ID3D12Resource> output,maskUpload,input,readback,maskReadback;
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT maskFootprint{};
 UINT64 maskReadbackBytes=0;
 ComPtr<ID3D12DescriptorHeap> heap;
 unsigned width=0,height=0;
 UINT64 bytes=0;
};
struct Lease;
struct State {
 std::vector<std::shared_ptr<Lease>> leases;
 std::vector<std::shared_ptr<Storage>> pool;
 std::vector<std::shared_ptr<Guide>> history;
 std::shared_ptr<Pipeline> pipeline;
 CaptureSchedule captureSchedule;
 uint64_t epoch=1,frame=0,submitted=0;
 unsigned width=0,height=0;
 std::string status;
 PersonDiagnostics diagnostic;
 void Invalidate(ResetReason reason=ResetReason::External){diagnostic.Reset(reason);++epoch;submitted=0;history.clear();captureSchedule={};}
};
inline State& Global(){static auto*s=new State;return *s;}
inline void Collect();
inline void ScheduleCollection();
struct Lease final:Submission::RecordingObserver {
 std::shared_ptr<Storage> storage;
 std::vector<std::shared_ptr<Guide>> history;
 std::shared_ptr<Guide> previousGuide;
 ComPtr<ID3D12Resource> original,first,final,motion,depth;
 std::vector<std::shared_ptr<Completion>> completions;
 Submission::RecordingIdentity identity{};
 uint64_t epoch=0,frame=0,previous=0,tick=0,maskTick=0;
 bool invalidated=false,unconfirmed=false,sent=false,continuation=false;
 bool diagnosticCaptured=false,diagnosticCollected=false;
 uint64_t diagnosticGeneration=0;
 unsigned executions=0;
 HRESULT BeforeExecute(const Submission::RecordingExecution&e)noexcept override{
  try {
   if(invalidated||!(e.identity==identity)||unconfirmed)return E_UNEXPECTED;
   auto&p=*storage->pipeline;auto&s=Global();
   if(p.unconfirmed)return E_FAIL;
   if(p.chain&&p.chain->queue!=e.queue&&!p.chain->Complete())Check(e.queue->Wait(p.chain->fence,p.chain->value));
   auto done=[](auto&v){v.erase(std::remove_if(v.begin(),v.end(),[](auto&c){return c->Complete();}),v.end());};
   done(completions);done(p.completions);
   // Bound auxiliary submissions as well as retained game recordings.
   if(p.completions.size()>=64)return E_OUTOFMEMORY;
   const auto now=GetTickCount64();
   const bool temporalValid=executions==0&&epoch==s.epoch&&previous==s.submitted&&now-tick<=250;
   if(!temporalValid)s.Invalidate(executions?ResetReason::Replay:epoch!=s.epoch?ResetReason::Epoch:
      previous!=s.submitted?ResetReason::Order:ResetReason::Delayed);
   const uint32_t valid=temporalValid&&(now-maskTick<=250);
   if(s.diagnostic.enabled){++s.diagnostic.executed;s.diagnostic.submitDelay.Add(now-tick);
    if(temporalValid&&now-maskTick>250)++s.diagnostic.executionExpired;}
   ++executions;unconfirmed=p.unconfirmed=true;
   p.control.Submit(p.device.Get(),e.queue,&valid,sizeof(valid),completions,p.completions);
   unconfirmed=p.unconfirmed=false;return S_OK;
  }catch(...){return E_FAIL;}
 }
 void Executed(const Submission::RecordingExecution&e)noexcept override{
  if(!e.producerSubmitted)return;
  auto&p=*storage->pipeline;unconfirmed=p.unconfirmed=true;
  if(FAILED(e.status)||!e.fence||!e.fenceValue||e.fenceValue==UINT64_MAX){unconfirmed=p.unconfirmed=false;return;}
  try {
   auto proof=std::make_shared<Completion>(e.fence,e.queue,e.fenceValue);completions.push_back(proof);p.chain=proof;
   auto&s=Global();
   if(epoch==s.epoch&&executions==1&&(!continuation||e.continuationSubmitted)){
    s.submitted=frame;s.history.push_back(storage->guide);
    if(s.history.size()>HistoryCount)s.history.erase(s.history.begin());
   }else s.Invalidate(epoch!=s.epoch?ResetReason::Epoch:executions!=1?ResetReason::Replay:ResetReason::Continuation);
   unconfirmed=p.unconfirmed=false;
  }catch(...){unconfirmed=p.unconfirmed=false;}
 }
 void Invalidated(Submission::RecordingIdentity id)noexcept override{
  if(id==identity){invalidated=true;try{Collect();ScheduleCollection();}catch(...){}}
 }
};
inline void Collect(){
 auto&s=Global();
 // Retain only the newest completed source for this collection. The shared
 // lease keeps its readback alive even if the game has already reset its list.
 std::shared_ptr<Lease> newest;
 uint64_t completedCaptures=0;
 for(auto it=s.leases.begin();it!=s.leases.end();){
  auto&l=**it;const bool removed=FAILED(l.storage->pipeline->device->GetDeviceRemovedReason());
  auto&v=l.completions;v.erase(std::remove_if(v.begin(),v.end(),[](auto&p){return p->Complete();}),v.end());
  if(!removed&&l.diagnosticCaptured&&!l.diagnosticCollected&&l.executions==1&&!l.unconfirmed&&v.empty()){
   l.diagnosticCollected=true;
   if(s.diagnostic.enabled&&l.diagnosticGeneration==s.diagnostic.generation){
    void* data=nullptr;D3D12_RANGE range{0,SIZE_T(l.storage->maskReadbackBytes)};
    if(SUCCEEDED(l.storage->maskReadback->Map(0,&range,&data))){
     uint64_t coverage=0,valid=0;
     for(unsigned y=0;y<160;++y){auto* row=reinterpret_cast<const float*>(static_cast<const char*>(data)+
       l.storage->maskFootprint.Offset+y*l.storage->maskFootprint.Footprint.RowPitch);
      for(unsigned x=0;x<160;++x){coverage+=row[2*x]>.5f;valid+=row[2*x+1]>.5f;}}
     D3D12_RANGE written{0,0};l.storage->maskReadback->Unmap(0,&written);
     s.diagnostic.warpedCoverage.Add(coverage*10000/(160*160));s.diagnostic.warpValid.Add(valid*10000/(160*160));
    }
   }
  }
  if(!removed&&!l.sent&&l.storage->readback&&l.executions==1&&!l.unconfirmed&&v.empty()){
   l.sent=true;
   const auto elapsed=GetTickCount64()-l.tick;
   if(s.diagnostic.enabled)s.diagnostic.captureAge.Add(elapsed);
   ++completedCaptures;
   if(l.epoch==s.epoch&&elapsed<=250&&s.captureSchedule.Newer(l.frame)&&(!newest||l.frame>newest->frame))newest=*it;
  }
  if(l.invalidated&&(removed||(!l.unconfirmed&&v.empty())))it=s.leases.erase(it);else ++it;
 }
 bool submitted=false;
 if(newest&&Worker().Ready()){
  auto& l=*newest;
  auto image=std::make_shared<Image>();image->epoch=l.epoch;image->frame=l.frame;image->tick=l.tick;image->width=l.storage->width;image->height=l.storage->height;
  image->rgb.resize(3*ModelSize*ModelSize);void*data=nullptr;D3D12_RANGE range{0,image->rgb.size()*sizeof(float)};
  if(SUCCEEDED(l.storage->readback->Map(0,&range,&data))){memcpy(image->rgb.data(),data,range.End);D3D12_RANGE written{0,0};l.storage->readback->Unmap(0,&written);submitted=Worker().Submit(image);}
  if(submitted)s.captureSchedule.Submitted(l.frame);
 }
 // Busy workers drop completed sources instead of building a stale CPU queue.
 if(s.diagnostic.enabled){s.diagnostic.captureSubmitted+=submitted;s.diagnostic.captureDiscarded+=completedCaptures-submitted;}
}
struct TimerState {PTP_TIMER timer=nullptr;HMODULE module=nullptr;};
inline TimerState& Timer(){static TimerState timer;return timer;}
inline void Arm(PTP_TIMER timer){LARGE_INTEGER due{};due.QuadPart=-1000000;FILETIME time{due.LowPart,DWORD(due.HighPart)};SetThreadpoolTimer(timer,&time,0,0);}
inline void CALLBACK CollectionCallback(PTP_CALLBACK_INSTANCE instance,void*,PTP_TIMER timer){
 std::lock_guard lock(Submission::RecordingMutex());
 try{Collect();}catch(...){}
 if(std::any_of(Global().leases.begin(),Global().leases.end(),[](auto&l){return l->invalidated;})){Arm(timer);return;}
 auto module=Timer().module;Timer()={};CloseThreadpoolTimer(timer);FreeLibraryWhenCallbackReturns(instance,module);
}
inline void ScheduleCollection(){
 if(Timer().timer||!std::any_of(Global().leases.begin(),Global().leases.end(),[](auto&l){return l->invalidated;}))return;
 HMODULE module=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&CollectionCallback),&module))return;
 auto*timer=CreateThreadpoolTimer(&CollectionCallback,nullptr,nullptr);if(!timer){FreeLibrary(module);return;}Timer()={timer,module};Arm(timer);
}
inline void Poll(){std::lock_guard lock(Submission::RecordingMutex());try{Collect();ScheduleCollection();}catch(...){}}
inline void Invalidate(){std::lock_guard lock(Submission::RecordingMutex());Global().Invalidate();}
inline void Reset(){
 std::lock_guard lock(Submission::RecordingMutex());auto&s=Global();s.Invalidate();s.pool.clear();s.pipeline.reset();s.status.clear();Collect();ScheduleCollection();
}
inline std::string Status(){std::lock_guard lock(Submission::RecordingMutex());return Global().status+" | "+Worker().Status();}
inline std::string DiagnosticReport(bool enabled)noexcept{
 try{std::lock_guard lock(Submission::RecordingMutex());auto& d=Global().diagnostic;
  const auto now=GetTickCount64();d.Enable(enabled,now);return d.Report(now);}catch(...){return {};}
}
inline bool Prepare(bool enabled,const std::filesystem::path& directory, int modelIndex = 0, unsigned faceSize = 320){
 try {
  const auto modelFile = DlssNr::Person::ModelFileName(modelIndex);
  Worker().Configure(enabled,directory/L"person-model", modelFile,modelIndex==2?faceSize:320);
  std::lock_guard lock(Submission::RecordingMutex());Collect();
  if(!enabled){auto&s=Global();if(s.pipeline||!s.history.empty())Reset();return false;}
  return Worker().Available();
 } catch (...) { return false; }
}
inline ID3D12Resource* Record(ID3D12GraphicsCommandList*cmd,ID3D12Resource*original,ID3D12Resource*first,ID3D12Resource*final,
 D3D12_RESOURCE_STATES originalState,unsigned width,unsigned height,const Effects::Guides& guides,
 std::shared_ptr<const Mask> mask = Worker().Latest(), Settings settings = {}){
 settings=settings.Bounded();
 std::lock_guard lock(Submission::RecordingMutex());auto&s=Global();
 s.diagnostic.Enable(settings.debugMask,GetTickCount64());
 if(!cmd||cmd->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT||!first||!final||!Effects::ValidGuides(guides,width,height)||guides.jittered){
  s.Invalidate(ResetReason::Guides);s.status="Person partition bypassed: first pass or reliable motion/depth unavailable";return final;
 }
 try {
  Collect();ComPtr<ID3D12Device>device;Check(cmd->GetDevice(IID_PPV_ARGS(&device)));
  const auto a=original->GetDesc(),b=first->GetDesc(),c=final->GetDesc();
  for(auto desc:{a,b,c})if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||
    desc.Width<width||desc.Height<height||(desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||!Effects::ColorFormat(desc.Format))
     throw std::runtime_error("unsupported person colour texture");
  if(guides.reset||s.width!=width||s.height!=height){s.Invalidate(ResetReason::Resize);s.width=width;s.height=height;s.pool.clear();}
  if(!s.pipeline||s.pipeline->device.Get()!=device.Get()||s.pipeline->unconfirmed){s.Invalidate(ResetReason::Device);s.pool.clear();s.pipeline=Pipeline::Create(device.Get());}
  ComPtr<Submission::ILogicalCommandList>logical;ComPtr<Submission::IRecordingResources>observer;
  Check(cmd->QueryInterface(IID_PPV_ARGS(&logical)));Check(cmd->QueryInterface(IID_PPV_ARGS(&observer)));
  if(!observer->CanAppendCompute())throw std::runtime_error("person recording state");
  std::vector<const Storage*>seen;UINT64 bytes=0;
  auto count=[&](const std::shared_ptr<Storage>&st){if(std::find(seen.begin(),seen.end(),st.get())==seen.end()){seen.push_back(st.get());bytes+=st->bytes;}};
  for(auto&st:s.pool)count(st);for(auto&l:s.leases)count(l->storage);
  std::shared_ptr<Storage>storage;
  for(auto&st:s.pool)if(st.use_count()==1&&st->width==width&&st->height==height){storage=st;break;}
  if(!storage){
   const auto desc=Effects::Storage::Description(width,height);const auto estimate=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes+16ull*1024*1024;
   if(seen.size()>=24||bytes+estimate>2048ull*1024*1024)throw std::runtime_error("person recording memory budget busy");
   storage=std::make_shared<Storage>();storage->pipeline=s.pipeline;storage->width=width;storage->height=height;storage->bytes=estimate;
   storage->output=Texture(device.Get(),width,height,DXGI_FORMAT_R16G16B16A16_FLOAT);
   storage->maskUpload=Buffer(device.Get(),160*160*4,D3D12_HEAP_TYPE_UPLOAD);
   D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,37,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
   Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&storage->heap)));s.pool.push_back(storage);
  }
  storage->guide=std::make_shared<Guide>();storage->guide->texture=Texture(device.Get(),160,160,DXGI_FORMAT_R32G32B32A32_FLOAT);
  storage->guide->mask=Texture(device.Get(),160,160,DXGI_FORMAT_R32G32_FLOAT);
  storage->guide->frame=++s.frame;storage->guide->epoch=s.epoch;storage->guide->jitterX=guides.jitterX;storage->guide->jitterY=guides.jitterY;
  auto lease=std::make_shared<Lease>();lease->storage=storage;lease->original=original;lease->first=first;lease->final=final;lease->motion=guides.motion;lease->depth=guides.depth;
  lease->identity=logical->Identity();lease->continuation=observer->InContinuation();lease->epoch=s.epoch;lease->frame=s.frame;lease->previous=s.submitted;lease->tick=GetTickCount64();
  storage->guide->tick=lease->tick;
  if(!s.history.empty()&&s.history.back()->frame+1==s.frame&&s.history.back()->epoch==s.epoch)lease->previousGuide=s.history.back();
  unsigned age=0;bool accepted=false;
  if(mask&&mask->values.size()==160*160&&mask->epoch==s.epoch&&mask->width==width&&mask->height==height&&lease->tick-mask->tick<=250&&mask->frame<=s.frame&&s.frame-mask->frame<=HistoryCount){
   age=unsigned(s.frame-mask->frame);
   if(age<=s.history.size()){
    accepted=true;
    for(unsigned i=0;i<age;++i){auto h=s.history[s.history.size()-1-i];if(h->frame!=s.frame-1-i||h->epoch!=s.epoch){accepted=false;break;}lease->history.push_back(h);}
   }
  }
  if(!accepted){age=0;lease->history.clear();}
  if(s.diagnostic.enabled){
   auto reason=accepted?MaskDecision::Accepted:!mask?MaskDecision::Missing:
    mask->epoch!=s.epoch?MaskDecision::Epoch:mask->values.size()!=160*160||mask->width!=width||mask->height!=height?MaskDecision::Size:
    lease->tick-mask->tick>250?MaskDecision::Expired:mask->frame>s.frame||s.frame-mask->frame>HistoryCount?MaskDecision::FrameAge:MaskDecision::Guides;
   s.diagnostic.Mask(reason);
   if(mask){s.diagnostic.maskAge.Add(lease->tick-mask->tick);
    if(mask->epoch!=s.diagnostic.rawEpoch||mask->frame!=s.diagnostic.rawFrame){
     s.diagnostic.rawEpoch=mask->epoch;s.diagnostic.rawFrame=mask->frame;
     s.diagnostic.arrivalAge.Add(lease->tick-mask->tick);
     if(!mask->values.empty())s.diagnostic.rawCoverage.Add(
      uint64_t(std::count_if(mask->values.begin(),mask->values.end(),[](float v){return v>.5f;}))*10000/mask->values.size());
    }
   }
  }
  storage->guide->maskAccepted=accepted;
  lease->maskTick=accepted?mask->tick:lease->tick;
  void*mapped=nullptr;D3D12_RANGE noRead{0,0};Check(storage->maskUpload->Map(0,&noRead,&mapped));
  if(accepted)memcpy(mapped,mask->values.data(),160*160*4);else memset(mapped,0,160*160*4);storage->maskUpload->Unmap(0,nullptr);
  const auto pending=std::count_if(s.leases.begin(),s.leases.end(),[](auto& l){return !l->sent&&l->storage->readback;});
  const bool capture=s.captureSchedule.Request(CaptureClock(),size_t(pending),Worker().Available());
  if(capture){if(!storage->input)storage->input=Buffer(device.Get(),3*640*640*4,D3D12_HEAP_TYPE_DEFAULT,true);
   if(!storage->readback)storage->readback=Buffer(device.Get(),3*640*640*4,D3D12_HEAP_TYPE_READBACK);
  }else lease->sent=true;
  if(s.diagnostic.Sample(lease->tick))try{
   if(!storage->maskReadback){auto desc=storage->guide->mask->GetDesc();
    device->GetCopyableFootprints(&desc,0,1,0,&storage->maskFootprint,nullptr,nullptr,&storage->maskReadbackBytes);
    storage->maskReadback=Buffer(device.Get(),storage->maskReadbackBytes,D3D12_HEAP_TYPE_READBACK);}
   lease->diagnosticCaptured=true;lease->diagnosticGeneration=s.diagnostic.generation;
  }catch(...){/* Optional diagnostics must not bypass person protection. */}
  s.leases.push_back(lease);if(FAILED(observer->ObserveResources(lease))){lease->invalidated=true;Collect();return final;}
  const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu=storage->heap->GetCPUDescriptorHandleForHeapStart();
  auto srv=[&](ID3D12Resource*r,DXGI_FORMAT fmt){D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=fmt;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Texture2D.MipLevels=1;device->CreateShaderResourceView(r,&v,cpu);cpu.ptr+=stride;};
  srv(original,Effects::ReadFormat(a.Format));srv(first,Effects::ReadFormat(b.Format));srv(final,Effects::ReadFormat(c.Format));
  srv(guides.motion,Effects::MotionFormat(guides.motion->GetDesc().Format));srv(guides.depth,Effects::DepthFormat(guides.depth->GetDesc().Format));
  srv(storage->guide->texture.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);
  for(unsigned i=0;i<HistoryCount;++i)srv(i<lease->history.size()?lease->history[i]->texture.Get():(i==0&&lease->previousGuide?lease->previousGuide->texture.Get():nullptr),DXGI_FORMAT_R32G32B32A32_FLOAT);
  D3D12_SHADER_RESOURCE_VIEW_DESC bv{};bv.Format=DXGI_FORMAT_R32_FLOAT;bv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;bv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;bv.Buffer.NumElements=160*160;
  device->CreateShaderResourceView(storage->maskUpload.Get(),&bv,cpu);cpu.ptr+=stride;srv(storage->guide->mask.Get(),DXGI_FORMAT_R32G32_FLOAT);
  srv(lease->previousGuide?lease->previousGuide->mask.Get():nullptr,DXGI_FORMAT_R32G32_FLOAT);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=DXGI_FORMAT_R32_FLOAT;uv.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uv.Buffer.NumElements=3*640*640;
  device->CreateUnorderedAccessView(storage->input.Get(),nullptr,&uv,cpu);cpu.ptr+=stride;
  auto uav=[&](ID3D12Resource*r,DXGI_FORMAT fmt){D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=fmt;v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(r,nullptr,&v,cpu);cpu.ptr+=stride;};
  uav(storage->guide->texture.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);uav(storage->guide->mask.Get(),DXGI_FORMAT_R32G32_FLOAT);uav(storage->output.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
  const auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  Effects::Barrier(cmd,original,originalState,read);Effects::Barrier(cmd,guides.motion,guides.motionState,read);Effects::Barrier(cmd,guides.depth,guides.depthState,read);
  auto*heap=storage->heap.Get();cmd->SetDescriptorHeaps(1,&heap);cmd->SetComputeRootSignature(s.pipeline->root.Get());cmd->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
  const float scaleX=float(width)/(std::max)(width,height),scaleY=float(height)/(std::max)(width,height);
  const auto prev=lease->previousGuide;
  const float deltaMs=prev?float(std::clamp<uint64_t>(lease->tick-prev->tick,1,100)):100.f;
  Constants constants{width,height,guides.motionWidth,guides.motionHeight,guides.motionScaleX/guides.motionWidth,guides.motionScaleY/guides.motionHeight,
   prev?(prev->jitterX-guides.jitterX)/width:0,prev?(prev->jitterY-guides.jitterY)/height:0,guides.preExposure,unsigned(capture),age,unsigned(accepted),
   scaleX,scaleY,(1-scaleX)*.5f,(1-scaleY)*.5f,unsigned(guides.inverted),settings.strength,settings.detail,
   accepted?(std::min)(MaskFreshness(lease->tick-mask->tick),age<=20?1.f:float(24-age)/4.f):0.f,
   unsigned(accepted&&prev&&prev->maskAccepted),1.f-std::exp(-deltaMs/30.f),1.f-std::exp(-deltaMs/50.f),settings.debugMask?1.f:0.f};
  cmd->SetComputeRoot32BitConstants(1,24,&constants,0);cmd->SetComputeRootConstantBufferView(2,s.pipeline->control.Address());
  Effects::Barrier(cmd,storage->guide->texture.Get(),read,write);cmd->SetPipelineState(s.pipeline->capture.Get());cmd->Dispatch(capture?80:20,capture?80:20,1);
  Effects::Barrier(cmd,storage->guide->texture.Get(),write,read);
  if(capture){Effects::Barrier(cmd,storage->input.Get(),write,D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyBufferRegion(storage->readback.Get(),0,storage->input.Get(),0,3*640*640*4);Effects::Barrier(cmd,storage->input.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,write);}
  Effects::Barrier(cmd,storage->guide->mask.Get(),read,write);cmd->SetPipelineState(s.pipeline->warp.Get());cmd->Dispatch(20,20,1);Effects::Barrier(cmd,storage->guide->mask.Get(),write,read);
  if(lease->diagnosticCaptured){
   Effects::Barrier(cmd,storage->guide->mask.Get(),read,D3D12_RESOURCE_STATE_COPY_SOURCE);
   D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=storage->guide->mask.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   dst.pResource=storage->maskReadback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=storage->maskFootprint;
   cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Effects::Barrier(cmd,storage->guide->mask.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,read);
  }
  Effects::Barrier(cmd,storage->output.Get(),read,write);cmd->SetPipelineState(s.pipeline->compose.Get());cmd->Dispatch((width+7)/8,(height+7)/8,1);Effects::Barrier(cmd,storage->output.Get(),write,read);
  Effects::Barrier(cmd,original,read,originalState);Effects::Barrier(cmd,guides.motion,read,guides.motionState);Effects::Barrier(cmd,guides.depth,read,guides.depthState);
  s.status=accepted?"Person protection active":"Person partition: waiting for a current mask";
  return storage->output.Get();
 }catch(const std::exception&e){s.Invalidate(ResetReason::Error);s.status=e.what();return final;}catch(...){s.Invalidate(ResetReason::Error);s.status="person preparation failed";return final;}
}
}

