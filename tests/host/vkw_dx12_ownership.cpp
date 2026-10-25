// VkwDx12SC queue family ownership transfers: every texture shared with D3D12 is
// VK_SHARING_MODE_EXCLUSIVE, so each handoff needs a release (Vulkan -> D3D12) and an acquire
// (D3D12 -> Vulkan) barrier naming the interop's real queue family and VK_QUEUE_FAMILY_EXTERNAL.
// The timeline semaphores only order execution; these barriers are the ownership contract.
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/with_dx12/vk_with_dx12_sc.h"
#include <cassert>
#include <cstdio>

int main()
{
    constexpr uint32_t family = 3; // Any real family index, never the EXTERNAL/IGNORED sentinels
    const auto image = (VkImage) 0x1234;

    for (const auto& [writeLayout, writeAccess] :
         {std::pair {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (VkAccessFlags) VK_ACCESS_TRANSFER_WRITE_BIT},
          std::pair {VK_IMAGE_LAYOUT_GENERAL, (VkAccessFlags) VK_ACCESS_SHADER_WRITE_BIT}})
    {
        // Vulkan -> D3D12: the release names the interop family as the source, hands the image
        // over in GENERAL and keeps no access of its own
        VkImageMemoryBarrier release {};
        VkwDx12ReleaseToD3D12(release, image, family, writeLayout, writeAccess);
        assert(release.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        assert(release.image == image);
        assert(release.srcQueueFamilyIndex == family);
        assert(release.dstQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL);
        assert(release.oldLayout == writeLayout);
        assert(release.newLayout == VK_IMAGE_LAYOUT_GENERAL);
        assert(release.srcAccessMask == writeAccess);
        assert(release.dstAccessMask == 0);
        assert(release.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
        assert(release.subresourceRange.levelCount == 1 && release.subresourceRange.layerCount == 1);

        // D3D12 -> Vulkan: the acquire takes the image back for the rewrite, the old contents
        // are discarded (UNDEFINED) and the write's access is made visible
        VkImageMemoryBarrier acquire {};
        VkwDx12AcquireFromD3D12(acquire, image, family, writeLayout, writeAccess);
        assert(acquire.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        assert(acquire.image == image);
        assert(acquire.srcQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL);
        assert(acquire.dstQueueFamilyIndex == family);
        assert(acquire.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        assert(acquire.newLayout == writeLayout);
        assert(acquire.srcAccessMask == 0);
        assert(acquire.dstAccessMask == writeAccess);
        assert(acquire.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);

        // Both directions of one handoff name the same family pair and meet in the same
        // working layout, so release and acquire pair up
        assert(acquire.dstQueueFamilyIndex == release.srcQueueFamilyIndex);
        assert(acquire.srcQueueFamilyIndex == release.dstQueueFamilyIndex);
        assert(acquire.newLayout == release.oldLayout);
    }

    // A different family index produces that family, not a fixed one
    VkImageMemoryBarrier other {};
    VkwDx12ReleaseToD3D12(other, image, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT);
    assert(other.srcQueueFamilyIndex == 0 && other.dstQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL);

    std::puts("VkwDx12 ownership transfer: PASS");
}
