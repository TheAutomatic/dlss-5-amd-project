#pragma once
// Physical-device handles returned within a layer chain are not necessarily
// outer-loader handles. Bind only constructor-time physical queries; ordinary
// app callers retain the original loader route. Thread-local scope also avoids
// redirecting unrelated calls while another runtime is being initialized.
#include <vulkan/vulkan.h>
#include <stdexcept>
namespace nr::runtime_dispatch {
inline thread_local VkInstance instance{};
inline thread_local PFN_vkGetInstanceProcAddr next{};
struct Scope {
    VkInstance saved_instance{instance}; PFN_vkGetInstanceProcAddr saved_next{next};
    Scope(VkInstance i, PFN_vkGetInstanceProcAddr n) { instance = i; next = n; }
    ~Scope() { instance = saved_instance; next = saved_next; }
};
template<class T> T query(const char* name, T fallback) {
    auto fn = next ? reinterpret_cast<T>(next(instance, name)) : fallback;
    if (!fn) throw std::runtime_error(name);
    return fn;
}
inline PFN_vkVoidFunction get_instance_proc(VkInstance i, const char* name) {
    return next ? next(i, name) : vkGetInstanceProcAddr(i, name);
}
}
// Limited to this runtime translation unit, including legacy graph helpers.
#define vkGetInstanceProcAddr nr::runtime_dispatch::get_instance_proc
#define vkGetPhysicalDeviceFeatures2 nr::runtime_dispatch::query("vkGetPhysicalDeviceFeatures2", ::vkGetPhysicalDeviceFeatures2)
#define vkGetPhysicalDeviceProperties nr::runtime_dispatch::query("vkGetPhysicalDeviceProperties", ::vkGetPhysicalDeviceProperties)
#define vkGetPhysicalDeviceProperties2 nr::runtime_dispatch::query("vkGetPhysicalDeviceProperties2", ::vkGetPhysicalDeviceProperties2)
#define vkGetPhysicalDeviceMemoryProperties nr::runtime_dispatch::query("vkGetPhysicalDeviceMemoryProperties", ::vkGetPhysicalDeviceMemoryProperties)
#define vkGetPhysicalDeviceQueueFamilyProperties nr::runtime_dispatch::query("vkGetPhysicalDeviceQueueFamilyProperties", ::vkGetPhysicalDeviceQueueFamilyProperties)
#define vkGetPhysicalDeviceFormatProperties nr::runtime_dispatch::query("vkGetPhysicalDeviceFormatProperties", ::vkGetPhysicalDeviceFormatProperties)
#define vkGetPhysicalDeviceFormatProperties2 nr::runtime_dispatch::query("vkGetPhysicalDeviceFormatProperties2", ::vkGetPhysicalDeviceFormatProperties2)
