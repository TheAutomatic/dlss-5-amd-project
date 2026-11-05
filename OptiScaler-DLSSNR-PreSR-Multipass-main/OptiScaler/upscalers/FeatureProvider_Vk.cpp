#include <pch.h>
#include "FeatureProvider_Vk.h"

#include "Util.h"
#include "Config.h"

#include "NVNGX_Parameter.h"

#include "upscalers/fsr2/FSR2Feature_Vk.h"
#include "upscalers/dlss/DLSSFeature_Vk.h"
#include "upscalers/dlssd/DLSSDFeature_Vk.h"
#include "upscalers/fsr2_212/FSR2Feature_Vk_212.h"
#include "upscalers/fsr2_212/FSR2Feature_VkOnDx12_212.h"
#include "upscalers/ffx/FFXFeature_Vk.h"
#include "upscalers/xess/XeSSFeature_Vk.h"
#include "upscalers/ffx/FFXFeature_VkOn12.h"
#include "upscalers/dlss/DLSSFeature_VkOn12.h"
#include <misc/IdentifyGpu.h>
#include "upscalers/opennr_rr/OpenNrRrFeature_Dx12.h"
#include "upscalers/opennr_rr/OpenNrRrFeature_Vk.h"

bool FeatureProvider_Vk::GetFeature(Upscaler upscaler, UINT handleId, NVSDK_NGX_Parameter* parameters,
                                    std::unique_ptr<IFeature_Vk>* feature)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();
    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    switch (upscaler)
    {
    case Upscaler::XeSS:
        *feature = std::make_unique<XeSSFeature_Vk>(handleId, parameters);
        break;

    case Upscaler::FSR21:
        *feature = std::make_unique<FSR2FeatureVk212>(handleId, parameters);
        break;

    case Upscaler::FSR21_on12:
        *feature = std::make_unique<FSR2FeatureVkOnDx12_212>(handleId, parameters);
        break;

    case Upscaler::FSR22:
        *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
        break;

    case Upscaler::FFX:
        *feature = std::make_unique<FFXFeatureVk>(handleId, parameters);
        break;

    case Upscaler::DLSS_on12:
        *feature = std::make_unique<DLSSFeatureVkOn12>(handleId, parameters);
        break;

    case Upscaler::FFX_on12:
        *feature = std::make_unique<FFXFeatureVkOn12>(handleId, parameters);
        break;

    case Upscaler::DLSS:
        if (primaryGpu.dlssCapable && state.NVNGX_DLSS_Path.has_value())
        {
            *feature = std::make_unique<DLSSFeatureVk>(handleId, parameters);
            break;
        }
        else
        {
            *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
            upscaler = Upscaler::FSR22;
            break;
        }

    case Upscaler::OpenNrRr:
        *feature = std::make_unique<OpenNrRrFeatureVk>(handleId, parameters);

        if ((*feature)->ModuleLoaded())
            break;

        *feature = nullptr;
        return false;

    case Upscaler::DLSSD:
        if (primaryGpu.dlssCapable && state.NVNGX_DLSSD_Path.has_value())
        {
            *feature = std::make_unique<DLSSDFeatureVk>(handleId, parameters);
            break;
        }
        else
        {
            *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
            upscaler = Upscaler::FSR22;
            break;
        }

    default:
        *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
        upscaler = Upscaler::FSR22;
        break;
    }

    bool loaded = (*feature)->ModuleLoaded();

    if (!loaded)
    {
        // Fail after the constructor
        ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "Falling back to FSR 2.2" });
        *feature = std::make_unique<FSR2FeatureVk>(handleId, parameters);
        upscaler = Upscaler::FSR22;
        loaded = true; // Assuming the fallback always loads successfully
    }

    // DLSSD is stored in the config as DLSS
    if (upscaler == Upscaler::DLSSD)
        upscaler = Upscaler::DLSS;

    if (upscaler != Upscaler::OpenNrRr)
        cfg.VulkanUpscaler = upscaler;

    return loaded;
}

