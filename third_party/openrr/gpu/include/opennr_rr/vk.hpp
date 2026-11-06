// SPDX-License-Identifier: MIT
// Minimal Vulkan 1.3 compute context: one queue, device-address buffers, push-constant-only kernels.
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

namespace onr {

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress addr = 0;   // device address (Vulkan) or GPU virtual address (D3D12)
    void* res = nullptr;        // ID3D12Resource* on the D3D12 device
};

// Sampled image (only for probes and, later, game inputs). Shaders see binding 0 of set 0 as
// `sampler2D tex[kTexSlots]`.
struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};
constexpr uint32_t kTexSlots = 8;
constexpr uint32_t kMaxQueries = 256;   // timestamps per recording

struct Kernel {
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    std::string name;
    void* pso = nullptr;   // ID3D12PipelineState* on the D3D12 device
    bool views = false;    // D3D12: set 0 is the recording's views (d3d_ kernels), not the sampled-image table
};

struct DeviceInfo {
    std::string name;
    uint32_t driver_version = 0;
    std::string driver_info;
    bool fp8_coopmat = false;
    bool coopmat = false;
    bool fp64 = false;
    bool int64_atomics = false;
};

// luid: pick the adapter with this LUID (8 bytes) instead of the first discrete AMD GPU. interop: also enable the
// Win32 external memory / semaphore extensions (D3D12 shared buffers and fences).
struct DeviceOptions {
    const uint8_t* luid = nullptr;
    bool interop = false;
    bool descriptors = false;   // VK_EXT_descriptor_buffer: combined_descriptor() (the D3D12 path's history sampler)
};

// A device someone else made (a Vulkan game's, FASE23): adopted, never destroyed or waited on here. queue_family /
// queue_index: a queue of it left to us for the one-shot work (uploads, fills); the buffers are shared by every
// queue family. Objects destroyed while it is bound go some recordings later (the game may still run them).
struct HostDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0, queue_index = 0;
};

// What the engine and the kernels record through: the Vulkan device below, or the D3D12 one (d3d12.hpp) that records
// into a game's command list. Buffers are reached by address in both.
class Gpu {
public:
    virtual ~Gpu() = default;
    virtual Buffer create(VkDeviceSize size) = 0;
    virtual void destroy(Buffer& b) = 0;
    virtual void upload(const Buffer& b, const void* src, VkDeviceSize size, VkDeviceSize off = 0) = 0;
    virtual void download(const Buffer& b, void* dst, VkDeviceSize size, VkDeviceSize off = 0) = 0;
    virtual void fill(const Buffer& b, uint32_t value) = 0;
    virtual Kernel load(const std::string& spv_path, uint32_t subgroup = 0) = 0;
    virtual void destroy(Kernel& k) = 0;
    // recorded
    virtual void dispatch(const Kernel& k, const void* push, uint32_t push_size, uint32_t gx, uint32_t gy = 1, uint32_t gz = 1) = 0;
    virtual void dispatch_indirect(const Kernel& k, const void* push, uint32_t push_size, const Buffer& args, VkDeviceSize off = 0) = 0;
    virtual void fill(const Buffer& b, VkDeviceSize off, VkDeviceSize size, uint32_t value) = 0;
    virtual void update(const Buffer& b, VkDeviceSize off, VkDeviceSize size, const void* data) = 0;
    virtual void barrier() = 0;
    virtual void barrier_wide() = 0;
    virtual void stamp() = 0;
    virtual uint32_t queries() const = 0;
    // sampled images over buffers (the fast post's history taps); a device without them returns an empty image
    virtual Image alias_image(const Buffer& b, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bpp) = 0;
    virtual void destroy(Image& i) = 0;
    virtual VkSampler sampler(bool linear, bool unnormalized) = 0;
    virtual void destroy(VkSampler s) = 0;
    virtual void set_texture(uint32_t slot, const Image& i, VkSampler s) = 0;
};

