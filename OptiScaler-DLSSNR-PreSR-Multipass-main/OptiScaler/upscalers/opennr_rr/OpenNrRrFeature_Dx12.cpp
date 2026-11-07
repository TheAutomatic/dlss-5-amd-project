#include "pch.h"
#include "OpenNrRrFeature_Dx12.h"
#include <upscalers/fsr31/FSRDFeature_Dx12.h>
#include <upscalers/fsr31/FSRDCameraMatrices.h>
#include <hooks/Streamline_Hooks.h>

#include <Config.h>
#include <Util.h>
#include <NVNGX_Parameter.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <misc/IdentifyGpu.h>
#include <misc/VulkanCallScope.h>
#include <proxies/FfxApi_Proxy.h>
#include <imgui/ImGuiNotify.hpp>
#include <dlssnr/backend/LmxxfEvaluateCut.h>
#include <dlssnr/backend/Selector.h>
#include <dlssnr/submission/GpuBatchTracker.h>
#include <dlssnr/submission/SubmissionTls.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>

namespace LmxxfCut = DlssNr::Backend::LmxxfCut;

namespace
{
std::mutex s_mutex;
HMODULE s_module = nullptr;
OnrRrApi s_api {};
// ABI 2 entry points (the denoised-image hop); null with a runtime that offers only ABI 1
OnrRrApi2 s_api2 {};
// ABI 3: the hop's GPU timing; null with an older runtime
decltype(OnrRrApi3::GetHopTiming) s_getHopTiming = nullptr;
// ABI 4: the D3D12 backend (Record / RecordPost into the game's list); null with an older runtime
decltype(OnrRrApi4::GetBackend) s_getBackend = nullptr;
decltype(OnrRrApi4::Record) s_record = nullptr;
decltype(&OnrRrSetDeltaRate) s_setDeltaRate = nullptr;
decltype(&OnrRrSetFastMode) s_setFastMode = nullptr;
decltype(&OnrRrGetFastMode) s_getFastMode = nullptr;
decltype(OnrRrApi4::RecordPost) s_recordPost = nullptr;
// ABI 5: the Vulkan host backend (PrepareVkDevice / FinishVkDevice / RecordVk); null with an older runtime
OnrRrApi5 s_api5 {};
bool s_loadTried = false;
bool s_checked = false;
bool s_usable = false;
LUID s_luid {};
std::string s_reason;
std::string s_blocked;
std::wstring s_dlssdPath;
void* s_infoContext = nullptr;

// Set by the between slot, read on the next evaluate.
std::atomic<int32_t> s_enqueueRc { ONR_RR_OK };

// NGX publishes the depth type only on a creation that carries DLSSD create params; a recreation keeps it.
bool s_ngxDepthTypeSeen = false;
bool s_ngxReportedHWDepth = false;
uint32_t s_observedMaxRenderWidth = 0;
uint32_t s_observedMaxRenderHeight = 0;

std::string RuntimeError()
{
    char text[256] {};
    if (s_api.GetLastError)
        s_api.GetLastError(text, sizeof text);
    return text;
}

// Caller holds s_mutex.
bool Load()
{
    if (s_loadTried)
        return s_module != nullptr;

    s_loadTried = true;

    const auto path = Util::DllPath().parent_path() / L"OpenNrRrRuntime.dll";
    std::error_code ec;

    if (!std::filesystem::exists(path, ec))
    {
        s_reason = "OpenNrRrRuntime.dll is not in OptiScaler's folder";
        LOG_INFO("OpenRR: {} ({})", s_reason, wstring_to_string(path.wstring()));
        return false;
    }

    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (module == nullptr)
    {
        s_reason = std::format("OpenNrRrRuntime.dll did not load (error {})", GetLastError());
        LOG_ERROR("OpenRR: {} ({})", s_reason, wstring_to_string(path.wstring()));
        return false;
    }

    auto getApi = reinterpret_cast<PFN_OnrRrGetApi>(GetProcAddress(module, "OnrRrGetApi"));
    OnrRrApi5 api5 {};
    OnrRrApi4& api4 = api5.v4;
    OnrRrApi3& api3 = api4.v3;
    OnrRrApi2& api2 = api3.v2;
    OnrRrApi& api = api2.v1;
    // ABI 5, 4, 3 or 2 when the runtime has it, otherwise 1
    int32_t rc = getApi != nullptr ? getApi(ONR_RR_ABI_VERSION_5, &api) : ONR_RR_FAILED;
    const bool has5 = rc == ONR_RR_OK && api.abi_version == ONR_RR_ABI_VERSION_5 && api5.PrepareVkDevice &&
                      api5.FinishVkDevice && api5.RecordVk && api4.GetBackend && api4.Record && api4.RecordPost;
    if (!has5)
    {
        api5 = {};
        rc = getApi != nullptr ? getApi(ONR_RR_ABI_VERSION_4, &api) : ONR_RR_FAILED;
    }
    if (!has5 && (rc != ONR_RR_OK || api.abi_version != ONR_RR_ABI_VERSION_4 || !api4.GetBackend || !api4.Record || !api4.RecordPost))
    {
        api5 = {};
        rc = getApi != nullptr ? getApi(ONR_RR_ABI_VERSION_3, &api) : ONR_RR_FAILED;
    }
    if (rc != ONR_RR_OK || api.abi_version < ONR_RR_ABI_VERSION_3 || !api3.GetHopTiming)
    {
        api5 = {};
        rc = getApi != nullptr ? getApi(ONR_RR_ABI_VERSION_2, &api) : ONR_RR_FAILED;
    }
    if (rc != ONR_RR_OK || api.abi_version < ONR_RR_ABI_VERSION_2 || !api2.PrepareDenoised || !api2.EnqueueNetwork ||
        !api2.EnqueuePost)
    {
        api5 = {};
        rc = getApi != nullptr ? getApi(ONR_RR_ABI_VERSION, &api) : ONR_RR_FAILED;
    }

    if (rc != ONR_RR_OK ||
        api.abi_version < ONR_RR_ABI_VERSION || api.abi_version > ONR_RR_ABI_VERSION_5 ||
        !api.Check || !api.Create || !api.Destroy ||
        !api.SetMode || !api.RecordInputs || !api.Enqueue || !api.RecordOutput || !api.Cancel || !api.GetInfo ||
        !api.GetLastError)
    {
        s_reason = std::format("OpenNrRrRuntime.dll does not offer ABI {} (rc {}, abi {})", ONR_RR_ABI_VERSION, rc,
                               api.abi_version);
        LOG_ERROR("OpenRR: {}", s_reason);
        FreeLibrary(module);
        return false;
    }

    s_module = module;
    s_api = api;
    s_api2 = api2;
    s_getHopTiming = api3.GetHopTiming;
    s_getBackend = api4.GetBackend;
    s_record = api4.Record;
    s_setDeltaRate = reinterpret_cast<decltype(&OnrRrSetDeltaRate)>(GetProcAddress(module, "OnrRrSetDeltaRate"));
    s_setFastMode = reinterpret_cast<decltype(&OnrRrSetFastMode)>(GetProcAddress(module, "OnrRrSetFastMode"));
    s_getFastMode = reinterpret_cast<decltype(&OnrRrGetFastMode)>(GetProcAddress(module, "OnrRrGetFastMode"));
    s_recordPost = api4.RecordPost;
    if (has5)
        s_api5 = api5;
    LOG_INFO("OpenRR: runtime loaded from {} (ABI {})", wstring_to_string(path.wstring()), api.abi_version);
    return true;
}

bool IsSupportedDlssdVersion(const std::wstring& path)
{
    if (path.empty())
        return false;
    version_t ver {};
    return Util::GetFileVersion(path, &ver) && ver.major == 310 && ver.minor == 9 && ver.patch == 1;
}

// The nvngx_dlssd.dll the weights are built from: opennr-rr folder first, then OptiScaler's own lookup, then the one
// the game loaded, then one beside the game's exe. Prefer a supported 310.9.1.x build when multiple files exist.
std::wstring FindDlssd(const char** source)
{
    struct Candidate
    {
        std::wstring path;
        const char* label = nullptr;
    };
    std::vector<Candidate> candidates;
    std::error_code ec;
    if (const auto own = Util::DllPath().parent_path() / L"opennr-rr" / L"nvngx_dlssd.dll"; std::filesystem::exists(own, ec))
        candidates.push_back({ own.wstring(), "opennr-rr folder" });

    if (const auto& known = State::Instance().NVNGX_DLSSD_Path; known.has_value() && std::filesystem::exists(known.value(), ec))
        candidates.push_back({ known.value(), "OptiScaler's nvngx_dlssd.dll path" });

    if (HMODULE loaded = GetModuleHandleW(L"nvngx_dlssd.dll"); loaded != nullptr)
    {
        wchar_t path[MAX_PATH] {};
        if (GetModuleFileNameW(loaded, path, MAX_PATH) != 0)
            candidates.push_back({ path, "loaded by the game" });
    }

    if (auto found = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssd.dll"); found.has_value())
        candidates.push_back({ found->wstring(), "beside the game's exe" });

    for (const auto& c : candidates)
    {
        if (IsSupportedDlssdVersion(c.path))
        {
            *source = c.label;
            return c.path;
        }
    }

    if (!candidates.empty())
    {
        *source = candidates.front().label;
        return candidates.front().path;
    }

    *source = "not found";
    return {};
}

enum class DepthKind
{
    Unknown,
    Hardware,
    GameWritten,
};

// Same reading as FSR-RR: a depth-stencil resource cannot hold a linear distance, a render target or UAV the game
// filled may.
DepthKind ClassifyDepth(ID3D12Resource* depth)
{
    if (depth == nullptr)
        return DepthKind::Unknown;

    const D3D12_RESOURCE_DESC desc = depth->GetDesc();

    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0)
        return DepthKind::Hardware;

    switch (desc.Format)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DepthKind::Hardware;
    default:
        break;
    }

