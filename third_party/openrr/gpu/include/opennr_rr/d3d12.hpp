// SPDX-License-Identifier: MIT
// The kernels on a D3D12 device, recorded into someone else's command list (FASE10-D3D12-RUNTIME.md). The pipeline
// ELF the AMD Vulkan driver builds for our SPIR-V is handed to the AMD D3D12 driver through its extension interface
// (IAmdExtD3DDevice5::CreateComputePipelineFromElf) with the push constants moved to root constants; buffers are
// reached by GPU virtual address as in Vulkan, so no descriptor heap is bound.
#pragma once
#include "opennr_rr/vk.hpp"

#include <array>
#include <map>
#include <memory>
#include <mutex>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12CommandAllocator;
struct ID3D12GraphicsCommandList;
struct ID3D12Fence;
struct ID3D12RootSignature;
struct ID3D12CommandSignature;
struct ID3D12QueryHeap;
struct ID3D12DescriptorHeap;
struct ID3D12Resource;
struct IUnknown;

namespace onr {

// Root constants of every kernel: push constant dwords 3..60 at their Vulkan user data entries 4..61, then dwords
// 0..2, then entry 65 (descriptor set 0, unused here). See patch_elf.
constexpr uint32_t kRootDwords = 62;
constexpr uint32_t kMaxPushBytes = 61 * 4;

// Vulkan pipeline ELF -> the D3D12 user data layout above. Throws when the ELF does not fit the scheme.
std::vector<uint8_t> patch_elf(const std::vector<uint8_t>& vk_elf);
// threadgroup dimensions from the ELF's metadata
void elf_threads(const std::vector<uint8_t>& elf, uint32_t t[3]);
// push constants -> the kRootDwords root constants
void root_constants(const void* push, uint32_t size, uint32_t* rc);

class D3dDevice : public Gpu {
public:
    static constexpr uint32_t kSlots = 64;         // recordings in flight (0..kGameSlots-1 game lists, kOwnSlot own_)
    static constexpr uint32_t kGameSlots = kSlots - 1;
    static constexpr uint32_t kOwnSlot = kSlots - 1;
    static constexpr uint32_t kOverflowDescSlots = 64;
    static constexpr uint32_t kSlotQueries = 192;  // timestamps per recording
    static constexpr uint32_t kSlotViews = 18;     // set 0 of the d3d_ kernels per recording: 16 SRVs, then 2 UAVs
    static constexpr uint32_t kTableViews = 12;    // set 0 of the others: 8 combined image samplers of 48 bytes
    static constexpr uint32_t kSlotStride = kSlotViews + kTableViews;

    ~D3dDevice() override;
    // dev: the game's device (kept referenced). compiler: the Vulkan device on the same adapter that builds the
    // pipeline ELFs (outlives this one). cache_dir: where built ELFs are kept between runs (empty: no cache).
    bool init(ID3D12Device* dev, Device* compiler, const std::wstring& cache_dir, std::string& err);
    ID3D12Device* d3d() const { return dev_; }
    uint64_t bytes() const { return bytes_; }
    bool vram(uint64_t& used, uint64_t& budget) const;
    uint64_t timestamp_frequency() const { return ts_freq_; }
    // ELFs taken from the cache / built since init
    uint32_t cache_hits() const { return hits_; }
    uint32_t cache_misses() const { return misses_; }

    // Recording into cl until unbind(): ring slot `slot` (upload memory, timestamps). unbind resolves the timestamps.
    uint64_t bind(ID3D12GraphicsCommandList* cl, uint32_t slot);
    void unbind();
    uint32_t bound_slot() const { return slot_; }
    bool using_overflow() const { return using_overflow_; }
    bool all_retired();
    ID3D12GraphicsCommandList* list() const { return cl_; }
    // views of the game's textures in the bound recording's descriptor table (set 0 of the d3d_ kernels, its low
    // address in root constant 61). False without a table: the descriptor size is not the 32 bytes the ELFs assume.
    bool views() const { return heap_ != nullptr; }
    void srv(ID3D12Resource* r, uint32_t index, uint32_t dxgi_format);
    void uav(ID3D12Resource* r, uint32_t index, uint32_t dxgi_format);
    // the slot's timestamps once its list ran (ticks of timestamp_frequency()); false while they are not written yet
    bool read_stamps(uint32_t slot, std::vector<uint64_t>& ts);
    // a list of its own on a queue of its own (create-time work and tests): begin_own(), recording, submit_own()
    void begin_own();
    void submit_own();
    // texture <-> buffer copies on the bound list; states are the texture's (left as given)
    void copy_texture_to_buffer(ID3D12Resource* tex, uint32_t state, const Buffer& dst, uint64_t off, uint32_t pitch, uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h);
    // every resource / copy of one batch with one barrier each way
    struct TexCopy { ID3D12Resource* tex; uint32_t state; uint64_t off; uint32_t pitch, x, y; };
    void copy_textures_to_buffer(const TexCopy* c, uint32_t n, const Buffer& dst);
    void copy_buffer_to_texture(const Buffer& src, uint32_t pitch, ID3D12Resource* tex, uint32_t state, uint32_t x, uint32_t y, uint32_t w,
                                uint32_t h);

