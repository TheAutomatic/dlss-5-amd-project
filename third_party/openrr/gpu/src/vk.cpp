// SPDX-License-Identifier: MIT
#include "opennr_rr/vk.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace onr {

static void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        char b[256];
        snprintf(b, sizeof b, "%s failed (%d)", what, int(r));
        throw std::runtime_error(b);
    }
}

static constexpr VkDeviceSize kStaging = 64ull << 20;

// every Device adopting a game's device shares the one queue its setup added (one per feature): VkQueue needs
// external synchronization, so their one-shot submits take turns
static std::mutex g_queue_m;

// a one-shot submit on the runtime's queue, waited for; a device that stops answering fails it instead of hanging
static void submit_wait(VkDevice dev, VkQueue q, VkCommandBuffer cmd, VkFence fence) {
    std::lock_guard l(g_queue_m);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    check(vkQueueSubmit(q, 1, &si, fence), "vkQueueSubmit");
    const VkResult r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
    if (r == VK_SUCCESS) vkResetFences(dev, 1, &fence);
    check(r, "vkWaitForFences");
}


Device::~Device() {
    if (!dev_) return;
    // hosted: the host's device is idle by now (the runtime's Destroy contract); everything goes at once
    const bool hosted = std::exchange(hosted_, false);
    if (!hosted) vkDeviceWaitIdle(dev_);
    sweep(true);
    destroy(staging_);
    if (qpool_) vkDestroyQueryPool(dev_, qpool_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev_, layout_, nullptr);
    if (d3d_layout_) vkDestroyPipelineLayout(dev_, d3d_layout_, nullptr);
    if (d3d_set_layout_) vkDestroyDescriptorSetLayout(dev_, d3d_set_layout_, nullptr);
    if (dpool_) vkDestroyDescriptorPool(dev_, dpool_, nullptr);
    if (vpool_) vkDestroyDescriptorPool(dev_, vpool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    if (fence_) vkDestroyFence(dev_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);
    if (hosted) return;
    vkDestroyDevice(dev_, nullptr);
    if (inst_) vkDestroyInstance(inst_, nullptr);
}

bool Device::init(std::string& err, const DeviceOptions& opt) {
    try {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "opennr-rr";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        check(vkCreateInstance(&ici, nullptr, &inst_), "vkCreateInstance");

        uint32_t n = 0;
        vkEnumeratePhysicalDevices(inst_, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(inst_, &n, devs.data());
        for (auto d : devs) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
            vkGetPhysicalDeviceProperties2(d, &p);
            if (opt.luid ? id.deviceLUIDValid && !memcmp(id.deviceLUID, opt.luid, VK_LUID_SIZE)
                         : p.properties.vendorID == 0x1002 && p.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                phys_ = d;
                break;
            }
        }
        if (!phys_ && n && !opt.luid) phys_ = devs[0];
        if (!phys_) throw std::runtime_error(opt.luid ? "no Vulkan device with the adapter's LUID" : "no Vulkan device");

        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
        vkGetPhysicalDeviceProperties2(phys_, &p2);
        info_.name = p2.properties.deviceName;
        info_.driver_version = p2.properties.driverVersion;
        info_.driver_info = drv.driverInfo;
        ts_period_ns_ = p2.properties.limits.timestampPeriod;
        vkGetPhysicalDeviceMemoryProperties(phys_, &memp_);

        // features we use
        VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
        VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
        VkPhysicalDeviceShaderFloat8FeaturesEXT f8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, &pe};
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, &f8};
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &cm};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13};
        VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};

        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ne, exts.data());
        auto has = [&](const char* e) {
            return std::any_of(exts.begin(), exts.end(), [&](auto& x) { return !strcmp(x.extensionName, e); });
        };
        std::vector<const char*> want;
        const bool has_cm = has(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        const bool has_f8 = has(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
        if (has_cm) want.push_back(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        if (has_f8) want.push_back(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
        const bool calibrated = has(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        if (calibrated) want.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        stats_ = has(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
        if (stats_) want.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
        else f8.pNext = nullptr;
        if (!has_f8) cm.pNext = stats_ ? &pe : nullptr;
        if (!has_cm) v13.pNext = nullptr;
        // aliased shared blocks (vector views of the LDS of the network kernels)
        VkPhysicalDeviceDescriptorBufferFeaturesEXT dbf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
        const bool has_desc = opt.descriptors && has(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
        if (has_desc) {
            want.push_back(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
            dbf.pNext = v11.pNext;
            v11.pNext = &dbf;
        }
        if (opt.interop) {
            for (const char* e : {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"})
                if (has(e)) want.push_back(e);
                else throw std::runtime_error(std::string("device lacks ") + e);
        }
        if (has(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME)) {
            want.push_back(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
            wl.pNext = v11.pNext;
            v11.pNext = &wl;
        }
        vkGetPhysicalDeviceFeatures2(phys_, &f2);
        info_.coopmat = has_cm && cm.cooperativeMatrix;
        info_.fp8_coopmat = has_f8 && f8.shaderFloat8 && f8.shaderFloat8CooperativeMatrix;
        info_.fp64 = f2.features.shaderFloat64;
        info_.int64_atomics = v12.shaderBufferInt64Atomics;
        host_query_reset_ = v12.hostQueryReset != VK_FALSE;
        if (!v12.bufferDeviceAddress || !v12.shaderFloat16 || !v11.storageBuffer16BitAccess || !f2.features.shaderInt64)
            throw std::runtime_error("device lacks bufferDeviceAddress / float16 / 16-bit storage / int64");
        if (opt.interop && !v12.timelineSemaphore) throw std::runtime_error("device lacks timeline semaphores");

        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &nq, qf.data());
        qfam_ = UINT32_MAX;
        for (uint32_t i = 0; i < nq; ++i)
            if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { qfam_ = i; break; }
        if (qfam_ == UINT32_MAX) throw std::runtime_error("no graphics+compute queue");
        float prio = 1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfam_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = uint32_t(want.size());
        dci.ppEnabledExtensionNames = want.data();
        check(vkCreateDevice(phys_, &dci, nullptr, &dev_), "vkCreateDevice");
        vkGetDeviceQueue(dev_, qfam_, 0, &queue_);
        if (calibrated) calibrated_ = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(vkGetDeviceProcAddr(dev_, "vkGetCalibratedTimestampsEXT"));
        if (opt.descriptors && dbf.descriptorBuffer)
            get_descriptor_ = reinterpret_cast<PFN_vkGetDescriptorEXT>(vkGetDeviceProcAddr(dev_, "vkGetDescriptorEXT"));

        setup_common();
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

// pools, layouts, the query pool and the staging buffer, on dev_ / qfam_
void Device::setup_common() {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = qfam_;
    check(vkCreateCommandPool(dev_, &pci, nullptr, &pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(dev_, &cai, &cmd_), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(dev_, &fci, nullptr, &fence_), "vkCreateFence");

    VkDescriptorSetLayoutBinding bind{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTexSlots, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorBindingFlags bflags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = 1;
    bfi.pBindingFlags = &bflags;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi};
    dli.bindingCount = 1;
    dli.pBindings = &bind;
    check(vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTexSlots * (hosted_ ? 64u : 1u)};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = hosted_ ? 64 : 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    check(vkCreateDescriptorPool(dev_, &dpi, nullptr, &dpool_), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = dpool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set_layout_;
    check(vkAllocateDescriptorSets(dev_, &dai, &set_), "vkAllocateDescriptorSets");

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 256};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &set_layout_;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcr;
    check(vkCreatePipelineLayout(dev_, &lci, nullptr, &layout_), "vkCreatePipelineLayout");

    VkDescriptorSetLayoutBinding db[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16, VK_SHADER_STAGE_COMPUTE_BIT},
                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2, VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorBindingFlags dflags[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo dfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    dfi.bindingCount = 2;
    dfi.pBindingFlags = dflags;
    VkDescriptorSetLayoutCreateInfo ddi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &dfi};
    ddi.bindingCount = 2;
    ddi.pBindings = db;
    check(vkCreateDescriptorSetLayout(dev_, &ddi, nullptr, &d3d_set_layout_), "vkCreateDescriptorSetLayout");
    lci.pSetLayouts = &d3d_set_layout_;
    check(vkCreatePipelineLayout(dev_, &lci, nullptr, &d3d_layout_), "vkCreatePipelineLayout");

    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = kMaxQueries;
    check(vkCreateQueryPool(dev_, &qpi, nullptr, &qpool_), "vkCreateQueryPool");
}

bool Device::adopt(const HostDevice& h, std::string& err) {
    try {
        hosted_ = true;
        phys_ = h.physical;
        dev_ = h.device;
        qfam_ = h.queue_family;
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
        vkGetPhysicalDeviceProperties2(phys_, &p2);
        info_.name = p2.properties.deviceName;
        info_.driver_version = p2.properties.driverVersion;
        info_.driver_info = drv.driverInfo;
        ts_period_ns_ = p2.properties.limits.timestampPeriod;
        vkGetPhysicalDeviceMemoryProperties(phys_, &memp_);
        {
            uint32_t ne = 0;
            vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ne, nullptr);
            std::vector<VkExtensionProperties> exts(ne);
            vkEnumerateDeviceExtensionProperties(phys_, nullptr, &ne, exts.data());
            for (auto& e : exts) budget_ |= !strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        }
        // the host enabled these through PrepareVkDevice, which asked the GPU the same
        VkPhysicalDeviceShaderFloat8FeaturesEXT f8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, &f8};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &cm};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};
        vkGetPhysicalDeviceFeatures2(phys_, &f2);
        info_.coopmat = cm.cooperativeMatrix;
        info_.fp8_coopmat = f8.shaderFloat8 && f8.shaderFloat8CooperativeMatrix;
        info_.fp64 = f2.features.shaderFloat64;
        host_query_reset_ = v12.hostQueryReset != VK_FALSE;
        if (!host_query_reset_)
            throw std::runtime_error("the host's device lacks hostQueryReset");
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &nq, nullptr);
        for (uint32_t i = 0; i < nq; ++i) families_.push_back(i);
        vkGetDeviceQueue(dev_, h.queue_family, h.queue_index, &queue_);
        if (!queue_)
            throw std::runtime_error("the host's device has no queue " + std::to_string(h.queue_index) + " in family " +
                                     std::to_string(h.queue_family));
        setup_common();
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

void Device::bury(const Grave& g) {
    Grave item = g;
    for (const auto& q : active_queries_) item.wait_serials.push_back(q.serial);
    if (current_serial_) item.wait_serials.push_back(current_serial_);
    grave_.push_back(std::move(item));
    sweep(false);
}

void Device::sweep(bool all) {
    if (!all && dev_) {
        std::erase_if(active_queries_, [&](const QueryProof& q) {
            if (q.serial == current_serial_) return false;
            if (binds_ - q.serial >= kViewSlots) return true;
            uint64_t ts = 0;
            return vkGetQueryPoolResults(dev_, q.pool, q.query, 1, sizeof(ts), &ts, sizeof(ts), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
        });
    } else if (all) {
        active_queries_.clear();
    }
    size_t keep = 0;
    for (auto& g : grave_) {
        if (!all) {
            std::erase_if(g.wait_serials, [&](uint64_t s) {
                if (s == current_serial_) return false;
                return std::none_of(active_queries_.begin(), active_queries_.end(), [&](const QueryProof& q) { return q.serial == s; });
            });
            if (!g.wait_serials.empty()) {
                grave_[keep++] = std::move(g);
                continue;
            }
        }
        if (g.set) vkFreeDescriptorSets(dev_, dpool_, 1, &g.set);
        if (g.vset) vkFreeDescriptorSets(dev_, vpool_, 1, &g.vset);
        if (g.qpool) vkDestroyQueryPool(dev_, g.qpool, nullptr);
        if (g.view) vkDestroyImageView(dev_, g.view, nullptr);
        if (g.img) vkDestroyImage(dev_, g.img, nullptr);
        if (g.buf) vkDestroyBuffer(dev_, g.buf, nullptr);
        if (g.mem) vkFreeMemory(dev_, g.mem, nullptr);
        if (g.pipe) vkDestroyPipeline(dev_, g.pipe, nullptr);
        if (g.module) vkDestroyShaderModule(dev_, g.module, nullptr);
    }
    grave_.resize(keep);
}

bool Device::pool_retired(VkQueryPool q) {
    if (!q) return true;
    sweep(false);
    return std::none_of(active_queries_.begin(), active_queries_.end(), [&](const QueryProof& p) { return p.pool == q; });
}

bool Device::all_retired() {
    sweep(false);
    return grave_.empty() && active_queries_.empty();
}

bool Device::vram(uint64_t& used, uint64_t& budget) const {
    if (!budget_) return false;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 m{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b};
    vkGetPhysicalDeviceMemoryProperties2(phys_, &m);
    used = budget = 0;
    for (uint32_t i = 0; i < m.memoryProperties.memoryHeapCount; ++i)
        if (m.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) used += b.heapUsage[i], budget += b.heapBudget[i];
    return true;
}

uint32_t Device::mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memp_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memp_.memoryTypes[i].propertyFlags & want) == want) return i;
    throw std::runtime_error("no memory type");
}

Buffer Device::create_ex(VkDeviceSize size, VkMemoryPropertyFlags props, void** map) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = std::max<VkDeviceSize>(size, 16);
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (hosted_ && families_.size() > 1) {
        bci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bci.queueFamilyIndexCount = uint32_t(families_.size());
        bci.pQueueFamilyIndices = families_.data();
    }
    check(vkCreateBuffer(dev_, &bci, nullptr, &b.buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b.buf, &req);
    VkMemoryAllocateFlagsInfo afi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    afi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &afi};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mem_type(req.memoryTypeBits, props);
    check(vkAllocateMemory(dev_, &mai, nullptr, &b.mem), "vkAllocateMemory");
    bytes_ += size;
    vkBindBufferMemory(dev_, b.buf, b.mem, 0);
    VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    dai.buffer = b.buf;
    b.addr = vkGetBufferDeviceAddress(dev_, &dai);
    if (map) check(vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, map), "vkMapMemory");
    return b;
}

Buffer Device::create(VkDeviceSize size) { return create_ex(size, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr); }

void Device::destroy(Buffer& b) {
    if (b.mem) bytes_ -= std::min(bytes_, uint64_t(b.size));
    if (hosted_ && (b.buf || b.mem)) {
        Grave g;
        g.buf = b.buf, g.mem = b.mem;
        bury(g);
        b = Buffer{};
        return;
    }
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev_, b.mem, nullptr);
    b = Buffer{};
}

void Device::one_shot_copy(VkBuffer src, VkBuffer dst, VkDeviceSize so, VkDeviceSize dof, VkDeviceSize size) {
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    VkBufferCopy c{so, dof, size};
    vkCmdCopyBuffer(cmd_, src, dst, 1, &c);
    vkEndCommandBuffer(cmd_);
    submit_wait(dev_, queue_, cmd_, fence_);
}

void Device::upload(const Buffer& b, const void* src, VkDeviceSize size, VkDeviceSize off) {
    if (!size) return;
    const VkDeviceSize need = std::min(kStaging, std::max<VkDeviceSize>((size + 65535) & ~VkDeviceSize(65535), 4ull << 20));
    if (staging_.size < need) {
        destroy(staging_);
        staging_ = create_ex(need, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_map_);
    }
    for (VkDeviceSize done = 0; done < size;) {
        VkDeviceSize n = std::min(staging_.size, size - done);
        memcpy(staging_map_, static_cast<const char*>(src) + done, size_t(n));
        one_shot_copy(staging_.buf, b.buf, 0, off + done, n);
        done += n;
    }
}

void Device::download(const Buffer& b, void* dst, VkDeviceSize size, VkDeviceSize off) {
    if (!size) return;
    const VkDeviceSize need = std::min(kStaging, std::max<VkDeviceSize>((size + 65535) & ~VkDeviceSize(65535), 4ull << 20));
    if (staging_.size < need) {
        destroy(staging_);
        staging_ = create_ex(need, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_map_);
    }
    for (VkDeviceSize done = 0; done < size;) {
        VkDeviceSize n = std::min(staging_.size, size - done);
        one_shot_copy(b.buf, staging_.buf, off + done, 0, n);
        memcpy(static_cast<char*>(dst) + done, staging_map_, size_t(n));
        done += n;
    }
}

void Device::fill(const Buffer& b, uint32_t value) {
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd_, &bi);
    vkCmdFillBuffer(cmd_, b.buf, 0, VK_WHOLE_SIZE, value);
    vkEndCommandBuffer(cmd_);
    submit_wait(dev_, queue_, cmd_, fence_);
}

Kernel Device::load(const std::string& path, uint32_t subgroup) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<char> code((std::istreambuf_iterator<char>(f)), {});
    Kernel k;
    k.name = path;
    VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sci.codeSize = code.size();
    sci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    check(vkCreateShaderModule(dev_, &sci, nullptr, &k.module), "vkCreateShaderModule");
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = k.module;
    cpi.stage.pName = "main";
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    rss.requiredSubgroupSize = subgroup;
    if (subgroup) {
        cpi.stage.pNext = &rss;
        cpi.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    }
    k.views = std::filesystem::path(path).stem().string().rfind("d3d_", 0) == 0;
    cpi.layout = k.views ? d3d_layout_ : layout_;
    if (stats_) cpi.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    const char* isa = stats_ ? getenv("ONR_ISA") : nullptr;   // directory: dump the driver's internal representations
    if (isa) cpi.flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    check(vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &cpi, nullptr, &k.pipe), "vkCreateComputePipelines");
    if (isa) {
        auto ir = reinterpret_cast<PFN_vkGetPipelineExecutableInternalRepresentationsKHR>(vkGetDeviceProcAddr(dev_, "vkGetPipelineExecutableInternalRepresentationsKHR"));
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = k.pipe;
        uint32_t n = 0;
        if (ir && ir(dev_, &ei, &n, nullptr) == VK_SUCCESS && n) {
            std::vector<VkPipelineExecutableInternalRepresentationKHR> rs(n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
            ir(dev_, &ei, &n, rs.data());
            std::vector<std::vector<char>> data(n);
            for (uint32_t i = 0; i < n; ++i) data[i].resize(rs[i].dataSize), rs[i].pData = data[i].data();
            ir(dev_, &ei, &n, rs.data());
            std::string stem = std::filesystem::path(path).stem().string();
            for (uint32_t i = 0; i < n; ++i) {
                std::ofstream o(std::string(isa) + "/" + stem + "." + std::to_string(i) + ".txt", std::ios::binary);
                o << rs[i].name << '\n' << rs[i].description << '\n';
                o.write(data[i].data(), std::streamsize(data[i].size()));
            }
        }
    }
    return k;
}

std::vector<uint8_t> Device::pipeline_elf(const std::string& path, uint32_t subgroup) {
    if (!stats_) return {};
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<char> code((std::istreambuf_iterator<char>(f)), {});
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sci.codeSize = code.size();
    sci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    check(vkCreateShaderModule(dev_, &sci, nullptr, &mod), "vkCreateShaderModule");
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    rss.requiredSubgroupSize = subgroup;
    if (subgroup) {
        cpi.stage.pNext = &rss;
        cpi.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    }
    cpi.layout = std::filesystem::path(path).stem().string().rfind("d3d_", 0) == 0 ? d3d_layout_ : layout_;
    cpi.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = vkCreateComputePipelines(dev_, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe);
    std::vector<uint8_t> elf;
    if (r == VK_SUCCESS) {
        auto ir = reinterpret_cast<PFN_vkGetPipelineExecutableInternalRepresentationsKHR>(
            vkGetDeviceProcAddr(dev_, "vkGetPipelineExecutableInternalRepresentationsKHR"));
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = pipe;
        uint32_t n = 0;
        if (ir && ir(dev_, &ei, &n, nullptr) == VK_SUCCESS && n) {
            std::vector<VkPipelineExecutableInternalRepresentationKHR> rs(n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
            ir(dev_, &ei, &n, rs.data());
            std::vector<std::vector<uint8_t>> data(n);
            for (uint32_t i = 0; i < n; ++i) data[i].resize(rs[i].dataSize), rs[i].pData = data[i].data();
            ir(dev_, &ei, &n, rs.data());
            const uint8_t magic[] = {0x7f, 'E', 'L', 'F'};
            for (uint32_t i = 0; i < n && elf.empty(); ++i) {
                auto at = std::search(data[i].begin(), data[i].end(), magic, magic + 4);
                if (!strncmp(rs[i].name, "ELF", 3) && at != data[i].end()) elf.assign(at, data[i].end());
            }
        }
        vkDestroyPipeline(dev_, pipe, nullptr);
    }
    vkDestroyShaderModule(dev_, mod, nullptr);
    check(r, "vkCreateComputePipelines");
    return elf;
}

std::string Device::stats(const Kernel& k) {
    if (!stats_) return {};
    auto props = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(vkGetDeviceProcAddr(dev_, "vkGetPipelineExecutableStatisticsKHR"));
    if (!props) return {};
    VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
    ei.pipeline = k.pipe;
    uint32_t n = 0;
    props(dev_, &ei, &n, nullptr);
    std::vector<VkPipelineExecutableStatisticKHR> st(n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
    props(dev_, &ei, &n, st.data());
    std::string r;
    for (auto& s : st) {
        char b[160];
        if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) snprintf(b, sizeof b, "%s=%llu ", s.name, (unsigned long long)s.value.u64);
        else if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR) snprintf(b, sizeof b, "%s=%lld ", s.name, (long long)s.value.i64);
        else continue;
        r += b;
    }
    return r;
}

void Device::destroy(Kernel& k) {
    if (hosted_ && (k.pipe || k.module)) {
        Grave g;
        g.pipe = k.pipe, g.module = k.module;
        bury(g);
        k = Kernel{};
        return;
    }
    if (k.pipe) vkDestroyPipeline(dev_, k.pipe, nullptr);
    if (k.module) vkDestroyShaderModule(dev_, k.module, nullptr);
    k = Kernel{};
}

Image Device::create_image(uint32_t w, uint32_t h, VkFormat fmt, const Buffer& src) {
    Image im;
    im.w = w;
    im.h = h;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    check(vkCreateImage(dev_, &ici, nullptr, &im.img), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, im.img, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(dev_, &mai, nullptr, &im.mem), "vkAllocateMemory");
    vkBindImageMemory(dev_, im.img, im.mem, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(dev_, &vci, nullptr, &im.view), "vkCreateImageView");

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd_, &bi);
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = im.img;
    ib.subresourceRange = vci.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(cmd_, &di);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd_, src.buf, im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier2(cmd_, &di);
    vkEndCommandBuffer(cmd_);
    submit_wait(dev_, queue_, cmd_, fence_);
    return im;
}