    if ((desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0)
        return DepthKind::GameWritten;

    return DepthKind::Unknown;
}

// The matrix as a row-vector 4x4 (v' = v M); falls back to Streamline camera constants when NGX leaves it unset.
bool ReadMatrix(const NVSDK_NGX_Parameter& params, const char* key, float (&out)[16])
{
    float* matrix = nullptr;

    if (params.Get(key, reinterpret_cast<void**>(&matrix)) != NVSDK_NGX_Result_Success || matrix == nullptr)
        return false;

    bool nonZero = false;
    for (int i = 0; i < 16; i++)
    {
        if (!std::isfinite(matrix[i]) || matrix[i] == sl::INVALID_FLOAT)
            return false;
        nonZero = nonZero || matrix[i] != 0.0f;
    }
    if (!nonZero)
        return false;

    memcpy(out, matrix, sizeof(out));
    return true;
}

void ResolveCameraMatricesImpl(const NVSDK_NGX_Parameter& params, bool depthInverted, float (&w2v)[16], float (&v2c)[16])
{
    const bool hasW2V = ReadMatrix(params, NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, w2v);
    const bool hasV2C = ReadMatrix(params, NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, v2c);

    DirectX::XMMATRIX ngxView {};
    DirectX::XMMATRIX ngxProj {};
    DirectX::XMMATRIX inv {};
    if (hasW2V)
        ngxView = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(w2v)));
    if (hasV2C)
        ngxProj = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(v2c)));

    const bool validW2V = hasW2V && FSRDCamera::TryInvert(ngxView, inv);
    const bool validV2C = hasV2C && FSRDCamera::TryInvert(ngxProj, inv);
    if (validW2V && validV2C)
        return;

    if (!StreamlineHooks::isSetConstantsHooked())
        return;

    const auto slSnap = StreamlineHooks::getSLConstantsSnapshot();
    const auto cam = FSRDCamera::Resolve(
        validW2V ? &ngxView : nullptr,
        validV2C ? &ngxProj : nullptr,
        &slSnap.constants,
        depthInverted);
    if (!validV2C && cam.projectionSource != FSRDCamera::Source::Missing)
    {
        const DirectX::XMMATRIX rows = DirectX::XMMatrixTranspose(cam.projection);
        DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(v2c), rows);
    }
    if (!validW2V && cam.viewSource != FSRDCamera::Source::Missing)
    {
        const DirectX::XMMATRIX rows = DirectX::XMMatrixTranspose(cam.view);
        DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(w2v), rows);
    }
}

