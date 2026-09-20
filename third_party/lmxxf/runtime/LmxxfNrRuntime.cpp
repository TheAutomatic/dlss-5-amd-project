#include "LmxxfNrApi.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include "LmxxfProductionOptions.h"
#include "native_game_codec.h"
#include "native_game_rgb_input.h"
#include "native_network_geometry.h"
#include "native_rgb_texture.h"
#include "hip_d3d12_bridge.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace
{
thread_local char g_lastError[256] = {};

void SetError(const char *text)
{
    if (!text)
        text = "";
    std::strncpy(g_lastError, text, sizeof(g_lastError) - 1);
    g_lastError[sizeof(g_lastError) - 1] = 0;
}

int32_t Fail(int32_t status, const char *text)
{
    SetError(text);
    return status;
}

std::string Utf8(const std::wstring &s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    if (n)
        WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}

bool IsDirectory(const std::wstring &path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExists(const std::wstring &path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring JoinPath(const std::wstring &dir, const wchar_t *name)
{
    std::wstring out = dir;
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/')
        out += L'\\';
    out += name;
    return out;
}

std::wstring DllDirectory()
{
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LmxxfNrGetApi), &mod);
    wchar_t path[MAX_PATH] {};
    if (!mod || !GetModuleFileNameW(mod, path, MAX_PATH))
        return {};
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        dir.resize(slash);
    return dir;
}

std::wstring FindShaderDir()
{
    const std::wstring dll = DllDirectory();
    const std::wstring candidates[] = {
        JoinPath(dll, L"shaders"),
        JoinPath(dll, L"..\\..\\third_party\\lmxxf\\shaders"),
        L"third_party\\lmxxf\\shaders",
    };
    for (const auto &c : candidates)
    {
        wchar_t full[MAX_PATH] {};
        GetFullPathNameW(c.c_str(), MAX_PATH, full, nullptr);
        if (FileExists(JoinPath(full, L"native_codec_encode.hlsl")))
            return full;
    }
    return {};
}

std::wstring FindWeightsDir(const std::wstring &assets)
{
    if (FileExists(JoinPath(assets, L"block0-ffn.f16")) || FileExists(JoinPath(assets, L"block0-ffn.f32")))
        return assets;
    const std::wstring parent = JoinPath(assets, L"..");
    wchar_t full[MAX_PATH] {};
    GetFullPathNameW(parent.c_str(), MAX_PATH, full, nullptr);
    if (FileExists(JoinPath(full, L"block0-ffn.f16")) || FileExists(JoinPath(full, L"block0-ffn.f32")))
        return full;
    wchar_t env[MAX_PATH] {};
    if (GetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", env, MAX_PATH) && env[0])
    {
        wchar_t full[MAX_PATH] {};
        GetFullPathNameW(env, MAX_PATH, full, nullptr);
        if (FileExists(JoinPath(full, L"block0-ffn.f16")) || FileExists(JoinPath(full, L"block0-ffn.f32")))
            return full;
    }
    return {};
}

bool ResolveModulesDir(const std::wstring &assets, std::wstring *modulesDir)
{
    if (FileExists(JoinPath(assets, L"SHA256SUMS")))
    {
        *modulesDir = assets;
        return true;
    }
    const std::wstring hip = JoinPath(assets, L"HIP");
    if (FileExists(JoinPath(hip, L"SHA256SUMS")))
    {
        *modulesDir = hip;
        return true;
    }
    const std::wstring nested = JoinPath(JoinPath(assets, L"native-game-tiled-assets"), L"HIP");
    if (FileExists(JoinPath(nested, L"SHA256SUMS")))
    {
        *modulesDir = nested;
        return true;
    }
    return false;
}

int32_t ValidateModuleSet(const std::wstring &modulesDir, uint32_t *outCount)
{
    *outCount = 0;
    const std::wstring sumsPath = JoinPath(modulesDir, L"SHA256SUMS");
    HANDLE file = CreateFileW(sumsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return Fail(LMXXF_NR_UNAVAILABLE, "Create: SHA256SUMS missing in modules directory");
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1 << 20)
    {
        CloseHandle(file);
        return Fail(LMXXF_NR_UNAVAILABLE, "Create: SHA256SUMS unreadable");
    }
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr))
    {
        CloseHandle(file);
        return Fail(LMXXF_NR_UNAVAILABLE, "Create: SHA256SUMS read failed");
    }
    CloseHandle(file);
    text.resize(read);

    uint32_t found = 0;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos)
            continue;
        size_t nameStart = line.find_first_not_of(" \t", sp);
        if (nameStart == std::string::npos)
            continue;
        std::string name = line.substr(nameStart);
        if (name.size() < 7 || name.rfind(".hsaco") != name.size() - 6)
            continue;
        std::wstring wname(name.begin(), name.end());
        const std::wstring full = JoinPath(modulesDir, wname.c_str());
        if (!IsDirectory(full) && GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES)
            ++found;
        else
            return Fail(LMXXF_NR_UNAVAILABLE, "Create: hsaco listed in SHA256SUMS is missing");
    }
    if (found == 0)
        return Fail(LMXXF_NR_UNAVAILABLE, "Create: no .hsaco entries in SHA256SUMS");
    if (found < 24)
        return Fail(LMXXF_NR_UNAVAILABLE, "Create: fewer than 24 hsaco modules; host/module set incomplete");
    *outCount = found;
    return static_cast<int32_t>(LMXXF_NR_OK);
}