Image Device::alias_image(const Buffer& b, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bpp) {
    Image im;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    if (hosted_ && families_.size() > 1) {
        ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ici.queueFamilyIndexCount = uint32_t(families_.size());
        ici.pQueueFamilyIndices = families_.data();
    }
    if (vkCreateImage(dev_, &ici, nullptr, &im.img) != VK_SUCCESS) return {};
    VkImageSubresource sr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout lay{};
    vkGetImageSubresourceLayout(dev_, im.img, &sr, &lay);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, im.img, &req);
    // the buffer's rows must be the image's rows
    if (lay.offset != 0 || lay.rowPitch != VkDeviceSize(w) * bpp || req.size > b.size || vkBindImageMemory(dev_, im.img, b.mem, 0) != VK_SUCCESS) {
        vkDestroyImage(dev_, im.img, nullptr);
        return {};
    }
    im.w = w, im.h = h;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(dev_, &vci, nullptr, &im.view), "vkCreateImageView");
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd_, &bi);
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = im.img;
    ib.subresourceRange = vci.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(cmd_, &di);
    vkEndCommandBuffer(cmd_);
    submit_wait(dev_, queue_, cmd_, fence_);
    return im;
}

void Device::destroy(Image& i) {
    if (hosted_ && (i.img || i.view || i.mem)) {
        Grave g;
        g.img = i.img, g.view = i.view, g.mem = i.mem;
        bury(g);
        i = Image{};
        return;
    }
    if (i.view) vkDestroyImageView(dev_, i.view, nullptr);
    if (i.img) vkDestroyImage(dev_, i.img, nullptr);
    if (i.mem) vkFreeMemory(dev_, i.mem, nullptr);
    i = Image{};
}

