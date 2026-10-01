#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <memory>
#include <vector>
#include <thread>
#include "queue_faults.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfEvaluateCut.h"

template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
static void Require(bool ok,const char* text){if(!ok){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
static void Check(HRESULT hr,const char* text){Require(SUCCEEDED(hr),text);}
namespace Submission=DlssNr::Submission;
struct Observer:Submission::RecordingObserver{
 unsigned invalidations=0,producers=0,between=0,continuations=0;
 bool reject=false;
 Submission::RecordingIdentity invalidated{};
 std::vector<Submission::RecordingExecution> executions;
 HRESULT BeforeExecute(const Submission::RecordingExecution&)noexcept override{return reject?E_FAIL:S_OK;}
 void ProducerSubmitted(const Submission::RecordingExecution& e)noexcept override{
  Require(e.producerSubmitted&&!e.continuationSubmitted,"producer event describes actual partial submission");++producers;
 }
 void Between(const Submission::RecordingExecution& e)noexcept override{
  Require(e.producerSubmitted&&!e.continuationSubmitted,"between runs after producer and before continuation");++between;
 }
 void Executed(const Submission::RecordingExecution& e)noexcept override{
  executions.push_back(e);if(e.continuationSubmitted)++continuations;
 }
 void Invalidated(Submission::RecordingIdentity id)noexcept override{++invalidations;invalidated=id;}
};
struct GatedObserver final:Observer {
 HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 HANDLE proceed=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 ~GatedObserver(){CloseHandle(entered);CloseHandle(proceed);}
 HRESULT BeforeExecute(const Submission::RecordingExecution&)noexcept override{
  Require(entered&&proceed,"race events");SetEvent(entered);
  Require(WaitForSingleObject(proceed,5000)==WAIT_OBJECT_0,"release race gate");return S_OK;
 }
};
static void WaitIdle(ID3D12Device* device,ID3D12CommandQueue* queue){
 Ptr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
 Check(queue->Signal(fence.Get(),1),"completion signal");
 HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"event");
 Check(fence->SetEventOnCompletion(1,event),"event registration");
 Require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"completion wait");
 Require(fence->GetCompletedValue()!=UINT64_MAX&&SUCCEEDED(device->GetDeviceRemovedReason()),"actual completion");
 CloseHandle(event);
}

