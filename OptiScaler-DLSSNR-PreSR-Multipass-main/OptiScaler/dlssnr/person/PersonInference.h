#pragma once
#include "PersonIpc.h"
#include "../../../../third_party/onnxruntime/onnxruntime_c_api.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace DlssNr::Person
{
constexpr unsigned ModelSize = 640, MaskSize = 160;
struct Image
{
    uint64_t epoch = 0, frame = 0, tick = 0;
    unsigned width = 0, height = 0;
    std::vector<float> rgb; // 1x3x640x640 RGB, letterboxed, [0,1].
};
struct Mask
{
    uint64_t epoch = 0, frame = 0, tick = 0;
    unsigned width = 0, height = 0;
    std::vector<float> values; // 160x160 letterboxed probabilities.
    double milliseconds = 0;
};
struct Detection { float x0, y0, x1, y1, score; unsigned index; };
inline float Intersection(const Detection& a, const Detection& b)
{
    const float intersection = (std::max)(0.f, (std::min)(a.x1,b.x1)-(std::max)(a.x0,b.x0)) *
                               (std::max)(0.f, (std::min)(a.y1,b.y1)-(std::max)(a.y0,b.y0));
    return intersection / (std::max)(1e-6f, (a.x1-a.x0)*(a.y1-a.y0)+(b.x1-b.x0)*(b.y1-b.y0)-intersection);
}
// YOLO11-seg export contract: [1,116,8400], [1,32,160,160], COCO person=0.
// The decoder is independent of inference and rejects NaNs before sorting/NMS.
inline std::vector<float> Decode(const float* detections, const float* prototypes)
{
    constexpr unsigned anchors = 8400, pixels = MaskSize * MaskSize;
    std::vector<Detection> candidates;
    for (unsigned i=0;i<anchors;++i) {
        const float score = detections[4*anchors+i];
        if (!std::isfinite(score) || score < .35f || score > 1) continue;
        bool person = true;
        for(unsigned c=1;c<80;++c) if(detections[(4+c)*anchors+i] > score) { person=false; break; }
        if(!person)continue;
        const float x=detections[i], y=detections[anchors+i], w=detections[2*anchors+i], h=detections[3*anchors+i];
        if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(w)||!std::isfinite(h)||w<=0||h<=0)continue;
        candidates.push_back({std::clamp((x-w*.5f)/4,0.f,160.f),std::clamp((y-h*.5f)/4,0.f,160.f),
                              std::clamp((x+w*.5f)/4,0.f,160.f),std::clamp((y+h*.5f)/4,0.f,160.f),score,i});
    }
    std::sort(candidates.begin(),candidates.end(),[](const auto&a,const auto&b){return a.score>b.score;});
    if(candidates.size()>128)candidates.resize(128);
    std::vector<Detection> selected;
    for(const auto& box:candidates) {
        if(std::any_of(selected.begin(),selected.end(),[&](const auto& b){return Intersection(box,b)>.45f;}))continue;
        selected.push_back(box); if(selected.size()==16)break;
    }
    std::vector<float> mask(pixels,0);
    for(const auto& b:selected)for(unsigned y=unsigned(b.y0);y<(std::min)(160u,unsigned(std::ceil(b.y1)));++y)
        for(unsigned x=unsigned(b.x0);x<(std::min)(160u,unsigned(std::ceil(b.x1)));++x) {
            float sum=0;
            for(unsigned c=0;c<32;++c)sum+=detections[(84+c)*anchors+b.index]*prototypes[c*pixels+y*160+x];
            if(std::isfinite(sum))mask[y*160+x]=(std::max)(mask[y*160+x],1.f/(1.f+std::exp(-std::clamp(sum,-20.f,20.f))));
        }
    return mask;
}
class Inference
{
    HMODULE dll = nullptr;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;
    std::string inputName;
    std::array<std::string,2> outputNames;
    void Check(OrtStatus* status) {
        if(!status)return;
        std::string message=api->GetErrorMessage(status); api->ReleaseStatus(status); throw std::runtime_error(message);
    }
    void Shape(bool input, size_t i, std::initializer_list<int64_t> expected) {
        OrtTypeInfo* type=nullptr;
        Check(input?api->SessionGetInputTypeInfo(session,i,&type):api->SessionGetOutputTypeInfo(session,i,&type));
        struct Guard {const OrtApi*a;OrtTypeInfo*p;~Guard(){a->ReleaseTypeInfo(p);}} guard{api,type};
        const OrtTensorTypeAndShapeInfo* tensor=nullptr; Check(api->CastTypeInfoToTensorInfo(type,&tensor));
        if(!tensor)throw std::runtime_error("model tensor type");
        ONNXTensorElementDataType element; Check(api->GetTensorElementType(tensor,&element));
        size_t rank=0; Check(api->GetDimensionsCount(tensor,&rank));
        if(rank!=expected.size()||element!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)throw std::runtime_error("requires YOLO11n-seg FP32 640 export");
        std::vector<int64_t> dims(rank);Check(api->GetDimensions(tensor,dims.data(),dims.size()));
        if(!std::equal(dims.begin(),dims.end(),expected.begin()))throw std::runtime_error("unsupported person model dimensions");
    }
public:
    ~Inference() {
        if(api){if(memory)api->ReleaseMemoryInfo(memory);if(session)api->ReleaseSession(session);if(env)api->ReleaseEnv(env);}
        if(dll)FreeLibrary(dll);
    }
    void Open(const std::filesystem::path& directory) {
        const auto library=std::filesystem::absolute(directory/L"onnxruntime.dll");
        if(!std::filesystem::is_regular_file(library))throw std::runtime_error("missing person-model/onnxruntime.dll (ONNX Runtime 1.23+ CPU x64)");
        const auto model=directory/L"yolo11n-seg.onnx";
        if(!std::filesystem::is_regular_file(model))throw std::runtime_error("missing person-model/yolo11n-seg.onnx (FP32 640, COCO)");
        dll=LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!dll){
            dll=LoadLibraryW(library.c_str());
            if(!dll)throw std::runtime_error("cannot load ONNX Runtime CPU x64");
        }
        const auto get=reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(GetProcAddress(dll,"OrtGetApiBase"));
        api=get?get()->GetApi(ORT_API_VERSION):nullptr;
        if(!api)throw std::runtime_error("ONNX Runtime C API 23 unavailable");
        Check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR,"OptScaler person",&env));
        OrtSessionOptions* options=nullptr; Check(api->CreateSessionOptions(&options));
        struct Guard {const OrtApi*a;OrtSessionOptions*p;~Guard(){a->ReleaseSessionOptions(p);}} guard{api,options};
        Check(api->SetIntraOpNumThreads(options,2)); Check(api->SetInterOpNumThreads(options,1));
        Check(api->SetSessionExecutionMode(options,ORT_SEQUENTIAL));
        Check(api->SetSessionGraphOptimizationLevel(options,ORT_ENABLE_ALL));
        Check(api->CreateSession(env,model.c_str(),options,&session));
        size_t count=0;Check(api->SessionGetInputCount(session,&count));
        if(count!=1)throw std::runtime_error("person model must have one input");
        Check(api->SessionGetOutputCount(session,&count));if(count!=2)throw std::runtime_error("person model must have two outputs");
        Shape(true,0,{1,3,640,640});Shape(false,0,{1,116,8400});Shape(false,1,{1,32,160,160});
        OrtAllocator* allocator=nullptr;Check(api->GetAllocatorWithDefaultOptions(&allocator));
        char* name=nullptr;Check(api->SessionGetInputName(session,0,allocator,&name)); inputName=name;allocator->Free(allocator,name);
        for(size_t i=0;i<2;++i){name=nullptr;Check(api->SessionGetOutputName(session,i,allocator,&name));outputNames[i]=name;allocator->Free(allocator,name);}
        Check(api->CreateCpuMemoryInfo(OrtArenaAllocator,OrtMemTypeDefault,&memory));
    }
    Mask Run(Image& image) {
        if(image.rgb.size()!=3*ModelSize*ModelSize)throw std::runtime_error("person input size");
        const int64_t dims[]={1,3,640,640};OrtValue* input=nullptr;std::array<OrtValue*,2> output{};
        struct Guard {const OrtApi*a;OrtValue*&in;std::array<OrtValue*,2>&out;~Guard(){if(in)a->ReleaseValue(in);for(auto*p:out)if(p)a->ReleaseValue(p);}} guard{api,input,output};
        Check(api->CreateTensorWithDataAsOrtValue(memory,image.rgb.data(),image.rgb.size()*sizeof(float),dims,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&input));
        const char* inputs[]={inputName.c_str()};const char* outputs[]={outputNames[0].c_str(),outputNames[1].c_str()};
        const OrtValue* in=input;
        const auto start=GetTickCount64();
        Check(api->Run(session,nullptr,inputs,&in,1,outputs,2,output.data()));
        float *detections=nullptr,*prototypes=nullptr;
        Check(api->GetTensorMutableData(output[0],reinterpret_cast<void**>(&detections)));
        Check(api->GetTensorMutableData(output[1],reinterpret_cast<void**>(&prototypes)));
        Mask mask{image.epoch,image.frame,image.tick,image.width,image.height,Decode(detections,prototypes)};
        mask.milliseconds=double(GetTickCount64()-start);return mask;
    }
};