class Device : public Gpu {
public:
    ~Device() override;
    bool init(std::string& err, const DeviceOptions& opt = {});
    bool adopt(const HostDevice& h, std::string& err);
    bool hosted() const { return hosted_; }
    VkInstance instance() const { return inst_; }
    VkPhysicalDevice physical() const { return phys_; }
    VkDevice vk() const { return dev_; }
    VkQueue queue() const { return queue_; }
    uint32_t family() const { return qfam_; }
    double timestamp_period_ns() const { return ts_period_ns_; }
    // a timestamp of this device's queue and the QueryPerformanceCounter at the same moment (VK_EXT_calibrated_timestamps)
    bool calibrate(uint64_t& gpu, uint64_t& qpc) const;
    uint32_t mem_type_index(uint32_t bits, VkMemoryPropertyFlags want) { return mem_type(bits, want); }
    // dispatches, fills and barriers go to c (owned by the caller) until unbind(); timestamps to q (reset here)
    void bind(VkCommandBuffer c, VkQueryPool q);
    void unbind();
    uint32_t queries() const override { return nquery_; }
    // per_dispatch false: dispatches write no timestamps, only stamp() does (the runtime's boundaries)
    void set_stamps(bool per_dispatch) { per_dispatch_ = per_dispatch; }
    void stamp() override;
    void update(const Buffer& b, VkDeviceSize off, VkDeviceSize size, const void* data) override;   // recorded
    const DeviceInfo& info() const { return info_; }
    // bytes in this device's live buffers (requested sizes)
    uint64_t bytes() const { return bytes_; }
    // device-local heaps: what the process uses and its budget (VK_EXT_memory_budget); false without the extension
    bool vram(uint64_t& used, uint64_t& budget) const;

    Buffer create(VkDeviceSize size) override;
    void destroy(Buffer& b) override;
    void upload(const Buffer& b, const void* src, VkDeviceSize size, VkDeviceSize off = 0) override;
    void download(const Buffer& b, void* dst, VkDeviceSize size, VkDeviceSize off = 0) override;
    void fill(const Buffer& b, uint32_t value) override;

    // w x h image filled from a buffer of tightly packed texels; left in GENERAL layout.
    Image create_image(uint32_t w, uint32_t h, VkFormat fmt, const Buffer& src);
    // linear image over a buffer's memory (rows of w * bpp bytes from offset 0), left in GENERAL; empty if the driver lays it out
    // otherwise. destroy() leaves the buffer's memory alone (mem stays null).
    Image alias_image(const Buffer& b, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bpp) override;
    void destroy(Image& i) override;
    VkSampler sampler(bool linear, bool unnormalized) override;
    void destroy(VkSampler s) override;
    void set_texture(uint32_t slot, const Image& i, VkSampler s) override;   // call outside begin()/submit()
    // hosted: set 0 of the d3d_ kernels (16 sampled images, then 2 storage images) for the bound recording, one set
    // per ring slot (a slot's set is written again kViewSlots recordings later); set_view fills index 0..17 of it
    static constexpr uint32_t kViewSlots = 64;
    void use_views(uint32_t slot);
    void set_view(uint32_t index, VkImageView view, VkImageLayout layout);
    bool pool_retired(VkQueryPool q);
    bool all_retired();

    // SPIR-V file -> compute pipeline. All kernels share one layout: 256 bytes of push constants.
    Kernel load(const std::string& spv_path, uint32_t subgroup = 0) override;   // subgroup: required size, 0 = any
    void destroy(Kernel& k) override;
    std::string stats(const Kernel& k);   // compiler statistics (registers, spills), if the driver exposes them
    // the 48 bytes of a combined image sampler descriptor (DeviceOptions::descriptors); false without the extension
    bool has_descriptors() const { return get_descriptor_ != nullptr; }
    bool combined_descriptor(const Image& im, VkSampler s, uint8_t out[48]);
    // the pipeline ELF the driver builds for this SPIR-V (its "ELF" internal representation); empty when not exposed
    std::vector<uint8_t> pipeline_elf(const std::string& spv_path, uint32_t subgroup);

