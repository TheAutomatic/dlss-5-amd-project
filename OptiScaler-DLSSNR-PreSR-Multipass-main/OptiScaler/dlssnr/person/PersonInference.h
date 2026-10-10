#pragma once
#include "PersonIpc.h"
#include "PersonModel.h"
#include "WorkerPolicy.h"

namespace DlssNr::Person
{
class Provider
{
    // Configure/teardown may wait for the receiver. Never join while holding mutex.
    std::mutex lifecycle;
    mutable std::mutex mutex;
    std::filesystem::path directory;
    std::wstring targetModel;
    unsigned targetFaceSize=320;
    bool active=false, busy=false, failed=false, initialized=false;
    std::string status="off";
    std::shared_ptr<Mask> result;
    std::shared_ptr<Image> request;
    uint64_t started=0, submitted=0;
    HANDLE mapping=nullptr, response=nullptr, stop=nullptr, ready=nullptr, job=nullptr;
    HMODULE receiverModule=nullptr;
    Ipc::ShmHeader* header=nullptr;
    PROCESS_INFORMATION process{};
    std::thread receiver;
    std::atomic<bool> stopping{false};

    void Fail(std::string message) { failed=true;busy=false;status=std::move(message); }
    void StopWorker()
    {
        stopping.store(true);
        if(stop)SetEvent(stop);
        if(receiver.joinable())receiver.join();
        if(process.hProcess&&WaitForSingleObject(process.hProcess,500)==WAIT_TIMEOUT&&job)
            TerminateJobObject(job,1);
        // A unique live worker must own these named IPC objects before reopening.
        if(process.hProcess)WaitForSingleObject(process.hProcess,2000);
        for(auto h:{process.hThread,process.hProcess,job,ready,response,stop})if(h)CloseHandle(h);
        process={};job=ready=response=stop=nullptr;
        if(header)UnmapViewOfFile(header);
        header=nullptr;
        if(mapping)CloseHandle(mapping);
        mapping=nullptr;
        if(receiverModule)FreeLibrary(receiverModule);
        receiverModule=nullptr;
    }
    void StartWorker()
    {
        auto require=[&](bool value,const char* message){if(!value)throw std::runtime_error(std::string(message)+" ("+std::to_string(GetLastError())+")");};
        const auto exe=directory/L"person-worker.exe";
        if(!std::filesystem::is_regular_file(exe))throw std::runtime_error("missing person-model/person-worker.exe; install the complete package");
        const auto pid=GetCurrentProcessId();
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,DWORD(Ipc::TotalShmSize),Ipc::ShmName(pid).c_str());
        require(mapping!=nullptr,"cannot create person IPC");
        require(GetLastError()!=ERROR_ALREADY_EXISTS,"person IPC is already owned");
        header=static_cast<Ipc::ShmHeader*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,Ipc::TotalShmSize));
        require(header!=nullptr,"cannot map person IPC");
        *header={};header->magic=Ipc::ShmMagic;header->version=Ipc::ShmVersion;
        header->reqRgbOffset=uint32_t(Ipc::InputRgbOffset);header->reqRgbBytes=uint32_t(Ipc::InputRgbSize);
        header->respMaskOffset=uint32_t(Ipc::OutputMaskOffset);header->respMaskBytes=uint32_t(Ipc::OutputMaskSize);
        ready=CreateEventW(nullptr,FALSE,FALSE,Ipc::ReqEventName(pid).c_str());
        response=CreateEventW(nullptr,FALSE,FALSE,Ipc::RespEventName(pid).c_str());
        stop=CreateEventW(nullptr,TRUE,FALSE,Ipc::StopEventName(pid).c_str());
        require(ready&&response&&stop,"cannot create person events");
        job=CreateJobObjectW(nullptr,nullptr);require(job!=nullptr,"cannot create person job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"cannot configure person job");
        std::wstring command=L"\""+exe.wstring()+L"\" --pid "+std::to_wstring(pid)+L" --threads "+
            std::to_wstring(WorkerThreads(std::thread::hardware_concurrency()));
        if(!targetModel.empty()){
            command+=L" --model \""+targetModel+L"\"";
        }
        command+=L" --face-size "+std::to_wstring(targetFaceSize);
        STARTUPINFOW startup{};startup.cb=sizeof(startup);
        require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED|NORMAL_PRIORITY_CLASS,
            nullptr,directory.c_str(),&startup,&process),"cannot start person worker");
        if(!AssignProcessToJobObject(job,process.hProcess)){
            const auto error=GetLastError();TerminateProcess(process.hProcess,error);SetLastError(error);
            require(false,"cannot own person worker lifetime");
        }
        require(ResumeThread(process.hThread)!=DWORD(-1),"cannot resume person worker");
        require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&Ipc::ValidHeader),&receiverModule),"cannot retain person receiver module");
        stopping.store(false);started=GetTickCount64();
        receiver=std::thread(&Provider::Receive,this);
    }
    void Receive()
    {
        // Stop wins when a response and shutdown arrive together.
        HANDLE handles[]={stop,response,process.hProcess};
        try {
            while(!stopping.load()){
                const auto wait=WaitForMultipleObjects(3,handles,FALSE,200);
                if(wait==WAIT_OBJECT_0)break;
                std::lock_guard lock(mutex);
                if(wait==WAIT_OBJECT_0+1){
                    if(!Ipc::ValidHeader(*header)||header->flags!=Ipc::ShmVersion){Fail("person worker protocol mismatch; install the complete package");break;}
                    if(header->state==uint32_t(Ipc::WorkerState::Error)){
                        Fail(std::string(header->statusMessage,strnlen_s(header->statusMessage,sizeof(header->statusMessage))));break;
                    }
                    if(header->state!=uint32_t(Ipc::WorkerState::Ready)){Fail("unexpected person worker response");break;}
                    if(!initialized){initialized=true;status="person model ready";continue;}
                    if(!busy||!request||header->respEpoch!=request->epoch||header->respFrame!=request->frame||
                       header->respTick!=request->tick||header->respWidth!=request->width||header->respHeight!=request->height){
                        Fail("person worker returned a mismatched frame");break;
                    }
                    auto mask=std::make_shared<Mask>();
                    mask->epoch=request->epoch;mask->frame=request->frame;mask->tick=request->tick;
                    mask->width=request->width;mask->height=request->height;mask->milliseconds=header->computeMilliseconds;
                    mask->values.resize(MaskSize*MaskSize);
                    memcpy(mask->values.data(),reinterpret_cast<const uint8_t*>(header)+Ipc::OutputMaskOffset,Ipc::OutputMaskSize);
                    if(!std::isfinite(mask->milliseconds)||mask->milliseconds<0||mask->milliseconds>60000||
                       !std::all_of(mask->values.begin(),mask->values.end(),[](float v){return std::isfinite(v)&&v>=0&&v<=1;})){
                        Fail("person worker returned invalid mask values");break;
                    }
                    result=std::move(mask);request.reset();busy=false;status="person mask ready";
                }else if(wait==WAIT_OBJECT_0+2){
                    DWORD code=0;GetExitCodeProcess(process.hProcess,&code);Fail("person worker exited ("+std::to_string(code)+")");break;
                }else if(wait==WAIT_TIMEOUT){
                    const auto now=GetTickCount64();
                    if((!initialized&&now-started>30000)||(busy&&now-submitted>5000)){
                        Fail("person worker timed out; toggle person protection to retry");SetEvent(stop);break;
                    }
                }else{Fail("person worker wait failed");break;}
            }
        }catch(...){std::lock_guard lock(mutex);Fail("person worker response failed");}
    }
