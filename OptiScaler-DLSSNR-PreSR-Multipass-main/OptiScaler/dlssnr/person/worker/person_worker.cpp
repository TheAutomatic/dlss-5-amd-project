#include "../PersonIpc.h"
#include "../../../../../third_party/onnxruntime/onnxruntime_c_api.h"

#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::filesystem::path g_exeDir;

void Log(const std::string& msg)
{
    try
    {
        std::filesystem::path logPath = g_exeDir / L"person-worker.log";
        std::ofstream out(logPath, std::ios::app);
        if (out)
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char timeBuf[64];
            snprintf(timeBuf, sizeof(timeBuf), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            out << timeBuf << msg << "\n";
        }
    }
    catch (...) {}
}

struct Detection
{
    float x0, y0, x1, y1, score;
    unsigned index;
};

inline float Intersection(const Detection& a, const Detection& b)
{
    const float intersection = (std::max)(0.f, (std::min)(a.x1, b.x1) - (std::max)(a.x0, b.x0)) *
                               (std::max)(0.f, (std::min)(a.y1, b.y1) - (std::max)(a.y0, b.y0));
    return intersection / (std::max)(1e-6f, (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - intersection);
}

// YOLO11-seg export contract: [1, 116, 8400], [1, 32, 160, 160], COCO person=0.
// Directly writes probabilities into outMask (160x160 floats).
inline void Decode(const float* detections, const float* prototypes, float* outMask)
{
    constexpr unsigned anchors = 8400;
    constexpr unsigned maskDim = DlssNr::Person::Ipc::MaskWidth;
    constexpr unsigned pixels = maskDim * maskDim;

    std::vector<Detection> candidates;
    for (unsigned i = 0; i < anchors; ++i)
    {
        const float score = detections[4 * anchors + i];
        if (!std::isfinite(score) || score < 0.35f || score > 1.0f)
            continue;

        bool person = true;
        for (unsigned c = 1; c < 80; ++c)
        {
            if (detections[(4 + c) * anchors + i] > score)
            {
                person = false;
                break;
            }
        }
        if (!person)
            continue;

        const float x = detections[i];
        const float y = detections[anchors + i];
        const float w = detections[2 * anchors + i];
        const float h = detections[3 * anchors + i];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) || w <= 0.0f || h <= 0.0f)
            continue;

        candidates.push_back({
            std::clamp((x - w * 0.5f) / 4.0f, 0.0f, 160.0f),
            std::clamp((y - h * 0.5f) / 4.0f, 0.0f, 160.0f),
            std::clamp((x + w * 0.5f) / 4.0f, 0.0f, 160.0f),
            std::clamp((y + h * 0.5f) / 4.0f, 0.0f, 160.0f),
            score,
            i
        });
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.score > b.score;
    });
    if (candidates.size() > 128)
        candidates.resize(128);

    std::vector<Detection> selected;
    for (const auto& box : candidates)
    {
        if (std::any_of(selected.begin(), selected.end(), [&](const auto& b) {
            return Intersection(box, b) > 0.45f;
        }))
            continue;

        selected.push_back(box);
        if (selected.size() == 16)
            break;
    }

    std::fill_n(outMask, pixels, 0.0f);
    for (const auto& b : selected)
    {
        const unsigned y0 = unsigned((std::max)(0.0f, std::floor(b.y0 - 2.0f)));
        const unsigned y1 = (std::min)(160u, unsigned(std::ceil(b.y1 + 2.0f)));
        const unsigned x0 = unsigned((std::max)(0.0f, std::floor(b.x0 - 2.0f)));
        const unsigned x1 = (std::min)(160u, unsigned(std::ceil(b.x1 + 2.0f)));

        float coeffs[32];
        for (unsigned c = 0; c < 32; ++c)
        {
            coeffs[c] = detections[(84 + c) * anchors + b.index];
        }

        for (unsigned y = y0; y < y1; ++y)
        {
            const unsigned rowOffset = y * 160;
            for (unsigned x = x0; x < x1; ++x)
            {
                const unsigned pixelIdx = rowOffset + x;
                float sum = 0.0f;
                for (unsigned c = 0; c < 32; ++c)
                {
                    sum += coeffs[c] * prototypes[c * pixels + pixelIdx];
                }
                if (std::isfinite(sum))
                {
                    const float prob = 1.0f / (1.0f + std::exp(-std::clamp(sum, -20.0f, 20.0f)));
                    if (prob > outMask[pixelIdx])
                    {
                        outMask[pixelIdx] = prob;
                    }
                }
            }
        }
    }
}