VkSampler Device::sampler(bool linear, bool unnormalized) {
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.unnormalizedCoordinates = unnormalized;
    VkSampler r;
    check(vkCreateSampler(dev_, &s, nullptr, &r), "vkCreateSampler");
    return r;
}

bool Device::combined_descriptor(const Image& im, VkSampler s, uint8_t out[48]) {
    if (!get_descriptor_ || !im.view) return false;
    VkDescriptorImageInfo ii{s, im.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorGetInfoEXT gi{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    gi.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    gi.data.pCombinedImageSampler = &ii;
    get_descriptor_(dev_, &gi, 48, out);
    return true;
}

void Device::destroy(VkSampler s) {
    if (s) vkDestroySampler(dev_, s, nullptr);
}

void Device::set_texture(uint32_t slot, const Image& i, VkSampler s) {
    VkDescriptorImageInfo ii{s, i.view, VK_IMAGE_LAYOUT_GENERAL};
    // hosted: a set a recording of the game may still read is not written again; a fresh one takes the slots
    if (hosted_ && set_bound_) {
        VkDescriptorSet fresh = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &set_layout_;
        check(vkAllocateDescriptorSets(dev_, &dai, &fresh), "vkAllocateDescriptorSets");
        Grave g;
        g.set = set_;
        bury(g);
        set_ = fresh;
        set_bound_ = false;
        for (uint32_t k = 0; k < kTexSlots; ++k) {
            if (!slots_[k].imageView || k == slot) continue;
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set_;
            w.dstArrayElement = k;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &slots_[k];
            vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
        }
    }
    if (slot < kTexSlots) slots_[slot] = ii;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
}

void Device::use_views(uint32_t slot) {
    if (!vpool_) {
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 * kViewSlots * 2},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kViewSlots * 2}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpi.maxSets = kViewSlots * 2;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = ps;
        check(vkCreateDescriptorPool(dev_, &dpi, nullptr, &vpool_), "vkCreateDescriptorPool");
    }
    if (slot >= kViewSlots) {
        VkDescriptorSet fresh = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = vpool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &d3d_set_layout_;
        check(vkAllocateDescriptorSets(dev_, &dai, &fresh), "vkAllocateDescriptorSets");
        Grave g;
        g.vset = fresh;
        bury(g);
        views_ = fresh;
        return;
    }
    if (!vsets_[slot]) {
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = vpool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &d3d_set_layout_;
        check(vkAllocateDescriptorSets(dev_, &dai, &vsets_[slot]), "vkAllocateDescriptorSets");
    }
    views_ = vsets_[slot];
}