constexpr const char* kNgxTextures[] = {
    "Color", "Output", "Depth", "MotionVectors", "ExposureTexture", "TransparencyMask", "GBuffer.Normals",
    "GBuffer.Roughness", "DLSS.Input.DiffuseAlbedo", "DLSS.Input.SpecularAlbedo", "DLSSD.Alpha", "DLSSD.OutputAlpha",
    "DLSSD.ReflectedAlbedo", "DLSSD.ColorBeforeParticles", "DLSSD.ColorAfterParticles", "DLSSD.ColorBeforeTransparency",
    "DLSSD.ColorAfterTransparency", "DLSSD.ColorBeforeFog", "DLSSD.ColorAfterFog",
    "DLSSD.ScreenSpaceSubsurfaceScatteringGuide", "DLSSD.DepthOfFieldGuide", "DLSSD.DiffuseHitDistance",
    "DLSSD.SpecularHitDistance", "DLSSD.DiffuseRayDirection", "DLSSD.SpecularRayDirection",
    "DLSSD.DiffuseRayDirectionHitDistance", "DLSSD.SpecularRayDirectionHitDistance", "DLSS.TransparencyLayer",
    "DLSS.TransparencyLayerOpacity", "DLSS.TransparencyLayerMvecs", "DLSS.DisocclusionMask", "DLSSD.ResponsivityMask",
    "GBuffer.IndirectAlbedo", "GBuffer.SpecularMvec", "GBuffer.Emissive", "MotionVectors3D", "IsParticleMask",
    "AnimatedTextureMask", "DepthHighRes", "Position.ViewSpace", "RayTracingHitDistance", "MotionVectorsReflection",
    "DLSS.Input.Reduce.Ghost.Mask", "DLSS.Input.Bias.Current.Color.Mask", "GBuffer.Attrib.15",
};

constexpr const char* kNgxScalars[] = {
    "DLSS.Pre.Exposure", "DLSS.Exposure.Scale", "MV.Scale.X", "MV.Scale.Y", "Jitter.Offset.X", "Jitter.Offset.Y",
    "DLSS.Input.Bias.Current.Color.Subrect.Base.X", "DLSS.Input.Bias.Current.Color.Subrect.Base.Y",
    "DLSS.TransparencyLayer.Subrect.Base.X", "DLSS.TransparencyLayer.Subrect.Base.Y", "TonemapperType",
    "DLSSD.ResponsivityMask.Subrect.Base.X", "DLSSD.ResponsivityMask.Subrect.Base.Y",
    "DLSS.TransparencyLayerMvecs.Subrect.Base.X", "DLSS.TransparencyLayerMvecs.Subrect.Base.Y",
};

void LogNgxInputs(const NVSDK_NGX_Parameter& params, ID3D12Resource* layer)
{
    static std::string lastSet;
    static uint32_t logs = 0;
    static uint64_t frames = 0, withLayer = 0;

    ++frames;
    withLayer += layer != nullptr;
    if (frames % 3000 == 0)
        LOG_INFO("OpenRR: transparency layer in {} of the last 3000 frames", std::exchange(withLayer, 0));

    std::string set;
    std::vector<std::string> lines;
    for (const char* key : kNgxTextures)
    {
        ID3D12Resource* resource = nullptr;
        if (!TryGetNGXVoidPointer(params, key, resource) || resource == nullptr)
            continue;
        set += key;
        set += ';';
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        lines.push_back(std::format("{} {:p} {}x{} format {} flags {:#x}", key, (void*) resource, desc.Width,
                                    desc.Height, (int) desc.Format, (uint32_t) desc.Flags));
    }

    if (set == lastSet || logs >= 20)
        return;

    lastSet = set;
    ++logs;
    LOG_INFO("OpenRR: DLSS-D inputs at frame {} ({} textures):", frames, lines.size());
    for (const auto& line : lines)
        LOG_INFO("OpenRR:   {}", line);
    for (const char* key : kNgxScalars)
    {
        if (float value = 0; params.Get(key, &value) == NVSDK_NGX_Result_Success)
            LOG_INFO("OpenRR:   {} = {}", key, value);
        else if (int ivalue = 0; params.Get(key, &ivalue) == NVSDK_NGX_Result_Success)
            LOG_INFO("OpenRR:   {} = {}", key, ivalue);
    }
}

const char* StateName(uint32_t state)
{
    switch (state)
    {
    case ONR_RR_STATE_BUILDING:
        return "building";
    case ONR_RR_STATE_READY:
        return "ready";
    case ONR_RR_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}
} // namespace

