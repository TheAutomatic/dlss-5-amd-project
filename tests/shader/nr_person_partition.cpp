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
 auto record=[&](std::shared_ptr<Person::Mask>mask={}){
  auto r=NewRecording(d.Get());r.output=Person::Record(r.proxy.Get(),base.Get(),first.Get(),final.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,w,h,guides,mask);
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
 Ptr<ID3D12InfoQueue>info;if(SUCCEEDED(d.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<char>data(bytes);auto*m=reinterpret_cast<D3D12_MESSAGE*>(data.data());info->GetMessage(i,m,&bytes);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)std::fprintf(stderr,"%s\n",m->pDescription);Require(m->Severity>D3D12_MESSAGE_SEVERITY_ERROR,"D3D12 debug");}
 puts("person partition: PASS (first/final, alpha, stale/replay, disocclusion, closed-list lifetime)");
}