bool FeatureProvider_Vk::ChangeFeature(Upscaler upscaler, VkInstance instance, VkPhysicalDevice pd, VkDevice device,
                                       VkCommandBuffer cmdBuffer, PFN_vkGetInstanceProcAddr gipa,
                                       PFN_vkGetDeviceProcAddr gdpa, UINT handleId, NVSDK_NGX_Parameter* parameters,
                                       ContextData<IFeature_Vk>* contextData)
{
    State& state = State::Instance();
    Config& cfg = *Config::Instance();

    const bool dlssOnNonCapable = !IdentifyGpu::getPrimaryGpu().dlssCapable && state.newBackend == Upscaler::DLSS;

    const bool rrFeature = contextData->featureID == NVSDK_NGX_Feature_RayReconstruction;
    if (state.newBackend == Upscaler::Reset && rrFeature && contextData->feature != nullptr &&
        contextData->feature->GetUpscalerType() == Upscaler::OpenNrRr)
        state.newBackend = Upscaler::OpenNrRr;
    if (state.newBackend == Upscaler::Reset || dlssOnNonCapable)
        state.newBackend = cfg.VulkanUpscaler.value_or_default();
    if (state.newBackend == Upscaler::OpenNrRr && contextData->changeBackendCounter == 0)
    {
        std::string why;
        if (!rrFeature || !OpenNrRr::VkUsable(device, pd, &why))
        {
            LOG_WARN("OpenRR not taken: {}", rrFeature ? why : "not a Ray Reconstruction feature");
            state.newBackend = Upscaler::Reset;
            state.changeBackend[handleId] = false;
            return true;
        }
    }

    contextData->changeBackendCounter++;

    LOG_INFO("changeBackend is true, counter: {0}", contextData->changeBackendCounter);

    // first release everything
    if (contextData->changeBackendCounter == 1)
    {
        if (contextData->feature != nullptr)
        {
            LOG_INFO("changing backend to {0}", UpscalerDisplayName(state.newBackend));

            auto* dc = contextData->feature.get();
            // Use given params if using DLSS passthrough
            const bool isPassthrough = state.newBackend == Upscaler::DLSSD || state.newBackend == Upscaler::DLSS;

            contextData->createParams = isPassthrough ? parameters : GetNGXParameters(API::Vulkan, false);
            unsigned int dynamicResolution = 0;
            parameters->Get("OptiScaler.DynamicResolution", &dynamicResolution);
            contextData->createParams->Set("OptiScaler.DynamicResolution", dynamicResolution);
            contextData->createParams->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, dc->GetFeatureFlags());
            contextData->createParams->Set(NVSDK_NGX_Parameter_Width, dc->RenderWidth());
            contextData->createParams->Set(NVSDK_NGX_Parameter_Height, dc->RenderHeight());
            contextData->createParams->Set(NVSDK_NGX_Parameter_OutWidth, dc->DisplayWidth());
            contextData->createParams->Set(NVSDK_NGX_Parameter_OutHeight, dc->DisplayHeight());
            contextData->createParams->Set(NVSDK_NGX_Parameter_PerfQualityValue, dc->PerfQualityValue());
            if (rrFeature)
                DLSSDFeature::SyncCreateParams(contextData->createParams);

            dc = nullptr;

            vkDeviceWaitIdle(device);

            State::Instance().currentFeature = nullptr;

            Util::DelayedDestroy(std::move(contextData->feature));

            // LOG_DEBUG("sleeping before reset of current feature for 1000ms");
            // std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            // contextData->feature.reset();
            // contextData->feature = nullptr;
        }
        else
        {
            LOG_ERROR("can't find handle {0} in VkContexts!", handleId);

            state.newBackend = Upscaler::Reset;
            state.changeBackend[handleId] = false;

            if (contextData->createParams != nullptr)
            {
                TryDestroyNGXParameters(contextData->createParams, NVNGXProxy::VULKAN_DestroyParameters());
                contextData->createParams = nullptr;
            }

            contextData->changeBackendCounter = 0;

            return false;
        }

        return true;
    }

    if (contextData->changeBackendCounter == 2)
    {
        LOG_INFO("Creating new {} upscaler", UpscalerDisplayName(state.newBackend));

        contextData->feature.reset();

        if (!GetFeature(state.newBackend, handleId, contextData->createParams, &contextData->feature))
        {
            LOG_ERROR("Upscaler can't created");
            return false;
        }

        return true;
    }

    if (contextData->changeBackendCounter == 3)
    {
        // next frame create context
        auto initResult = false;
        {
            ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
            initResult =
                contextData->feature->Init(instance, pd, device, cmdBuffer, gipa, gdpa, contextData->createParams);
        }

        contextData->changeBackendCounter = 0;

        if (!initResult || !contextData->feature->ModuleLoaded())
        {
            LOG_ERROR("init failed with {0} feature", UpscalerDisplayName(state.newBackend));

            if (state.newBackend == Upscaler::OpenNrRr)
            {
                if (contextData->feature && contextData->feature->DisplayWidth() >= 960)
                {
                    OpenNrRr::Block("it did not initialise");
                    ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "OpenRR did not start, see the log" });
                }
                contextData->feature.reset();
                state.changeBackend[handleId] = false;
                return false;
            }
            else if (state.newBackend != Upscaler::DLSSD)
            {
                if (cfg.VulkanUpscaler == Upscaler::DLSS)
                {
                    state.newBackend = Upscaler::XeSS;
                    ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "Falling back to XeSS" });
                }
                else
                {
                    state.newBackend = Upscaler::FSR21;
                    ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "Falling back to FSR 2.1.2" });
                }
            }
            else
            {
                // Retry DLSSD
                state.newBackend = Upscaler::DLSSD;
            }

            state.changeBackend[handleId] = true;

            return false;
        }
        else
        {
            LOG_INFO("init successful for {0}, upscaler changed", UpscalerDisplayName(state.newBackend));

            state.newBackend = Upscaler::Reset;
            state.changeBackend[handleId] = false;
        }

        // If this is an OptiScaler fake NVNGX param table, delete it
        int optiParam = 0;

        if (contextData->createParams->Get("OptiScaler", &optiParam) == NVSDK_NGX_Result_Success && optiParam == 1)
        {
            TryDestroyNGXParameters(contextData->createParams, NVNGXProxy::VULKAN_DestroyParameters());
            contextData->createParams = nullptr;
        }
    }

    // if initial feature can't be inited
    state.currentFeature = contextData->feature.get();

    return true;
}