namespace OpenNrRr
{
bool Usable(LUID adapter)
{
    std::lock_guard lock(s_mutex);

    if (!s_blocked.empty())
        return false;

    if (s_checked && IsEqualLUID(adapter, s_luid))
        return s_usable;

    s_checked = true;
    s_luid = adapter;
    s_usable = false;

    if (IdentifyGpu::getPrimaryGpu().vendorId != VendorId::AMD)
    {
        s_reason = "needs an AMD RDNA4 GPU";
        return false;
    }

    if (!Load())
        return false;

    const char* source = nullptr;
    s_dlssdPath = FindDlssd(&source);
    LOG_INFO("OpenRR: nvngx_dlssd.dll {} ({})", source, wstring_to_string(s_dlssdPath));

    int32_t rc;
    {
        ScopedSkipVulkanHooks skipVulkanHooks {};
        ScopedCreatingD3DDevice creatingDevice {};
        ScopedSkipSpoofingGlobal skipSpoofing {};
        rc = s_api.Check(&adapter, s_dlssdPath.empty() ? nullptr : s_dlssdPath.c_str());
    }

    if (rc != ONR_RR_OK)
    {
        s_reason = RuntimeError();
        if (s_reason.empty())
            s_reason = std::format("not supported here (rc {})", rc);
        LOG_INFO("OpenRR: Check rc={}: {}", rc, s_reason);
        return false;
    }

    if (s_dlssdPath.empty())
    {
        s_reason = "nvngx_dlssd.dll not found, the weights are built from it";
        LOG_INFO("OpenRR: {}", s_reason);
        return false;
    }

    s_reason.clear();
    s_usable = true;
    LOG_INFO("OpenRR: Check passed on adapter {:08X}:{:08X}", (uint32_t) adapter.HighPart, (uint32_t) adapter.LowPart);
    return true;
}

bool Checked()
{
    std::lock_guard lock(s_mutex);
    return s_checked || !s_blocked.empty();
}

std::string Reason()
{
    std::lock_guard lock(s_mutex);
    return s_blocked.empty() ? s_reason : s_blocked;
}

void Block(const std::string& why)
{
    {
        std::lock_guard lock(s_mutex);
        if (s_blocked.empty())
            s_blocked = "stopped this session: " + why;
        s_usable = false;
    }
    DlssNr::Backend::InvalidateInstallProbe();
}

bool Info(OnrRrInfo& info)
{
    std::lock_guard lock(s_mutex);

    if (s_infoContext == nullptr)
        return false;

    info = {};
    info.size = sizeof(info);
    return s_api.GetInfo(s_infoContext, &info) == ONR_RR_OK;
}

bool SplitReady()
{
    {
        std::lock_guard lock(s_mutex);
        const auto backend = Config::Instance()->OpenNrRrBackend.value_or_default();
        if (Load() && s_record != nullptr && _stricmp(backend.c_str(), "vulkan") != 0)
            return true;
    }
    return DlssNr::Backend::SubmissionHooksWanted() && DlssNr::Submission::Hooks::IsArmed() &&
           DlssNr::Submission::Hooks::ProxyWrapEnabled();
}

std::vector<const char*> NgxTextureKeys() { return { std::begin(kNgxTextures), std::end(kNgxTextures) }; }
std::vector<const char*> NgxScalarKeys() { return { std::begin(kNgxScalars), std::end(kNgxScalars) }; }

const OnrRrApi5* Api5()
{
    std::lock_guard lock(s_mutex);
    return Load() && s_api5.RecordVk != nullptr ? &s_api5 : nullptr;
}

std::wstring DlssdPath()
{
    std::lock_guard lock(s_mutex);
    return s_dlssdPath;
}

std::string LastError()
{
    std::lock_guard lock(s_mutex);
    return RuntimeError();
}

bool DlssdFound()
{
    const char* source = nullptr;
    return IsSupportedDlssdVersion(FindDlssd(&source));
}

void SetInfoContext(void* context)
{
    std::lock_guard lock(s_mutex);
    s_infoContext = context;
}

void ClearInfoContext(void* context)
{
    std::lock_guard lock(s_mutex);
    if (s_infoContext == context)
        s_infoContext = nullptr;
}

void SyncFastMode(void* context)
{
    if (context == nullptr || s_setFastMode == nullptr)
        return;
    const auto mode = Config::Instance()->OpenNrRrMode.value_or_default();
    const bool fast = _stricmp(mode.c_str(), "legacy") != 0;
    s_setFastMode(context, fast ? 1 : 0);
}

int32_t FastModeState()
{
    std::lock_guard lock(s_mutex);
    if (s_infoContext == nullptr || s_getFastMode == nullptr)
        return -1;
    return s_getFastMode(s_infoContext);
}

void ResolveMatrices(const NVSDK_NGX_Parameter& params, bool depthInverted, float (&w2v)[16], float (&v2c)[16])
{
    ResolveCameraMatricesImpl(params, depthInverted, w2v, v2c);
}
} // namespace OpenNrRr

OpenNrRrFeatureDx12::OpenNrRrFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters)
{
    std::lock_guard lock(s_mutex);
    _moduleLoaded = Load();
}

OpenNrRrFeatureDx12::~OpenNrRrFeatureDx12()
{
    if (State::Instance().isShuttingDown || _context == nullptr)
        return;

    DropStaleJob();

    {
        std::lock_guard lock(s_mutex);
        if (s_infoContext == _context)
            s_infoContext = nullptr;
    }

    s_api.Destroy(_context);
    _context = nullptr;
}

bool OpenNrRrFeatureDx12::RecordingsRetired()
{
    auto& tracker = DlssNr::Submission::GpuBatch::Tracker::Instance();
    std::erase_if(_recordingTickets, [&tracker](uint64_t ticket) { return tracker.Retired(ticket); });
    return _recordingTickets.empty();
}

namespace
{
std::mutex g_deferredOpenRrMutex;
std::vector<std::unique_ptr<OpenNrRrFeatureDx12>> g_deferredOpenRrFeatures;
}

void OpenNrRrFeatureDx12::DestroyWhenRetired(std::unique_ptr<OpenNrRrFeatureDx12> feature)
{
    if (!feature)
        return;
    if (feature->RecordingsRetired())
        return;
    LOG_INFO("OpenRR feature deferred: {} recording(s) still outstanding at teardown",
             feature->_recordingTickets.size());
    std::lock_guard lock(g_deferredOpenRrMutex);
    g_deferredOpenRrFeatures.push_back(std::move(feature));
}

void OpenNrRrFeatureDx12::ReapDeferredFeatures()
{
    std::vector<std::unique_ptr<OpenNrRrFeatureDx12>> ripe;
    {
        std::lock_guard lock(g_deferredOpenRrMutex);
        std::erase_if(g_deferredOpenRrFeatures, [&ripe](std::unique_ptr<OpenNrRrFeatureDx12>& feature) {
            if (feature->RecordingsRetired())
            {
                ripe.push_back(std::move(feature));
                return true;
            }
            return false;
        });
    }
}