class Inference
{
    HMODULE dll = nullptr;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;
    std::string inputName;
    std::array<std::string, 2> outputNames;

    void Check(OrtStatus* status)
    {
        if (!status) return;
        std::string message = api->GetErrorMessage(status);
        api->ReleaseStatus(status);
        throw std::runtime_error(message);
    }

    void Shape(bool input, size_t i, std::initializer_list<int64_t> expected)
    {
        OrtTypeInfo* type = nullptr;
        Check(input ? api->SessionGetInputTypeInfo(session, i, &type)
                    : api->SessionGetOutputTypeInfo(session, i, &type));
        struct Guard { const OrtApi* a; OrtTypeInfo* p; ~Guard() { a->ReleaseTypeInfo(p); } } guard{ api, type };
        const OrtTensorTypeAndShapeInfo* tensor = nullptr;
        Check(api->CastTypeInfoToTensorInfo(type, &tensor));
        if (!tensor) throw std::runtime_error("model tensor type error");
        ONNXTensorElementDataType element;
        Check(api->GetTensorElementType(tensor, &element));
        size_t rank = 0;
        Check(api->GetDimensionsCount(tensor, &rank));
        if (rank != expected.size() || element != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error("requires YOLO11n-seg FP32 640 export");
        std::vector<int64_t> dims(rank);
        Check(api->GetDimensions(tensor, dims.data(), dims.size()));
        if (!std::equal(dims.begin(), dims.end(), expected.begin()))
            throw std::runtime_error("unsupported person model dimensions");
    }

public:
    ~Inference()
    {
        if (api)
        {
            if (memory) api->ReleaseMemoryInfo(memory);
            if (session) api->ReleaseSession(session);
            if (env) api->ReleaseEnv(env);
        }
        if (dll) FreeLibrary(dll);
    }

    void Open(const std::filesystem::path& directory, const std::filesystem::path& customModelPath, unsigned threadCount = 2)
    {
        const auto library = std::filesystem::absolute(directory / L"onnxruntime.dll");
        if (!std::filesystem::is_regular_file(library))
            throw std::runtime_error("missing person-model/onnxruntime.dll (ONNX Runtime 1.23+ CPU x64)");

        auto model = customModelPath.empty() ? (directory / L"yolo11n-seg.onnx") : customModelPath;
        if (!std::filesystem::is_regular_file(model))
            throw std::runtime_error("missing person-model/yolo11n-seg.onnx (FP32 640, COCO)");

        dll = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll)
        {
            dll = LoadLibraryW(library.c_str());
            if (!dll)
            {
                DWORD err = GetLastError();
                throw std::runtime_error("cannot load ONNX Runtime CPU x64 (Win32 Error " + std::to_string(err) + ")");
            }
        }

        const auto get = reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(GetProcAddress(dll, "OrtGetApiBase"));
        api = get ? get()->GetApi(ORT_API_VERSION) : nullptr;
        if (!api)
            throw std::runtime_error("ONNX Runtime C API version unavailable");

        Check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "OptScaler person worker", &env));

        OrtSessionOptions* options = nullptr;
        Check(api->CreateSessionOptions(&options));
        struct Guard { const OrtApi* a; OrtSessionOptions* p; ~Guard() { a->ReleaseSessionOptions(p); } } guard{ api, options };

        Check(api->SetIntraOpNumThreads(options, threadCount));
        Check(api->SetInterOpNumThreads(options, 1));
        Check(api->SetSessionExecutionMode(options, ORT_SEQUENTIAL));
        Check(api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));
        Check(api->CreateSession(env, model.c_str(), options, &session));

        size_t count = 0;
        Check(api->SessionGetInputCount(session, &count));
        if (count != 1)
            throw std::runtime_error("person model must have 1 input");
        Check(api->SessionGetOutputCount(session, &count));
        if (count != 2)
            throw std::runtime_error("person model must have 2 outputs");

        Shape(true, 0, { 1, 3, 640, 640 });
        Shape(false, 0, { 1, 116, 8400 });
        Shape(false, 1, { 1, 32, 160, 160 });

        OrtAllocator* allocator = nullptr;
        Check(api->GetAllocatorWithDefaultOptions(&allocator));
        char* name = nullptr;
        Check(api->SessionGetInputName(session, 0, allocator, &name));
        inputName = name;
        allocator->Free(allocator, name);

        for (size_t i = 0; i < 2; ++i)
        {
            name = nullptr;
            Check(api->SessionGetOutputName(session, i, allocator, &name));
            outputNames[i] = name;
            allocator->Free(allocator, name);
        }

        Check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory));
    }

    void Run(const float* rgbData, float* maskData, double& outMilliseconds)
    {
        const int64_t dims[] = { 1, 3, 640, 640 };
        OrtValue* input = nullptr;
        std::array<OrtValue*, 2> output{};
        struct Guard
        {
            const OrtApi* a; OrtValue*& in; std::array<OrtValue*, 2>& out;
            ~Guard() { if (in) a->ReleaseValue(in); for (auto* p : out) if (p) a->ReleaseValue(p); }
        } guard{ api, input, output };

        Check(api->CreateTensorWithDataAsOrtValue(
            memory, const_cast<float*>(rgbData),
            3 * 640 * 640 * sizeof(float), dims, 4,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input));

        const char* inputs[] = { inputName.c_str() };
        const char* outputs[] = { outputNames[0].c_str(), outputNames[1].c_str() };
        const OrtValue* in = input;

        const auto start = GetTickCount64();
        Check(api->Run(session, nullptr, inputs, &in, 1, outputs, 2, output.data()));

        float* detections = nullptr;
        float* prototypes = nullptr;
        Check(api->GetTensorMutableData(output[0], reinterpret_cast<void**>(&detections)));
        Check(api->GetTensorMutableData(output[1], reinterpret_cast<void**>(&prototypes)));

        Decode(detections, prototypes, maskData);
        outMilliseconds = double(GetTickCount64() - start);
    }
};