class Provider
{
    mutable std::mutex mutex;
    std::filesystem::path directory;
    bool active = false;
    bool busy = false;
    bool failed = false;
    std::string status = "off";
    std::shared_ptr<Mask> result;

    HANDLE hShm = nullptr;
    void* shmBase = nullptr;
    Ipc::ShmHeader* header = nullptr;
    HANDLE hReqEvent = nullptr;
    HANDLE hRespEvent = nullptr;
    HANDLE hStopEvent = nullptr;
    HANDLE hJob = nullptr;
    PROCESS_INFORMATION processInfo{};

    std::thread receiverThread;
    std::atomic<bool> stopping{false};

    void StopWorker()
    {
        stopping.store(true, std::memory_order_relaxed);
        if (hStopEvent) SetEvent(hStopEvent);
        if (receiverThread.joinable())
        {
            receiverThread.join();
        }
        if (processInfo.hProcess)
        {
            WaitForSingleObject(processInfo.hProcess, 500);
            CloseHandle(processInfo.hProcess);
            processInfo.hProcess = nullptr;
        }
        if (processInfo.hThread)
        {
            CloseHandle(processInfo.hThread);
            processInfo.hThread = nullptr;
        }
        if (hJob) { CloseHandle(hJob); hJob = nullptr; }
        if (hReqEvent) { CloseHandle(hReqEvent); hReqEvent = nullptr; }
        if (hRespEvent) { CloseHandle(hRespEvent); hRespEvent = nullptr; }
        if (hStopEvent) { CloseHandle(hStopEvent); hStopEvent = nullptr; }
        if (shmBase) { UnmapViewOfFile(shmBase); shmBase = nullptr; header = nullptr; }
        if (hShm) { CloseHandle(hShm); hShm = nullptr; }
    }

