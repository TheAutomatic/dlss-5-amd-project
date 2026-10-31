#include <vulkan/vulkan.h>
#include <cstdio>
#include <string>
#include <vector>

static std::string quote(const char* value) {
    std::string out = "\"";
    for (; *value; ++value) {
        unsigned char c = static_cast<unsigned char>(*value);
        if (c == '\\' || c == '"') out += '\\';
        if (c >= 32) out += static_cast<char>(c);
    }
    return out + "\"";
}

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance{};
    VkResult rc = vkCreateInstance(&ci, nullptr, &instance);
    if (rc != VK_SUCCESS) { std::fprintf(stderr, "vkCreateInstance: %d\n", rc); return 1; }
    uint32_t count = 0;
    rc = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (rc != VK_SUCCESS) { vkDestroyInstance(instance, nullptr); return 1; }
    std::vector<VkPhysicalDevice> devices(count);
    rc = vkEnumeratePhysicalDevices(instance, &count, devices.data());
    if (rc != VK_SUCCESS) { vkDestroyInstance(instance, nullptr); return 1; }
    std::printf("[");
    for (uint32_t n = 0; n < count; ++n) {
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        id.pNext = &driver;
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &id;
        vkGetPhysicalDeviceProperties2(devices[n], &props);
        char uuid[33], luid[17];
        for (int k = 0; k < 16; ++k) std::sprintf(uuid + k * 2, "%02x", id.deviceUUID[k]);
        for (int k = 0; k < 8; ++k) std::sprintf(luid + k * 2, "%02x", id.deviceLUID[k]);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(devices[n], &memory);
        unsigned long long heapBytes = 0;
        for (uint32_t k = 0; k < memory.memoryHeapCount; ++k)
            if (memory.memoryHeaps[k].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) heapBytes += memory.memoryHeaps[k].size;
        const auto& p = props.properties;
        std::printf("%s{\"index\":%u,\"name\":%s,\"vendor\":%u,\"device\":%u,"
                    "\"driverVersion\":%u,\"driverName\":%s,\"driverInfo\":%s,\"apiVersion\":%u,"
                    "\"uuid\":\"%s\",\"luid\":\"%s\",\"luidValid\":%s,\"deviceLocalHeapBytes\":%llu}",
                    n ? "," : "", n, quote(p.deviceName).c_str(), p.vendorID, p.deviceID,
                    p.driverVersion, quote(driver.driverName).c_str(), quote(driver.driverInfo).c_str(),
                    p.apiVersion, uuid, luid, id.deviceLUIDValid ? "true" : "false", heapBytes);
    }
    std::printf("]\n");
    vkDestroyInstance(instance, nullptr);
}
