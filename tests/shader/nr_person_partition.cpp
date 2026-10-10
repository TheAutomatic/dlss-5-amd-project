#include "nr_effects_float_utils.h"
#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person/PersonPartition.h"
namespace Person = DlssNr::Person;
static void UploadGuide(ID3D12Device*d,ID3D12CommandQueue*q,ID3D12Resource*r,float value,unsigned channels){
 auto desc=r->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;d->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
 auto upload=Person::Buffer(d,bytes,D3D12_HEAP_TYPE_UPLOAD);void*ptr=nullptr;D3D12_RANGE empty{};Check(upload->Map(0,&empty,&ptr),"guide map");
 for(unsigned y=0;y<desc.Height;++y){auto*row=reinterpret_cast<float*>(static_cast<char*>(ptr)+y*fp.Footprint.RowPitch);for(unsigned x=0;x<desc.Width*channels;++x)row[x]=value;}
 upload->Unmap(0,nullptr);Ptr<ID3D12CommandAllocator>a;Ptr<ID3D12GraphicsCommandList>c;
 Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"alloc");Check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&c)),"list");
 Effects::Barrier(c.Get(),r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
 D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=r;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
 c->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Effects::Barrier(c.Get(),r,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Check(c->Close(),"close");ID3D12CommandList*lists[]={c.Get()};q->ExecuteCommandLists(1,lists);WaitQueue(d,q);
}
int main(){
 for(auto entry:{"capture_main","warp_main","compose_main"}){
  Ptr<ID3DBlob>blob,error;auto hr=NativeCompileShaderBlob(Person::Shader,strlen(Person::Shader),"person test",nullptr,nullptr,entry,&blob,&error);
  if(FAILED(hr)&&error)std::fprintf(stderr,"%s\n",static_cast<const char*>(error->GetBufferPointer()));Check(hr,entry);
 }
 Ptr<ID3D12Debug>debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
 Ptr<IDXGIFactory4>factory;Ptr<IDXGIAdapter>adapter;Ptr<ID3D12Device>d;
 Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");SelectEffectsAdapter(factory.Get(),&adapter);Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)),"device");
 Ptr<ID3D12CommandQueue>q,other;D3D12_COMMAND_QUEUE_DESC qd{};Check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)),"queue");Check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&other)),"other");
 constexpr unsigned w=32,h=24;
 auto base=FloatTexture(d.Get(),w,h),first=FloatTexture(d.Get(),w,h),final=FloatTexture(d.Get(),w,h);
 auto depth=Person::Texture(d.Get(),w,h,DXGI_FORMAT_R32_FLOAT),motion=Person::Texture(d.Get(),w,h,DXGI_FORMAT_R32G32_FLOAT);
 std::vector<Pixel>pixels(w*h,Pixel{.2f,.2f,.2f,.37f});Transfer(d.Get(),q.Get(),base.Get(),&pixels);
 for(auto&v:pixels)v={.3f,.3f,.3f,.9f};Transfer(d.Get(),q.Get(),first.Get(),&pixels);
 for(auto&v:pixels)v={.7f,.7f,.7f,.9f};Transfer(d.Get(),q.Get(),final.Get(),&pixels);
 UploadGuide(d.Get(),q.Get(),depth.Get(),.5f,1);UploadGuide(d.Get(),q.Get(),motion.Get(),0,2);
 Effects::Guides guides{motion.Get(),depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,w,h};
 auto record=[&](std::shared_ptr<Person::Mask>mask={},Person::Settings settings={},bool single=false){
  auto r=NewRecording(d.Get());r.output=Person::Record(r.proxy.Get(),base.Get(),single?final.Get():first.Get(),final.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,w,h,guides,mask,settings);
  Require(r.output!=final.Get(),"person recording prepared");Check(r.proxy->Close(),"close");return r;
 };
 auto check=[&](Recording&r,ID3D12CommandQueue*queue,float left,float right){
  Check(r.proxy->ExecuteOn(queue),"execute");WaitQueue(d.Get(),queue);auto out=Transfer(d.Get(),queue,r.output);
  if(std::abs(out[h/2*w+w/4][0]-left)>=.002f||std::abs(out[h/2*w+3*w/4][0]-right)>=.002f)printf("got %f %f expected %f %f\n",out[h/2*w+w/4][0],out[h/2*w+3*w/4][0],left,right);
  Require(std::abs(out[h/2*w+w/4][0]-left)<.002f&&std::abs(out[h/2*w+3*w/4][0]-right)<.002f,"person/background composition");
  for(auto v:out)Require(std::abs(v[3]-.37f)<.001f,"original alpha");
 };
 auto seed=record();check(seed,q.Get(),.7f,.7f);
 auto mask=std::make_shared<Person::Mask>();mask->epoch=Person::Global().epoch;mask->frame=Person::Global().frame;mask->tick=GetTickCount64();mask->width=w;mask->height=h;mask->values.resize(160*160);
 for(unsigned y=0;y<160;++y)for(unsigned x=0;x<80;++x)mask->values[y*160+x]=1;
 auto active=record(mask);check(active,q.Get(),.3f,.7f);
 check(active,other.Get(),.7f,.7f); // Replay cannot paste old semantic content.
 auto stale=record(mask);check(stale,q.Get(),.7f,.7f);
 mask->epoch=Person::Global().epoch;mask->frame=Person::Global().frame;mask->tick=GetTickCount64();
 UploadGuide(d.Get(),q.Get(),depth.Get(),.9f,1);
 auto disoccluded=record(mask);check(disoccluded,q.Get(),.7f,.7f);
 auto closed=record();Person::Reset();check(closed,q.Get(),.7f,.7f);
 seed.proxy.Reset();active.proxy.Reset();stale.proxy.Reset();disoccluded.proxy.Reset();closed.proxy.Reset();Person::Reset();
 Require(Person::Global().leases.empty(),"retired resources released");
 // A two-frame-old mask must follow both intervening backward motion fields.
 // The object moves right four pixels per frame; y motion stays zero.
 UploadGuide(d.Get(),q.Get(),depth.Get(),.5f,1);
 UploadGuide(d.Get(),q.Get(),motion.Get(),-4,2);guides.motionScaleY=0;
 auto motionSeed=record();check(motionSeed,q.Get(),.7f,.7f);
 mask->epoch=Person::Global().epoch;mask->frame=Person::Global().frame;mask->tick=GetTickCount64();
 auto between=record();check(between,q.Get(),.7f,.7f);
 mask->tick=GetTickCount64();
 auto moving=record(mask);Check(moving.proxy->ExecuteOn(q.Get()),"moving execute");WaitQueue(d.Get(),q.Get());
 auto moved=Transfer(d.Get(),q.Get(),moving.output);
 Require(std::abs(moved[h/2*w+20][0]-.3f)<.002f,"two-frame mask follows object");
 Require(std::abs(moved[h/2*w+4][0]-.7f)<.002f,"offscreen reprojection rejects mask");
 // Time expiry and a camera reset must each reject an otherwise matching mask.
 mask->tick=GetTickCount64()-251;
 auto expired=record(mask);check(expired,q.Get(),.7f,.7f);
 mask->tick=GetTickCount64();guides.reset=true;
 auto reset=record(mask);check(reset,q.Get(),.7f,.7f);guides.reset=false;
 motionSeed.proxy.Reset();between.proxy.Reset();moving.proxy.Reset();expired.proxy.Reset();reset.proxy.Reset();Person::Reset();
 Require(Person::Global().leases.empty(),"motion test resources retired");
 // The same host compositor handles both backends without additional network work.
 UploadGuide(d.Get(),q.Get(),motion.Get(),0,2);guides.motionScaleY=1;
 auto fresh=[&]{
  auto m=std::make_shared<Person::Mask>(*mask);m->epoch=Person::Global().epoch;
  m->frame=Person::Global().frame;m->tick=GetTickCount64();return m;
 };
 {auto r=record();check(r,q.Get(),.7f,.7f);}
 {auto r=record(fresh(),{0,1});check(r,q.Get(),.2f,.7f);}
 {auto r=record(fresh(),{.5f,1});check(r,q.Get(),.25f,.7f);}
 {auto r=record(fresh(),{1,0});check(r,q.Get(),.3f,.7f);} // Constant correction survives detail suppression.
 {auto r=record(fresh(),{.5f,1},true);check(r,q.Get(),.45f,.7f);}
 {auto r=record(fresh(),{},true);check(r,q.Get(),.7f,.7f);}
 // A mask can expire between recording and submission while the recording itself
 // is still young. Its original capture time, not the record time, is the limit.
 {auto m=fresh();m->tick-=225;auto r=record(m);Sleep(40);check(r,q.Get(),.7f,.7f);}
 Person::Reset();
 // Suppress an invented fine impulse on the person; do not modify the scene.
 pixels.assign(w*h,Pixel{.3f,.3f,.3f,.9f});pixels[h/2*w+w/4]={1,1,1,.9f};
 Transfer(d.Get(),q.Get(),first.Get(),&pixels);
 {auto r=record();check(r,q.Get(),.7f,.7f);}
 {auto r=record(fresh(),{1,0});Check(r.proxy->ExecuteOn(q.Get()),"detail execute");WaitQueue(d.Get(),q.Get());
  auto out=Transfer(d.Get(),q.Get(),r.output);
  Require(out[h/2*w+w/4][0]<.6f&&out[h/2*w+w/4][0]>.3f,"invented person detail reduced");
  Require(std::abs(out[h/2*w+3*w/4][0]-.7f)<.002f,"scene detail unchanged");
 }
 Person::Reset();
 // HDR strength interpolation must not clip to SDR or modify alpha.
 pixels.assign(w*h,Pixel{4,4,4,.37f});Transfer(d.Get(),q.Get(),base.Get(),&pixels);
 pixels.assign(w*h,Pixel{8,8,8,.9f});Transfer(d.Get(),q.Get(),first.Get(),&pixels);
 pixels.assign(w*h,Pixel{16,16,16,.9f});Transfer(d.Get(),q.Get(),final.Get(),&pixels);
 {auto r=record();check(r,q.Get(),16,16);}
 {auto r=record(fresh(),{.5f,1});check(r,q.Get(),6,16);}
 Person::Reset();
 Require(Person::Global().leases.empty(),"person controls resources retired");
 // Mask refresh pulses must be smoothed on a continuous surface, while an
 // expired source must still bypass. Exercise actual submitted GPU history.
 pixels.assign(w*h,Pixel{.2f,.2f,.2f,.37f});Transfer(d.Get(),q.Get(),base.Get(),&pixels);
 pixels.assign(w*h,Pixel{.3f,.3f,.3f,.9f});Transfer(d.Get(),q.Get(),first.Get(),&pixels);
 pixels.assign(w*h,Pixel{.7f,.7f,.7f,.9f});Transfer(d.Get(),q.Get(),final.Get(),&pixels);
 {auto r=record();check(r,q.Get(),.7f,.7f);}
 {auto r=record(fresh());check(r,q.Get(),.3f,.7f);}
 {
  auto empty=fresh();std::fill(empty->values.begin(),empty->values.end(),0.f);
  auto r=record(empty);Check(r.proxy->ExecuteOn(q.Get()),"mask refresh execute");WaitQueue(d.Get(),q.Get());
  auto out=Transfer(d.Get(),q.Get(),r.output);const float value=out[h/2*w+w/4][0];
  Require(value>.3f&&value<.699f,"refresh pulse reduced without freezing the mask");
 }
 Person::Reset();
 {auto r=record();check(r,q.Get(),.7f,.7f);}
 auto tooOld=fresh();
 for(unsigned i=0;i<Person::HistoryCount;++i){auto r=record();check(r,q.Get(),.7f,.7f);}
 tooOld->tick=GetTickCount64(); // Time still valid; intervening guide count is not.
 {auto r=record(tooOld);check(r,q.Get(),.7f,.7f);}
 Person::Reset();
 Require(Person::Global().leases.empty(),"temporal mask history resources retired");
 // Overlay diagnostics read only completed recordings and do not extend their
 // lifetime. The half-image fixture is present both before and after warping.
 {auto r=record();check(r,q.Get(),.7f,.7f);}
 {
  auto r=record(fresh(),{1,1,true});Check(r.proxy->ExecuteOn(q.Get()),"diagnostic execute");WaitQueue(d.Get(),q.Get());
  Person::Poll();auto& diagnostic=Person::Global().diagnostic;
  Require(diagnostic.rawCoverage.count==1&&diagnostic.rawCoverage.Mean()==5000,"raw diagnostic coverage");
  Require(diagnostic.warpedCoverage.count==1&&diagnostic.warpedCoverage.Mean()==5000,"GPU diagnostic coverage");
  Require(diagnostic.warpValid.count==1&&diagnostic.warpValid.Mean()==10000,"GPU diagnostic validity");
  Person::Poll();Require(diagnostic.warpedCoverage.count==1,"diagnostic sample collected once");
 }
 Person::Reset();Require(Person::Global().leases.empty(),"diagnostic resources retired");
 // Hold the GPU behind a fence: four recordings may capture without waiting
 // for any prior result. On release, only the newest completed image goes to
 // the real isolated IPC provider; older images cannot form a stale queue.
 wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
 auto fixture=std::filesystem::path(executable).parent_path()/L"person-capture-worker";
 Person::Worker().Configure(true,fixture);
 auto until=[&](auto ready){auto start=GetTickCount64();while(!ready()){
  Require(GetTickCount64()-start<5000,"capture worker timeout");Sleep(1);}};
 until([&]{return Person::Worker().Ready();});
 Ptr<ID3D12Fence>gate;Check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"capture gate");
 struct Unblock{ID3D12Fence* fence;~Unblock(){fence->Signal(1);}} unblock{gate.Get()};
 Check(q->Wait(gate.Get(),1),"hold capture GPU");
 std::vector<Recording>pending;
 uint64_t newestCapture=0;
 for(unsigned i=0;i<5;++i){
  if(i)Sleep(64); // Leave margin for GetTickCount64's ~16 ms quantization.
  pending.push_back(record());
  Check(pending.back().proxy->ExecuteOn(q.Get()),"pipelined capture execute");
  if(i<4)newestCapture=Person::Global().frame;
 }
 auto pendingCount=std::count_if(Person::Global().leases.begin(),Person::Global().leases.end(),[](auto& l){return !l->sent&&l->storage->readback;});
 Require(pendingCount==4,"four pending GPU captures, fifth bounded");
 Check(gate->Signal(1),"release captures");WaitQueue(d.Get(),q.Get());Person::Poll();
 until([&]{return Person::Worker().Latest()!=nullptr;});
 Require(Person::Worker().Latest()->frame==newestCapture,"latest completed GPU capture wins");
 pending.clear();Person::Worker().Configure(false,fixture);Person::Reset();
 Require(Person::Global().leases.empty(),"pipeline captures released after fence");
 Ptr<ID3D12InfoQueue>info;if(SUCCEEDED(d.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<char>data(bytes);auto*m=reinterpret_cast<D3D12_MESSAGE*>(data.data());info->GetMessage(i,m,&bytes);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)std::fprintf(stderr,"%s\n",m->pDescription);Require(m->Severity>D3D12_MESSAGE_SEVERITY_ERROR,"D3D12 debug");}
 puts("person partition: PASS (first/final, alpha, two-frame motion, time/reset rejection, stale/replay, disocclusion, closed-list lifetime)");
}