    bool StartWorker()
    {
        uint32_t pid = GetCurrentProcessId();
        std::wstring shmName = Ipc::ShmName(pid);
        std::wstring reqName = Ipc::ReqEventName(pid);
        std::wstring respName = Ipc::RespEventName(pid);
        std::wstring stopName = Ipc::StopEventName(pid);

        hShm = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(Ipc::TotalShmSize), shmName.c_str());
        if (!hShm)
        {
            status = "cannot create person shm";
            failed = true;
            return false;
        }

        shmBase = MapViewOfFile(hShm, FILE_MAP_ALL_ACCESS, 0, 0, Ipc::TotalShmSize);
        if (!shmBase)
        {
            status = "cannot map person shm";
            failed = true;
            return false;
        }

        memset(shmBase, 0, sizeof(Ipc::ShmHeader));
        header = reinterpret_cast<Ipc::ShmHeader*>(shmBase);
        header->magic = Ipc::ShmMagic;
        header->version = Ipc::ShmVersion;
        header->state = static_cast<uint32_t>(Ipc::WorkerState::Uninitialized);
        header->reqRgbOffset = static_cast<uint32_t>(Ipc::InputRgbOffset);
        header->reqRgbBytes = static_cast<uint32_t>(Ipc::InputRgbSize);
        header->respMaskOffset = static_cast<uint32_t>(Ipc::OutputMaskOffset);
        header->respMaskBytes = static_cast<uint32_t>(Ipc::OutputMaskSize);

        hReqEvent = CreateEventW(nullptr, FALSE, FALSE, reqName.c_str());
        hRespEvent = CreateEventW(nullptr, FALSE, FALSE, respName.c_str());
        hStopEvent = CreateEventW(nullptr, TRUE, FALSE, stopName.c_str());
        if (!hReqEvent || !hRespEvent || !hStopEvent)
        {
            status = "cannot create person events";
            failed = true;
            return false;
        }