bool LooksLikeObject(void *p)
{
    if (!p)
        return false;
    MEMORY_BASIC_INFORMATION info {};
    if (!VirtualQuery(p, &info, sizeof info))
        return false;
    if (!(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        return false;
    return info.State == MEM_COMMIT;
}

struct Job
{
    uint32_t state = LMXXF_NR_JOB_NONE;
    ID3D12Resource *color = nullptr;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    UINT width = 0, height = 0;
    uint32_t seed = 1;
};

struct Session
{
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    std::wstring assetsDir;
    std::wstring modulesDir;
    std::wstring weightsDir;
    std::wstring shaderDir;
    uint32_t hsacoCount = 0;
    bool modulesValidated = false;
    bool hipPrepared = false;
    bool queueBound = false;
    hip_reference::D3D12Bridge *bridge = nullptr;
    NativeGameCodec *encode = nullptr;
    NativeGameRgbInput *rgbInput = nullptr;
    NativeRgbTexture *rgbTex = nullptr;
    Job job {};

    // Wait until queue work that may touch encode/rgb/bridge shared resources is done.
    // Returns S_OK only when completion is confirmed; callers must retain resources on failure.
    HRESULT DrainGpu()
    {
        if (!device || !queue)
            return S_OK;
        ID3D12Fence *fence = nullptr;
        HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr) || !fence)
            return FAILED(hr) ? hr : E_FAIL;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ev)
        {
            const DWORD err = GetLastError();
            fence->Release();
            return HRESULT_FROM_WIN32(err ? err : ERROR_OUTOFMEMORY);
        }
        const UINT64 v = 1;
        hr = queue->Signal(fence, v);
        if (FAILED(hr))
        {
            CloseHandle(ev);
            fence->Release();
            return hr;
        }
        hr = fence->SetEventOnCompletion(v, ev);
        if (FAILED(hr))
        {
            CloseHandle(ev);
            fence->Release();
            return hr;
        }
        const DWORD wr = WaitForSingleObject(ev, 30000);
        CloseHandle(ev);
        const UINT64 completed = fence->GetCompletedValue();
        fence->Release();
        if (wr != WAIT_OBJECT_0 || completed < v)
            return wr == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : E_FAIL;
        return S_OK;
    }

    void AbandonSessionResources()
    {
        // Fail-closed intentional leak: GPU may still reference the whole chain.
        bridge = nullptr;
        rgbTex = nullptr;
        rgbInput = nullptr;
        encode = nullptr;
        queue = nullptr;
        device = nullptr;
    }

    ~Session()
    {
        // GPU may still be reading codec/bridge resources; drain before teardown.
        if (FAILED(DrainGpu()))
        {
            AbandonSessionResources();
            return;
        }
        // Bridge dtor also synchronizes HIP / pending fence, then frees shared buffers.
        delete bridge;
        bridge = nullptr;
        delete rgbTex;
        rgbTex = nullptr;
        delete rgbInput;
        rgbInput = nullptr;
        delete encode;
        encode = nullptr;
        if (queue)
            queue->Release();
        queue = nullptr;
        if (device)
            device->Release();
        device = nullptr;
    }
};