    Buffer create(VkDeviceSize size) override;
    void destroy(Buffer& b) override;
    void destroy_raw(IUnknown* u) { bury(u); }
    void upload(const Buffer& b, const void* src, VkDeviceSize size, VkDeviceSize off = 0) override;
    void download(const Buffer& b, void* dst, VkDeviceSize size, VkDeviceSize off = 0) override;
    void fill(const Buffer& b, uint32_t value) override;
    Kernel load(const std::string& spv_path, uint32_t subgroup = 0) override;
    void destroy(Kernel& k) override;
    void dispatch(const Kernel& k, const void* push, uint32_t push_size, uint32_t gx, uint32_t gy = 1, uint32_t gz = 1) override;
    void dispatch_indirect(const Kernel& k, const void* push, uint32_t push_size, const Buffer& args, VkDeviceSize off = 0) override;
    void fill(const Buffer& b, VkDeviceSize off, VkDeviceSize size, uint32_t value) override;
    void update(const Buffer& b, VkDeviceSize off, VkDeviceSize size, const void* data) override;
    void barrier() override;
    void barrier_wide() override { barrier(); }
    // per_dispatch: a timestamp before and after every dispatch too (measurements; serialises them)
    void set_stamps(bool per_dispatch) { per_dispatch_ = per_dispatch; }
    void stamp() override;
    uint32_t queries() const override { return nquery_; }
    std::vector<std::string> stamp_names;   // per_dispatch: the kernel of each stamped dispatch, this recording
    // The sampled images of the other kernels' set 0 (the fast post's history taps): a linear image over a buffer,
    // its descriptor taken from a Vulkan device of its own (VK_EXT_descriptor_buffer) with the base address moved to
    // the D3D12 buffer, in a table of the heap. init_sampler checks where the address sits in the descriptor; without
    // it alias_image returns an empty image and the post blends by hand.
    bool init_sampler(std::string& why);
    bool sampler_ready() const { return srd_ok_; }
    void sampler_off() { srd_ok_ = false, srd_.reset(); }
    Image alias_image(const Buffer& b, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bpp) override;
    void destroy(Image& i) override;
    VkSampler sampler(bool linear, bool unnormalized) override;
    void destroy(VkSampler) override {}
    void set_texture(uint32_t slot, const Image& i, VkSampler s) override;

private:
    std::vector<uint8_t> elf_of(const std::string& spv_path, uint32_t subgroup);
    void bind_kernel(const Kernel& k, const void* push, uint32_t push_size);
    void staged(const Buffer& b, uint64_t off, uint64_t size, const void* data, uint32_t fill_value, bool is_fill);
    void wait_own();

    ID3D12Device* dev_ = nullptr;
    void* ext_ = nullptr;   // IAmdExtD3DDevice5
    Device* compiler_ = nullptr;
    std::wstring cache_;
    std::string driver_key_;
    ID3D12RootSignature* rs_ = nullptr;
    ID3D12CommandSignature* dispatch_sig_ = nullptr;
    ID3D12CommandQueue* q_ = nullptr;
    ID3D12CommandAllocator* ca_ = nullptr;
    ID3D12GraphicsCommandList* own_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    uint64_t fv_ = 0;
    struct SlotState {
        uint64_t serial = 0;
        bool own = false;
        uint64_t own_fv = 0;
        uint32_t query_count = 0;
    };
    struct OverflowUpload {
        uint64_t serial = 0;
        uint32_t desc_slot = 0;
        ID3D12Resource* ring = nullptr;
        uint8_t* map = nullptr;
        ID3D12QueryHeap* qh = nullptr;
        ID3D12Resource* qrb = nullptr;
        volatile uint64_t* done = nullptr;
    };
    struct Grave {
        std::vector<uint64_t> wait_serials;
        IUnknown* obj = nullptr;
    };
    ID3D12Resource* ring_ = nullptr;   // upload heap, kSlots regions
    uint8_t* ring_map_ = nullptr;
    ID3D12Resource* cur_ring_ = nullptr;
    uint8_t* cur_ring_map_ = nullptr;
    uint64_t ring_off_ = 0, ring_end_ = 0;
    volatile uint64_t* done_map_ = nullptr;
    SlotState slots_[kSlots] = {};
    std::vector<OverflowUpload> overflow_;
    uint64_t current_serial_ = 0;
    bool using_overflow_ = false;
    ID3D12QueryHeap* qh_ = nullptr;
    ID3D12Resource* qrb_ = nullptr;    // readback of the timestamps
    uint64_t ts_freq_ = 1;
    ID3D12GraphicsCommandList* cl_ = nullptr;
    uint32_t slot_ = 0, desc_slot_ = 0, overflow_cursor_ = 0, nquery_ = 0;
    ID3D12DescriptorHeap* heap_ = nullptr;
    uint64_t heap_cpu_ = 0, heap_gpu_ = 0;
    uint32_t desc_lo_ = 0;             // root constant 61 of the bound recording (d3d_ kernels)
    uint32_t table_lo_ = 0;            // root constant 61 of the others (per-slot snapshot)
    uint8_t tex_table_[8][48] = {};
    std::unique_ptr<Device> srd_;      // descriptors of linear images (alias_image)
    bool srd_ok_ = false;
    std::map<uint64_t, std::array<uint8_t, 48>> images_;
    uint64_t next_image_ = 1;
    bool dirty_ = false;               // a dispatch since the last barrier
    bool per_dispatch_ = false;
    const Kernel* bound_ = nullptr;
    std::mutex own_m_;
    uint32_t hits_ = 0, misses_ = 0;
    std::vector<Grave> grave_;
    uint64_t binds_ = 0;
    uint64_t bytes_ = 0;
    bool slot_retired(SlotState& s, uint32_t idx);
    bool overflow_retired(OverflowUpload& o);
    bool serial_retired(uint64_t serial);
    void reap(bool all = false);
    void bury(IUnknown* u);
};

}  // namespace onr
