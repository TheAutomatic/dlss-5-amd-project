#include "../PersonIpc.h"
#include "../PersonModel.h"
#include "../WorkerPolicy.h"

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
        if(std::filesystem::exists(logPath)&&std::filesystem::file_size(logPath)>65536){
            std::error_code ec;
            std::filesystem::remove(g_exeDir/L"person-worker.1.log",ec);
            std::filesystem::rename(logPath,g_exeDir/L"person-worker.1.log",ec);
        }
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

using DlssNr::Person::Inference;

int RunWorker(int argc, wchar_t* argv[])
{
    uint32_t parentPid = 0;
    unsigned hw = std::thread::hardware_concurrency();
    unsigned threadCount = DlssNr::Person::WorkerThreads(hw);

    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if ((arg == L"--pid" || arg == L"-p") && i + 1 < argc)
        {
            parentPid = static_cast<uint32_t>(_wtoi(argv[++i]));
        }
        else if ((arg == L"--threads" || arg == L"-t") && i + 1 < argc)
        {
            const int requested=_wtoi(argv[++i]);
            if(requested<1||requested>8)return 1;
            threadCount=static_cast<unsigned>(requested);
        }
    }

    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

    wchar_t exePathBuf[32768]{};
    const auto exeLength=GetModuleFileNameW(nullptr,exePathBuf,32768);
    if(!exeLength||exeLength>=32768)return 8;
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

    if (!DlssNr::Person::Ipc::ValidHeader(*header))
    {
        strncpy_s(header->statusMessage, "person worker protocol mismatch; install the complete package", _TRUNCATE);
        header->state=static_cast<uint32_t>(DlssNr::Person::Ipc::WorkerState::Error);
        SetEvent(hResp);
        CloseHandle(hReq);CloseHandle(hResp);CloseHandle(hStop);UnmapViewOfFile(shmBase);CloseHandle(hShm);CloseHandle(hParent);
        return 7;
    }
    header->flags=DlssNr::Person::Ipc::ShmVersion;
    Log("CPU policy: threads="+std::to_string(threadCount)+", priority=normal, ORT spinning=off");
    std::unique_ptr<Inference> inference;
    std::string loadError;
    try
    {
        inference = std::make_unique<Inference>();
        inference->Open(g_exeDir, threadCount);
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
    for(auto name:{L"onnxruntime.dll",L"msvcp140.dll",L"vcruntime140.dll",L"vcruntime140_1.dll"}){
        wchar_t path[32768]{};
        if(auto module=GetModuleHandleW(name);module&&GetModuleFileNameW(module,path,32768)){
            const auto utf8=std::filesystem::path(path).u8string();
            Log("Loaded dependency: "+std::string(utf8.begin(),utf8.end()));
        }
    }
    strncpy_s(header->statusMessage, "person model ready", sizeof(header->statusMessage) - 1);
    header->state = static_cast<uint32_t>(DlssNr::Person::Ipc::WorkerState::Ready);
    SetEvent(hResp);

    unsigned completed=0;double totalMs=0;
    HANDLE waitHandles[] = { hStop, hParent, hReq };
    while (true)
    {
        DWORD wr = WaitForMultipleObjects(3, waitHandles, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0 + 2) // hReq signaled: new frame to infer
        {
            header->state = static_cast<uint32_t>(DlssNr::Person::Ipc::WorkerState::Processing);
            float* inputRgb = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(shmBase) + DlssNr::Person::Ipc::InputRgbOffset);
            float* outputMask = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(shmBase) + DlssNr::Person::Ipc::OutputMaskOffset);

            try
            {
                if(!DlssNr::Person::Ipc::ValidHeader(*header))throw std::runtime_error("person worker protocol changed");
                auto mask=inference->RunRgb(inputRgb);
                memcpy(outputMask,mask.values.data(),DlssNr::Person::Ipc::OutputMaskSize);
                ++completed;totalMs+=mask.milliseconds;
                if(completed<=3)Log("frame="+std::to_string(header->reqFrame)+" CPU="+std::to_string(mask.milliseconds)+" ms");
                header->respEpoch = header->reqEpoch;
                header->respFrame = header->reqFrame;
                header->respTick = header->reqTick;
                header->respWidth = header->reqWidth;
                header->respHeight = header->reqHeight;
                header->computeMilliseconds = mask.milliseconds;
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
        else if (wr == WAIT_OBJECT_0) // hStop signaled
        {
            Log("Stop event signaled by parent. Exiting cleanly.");
            break;
        }
        else if (wr == WAIT_OBJECT_0 + 1) // hParent terminated
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

    Log("person-worker finished: frames="+std::to_string(completed)+", mean CPU ms="+std::to_string(completed?totalMs/completed:0));
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