bool OpenNrRrFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (IsInited())
        return true;

    if (!ModuleLoaded() || Device == nullptr)
        return false;

    _initParameters = SetInitParameters(InParameters);

    if (!_initParameters)
        return false;

    if (!OpenNrRr::Usable(Device->GetAdapterLuid()))
    {
        LOG_ERROR("OpenRR: not usable: {}", OpenNrRr::Reason());
        return false;
    }

    if (!LowResMV())
    {
        LOG_ERROR("OpenRR: the game uses display-resolution motion vectors, which OpenRR does not support; falling back");
        OpenNrRr::Block("display-resolution motion vectors are not supported");
        return false;
    }

    if (int value; InParameters->Get(NVSDK_NGX_Parameter_Use_HW_Depth, &value) == NVSDK_NGX_Result_Success)
    {
        _hasNGXDepthType = true;
        _ngxHWDepth = value == NVSDK_NGX_DLSS_Depth_Type_HW;
        s_ngxDepthTypeSeen = true;
        s_ngxReportedHWDepth = _ngxHWDepth;
    }
    else if (s_ngxDepthTypeSeen)
    {
        _hasNGXDepthType = true;
        _ngxHWDepth = s_ngxReportedHWDepth;
    }

    if (int value; InParameters->Get(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, &value) == NVSDK_NGX_Result_Success)
    {
        if (value == NVSDK_NGX_DLSS_Roughness_Mode_Packed)
            _roughnessPacked = 1;
        else if (value == NVSDK_NGX_DLSS_Roughness_Mode_Unpacked)
            _roughnessPacked = 0;
    }

    _maxRenderWidth = std::max(RenderWidth(), std::min(s_observedMaxRenderWidth, DisplayWidth()));
    _maxRenderHeight = std::max(RenderHeight(), std::min(s_observedMaxRenderHeight, DisplayHeight()));

    const auto dataDir = (Util::DllPath().parent_path() / L"opennr-rr").wstring();
    std::wstring dlssd;
    {
        std::lock_guard lock(s_mutex);
        dlssd = s_dlssdPath;
    }

    OnrRrCreateInfo info {};
    info.size = sizeof(info);
    info.mode = ONR_RR_MODE_FAST;
    info.d3d12_device = Device;
    info.max_render_width = _maxRenderWidth;
    info.max_render_height = _maxRenderHeight;
    info.output_width = DisplayWidth();
    info.output_height = DisplayHeight();
    info.dlssd_path = dlssd.c_str();
    info.data_dir = dataDir.c_str();
    const auto backend = Config::Instance()->OpenNrRrBackend.value_or_default();
    info.backend = _stricmp(backend.c_str(), "vulkan") == 0  ? ONR_RR_BACKEND_CHOICE_VULKAN
                   : _stricmp(backend.c_str(), "d3d12") == 0 ? ONR_RR_BACKEND_CHOICE_D3D12
                                                             : ONR_RR_BACKEND_CHOICE_AUTO;

    int32_t rc;
    {
        ScopedSkipHeapCapture skipHeapCapture {};
        ScopedSkipVulkanHooks skipVulkanHooks {};
        ScopedCreatingD3DDevice creatingDevice {};
        ScopedSkipSpoofingGlobal skipSpoofing {};
        rc = s_api.Create(&info, &_context);
    }

    if (rc != ONR_RR_OK || _context == nullptr)
    {
        const auto error = RuntimeError();
        LOG_ERROR("OpenRR: Create rc={}: {}", rc, error);
        OpenNrRr::Block(std::format("Create failed ({})", error));
        _context = nullptr;
        return false;
    }

    {
        std::lock_guard lock(s_mutex);
        s_infoContext = _context;
    }
    OpenNrRr::SyncFastMode(_context);

    uint32_t running = ONR_RR_BACKEND_VULKAN;
    if (s_getBackend != nullptr)
        s_getBackend(_context, &running);
    LOG_INFO("OpenRR: created, render {}x{} -> output {}x{}, backend {} (asked: {}), weights from {}",
             _maxRenderWidth, _maxRenderHeight, DisplayWidth(), DisplayHeight(),
             running == ONR_RR_BACKEND_D3D12 ? "D3D12" : "Vulkan", backend, wstring_to_string(dlssd));

    SetInit(true);
    return true;
}

void OpenNrRrFeatureDx12::DropStaleJob()
{
    if (_job == nullptr)
        return;

    bool stillArmed = false;
    {
        auto& pending = LmxxfCut::Pending();
        std::lock_guard lock(pending.mutex);
        if (pending.job == _job && pending.session == this && pending.targetList == _jobList)
        {
            stillArmed = true;
            pending.session = nullptr;
            pending.job = nullptr;
            pending.enqueueHip = nullptr;
            pending.getLastError = nullptr;
            pending.targetList = nullptr;
            pending.expectedQueue = nullptr;
        }
    }

    if (stillArmed)
    {
        s_api.Cancel(_context, _job);

        static uint32_t dropped = 0;
        if (++dropped <= 10 || dropped % 300 == 0)
            LOG_WARN("OpenRR: the list of the previous frame never ran its between slot; job cancelled (#{})",
                     dropped);
    }

    _job = nullptr;
    _jobList = nullptr;
}

void OpenNrRrFeatureDx12::FallBack(const std::string& why)
{
    if (_fellBack)
        return;

    _fellBack = true;
    LOG_ERROR("OpenRR: {}; switching Ray Reconstruction back to the previous provider", why);
    ImGui::InsertNotification(
        { ImGuiToastType::Warning, 10000, "OpenRR stopped, Ray Reconstruction goes back to FSR-RR (see the log)" });

    OpenNrRr::Block(why);

    auto& state = State::Instance();
    if (FfxApiProxy::IsDenoiserApiImplementedDx12() && DlssNr::Backend::RecordingHooksPrepared())
        Config::Instance()->FsrRrEnabled.set_volatile_value(true);
    state.newBackend = Upscaler::FSR_RR;
    state.changeBackend[Handle()->Id] = true;
}