        hJob = CreateJobObjectW(nullptr, nullptr);
        if (hJob)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        }

        std::filesystem::path workerExe = directory / L"person-worker.exe";
        if (!std::filesystem::is_regular_file(workerExe))
        {
            status = "missing person-model/person-worker.exe";
            failed = true;
            return false;
        }

        unsigned hw = std::thread::hardware_concurrency();
        unsigned threads = (std::max)(2u, (std::min)(8u, hw ? (hw / 2) : 4u));
        std::wstring cmd = L"\"" + workerExe.wstring() + L"\" --pid " + std::to_wstring(pid) +
                           L" --threads " + std::to_wstring(threads);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        DWORD flags = CREATE_NO_WINDOW;
        if (hJob) flags |= CREATE_SUSPENDED;

        std::vector<wchar_t> cmdLine(cmd.begin(), cmd.end());
        cmdLine.push_back(0);

        if (!CreateProcessW(workerExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, flags, nullptr, directory.c_str(), &si, &processInfo))
        {
            DWORD err = GetLastError();
            status = "failed to start person worker (" + std::to_string(err) + ")";
            failed = true;
            return false;
        }

        if (hJob)
        {
            AssignProcessToJobObject(hJob, processInfo.hProcess);
            ResumeThread(processInfo.hThread);
        }

        stopping.store(false, std::memory_order_relaxed);
        receiverThread = std::thread(&Provider::ReceiverLoop, this);
        return true;
    }

    void ReceiverLoop()
    {
        HANDLE handles[3] = { hRespEvent, processInfo.hProcess, hStopEvent };
        while (!stopping.load(std::memory_order_relaxed))
        {
            DWORD wr = WaitForMultipleObjects(3, handles, FALSE, 200);
            if (wr == WAIT_OBJECT_0) // Response received
            {
                std::lock_guard lock(mutex);
                if (!header) break;

                if (header->state == static_cast<uint32_t>(Ipc::WorkerState::Ready))
                {
                    if (header->respFrame > 0 && header->respMaskBytes == Ipc::OutputMaskSize)
                    {
                        auto mask = std::make_shared<Mask>();
                        mask->epoch = header->respEpoch;
                        mask->frame = header->respFrame;
                        mask->tick = header->respTick;
                        mask->width = header->respWidth;
                        mask->height = header->respHeight;
                        mask->milliseconds = header->computeMilliseconds;
                        mask->values.resize(MaskSize * MaskSize);
                        memcpy(mask->values.data(),
                               reinterpret_cast<const uint8_t*>(shmBase) + header->respMaskOffset,
                               Ipc::OutputMaskSize);
                        result = std::move(mask);
                        status = "person mask ready";
                    }
                    else
                    {
                        status = header->statusMessage[0] ? header->statusMessage : "person model ready";
                    }
                    failed = false;
                }
                else if (header->state == static_cast<uint32_t>(Ipc::WorkerState::Error))
                {
                    failed = true;
                    status = header->statusMessage[0] ? header->statusMessage : "person worker error";
                }
                busy = false;
            }
            else if (wr == WAIT_OBJECT_0 + 1) // Worker process died
            {
                std::lock_guard lock(mutex);
                DWORD exitCode = 0;
                if (processInfo.hProcess) GetExitCodeProcess(processInfo.hProcess, &exitCode);
                failed = true;
                busy = false;
                status = "person worker exited (" + std::to_string(exitCode) + ")";
                break;
            }
            else if (wr == WAIT_OBJECT_0 + 2) // Stop event signaled
            {
                break;
            }
            else if (wr == WAIT_TIMEOUT)
            {
                continue;
            }
            else
            {
                break;
            }
        }
    }

public:
    ~Provider()
    {
        StopWorker();
    }

    void Configure(bool enabled, const std::filesystem::path& dir)
    {
        std::lock_guard lock(mutex);
        if (enabled == active && directory == dir && (!enabled || (!failed && processInfo.hProcess)))
        {
            return;
        }

        active = enabled;
        directory = dir;
        result.reset();
        busy = false;

        if (!enabled)
        {
            StopWorker();
            status = "off";
            failed = false;
        }
        else
        {
            StopWorker();
            failed = false;
            status = "loading person model";
            StartWorker();
        }
    }

    bool Available()
    {
        std::lock_guard lock(mutex);
        return active && !failed && processInfo.hProcess != nullptr && header &&
               (header->state == static_cast<uint32_t>(Ipc::WorkerState::Ready) ||
                header->state == static_cast<uint32_t>(Ipc::WorkerState::Processing));
    }

    bool Ready()
    {
        std::lock_guard lock(mutex);
        return active && !busy && !failed && processInfo.hProcess != nullptr && header &&
               (header->state == static_cast<uint32_t>(Ipc::WorkerState::Ready));
    }

    bool Submit(std::shared_ptr<Image> image)
    {
        if (!image) return false;
        std::lock_guard lock(mutex);
        if (!active || busy || failed || !processInfo.hProcess || !header ||
            header->state != static_cast<uint32_t>(Ipc::WorkerState::Ready))
        {
            return false;
        }
        if (image->rgb.size() * sizeof(float) != Ipc::InputRgbSize)
        {
            return false;
        }

        header->reqEpoch = image->epoch;
        header->reqFrame = image->frame;
        header->reqTick = image->tick;
        header->reqWidth = image->width;
        header->reqHeight = image->height;

        memcpy(reinterpret_cast<uint8_t*>(shmBase) + header->reqRgbOffset,
               image->rgb.data(), Ipc::InputRgbSize);

        busy = true;
        SetEvent(hReqEvent);
        return true;
    }

    std::shared_ptr<const Mask> Latest()
    {
        std::lock_guard lock(mutex);
        return result;
    }

    std::string Status()
    {
        std::lock_guard lock(mutex);
        return status + (result ? " | CPU " + std::to_string(unsigned(result->milliseconds)) + " ms" : "");
    }
};

inline Provider& Worker()
{
    static auto* provider = new Provider;
    return *provider;
}
}
