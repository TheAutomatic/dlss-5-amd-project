#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <chrono>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfEvaluateCut.h"

using namespace DlssNr;
using namespace std::chrono_literals;
static void Require(bool ok, const char *message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}

// Minimal CPU COM services used by BindClosedProducer/Reset/Release. No D3D12
// device is created; calling a GPU or an unexpected COM operation is an error.
struct SyntheticCom { void **vtable; ULONG refs=1; };
static ULONG STDMETHODCALLTYPE AddRef(IUnknown *object)
{ return ++reinterpret_cast<SyntheticCom *>(object)->refs; }
static ULONG STDMETHODCALLTYPE Release(IUnknown *object)
{ return --reinterpret_cast<SyntheticCom *>(object)->refs; }
static HRESULT resetResult=E_FAIL,closeResult=E_FAIL;
static unsigned resetExecuteCalls=0;
static HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList *) { return closeResult; }
static HRESULT STDMETHODCALLTYPE Reset(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *)
{ return resetResult; }
static void WINAPI ExecuteResetGeneration(ID3D12CommandQueue *,UINT count,ID3D12CommandList *const *)
{ Require(count==1,"reset generation preserves batch"); ++resetExecuteCalls; }
static void GenerationCredentials()
{
    void *unknownVtable[3] {nullptr, reinterpret_cast<void *>(&AddRef), reinterpret_cast<void *>(&Release)};
    void *listVtable[11] {};
    listVtable[1]=reinterpret_cast<void *>(&AddRef);listVtable[2]=reinterpret_cast<void *>(&Release);
    listVtable[9]=reinterpret_cast<void *>(&Close);
    listVtable[10]=reinterpret_cast<void *>(&Reset);
    SyntheticCom device{unknownVtable}, allocator{unknownVtable}, native{listVtable};
    Submission::CommandListProxy *proxy=nullptr;
    Require(SUCCEEDED(Submission::CommandListProxy::CreateClosed(
        reinterpret_cast<ID3D12Device *>(&device),reinterpret_cast<ID3D12GraphicsCommandList *>(&native),&proxy)),
        "construct CPU proxy");
    const auto initial=proxy->RecordingGeneration();
    Require(initial.token && !initial.Discarded(), "live generation credential");
    Backend::JobLifecycle life;
    int job;
    Require(life.BeginRecord() && life.Publish(&job,proxy,initial)!=0,"bind job to actual proxy generation");
    life.EndRecord();
    resetResult=E_FAIL;
    Require(FAILED(proxy->Reset(reinterpret_cast<ID3D12CommandAllocator *>(&allocator),nullptr)), "native Reset failure retained");
    Require(!initial.Discarded(), "failed proxy Reset cannot authorize cancellation");
    Require(FAILED(proxy->ExecuteOnWithBetween(nullptr,nullptr,nullptr)) &&
            !proxy->LastExecuteSubmittedProducer(), "invalid Execute provides no-producer credential");
    resetResult=S_OK;
    Require(SUCCEEDED(proxy->Reset(reinterpret_cast<ID3D12CommandAllocator *>(&allocator),nullptr)), "successful proxy Reset");
    Require(initial.Discarded(), "successful proxy Reset invalidates old recording");
    const auto current=proxy->RecordingGeneration();
    Require(!current.Discarded() && current.generation!=initial.generation, "new recording has distinct generation");
    Require(FAILED(proxy->ExecuteOnWithBetween(reinterpret_cast<ID3D12CommandQueue *>(uintptr_t(0x1234)),nullptr,nullptr)) &&
            !proxy->LastExecuteSubmittedProducer(), "Close failure provides no-producer credential before any GPU call");
    Require(!life.BeginSubmission(*life.Active()),"new recording cannot claim old generation at same proxy address");
    closeResult=S_OK;
    Require(SUCCEEDED(proxy->Close()),"close new unsplit recording");
    ID3D12CommandList *batch[]{proxy};
    Submission::Hooks::ExecuteExpanded(reinterpret_cast<ID3D12CommandQueue *>(uintptr_t(0x1234)),1,batch,
                                      nullptr,nullptr,&ExecuteResetGeneration);
    Require(resetExecuteCalls==1 && life.Current()==Backend::JobLifecycle::Phase::Armed,
            "new unsplit submission leaves discarded old job cancellable");
    const auto cancelled=life.BeginCancelIfArmed(proxy);
    Require(cancelled && life.EndOperation(*cancelled) && life.BeginRecord(),
            "NR resumes after cancel of discarded generation");
    life.EndRecord();
    Require(proxy->Release()==0, "final release destroys proxy");
    Require(current.Discarded(), "credential outlives proxy and detects final release");
    Require(device.refs==1 && allocator.refs==1 && native.refs==1, "proxy released synthetic references");
    const Submission::ListGenerationSnapshot absent;
    Require(!absent.Discarded(), "absent tracking never authorizes cancellation");
}