int RunWorker(int argc, wchar_t* argv[])
{
    uint32_t parentPid = 0;
    std::filesystem::path customModel;
    unsigned hw = std::thread::hardware_concurrency();
    unsigned threadCount = (std::max)(2u, (std::min)(8u, hw ? (hw / 2) : 4u));

    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if ((arg == L"--pid" || arg == L"-p") && i + 1 < argc)
        {
            parentPid = static_cast<uint32_t>(_wtoi(argv[++i]));
        }
        else if ((arg == L"--model" || arg == L"-m") && i + 1 < argc)
        {
            customModel = argv[++i];
        }
        else if ((arg == L"--threads" || arg == L"-t") && i + 1 < argc)
        {
            threadCount = static_cast<unsigned>(_wtoi(argv[++i]));
            if (threadCount == 0) threadCount = (std::max)(2u, (std::min)(6u, hw ? (hw / 2) : 4u));
        }
    }

    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    wchar_t exePathBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    g_exeDir = std::filesystem::path(exePathBuf).parent_path();

    Log("person-worker started with PID=" + std::to_string(GetCurrentProcessId()) +
        ", Parent PID=" + std::to_string(parentPid));

    if (parentPid == 0)
    {
        Log("Missing or invalid --pid argument. Usage: person-worker.exe --pid <parent_pid>");
        return 1;
    }

    HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!hParent)
    {
        Log("Failed to open parent process handle: " + std::to_string(GetLastError()));
        return 2;
    }

    std::wstring shmName = DlssNr::Person::Ipc::ShmName(parentPid);
    HANDLE hShm = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmName.c_str());
    if (!hShm)
    {
        Log("Failed to open shared memory mapping: " + std::to_string(GetLastError()));
        CloseHandle(hParent);
        return 3;
    }

    void* shmBase = MapViewOfFile(hShm, FILE_MAP_ALL_ACCESS, 0, 0, DlssNr::Person::Ipc::TotalShmSize);
    if (!shmBase)
    {
        Log("Failed to map view of shared memory: " + std::to_string(GetLastError()));
        CloseHandle(hShm);
        CloseHandle(hParent);
        return 4;
    }

    auto* header = reinterpret_cast<DlssNr::Person::Ipc::ShmHeader*>(shmBase);

    HANDLE hReq = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, DlssNr::Person::Ipc::ReqEventName(parentPid).c_str());
    HANDLE hResp = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, DlssNr::Person::Ipc::RespEventName(parentPid).c_str());
    HANDLE hStop = OpenEventW(SYNCHRONIZE, FALSE, DlssNr::Person::Ipc::StopEventName(parentPid).c_str());

    if (!hReq || !hResp || !hStop)
    {
        Log("Failed to open synchronization events.");
        if (hReq) CloseHandle(hReq);
        if (hResp) CloseHandle(hResp);
        if (hStop) CloseHandle(hStop);
        UnmapViewOfFile(shmBase);
        CloseHandle(hShm);
        CloseHandle(hParent);
        return 5;
    }

    std::unique_ptr<Inference> inference;
    std::string loadError;
    try
    {
        inference = std::make_unique<Inference>();
        inference->Open(g_exeDir, customModel, threadCount);
    }
    catch (const std::exception& e)
    {
        loadError = e.what();
    }
    catch (...)
    {
        loadError = "unknown failure opening person model";
    }

    if (!loadError.empty() || !inference)
    {
        Log("Model initialization failed: " + loadError);
        strncpy_s(header->statusMessage, loadError.c_str(), sizeof(header->statusMessage) - 1);
        header->state = static_cast<uint32_t>(DlssNr::Person::Ipc::WorkerState::Error);
        SetEvent(hResp);

        // Keep process alive briefly so parent can read the error message
        HANDLE errWaitHandles[] = { hStop, hParent };
        WaitForMultipleObjects(2, errWaitHandles, FALSE, 5000);

        CloseHandle(hReq);
        CloseHandle(hResp);
        CloseHandle(hStop);
        UnmapViewOfFile(shmBase);
        CloseHandle(hShm);
        CloseHandle(hParent);
        return 6;
    }

    Log("Model loaded successfully. Entering inference loop.");
    strncpy_s(header->statusMessage, "person model ready", sizeof(header->statusMessage) - 1);
    header->state = static_cast<uint32_t>(DlssNr::Person::Ipc::WorkerState::Ready);
    SetEvent(hResp);

    HANDLE waitHandles[] = { hReq, hStop, hParent };
    while (true)
    {
        DWORD wr = WaitForMultipleObjects(3, waitHandles, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0) // hReq signaled: new frame to infer
        {
            header->state = static_cast<uint32_t>(DlssNr::Person::Ipc::WorkerState::Processing);
            const float* inputRgb = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(shmBase) + header->reqRgbOffset);
            float* outputMask = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(shmBase) + header->respMaskOffset);

            double ms = 0.0;
            try
            {
                inference->Run(inputRgb, outputMask, ms);
                header->respEpoch = header->reqEpoch;
                header->respFrame = header->reqFrame;
                header->respTick = header->reqTick;
                header->respWidth = header->reqWidth;
                header->respHeight = header->reqHeight;
                header->computeMilliseconds = ms;
                header->state = static_cast<uint32_t>(DlssNr::Person::Ipc::WorkerState::Ready);
            }
            catch (const std::exception& e)
            {
                Log("Inference run error: " + std::string(e.what()));
                strncpy_s(header->statusMessage, e.what(), sizeof(header->statusMessage) - 1);
                header->state = static_cast<uint32_t>(DlssNr::Person::Ipc::WorkerState::Error);
            }

            SetEvent(hResp);
        }
        else if (wr == WAIT_OBJECT_0 + 1) // hStop signaled
        {
            Log("Stop event signaled by parent. Exiting cleanly.");
            break;
        }
        else if (wr == WAIT_OBJECT_0 + 2) // hParent terminated
        {
            Log("Parent process terminated. Exiting cleanly.");
            break;
        }
        else
        {
            Log("Wait failed or abandoned: " + std::to_string(GetLastError()));
            break;
        }
    }

    header->state = static_cast<uint32_t>(DlssNr::Person::Ipc::WorkerState::Terminating);

    CloseHandle(hReq);
    CloseHandle(hResp);
    CloseHandle(hStop);
    UnmapViewOfFile(shmBase);
    CloseHandle(hShm);
    CloseHandle(hParent);

    Log("person-worker finished.");
    return 0;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int res = RunWorker(argc, argv);
    LocalFree(argv);
    return res;
}

int wmain(int argc, wchar_t* argv[])
{
    return RunWorker(argc, argv);
}
