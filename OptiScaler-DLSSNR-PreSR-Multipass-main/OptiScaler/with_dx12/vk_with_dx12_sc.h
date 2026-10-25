#pragma once

#include "SysUtils.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>

#include <mutex>
#include <vector>

// A Vulkan image backed by a D3D12 resource
struct VkwDx12Image
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    ID3D12Resource* resource = nullptr;
};

// Queue family ownership transfers for the images shared with D3D12. They are
// VK_SHARING_MODE_EXCLUSIVE and the timeline semaphores only order the execution across the API
// boundary: these barriers move the ownership between the interop's Vulkan queue family and
// VK_QUEUE_FAMILY_EXTERNAL, whose accesses are D3D12's. The release goes into the submit whose
// timeline signal D3D12 waits before its read, the acquire into the copy that already waited
// D3D12's completion (the copiedValue timeline wait or WaitForInputSlot).
inline void VkwDx12AcquireFromD3D12(VkImageMemoryBarrier& barrier, VkImage image, uint32_t queueFamily,
                                    VkImageLayout writeLayout, VkAccessFlags writeAccess)
{
    barrier = VkImageMemoryBarrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.image = image;
    // D3D12 is done with the image and its contents are rewritten
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = writeLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.dstQueueFamilyIndex = queueFamily;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = writeAccess;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
}

inline void VkwDx12ReleaseToD3D12(VkImageMemoryBarrier& barrier, VkImage image, uint32_t queueFamily,
                                  VkImageLayout writeLayout, VkAccessFlags writeAccess)
{
    barrier = VkImageMemoryBarrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.image = image;
    // GENERAL is the layout the shared resource is handed to D3D12 in
    barrier.oldLayout = writeLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = queueFamily;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.srcAccessMask = writeAccess;
    barrier.dstAccessMask = 0;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
}

// The game's Vulkan swapchain for the DX12 FG outputs, what Dx11wDx12SC is for DX11 games.
// The game renders to a real swapchain on a hidden window, its presented image is copied to a texture shared with
// D3D12 and presented through the DX12 FG swapchain on the game's window.
class VkwDx12SC
{
  public:
    struct Functions
    {
        PFN_vkCreateWin32SurfaceKHR CreateWin32Surface = nullptr;
        PFN_vkCreateSwapchainKHR CreateSwapchain = nullptr;
        PFN_vkDestroySwapchainKHR DestroySwapchain = nullptr;
        PFN_vkGetSwapchainImagesKHR GetSwapchainImages = nullptr;
        PFN_vkQueuePresentKHR QueuePresent = nullptr;
    };

    // The selected FG output only runs on DX12
    static bool Wanted();

    // Presents need the queue families of the device's queues
    static void DeviceCreated(VkDevice device, const VkDeviceCreateInfo* pCreateInfo);

    // False when the swapchain can't be presented through DX12, the game's own is created then
    static bool CreateSwapchain(VkDevice device, VkPhysicalDevice physicalDevice, VkInstance instance, HWND hwnd,
                                const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                                const Functions& functions, VkSwapchainKHR* pSwapchain, VkResult& result);

    // False when the swapchain isn't one of ours
    static bool DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator);

    static bool OwnsSwapchain(VkSwapchainKHR swapchain);

    static VkResult QueuePresent(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
    static void SetHdrMetadata(VkSwapchainKHR swapchain, const VkHdrMetadataEXT* pMetadata);

    // The devices the FG inputs are shared between, false without an FG swapchain
    static bool InteropDevices(VkDevice& device, VkPhysicalDevice& physicalDevice, ID3D12Device*& dx12Device);

    // The family of the queue the interop copies run on, UINT32_MAX before the first present
    static uint32_t InteropQueueFamily();

    // Formats the FG inputs can be shared in
    static bool CanShare(VkFormat format);

    static bool CreateSharedImage(VkDevice device, VkPhysicalDevice physicalDevice, ID3D12Device* dx12Device,
                                  VkwDx12Image& image, VkFormat format, uint32_t width, uint32_t height,
                                  VkImageUsageFlags usage);
    static void DestroySharedImage(VkDevice device, VkwDx12Image& image);

    // FG inputs of a frame index, written again once D3D12 is done with the last ones
    static bool WaitForInputSlot(int index);
    static void InputSlotUsed(int index);

  private:
    struct PresentImage
    {
        VkwDx12Image shared;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool submitted = false;
        VkSemaphore presentSemaphore = VK_NULL_HANDLE; // The hidden window's present waits for the copy
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* commandList = nullptr;
        UINT64 copiedValue = 0; // _dxDone after D3D12 copied from shared
    };

    inline static std::mutex _mutex;
    inline static VkwDx12SC* _current = nullptr;

    Functions _functions;
    VkDevice _device = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    VkInstance _instance = VK_NULL_HANDLE;
    HWND _hwnd = nullptr;
    HWND _hiddenHwnd = nullptr;
    VkSurfaceKHR _hiddenSurface = VK_NULL_HANDLE;

    // The game's handle, a real swapchain on the hidden window
    VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
    std::vector<VkSwapchainKHR> _retired;
    std::vector<VkImage> _swapchainImages;
    VkFormat _swapchainFormat = VK_FORMAT_UNDEFINED;
    VkFormat _sharedFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D _extent {};
    VkPresentModeKHR _gamePresentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkColorSpaceKHR _colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    std::vector<PresentImage> _images;
    VkCommandPool _commandPool = VK_NULL_HANDLE;
    uint32_t _commandPoolFamily = UINT32_MAX;

    IDXGIFactory4* _factory = nullptr;
    ID3D12Device* _dx12Device = nullptr;
    ID3D12CommandQueue* _dx12Queue = nullptr;
    IDXGISwapChain4* _fgSwapChain = nullptr;
    DXGI_FORMAT _dxgiFormat = DXGI_FORMAT_UNKNOWN;
    UINT _fgFlags = 0;
    bool _tearingSupported = false;

    // Vulkan signals _vkDone once the presented image is in the shared texture, D3D12 _dxDone after its copies
    ID3D12Fence* _vkDoneFence = nullptr;
    ID3D12Fence* _dxDoneFence = nullptr;
    VkSemaphore _vkDone = VK_NULL_HANDLE;
    VkSemaphore _dxDone = VK_NULL_HANDLE;
    UINT64 _vkDoneValue = 0;
    UINT64 _dxDoneValue = 0;
    HANDLE _fenceEvent = nullptr;

    UINT64 _inputSlotValues[BUFFER_COUNT] {};

    bool Init(const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator);
    VkResult Recreate(const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator);
    bool CreateDx12Objects();
    bool CreateSyncObjects();
    VkResult CreateRealSwapchain(const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                                 VkSwapchainKHR oldSwapchain);
    bool CreateFGSwapchain();
    bool ResizeFGSwapchain();
    bool CreatePresentImages();
    void DestroyPresentImages();
    bool PrepareCommandPool(uint32_t queueFamily);
    bool WaitForDx12(UINT64 value);
    void WaitForIdle();
    void Destroy(const VkAllocationCallbacks* pAllocator);

    VkResult Present(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
    bool RecordCopy(PresentImage& presentImage, VkImage swapchainImage);
    bool CopyToFGBackBuffer(PresentImage& presentImage);
    UINT SyncInterval() const;
    UINT PresentFlags() const;
};