bool OpenNrRrFeatureDx12::FillFrame(const NVSDK_NGX_Parameter& params, OnrRrFrame& frame)
{
    const auto& cfg = *Config::Instance();
    auto& state = State::Instance();

    OpenNrRr::SyncFastMode(_context);

    frame = {};
    frame.size = sizeof(frame);

    unsigned int width = 0;
    unsigned int height = 0;
    GetRenderResolution(&params, &width, &height);
    if (width > _maxRenderWidth || height > _maxRenderHeight)
    {
        LOG_INFO("OpenRR: render size {}x{} exceeds creation ceiling {}x{}; recreating context",
                 width, height, _maxRenderWidth, _maxRenderHeight);
        s_observedMaxRenderWidth = std::max(s_observedMaxRenderWidth, width);
        s_observedMaxRenderHeight = std::max(s_observedMaxRenderHeight, height);
        state.newBackend = Upscaler::OpenNrRr;
        state.changeBackend[Handle()->Id] = true;
        return false;
    }
    frame.render_width = width;
    frame.render_height = height;

    params.Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &frame.jitter_x);
    params.Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &frame.jitter_y);
    if (!std::isfinite(frame.jitter_x))
        frame.jitter_x = 0.0f;
    if (!std::isfinite(frame.jitter_y))
        frame.jitter_y = 0.0f;

    if (params.Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.mv_scale_x) != NVSDK_NGX_Result_Success ||
        !std::isfinite(frame.mv_scale_x) || frame.mv_scale_x == 0.0f)
        frame.mv_scale_x = 1.0f;
    if (params.Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.mv_scale_y) != NVSDK_NGX_Result_Success ||
        !std::isfinite(frame.mv_scale_y) || frame.mv_scale_y == 0.0f)
        frame.mv_scale_y = 1.0f;

    OpenNrRr::ResolveMatrices(params, DepthInverted(), frame.world_to_view, frame.view_to_clip);

    const bool unreal =
        state.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL || state.gameQuirks & GameQuirk::ForceUnrealEngine;
    const uint32_t read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const uint32_t colorState = cfg.ColorResourceBarrier.value_or(unreal ? D3D12_RESOURCE_STATE_RENDER_TARGET : read);
    const uint32_t motionState = cfg.MVResourceBarrier.value_or(unreal ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : read);
    const uint32_t depthState = cfg.DepthResourceBarrier.value_or(read);
    const uint32_t outputState = cfg.OutputResourceBarrier.value_or(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const auto texture = [&params](const char* key, const char* baseX, const char* baseY, uint32_t textureState)
    {
        OnrRrTexture t {};
        ID3D12Resource* resource = nullptr;

        if (TryGetNGXVoidPointer(params, key, resource))
        {
            t.resource = resource;
            t.state = textureState;

            if (baseX != nullptr)
            {
                params.Get(baseX, &t.x);
                params.Get(baseY, &t.y);
            }
        }

        return t;
    };

    frame.color = texture(NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, colorState);
    frame.diffuse_albedo =
        texture(NVSDK_NGX_Parameter_DiffuseAlbedo, NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X,
                NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y, read);
    frame.specular_albedo =
        texture(NVSDK_NGX_Parameter_SpecularAlbedo, NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X,
                NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y, read);
    frame.normals = texture(NVSDK_NGX_Parameter_GBuffer_Normals, NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X,
                            NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y, read);
    frame.roughness =
        texture(NVSDK_NGX_Parameter_GBuffer_Roughness, NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X,
                NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y, read);
    frame.depth = texture(NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, depthState);
    frame.motion = texture(NVSDK_NGX_Parameter_MotionVectors, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                           NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, motionState);
    frame.specular_hit_distance = texture(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance,
                                          NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X,
                                          NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y, read);
    frame.specular_ray_direction_hit_distance =
        texture(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance,
                NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_X,
                NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_Y, read);
    frame.output = texture(NVSDK_NGX_Parameter_Output, nullptr, nullptr, outputState);
    // ColorBeforeParticles is a whole-scene guide (already contained in Color), not a premultiplied overlay.
    frame.transparency_layer = texture("DLSS.TransparencyLayer", "DLSS.TransparencyLayer.Subrect.Base.X",
                                       "DLSS.TransparencyLayer.Subrect.Base.Y", read);
    frame.transparency_opacity =
        texture("DLSS.TransparencyLayerOpacity", "DLSS.TransparencyLayerOpacity.Subrect.Base.X",
                "DLSS.TransparencyLayerOpacity.Subrect.Base.Y", read);
    frame.disocclusion_mask = texture("DLSS.DisocclusionMask", "DLSS.DisocclusionMask.Subrect.Base.X",
                                      "DLSS.DisocclusionMask.Subrect.Base.Y", read);
    frame.sss_guide = texture("DLSSD.ColorBeforeFog", nullptr, nullptr, read);
    if (frame.sss_guide.resource == nullptr)
        frame.sss_guide = texture("DLSSD.ScreenSpaceSubsurfaceScatteringGuide", nullptr, nullptr, read);
    frame.sss_guide.x = frame.color.x;
    frame.sss_guide.y = frame.color.y;
    frame.responsivity_mask = texture("DLSSD.ResponsivityMask", "DLSSD.ResponsivityMask.Subrect.Base.X",
                                      "DLSSD.ResponsivityMask.Subrect.Base.Y", read);
    frame.color_before_transparency = texture(NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency, nullptr, nullptr, read);
    frame.color_before_transparency.x = frame.color.x;
    frame.color_before_transparency.y = frame.color.y;
    frame.depth_of_field_guide = texture(NVSDK_NGX_Parameter_DLSSD_DepthOfFieldGuide, nullptr, nullptr, read);
    frame.depth_of_field_guide.x = frame.color.x;
    frame.depth_of_field_guide.y = frame.color.y;
    frame.specular_motion = texture(NVSDK_NGX_Parameter_GBuffer_SpecularMvec, nullptr, nullptr, read);
    frame.specular_motion.x = frame.motion.x;
    frame.specular_motion.y = frame.motion.y;
    frame.transparency_motion = texture("DLSS.TransparencyLayerMvecs", "DLSS.TransparencyLayerMvecs.Subrect.Base.X",
                                        "DLSS.TransparencyLayerMvecs.Subrect.Base.Y", read);

    const bool useMask = cfg.OpenNrRrUseDisocclusionMask.value_or_default();
    const bool useSss = cfg.OpenNrRrUseSssGuide.value_or_default();
    const bool invertMask = cfg.OpenNrRrInvertDisocclusionMask.value_or_default();
    const bool clampMask = cfg.OpenNrRrClampDisocclusionMask.value_or_default();
    const float maskScale = cfg.OpenNrRrDisocclusionMaskScale.value_or_default();
    const bool useResp = cfg.OpenNrRrUseResponsivityMask.value_or_default();
    const bool useCbt = cfg.OpenNrRrUseColorBeforeTransparency.value_or_default();
    const bool useDof = cfg.OpenNrRrUseDepthOfFieldGuide.value_or_default();
    const bool useSmv = cfg.OpenNrRrUseReflectionMotionVectors.value_or_default();
    const bool useLayerExtras = cfg.OpenNrRrUseLayerOpacityAndMotion.value_or_default();
    if (const int switches = int(useMask) | int(useSss) << 1 | int(invertMask) << 2 | int(clampMask) << 3 |
                             int(useResp) << 4 | int(useCbt) << 5 | int(useDof) << 6 | int(useSmv) << 7 |
                             int(useLayerExtras) << 8;
        switches != _loggedSwitches || maskScale != _loggedMaskScale)
    {
        _loggedSwitches = switches;
        _loggedMaskScale = maskScale;
        LOG_INFO("OpenRR: use disocclusion mask {}, scale it by {}, invert it {}, clamp it to 0..1 {}, use SSS guide {}, "
                 "use responsivity mask {}, colour before transparency {}, depth of field guide {}, reflection motion "
                 "vectors {}, transparency layer opacity and motion {}",
                 useMask, maskScale, invertMask, clampMask, useSss, useResp, useCbt, useDof, useSmv, useLayerExtras);
    }
    frame.disocclusion_mask_scale = maskScale > 0.0f ? maskScale : 1.0f;
    if (!useMask)
        frame.disocclusion_mask = {};
    if (!useSss)
        frame.sss_guide = {};
    if (!useResp)
        frame.responsivity_mask = {};
    if (!useCbt)
        frame.color_before_transparency = {};
    if (!useDof)
        frame.depth_of_field_guide = {};
    if (!useSmv)
        frame.specular_motion = {};
    if (!useLayerExtras)
        frame.transparency_opacity = frame.transparency_motion = {};
    LogNgxInputs(params, static_cast<ID3D12Resource*>(frame.transparency_layer.resource));

    if (frame.color.resource == nullptr || frame.depth.resource == nullptr || frame.motion.resource == nullptr ||
        frame.output.resource == nullptr)
    {
        FallBack(std::format("the game gave no {}", frame.color.resource == nullptr    ? "colour"
                                                    : frame.depth.resource == nullptr  ? "depth"
                                                    : frame.motion.resource == nullptr ? "motion vectors"
                                                                                       : "output"));
        return false;
    }

    static bool guidesLogged = false;
    if (!guidesLogged)
    {
        guidesLogged = true;
        LOG_INFO("OpenRR: guides diffuse albedo {}, specular albedo {}, normals {}, roughness {}, specular hit "
                 "distance {}, ray direction + hit distance {} (not read), transparency layer {}, its opacity {}, its "
                 "motion vectors {}, disocclusion mask {}, SSS guide {}, responsivity mask {}, colour before "
                 "transparency {}, depth of field guide {}, reflection motion vectors {}",
                 frame.diffuse_albedo.resource != nullptr, frame.specular_albedo.resource != nullptr,
                 frame.normals.resource != nullptr, frame.roughness.resource != nullptr,
                 frame.specular_hit_distance.resource != nullptr,
                 frame.specular_ray_direction_hit_distance.resource != nullptr,
                 frame.transparency_layer.resource != nullptr, frame.transparency_opacity.resource != nullptr,
                 frame.transparency_motion.resource != nullptr, frame.disocclusion_mask.resource != nullptr,
                 frame.sss_guide.resource != nullptr, frame.responsivity_mask.resource != nullptr,
                 frame.color_before_transparency.resource != nullptr, frame.depth_of_field_guide.resource != nullptr,
                 frame.specular_motion.resource != nullptr);
    }

    if (_roughnessPacked < 0)
    {
        _roughnessPacked = frame.roughness.resource == nullptr ? 1 : 0;
        LOG_INFO("OpenRR: roughness mode not declared, using {}", _roughnessPacked ? "packed" : "separate");
    }

    bool hardwareDepth;
    const DepthKind kind = ClassifyDepth(static_cast<ID3D12Resource*>(frame.depth.resource));

    if (cfg.FfxDenoiserHardwareDepth.has_value())
        hardwareDepth = cfg.FfxDenoiserHardwareDepth.value();
    else if (kind == DepthKind::Hardware)
        hardwareDepth = true;
    else if (_hasNGXDepthType)
        hardwareDepth = _ngxHWDepth;
    else
        hardwareDepth = kind != DepthKind::GameWritten;

    if (static_cast<int>(hardwareDepth) != _loggedHWDepth)
    {
        _loggedHWDepth = hardwareDepth;
        LOG_INFO("OpenRR: depth is {} ({}), {}", hardwareDepth ? "hardware" : "linear",
                 _hasNGXDepthType ? (_ngxHWDepth ? "NGX says hardware" : "NGX says linear") : "NGX says nothing",
                 DepthInverted() ? "reversed Z" : "standard Z");
    }

    if (uint32_t reset = 0; params.Get(NVSDK_NGX_Parameter_Reset, &reset) == NVSDK_NGX_Result_Success && reset == 1)
        frame.flags |= ONR_RR_FRAME_RESET;
    if (hardwareDepth)
        frame.flags |= ONR_RR_FRAME_HW_DEPTH;
    if (DepthInverted())
        frame.flags |= ONR_RR_FRAME_DEPTH_INVERTED;
    if (JitteredMV())
        frame.flags |= ONR_RR_FRAME_MV_JITTERED;
    if (_roughnessPacked == 1)
        frame.flags |= ONR_RR_FRAME_ROUGHNESS_PACKED;
    if (invertMask)
        frame.flags |= ONR_RR_FRAME_DM_INVERT;
    if (clampMask)
        frame.flags |= ONR_RR_FRAME_DM_CLAMP;

    return true;
}

