#pragma once
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "nr_device_chain_sizes.hpp"

namespace nr {
// Preserve app feature fields and all known extension structures. Never write
// through the application's const pNext pointers or prepend duplicate features.
class DeviceFeatures {
    using Node = std::unique_ptr<void, decltype(&std::free)>;
    std::vector<Node> nodes;
public:
    VkBaseOutStructure* head{};
    explicit DeviceFeatures(const void* chain) {
        VkBaseOutStructure** tail = &head;
        for (auto p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext) {
            if (nodes.size() >= 512) throw std::runtime_error("cyclic or oversized device pNext");
            const size_t bytes = nr_device_chain_size(p->sType);
            if (!bytes) throw std::runtime_error("unknown device pNext sType " + std::to_string(p->sType));
            auto* copy = static_cast<VkBaseOutStructure*>(std::malloc(bytes));
            if (!copy) throw std::bad_alloc();
            nodes.emplace_back(copy, std::free);
            std::memcpy(copy, p, bytes); copy->pNext = nullptr;
            *tail = copy; tail = &copy->pNext;
        }
    }
    template<class T> T* find(VkStructureType type) {
        for (auto* p = head; p; p = p->pNext)
            if (p->sType == type) return reinterpret_cast<T*>(p);
        return nullptr;
    }
    template<class T> T* get(VkStructureType type) {
        if (auto* p = find<T>(type)) return p;
        auto* p = static_cast<T*>(std::calloc(1, sizeof(T)));
        if (!p) throw std::bad_alloc();
        nodes.emplace_back(p, std::free);
        p->sType = type; p->pNext = head;
        head = reinterpret_cast<VkBaseOutStructure*>(p);
        return p;
    }
    void enable() {
        if (auto* f = find<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES))
            f->storageBuffer16BitAccess = true;
        else get<VkPhysicalDevice16BitStorageFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES)->storageBuffer16BitAccess = true;
        if (auto* f = find<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)) {
            f->storageBuffer8BitAccess = f->shaderFloat16 = f->shaderInt8 = f->vulkanMemoryModel = true;
            f->bufferDeviceAddress = true;
        } else {
            get<VkPhysicalDevice8BitStorageFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES)->storageBuffer8BitAccess = true;
            auto* half = get<VkPhysicalDeviceShaderFloat16Int8Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
            half->shaderFloat16 = half->shaderInt8 = true;
            get<VkPhysicalDeviceVulkanMemoryModelFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES)->vulkanMemoryModel = true;
            // The network reads the activation arena through a buffer_reference on Windows
            // (vit_attn.comp NR_VBDA). vkd3d-proton enables it anyway; DXVK does not.
            get<VkPhysicalDeviceBufferDeviceAddressFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)->bufferDeviceAddress = true;
        }
        if (auto* f = find<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES))
            f->subgroupSizeControl = true;
        else get<VkPhysicalDeviceSubgroupSizeControlFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES)->subgroupSizeControl = true;
        get<VkPhysicalDeviceCooperativeMatrixFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR)->cooperativeMatrix = true;
        auto* f = get<VkPhysicalDeviceShaderFloat8FeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT);
        f->shaderFloat8 = f->shaderFloat8CooperativeMatrix = true;
    }
};
} // namespace nr
