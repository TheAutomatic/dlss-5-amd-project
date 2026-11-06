#include "pch.h"
#include "OpenNrRrFeature_Vk.h"
#include "OpenNrRrFeature_Dx12.h"

#include <Config.h>
#include <Util.h>
#include <NVNGX_Parameter.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_vk.h>
#include <misc/IdentifyGpu.h>
#include <imgui/ImGuiNotify.hpp>

#include <cmath>
#include <filesystem>
#include <format>
#include <mutex>

namespace
{
std::mutex s_vkMutex;
// the device the runtime's setup went into at vkCreateDevice, and its queue
VkDevice s_vkDevice = VK_NULL_HANDLE;
VkPhysicalDevice s_vkPhysical = VK_NULL_HANDLE;
uint32_t s_vkQueueFamily = 0;
uint32_t s_vkQueueIndex = 0;
uint32_t s_vkApiVersion = 0;
// empty while no device was made: the answer then rests on the runtime alone
std::string s_vkReason;
bool s_vkSeen = false;
uint32_t s_vkObservedMaxRenderWidth = 0;
uint32_t s_vkObservedMaxRenderHeight = 0;

bool Luid(VkPhysicalDevice physicalDevice, LUID& luid)
{
    VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
    {
        ScopedSkipSpoofingGlobal skipSpoofing {};
        vkGetPhysicalDeviceProperties2(physicalDevice, &props);
    }
    if (!id.deviceLUIDValid)
        return false;
    memcpy(&luid, id.deviceLUID, sizeof(luid));
    return true;
}

NVSDK_NGX_Resource_VK* VkResource(const NVSDK_NGX_Parameter& params, const char* key)
{
    void* resource = nullptr;
    return params.Get(key, &resource) == NVSDK_NGX_Result_Success ? static_cast<NVSDK_NGX_Resource_VK*>(resource) : nullptr;
}

bool IsDepthFormat(VkFormat format)
{
    return format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_X8_D24_UNORM_PACK32 || format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D16_UNORM_S8_UINT;
}

void LogNgxInputsVk(const NVSDK_NGX_Parameter& params)
{
    static std::string lastSet;
    static uint32_t logs = 0;
    static uint64_t frames = 0;
    ++frames;

    std::string set;
    std::vector<std::string> lines;
    for (const char* key : OpenNrRr::NgxTextureKeys())
    {
        NVSDK_NGX_Resource_VK* resource = VkResource(params, key);
        if (resource == nullptr)
            continue;
        set += key;
        set += ';';
        if (resource->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW)
        {
            lines.push_back(std::format("{} {:p} buffer of {} bytes", key, (void*) resource->Resource.BufferInfo.Buffer,
                                        resource->Resource.BufferInfo.SizeInBytes));
            continue;
        }
        const auto& v = resource->Resource.ImageViewInfo;
        lines.push_back(std::format("{} image {:p} view {:p} {}x{} VkFormat {} aspect {:#x} mip {} layer {}{}", key, (void*) v.Image,
                                    (void*) v.ImageView, v.Width, v.Height, (int) v.Format, v.SubresourceRange.aspectMask,
                                    v.SubresourceRange.baseMipLevel, v.SubresourceRange.baseArrayLayer, resource->ReadWrite ? " read-write" : ""));
    }

    if (set == lastSet || logs >= 20)
        return;

    lastSet = set;
    ++logs;
    LOG_INFO("OpenRR: DLSS-D inputs at frame {} ({} textures, Vulkan):", frames, lines.size());
    for (const auto& line : lines)
        LOG_INFO("OpenRR:   {}", line);
    for (const char* key : OpenNrRr::NgxScalarKeys())
    {
        if (float value = 0; params.Get(key, &value) == NVSDK_NGX_Result_Success)
            LOG_INFO("OpenRR:   {} = {}", key, value);
        else if (int ivalue = 0; params.Get(key, &ivalue) == NVSDK_NGX_Result_Success)
            LOG_INFO("OpenRR:   {} = {}", key, ivalue);
    }
    float* m = nullptr;
    LOG_INFO("OpenRR:   WorldToView matrix {}, ViewToClip matrix {}",
             params.Get(NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, reinterpret_cast<void**>(&m)) == NVSDK_NGX_Result_Success && m ? "given" : "absent",
             (m = nullptr, params.Get(NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, reinterpret_cast<void**>(&m)) == NVSDK_NGX_Result_Success && m)
                 ? "given"
                 : "absent");
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
bool PrepareVkDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* in, VkDeviceCreateInfo* out, void** setup)
{
    *setup = nullptr;

    if (!DlssdFound())
    {
        std::lock_guard lock(s_vkMutex);
        s_vkSeen = true;
        s_vkReason = "no nvngx_dlssd.dll beside the game or in the opennr-rr folder";
        LOG_INFO("OpenRR Vulkan: not offered: {}", s_vkReason);
        return false;
    }

    const OnrRrApi5* api = Api5();

    if (api == nullptr)
    {
        std::lock_guard lock(s_vkMutex);
        s_vkSeen = true;
        s_vkReason = Reason().empty() ? "OpenNrRrRuntime.dll has no Vulkan path (ABI 5)" : Reason();
        LOG_INFO("OpenRR Vulkan: not offered: {}", s_vkReason);
        return false;
    }

    int32_t rc;
    {
        ScopedSkipSpoofingGlobal skipSpoofing {};
        rc = api->PrepareVkDevice(physicalDevice, in, out, setup);
    }

    if (rc != ONR_RR_OK)
    {
        *setup = nullptr;
        std::lock_guard lock(s_vkMutex);
        s_vkSeen = true;
        s_vkReason = LastError();
        LOG_INFO("OpenRR Vulkan: not offered on this device: {}", s_vkReason);
        return false;
    }

    LOG_INFO("OpenRR Vulkan: the runtime's extensions, features and queue go into the game's device ({} extensions, {} "
             "queue families)",
             out->enabledExtensionCount, out->queueCreateInfoCount);
    return true;
}

void FinishVkDevice(void* setup, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion)
{
    const OnrRrApi5* api = Api5();
    if (setup == nullptr || api == nullptr)
        return;

    uint32_t family = 0, index = 0;
    const int32_t rc = api->FinishVkDevice(setup, device, &family, &index);
    std::lock_guard lock(s_vkMutex);
    s_vkSeen = true;

    if (rc != ONR_RR_OK || device == VK_NULL_HANDLE)
    {
        s_vkReason = "vkCreateDevice refused the runtime's additions";
        return;
    }

    s_vkDevice = device;
    s_vkPhysical = physicalDevice;
    s_vkQueueFamily = family;
    s_vkQueueIndex = index;
    s_vkApiVersion = apiVersion;
    s_vkReason.clear();
    LOG_INFO("OpenRR Vulkan: device {:p} made for the runtime, its queue: family {} index {}, instance API {}.{}", (void*) device, family,
             index, VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion));
}