bool OpenNrRrFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (!IsInited() || _context == nullptr || _fellBack)
        return false;

    ReapDeferredFeatures();
    FSRDFeatureDx12::ReapDeferredFeatures();
    DropStaleJob();

    if (const int32_t rc = s_enqueueRc.exchange(ONR_RR_OK, std::memory_order_acq_rel); rc != ONR_RR_OK)
    {
        FallBack(std::format("Enqueue rc={}: {}", rc, LmxxfCut::LastEnqueueDiagnostic().error.data()));
        return false;
    }

    if (_frameCount % 30 == 0)
    {
        OnrRrInfo info {};
        info.size = sizeof(info);

        if (s_api.GetInfo(_context, &info) == ONR_RR_OK)
        {
            if (_frameCount % 300 == 0)
                LOG_INFO("OpenRR: {} ({}), {} network frames, GPU {:.2f} ms (avg {:.2f}), {}x{} -> {}x{}",
                         StateName(info.state), info.message, info.frames, info.gpu_ms_last, info.gpu_ms_average,
                         info.render_width, info.render_height, info.output_width, info.output_height);

            if (info.state == ONR_RR_STATE_FAILED)
            {
                FallBack(std::format("the runtime failed: {}", info.message));
                return false;
            }
        }
    }

    // D3D12 backend: the frame goes straight into the game's list, no split
    if (uint32_t backend = ONR_RR_BACKEND_VULKAN; s_getBackend != nullptr &&
                                                  s_getBackend(_context, &backend) == ONR_RR_OK && backend == ONR_RR_BACKEND_D3D12)
        return EvaluateD3d12(InCommandList, InParameters);

    DlssNr::Submission::ILogicalCommandList* logical = nullptr;

    if (FAILED(InCommandList->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                             reinterpret_cast<void**>(&logical))) ||
        logical == nullptr)
    {
        FallBack("the game's command list is not one OptiScaler can split (the submission proxies are off)");
        return false;
    }

    ID3D12GraphicsCommandList* list = nullptr;
    logical->QueryInterface(IID_PPV_ARGS(&list));

    if (list != nullptr)
        list->Release(); // the game holds the list for this evaluate

    if (list == nullptr || logical->IsSplitIneligible())
    {
        FallBack(std::format("the command list cannot be split here ({})", logical->SplitRejectionReason()));
        logical->Release();
        return false;
    }

    OnrRrFrame frame {};

    if (!FillFrame(*InParameters, frame))
    {
        logical->Release();
        return false;
    }

    void* job = nullptr;

    if (const int32_t rc = s_api.RecordInputs(_context, &frame, list, &job); rc != ONR_RR_OK || job == nullptr)
    {
        FallBack(std::format("RecordInputs rc={}: {}", rc, RuntimeError()));
        logical->Release();
        return false;
    }

    if (const HRESULT hr = LmxxfCut::TrySplitAtEvaluate(list); FAILED(hr) || hr == S_FALSE)
    {
        s_api.Cancel(_context, job);
        FallBack(std::format("the split was refused (hr {:#x}, {})", (uint32_t) hr, logical->SplitRejectionReason()));
        logical->Release();
        return false;
    }

    logical->Release();

    if (const int32_t rc = s_api.RecordOutput(_context, job, list); rc != ONR_RR_OK)
    {
        s_api.Cancel(_context, job);
        FallBack(std::format("RecordOutput rc={}: {}", rc, RuntimeError()));
        return false;
    }

    LmxxfCut::SetPendingEnqueue(this, job, &OpenNrRrFeatureDx12::Between, s_api.GetLastError, list);
    LmxxfCut::ArmBetweenSlot();
    _job = job;
    _jobList = list;
    RecordingsRetired();
    _recordingTickets.push_back(DlssNr::Submission::GpuBatch::Tracker::Instance().Record(InCommandList));

    _hasColor = true;
    _hasDepth = true;
    _hasMV = true;
    _hasOutput = true;
    _frameCount++;
    return true;
}

