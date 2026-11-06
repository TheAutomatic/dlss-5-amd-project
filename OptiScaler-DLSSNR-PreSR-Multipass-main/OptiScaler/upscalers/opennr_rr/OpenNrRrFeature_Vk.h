#pragma once
#include <upscalers/IFeature_Vk.h>
#include "opennr_rr_api.h"

#include <string>

// OpenRR in Vulkan games: the runtime's kernels on the game's own device, recorded into the game's command buffer
// (OnrRrApi5::RecordVk). The device has to be made for it at vkCreateDevice.
namespace OpenNrRr
{
// At the game's vkCreateDevice (Vulkan_Hooks): out = the game's create info plus what the kernels need. False when
// the runtime does not offer it here (the reason is kept for VkUsable).
bool PrepareVkDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* in, VkDeviceCreateInfo* out, void** setup);
// After the create, with the device (null when it failed) and the instance's API version.
void FinishVkDevice(void* setup, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion);
// Whether this device was made for OpenRR and the runtime takes the GPU and nvngx_dlssd.dll; why not otherwise.
bool VkUsable(VkDevice device, VkPhysicalDevice physicalDevice, std::string* why = nullptr);
} // namespace OpenNrRr

class OpenNrRrFeatureVk : public IFeature_Vk
{
  public:
    OpenNrRrFeatureVk(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~OpenNrRrFeatureVk();

    // OpenRR's release, as the overlay and the menu show it (not the runtime's ABI)
    feature_version Version() override { return { 0, 1, 1 }; }
    Upscaler GetUpscalerType() const override { return Upscaler::OpenNrRr; }
    bool IsWithDx12() final { return false; }

  protected:
    bool InitInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters) override;

  private:
    void* _context = nullptr;
    const OnrRrApi5* _api = nullptr;
    uint32_t _maxRenderWidth = 0;
    uint32_t _maxRenderHeight = 0;
    bool _ngxHWDepth = false;
    bool _hasNGXDepthType = false;
    int _roughnessPacked = -1;
    int _loggedHWDepth = -1;
    bool _fellBack = false;
    // the frame's images, pointed to by the OnrRrFrame textures while RecordVk runs
    OnrRrVkImage _images[24] {};
    int _nimages = 0;

    bool FillFrame(const NVSDK_NGX_Parameter& params, OnrRrFrame& frame);
    void FallBack(const std::string& why);
};