void Device::set_view(uint32_t index, VkImageView view, VkImageLayout layout) {
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, index < 16 ? layout : VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = views_;
    w.dstBinding = index < 16 ? 0 : 1;
    w.dstArrayElement = index < 16 ? index : index - 16;
    w.descriptorCount = 1;
    w.descriptorType = index < 16 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
}

bool Device::calibrate(uint64_t& gpu, uint64_t& qpc) const {
    if (!calibrated_) return false;
    VkCalibratedTimestampInfoEXT ci[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT},
                                          {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT}};
    uint64_t t[2] = {}, dev = 0;
    if (calibrated_(dev_, 2, ci, t, &dev) != VK_SUCCESS) return false;
    gpu = t[0], qpc = t[1];
    return true;
}

void Device::begin() {
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    vkCmdResetQueryPool(cmd_, qpool_, 0, kMaxQueries);
    nquery_ = 0;
}

void Device::dispatch(const Kernel& k, const void* push, uint32_t push_size, uint32_t gx, uint32_t gy, uint32_t gz) {
    set_bound_ = true;
    if (transfer_) barrier_wide();
    VkPipelineLayout lay = k.views ? d3d_layout_ : layout_;
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipe);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, k.views ? &views_ : &set_, 0, nullptr);
    if (push_size) vkCmdPushConstants(cmd_, lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
    const bool timed = per_dispatch_ && nquery_ + 2 <= kMaxQueries;
    if (timed) vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_NONE, qpool_, nquery_++);
    vkCmdDispatch(cmd_, gx, gy, gz);
    shader_ = true;
    if (timed) vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, qpool_, nquery_++);
}