bool OpenNrRrFeatureDx12::EvaluateD3d12(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    OnrRrFrame frame {};

    if (!FillFrame(*InParameters, frame))
        return false;

    ScopedSkipHeapCapture skipHeapCapture {};
    Microsoft::WRL::ComPtr<DlssNr::Submission::IRecordingResources> rec;
    if (InCommandList != nullptr &&
        FAILED(InCommandList->QueryInterface(__uuidof(DlssNr::Submission::IRecordingResources),
                                             reinterpret_cast<void**>(rec.GetAddressOf()))))
    {
        if (const auto logical = DlssNr::Submission::QueryLogicalCommandList(InCommandList))
            logical.As(&rec);
    }

    if (rec != nullptr)
        rec->BeginPrivateBindings();

    void* job = nullptr;
    const int32_t rc = s_record(_context, &frame, InCommandList, ONR_RR_EDIT_OFF, nullptr, &job);

    if (rec != nullptr)
        rec->EndPrivateBindings();

    if (rc != ONR_RR_OK)
    {
        FallBack(std::format("Record rc={}: {}", rc, RuntimeError()));
        return false;
    }

    RecordingsRetired();
    _recordingTickets.push_back(DlssNr::Submission::GpuBatch::Tracker::Instance().Record(InCommandList));

    _hasColor = true;
    _hasDepth = true;
    _hasMV = true;
    _hasOutput = true;
    _frameCount++;
    return true;
}

int32_t OpenNrRrFeatureDx12::Between(void* feature, void* job, void* queue)
{
    auto* self = static_cast<OpenNrRrFeatureDx12*>(feature);
    const int32_t rc = s_api.Enqueue(self->_context, job, queue);
    if (rc != ONR_RR_OK)
        s_enqueueRc.store(rc, std::memory_order_release);
    return rc;
}