    // Recording: begin(), any number of dispatch()/barrier(), then submit() which waits and returns
    // the GPU time of each timed dispatch in ms (timestamps around each dispatch).
    void begin();
    void dispatch(const Kernel& k, const void* push, uint32_t push_size, uint32_t gx, uint32_t gy = 1, uint32_t gz = 1) override;
    void dispatch_indirect(const Kernel& k, const void* push, uint32_t push_size, const Buffer& args, VkDeviceSize off = 0) override;
    void fill(const Buffer& b, VkDeviceSize off, VkDeviceSize size, uint32_t value) override;   // recorded, no barrier
    void barrier() override;        // compute -> compute (wide after a fill / update)
    void barrier_wide() override;   // compute and transfer -> compute, transfer and indirect
    std::vector<double> submit();
    double span() const { return span_ms_; }   // first dispatch start to last dispatch end of the last submit, ms
    // whole-buffer copy, submitted and waited for (outside begin()/submit())
    void copy(const Buffer& src, const Buffer& dst, VkDeviceSize size) { one_shot_copy(src.buf, dst.buf, 0, 0, size); }

private:
    void setup_common();
    struct QueryProof {
        VkQueryPool pool = VK_NULL_HANDLE;
        uint32_t query = 0;
        uint64_t serial = 0;
    };
    // hosted: destroyed only after all in-flight query proofs at bury time complete on the GPU
    struct Grave {
        std::vector<uint64_t> wait_serials;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImage img = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkShaderModule module = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSet vset = VK_NULL_HANDLE;
        VkQueryPool qpool = VK_NULL_HANDLE;
    };
    void bury(const Grave& g);
    void sweep(bool all);
    void one_shot_copy(VkBuffer src, VkBuffer dst, VkDeviceSize src_off, VkDeviceSize dst_off, VkDeviceSize size);
    uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want);
    Buffer create_ex(VkDeviceSize size, VkMemoryPropertyFlags props, void** map);

    VkInstance inst_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t qfam_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    // d3d_ kernels (pipeline_elf only): set 0 = 16 sampled images, then 2 storage images, as a D3D12 descriptor table
    VkPipelineLayout d3d_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout d3d_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkQueryPool qpool_ = VK_NULL_HANDLE;
    VkCommandBuffer own_cmd_ = VK_NULL_HANDLE;
    VkQueryPool own_qpool_ = VK_NULL_HANDLE;
    uint32_t nquery_ = 0;
    bool per_dispatch_ = true;
    bool shader_ = false, transfer_ = false;   // recorded since the last barrier
    double span_ms_ = 0;
    double ts_period_ns_ = 1.0;
    VkPhysicalDeviceMemoryProperties memp_{};
    Buffer staging_;
    void* staging_map_ = nullptr;
    DeviceInfo info_;
    bool stats_ = false;
    PFN_vkGetCalibratedTimestampsEXT calibrated_ = nullptr;
    PFN_vkGetDescriptorEXT get_descriptor_ = nullptr;
    bool hosted_ = false;
    bool host_query_reset_ = false;
    std::vector<uint32_t> families_;          // hosted: the buffers' and images' concurrent queue families
    std::vector<QueryProof> active_queries_;
    std::vector<Grave> grave_;
    uint64_t binds_ = 0;
    uint64_t current_serial_ = 0;
    uint64_t bytes_ = 0;
    bool budget_ = false;                     // the GPU offers VK_EXT_memory_budget
    bool set_bound_ = false;                  // hosted: set_ was bound by a dispatch since it was written
    VkDescriptorImageInfo slots_[kTexSlots] = {};   // hosted: what set_ holds, to write a fresh set
    VkDescriptorPool vpool_ = VK_NULL_HANDLE;
    VkDescriptorSet vsets_[kViewSlots] = {};
    VkDescriptorSet views_ = VK_NULL_HANDLE;
};

}  // namespace onr