void Device::dispatch_indirect(const Kernel& k, const void* push, uint32_t push_size, const Buffer& args, VkDeviceSize off) {
    set_bound_ = true;
    barrier_wide();   // the arguments were written by a shader
    VkPipelineLayout lay = k.views ? d3d_layout_ : layout_;
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipe);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, k.views ? &views_ : &set_, 0, nullptr);
    if (push_size) vkCmdPushConstants(cmd_, lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
    const bool timed = per_dispatch_ && nquery_ + 2 <= kMaxQueries;
    if (timed) vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_NONE, qpool_, nquery_++);
    vkCmdDispatchIndirect(cmd_, args.buf, off);
    shader_ = true;
    if (timed) vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, qpool_, nquery_++);
}

void Device::fill(const Buffer& b, VkDeviceSize off, VkDeviceSize size, uint32_t value) {
    if (shader_) barrier_wide();   // a shader may still read what the fill overwrites
    transfer_ = true;
    vkCmdFillBuffer(cmd_, b.buf, off, size, value);
}

void Device::update(const Buffer& b, VkDeviceSize off, VkDeviceSize size, const void* data) {
    if (shader_) barrier_wide();
    transfer_ = true;
    vkCmdUpdateBuffer(cmd_, b.buf, off, size, data);
}