int main(){
 Ptr<IDXGIFactory4> factory;Ptr<IDXGIAdapter> warp;Ptr<ID3D12Device> device;
 Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
 Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP");
 Check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
 Ptr<ID3D12CommandAllocator> alloc;Ptr<ID3D12GraphicsCommandList> native;
 Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)),"allocator");
 Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&native)),"native list");
 DlssNr::Submission::CommandListProxy* proxy=nullptr;
 Check(DlssNr::Submission::CommandListProxy::Create(device.Get(),alloc.Get(),native.Get(),&proxy),"proxy");
 auto first=std::make_shared<Observer>();auto identity=proxy->Identity();
 Check(proxy->ObserveRecording(first),"observe first generation");
 auto resourceOwner=std::make_shared<Observer>();
 Check(proxy->ObserveResources(resourceOwner),"attach independent resources");
 Require(FAILED(proxy->ObserveRecording(std::make_shared<Observer>())),"second NR job still rejected");
 Require(FAILED(proxy->Reset(alloc.Get(),nullptr)),"Reset while open fails");
 Require(first->invalidations==0,"failed Reset retains recording");
 Check(proxy->Close(),"close");
 Require(first->invalidations==0,"Close does not invalidate");
 Check(proxy->Reset(alloc.Get(),nullptr),"successful Reset");
 Require(first->invalidations==1&&first->invalidated==identity,"successful real proxy Reset invalidates exact old identity");
 Require(resourceOwner->invalidations==1&&resourceOwner->invalidated==identity,"resource owner invalidates with same generation");
 Require(proxy->Identity().list==identity.list&&proxy->Identity().generation!=identity.generation,"stable list ID with new generation");
 auto second=std::make_shared<Observer>();identity=proxy->Identity();
 Check(proxy->ObserveRecording(second),"observe second generation");
 auto shared=std::make_shared<Observer>();Check(proxy->ObserveResources(shared),"observe shared output resources");
 Check(proxy->SplitSegments(),"split second generation");Check(proxy->Close(),"close second generation");
 Ptr<ID3D12CommandQueue> queue,otherQueue;D3D12_COMMAND_QUEUE_DESC desc{};
 Check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"queue");
 Check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&otherQueue)),"second queue");
 for(unsigned i=0;i<120;++i)Require(second->executions.empty()&&!second->invalidations,"delayed live recording stays pending");
 second->reject=true;
 Require(FAILED(proxy->ExecuteOn(queue.Get())),"pre-submission refusal reported");
 Require(second->executions.size()==1&&!second->executions[0].producerSubmitted&&!second->executions[0].continuationSubmitted,"failed execution cannot manufacture Submitted");
 second->reject=false;
 Check(proxy->ExecuteOn(queue.Get()),"first real execute");
 Check(proxy->ExecuteOn(otherQueue.Get()),"cross-queue closed-list replay");
 Check(proxy->ExecuteOn(queue.Get()),"same generation re-execute");
 WaitIdle(device.Get(),queue.Get());
 Require(second->producers==3&&second->between==3&&second->continuations==3,"every real replay reports all actual stages");
 Require(shared->producers==3&&shared->between==3&&shared->continuations==3&&shared->executions.size()==second->executions.size(),"independent owners receive the same actual execution facts");
 Require(second->invalidations==0,"submission does not invalidate live recording");
 for(size_t i=1;i<second->executions.size();++i){
  const auto& e=second->executions[i];
  Require(e.identity==identity&&e.serial>second->executions[i-1].serial&&SUCCEEDED(e.status)&&e.fence&&e.fenceValue,"execution serial and completion credential");
 }
 proxy->Release();
 Require(second->invalidations==1&&second->invalidated==identity,"final COM Release invalidates once without a reference cycle");
 // Another proxy must not reuse the stable ID even if its allocation address is recycled.
 DlssNr::Submission::CommandListProxy* replacement=nullptr;
 Check(DlssNr::Submission::CommandListProxy::Create(device.Get(),alloc.Get(),native.Get(),&replacement),"replacement proxy");
 Require(replacement->Identity().list!=identity.list,"new proxy has distinct stable identity");replacement->Release();
 // Reset racing an actual execution cannot revoke a generation between producer
 // and consumer, and cannot release its observer while the execute callback runs.
 Ptr<ID3D12GraphicsCommandList> raceNative;
 Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&raceNative)),"race native");
 Check(Submission::CommandListProxy::Create(device.Get(),alloc.Get(),raceNative.Get(),&proxy),"race proxy");
 auto gated=std::make_shared<GatedObserver>();Check(proxy->ObserveRecording(gated),"race observer");
 Check(proxy->SplitSegments(),"race split");Check(proxy->Close(),"race close");
 HANDLE attempted=CreateEventW(nullptr,TRUE,FALSE,nullptr),resetDone=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 HRESULT executeHr=E_PENDING,resetHr=E_PENDING;
 std::thread executeThread([&]{executeHr=proxy->ExecuteOn(queue.Get());});
 Require(WaitForSingleObject(gated->entered,5000)==WAIT_OBJECT_0,"execute entered callback");
 std::thread resetThread([&]{SetEvent(attempted);resetHr=proxy->Reset(alloc.Get(),nullptr);SetEvent(resetDone);});
 Require(WaitForSingleObject(attempted,5000)==WAIT_OBJECT_0,"Reset attempted concurrently");
 Require(WaitForSingleObject(resetDone,30)==WAIT_TIMEOUT,"Reset cannot cross active execution");
 SetEvent(gated->proceed);executeThread.join();resetThread.join();
 Check(executeHr,"racing execute");Check(resetHr,"racing reset");WaitIdle(device.Get(),queue.Get());
 Require(gated->executions.size()==1&&gated->executions[0].continuationSubmitted&&gated->invalidations==1,"execute completes before exact generation invalidation");
 CloseHandle(attempted);CloseHandle(resetDone);Check(proxy->Close(),"close reset race generation");proxy->Release();
 // Submission and Signal result are distinct facts. A failed completion marker
 // cannot be reclassified as "never executed", or repaired by a later replay.
 Ptr<ID3D12GraphicsCommandList> faultNative;
 Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&faultNative)),"fault native");
 Check(Submission::CommandListProxy::Create(device.Get(),alloc.Get(),faultNative.Get(),&proxy),"fault proxy");
 auto fault=std::make_shared<Observer>();Check(proxy->ObserveRecording(fault),"fault observer");
 auto faultResources=std::make_shared<Observer>();Check(proxy->ObserveResources(faultResources),"fault resources");
 Check(proxy->SplitSegments(),"fault split");Check(proxy->Close(),"fault close");
 auto* failingQueue=new SignalFailQueue(queue.Get());
 Require(FAILED(proxy->ExecuteOn(failingQueue)),"Signal failure returned");
 Require(fault->executions.size()==1&&fault->executions[0].producerSubmitted&&fault->executions[0].continuationSubmitted,"Signal failure retains actual submission facts");
 Require(FAILED(proxy->ExecuteOn(queue.Get())),"unconfirmed submission cannot be hidden by later fence");
 Require(fault->executions.size()==2&&!fault->executions[1].producerSubmitted,"no false later execution");
 WaitIdle(device.Get(),queue.Get());proxy->Release();failingQueue->Release();
 Require(fault->invalidations==1,"failed-signal recording still invalidates exactly once");
 Require(faultResources->invalidations==1&&faultResources->executions.size()==2&&FAILED(faultResources->executions[0].status),"failed Signal delivered to independent resource owner");
 std::puts("recording lifecycle: PASS");
}