bool VkUsable(VkDevice device, VkPhysicalDevice physicalDevice, std::string* why)
{
    std::string reason;
    {
        std::lock_guard lock(s_vkMutex);
        if (s_vkDevice == VK_NULL_HANDLE && (s_vkSeen || device != VK_NULL_HANDLE))
            reason = s_vkReason.empty() ? "the game's Vulkan device was not created through OptiScaler's hook" : s_vkReason;
        else if (device != VK_NULL_HANDLE && device != s_vkDevice)
            reason = "this is not the device made for the runtime";
        if (physicalDevice == VK_NULL_HANDLE)
            physicalDevice = s_vkPhysical;
    }
    if (reason.empty() && Api5() == nullptr)
        reason = "OpenNrRrRuntime.dll has no Vulkan path (ABI 5)";

    LUID luid = IdentifyGpu::getPrimaryGpu().luid;
    if (reason.empty() && physicalDevice != VK_NULL_HANDLE && !Luid(physicalDevice, luid))
        reason = "the Vulkan device has no LUID";
    if (reason.empty() && !Usable(luid))
        reason = Reason();

    if (why != nullptr)
        *why = reason;
    return reason.empty();
}
} // namespace OpenNrRr

OpenNrRrFeatureVk::OpenNrRrFeatureVk(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Vk(InHandleId, InParameters)
{
    _api = OpenNrRr::Api5();
    _moduleLoaded = _api != nullptr;
}

OpenNrRrFeatureVk::~OpenNrRrFeatureVk()
{
    if (State::Instance().isShuttingDown || _context == nullptr)
        return;

    OpenNrRr::ClearInfoContext(_context);
    _api->v4.v3.v2.v1.Destroy(_context);
    _context = nullptr;
}

void OpenNrRrFeatureVk::FallBack(const std::string& why)
{
    if (_fellBack)
        return;

    _fellBack = true;
    LOG_ERROR("OpenRR: {}; disabling OpenRR on this Vulkan session", why);
    ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "OpenRR stopped, see the log" });
    OpenNrRr::Block(why);
}

