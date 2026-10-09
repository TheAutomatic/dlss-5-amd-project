#pragma once
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
    // Accumulate one channel across contiguous pixels. This keeps prototype reads
    // cache-friendly without changing the export's bounding-box crop or values.
    for (const auto& b : selected) {
        const unsigned x0=unsigned(b.x0), x1=(std::min)(160u,unsigned(std::ceil(b.x1)));
        for (unsigned y=unsigned(b.y0);y<(std::min)(160u,unsigned(std::ceil(b.y1)));++y) {
            float sums[160]{};
            for (unsigned c=0;c<32;++c) {
                const float coefficient=detections[(84+c)*anchors+b.index];
                const float* row=prototypes+c*pixels+y*160;
                for (unsigned x=x0;x<x1;++x)sums[x]+=coefficient*row[x];
            }
            for (unsigned x=x0;x<x1;++x)if(std::isfinite(sums[x]))
                mask[y*160+x]=(std::max)(mask[y*160+x],1.f/(1.f+std::exp(-std::clamp(sums[x],-20.f,20.f))));
        }
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
    void Open(const std::filesystem::path& directory, unsigned threads = 2) {
        const auto library=std::filesystem::absolute(directory/L"onnxruntime.dll");
        if(!std::filesystem::is_regular_file(library))throw std::runtime_error("missing person-model/onnxruntime.dll (ONNX Runtime 1.23+ CPU x64)");
        const auto model=directory/L"yolo11n-seg.onnx";
        if(!std::filesystem::is_regular_file(model))throw std::runtime_error("missing person-model/yolo11n-seg.onnx (FP32 640, COCO)");
        dll=LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!dll)throw std::runtime_error("cannot load ONNX Runtime CPU x64 (Win32 " + std::to_string(GetLastError()) + ")");
        const auto get=reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(GetProcAddress(dll,"OrtGetApiBase"));
        api=get?get()->GetApi(ORT_API_VERSION):nullptr;
        if(!api)throw std::runtime_error("ONNX Runtime C API 23 unavailable");
        Check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR,"OptScaler person",&env));
        OrtSessionOptions* options=nullptr; Check(api->CreateSessionOptions(&options));
        struct Guard {const OrtApi*a;OrtSessionOptions*p;~Guard(){a->ReleaseSessionOptions(p);}} guard{api,options};
        Check(api->SetIntraOpNumThreads(options,static_cast<int>(threads))); Check(api->SetInterOpNumThreads(options,1));
        Check(api->SetSessionExecutionMode(options,ORT_SEQUENTIAL));
        Check(api->AddSessionConfigEntry(options,"session.intra_op.allow_spinning","0"));
        Check(api->AddSessionConfigEntry(options,"session.inter_op.allow_spinning","0"));
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
    Mask RunRgb(float* rgb) {
        const int64_t dims[]={1,3,640,640};OrtValue* input=nullptr;std::array<OrtValue*,2> output{};
        struct Guard {const OrtApi*a;OrtValue*&in;std::array<OrtValue*,2>&out;~Guard(){if(in)a->ReleaseValue(in);for(auto*p:out)if(p)a->ReleaseValue(p);}} guard{api,input,output};
        Check(api->CreateTensorWithDataAsOrtValue(memory,rgb,3*ModelSize*ModelSize*sizeof(float),dims,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&input));
        const char* inputs[]={inputName.c_str()};const char* outputs[]={outputNames[0].c_str(),outputNames[1].c_str()};
        const OrtValue* in=input;
        const auto start=GetTickCount64();
        Check(api->Run(session,nullptr,inputs,&in,1,outputs,2,output.data()));
        float *detections=nullptr,*prototypes=nullptr;
        Check(api->GetTensorMutableData(output[0],reinterpret_cast<void**>(&detections)));
        Check(api->GetTensorMutableData(output[1],reinterpret_cast<void**>(&prototypes)));
        Mask mask{};mask.values=Decode(detections,prototypes);
        mask.milliseconds=double(GetTickCount64()-start);return mask;
    }
    Mask Run(Image& image) {
        if(image.rgb.size()!=3*ModelSize*ModelSize)throw std::runtime_error("person input size");
        auto mask=RunRgb(image.rgb.data());mask.epoch=image.epoch;mask.frame=image.frame;mask.tick=image.tick;
        mask.width=image.width;mask.height=image.height;return mask;
    }
};

}
