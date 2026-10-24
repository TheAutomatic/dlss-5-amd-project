#include "runtime_test_helpers.h"
#include <psapi.h>
#include <filesystem>
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
    auto getProgress=reinterpret_cast<PFN_MochizukiNrGetBuildProgress>(GetProcAddress(dll,"MochizukiNrGetBuildProgress"));
    auto directHandle = reinterpret_cast<PFN_MochizukiNrGetDirectCommandBuffer>(GetProcAddress(dll, "MochizukiNrGetDirectCommandBuffer"));
    auto directRecord = reinterpret_cast<PFN_MochizukiNrRecordDirect>(GetProcAddress(dll, "MochizukiNrRecordDirect"));
    Require(directHandle && directRecord, "missing direct recording exports; replace complete package");
    uint64_t invalidHandle = UINT64_MAX;
    Require(directHandle(nullptr, nullptr, &invalidHandle) == LMXXF_NR_INVALID_ARGUMENT, "direct handle accepted null session");
    Require(directRecord(nullptr, nullptr, nullptr, 0) == LMXXF_NR_INVALID_ARGUMENT, "direct record accepted null recipe");
    // The level the game shows the frame at. The runtime exports it and nothing has ever called it, so the image
    // mode names it on the command line to see what the network does when the frame is levelled like the other lanes.
    auto setExposure=reinterpret_cast<PFN_MochizukiNrSetExposure>(GetProcAddress(dll,"MochizukiNrSetExposure"));
    // The meter is opt-in (host INI MochizukiWhitePointSource 2/3); the image mode turns it on when no exposure is named.
    auto setAutoExposure=reinterpret_cast<PFN_MochizukiNrSetAutoExposure>(GetProcAddress(dll,"MochizukiNrSetAutoExposure"));
    Require(getProgress!=nullptr,"missing build progress export");
    MochizukiNrBuildProgress invalidProgress {sizeof invalidProgress};
    Require(getProgress(nullptr,&invalidProgress)==LMXXF_NR_INVALID_ARGUMENT,"progress accepted null session");
    Require(defaults&&set&&prepare&&getInfo,"missing controls/frame exports");Rc(defaults(&controls));
    MochizukiNrControls oldControls {sizeof controls-4};
    Require(defaults(&oldControls)==LMXXF_NR_INVALID_ARGUMENT,"partial controls accepted");
    MochizukiNrControls oversizedControls {sizeof controls+4};
    Require(defaults(&oversizedControls)==LMXXF_NR_INVALID_ARGUMENT,"oversized controls accepted");
    if(argc==2) {puts("MOCHIZUKI_ABI_OK");FreeLibrary(dll);return 0;}
    reinterpret_cast<void(*)(uint32_t)>(GetProcAddress(dll,"MochizukiNrSetLogging"))(1);
    ComPtr<IDXGIFactory6> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D12Device> device;
    bool acoFallback=false, acoDevice=false, acoBudget=false, acoRapid=false;
    for(int i=3;i<argc;++i) {
        const std::wstring arg=argv[i];
        acoFallback=acoFallback || arg==L"--aco-fallback";
        acoDevice=acoDevice || arg==L"--aco-device";
        acoBudget=acoBudget || arg==L"--aco-budget";
        acoRapid=acoRapid || arg==L"--aco-rapid";
    }
    const bool acoMode=(argc>3 && std::wstring(argv[3])==L"--aco") || acoFallback || acoDevice || acoBudget || acoRapid;
    DXGI_ADAPTER_DESC1 adapterDesc {};
    for(UINT i=0;factory->EnumAdapters1(i,&adapter)==S_OK;i++,adapter.Reset()) {
        DXGI_ADAPTER_DESC1 desc;adapter->GetDesc1(&desc);
        const bool hardwareAmd=desc.VendorId==0x1002 && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE);
        if(!hardwareAmd) continue;
        if(acoMode && !acoFallback && !wcsstr(desc.Description, L"9070 XT")) continue;
        if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)))) { adapterDesc=desc; break; }
    }
    Require(device!=nullptr, acoMode ? "RX 9070 XT D3D12 adapter required" : "AMD D3D12 adapter required");
    if(acoMode) {
        char adapterName[160] {};
        WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, adapterName, sizeof adapterName, nullptr, nullptr);
        printf("ACO_ADAPTER %s vendor=0x%04X device=0x%04X\n", adapterName, adapterDesc.VendorId, adapterDesc.DeviceId);
        fflush(stdout);
    }
    ComPtr<ID3D12CommandQueue> q[2]; D3D12_COMMAND_QUEUE_DESC qd {};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    for(auto& a:q)Hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&a)));
    ComPtr<ID3D12Fence> tail;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&tail)));uint64_t value=0;
    LmxxfNrCreateInfo ci {sizeof ci,device.Get(),q[0].Get(),argv[2],LMXXF_NR_CREATE_FLAG_RECORDING_LEASES};
    Rc(api.Create(&ci,&context));
    if(acoMode) {
        // The damaged-bundle fixtures supply only Original weights. Select it
        // explicitly so this checks compiler fallback, not missing Small assets.
        if(acoFallback) controls.network_model=0;
        auto readInfo=[&] {
            MochizukiNrInfo info {sizeof info}; Rc(getInfo(context,&info)); return info;
        };
        auto open=[&](uint32_t aco) {
            if(context) { Rc(api.Destroy(context)); context=nullptr; }
            Rc(api.Create(&ci,&context));
            controls.aco_mode=aco; Rc(set(context,&controls));
            Rc(api.PrepareSession(context));
            return readInfo();
        };
        controls.aco_mode=1; Rc(set(context,&controls));
        Rc(api.PrepareSession(context));
        MochizukiNrInfo started=readInfo();
        Require(started.aco_requested==1, "ACO requested before device create");
        Require(started.aco_effective==0, "ACO effective before a network is installed");
        const bool driverRejected=started.aco_fallback[0]!=0;
        auto runFrame=[&] {
            Frame frame(device.Get(),256,256);
            List upload(device.Get()); frame.Upload(upload.cmd.Get()); Hr(upload.cmd->Close());
            Submit(q[0].Get(), upload); Hr(q[0]->Signal(tail.Get(), ++value)); Wait(tail.Get(), value);
            MochizukiNrFrameInfo fi {}; fi.struct_size=sizeof fi; fi.color=frame.color.Get();
            fi.color_width=frame.width; fi.color_height=frame.height;
            fi.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            fi.flags=LMXXF_NR_FRAME_FLAG_STRENGTH; fi.transfer_strength=fi.color_strength=1; fi.model_scale=1; fi.passes=1;
            LmxxfNrJob job {sizeof job}; const auto deadline=GetTickCount64()+300000;
            for(;;) {
                int rc=prepare(context,&fi,&job);
                if(!rc) break;
                if(rc!=LMXXF_NR_UNAVAILABLE || GetTickCount64()>deadline) Rc(rc);
                Sleep(100);
            }
            List inputs(device.Get()), outputs(device.Get());
            Rc(api.RecordInputs(context, job.handle, inputs.cmd.Get()));
            Rc(api.RecordOutputs(context, job.handle, outputs.cmd.Get()));
            frame.Read(outputs.cmd.Get(), static_cast<ID3D12Resource*>(job.private_output));
            Hr(inputs.cmd->Close()); Hr(outputs.cmd->Close());
            Rc(api.BeginRecordingExecution(context, job.handle, q[0].Get())); Submit(q[0].Get(), inputs);
            Rc(api.EnqueueHip(context, job.handle, q[0].Get())); Submit(q[0].Get(), outputs);
            auto signal=q[0]->Signal(tail.Get(), ++value);
            Rc(api.EndRecordingExecution(context, job.handle, q[0].Get(), 3, tail.Get(), value, signal));
            Wait(tail.Get(), value); frame.Check();
            Rc(api.InvalidateRecording(context, job.handle)); Rc(api.CollectRecording(context, job.handle));
        };
        if(acoRapid) {
            Require(!driverRejected && started.aco_fallback[0]==0, "rapid switch has no ACO device");
            runFrame();
            MochizukiNrInfo installed=readInfo();
            Require(installed.aco_effective==1 && installed.aco_fallback[0]==0, "rapid switch did not install ACO");
            printf("ACO_FRAME_OK 256x256\n");
            MochizukiNrBuildProgress before{sizeof before}; Rc(getProgress(context,&before));
            controls.aco_mode=0; Rc(set(context,&controls));
            runFrame();
            MochizukiNrInfo pending=readInfo();
            Require(pending.building!=0, "native candidate did not start");
            Require(pending.aco_requested==0 && pending.aco_effective==1, "turning ACO off changed the installed network");
            controls.aco_mode=1; Rc(set(context,&controls));
            const auto nativeDeadline=GetTickCount64()+300000;
            for(;;) {
                MochizukiNrBuildProgress prog{sizeof prog}; Rc(getProgress(context,&prog));
                const bool thisBuild=prog.start_tick!=before.start_tick || prog.active!=0;
                if(thisBuild && prog.active==0) break;
                Require(GetTickCount64()<nativeDeadline, "native candidate did not finish");
                Sleep(20);
            }
            // One prepare installs the finished native candidate. The request is ACO again, so the
            // following ACO graph may start, but it is not installed by this call.
            Frame once(device.Get(),256,256);
            List onceUpload(device.Get()); once.Upload(onceUpload.cmd.Get()); Hr(onceUpload.cmd->Close());
            Submit(q[0].Get(), onceUpload); Hr(q[0]->Signal(tail.Get(), ++value)); Wait(tail.Get(), value);
            MochizukiNrFrameInfo onceInfo{}; onceInfo.struct_size=sizeof onceInfo; onceInfo.color=once.color.Get();
            onceInfo.color_width=once.width; onceInfo.color_height=once.height;
            onceInfo.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            onceInfo.flags=LMXXF_NR_FRAME_FLAG_STRENGTH; onceInfo.transfer_strength=onceInfo.color_strength=1;
            onceInfo.model_scale=1; onceInfo.passes=1;
            LmxxfNrJob onceJob{sizeof onceJob};
            const int onceRc=prepare(context,&onceInfo,&onceJob);
            MochizukiNrInfo reenabled=readInfo();
            printf("ACO_RAPID_REENABLE requested=%u effective=%u building=%u\n",
                   reenabled.aco_requested, reenabled.aco_effective, reenabled.building);
            fflush(stdout);
            if(!onceRc) {
                List inputs(device.Get()), outputs(device.Get());
                Rc(api.RecordInputs(context, onceJob.handle, inputs.cmd.Get()));
                Rc(api.RecordOutputs(context, onceJob.handle, outputs.cmd.Get()));
                once.Read(outputs.cmd.Get(), static_cast<ID3D12Resource*>(onceJob.private_output));
                Hr(inputs.cmd->Close()); Hr(outputs.cmd->Close());
                Rc(api.BeginRecordingExecution(context, onceJob.handle, q[0].Get())); Submit(q[0].Get(), inputs);
                Rc(api.EnqueueHip(context, onceJob.handle, q[0].Get())); Submit(q[0].Get(), outputs);
                auto signal=q[0]->Signal(tail.Get(), ++value);
                Rc(api.EndRecordingExecution(context, onceJob.handle, q[0].Get(), 3, tail.Get(), value, signal));
                Wait(tail.Get(), value);
                Rc(api.InvalidateRecording(context, onceJob.handle)); Rc(api.CollectRecording(context, onceJob.handle));
            } else {
                Require(onceRc==LMXXF_NR_UNAVAILABLE, "native install prepare failed");
            }
            Require(reenabled.aco_requested==1 && reenabled.aco_effective==0, "native candidate installed after re-enable still reports ACO");
            Require(std::strstr(reenabled.aco_fallback, "next game start")==nullptr, "re-enable asked for a new device");
            const auto restoreDeadline=GetTickCount64()+300000;
            MochizukiNrInfo restored{};
            for(;;) {
                runFrame();
                restored=readInfo();
                if(!restored.building && restored.aco_effective==1) break;
                Require(GetTickCount64()<restoreDeadline, "re-enabled ACO network did not install");
            }
            Require(restored.aco_requested==1 && restored.aco_fallback[0]==0, "re-enabled ACO network kept a fallback");
            printf("ACO_RAPID_OK\n");
            fflush(stdout);
            Rc(api.Destroy(context));
            FreeLibrary(dll);
            return 0;
        }
        if(acoDevice) {
            Require(driverRejected, "pipeline-binary device create was accepted");
            Require(std::strstr(started.aco_fallback, "rejected pipeline binaries")!=nullptr, "device rejection has no reason");
            runFrame();
            MochizukiNrInfo after=readInfo();
            Require(after.aco_requested==1 && after.aco_effective==0, "rejected device installed an ACO network");
            Require(std::strstr(after.aco_fallback, "rejected pipeline binaries")!=nullptr, "native frame cleared the device rejection");
            printf("ACO_DEVICE_FALLBACK_OK %s\n", after.aco_fallback);
            fflush(stdout);
            Rc(api.Destroy(context));
            FreeLibrary(dll);
            return 0;
        }
        if(acoBudget) {
            Require(!driverRejected, "budget test has no ACO device");
            runFrame();
            MochizukiNrInfo installed=readInfo();
            Require(installed.aco_effective==1 && installed.aco_fallback[0]==0, "budget test did not install ACO");
            controls.aco_mode=0; Rc(set(context,&controls));
            Frame rejected(device.Get(),320,320);
            List uploadRejected(device.Get()); rejected.Upload(uploadRejected.cmd.Get()); Hr(uploadRejected.cmd->Close());
            Submit(q[0].Get(), uploadRejected); Hr(q[0]->Signal(tail.Get(), ++value)); Wait(tail.Get(), value);
            MochizukiNrFrameInfo rejectedInfo {}; rejectedInfo.struct_size=sizeof rejectedInfo; rejectedInfo.color=rejected.color.Get();
            rejectedInfo.color_width=rejected.width; rejectedInfo.color_height=rejected.height;
            rejectedInfo.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            rejectedInfo.flags=LMXXF_NR_FRAME_FLAG_STRENGTH; rejectedInfo.transfer_strength=rejectedInfo.color_strength=1;
            rejectedInfo.model_scale=1; rejectedInfo.passes=1;
            const auto budgetDeadline=GetTickCount64()+180000;
            MochizukiNrInfo failed {};
            for(;;) {
                LmxxfNrJob job {sizeof job};
                const int rc=prepare(context,&rejectedInfo,&job);
                failed=readInfo();
                if(std::strstr(failed.last_error, "frame buffer")) {
                    Require(rc==LMXXF_NR_UNAVAILABLE, "rejected candidate was installed");
                    break;
                }
                Require(rc==LMXXF_NR_UNAVAILABLE, "budget frame completed before the refusal");
                Require(GetTickCount64()<budgetDeadline, "budget refusal was not observed");
                Sleep(100);
            }
            Require(failed.aco_requested==0 && failed.aco_effective==1, "failed candidate published a new mode");
            printf("ACO_BUDGET_KEEP_OK effective=%u\n", failed.aco_effective);
            fflush(stdout);
            Rc(api.Destroy(context));
            FreeLibrary(dll);
            return 0;
        }
        if(acoFallback) {
            Require(driverRejected, "fallback fixture configured ACO onto this device");
            Require(started.aco_fallback[0]!=0, "native fallback has no reason");
            runFrame();
            MochizukiNrInfo after=readInfo();
            Require(after.aco_effective==0, "fallback frame imported an ACO network");
            Require(after.aco_fallback[0]!=0, "fallback frame cleared its reason");
            printf("ACO_FALLBACK_OK %s\n", after.aco_fallback);
            fflush(stdout);
            Rc(api.Destroy(context));
            FreeLibrary(dll);
            return 0;
        }
        uint32_t installedEffective=0;
        if(driverRejected) {
            Require(std::strstr(started.aco_fallback, "do not match")==nullptr, "ACO preflight rejected the aco/shaders corpus");
            Require(std::strstr(started.aco_fallback, "incomplete")==nullptr, "ACO bundle incomplete at device create");
            Require(std::strstr(started.aco_fallback, "could not be read")==nullptr, "ACO bundle could not be read");
            printf("ACO_DRIVER_REJECTED %s\n", started.aco_fallback);
        } else {
            Require(started.aco_fallback[0]==0, "unbuilt ACO device has a fallback");
            runFrame();
            MochizukiNrInfo after=readInfo();
            Require(after.aco_effective==1 && after.aco_fallback[0]==0, "ACO frame neither imported nor fell back");
            installedEffective=after.aco_effective;
            printf("ACO_FRAME_OK 256x256\n");
            controls.aco_mode=0; Rc(set(context,&controls));
            MochizukiNrInfo toggledOff=readInfo();
            Require(toggledOff.aco_requested==0 && toggledOff.aco_effective==1, "toggle off changed the installed network");
            controls.aco_mode=1; Rc(set(context,&controls));
            MochizukiNrInfo toggledOn=readInfo();
            Require(toggledOn.aco_requested==1 && toggledOn.aco_effective==1 && toggledOn.aco_fallback[0]==0, "toggle on changed the installed network");
            auto runInstalled=[&](uint32_t wantEffective) {
                const auto deadline=GetTickCount64()+300000;
                MochizukiNrInfo info {};
                for(;;) {
                    runFrame();
                    info=readInfo();
                    if(!info.building && info.aco_effective==wantEffective) return info;
                    Require(GetTickCount64()<deadline, "installed ACO state did not follow the frame");
                }
            };
            controls.aco_mode=0; Rc(set(context,&controls));
            MochizukiNrInfo nativeFrame=runInstalled(0);
            Require(nativeFrame.aco_requested==0 && nativeFrame.aco_effective==0 && nativeFrame.aco_fallback[0]==0, "native frame kept the ACO network");
            printf("ACO_SWITCH_NATIVE_OK\n");
            controls.aco_mode=1; Rc(set(context,&controls));
            MochizukiNrInfo heldNative=readInfo();
            Require(heldNative.aco_requested==1 && heldNative.aco_effective==0, "ACO request replaced the running native network");
            MochizukiNrInfo restored=runInstalled(1);
            Require(restored.aco_requested==1 && restored.aco_effective==1 && restored.aco_fallback[0]==0, "ACO frame did not replace the native network");
            installedEffective=restored.aco_effective;
            printf("ACO_SWITCH_ACO_OK\n");
        }
        fflush(stdout);
        MochizukiNrInfo reopened=open(1);
        Require(reopened.aco_requested==1 && reopened.aco_effective==0, "reopen published ACO before a network");
        if(driverRejected) {
            Require(reopened.aco_fallback[0]!=0, "reopen lost the driver rejection");
            Require(std::strstr(reopened.aco_fallback, "do not match")==nullptr, "reopen fell back to a record mismatch");
        } else {
            Require(reopened.aco_fallback[0]==0, "reopen with ACO requested has a fallback");
        }
        MochizukiNrInfo switchedOff=open(0);
        Require(switchedOff.aco_requested==0 && switchedOff.aco_effective==0, "a new device with ACO off stayed on the imported network");
        Rc(api.Destroy(context));
        printf("ACO_CONTRACT_OK effective=%u\n", installedEffective);
        fflush(stdout);
        FreeLibrary(dll);
        return 0;
    }
    Rc(api.PrepareSession(context));Rc(set(context,&controls));
    {
        uint64_t nativeHandle = UINT64_MAX;
        Rc(directHandle(context, nullptr, &nativeHandle));
        Require(nativeHandle == 0, "Windows unexpectedly selected direct game command stream");
    }
    {
        MochizukiNrControls aco=controls; aco.aco_mode=1; Rc(set(context,&aco));
        MochizukiNrInfo acoInfo {sizeof acoInfo}; Rc(getInfo(context,&acoInfo));
        Require(acoInfo.aco_requested==1 && acoInfo.aco_effective==0, "ACO request after device create stays off");
        Require(std::strstr(acoInfo.aco_fallback, "next game start")!=nullptr, "ACO change after device create asks for a restart");
        Rc(set(context,&controls));
    }
    MochizukiNrBuildProgress wrongProgress {sizeof(MochizukiNrBuildProgress)-4};
    Require(getProgress(context,&wrongProgress)==LMXXF_NR_INVALID_ARGUMENT,"partial progress struct accepted");
    MochizukiNrInfo oldInfo {sizeof(MochizukiNrInfo)-16};
    Require(getInfo(context,&oldInfo)==LMXXF_NR_INVALID_ARGUMENT,"old info contract accepted");
    MochizukiNrInfo previousInfo {static_cast<uint32_t>(offsetof(MochizukiNrInfo, gpu_ms_network_median))};
    Require(getInfo(context,&previousInfo)==LMXXF_NR_INVALID_ARGUMENT,"previous timing ABI accepted");
    MochizukiNrInfo previousSupplyInfo {static_cast<uint32_t>(offsetof(MochizukiNrInfo, recording_timing_reads))};
    Require(getInfo(context,&previousSupplyInfo)==LMXXF_NR_INVALID_ARGUMENT,"previous timing supply ABI accepted");
    MochizukiNrInfo initialSupplyInfo {sizeof initialSupplyInfo}; Rc(getInfo(context,&initialSupplyInfo));
    Require(initialSupplyInfo.recording_timing_reads==0 && initialSupplyInfo.recording_timing_rejected==0,
            "timing supply counters nonzero before a runtime exists");
    const bool profile=argc>3 && std::wstring(argv[3])==L"--profile";
    // The frame-side strengths, which the image mode may override: MochizukiDetailStrength / MochizukiColourStrength
    // reach the model here, while Controls::intensity reaches it through the control block.
    float g_transferStrength=1.f,g_colorStrength=1.f;
    const bool startup=profile || (argc>3 && std::wstring(argv[3])==L"--startup");
    const float profileScale=profile && argc>7 ? float(_wtof(argv[7])) : 1.f;
    const uint32_t profilePasses=profile && argc>8 ? uint32_t(_wtoi(argv[8])) : 1u;
    Require(std::isfinite(profileScale) && profileScale>=.25f && profileScale<=1.f &&
            profilePasses>=1 && profilePasses<=3,"invalid profile scale/passes");
    if(profile) {
        controls.preprocess=argc>9 && _wtoi(argv[9])!=0;
        controls.compact_transfer=argc>10 ? _wtoi(argv[10])!=0 : 1;
        controls.enlarge_mode=argc>11 ? uint32_t(_wtoi(argv[11])) : 0;
        Rc(set(context,&controls));
    }
    UINT startupWidth=1920,startupHeight=1080;
    if(startup && argc>6) {
        startupWidth=UINT(_wtoi(argv[5]));startupHeight=UINT(_wtoi(argv[6]));
        Require(startupWidth>0 && startupHeight>0 && startupWidth<=3840 && startupHeight<=2160,"invalid startup dimensions");
    }
    const bool startupCachePresent = startup && std::filesystem::exists(std::filesystem::path(argv[2]) / L"dlssnr-amd/pipeline.cache");
    Frame frame(device.Get(),startup?startupWidth:256,startup?startupHeight:256,startup?DXGI_FORMAT_R11G11B10_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT);
    List upload(device.Get());frame.Upload(upload.cmd.Get());Hr(upload.cmd->Close());Submit(q[0].Get(),upload);Hr(q[0]->Signal(tail.Get(),++value));Wait(tail.Get(),value);
    std::unique_ptr<Frame> timingMotion;
    if(argc>3 && std::wstring(argv[3])==L"--stage-timing") {
        timingMotion=std::make_unique<Frame>(device.Get(),256,256);
        void* data=nullptr;Hr(timingMotion->upload->Map(0,nullptr,&data));
        std::memset(data,0,size_t(timingMotion->footprint.Footprint.RowPitch)*timingMotion->height);
        timingMotion->upload->Unmap(0,nullptr);List up(device.Get());timingMotion->Upload(up.cmd.Get());Hr(up.cmd->Close());
        Submit(q[0].Get(),up);Hr(q[0]->Signal(tail.Get(),++value));Wait(tail.Get(),value);
    }
    auto make=[&](Frame& f,UINT validWidth=0,bool allowBackgroundBuild=false) {
        MochizukiNrFrameInfo info {};info.struct_size=sizeof info;info.color=f.color.Get();info.color_width=validWidth?validWidth:f.width;info.color_height=f.height;
        info.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;info.flags=LMXXF_NR_FRAME_FLAG_STRENGTH;info.transfer_strength=g_transferStrength;info.color_strength=g_colorStrength;info.model_scale=profileScale;info.passes=profilePasses;
        if(timingMotion) {
            info.motion=timingMotion->color.Get();info.motion_width=timingMotion->width;info.motion_height=timingMotion->height;
            info.motion_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;info.motion_scale_x=info.motion_scale_y=1;
            info.flags|=MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL;
        }
        LmxxfNrJob job {sizeof job};const auto deadline=GetTickCount64()+300000;
        std::string lastStage;uint32_t lastCompleted=UINT32_MAX;
        for(;;) {
            int rc=prepare(context,&info,&job);
            MochizukiNrBuildProgress progress {sizeof progress};Rc(getProgress(context,&progress));
            Require(progress.completed<=progress.total,"invalid build progress count");
            if(progress.active && (lastStage!=progress.stage || lastCompleted!=progress.completed)) {
                printf("BUILD_PROGRESS %.1fs %s %u/%u\n",double(GetTickCount64()-progress.start_tick)/1000,progress.stage,progress.completed,progress.total);
                fflush(stdout);lastStage=progress.stage;lastCompleted=progress.completed;
            }
            if(!rc) {Require(allowBackgroundBuild || !progress.active,"progress still active after ready");break;}
            if(rc!=LMXXF_NR_UNAVAILABLE||GetTickCount64()>deadline)Rc(rc);Sleep(100);
        }
        return job;
    };
    if(argc>3 && std::wstring(argv[3])==L"--stage-timing") {
        auto retire=[&](LmxxfNrJob& j) { Rc(api.InvalidateRecording(context,j.handle));Rc(api.CollectRecording(context,j.handle)); };
        auto installed=[&](bool reno,uint32_t passes) {
            const auto deadline=GetTickCount64()+300000;
            for (;;) {
                auto j=make(frame,0,true);MochizukiNrInfo t {sizeof t};Rc(getInfo(context,&t));
                if (!t.building && bool(t.renodx_effective)==reno && t.max_passes==passes) return j;
                retire(j);Require(GetTickCount64()<deadline,"timing fixture runtime switch timed out");Sleep(10);
            }
        };
        for (uint32_t passes : {1u, 30u, 0u}) {
            controls.renodx_enabled=1; controls.reno_unlock_passes=1;
            controls.reno_passes=std::max(passes,1u); controls.reno_nr_enabled=passes!=0;
            controls.max_passes=std::max(passes,1u);
            Rc(set(context,&controls));
            auto j=installed(true,std::max(passes,1u)); List in(device.Get()),out(device.Get());
            Rc(api.RecordInputs(context,j.handle,in.cmd.Get()));Rc(api.RecordOutputs(context,j.handle,out.cmd.Get()));
            Hr(in.cmd->Close());Hr(out.cmd->Close());
            for (uint32_t f=0;f<12;++f) {
                auto* queue=q[f%2].Get();Rc(api.BeginRecordingExecution(context,j.handle,queue));Submit(queue,in);
                Rc(api.EnqueueHip(context,j.handle,queue));Submit(queue,out);const auto hr=queue->Signal(tail.Get(),++value);
                Rc(api.EndRecordingExecution(context,j.handle,queue,3,tail.Get(),value,hr));Wait(tail.Get(),value);
            }
            MochizukiNrInfo t {sizeof t};Rc(getInfo(context,&t));
            Require(t.renodx_effective && t.gpu_samples && t.gpu_ms_last>0,"missing stage sample");
            Require(t.gpu_ms_network_last>=0 && t.gpu_ms_other_last>=0 &&
                std::abs(t.gpu_ms_last-t.gpu_ms_network_last-t.gpu_ms_other_last)<1e-4f,"incoherent stage sample");
            Require(passes ? t.gpu_ms_network_last>0 : t.gpu_ms_network_last==0,"wrong zero/multipass network timing");
            Require(t.gpu_ms_median>=t.gpu_ms_network_median && t.gpu_ms_p95>=t.gpu_ms_network_p95,"stage total quantiles changed");
            MochizukiNrInfo polled {sizeof polled};Rc(getInfo(context,&polled));
            Require(polled.gpu_tick==t.gpu_tick && polled.gpu_samples==t.gpu_samples,"poll refreshed timing sample");
            printf("STAGE_TIMING passes=%u samples=%llu total=%.3f network=%.3f other=%.3f ms\n",passes,
                static_cast<unsigned long long>(t.gpu_samples),t.gpu_ms_last,t.gpu_ms_network_last,t.gpu_ms_other_last);
            retire(j);
        }
        controls.renodx_enabled=0;Rc(set(context,&controls));auto standard=installed(false,1);
        MochizukiNrInfo switched {sizeof switched};Rc(getInfo(context,&switched));
        Require(!switched.renodx_effective && switched.gpu_ms_median==0 && switched.gpu_tick==0,"route switch retained timing history");
        retire(standard);Rc(api.Destroy(context));FreeLibrary(dll);puts("MOCHIZUKI_STAGE_TIMING_OK");return 0;
    }
    if(argc>3 && std::wstring(argv[3])==L"--split-retain") {
        struct Held {
            LmxxfNrJob job {sizeof job};
            List in, out;
            explicit Held(ID3D12Device* d) : in(d), out(d) {}
        };
        std::vector<std::unique_ptr<Held>> kept;
        auto record=[&](Held& h) {
            Rc(api.RecordInputs(context,h.job.handle,h.in.cmd.Get()));
            Rc(api.RecordOutputs(context,h.job.handle,h.out.cmd.Get()));
            frame.Read(h.out.cmd.Get(),static_cast<ID3D12Resource*>(h.job.private_output));
            Hr(h.in.cmd->Close()); Hr(h.out.cmd->Close());
        };
        auto execute=[&](Held& h) {
            Rc(api.BeginRecordingExecution(context,h.job.handle,q[0].Get()));
            Submit(q[0].Get(),h.in);
            Rc(api.EnqueueHip(context,h.job.handle,q[0].Get()));
            Submit(q[0].Get(),h.out);
            auto hr=q[0]->Signal(tail.Get(),++value);
            Rc(api.EndRecordingExecution(context,h.job.handle,q[0].Get(),3,tail.Get(),value,hr));
            Wait(tail.Get(),value);
        };
        ID3D12Resource* shared=nullptr;
        for(int i=0;i<5;++i) {
            auto h=std::make_unique<Held>(device.Get());
            h->job=make(frame);
            auto* output=static_cast<ID3D12Resource*>(h->job.private_output);
            Require(output!=nullptr,"split recording has no output");
            if(!shared) shared=output;
            Require(output==shared,"split recording allocated a private buffer set");
            record(*h);
            execute(*h);
            char error[256] {}; api.GetLastError(error,sizeof error);
            Require(std::strstr(error,"inline slots are full")==nullptr,"split prepare reported inline slots full");
            Require(api.CollectRecording(context,h->job.handle)==LMXXF_NR_UNAVAILABLE,"completed split recording collected before invalidate");
            kept.push_back(std::move(h));
        }
        execute(*kept.front());
        {
            auto extra=std::make_unique<Held>(device.Get());
            extra->job=make(frame);
            Require(static_cast<ID3D12Resource*>(extra->job.private_output)==shared,"new split recording replaced the shared output");
            record(*extra);
            Rc(api.BeginRecordingExecution(context,extra->job.handle,q[0].Get()));
            MochizukiNrFrameInfo blocked {}; blocked.struct_size=sizeof blocked;
            blocked.color=frame.color.Get(); blocked.color_width=frame.width; blocked.color_height=frame.height;
            blocked.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            blocked.flags=LMXXF_NR_FRAME_FLAG_STRENGTH; blocked.transfer_strength=blocked.color_strength=blocked.model_scale=1; blocked.passes=1;
            LmxxfNrJob rejected {sizeof rejected};
            const int rc=prepare(context,&blocked,&rejected);
            char error[256] {}; api.GetLastError(error,sizeof error);
            Require(rc==LMXXF_NR_UNAVAILABLE && std::strstr(error,"execution unresolved")!=nullptr,"in-flight split recording admitted another frame");
            Rc(api.EndRecordingExecution(context,extra->job.handle,q[0].Get(),0,nullptr,0,S_OK));
            auto followed=std::make_unique<Held>(device.Get());
            followed->job=make(frame);
            Require(static_cast<ID3D12Resource*>(followed->job.private_output)==shared,"split output changed after the in-flight frame ended");
            record(*followed); execute(*followed);
            kept.push_back(std::move(extra));
            kept.push_back(std::move(followed));
        }
        for(auto& h:kept) {
            Rc(api.InvalidateRecording(context,h->job.handle));
            Rc(api.CollectRecording(context,h->job.handle));
        }
        Rc(api.Destroy(context)); FreeLibrary(dll);
        puts("SPLIT_RETAIN_OK");
        return 0;
    }
    if(argc>3 && std::wstring(argv[3])==L"--first-pass") {
        auto release=[&](LmxxfNrJob& job) {
            Rc(api.InvalidateRecording(context,job.handle));
            Rc(api.CollectRecording(context,job.handle));
        };
        auto open=[&](uint32_t passes, bool preprocess, bool apply) {
            Rc(api.Destroy(context)); context=nullptr;
            Rc(api.Create(&ci,&context)); Rc(api.PrepareSession(context));
            controls.preprocess=preprocess?1u:0u; controls.preprocess_exposure=2;
            controls.apply_model=apply?1u:0u; controls.compact_transfer=1; controls.enlarge_mode=0;
            Rc(set(context,&controls));
            MochizukiNrFrameInfo fi {}; fi.struct_size=sizeof fi; fi.frame_id=1;
            fi.color=frame.color.Get(); fi.color_width=frame.width; fi.color_height=frame.height;
            fi.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            fi.flags=LMXXF_NR_FRAME_FLAG_STRENGTH|LMXXF_NR_FRAME_FLAG_FIRST_PASS;
            fi.model_scale=1; fi.passes=passes; fi.transfer_strength=fi.color_strength=1;
            LmxxfNrJob job {sizeof job};
            const auto deadline=GetTickCount64()+300000;
            for(;;) {
                int rc=prepare(context,&fi,&job);
                if(!rc) {
                    MochizukiNrInfo ready {sizeof ready}; Rc(getInfo(context,&ready));
                    if(!ready.building) break;
                    Rc(api.InvalidateRecording(context,job.handle));
                    Rc(api.CollectRecording(context,job.handle));
                    rc=LMXXF_NR_UNAVAILABLE;
                }
                if(rc!=LMXXF_NR_UNAVAILABLE || GetTickCount64()>deadline) Rc(rc);
                Sleep(100);
            }
            return job;
        };
        auto single=open(1,false,true);
        Require(single.first_pass_output==nullptr,"single-pass exported a first-pass image");
        release(single);
        auto pre=open(2,true,true);
        Require(pre.first_pass_output==nullptr,"preprocess exported a first-pass image");
        release(pre);
        auto hidden=open(2,false,false);
        Require(hidden.first_pass_output==nullptr,"apply-model off exported a first-pass image");
        release(hidden);
        auto two=open(2,false,true);
        Require(two.first_pass_output!=nullptr,"two-pass first output missing");
        List inputs(device.Get()),outputs(device.Get());
        Rc(api.RecordInputs(context,two.handle,inputs.cmd.Get()));
        Rc(api.RecordOutputs(context,two.handle,outputs.cmd.Get()));
        frame.Read(outputs.cmd.Get(),static_cast<ID3D12Resource*>(two.private_output));
        auto firstBack=Buffer(device.Get(),UINT64(frame.footprint.Footprint.RowPitch)*frame.height,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        auto* firstImage=static_cast<ID3D12Resource*>(two.first_pass_output);
        Barrier(outputs.cmd.Get(),firstImage,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst {firstBack.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}}; dst.PlacedFootprint=frame.footprint;
        D3D12_TEXTURE_COPY_LOCATION src {firstImage,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
        outputs.cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Barrier(outputs.cmd.Get(),firstImage,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Hr(inputs.cmd->Close()); Hr(outputs.cmd->Close());
        Rc(api.BeginRecordingExecution(context,two.handle,q[0].Get())); Submit(q[0].Get(),inputs);
        Rc(api.EnqueueHip(context,two.handle,q[0].Get())); Submit(q[0].Get(),outputs);
        Hr(q[0]->Signal(tail.Get(),++value));
        Rc(api.EndRecordingExecution(context,two.handle,q[0].Get(),3,tail.Get(),value,S_OK)); Wait(tail.Get(),value);
        auto grab=[&](ID3D12Resource* rb) {
            void* ptr=nullptr; Hr(rb->Map(0,nullptr,&ptr));
            const size_t row=size_t(frame.width)*8;
            std::vector<unsigned char> bytes(row*frame.height);
            for(UINT y=0;y<frame.height;++y)
                memcpy(bytes.data()+y*row,static_cast<char*>(ptr)+y*frame.footprint.Footprint.RowPitch,row);
            rb->Unmap(0,nullptr); return bytes;
        };
        const auto finalBytes=grab(frame.readback.Get()), firstBytes=grab(firstBack.Get());
        Require(finalBytes!=firstBytes,"first pass matched the final two-pass output");
        auto finite=[&](const std::vector<unsigned char>& bytes) {
            double sum=0;
            for(UINT y=0;y<frame.height;++y) for(UINT x=0;x<frame.width;++x) {
                const auto* px=reinterpret_cast<const uint16_t*>(bytes.data()+(size_t(y)*frame.width+x)*8);
                const float v=DirectX::PackedVector::XMConvertHalfToFloat(px[0]);
                Require(std::isfinite(v)&&std::abs(v)<100.f,"non-finite first or final output");
                sum+=std::abs(v);
            }
            Require(sum>0,"black first or final output");
        };
        finite(finalBytes); finite(firstBytes);
        release(two); Rc(api.Destroy(context)); FreeLibrary(dll);
        puts("MOCHIZUKI_FIRST_PASS_OK single=rejected preprocess=rejected apply_off=rejected two_pass=distinct");
        return 0;
    }
    if(argc>3 && std::wstring(argv[3])==L"--composition") {
        Frame motion(device.Get(),256,256);void* data=nullptr;
        Hr(motion.upload->Map(0,nullptr,&data));
        std::memset(data,0,size_t(motion.footprint.Footprint.RowPitch)*motion.height);
        motion.upload->Unmap(0,nullptr);
        List up(device.Get());motion.Upload(up.cmd.Get());Hr(up.cmd->Close());
        Submit(q[0].Get(),up);Hr(q[0]->Signal(tail.Get(),++value));Wait(tail.Get(),value);
        struct Recording {
            LmxxfNrJob job {sizeof job};List in,out;
            Recording(ID3D12Device* d):in(d),out(d) {}
        };
        uint64_t id=0;
        for(auto format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
            Frame f(device.Get(),256,256,format);List uploadFrame(device.Get());
            f.Upload(uploadFrame.cmd.Get());Hr(uploadFrame.cmd->Close());Submit(q[0].Get(),uploadFrame);
            Hr(q[0]->Signal(tail.Get(),++value));Wait(tail.Get(),value);
            const size_t inputRow=f.width*(format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4);
            std::vector<unsigned char> original(inputRow*f.height);
            void* source=nullptr;Hr(f.upload->Map(0,nullptr,&source));
            for(UINT y=0;y<f.height;++y)
                memcpy(original.data()+y*inputRow,static_cast<char*>(source)+y*f.footprint.Footprint.RowPitch,inputRow);
            f.upload->Unmap(0,nullptr);
            for(uint32_t passes:{1u,3u}) for(uint32_t mode:{0u,1u}) {
                Rc(api.Destroy(context));context=nullptr;
                Rc(api.Create(&ci,&context));Rc(api.PrepareSession(context));
                std::vector<std::vector<unsigned char>> reference;
                std::vector<std::unique_ptr<Recording>> held;
                uint32_t lastConsumed=0;
                auto execute=[&](Recording& r, unsigned queueIndex=0) {
                    // Queue switches intentionally reset runtime history.
                    // Keep the temporal comparison on one queue; switch for replay.
                    auto* queue=q[queueIndex].Get();
                    Rc(api.BeginRecordingExecution(context,r.job.handle,queue));Submit(queue,r.in);
                    Rc(api.EnqueueHip(context,r.job.handle,queue));Submit(queue,r.out);
                    auto hr=queue->Signal(tail.Get(),++value);
                    Rc(api.EndRecordingExecution(context,r.job.handle,queue,3,tail.Get(),value,hr));Wait(tail.Get(),value);
                    void* ptr=nullptr;Hr(f.readback->Map(0,nullptr,&ptr));
                    const size_t row=f.width*(format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4);
                    std::vector<unsigned char> result(row*f.height);
                    for(UINT y=0;y<f.height;++y) memcpy(result.data()+y*row,static_cast<char*>(ptr)+y*f.footprint.Footprint.RowPitch,row);
                    f.readback->Unmap(0,nullptr);return result;
                };
                for(uint32_t compact:{0u,1u}) {
                    lastConsumed=0;
                    controls.compact_transfer=compact;controls.enlarge_mode=mode;
                    controls.apply_model=1;
                    controls.preprocess=passes==3;controls.preprocess_exposure=2;
                    Rc(set(context,&controls));Rc(api.ResetHistory(context));
                    // Warm lazy driver work before comparing consecutive history:
                    // the runtime intentionally resets after a >250 ms gap.
                    for(unsigned n=0;n<7;++n) {
                        if(n==3) Rc(api.ResetHistory(context));
                        if(n==6) { controls.apply_model=0;Rc(set(context,&controls)); }
                        auto r=std::make_unique<Recording>(device.Get());
                        MochizukiNrFrameInfo fi{};fi.struct_size=sizeof fi;fi.frame_id=++id;
                        fi.color=f.color.Get();fi.color_width=f.width;fi.color_height=f.height;
                        fi.motion=motion.color.Get();fi.motion_width=motion.width;fi.motion_height=motion.height;
                        fi.color_state=fi.motion_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        fi.motion_scale_x=fi.motion_scale_y=1;
                        fi.flags=LMXXF_NR_FRAME_FLAG_STRENGTH|MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL;
                        fi.model_scale=.5f;fi.passes=passes;fi.transfer_strength=fi.color_strength=1;
                        auto deadline=GetTickCount64()+300000;
                        for(;;) {
                            int rc=prepare(context,&fi,&r->job);
                            if(!rc) {
                                MochizukiNrInfo ready{sizeof ready};Rc(getInfo(context,&ready));
                                if(!ready.building) break;
                                // The host may keep serving the old layout during
                                // rebuild. Compare only after the new one is installed.
                                Rc(api.InvalidateRecording(context,r->job.handle));
                                Rc(api.CollectRecording(context,r->job.handle));
                                rc=LMXXF_NR_UNAVAILABLE;
                            }
                            if(rc!=LMXXF_NR_UNAVAILABLE || GetTickCount64()>deadline)Rc(rc);
                            Sleep(10);
                        }
                        Rc(api.RecordInputs(context,r->job.handle,r->in.cmd.Get()));
                        Rc(api.RecordOutputs(context,r->job.handle,r->out.cmd.Get()));
                        f.Read(r->out.cmd.Get(),static_cast<ID3D12Resource*>(r->job.private_output));
                        Hr(r->in.cmd->Close());Hr(r->out.cmd->Close());
                        auto bytes=execute(*r);
                        MochizukiNrInfo sample{sizeof sample};Rc(getInfo(context,&sample));
                        Require(sample.frames<64,"composition history observation exceeded its window");
                        // History statistics reset on network replacement; the
                        // session's overall frame counter does not.
                        const auto consumed=uint32_t(std::lround(sample.history_consumed_pct*(n+1)/100.0));
                        if(n>=3 && consumed!=lastConsumed+(n>3?1u:0u)) {
                            fprintf(stderr,"HISTORY_MISMATCH format=%u passes=%u mode=%u compact=%u sample=%u frames=%llu pct=%u consumed=%u previous=%u\n",
                                    unsigned(format),passes,mode,compact,n,static_cast<unsigned long long>(sample.frames),
                                    sample.history_consumed_pct,consumed,lastConsumed);
                            throw std::runtime_error("comparison did not use the intended temporal history");
                        }
                        lastConsumed=consumed;
                        if(n==6) {
                            Require(bytes==original,"Apply model off changed the original image");
                            held.push_back(std::move(r));continue;
                        }
                        if(n<3) { held.push_back(std::move(r)); continue; }
                        if(!compact) reference.push_back(bytes);
                        else if(bytes!=reference[n-3]) {
                            size_t changed=0;for(size_t k=0;k<bytes.size();++k) changed+=bytes[k]!=reference[n-3][k];
                            MochizukiNrInfo state{sizeof state};Rc(getInfo(context,&state));
                            fprintf(stderr,"MISMATCH format=%u passes=%u mode=%u frame=%u changed_bytes=%zu/%zu history_pct=%u\n",
                                    unsigned(format),passes,mode,n,changed,bytes.size(),state.history_consumed_pct);
                            throw std::runtime_error("compact/separate temporal output mismatch");
                        }
                        held.push_back(std::move(r));
                    }
                }
                Rc(api.ResetHistory(context));
                Require(execute(*held[3],1)==reference.front(),"retained old recording changed after buffer-mode switch");
                for(auto& r:held) {
                    Rc(api.InvalidateRecording(context,r->job.handle));Rc(api.CollectRecording(context,r->job.handle));
                }
                printf("COMPOSITION_OK format=%u passes=%u edge=%u preprocess=%u temporal_frames=3 old_recording=passed\n",
                       unsigned(format),passes,mode,controls.preprocess);
            }
        }
        Rc(api.Destroy(context));FreeLibrary(dll);
        puts("MOCHIZUKI_COMPOSITION_OK");return 0;
    }
    if(argc>3 && std::wstring(argv[3])==L"--image") {
        // One real frame through the network with the settings named on the command line, then the input and the
        // output measured against each other: how much NR actually edits a game's own picture, and whether a setting
        // changes that. The build is the slow part on a cold pipeline cache, so this prints the same progress.
        Require(argc>=12,"usage: --image <in.f32> <w> <h> <dxgi> <white> <intensity> <preprocess> <linear> [out.f32] [mask] [skin] [style] [structure] [tone] [transfer] [colour] [max_ratio] [curve] [bias_ev] [contrast] [saturation] [frames] [exposure]");
        const UINT shotW=UINT(_wtoi(argv[5])),shotH=UINT(_wtoi(argv[6]));
        Require(shotW>0&&shotH>0&&shotW<=3840&&shotH<=2160,"invalid image dimensions");
        const DXGI_FORMAT shotFormat=DXGI_FORMAT(_wtoi(argv[7]));
        controls.white_point=float(_wtof(argv[8]));
        controls.intensity=float(_wtof(argv[9]));
        controls.preprocess=_wtoi(argv[10])!=0?1u:0u;
        controls.preprocess_exposure=1;
        controls.linear_input=uint32_t(_wtoi(argv[11]));
        controls.apply_model=1;
        // The model's own conditioning, which is what makes it treat the character differently from the scenery.
        if(argc>13&&argv[13][0])controls.automatic_mask=uint32_t(_wtoi(argv[13]));
        if(argc>14&&argv[14][0])controls.skin_structure=float(_wtof(argv[14]));
        if(argc>15&&argv[15][0])controls.style=uint32_t(_wtoi(argv[15]));
        if(argc>16&&argv[16][0])controls.local_structure=float(_wtof(argv[16]));
        if(argc>17&&argv[17][0])controls.local_tone=float(_wtof(argv[17]));
        if(argc>18&&argv[18][0])g_transferStrength=float(_wtof(argv[18]));
        if(argc>19&&argv[19][0])g_colorStrength=float(_wtof(argv[19]));
        if(argc>20&&argv[20][0])controls.max_ratio=float(_wtof(argv[20]));
        // The preprocess: exposure 1 (auto) is what this mode has always used, so what is left to name is the
        // curve it puts on top of the frame. Curve 0 is the level change alone - what a frame needs when the game
        // hands over no exposure and the frame is not the level the network was built for.
        if(argc>21&&argv[21][0])controls.preprocess_curve=uint32_t(_wtoi(argv[21]));
        if(argc>22&&argv[22][0])controls.preprocess_bias_ev=float(_wtof(argv[22]));
        if(argc>23&&argv[23][0])controls.preprocess_contrast=float(_wtof(argv[23]));
        if(argc>24&&argv[24][0])controls.preprocess_saturation=float(_wtof(argv[24]));
        int shotFrames=(argc>25&&argv[25][0])?_wtoi(argv[25]):1;if(shotFrames<1)shotFrames=1;
        Rc(set(context,&controls));
        // The lane's level reference: an explicit exposure here, or - with no argument, which is what every host frame
        // passes so far, Monster Hunter Wilds included - the runtime's own GPU meter, which puts the measured mean of
        // the linear frame on the key luminance (0.2073/mean, so the white point ends up at 4.826*mean, exactly what
        // the lmxxf and AMD lanes do with their own meters). An explicit exposure wins; the white point argument is the
        // trim of whichever reference is in force.
        if(argc>26&&argv[26][0]) {
            Require(setExposure!=nullptr,"missing exposure export");
            const float exposure=float(_wtof(argv[26]));
            Rc(setExposure(context,exposure));
            printf("IMAGE_EXPOSURE exposure=%.6f white_point=%.6f\n",double(exposure),double(controls.white_point));
        } else {
            Require(setAutoExposure!=nullptr,"missing auto exposure export");
            Rc(setAutoExposure(context,1u));
        }
        Frame shot(device.Get(),shotW,shotH,shotFormat);
        shot.Load(argv[4]);
        List shotUpload(device.Get());shot.Upload(shotUpload.cmd.Get());Hr(shotUpload.cmd->Close());
        Submit(q[0].Get(),shotUpload);Hr(q[0]->Signal(tail.Get(),++value));Wait(tail.Get(),value);
        for(int shotFrame=0;shotFrame<shotFrames;shotFrame++) {
            // The game's own shape: one prepare per frame, one recording per frame, both released once it has run.
            // The meter is session state, so a later frame sees the level an earlier one measured.
            auto shotJob=make(shot);
            List shotIn(device.Get()),shotOut(device.Get());
            Rc(api.RecordInputs(context,shotJob.handle,shotIn.cmd.Get()));
            Rc(api.RecordOutputs(context,shotJob.handle,shotOut.cmd.Get()));
            shot.Read(shotOut.cmd.Get(),static_cast<ID3D12Resource*>(shotJob.private_output));
            Hr(shotIn.cmd->Close());Hr(shotOut.cmd->Close());
            Rc(api.BeginRecordingExecution(context,shotJob.handle,q[0].Get()));Submit(q[0].Get(),shotIn);
            Rc(api.EnqueueHip(context,shotJob.handle,q[0].Get()));Submit(q[0].Get(),shotOut);
            auto shotHr=q[0]->Signal(tail.Get(),++value);
            Rc(api.EndRecordingExecution(context,shotJob.handle,q[0].Get(),3,tail.Get(),value,shotHr));
            Wait(tail.Get(),value);
            MochizukiNrInfo metered {sizeof metered};Rc(getInfo(context,&metered));
            if(shotFrames>1)
                printf("IMAGE_METER frame=%d white=%.4f preprocess_ev=%.4f\n",shotFrame,double(metered.white_point),
                       double(metered.preprocess_ev));
            Rc(api.InvalidateRecording(context,shotJob.handle));Rc(api.CollectRecording(context,shotJob.handle));
        }
        {   // The level the network was actually shown at: white_point is what the encode divided the linear
            // colour by, preprocess_ev the gain the meter settled on (NaN when it is not metering one).
            MochizukiNrInfo shown {sizeof shown};Rc(getInfo(context,&shown));
            printf("IMAGE_INFO white=%.4f preprocess_ev=%.4f frames=%llu history=%u%% passes=%u\n",
                   double(shown.white_point),double(shown.preprocess_ev),
                   static_cast<unsigned long long>(shown.frames),shown.history_consumed_pct,shown.max_passes);
        }
        shot.Compare();
        if(argc>12&&argv[12][0]) {
            FILE* file=nullptr;Require(_wfopen_s(&file,argv[12],L"wb")==0,"output dump open failed");
            void* pixels=nullptr;Hr(shot.readback->Map(0,nullptr,&pixels));
            std::vector<float> out(size_t(shotW)*shotH*3,0.f);
            for(UINT y=0;y<shotH;y++)for(UINT x=0;x<shotW;x++) {
                auto* row=static_cast<char*>(pixels)+y*shot.footprint.Footprint.RowPitch;float o[3] {};
                if(shotFormat==DXGI_FORMAT_R9G9B9E5_SHAREDEXP) {
                    DirectX::XMFLOAT3 v;DirectX::XMStoreFloat3(&v,DirectX::PackedVector::XMLoadFloat3SE(reinterpret_cast<DirectX::PackedVector::XMFLOAT3SE*>(row)+x));
                    o[0]=v.x;o[1]=v.y;o[2]=v.z;
                } else if(shotFormat==DXGI_FORMAT_R11G11B10_FLOAT) {
                    DirectX::XMFLOAT3 v;DirectX::XMStoreFloat3(&v,DirectX::PackedVector::XMLoadFloat3PK(reinterpret_cast<DirectX::PackedVector::XMFLOAT3PK*>(row)+x));
                    o[0]=v.x;o[1]=v.y;o[2]=v.z;
                } else if(shotFormat==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||shotFormat==DXGI_FORMAT_R8G8B8A8_UNORM) {
                    auto* p=reinterpret_cast<uint8_t*>(row)+x*4;for(int k=0;k<3;k++)o[k]=p[k]/255.f;
                } else {
                    auto* p=reinterpret_cast<uint16_t*>(row)+x*4;for(int k=0;k<3;k++)o[k]=DirectX::PackedVector::XMConvertHalfToFloat(p[k]);
                }
                memcpy(&out[(size_t(y)*shotW+x)*3],o,sizeof o);
            }
            shot.readback->Unmap(0,nullptr);
            Require(fwrite(out.data(),4,out.size(),file)==out.size(),"output dump write failed");fclose(file);
        }
        Rc(api.Destroy(context));FreeLibrary(dll);
        printf("MOCHIZUKI_IMAGE_OK %ux%u\n",shotW,shotH);
        return 0;
    }
    if(argc>3 && std::wstring(argv[3])==L"--pass-switch") {
        const std::wstring mode=argc>4?argv[4]:L"normal";
        Require(mode==L"normal" || mode==L"budget" || mode==L"oom" || mode==L"frame-budget","unknown pass-switch mode");
        const bool expansionFailure=mode==L"budget" || mode==L"oom";
        bool sawFallback=false, sawCandidateFailure=false;
        uint32_t oneDispatches=0,twoDispatches=0;
        auto infoNow=[&] {MochizukiNrInfo i {sizeof i};Rc(getInfo(context,&i));return i;};
        auto snapshot=[&] {
            void* pixels=nullptr;Hr(frame.readback->Map(0,nullptr,&pixels));
            std::vector<unsigned char> bytes(frame.width*frame.height*8);
            for(UINT y=0;y<frame.height;y++)
                memcpy(bytes.data()+y*frame.width*8,static_cast<char*>(pixels)+y*frame.footprint.Footprint.RowPitch,frame.width*8);
            frame.readback->Unmap(0,nullptr);return bytes;
        };
        auto execute=[&](LmxxfNrJob& j,List& in,List& out) {
            Rc(api.BeginRecordingExecution(context,j.handle,q[0].Get()));Submit(q[0].Get(),in);
            Rc(api.EnqueueHip(context,j.handle,q[0].Get()));Submit(q[0].Get(),out);
            auto hr=q[0]->Signal(tail.Get(),++value);Rc(api.EndRecordingExecution(context,j.handle,q[0].Get(),3,tail.Get(),value,hr));
            Wait(tail.Get(),value);return snapshot();
        };
        auto retire=[&](LmxxfNrJob& j) {Rc(api.InvalidateRecording(context,j.handle));Rc(api.CollectRecording(context,j.handle));};
        auto record=[&](LmxxfNrJob& j,List& in,List& out) {
            Rc(api.RecordInputs(context,j.handle,in.cmd.Get()));Rc(api.RecordOutputs(context,j.handle,out.cmd.Get()));
            frame.Read(out.cmd.Get(),static_cast<ID3D12Resource*>(j.private_output));Hr(in.cmd->Close());Hr(out.cmd->Close());
        };
        std::vector<unsigned char> goldenOne,goldenTwo;
        auto run=[&](uint32_t passes,uint32_t capacity,bool immediate) {
            const auto deadline=GetTickCount64()+120000;
            unsigned frames=0;
            for(;;) {
                MochizukiNrFrameInfo f {};f.struct_size=sizeof f;f.color=frame.color.Get();f.color_width=frame.width;f.color_height=frame.height;
                f.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;f.flags=LMXXF_NR_FRAME_FLAG_STRENGTH;
                f.transfer_strength=f.color_strength=f.model_scale=1;f.passes=passes;
                LmxxfNrJob j {sizeof j};int rc=prepare(context,&f,&j);auto state=infoNow();
                if(rc==LMXXF_NR_UNAVAILABLE) {
                    Require(!immediate && !(expansionFailure && !goldenOne.empty()),"lost executable network during capacity change");
                    if(mode==L"frame-budget" && strstr(state.last_error,"candidate frame buffers failed")) {
                        Require(state.max_passes==0,"incomplete candidate was installed");sawCandidateFailure=true;
                    }
                } else {
                    Rc(rc);List in(device.Get()),out(device.Get());record(j,in,out);auto pixels=execute(j,in,out);retire(j);state=infoNow();++frames;
                    if(state.max_passes==capacity && !state.building) {
                        frame.Check();
                        printf("PASS_SWITCH request=%u capacity=%u dispatches=%u wait_frames=%u\n",passes,state.max_passes,state.network_dispatches,frames);
                        return pixels;
                    }
                    Require(!immediate,"automatic pass reduction rebuilt the network");
                    if(expansionFailure && !goldenOne.empty() && state.max_passes==1) {
                        Require(pixels==goldenOne,"fallback changed the old network output");
                        char status[512] {};Rc(api.GetStatus(context,status,sizeof status));
                        Require(strstr(status,"requested 2 passes, using 1")!=nullptr,"missing requested/effective fallback status");
                        if((mode==L"budget" && strstr(status,"insufficient VRAM")) ||
                           (mode==L"oom" && strstr(state.last_error,"MZ_TEST_UPGRADE_OOM_ONCE"))) sawFallback=true;
                    }
                }
                Require(GetTickCount64()<deadline,"pass switch did not recover");Sleep(100);
            }
        };
        goldenOne=run(1,1,false);oneDispatches=infoNow().network_dispatches;
        goldenTwo=run(2,2,false);twoDispatches=infoNow().network_dispatches;
        Require(goldenOne!=goldenTwo && twoDispatches>oneDispatches,"two passes made no distinct GPU output");
        if(expansionFailure)Require(sawFallback,"failure injection was not observed with working old output");
        if(mode==L"frame-budget")Require(sawCandidateFailure,"candidate buffer failure was not observed");
        // Keep a replayable two-pass recording while other frames change the count.
        MochizukiNrFrameInfo f {};f.struct_size=sizeof f;f.color=frame.color.Get();f.color_width=frame.width;f.color_height=frame.height;
        f.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;f.flags=LMXXF_NR_FRAME_FLAG_STRENGTH;
        f.transfer_strength=f.color_strength=f.model_scale=1;f.passes=2;
        LmxxfNrJob old {sizeof old};Rc(prepare(context,&f,&old));List oldIn(device.Get()),oldOut(device.Get());record(old,oldIn,oldOut);
        for(int cycle=0;cycle<3;cycle++) {
            Require(run(1,2,true)==goldenOne,"single-pass output changed after capacity reuse");
            Require(infoNow().network_dispatches==oneDispatches,"single-pass dispatch count changed");
            Require(run(2,2,true)==goldenTwo,"two-pass output changed after capacity reuse");
            Require(infoNow().network_dispatches==twoDispatches,"two-pass dispatch count changed");
        }
        auto three=run(3,3,false);Require(three!=goldenTwo,"third pass made no distinct GPU output");
        Require(run(1,3,true)==goldenOne,"single-pass output changed at capacity three");
        Require(execute(old,oldIn,oldOut)==goldenTwo,"retained old recording no longer replays correctly");
        ComPtr<ID3D12Fence> gate;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
        Hr(q[0]->Wait(gate.Get(),1));Rc(api.BeginRecordingExecution(context,old.handle,q[0].Get()));Submit(q[0].Get(),oldIn);
        Rc(api.EnqueueHip(context,old.handle,q[0].Get()));Submit(q[0].Get(),oldOut);
        auto pending=q[0]->Signal(tail.Get(),++value);Rc(api.EndRecordingExecution(context,old.handle,q[0].Get(),3,tail.Get(),value,pending));
        Rc(api.InvalidateRecording(context,old.handle));
        Require(api.CollectRecording(context,old.handle)==LMXXF_NR_UNAVAILABLE,"pending old recording collected early");
        Hr(gate->Signal(1));Wait(tail.Get(),value);Rc(api.CollectRecording(context,old.handle));
        // An explicit smaller prebuild capacity still frees the larger network.
        controls.max_passes=1;Rc(set(context,&controls));Require(run(1,1,false)==goldenOne,"explicit capacity reduction changed output");
        // Different extents/formats cannot use the capacity fallback.
        for(auto format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R9G9B9E5_SHAREDEXP}) {
            Frame changed(device.Get(),format==DXGI_FORMAT_R16G16B16A16_FLOAT?320:256,256,format);
            List up(device.Get());changed.Upload(up.cmd.Get());Hr(up.cmd->Close());Submit(q[0].Get(),up);
            Hr(q[0]->Signal(tail.Get(),++value));Wait(tail.Get(),value);
            f.color=changed.color.Get();f.color_width=changed.width;f.color_height=changed.height;f.passes=1;
            LmxxfNrJob initial {sizeof initial};Require(prepare(context,&f,&initial)==LMXXF_NR_UNAVAILABLE,"incompatible frame reused old network");
            auto j=make(changed);List in(device.Get()),out(device.Get());
            Rc(api.RecordInputs(context,j.handle,in.cmd.Get()));Rc(api.RecordOutputs(context,j.handle,out.cmd.Get()));
            changed.Read(out.cmd.Get(),static_cast<ID3D12Resource*>(j.private_output));Hr(in.cmd->Close());Hr(out.cmd->Close());
            Rc(api.BeginRecordingExecution(context,j.handle,q[0].Get()));Submit(q[0].Get(),in);
            Rc(api.EnqueueHip(context,j.handle,q[0].Get()));Submit(q[0].Get(),out);
            auto hr=q[0]->Signal(tail.Get(),++value);Rc(api.EndRecordingExecution(context,j.handle,q[0].Get(),3,tail.Get(),value,hr));
            Wait(tail.Get(),value);changed.Check();retire(j);
        }
        Rc(api.Destroy(context));
        if(mode==L"normal") {
            controls.max_passes=0;
            Rc(api.Create(&ci,&context));Rc(api.PrepareSession(context));Rc(set(context,&controls));
            Require(run(2,2,false)==goldenTwo,"expanded two-pass output differs from independent cold build");Rc(api.Destroy(context));
            Rc(api.Create(&ci,&context));Rc(api.PrepareSession(context));Rc(set(context,&controls));
            Require(run(3,3,false)==three,"expanded three-pass output differs from independent cold build");Rc(api.Destroy(context));
        }
        FreeLibrary(dll);
        puts("MOCHIZUKI_PASS_SWITCH_OK");return 0;
    }
    auto destroyAfterOwnedTails = [&] {
        // The recorded NR tails are complete. Later unrelated game work must not
        // become a dependency of session destruction (NR off/backend switch).
        ComPtr<ID3D12Fence> unrelated;
        Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&unrelated)));
        for(auto& queue:q) Hr(queue->Wait(unrelated.Get(),1));
        std::atomic<bool> returned{false}, forcedRelease{false};
        std::thread release([&] {
            const auto deadline=GetTickCount64()+1500;
            while(!returned.load() && GetTickCount64()<deadline) Sleep(1);
            forcedRelease=!returned.load();
            unrelated->Signal(1);
        });
        const int rc=api.Destroy(context);
        returned=true;
        release.join();
        Rc(rc);context=nullptr;
        Require(!forcedRelease,"Destroy waited for unrelated game work after owned tails completed");
        puts("MOCHIZUKI_DESTROY_OWNED_TAILS_OK");
    };
    auto job=make(frame);List inputs(device.Get()),outputs(device.Get());Rc(api.RecordInputs(context,job.handle,inputs.cmd.Get()));Rc(api.RecordOutputs(context,job.handle,outputs.cmd.Get()));frame.Read(outputs.cmd.Get(),static_cast<ID3D12Resource*>(job.private_output));Hr(inputs.cmd->Close());Hr(outputs.cmd->Close());
    // A second Prepare must not invalidate the first closed recording.
    auto neverSubmitted=make(frame);Rc(api.InvalidateRecording(context,neverSubmitted.handle));Rc(api.CollectRecording(context,neverSubmitted.handle));
    Require(api.CollectRecording(context,neverSubmitted.handle)==LMXXF_NR_INVALID_ARGUMENT,"stale handle accepted");
    Require(api.Destroy(context)==LMXXF_NR_UNAVAILABLE,"live recording destroyed");
    double totalProfile=0,networkProfile=0,otherProfile=0;uint32_t profileSamples=0;uint64_t profileSerial=0;
    for(int i=0;i<(profile?80:8);i++) {
        auto* queue=q[i%2].Get();Rc(api.BeginRecordingExecution(context,job.handle,queue));Submit(queue,inputs);
        Rc(api.EnqueueHip(context,job.handle,queue));Submit(queue,outputs);HRESULT hr=queue->Signal(tail.Get(),++value);
        Rc(api.EndRecordingExecution(context,job.handle,queue,3,tail.Get(),value,hr));Wait(tail.Get(),value);
        if(profile && i>=16) {
            MochizukiNrInfo t {sizeof t};Rc(getInfo(context,&t));
            if(t.gpu_samples!=profileSerial) {
                Require(t.gpu_ms_network_last>0 && t.gpu_ms_other_last>=0 &&
                    std::abs(t.gpu_ms_last-t.gpu_ms_network_last-t.gpu_ms_other_last)<1e-4f,"invalid split timing");
                totalProfile+=t.gpu_ms_last;networkProfile+=t.gpu_ms_network_last;otherProfile+=t.gpu_ms_other_last;
                ++profileSamples;profileSerial=t.gpu_samples;
            }
        }
    }
    if(profile) {
        Require(profileSamples>=32,"not enough completed profile samples");
        printf("PROFILE %ux%u scale=%.2f passes=%u preprocess=%u compact=%u enlarge=%u samples=%u core=%.4f network=%.4f other=%.4f ms\n",
            frame.width,frame.height,profileScale,profilePasses,controls.preprocess,controls.compact_transfer,controls.enlarge_mode,profileSamples,
            totalProfile/profileSamples,networkProfile/profileSamples,otherProfile/profileSamples);
    }
    frame.Check();
    if(argc>3 && std::wstring(argv[3])==L"--destroy-tail") {
        Rc(api.InvalidateRecording(context,job.handle));Rc(api.CollectRecording(context,job.handle));
        destroyAfterOwnedTails();FreeLibrary(dll);return 0;
    }
    if(startup) {
        if(argc>4 && argv[4][0]) {
            FILE* file=nullptr;Require(_wfopen_s(&file,argv[4],L"wb")==0,"output dump open failed");
            void* pixels=nullptr;Hr(frame.readback->Map(0,nullptr,&pixels));
            for(UINT y=0;y<frame.height;y++)
                Require(fwrite(static_cast<char*>(pixels)+y*frame.footprint.Footprint.RowPitch,4,frame.width,file)==frame.width,"output dump write failed");
            frame.readback->Unmap(0,nullptr);fclose(file);
        }
        MochizukiNrInfo startupInfo {sizeof startupInfo}; Rc(getInfo(context, &startupInfo));
        PROCESS_MEMORY_COUNTERS memory {sizeof memory};
        Require(GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory) != FALSE, "startup peak RSS query failed");
        printf("STARTUP_BUILD seconds=%.3f peak_rss_bytes=%llu pipeline_cache_at_start=%s\n", startupInfo.build_seconds,
               static_cast<unsigned long long>(memory.PeakWorkingSetSize), startupCachePresent ? "warm" : "cold");
        Rc(api.InvalidateRecording(context,job.handle));Rc(api.CollectRecording(context,job.handle));Rc(api.Destroy(context));
        FreeLibrary(dll);printf("MOCHIZUKI_STARTUP_OK %ux%u R11G11B10_FLOAT\n",frame.width,frame.height);return 0;
    }
    MochizukiNrInfo timing {sizeof timing};Rc(getInfo(context,&timing));
    Require(timing.gpu_samples>0 && std::isfinite(timing.gpu_ms_last) && timing.gpu_ms_last>0,"missing completed network timing");
    Require(timing.gpu_ms_network_last>0 && timing.gpu_ms_other_last>=0 &&
            std::abs(timing.gpu_ms_last-timing.gpu_ms_network_last-timing.gpu_ms_other_last)<1e-4f,
            "split last timing is inconsistent");
    Require(std::abs(timing.gpu_ms_mean-timing.gpu_ms_network_mean-timing.gpu_ms_other_mean)<1e-4f,
            "split mean timing is inconsistent");
    Require(timing.gpu_ms_median >= timing.gpu_ms_network_median &&
            timing.gpu_ms_p95 >= timing.gpu_ms_network_p95 && timing.gpu_ms_network_p95 >= timing.gpu_ms_network_median,
            "total/network timing quantile contract changed");
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
    Rc(api.Destroy(context));
    // Auto mode with whole, differently sized allocations is still exact. A
    // one-frame excursion must not destroy the ready network or launch a build.
    context=nullptr;Rc(api.Create(&ci,&context));Rc(api.PrepareSession(context));Rc(set(context,&controls));
    {
        auto frameInfo=[](Frame& f, UINT validWidth=0) {
            MochizukiNrFrameInfo i {};i.struct_size=sizeof i;i.color=f.color.Get();
            i.color_width=validWidth?validWidth:f.width;i.color_height=f.height;
            i.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            i.flags=LMXXF_NR_FRAME_FLAG_STRENGTH;i.transfer_strength=i.color_strength=i.model_scale=1;i.passes=1;
            return i;
        };
        auto run=[&](Frame& f,LmxxfNrJob& j,UINT validWidth=0) {
            List in(device.Get()),out(device.Get());
            Rc(api.RecordInputs(context,j.handle,in.cmd.Get()));Rc(api.RecordOutputs(context,j.handle,out.cmd.Get()));
            f.Read(out.cmd.Get(),static_cast<ID3D12Resource*>(j.private_output));Hr(in.cmd->Close());Hr(out.cmd->Close());
            Rc(api.BeginRecordingExecution(context,j.handle,q[0].Get()));Submit(q[0].Get(),in);
            Rc(api.EnqueueHip(context,j.handle,q[0].Get()));Submit(q[0].Get(),out);
            auto hr=q[0]->Signal(tail.Get(),++value);
            Rc(api.EndRecordingExecution(context,j.handle,q[0].Get(),3,tail.Get(),value,hr));Wait(tail.Get(),value);
            f.Check(validWidth);Rc(api.InvalidateRecording(context,j.handle));Rc(api.CollectRecording(context,j.handle));
        };
        auto stable=make(frame);run(frame,stable);
        auto transient=frameInfo(resized),normal=frameInfo(frame);
        for(unsigned attempt=0;attempt<3;++attempt) {
            LmxxfNrJob candidate {sizeof candidate};
            Require(prepare(context,&transient,&candidate)==LMXXF_NR_UNAVAILABLE,"transient extent was accepted");
            char reason[256] {};api.GetLastError(reason,sizeof reason);
            Require(std::strstr(reason,"input resolution is settling")!=nullptr,"transient extent started rebuilding");
            MochizukiNrBuildProgress progress {sizeof progress};Rc(getProgress(context,&progress));
            Require(!progress.active,"transient extent launched a builder");
            MochizukiNrInfo current {sizeof current};Rc(getInfo(context,&current));
            Require(!current.building,"transient extent scheduled a builder");
            // Time between visits alone must not count as a sustained new size.
            Sleep(350);
            LmxxfNrJob resumed {sizeof resumed};Rc(prepare(context,&normal,&resumed));run(frame,resumed);
            Rc(getInfo(context,&current));
            Require(current.frame_w==frame.width && current.frame_h==frame.height && !current.building,
                    "normal frames lost their ready network");
        }
        // A sustained exact extent must eventually replace the old network.
        auto sustained=make(resized);run(resized,sustained);
        MochizukiNrInfo current {sizeof current};Rc(getInfo(context,&current));
        Require(current.frame_w==resized.width && current.frame_h==resized.height,"stable resize never applied");
        // Genuine DRS bucket growth must start at once, without exact-size settling.
        Frame larger(device.Get(),384,256);List up(device.Get());larger.Upload(up.cmd.Get());Hr(up.cmd->Close());
        Submit(q[0].Get(),up);Hr(q[0]->Signal(tail.Get(),++value));Wait(tail.Get(),value);
        auto growth=frameInfo(larger,352);LmxxfNrJob growing {sizeof growing};
        Require(prepare(context,&growth,&growing)==LMXXF_NR_UNAVAILABLE,"new bucket unexpectedly ready");
        // Build progress is published by the worker; Info.building is set before
        // PrepareFrame returns and does not depend on when that worker is scheduled.
        Rc(getInfo(context,&current));
        Require(current.building,"DRS bucket growth was delayed by exact-size settling");
        auto bucketJob=make(larger,352);run(larger,bucketJob,352);
        puts("MOCHIZUKI_EXTENT_SETTLE_OK excursions=3 immediate_resume=passed stable_resize=passed bucket_growth=immediate");
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
    destroyAfterOwnedTails();
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