bool OpenNrRrFeatureVk::InitInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters)
{
    if (IsInited())
        return true;

    if (!ModuleLoaded() || Device == VK_NULL_HANDLE)
        return false;

    _initParameters = SetInitParameters(InParameters);

    if (!_initParameters)
        return false;

    std::string why;
    if (!OpenNrRr::VkUsable(Device, PhysicalDevice, &why))
    {
        LOG_ERROR("OpenRR Vulkan: not usable: {}", why);
        OpenNrRr::Block(why);
        return false;
    }

    if (!LowResMV())
    {
        LOG_ERROR("OpenRR Vulkan: the game uses display-resolution motion vectors, which OpenRR does not support; falling back");
        OpenNrRr::Block("display-resolution motion vectors are not supported");
        return false;
    }

    if (int value; InParameters->Get(NVSDK_NGX_Parameter_Use_HW_Depth, &value) == NVSDK_NGX_Result_Success)
    {
        _hasNGXDepthType = true;
        _ngxHWDepth = value == NVSDK_NGX_DLSS_Depth_Type_HW;
    }

    if (int value; InParameters->Get(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, &value) == NVSDK_NGX_Result_Success)
    {
        if (value == NVSDK_NGX_DLSS_Roughness_Mode_Packed)
            _roughnessPacked = 1;
        else if (value == NVSDK_NGX_DLSS_Roughness_Mode_Unpacked)
            _roughnessPacked = 0;
    }

    _maxRenderWidth = std::max(RenderWidth(), std::min(s_vkObservedMaxRenderWidth, DisplayWidth()));
    _maxRenderHeight = std::max(RenderHeight(), std::min(s_vkObservedMaxRenderHeight, DisplayHeight()));

    const auto dataDir = (Util::DllPath().parent_path() / L"opennr-rr").wstring();
    const std::wstring dlssd = OpenNrRr::DlssdPath();

    OnrRrVkDevice vd {};
    vd.size = sizeof(vd);
    {
        std::lock_guard lock(s_vkMutex);
        vd.api_version = s_vkApiVersion != 0 ? s_vkApiVersion : VK_API_VERSION_1_3;
        vd.queue_family = s_vkQueueFamily;
        vd.queue_index = s_vkQueueIndex;
        if (s_vkApiVersion == 0)
            LOG_WARN("OpenRR Vulkan: the instance's API version was not tracked, 1.3 assumed");
    }
    vd.instance = Instance;
    vd.physical_device = PhysicalDevice;
    vd.device = Device;

    OnrRrCreateInfo info {};
    info.size = sizeof(info);
    info.mode = ONR_RR_MODE_FAST;
    info.max_render_width = _maxRenderWidth;
    info.max_render_height = _maxRenderHeight;
    info.output_width = DisplayWidth();
    info.output_height = DisplayHeight();
    info.dlssd_path = dlssd.c_str();
    info.data_dir = dataDir.c_str();
    info.vk_device = &vd;

    int32_t rc;
    {
        ScopedSkipSpoofingGlobal skipSpoofing {};
        rc = _api->v4.v3.v2.v1.Create(&info, &_context);
    }

    if (rc != ONR_RR_OK || _context == nullptr)
    {
        const auto error = OpenNrRr::LastError();
        LOG_ERROR("OpenRR Vulkan: Create rc={}: {}", rc, error);
        if (DisplayWidth() >= 960)
            OpenNrRr::Block(std::format("Create failed ({})", error));
        _context = nullptr;
        return false;
    }

    OpenNrRr::SetInfoContext(_context);
    OpenNrRr::SyncFastMode(_context);

    uint32_t running = 0;
    _api->v4.GetBackend(_context, &running);
    LOG_INFO("OpenRR: created on the game's Vulkan device, render {}x{} -> output {}x{}, backend {}, weights from {}", _maxRenderWidth,
             _maxRenderHeight, DisplayWidth(), DisplayHeight(), running == ONR_RR_BACKEND_VULKAN_HOST ? "Vulkan host" : "other",
             wstring_to_string(dlssd));

    SetInit(true);
    return true;
}