void Device::bind(VkCommandBuffer c, VkQueryPool q) {
    current_serial_ = ++binds_;
    if (hosted_) sweep(false);
    own_cmd_ = cmd_, own_qpool_ = qpool_;
    if (hosted_ && (!q || !pool_retired(q))) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kMaxQueries;
        VkQueryPool fresh = VK_NULL_HANDLE;
        check(vkCreateQueryPool(dev_, &qi, nullptr, &fresh), "vkCreateQueryPool");
        Grave g;
        g.qpool = fresh;
        bury(g);
        q = fresh;
    }
    cmd_ = c, qpool_ = q;
    if (hosted_ && host_query_reset_)
        vkResetQueryPool(dev_, qpool_, 0, kMaxQueries);
    else
        vkCmdResetQueryPool(cmd_, qpool_, 0, kMaxQueries);
    nquery_ = 0;
}

void Device::stamp() {
    if (nquery_ < kMaxQueries) vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, qpool_, nquery_++);
}

void Device::unbind() {
    if (hosted_ && qpool_ && qpool_ != own_qpool_) {
        if (nquery_ == 0) stamp();
        active_queries_.push_back({qpool_, nquery_ - 1, current_serial_});
    }
    current_serial_ = 0;
    cmd_ = own_cmd_, qpool_ = own_qpool_;
}

// between kernels: compute to compute only (a wider destination also syncs the command processor); after a fill or
// an update the transfer side too
void Device::barrier() {
    if (transfer_) return barrier_wide();
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd_, &di);
}

void Device::barrier_wide() {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                       VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd_, &di);
    transfer_ = shader_ = false;
}

std::vector<double> Device::submit() {
    barrier_wide();
    vkEndCommandBuffer(cmd_);
    submit_wait(dev_, queue_, cmd_, fence_);
    std::vector<double> ms;
    if (nquery_) {
        std::vector<uint64_t> ts(nquery_);
        vkGetQueryPoolResults(dev_, qpool_, 0, nquery_, ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        for (uint32_t i = 0; i + 1 < nquery_; i += 2) ms.push_back(double(ts[i + 1] - ts[i]) * ts_period_ns_ * 1e-6);
        span_ms_ = double(ts[nquery_ - 1] - ts[0]) * ts_period_ns_ * 1e-6;
    }
    return ms;
}

}  // namespace onr
