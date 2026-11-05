// SPDX-License-Identifier: MIT
// The Vulkan host backend's device setup (ABI 5, docs/FASE23-VULKAN.md): what our kernels need from a Vulkan game's
// device, added to its VkDeviceCreateInfo at vkCreateDevice. The extensions and features come from the SPIR-V the
// runtime loads (its OpCapability list); one queue is added for the runtime's own one-shot work.
#define NOMINMAX
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "vk_host.hpp"

namespace onr_host {

namespace {

template <class T> T* chain_find(const void* p, VkStructureType s) {
    for (auto* b = static_cast<const VkBaseInStructure*>(p); b; b = b->pNext)
        if (b->sType == s) return reinterpret_cast<T*>(const_cast<VkBaseInStructure*>(b));
    return nullptr;
}

struct Setup {
    VkDeviceCreateInfo out{};
    std::vector<const char*> exts;
    std::vector<VkDeviceQueueCreateInfo> queues;
    std::vector<std::vector<float>> prios;
    VkPhysicalDeviceFeatures core{};
    VkPhysicalDevice16BitStorageFeatures s16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDevice8BitStorageFeatures s8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features f16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceVulkanMemoryModelFeatures vmm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES};
    VkPhysicalDeviceShaderAtomicInt64Features a64{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES};
    VkPhysicalDeviceDescriptorIndexingFeatures di{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceHostQueryResetFeatures hqr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
    VkPhysicalDeviceSynchronization2Features sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    VkPhysicalDeviceSubgroupSizeControlFeatures ssc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
    std::vector<std::pair<VkBool32*, VkBool32>> restore;   // the game's own structures we switched features on in
    uint32_t qfam = 0, qindex = 0;
    std::string added;

    void on(VkBool32& v, const char* name) {
        if (v) return;
        restore.push_back({&v, v});
        v = VK_TRUE;
        added += added.empty() ? name : std::string(" ") + name;
    }
    // the game's structure of this type in the chain, or ours put at its head
    template <class T> T& ext(T& own) {
        if (T* g = chain_find<T>(out.pNext, own.sType)) return *g;
        own.pNext = const_cast<void*>(out.pNext);
        out.pNext = &own;
        return own;
    }
    // a feature promoted to core: the game's VkPhysicalDeviceVulkan1xFeatures when it has one, else the extension's
    // structure (the two may not both be in a chain)
    template <class V, class T> void core_on(VkStructureType vs, VkBool32 V::*vm, T& own, VkBool32 T::*tm, const char* name) {
        if (V* v = chain_find<V>(out.pNext, vs)) on(v->*vm, name);
        else on(ext(own).*tm, name);
    }
    void put_back() {
        for (auto it = restore.rbegin(); it != restore.rend(); ++it) *it->first = it->second;
        restore.clear();
    }
};

bool has_ext(const std::vector<VkExtensionProperties>& e, const char* n) {
    return std::any_of(e.begin(), e.end(), [&](const VkExtensionProperties& x) { return !strcmp(x.extensionName, n); });
}

}  // namespace

int prepare(VkPhysicalDevice phys, const VkDeviceCreateInfo* in, VkDeviceCreateInfo* out, void** token, std::string& why) {
    *out = *in;
    *token = nullptr;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) {
        why = "the GPU's Vulkan is older than 1.3";
        return -1;
    }
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> exts(ne);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, exts.data());
    const char* const need[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
                                VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME};
    for (const char* n : need)
        if (!has_ext(exts, n)) {
            why = std::string("the GPU lacks ") + n + " (needs an RDNA4 GPU)";
            return -1;
        }
    // the EXT buffer device address excludes the core feature the kernels use (the AMD driver does not offer it)
    for (uint32_t i = 0; i < in->enabledExtensionCount; ++i)
        if (!strcmp(in->ppEnabledExtensionNames[i], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
            why = "the game enables VK_EXT_buffer_device_address";
            return -1;
        }

    // what the GPU offers
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, &wl};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, &f8};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &cm};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    const struct { VkBool32 v; const char* n; } req[] = {
        {f2.features.shaderInt64, "shaderInt64"}, {f2.features.shaderInt16, "shaderInt16"}, {f2.features.shaderFloat64, "shaderFloat64"},
        {v11.storageBuffer16BitAccess, "storageBuffer16BitAccess"}, {v12.storageBuffer8BitAccess, "storageBuffer8BitAccess"},
        {v12.shaderFloat16, "shaderFloat16"}, {v12.shaderInt8, "shaderInt8"}, {v12.bufferDeviceAddress, "bufferDeviceAddress"},
        {v12.vulkanMemoryModel, "vulkanMemoryModel"}, {v12.vulkanMemoryModelDeviceScope, "vulkanMemoryModelDeviceScope"},
        {v12.shaderBufferInt64Atomics, "shaderBufferInt64Atomics"}, {v12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
        {v12.hostQueryReset, "hostQueryReset"},
        {v13.synchronization2, "synchronization2"}, {v13.subgroupSizeControl, "subgroupSizeControl"},
        {v13.computeFullSubgroups, "computeFullSubgroups"}, {cm.cooperativeMatrix, "cooperativeMatrix"},
        {f8.shaderFloat8, "shaderFloat8"}, {f8.shaderFloat8CooperativeMatrix, "shaderFloat8CooperativeMatrix"},
        {wl.workgroupMemoryExplicitLayout, "workgroupMemoryExplicitLayout"},
        {wl.workgroupMemoryExplicitLayout16BitAccess, "workgroupMemoryExplicitLayout16BitAccess"},
    };
    for (auto& r : req)
        if (!r.v) {
            why = std::string("the GPU lacks the feature ") + r.n + " (needs an RDNA4 GPU)";
            return -1;
        }

    // a queue of our own: a compute family with one to spare, one without graphics first
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf.data());
    auto st = std::make_unique<Setup>();
    st->out = *in;
    st->queues.assign(in->pQueueCreateInfos, in->pQueueCreateInfos + in->queueCreateInfoCount);
    int pick = -1;
    uint32_t used = 0;
    for (int pass = 0; pass < 2 && pick < 0; ++pass)
        for (uint32_t f = 0; f < nq && pick < 0; ++f) {
            if (!(qf[f].queueFlags & VK_QUEUE_COMPUTE_BIT) || (pass == 0 && (qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT))) continue;
            uint32_t n = 0;
            bool flagged = false;
            for (auto& q : st->queues)
                if (q.queueFamilyIndex == f) n += q.queueCount, flagged |= q.flags != 0;
            if (flagged || n >= qf[f].queueCount) continue;
            pick = int(f), used = n;
        }
    if (pick < 0) {
        why = "no queue family has a queue left for the runtime";
        return -1;
    }
    st->qfam = uint32_t(pick), st->qindex = used;
    st->prios.resize(st->queues.size() + 1);
    bool bumped = false;
    for (size_t i = 0; i < st->queues.size(); ++i) {
        auto& q = st->queues[i];
        st->prios[i].assign(q.pQueuePriorities, q.pQueuePriorities + q.queueCount);
        if (q.queueFamilyIndex == st->qfam && !bumped) {
            st->prios[i].push_back(*std::min_element(st->prios[i].begin(), st->prios[i].end()));
            ++q.queueCount;
            bumped = true;
        }
        q.pQueuePriorities = st->prios[i].data();
    }
    if (!bumped) {
        st->prios.back() = {0.5f};
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = st->qfam;
        q.queueCount = 1;
        q.pQueuePriorities = st->prios.back().data();
        st->queues.push_back(q);
    }
    st->out.queueCreateInfoCount = uint32_t(st->queues.size());
    st->out.pQueueCreateInfos = st->queues.data();

    st->exts.assign(in->ppEnabledExtensionNames, in->ppEnabledExtensionNames + in->enabledExtensionCount);
    for (const char* n : need)
        if (std::none_of(st->exts.begin(), st->exts.end(), [&](const char* e) { return !strcmp(e, n); })) {
            st->exts.push_back(n);
            st->added += st->added.empty() ? n : std::string(" ") + n;
        }
    st->out.enabledExtensionCount = uint32_t(st->exts.size());
    st->out.ppEnabledExtensionNames = st->exts.data();

    Setup& s = *st;
    if (auto* g2 = chain_find<VkPhysicalDeviceFeatures2>(s.out.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)) {
        s.on(g2->features.shaderInt64, "shaderInt64"), s.on(g2->features.shaderInt16, "shaderInt16"), s.on(g2->features.shaderFloat64, "shaderFloat64");
    } else {
        if (in->pEnabledFeatures) s.core = *in->pEnabledFeatures;
        s.out.pEnabledFeatures = &s.core;
        s.on(s.core.shaderInt64, "shaderInt64"), s.on(s.core.shaderInt16, "shaderInt16"), s.on(s.core.shaderFloat64, "shaderFloat64");
    }
    using V11 = VkPhysicalDeviceVulkan11Features;
    using V12 = VkPhysicalDeviceVulkan12Features;
    using V13 = VkPhysicalDeviceVulkan13Features;
    constexpr auto S11 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, S12 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                   S13 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    s.core_on(S11, &V11::storageBuffer16BitAccess, s.s16, &VkPhysicalDevice16BitStorageFeatures::storageBuffer16BitAccess, "storageBuffer16BitAccess");
    s.core_on(S12, &V12::storageBuffer8BitAccess, s.s8, &VkPhysicalDevice8BitStorageFeatures::storageBuffer8BitAccess, "storageBuffer8BitAccess");
    s.core_on(S12, &V12::shaderFloat16, s.f16, &VkPhysicalDeviceShaderFloat16Int8Features::shaderFloat16, "shaderFloat16");
    s.core_on(S12, &V12::shaderInt8, s.f16, &VkPhysicalDeviceShaderFloat16Int8Features::shaderInt8, "shaderInt8");
    s.core_on(S12, &V12::bufferDeviceAddress, s.bda, &VkPhysicalDeviceBufferDeviceAddressFeatures::bufferDeviceAddress, "bufferDeviceAddress");
    s.core_on(S12, &V12::vulkanMemoryModel, s.vmm, &VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModel, "vulkanMemoryModel");
    s.core_on(S12, &V12::vulkanMemoryModelDeviceScope, s.vmm, &VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModelDeviceScope,
              "vulkanMemoryModelDeviceScope");
    s.core_on(S12, &V12::shaderBufferInt64Atomics, s.a64, &VkPhysicalDeviceShaderAtomicInt64Features::shaderBufferInt64Atomics, "shaderBufferInt64Atomics");
    s.core_on(S12, &V12::descriptorBindingPartiallyBound, s.di, &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingPartiallyBound,
              "descriptorBindingPartiallyBound");
    s.core_on(S12, &V12::hostQueryReset, s.hqr, &VkPhysicalDeviceHostQueryResetFeatures::hostQueryReset, "hostQueryReset");
    s.core_on(S13, &V13::synchronization2, s.sync2, &VkPhysicalDeviceSynchronization2Features::synchronization2, "synchronization2");
    s.core_on(S13, &V13::subgroupSizeControl, s.ssc, &VkPhysicalDeviceSubgroupSizeControlFeatures::subgroupSizeControl, "subgroupSizeControl");
    s.core_on(S13, &V13::computeFullSubgroups, s.ssc, &VkPhysicalDeviceSubgroupSizeControlFeatures::computeFullSubgroups, "computeFullSubgroups");
    s.on(s.ext(s.cm).cooperativeMatrix, "cooperativeMatrix");
    s.on(s.ext(s.f8).shaderFloat8, "shaderFloat8");
    s.on(s.ext(s.f8).shaderFloat8CooperativeMatrix, "shaderFloat8CooperativeMatrix");
    s.on(s.ext(s.wl).workgroupMemoryExplicitLayout, "workgroupMemoryExplicitLayout");
    s.on(s.ext(s.wl).workgroupMemoryExplicitLayout16BitAccess, "workgroupMemoryExplicitLayout16BitAccess");
    if (wl.workgroupMemoryExplicitLayout8BitAccess) s.on(s.ext(s.wl).workgroupMemoryExplicitLayout8BitAccess, "workgroupMemoryExplicitLayout8BitAccess");
    if (wl.workgroupMemoryExplicitLayoutScalarBlockLayout)
        s.on(s.ext(s.wl).workgroupMemoryExplicitLayoutScalarBlockLayout, "workgroupMemoryExplicitLayoutScalarBlockLayout");

    *out = s.out;
    *token = st.release();
    return 0;
}

int finish(void* token, VkDevice device, uint32_t* family, uint32_t* index, std::string* added) {
    std::unique_ptr<Setup> st(static_cast<Setup*>(token));
    if (!st) return -1;
    st->put_back();
    if (family) *family = st->qfam;
    if (index) *index = st->qindex;
    if (added) *added = st->added;
    return device ? 0 : -1;
}

}  // namespace onr_host