bool OpenNrRrFeatureVk::FillFrame(const NVSDK_NGX_Parameter& params, OnrRrFrame& frame)
{
    const auto& cfg = *Config::Instance();

    OpenNrRr::SyncFastMode(_context);

    frame = {};
    frame.size = sizeof(frame);
    _nimages = 0;

    unsigned int width = 0;
    unsigned int height = 0;
    GetRenderResolution(&params, &width, &height);
    if (width > _maxRenderWidth || height > _maxRenderHeight)
    {
        LOG_INFO("OpenRR Vulkan: render size {}x{} exceeds creation ceiling {}x{}; recreating context",
                 width, height, _maxRenderWidth, _maxRenderHeight);
        s_vkObservedMaxRenderWidth = std::max(s_vkObservedMaxRenderWidth, width);
        s_vkObservedMaxRenderHeight = std::max(s_vkObservedMaxRenderHeight, height);
        State::Instance().newBackend = Upscaler::OpenNrRr;
        State::Instance().changeBackend[Handle()->Id] = true;
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

    const auto inputLayout = cfg.OpenNrRrVkInputLayout.value_or_default();
    const uint32_t read = _stricmp(inputLayout.c_str(), "general") == 0 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const uint32_t outputLayout = VK_IMAGE_LAYOUT_GENERAL;

    const auto texture = [this, &params](const char* key, const char* baseX, const char* baseY, uint32_t layout)
    {
        OnrRrTexture t {};
        NVSDK_NGX_Resource_VK* resource = VkResource(params, key);

        if (resource == nullptr ||
            resource->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW || resource->Resource.ImageViewInfo.Image == VK_NULL_HANDLE ||
            _nimages >= (int) std::size(_images))
            return t;

        const auto& v = resource->Resource.ImageViewInfo;
        OnrRrVkImage& image = _images[_nimages++];
        image = { reinterpret_cast<uint64_t>(v.Image),
                  reinterpret_cast<uint64_t>(v.ImageView),
                  (uint32_t) v.Format,
                  v.Width,
                  v.Height,
                  v.SubresourceRange.baseMipLevel,
                  v.SubresourceRange.baseArrayLayer,
                  uint32_t(VK_IMAGE_USAGE_SAMPLED_BIT | (resource->ReadWrite ? VK_IMAGE_USAGE_STORAGE_BIT : 0)) };
        t.resource = &image;
        t.state = layout;

        if (baseX != nullptr)
        {
            params.Get(baseX, &t.x);
            params.Get(baseY, &t.y);
        }

        return t;
    };

    frame.color = texture(NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                          NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, read);
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
                          NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, read);
    frame.motion = texture(NVSDK_NGX_Parameter_MotionVectors, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                           NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, read);
    frame.specular_hit_distance = texture(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance,
                                          NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X,
                                          NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y, read);
    frame.output = texture(NVSDK_NGX_Parameter_Output, nullptr, nullptr, outputLayout);
    frame.transparency_layer = texture("DLSS.TransparencyLayer", "DLSS.TransparencyLayer.Subrect.Base.X",
                                       "DLSS.TransparencyLayer.Subrect.Base.Y", read);
    frame.transparency_opacity = texture("DLSS.TransparencyLayerOpacity", "DLSS.TransparencyLayerOpacity.Subrect.Base.X",
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

    const float maskScale = cfg.OpenNrRrDisocclusionMaskScale.value_or_default();
    frame.disocclusion_mask_scale = maskScale > 0.0f ? maskScale : 1.0f;
    if (!cfg.OpenNrRrUseDisocclusionMask.value_or_default())
        frame.disocclusion_mask = {};
    if (!cfg.OpenNrRrUseSssGuide.value_or_default())
        frame.sss_guide = {};
    if (!cfg.OpenNrRrUseResponsivityMask.value_or_default())
        frame.responsivity_mask = {};
    if (!cfg.OpenNrRrUseColorBeforeTransparency.value_or_default())
        frame.color_before_transparency = {};
    if (!cfg.OpenNrRrUseDepthOfFieldGuide.value_or_default())
        frame.depth_of_field_guide = {};
    if (!cfg.OpenNrRrUseReflectionMotionVectors.value_or_default())
        frame.specular_motion = {};
    if (!cfg.OpenNrRrUseLayerOpacityAndMotion.value_or_default())
        frame.transparency_opacity = frame.transparency_motion = {};
    LogNgxInputsVk(params);

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
        LOG_INFO("OpenRR: guides diffuse albedo {}, specular albedo {}, normals {}, roughness {}, specular hit distance {}, "
                 "transparency layer {}, disocclusion mask {}, SSS guide {}, responsivity mask {}, colour before transparency "
                 "{}, depth of field guide {}, reflection motion vectors {}; input layout {} ({}), output GENERAL",
                 frame.diffuse_albedo.resource != nullptr, frame.specular_albedo.resource != nullptr, frame.normals.resource != nullptr,
                 frame.roughness.resource != nullptr, frame.specular_hit_distance.resource != nullptr,
                 frame.transparency_layer.resource != nullptr, frame.disocclusion_mask.resource != nullptr,
                 frame.sss_guide.resource != nullptr, frame.responsivity_mask.resource != nullptr,
                 frame.color_before_transparency.resource != nullptr, frame.depth_of_field_guide.resource != nullptr,
                 frame.specular_motion.resource != nullptr, read == VK_IMAGE_LAYOUT_GENERAL ? "GENERAL" : "SHADER_READ_ONLY_OPTIMAL",
                 inputLayout);
    }

    if (_roughnessPacked < 0)
    {
        _roughnessPacked = frame.roughness.resource == nullptr ? 1 : 0;
        LOG_INFO("OpenRR: roughness mode not declared, using {}", _roughnessPacked ? "packed" : "separate");
    }

    bool hardwareDepth;
    const auto* depth = static_cast<const OnrRrVkImage*>(frame.depth.resource);

    if (cfg.FfxDenoiserHardwareDepth.has_value())
        hardwareDepth = cfg.FfxDenoiserHardwareDepth.value();
    else if (IsDepthFormat(VkFormat(depth->format)))
        hardwareDepth = true;
    else if (_hasNGXDepthType)
        hardwareDepth = _ngxHWDepth;
    else
        hardwareDepth = true;

    if (static_cast<int>(hardwareDepth) != _loggedHWDepth)
    {
        _loggedHWDepth = hardwareDepth;
        LOG_INFO("OpenRR: depth is {} (VkFormat {}, {}), {}", hardwareDepth ? "hardware" : "linear", depth->format,
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
    if (cfg.OpenNrRrInvertDisocclusionMask.value_or_default())
        frame.flags |= ONR_RR_FRAME_DM_INVERT;
    if (cfg.OpenNrRrClampDisocclusionMask.value_or_default())
        frame.flags |= ONR_RR_FRAME_DM_CLAMP;

    return true;
}

bool OpenNrRrFeatureVk::EvaluateInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters)
{
    if (!IsInited() || _context == nullptr || _fellBack)
        return false;

    if (_frameCount % 30 == 0)
    {
        OnrRrInfo info {};
        info.size = sizeof(info);

        if (_api->v4.v3.v2.v1.GetInfo(_context, &info) == ONR_RR_OK)
        {
            if (_frameCount % 300 == 0)
                LOG_INFO("OpenRR: {} ({}), {} network frames, GPU {:.2f} ms (avg {:.2f}), {}x{} -> {}x{}, Vulkan", StateName(info.state),
                         info.message, info.frames, info.gpu_ms_last, info.gpu_ms_average, info.render_width, info.render_height,
                         info.output_width, info.output_height);

            if (info.state == ONR_RR_STATE_FAILED)
            {
                FallBack(std::format("the runtime failed: {}", info.message));
                return false;
            }
        }
    }

    OnrRrFrame frame {};

    if (!FillFrame(*InParameters, frame))
        return false;

    if (const int32_t rc = _api->RecordVk(_context, &frame, InCmdBuffer); rc != ONR_RR_OK)
    {
        FallBack(std::format("RecordVk rc={}: {}", rc, OpenNrRr::LastError()));
        return false;
    }

    _hasColor = true;
    _hasDepth = true;
    _hasMV = true;
    _hasOutput = true;
    _frameCount++;
    return true;
}