static std::promise<void> entered, resume;
static std::future<void> resumeFuture;
static int32_t BlockingEnqueue(void *,void *,void *)
{
    entered.set_value();
    Require(resumeFuture.wait_for(5s)==std::future_status::ready,"enqueue released");
    resumeFuture.get();return 0;
}
static void ActualBetweenAndCleanup()
{
    using namespace Backend;
    auto lifetime=std::make_shared<JobLifecycle>();
    auto &life=*lifetime;
    int session,job;
    auto *list=reinterpret_cast<ID3D12CommandList *>(uintptr_t(0x1000));
    Require(life.BeginRecord(), "record admitted");
    const auto token=life.Publish(&job,list);life.EndRecord();
    Require(life.BeginSubmission(*life.Active()), "batch claims producer ownership before actual Between");
    LmxxfCut::SetPendingEnqueue(&session,&job,&BlockingEnqueue,nullptr,list,nullptr,lifetime,token);
    auto enteredFuture=entered.get_future();resumeFuture=resume.get_future();
    auto enqueue=std::async(std::launch::async,[&]{LmxxfCut::BetweenThunk(nullptr,list,nullptr);});
    Require(enteredFuture.wait_for(5s)==std::future_status::ready,"Between entered actual callback");
    enteredFuture.get();
    Require(life.Current()==JobLifecycle::Phase::Enqueueing,"actual Between publishes Enqueueing");
    for(int i=0;i<32;++i)
        Require(!life.BeginRecord() && !life.BeginCancelIfArmed(list) && !life.BeginCompletion(list),"running callback owns the job");
    resume.set_value();
    Require(enqueue.wait_for(5s)==std::future_status::ready,"Between returns");enqueue.get();
    Require(life.Current()==JobLifecycle::Phase::EnqueueCompleted,"actual Between publishes completion only after return");
    Require(!life.BeginRecord(),"consumer submission still required");
    const auto retiring=life.BeginCompletion(list);
    Require(retiring.has_value(),"outer submission claims retirement");
    LmxxfCut::ClearPendingEnqueue(token);
    Require(!life.BeginRecord(),"Cut cleanup alone cannot release ownership");
    Require(life.EndOperation(*retiring),"retirement releases ownership last");
    Require(life.BeginRecord(),"next recording admitted");
    const auto next=life.Publish(&job,list);life.EndRecord();
    LmxxfCut::SetPendingEnqueue(&session,&job,&BlockingEnqueue,nullptr,list,nullptr,lifetime,next);
    LmxxfCut::ClearPendingEnqueue(token);
    { auto &p=LmxxfCut::Pending();std::lock_guard lock(p.mutex);
      Require(p.token==next && p.job==&job,"late cleanup cannot clear reused addresses"); }
    const auto cancel=life.BeginCancelIfArmed(list);
    Require(cancel.has_value(),"discard can cancel new armed generation");
    LmxxfCut::ClearPendingEnqueue(next);Require(life.EndOperation(*cancel),"cancel cleanup releases ownership");
}

static void CallbackKeepsOwnershipAlive()
{
    using namespace Backend;
    entered=std::promise<void>{};resume=std::promise<void>{};
    auto enteredFuture=entered.get_future();resumeFuture=resume.get_future();
    auto owner=std::make_shared<JobLifecycle>();std::weak_ptr<JobLifecycle> weak=owner;
    int session,job;auto *list=reinterpret_cast<ID3D12CommandList *>(uintptr_t(0x1000));
    Require(owner->BeginRecord(),"lifetime test Record");
    const auto token=owner->Publish(&job,list);owner->EndRecord();
    LmxxfCut::SetPendingEnqueue(&session,&job,&BlockingEnqueue,nullptr,list,nullptr,owner,token);
    auto enqueue=std::async(std::launch::async,[&]{LmxxfCut::BetweenThunk(nullptr,list,nullptr);});
    Require(enteredFuture.wait_for(5s)==std::future_status::ready,"lifetime callback entered");enteredFuture.get();
    owner.reset();
    // Even if all persistent owners go away, the executing callback keeps its
    // lifecycle alive through EndEnqueue. It never follows a backend raw pointer.
    LmxxfCut::ClearPendingEnqueue(token);
    Require(!weak.expired(),"executing callback holds shared lifecycle");
    resume.set_value();Require(enqueue.wait_for(5s)==std::future_status::ready,"lifetime callback returned");enqueue.get();
    Require(weak.expired(),"last callback releases shared lifecycle after completion");
}

static unsigned rawCalls=0;
static void WINAPI RawExecute(ID3D12CommandQueue *,UINT,ID3D12CommandList *const *) { ++rawCalls; }
static void InternalExecuteReentry()
{
    auto &hooks=Submission::Hooks::g_armed;
    hooks.store(true);
    Submission::Hooks::o_ExecuteCommandLists=&RawExecute;
    {
        Submission::LogicalExecuteScope outer;
        // Holding the real expand mutex makes accidental re-expansion deadlock;
        // the TLS guard must invoke the raw callback directly on this thread.
        std::lock_guard held(Submission::Hooks::g_executeMu);
        Submission::Hooks::hkExecuteCommandLists(nullptr,0,nullptr);
    }
    Require(rawCalls==1,"internal Execute bypasses expand and callback dispatch");
    hooks.store(false);Submission::Hooks::o_ExecuteCommandLists=nullptr;
}
int main()
{
    GenerationCredentials();ActualBetweenAndCleanup();CallbackKeepsOwnershipAlive();InternalExecuteReentry();
    std::puts("lmxxf submission lifecycle: PASS (CPU proxy Reset/release, actual Between, tokens, internal Execute)");
}