template <class Fn>
int32_t Guard(Fn &&fn)
{
    try
    {
        return fn();
    }
    catch (const std::exception &ex)
    {
        return Fail(LMXXF_NR_FAILED, ex.what());
    }
    catch (...)
    {
        return Fail(LMXXF_NR_FAILED, "unhandled exception");
    }
}

int32_t QueryCapabilities(LmxxfNrCapabilities *out)
{
    return Guard([&] {
        if (!out)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "QueryCapabilities: null out");
        if (out->struct_size != sizeof(LmxxfNrCapabilities))
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "QueryCapabilities: struct_size mismatch");
        out->abi_version = LMXXF_NR_ABI_VERSION;
        out->max_input_width = 1920;
        out->max_input_height = 1080;
        out->history_supported = 0;
        out->overlap_supported = 0;
        out->graph_supported = 0;
        out->hip_ready = 0;
        out->gfx1201_target = 1;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t Create(const LmxxfNrCreateInfo *info, void **context)
{
    return Guard([&] {
        if (!info || !context)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: null info or context");
        *context = nullptr;
        if (info->struct_size != sizeof(LmxxfNrCreateInfo))
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: struct_size mismatch");
        if (info->flags != 0)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: flags must be 0");
        if (!info->device || !info->queue)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: device and queue required");
        if (!info->assets_directory || !info->assets_directory[0])
            return Fail(LMXXF_NR_INVALID_ARGUMENT,
                        "Create: assets_directory required (modules dir from 68dc099 build)");

        std::wstring assets = info->assets_directory;
        if (!IsDirectory(assets))
            return Fail(LMXXF_NR_UNAVAILABLE, "Create: assets_directory is not a directory");
        std::wstring modulesDir;
        if (!ResolveModulesDir(assets, &modulesDir))
            return Fail(LMXXF_NR_UNAVAILABLE,
                        "Create: modules directory needs SHA256SUMS + hsaco (or HIP/ under assets)");
        uint32_t count = 0;
        const int32_t st = ValidateModuleSet(modulesDir, &count);
        if (st != LMXXF_NR_OK)
            return st;

        auto *session = new Session;
        session->assetsDir = assets;
        session->modulesDir = modulesDir;
        session->weightsDir = FindWeightsDir(assets);
        session->shaderDir = FindShaderDir();
        session->hsacoCount = count;
        session->modulesValidated = true;
        if (LooksLikeObject(info->device) && LooksLikeObject(info->queue))
        {
            auto *dev = static_cast<ID3D12Device *>(info->device);
            auto *q = static_cast<ID3D12CommandQueue *>(info->queue);
            ID3D12Device *qiDev = nullptr;
            ID3D12CommandQueue *qiQ = nullptr;
            if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&qiDev))) &&
                SUCCEEDED(q->QueryInterface(IID_PPV_ARGS(&qiQ))))
            {
                session->device = qiDev;
                session->queue = qiQ;
            }
            else
            {
                if (qiDev)
                    qiDev->Release();
                if (qiQ)
                    qiQ->Release();
            }
        }
        *context = session;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t Destroy(void *context)
{
    return Guard([&] {
        delete static_cast<Session *>(context);
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t PrepareSession(void *context)
{
    return Guard([&] {
        auto *session = static_cast<Session *>(context);
        if (!session)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareSession: null context");
        if (!session->modulesValidated)
            return Fail(LMXXF_NR_UNAVAILABLE, "PrepareSession: modules not validated");
        if (!session->device || !session->queue)
            return Fail(LMXXF_NR_INVALID_ARGUMENT,
                        "PrepareSession: device/queue are not live ID3D12 objects");
        // Upstream 0.21+: network tier follows the first real input size (DLSS5_NETWORK_HEIGHT=auto).
        // Defer HIP bridge Create until PrepareFrame so we do not bake 1920x1080 for a 720p Color.
        session->queueBound = true;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t PrepareFrame(void *context, const LmxxfNrFrameInfo *info, LmxxfNrJob *job)
{
    return Guard([&] {
        auto *session = static_cast<Session *>(context);
        if (!session || !info || !job)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: null argument");
        if (info->struct_size != sizeof(LmxxfNrFrameInfo) || job->struct_size != sizeof(LmxxfNrJob))
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: struct_size mismatch");
        job->handle = nullptr;
        job->private_output = nullptr;
        if (!session->queueBound && !session->hipPrepared)
            return Fail(LMXXF_NR_NOT_IMPLEMENTED, "PrepareFrame: call PrepareSession with a live D3D12 queue first");
        if (!info->color || !info->color_width || !info->color_height)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: color resource and size required");
        if (info->flags != 0)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: flags must be 0");
        if (session->shaderDir.empty())
            return Fail(LMXXF_NR_UNAVAILABLE, "PrepareFrame: native_codec_encode.hlsl not found");

        // Match upstream auto tier: <=1280x720 -> 720, <=1600x900 -> 900, else 1080.
        // Prefer CRT _putenv so MinGW std::getenv sees "auto" (SetEnvironmentVariable alone may not).
        if (!std::getenv("DLSS5_NETWORK_HEIGHT"))
            _putenv("DLSS5_NETWORK_HEIGHT=auto");
        NativeResolveNetworkGeometry(info->color_width, info->color_height);
        if (!session->hipPrepared)
        {
            auto geo = NativeCurrentNetworkGeometry();
            auto opt = LmxxfProductionOptions(geo.processing_width, geo.processing_height,
                                              Utf8(session->modulesDir), Utf8(session->weightsDir));
            if (opt.graph)
                return Fail(LMXXF_NR_FAILED, "PrepareFrame: graph must stay off");
            session->bridge = new hip_reference::D3D12Bridge();
            session->bridge->Create(session->queue, opt, {});
            session->hipPrepared = true;
            char geoMsg[192] {};
            std::snprintf(geoMsg, sizeof geoMsg,
                          "lmxxf: HIP lazy Create color=%ux%u network=%ux%u (proc %ux%u)",
                          info->color_width, info->color_height, geo.valid_width, geo.valid_height,
                          geo.processing_width, geo.processing_height);
            OutputDebugStringA(geoMsg);
            OutputDebugStringA("\n");
            SetError(geoMsg);
        }
        auto *color = static_cast<ID3D12Resource *>(info->color);
        if (!session->encode)
        {
            // Build on temporaries; publish only after the full chain succeeds.
            NativeGameCodec *enc = nullptr;
            NativeGameRgbInput *rgbIn = nullptr;
            NativeRgbTexture *rgbOut = nullptr;
            try
            {
                enc = new NativeGameCodec();
                enc->Create(session->device, {color}, session->shaderDir);
                rgbIn = new NativeGameRgbInput();
                rgbIn->Create(session->device, enc->Output(), session->shaderDir);
                rgbOut = new NativeRgbTexture();
                rgbOut->Create(session->device, session->bridge->Output(), session->shaderDir);
            }
            catch (...)
            {
                delete rgbOut;
                delete rgbIn;
                delete enc;
                throw;
            }
            session->encode = enc;
            session->rgbInput = rgbIn;
            session->rgbTex = rgbOut;
        }
        else if (color != session->job.color)
        {
            if (session->encode->RebindNeedsCompletion(0, color))
                return Fail(LMXXF_NR_UNAVAILABLE, "PrepareFrame: color rebind needs GPU completion");
            session->encode->RebindInputAfterCompletion(0, color);
        }

        session->job = {};
        session->job.color = color;
        session->job.colorState = static_cast<D3D12_RESOURCE_STATES>(info->color_state);
        session->job.width = info->color_width;
        session->job.height = info->color_height;
        session->job.seed = 1;
        session->job.state = LMXXF_NR_JOB_PREPARED;
        job->handle = &session->job;
        job->private_output = session->rgbTex->Output();
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t RecordInputs(void *context, void *job, void *command_list)
{
    return Guard([&] {
        auto *session = static_cast<Session *>(context);
        if (!session)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordInputs: null context");
        if (!session->hipPrepared)
            return Fail(LMXXF_NR_NOT_IMPLEMENTED, "RecordInputs is not wired (HIP/codec next)");
        auto *list = static_cast<ID3D12GraphicsCommandList *>(command_list);
        auto *j = static_cast<Job *>(job ? job : &session->job);
        if (!list || !j || j->state < LMXXF_NR_JOB_PREPARED)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordInputs: need prepared job and command list");
        session->encode->Record(list, {j->colorState}, 1.f);
        session->rgbInput->Record(list, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        session->bridge->RecordInputCopy(list, session->rgbInput->PostBase(), nullptr);
        j->state = LMXXF_NR_JOB_PREPARED;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t EnqueueHip(void *context, void *job)
{
    return Guard([&] {
        auto *session = static_cast<Session *>(context);
        if (!session)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "EnqueueHip: null context");
        if (!session->hipPrepared)
            return Fail(LMXXF_NR_NOT_IMPLEMENTED, "EnqueueHip is not wired (HIP/codec next)");
        if (session->weightsDir.empty())
            return Fail(LMXXF_NR_UNAVAILABLE,
                        "EnqueueHip: weights not found (set LMXXF_WEIGHTS_DIR to tiled assets, not 0.24.2 HIP/)");
        auto *j = static_cast<Job *>(job ? job : &session->job);
        session->bridge->EnqueueAfterProducer(j->seed, false);
        j->state = LMXXF_NR_JOB_NR_COMPLETE;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t RecordOutputs(void *context, void *job, void *command_list)
{
    return Guard([&] {
        auto *session = static_cast<Session *>(context);
        if (!session)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordOutputs: null context");
        if (!session->hipPrepared)
            return Fail(LMXXF_NR_NOT_IMPLEMENTED, "RecordOutputs is not wired (HIP/codec next)");
        auto *list = static_cast<ID3D12GraphicsCommandList *>(command_list);
        auto *j = static_cast<Job *>(job ? job : &session->job);
        if (!list || !j)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordOutputs: need job and command list");
        session->bridge->RecordOutputReadable(list);
        session->rgbTex->Record(list);
        j->state = LMXXF_NR_JOB_CONSUMER_COMPLETE;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t ExecuteAfterProducer(void *context, void *job)
{
    return EnqueueHip(context, job);
}

int32_t CancelUnsubmitted(void *context, void *)
{
    return Guard([&] {
        if (!context)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "null context");
        return Fail(LMXXF_NR_NOT_IMPLEMENTED, "CancelUnsubmitted is not wired");
    });
}
int32_t Poll(void *context, void *job, uint32_t *state)
{
    return Guard([&] {
        auto *session = static_cast<Session *>(context);
        if (!session)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "null context");
        auto *j = static_cast<Job *>(job ? job : &session->job);
        if (state)
            *state = j ? j->state : LMXXF_NR_JOB_NONE;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}
int32_t Retire(void *context, void *job)
{
    return Guard([&] {
        auto *session = static_cast<Session *>(context);
        if (!session)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "null context");
        auto *j = static_cast<Job *>(job ? job : &session->job);
        if (j)
            j->state = LMXXF_NR_JOB_RETIRED;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}
int32_t ResetHistory(void *context)
{
    return Guard([&] {
        if (!context)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "null context");
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}
int32_t Drain(void *context)
{
    return Guard([&] {
        auto *session = static_cast<Session *>(context);
        if (!session)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "null context");
        const HRESULT hr = session->DrainGpu();
        if (FAILED(hr))
            return Fail(LMXXF_NR_FAILED, "Drain: GPU wait failed or timed out");
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t GetStatus(void *context, char *buf, uint32_t buf_chars)
{
    return Guard([&] {
        if (!buf || buf_chars == 0)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetStatus: empty buffer");
        auto *session = static_cast<Session *>(context);
        char text[256] {};
        if (!session)
            std::snprintf(text, sizeof text, "no session");
        else if (!session->modulesValidated)
            std::snprintf(text, sizeof text, "lmxxf runtime stub (no modules path)");
        else
            if (session->hipPrepared && NativeNetworkGeometryResolved())
            {
                auto geo = NativeCurrentNetworkGeometry();
                std::snprintf(text, sizeof text,
                              "lmxxf modules_ok=%u hip=1 net=%ux%u color_job=%ux%u weights=%u",
                              static_cast<unsigned>(session->hsacoCount), geo.valid_width, geo.valid_height,
                              session->job.width, session->job.height,
                              session->weightsDir.empty() ? 0u : 1u);
            }
            else
            {
                std::snprintf(text, sizeof text, "lmxxf modules_ok=%u hip=0 prepared=%u queue=%u weights=%u",
                              static_cast<unsigned>(session->hsacoCount), session->hipPrepared ? 1u : 0u,
                              session->queueBound ? 1u : 0u,
                              session->weightsDir.empty() ? 0u : 1u);
            }
        std::strncpy(buf, text, buf_chars - 1);
        buf[buf_chars - 1] = 0;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t GetLastError(char *buf, uint32_t buf_chars)
{
    return Guard([&] {
        if (!buf || buf_chars == 0)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetLastError: empty buffer");
        std::strncpy(buf, g_lastError, buf_chars - 1);
        buf[buf_chars - 1] = 0;
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}
} // namespace

extern "C" int32_t LmxxfNrGetApi(uint32_t abi_version, LmxxfNrApi *out)
{
    return Guard([&] {
        if (!out)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetApi: null out");
        if (out->struct_size != sizeof(LmxxfNrApi))
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetApi: struct_size mismatch");
        if (abi_version != LMXXF_NR_ABI_VERSION)
            return Fail(LMXXF_NR_UNSUPPORTED_ABI, "GetApi: unsupported abi_version");
        std::memset(out, 0, sizeof(*out));
        out->struct_size = sizeof(LmxxfNrApi);
        out->abi_version = LMXXF_NR_ABI_VERSION;
        out->QueryCapabilities = QueryCapabilities;
        out->Create = Create;
        out->Destroy = Destroy;
        out->PrepareSession = PrepareSession;
        out->PrepareFrame = PrepareFrame;
        out->RecordInputs = RecordInputs;
        out->EnqueueHip = EnqueueHip;
        out->RecordOutputs = RecordOutputs;
        out->ExecuteAfterProducer = ExecuteAfterProducer;
        out->CancelUnsubmitted = CancelUnsubmitted;
        out->Poll = Poll;
        out->Retire = Retire;
        out->ResetHistory = ResetHistory;
        out->Drain = Drain;
        out->GetStatus = GetStatus;
        out->GetLastError = GetLastError;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