public:
    ~Provider(){StopWorker();}
    void Configure(bool enabled,const std::filesystem::path& dir,const std::wstring& modelName=L"", unsigned faceSize=320)
    {
        faceSize=Face::BoundedSize(faceSize);
        std::lock_guard serial(lifecycle);
        const auto absolute=std::filesystem::absolute(dir);
        {
            std::lock_guard lock(mutex);
            // Failures latch. Rendering must not launch a new process every frame.
            if(enabled==active&&directory==absolute&&targetModel==modelName&&targetFaceSize==faceSize)return;
            active=false;
        }
        StopWorker();
        bool cleanup=false;
        {
            std::lock_guard lock(mutex);
            directory=absolute;targetModel=modelName;targetFaceSize=faceSize;active=enabled;failed=busy=initialized=false;result.reset();request.reset();
            status=enabled?"loading person model":"off";
            if(enabled)try{StartWorker();}catch(const std::exception&e){Fail(e.what());cleanup=true;}
        }
        if(cleanup)StopWorker();
    }
    bool Available(){std::lock_guard lock(mutex);return active&&!failed&&initialized;}
    bool Ready(){std::lock_guard lock(mutex);return active&&!failed&&initialized&&!busy;}
    bool Submit(std::shared_ptr<Image> image)
    {
        std::lock_guard lock(mutex);
        if(!active||failed||!initialized||busy||!image||image->rgb.size()*sizeof(float)!=Ipc::InputRgbSize)return false;
        header->reqEpoch=image->epoch;header->reqFrame=image->frame;header->reqTick=image->tick;
        header->reqWidth=image->width;header->reqHeight=image->height;
        memcpy(reinterpret_cast<uint8_t*>(header)+Ipc::InputRgbOffset,image->rgb.data(),Ipc::InputRgbSize);
        request=std::move(image);busy=true;submitted=GetTickCount64();
        if(!SetEvent(ready)){Fail("cannot submit person frame");return false;}
        return true;
    }
    std::shared_ptr<const Mask> Latest(){std::lock_guard lock(mutex);return active&&!failed?result:nullptr;}
    std::string Status(){std::lock_guard lock(mutex);return status+(result?" | CPU "+std::to_string(unsigned(result->milliseconds))+" ms":"");}
};
inline Provider& Worker(){static auto* provider=new Provider;return *provider;}
}
