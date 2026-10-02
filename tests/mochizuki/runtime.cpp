#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <memory>
#include "dlssnr/backend/mochizuki_runtime/MochizukiNrApi.h"
using Microsoft::WRL::ComPtr;
void Require(bool yes, const char* text) { if (!yes) throw std::runtime_error(text); }
void Hr(HRESULT hr) { if (FAILED(hr)) { char b[64]; sprintf_s(b, "HRESULT 0x%08X", unsigned(hr)); throw std::runtime_error(b); } }
LmxxfNrApi api {};
void Rc(int rc) { if (rc) { char text[256] {}; api.GetLastError(text, sizeof text); throw std::runtime_error(text); } }
struct List {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    explicit List(ID3D12Device* d) {
        Hr(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        Hr(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&cmd)));
    }
};
void Submit(ID3D12CommandQueue* q, List& list) { ID3D12CommandList* lists[] = {list.cmd.Get()}; q->ExecuteCommandLists(1,lists); }
void Wait(ID3D12Fence* fence, UINT64 value) {
    HANDLE ev=CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(ev != nullptr,"event");
    Hr(fence->SetEventOnCompletion(value,ev)); DWORD rc=WaitForSingleObject(ev,30000); CloseHandle(ev);
    Require(rc==WAIT_OBJECT_0,"GPU timeout"); Require(fence->GetCompletedValue()!=UINT64_MAX,"device removed");
}
void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v {};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&v);
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device* d,UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp {}; hp.Type=heap;
    D3D12_RESOURCE_DESC desc {};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;Hr(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)));return r;
}
struct Frame {
    ComPtr<ID3D12Resource> color, upload, readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT width, height;
    DXGI_FORMAT format;
    Frame(ID3D12Device* d, UINT w, UINT h, DXGI_FORMAT fmt=DXGI_FORMAT_R16G16B16A16_FLOAT):width(w),height(h),format(fmt) {
        D3D12_HEAP_PROPERTIES hp {};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
        desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=fmt;
        Hr(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&color)));
        UINT64 bytes; d->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        upload=Buffer(d,bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        readback=Buffer(d,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        void* data;Hr(upload->Map(0,nullptr,&data));
        for(UINT y=0;y<h;y++)for(UINT x=0;x<w;x++) {
            float base=.05f+.7f*float(x)/w + ((x/8+y/8)%2)*.08f;
            auto* row=static_cast<char*>(data)+footprint.Footprint.RowPitch*y;
            if(fmt==DXGI_FORMAT_R9G9B9E5_SHAREDEXP)
                reinterpret_cast<DirectX::PackedVector::XMFLOAT3SE*>(row)[x]=DirectX::PackedVector::XMFLOAT3SE(base,base*.8f,base*.6f);
            else if(fmt==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                auto* p=reinterpret_cast<uint8_t*>(row)+x*4;
                for(int k=0;k<3;k++)p[k]=uint8_t(std::round(base*(1.f-.2f*k)*255));p[3]=255;
            } else {
                auto* p=reinterpret_cast<uint16_t*>(row)+x*4;
                for(int k=0;k<3;k++)p[k]=DirectX::PackedVector::XMConvertFloatToHalf(base*(1.f-.2f*k));p[3]=0x3c00;
            }
        }
        upload->Unmap(0,nullptr);
    }
    void Upload(ID3D12GraphicsCommandList* c) {
        D3D12_TEXTURE_COPY_LOCATION a {color.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
        D3D12_TEXTURE_COPY_LOCATION b {upload.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}};b.PlacedFootprint=footprint;
        c->CopyTextureRegion(&a,0,0,0,&b,nullptr);
        Barrier(c,color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    void Read(ID3D12GraphicsCommandList* c,ID3D12Resource* output) {
        Barrier(c,output,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION a {readback.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}};a.PlacedFootprint=footprint;
        D3D12_TEXTURE_COPY_LOCATION b {output,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
        c->CopyTextureRegion(&a,0,0,0,&b,nullptr);
        Barrier(c,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    void Check(UINT validWidth=0, UINT validHeight=0) {
        if(!validWidth)validWidth=width;if(!validHeight)validHeight=height;
        void* data;Hr(readback->Map(0,nullptr,&data));double sum=0,diff=0;
        for(UINT y=0;y<validHeight;y++)for(UINT x=0;x<validWidth;x++) {
            auto* row=static_cast<char*>(data)+footprint.Footprint.RowPitch*y;
            float v;
            if(format==DXGI_FORMAT_R9G9B9E5_SHAREDEXP)
                v=DirectX::XMVectorGetX(DirectX::PackedVector::XMLoadFloat3SE(reinterpret_cast<DirectX::PackedVector::XMFLOAT3SE*>(row)+x));
            else if(format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) v=reinterpret_cast<uint8_t*>(row)[x*4]/255.f;
            else v=DirectX::PackedVector::XMConvertHalfToFloat(reinterpret_cast<uint16_t*>(row)[x*4]);
            Require(std::isfinite(v)&&std::abs(v)<100,"non-finite or excessive output");sum+=v;
            float expected=.05f+.7f*float(x)/width+((x/8+y/8)%2)*.08f;diff+=std::abs(v-expected);
        }
        readback->Unmap(0,nullptr);
        printf("OUTPUT %ux%u format=%u mean=%f mean_abs_correction=%f\n",validWidth,validHeight,unsigned(format),sum/(validWidth*validHeight),diff/(validWidth*validHeight));
        Require(sum/(validWidth*validHeight)>.01,"black output");
        Require(diff/(validWidth*validHeight)>.0001,"network made no observable correction");
    }
};
int wmain(int argc,wchar_t**argv) try {
    Require(argc>=2,"runtime path required");
    HMODULE dll=LoadLibraryW(argv[1]);Require(dll!=nullptr,"runtime load failed");
    auto get=reinterpret_cast<PFN_MochizukiNrGetApi>(GetProcAddress(dll,"MochizukiNrGetApi"));Require(get!=nullptr,"get API export");
    api.struct_size=sizeof api;Require(get(1,&api)==LMXXF_NR_UNSUPPORTED_ABI,"old ABI accepted");
    api.struct_size=sizeof api;Rc(get(LMXXF_NR_ABI_VERSION,&api));
    Require(api.BeginRecordingExecution&&api.EndRecordingExecution&&api.CollectRecording,"missing v2 lifecycle");
    LmxxfNrCapabilities cap {sizeof cap};Rc(api.QueryCapabilities(&cap));
    Require(cap.history_supported==1,"history capability");
    LmxxfNrCreateInfo bad {sizeof bad};void*context=nullptr;
    Require(api.Create(&bad,&context)!=LMXXF_NR_OK,"invalid create accepted");
    MochizukiNrControls controls {sizeof controls};
    auto defaults=reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(dll,"MochizukiNrGetControlDefaults"));
    auto set=reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(dll,"MochizukiNrSetControls"));
    auto prepare=reinterpret_cast<PFN_MochizukiNrPrepareFrame>(GetProcAddress(dll,"MochizukiNrPrepareFrame"));
    auto getInfo=reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(dll,"MochizukiNrGetInfo"));
    Require(defaults&&set&&prepare&&getInfo,"missing controls/frame exports");Rc(defaults(&controls));
    Require(controls.intensity==1&&controls.preprocess==0,"control defaults");
    MochizukiNrControls oldControls {sizeof controls-4};
    Require(defaults(&oldControls)==LMXXF_NR_INVALID_ARGUMENT,"partial controls accepted");
    if(argc==2) {puts("MOCHIZUKI_ABI_OK");FreeLibrary(dll);return 0;}
    reinterpret_cast<void(*)(uint32_t)>(GetProcAddress(dll,"MochizukiNrSetLogging"))(1);
    ComPtr<IDXGIFactory6> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D12Device> device;
    for(UINT i=0;factory->EnumAdapters1(i,&adapter)==S_OK;i++,adapter.Reset()) {
        DXGI_ADAPTER_DESC1 desc;adapter->GetDesc1(&desc);
        if(desc.VendorId==0x1002&&!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)))) break;
    }
    Require(device!=nullptr,"AMD D3D12 adapter required");
    ComPtr<ID3D12CommandQueue> q[2]; D3D12_COMMAND_QUEUE_DESC qd {};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    for(auto& a:q)Hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&a)));
    ComPtr<ID3D12Fence> tail;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&tail)));uint64_t value=0;
    LmxxfNrCreateInfo ci {sizeof ci,device.Get(),q[0].Get(),argv[2],LMXXF_NR_CREATE_FLAG_RECORDING_LEASES};
    Rc(api.Create(&ci,&context));Rc(api.PrepareSession(context));Rc(set(context,&controls));
    Frame frame(device.Get(),256,256);List upload(device.Get());frame.Upload(upload.cmd.Get());Hr(upload.cmd->Close());Submit(q[0].Get(),upload);Hr(q[0]->Signal(tail.Get(),++value));Wait(tail.Get(),value);
    auto make=[&](Frame& f,UINT validWidth=0) {
        MochizukiNrFrameInfo info {};info.struct_size=sizeof info;info.color=f.color.Get();info.color_width=validWidth?validWidth:f.width;info.color_height=f.height;
        info.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;info.flags=LMXXF_NR_FRAME_FLAG_STRENGTH;info.transfer_strength=info.color_strength=info.model_scale=1;info.passes=1;
        LmxxfNrJob job {sizeof job};const auto deadline=GetTickCount64()+300000;
        for(;;) {int rc=prepare(context,&info,&job);if(!rc)break; if(rc!=LMXXF_NR_UNAVAILABLE||GetTickCount64()>deadline)Rc(rc);Sleep(100);}
        return job;
    };
    auto job=make(frame);List inputs(device.Get()),outputs(device.Get());Rc(api.RecordInputs(context,job.handle,inputs.cmd.Get()));Rc(api.RecordOutputs(context,job.handle,outputs.cmd.Get()));frame.Read(outputs.cmd.Get(),static_cast<ID3D12Resource*>(job.private_output));Hr(inputs.cmd->Close());Hr(outputs.cmd->Close());
    // A second Prepare must not invalidate the first closed recording.
    auto neverSubmitted=make(frame);Rc(api.InvalidateRecording(context,neverSubmitted.handle));Rc(api.CollectRecording(context,neverSubmitted.handle));
    Require(api.CollectRecording(context,neverSubmitted.handle)==LMXXF_NR_INVALID_ARGUMENT,"stale handle accepted");
    Require(api.Destroy(context)==LMXXF_NR_UNAVAILABLE,"live recording destroyed");
    for(int i=0;i<8;i++) {
        auto* queue=q[i%2].Get();Rc(api.BeginRecordingExecution(context,job.handle,queue));Submit(queue,inputs);
        Rc(api.EnqueueHip(context,job.handle,queue));Submit(queue,outputs);HRESULT hr=queue->Signal(tail.Get(),++value);
        Rc(api.EndRecordingExecution(context,job.handle,queue,3,tail.Get(),value,hr));Wait(tail.Get(),value);
    }
    frame.Check();
    MochizukiNrInfo timing {sizeof timing};Rc(getInfo(context,&timing));
    Require(timing.gpu_samples>0 && std::isfinite(timing.gpu_ms_last) && timing.gpu_ms_last>0,"missing completed network timing");
    const auto samples=timing.gpu_samples;Rc(getInfo(context,&timing));
    Require(timing.gpu_samples==samples,"status polling duplicated GPU samples");
    printf("TIMING samples=%llu last=%.3f ms repeated_poll=stable\n",static_cast<unsigned long long>(samples),timing.gpu_ms_last);
    // Switch geometry while the first executable recording still owns its network.
    Frame resized(device.Get(),320,256);List resizeUpload(device.Get());resized.Upload(resizeUpload.cmd.Get());Hr(resizeUpload.cmd->Close());Submit(q[0].Get(),resizeUpload);Hr(q[0]->Signal(tail.Get(),++value));Wait(tail.Get(),value);
    auto resizedJob=make(resized);List resizedIn(device.Get()),resizedOut(device.Get());
    Rc(api.RecordInputs(context,resizedJob.handle,resizedIn.cmd.Get()));Rc(api.RecordOutputs(context,resizedJob.handle,resizedOut.cmd.Get()));
    resized.Read(resizedOut.cmd.Get(),static_cast<ID3D12Resource*>(resizedJob.private_output));Hr(resizedIn.cmd->Close());Hr(resizedOut.cmd->Close());
    for(int i=0;i<6;i++) {
        const bool old=i%2!=0;auto token=old?job.handle:resizedJob.handle;auto* queue=q[i%2].Get();
        Rc(api.BeginRecordingExecution(context,token,queue));Submit(queue,old?inputs:resizedIn);
        Rc(api.EnqueueHip(context,token,queue));Submit(queue,old?outputs:resizedOut);
        auto result=queue->Signal(tail.Get(),++value);Rc(api.EndRecordingExecution(context,token,queue,3,tail.Get(),value,result));Wait(tail.Get(),value);
    }
    resized.Check();frame.Check();Rc(api.InvalidateRecording(context,resizedJob.handle));Rc(api.CollectRecording(context,resizedJob.handle));
    // A blocked producer proves collection cannot free an invalidated pending list.
    ComPtr<ID3D12Fence> gate;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
    Hr(q[0]->Wait(gate.Get(),1));Rc(api.BeginRecordingExecution(context,job.handle,q[0].Get()));Submit(q[0].Get(),inputs);
    Rc(api.EnqueueHip(context,job.handle,q[0].Get()));Submit(q[0].Get(),outputs);auto hr=q[0]->Signal(tail.Get(),++value);
    Rc(api.EndRecordingExecution(context,job.handle,q[0].Get(),3,tail.Get(),value,hr));Rc(api.InvalidateRecording(context,job.handle));
    Require(api.CollectRecording(context,job.handle)==LMXXF_NR_UNAVAILABLE,"pending recording collected early");
    Hr(gate->Signal(1));Wait(tail.Get(),value);Rc(api.CollectRecording(context,job.handle));Rc(api.Destroy(context));
    // SR consumes a copied texture. RGB9E5 and sRGB must not require UAV support.
    for(auto format:{DXGI_FORMAT_R9G9B9E5_SHAREDEXP,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
        context=nullptr;Rc(api.Create(&ci,&context));Rc(api.PrepareSession(context));Rc(set(context,&controls));
        Frame packed(device.Get(),256,256,format);List up(device.Get());packed.Upload(up.cmd.Get());Hr(up.cmd->Close());
        Submit(q[0].Get(),up);Hr(q[0]->Signal(tail.Get(),++value));Wait(tail.Get(),value);
        auto packedJob=make(packed);List in(device.Get()),out(device.Get());
        Rc(api.RecordInputs(context,packedJob.handle,in.cmd.Get()));Rc(api.RecordOutputs(context,packedJob.handle,out.cmd.Get()));
        packed.Read(out.cmd.Get(),static_cast<ID3D12Resource*>(packedJob.private_output));Hr(in.cmd->Close());Hr(out.cmd->Close());
        Rc(api.BeginRecordingExecution(context,packedJob.handle,q[0].Get()));Submit(q[0].Get(),in);
        Rc(api.EnqueueHip(context,packedJob.handle,q[0].Get()));Submit(q[0].Get(),out);auto signal=q[0]->Signal(tail.Get(),++value);
        Rc(api.EndRecordingExecution(context,packedJob.handle,q[0].Get(),3,tail.Get(),value,signal));Wait(tail.Get(),value);
        packed.Check();Rc(api.InvalidateRecording(context,packedJob.handle));Rc(api.CollectRecording(context,packedJob.handle));Rc(api.Destroy(context));
    }
    // The same allocation may change its render subrect every frame. The DRS
    // bucket keeps serving it without waiting for a stable input extent.
    context=nullptr;Rc(api.Create(&ci,&context));Rc(api.PrepareSession(context));
    controls.drs_mode=1;Rc(set(context,&controls));
    for(UINT width:{304u,288u,320u,304u}) {
        auto drsJob=make(resized,width);List in(device.Get()),out(device.Get());
        Rc(api.RecordInputs(context,drsJob.handle,in.cmd.Get()));Rc(api.RecordOutputs(context,drsJob.handle,out.cmd.Get()));
        resized.Read(out.cmd.Get(),static_cast<ID3D12Resource*>(drsJob.private_output));Hr(in.cmd->Close());Hr(out.cmd->Close());
        Rc(api.BeginRecordingExecution(context,drsJob.handle,q[0].Get()));Submit(q[0].Get(),in);
        Rc(api.EnqueueHip(context,drsJob.handle,q[0].Get()));Submit(q[0].Get(),out);auto signal=q[0]->Signal(tail.Get(),++value);
        Rc(api.EndRecordingExecution(context,drsJob.handle,q[0].Get(),3,tail.Get(),value,signal));Wait(tail.Get(),value);
        resized.Check(width);Rc(api.InvalidateRecording(context,drsJob.handle));Rc(api.CollectRecording(context,drsJob.handle));
    }
    Rc(api.Destroy(context));controls.drs_mode=0;
    // Prepare order is not execution order. A discarded or late recording must
    // not make history continuous, and ResetHistory also applies to closed lists.
    context=nullptr;Rc(api.Create(&ci,&context));Rc(api.PrepareSession(context));Rc(set(context,&controls));
    Frame motion(device.Get(),256,256);void* motionData=nullptr;Hr(motion.upload->Map(0,nullptr,&motionData));
    std::memset(motionData,0,size_t(motion.footprint.Footprint.RowPitch)*motion.height);motion.upload->Unmap(0,nullptr);
    List motionUpload(device.Get());motion.Upload(motionUpload.cmd.Get());Hr(motionUpload.cmd->Close());
    Submit(q[0].Get(),motionUpload);Hr(q[0]->Signal(tail.Get(),++value));Wait(tail.Get(),value);
    struct TemporalRecording {
        LmxxfNrJob job {sizeof job};List input,output;
        TemporalRecording(ID3D12Device* d):input(d),output(d) {}
    };
    auto temporal=[&](uint64_t id) {
        auto r=std::make_unique<TemporalRecording>(device.Get());
        MochizukiNrFrameInfo f {};f.struct_size=sizeof f;f.frame_id=id;
        f.color=frame.color.Get();f.color_width=frame.width;f.color_height=frame.height;
        f.color_state=f.motion_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        f.motion=motion.color.Get();f.motion_width=motion.width;f.motion_height=motion.height;
        f.motion_scale_x=f.motion_scale_y=1;f.flags=LMXXF_NR_FRAME_FLAG_STRENGTH|MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL;
        f.transfer_strength=f.color_strength=f.model_scale=1;f.passes=1;
        const auto deadline=GetTickCount64()+300000;
        for(;;) {int rc=prepare(context,&f,&r->job);if(!rc)break;if(GetTickCount64()>deadline)Rc(rc);Sleep(10);}
        Rc(api.RecordInputs(context,r->job.handle,r->input.cmd.Get()));Rc(api.RecordOutputs(context,r->job.handle,r->output.cmd.Get()));
        Hr(r->input.cmd->Close());Hr(r->output.cmd->Close());return r;
    };
    uint32_t temporalFrames=0,consumedFrames=0;
    auto executeTemporal=[&](TemporalRecording& r,bool consume) {
        auto token=r.job.handle;Rc(api.BeginRecordingExecution(context,token,q[0].Get()));Submit(q[0].Get(),r.input);
        Rc(api.EnqueueHip(context,token,q[0].Get()));Submit(q[0].Get(),r.output);auto signal=q[0]->Signal(tail.Get(),++value);
        Rc(api.EndRecordingExecution(context,token,q[0].Get(),3,tail.Get(),value,signal));Wait(tail.Get(),value);
        ++temporalFrames;consumedFrames+=consume;MochizukiNrInfo info {sizeof info};Rc(getInfo(context,&info));
        printf("HISTORY frame=%u consumed_pct=%u expected=%u\n",temporalFrames,info.history_consumed_pct,consumedFrames*100/temporalFrames);
        Require(info.history_consumed_pct==consumedFrames*100/temporalFrames,"history did not follow execution/reset order");
    };
    auto releaseTemporal=[&](TemporalRecording& r) {Rc(api.InvalidateRecording(context,r.job.handle));Rc(api.CollectRecording(context,r.job.handle));};
    auto t1=temporal(1);executeTemporal(*t1,false);releaseTemporal(*t1);
    auto t2=temporal(2);executeTemporal(*t2,true);releaseTemporal(*t2);
    auto discarded=temporal(3);auto t4=temporal(4);releaseTemporal(*discarded);executeTemporal(*t4,false);releaseTemporal(*t4);
    auto t5=temporal(5);Rc(api.ResetHistory(context));executeTemporal(*t5,false);releaseTemporal(*t5);
    auto t6=temporal(6);auto t7=temporal(7);executeTemporal(*t7,false);executeTemporal(*t6,false);releaseTemporal(*t7);releaseTemporal(*t6);
    auto t8=temporal(8);executeTemporal(*t8,false);releaseTemporal(*t8);
    auto t9=temporal(9);executeTemporal(*t9,true);executeTemporal(*t9,false);releaseTemporal(*t9);
    Rc(api.Destroy(context));
    // Cancellation must not return an unloadable DLL while its builder still runs.
    context=nullptr;Rc(api.Create(&ci,&context));Rc(api.PrepareSession(context));
    MochizukiNrFrameInfo cold {};cold.struct_size=sizeof cold;cold.color=resized.color.Get();cold.color_width=320;cold.color_height=256;
    cold.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;cold.model_scale=.5f;cold.passes=2;
    LmxxfNrJob coldJob {sizeof coldJob};const int coldRc=prepare(context,&cold,&coldJob);
    Require(coldRc==LMXXF_NR_OK || coldRc==LMXXF_NR_UNAVAILABLE,"cancellation test did not reach network preparation");
    if(coldRc==LMXXF_NR_UNAVAILABLE) {
        char reason[256] {};api.GetLastError(reason,sizeof reason);
        Require(std::string(reason).find("building the network")!=std::string::npos,"cancellation test did not start a build");
    }
    if(coldRc==LMXXF_NR_OK) {Rc(api.InvalidateRecording(context,coldJob.handle));Rc(api.CollectRecording(context,coldJob.handle));}
    Rc(api.Destroy(context));
    FreeLibrary(dll);puts("MOCHIZUKI_GPU_OK replay=14 queues=2 resize=passed delayed_collection=passed build_cancel=passed history_order=passed formats=passed drs=passed");return 0;
} catch(const std::exception& e) {fprintf(stderr,"FAIL %s\n",e.what());return 1;}
