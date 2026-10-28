#include "nr_runtime.hpp"
#include "nr_native_plan.hpp"
#include "nr_runtime_dispatch.hpp"
#include "core/nr_driver_identity.hpp"
#define NR_NO_MAIN 1
#include "nr_graph.cpp"
#include "nr_input_check.hpp"
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <string>
#include <mutex>
#include "nr_ini_network.hpp"
#if NR_INT4
#include "nr_ini_int4.hpp"
#endif
#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char*, char*, unsigned long);
#endif

// Arena reuse: byte-identical to one value one slot at 16 extents, first and fourth
// frame, model scale 1 and 0.5.
constexpr bool kArenaReuseDefault = true;

namespace nr {

// What Runtime::act_arena / RuntimeConfig::act_arena pass around (GraphAct).
struct ActArenaRef { std::shared_ptr<GraphAct> act; };
namespace {
std::mutex build_mutex;

void validate(const Controls& c) {
    auto range = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (!range(c.intensity, 0, 2) || !range(c.detail_strength, 0, 2) || !range(c.colour_strength, 0, 4) ||
        !range(c.max_ratio, 1, 8) || !range(c.local_tone, 0, 2) ||
        !range(c.local_structure, 0, 2) || !range(c.skin_structure, -1, 2) ||
        c.style < 0 || c.style > 2 || c.passes < 1)
        throw std::invalid_argument("invalid NR controls");
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
             VkImageLayout new_layout, VkPipelineStageFlags src_stage,
             VkAccessFlags src_access, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access,
             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access; b.dstAccessMask = dst_access;
    b.oldLayout = old_layout; b.newLayout = new_layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// The aspects of a depth/stencil format (zero for a colour one). A barrier on
// such an image has to name them all; a view that samples it names DEPTH only.
VkImageAspectFlags depth_stencil_aspects(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return 0;
    }
}

// `exec_only` drops the memory half. That is correct only between two network
// steps whose pipelines were built with `NR_COHERENT_ACT=1` - every activation
// load and store then carries device scope and reaches L2 itself - which is
// what `s.runner.exec_barrier` says after `build()` read `coherent-act.txt`
// beside the SPVs. Measured in `nr_graph` at -3.7% of the 1080p frame, byte-
// identical; the passes around the network keep the full barrier.
//
// `inv_only` drops the *source* half instead, which is what ships:
// the destination invalidate stays and the availability operation goes, because
// on gfx1201 there is nothing to make available - every cache below the device
// coherence point is write-through. See `runner.inv_barrier` in nr_graph.cpp for
// the argument and the numbers (1080p 7.947 -> 7.829 ms, 4K 29.457 -> 28.939).
void compute_barrier(VkCommandBuffer cmd, bool exec_only = false, bool inv_only = false) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = inv_only ? 0u : VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, exec_only ? 0u : 1u, &b,
                         0, nullptr, 0, nullptr);
}

// runtime_prep.comp's push block; see the shader.
// runtime_transfer.comp's push block, byte for byte.
struct TransferPush {
    uint32_t w, h, model_w, model_h, passthrough;
    float detail, colour, max_ratio, white;
    uint32_t mode, flags;
    float sigma;
};
static_assert(sizeof(TransferPush) == 48, "runtime_transfer.comp's push block");
constexpr uint32_t kTransferEnlarged = 1, kTransferGuided = 2, kTransferRoundU8 = 4;
// runtime_transfer.comp's mode: 0 classic, 1 matched residual, 2 edge-aware.
// Controls::transfer is kEnlargeMatched / kEnlargeEdgeAware / kEnlargeClassic.
static const uint32_t kShaderMode[3] = {1u, 2u, 0u};

struct PrepPush {
    uint32_t w, h, mode, flags, curve;
    float bias, contrast, saturation, k, cap;
    float dt;   // seconds since the last metered frame (the meter adapts in real time)
};
enum : uint32_t { kPrepAuto = 1, kPrepUnknee = 2, kPrepRestore = 4, kPrepReset = 8 };
// Each curve's input scale that leaves mid grey (0.18) where it was, so the
// curves shape shadows and highlights and the exposure alone sets brightness.
// Solved numerically, once, for each curve below.
constexpr float kPrepAnchor[7] = {1.0f, 1.0f, 1.2195122f, 2.9275228f, 1.0052344f, 0.7231708f, 0.8083602f};

void dispatch(VkCommandBuffer cmd, const nrvk::Kernel& k, uint32_t x, uint32_t y,
              uint32_t z, const void* push, uint32_t bytes, VkDescriptorSet set = VK_NULL_HANDLE) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.layout, 0, 1, set ? &set : &k.set, 0, nullptr);
    k.push_constants(cmd, bytes, push);
    vkCmdDispatch(cmd, x, y, z);
}

// Another descriptor set for a kernel's layout, over other resources: buffers
// first, then images in binding order, exactly as nrvk::Kernel::create writes
// its own (a sampler makes a combined image sampler, none a storage image). The e4m3 bindings
// read the FP16 twin of their arena and the alias bindings the arena itself, as `create` binds
// them (RDNA3); a set that skipped this made the kernel treat a byte arena as FP16.
VkDescriptorSet kernel_set(VkDevice device, VkDescriptorPool pool, const nrvk::Kernel& k,
                           const std::vector<VkBuffer>& buffers,
                           const std::vector<const nrvk::Context::Image*>& images) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &k.dsl;
    VkDescriptorSet set{};
    if (vkAllocateDescriptorSets(device, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    const uint32_t nb = uint32_t(buffers.size()), ni = uint32_t(images.size());
    std::vector<VkBuffer> bound(buffers);
    if (k.twin_map && !k.twin_map->empty())
        for (uint32_t b : k.twin_bindings) {
            const auto twin = b < nb ? k.twin_map->find(buffers[b]) : k.twin_map->end();
            if (twin != k.twin_map->end()) bound[b] = twin->second;
        }
    std::vector<uint32_t> alias_slots;
    for (uint32_t a : k.alias_bindings)
        if (a >= nrvk::Kernel::kAliasBase && a - nrvk::Kernel::kAliasBase < nb) alias_slots.push_back(a);
    const uint32_t na = uint32_t(alias_slots.size());
    std::vector<VkDescriptorBufferInfo> bi(nb + na);
    std::vector<VkDescriptorImageInfo> ii(ni);
    std::vector<VkWriteDescriptorSet> w(nb + ni + na);
    for (uint32_t i = 0; i < nb; ++i) {
        bi[i] = {bound[i], 0, VK_WHOLE_SIZE};
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set; w[i].dstBinding = i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
    }
    for (uint32_t i = 0; i < ni; ++i) {
        ii[i] = {images[i]->sampler, images[i]->view, images[i]->layout};
        w[nb + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nb + i].dstSet = set; w[nb + i].dstBinding = nb + i; w[nb + i].descriptorCount = 1;
        w[nb + i].descriptorType = images[i]->sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[nb + i].pImageInfo = &ii[i];
    }
    for (uint32_t i = 0; i < na; ++i) {
        bi[nb + i] = {buffers[alias_slots[i] - nrvk::Kernel::kAliasBase], 0, VK_WHOLE_SIZE};
        w[nb + ni + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[nb + ni + i].dstSet = set; w[nb + ni + i].dstBinding = alias_slots[i]; w[nb + ni + i].descriptorCount = 1;
        w[nb + ni + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nb + ni + i].pBufferInfo = &bi[nb + i];
    }
    vkUpdateDescriptorSets(device, nb + ni + na, w.data(), 0, nullptr);
    return set;
}

// How a caller's colour format reaches the network's RGBA32F image.
//
//   identical   - RGBA32F: a plain copy, no conversion anywhere.
//   through a   - the four 8-bit formats: the bits are copied into a private
//   transfer      UNORM image and blitted from there, so an *_SRGB view never
//   image         applies its transfer function to the stored values. This is
//                 the existing path.
//   direct blit - the wide formats a Wayland surface offers first: FP16, 16-bit
//                 UNORM and the two 10-bit packed ones, plus the packed 11/11/10
//                 float an engine renders into before its upscaler. None of them
//                 carries an sRGB transfer function, so there is no hazard to
//                 route around and `vkCmdBlitImage` converts directly. The
//                 swapchain layer only accepts these with SRGB_NONLINEAR; the
//                 engine-data path hands over linear scene-referred values and
//                 nothing here rescales or encodes them.
enum class Transfer { Identical, Encoded, DirectBlit };
#ifndef NR_DIRECT_IN
#define NR_DIRECT_IN 1
#endif
#ifndef NR_DIRECT_OUT
#define NR_DIRECT_OUT 1
#endif
#ifndef NR_DIRECT_MOTION
// The engine's motion vectors sampled by the pre and post blocks at full resolution, in place,
// as the original reads them; 0: the old blit into the estimator's quarter-resolution field.
#define NR_DIRECT_MOTION 1
#endif
#ifndef NR_DIRECT_SAMPLE
#define NR_DIRECT_SAMPLE 1
#endif

Transfer transfer_mode(VkFormat format) {
    switch(format) {
        case VK_FORMAT_R32G32B32A32_SFLOAT: return Transfer::Identical;
        // Only an *_SRGB frame needs the private UNORM image: a UNORM frame's own
        // blit is the same UNORM <-> float conversion, without the two copies.
        case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: return Transfer::Encoded;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        // The engine-data path's usual colour buffer (DXGI R11G11B10_FLOAT):
        // unsigned floats, no alpha, no transfer function, values above 1.0
        // allowed. The blit in carries them unchanged and the blit out clamps
        // anything negative to zero, which is what the format can hold.
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return Transfer::DirectBlit;
        default:
            break;
    }
    // Every other RGB colour format a frame can come in, if NR_FORMAT_FALLBACK is
    // not 0. The cases above keep their paths; this is only reached by a format
    // that used to be refused here. Blitting converts each of these to RGBA32F
    // and back by the format's own definition, and the constructor checks this
    // GPU can blit it before anything is built. Left out on purpose: integer
    // formats (a blit cannot convert them to float), sRGB ones other than the
    // two above (a blit would decode their transfer function, which the network
    // must not see), block-compressed and depth formats.
    const char* fallback = std::getenv("NR_FORMAT_FALLBACK");
    if (!(fallback && fallback[0] == '0')) switch(format) {
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R16G16B16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16_UNORM: case VK_FORMAT_R16G16B16_SNORM:
        case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_B8G8R8A8_SNORM:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
        case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_R8G8B8_SNORM:
        case VK_FORMAT_B8G8R8_UNORM: case VK_FORMAT_B8G8R8_SNORM:
        case VK_FORMAT_A2R10G10B10_SNORM_PACK32: case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
        case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16: case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
        case VK_FORMAT_A4R4G4B4_UNORM_PACK16: case VK_FORMAT_A4B4G4R4_UNORM_PACK16:
            return Transfer::DirectBlit;
        default:
            break;
    }
    throw std::invalid_argument("unsupported NR colour format, VkFormat " + std::to_string(unsigned(format)));
}

VkFormat encoded_format(VkFormat format) {
    switch(format) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return format;
    }
}
}  // namespace

// The fallback motion estimator's pyramid, as measured:
// base /4 with four levels, and a per-level
// search radius that a /2 base was shown to be *worse* than, not cheaper than.
constexpr unsigned kTemporalLevels = 4;
constexpr unsigned kTemporalBase = 4;
constexpr int kTemporalRadius[kTemporalLevels] = {2, 3, 3, 4};
constexpr float kTemporalReject = 0.50f;
// The post block's history weight is sigmoid(net) * clamp(s, 0, 1), where s is
// the model's own f16 weight <post block>.layer0.blend_scale (the post kernel's
// +104 pointer; the DLL looks it up next to the layer's weights). Both models
// are pinned by their SHA-256, so these are constants: 310.8.0 block70 0x39EB,
// the 310.9 small model's block30 0x3A23. TemporalConfig::history_strength scales it.
constexpr float kPostBlendScale = 0.73974609375f;
constexpr float kPostBlendScaleSmall = 0.76708984375f;
// The pre block's push, PushFSwin + PushPreImage (nr_graph.cpp).
constexpr uint32_t kPrePushBytes = sizeof(PushFSwin) + sizeof(PushPreImage);
constexpr uint32_t kPreSeedAt = sizeof(PushFSwin) + offsetof(PushPreImage, seed);
constexpr uint32_t kPreNoiseFieldAt = sizeof(PushFSwin) + offsetof(PushPreImage, noise_off);

// The model extent. At scale 1 it is the frame's own, unaligned, as every
// game has run so far; below 1 it is rounded to a multiple of 8 (the
// network's tile) and never above the frame.
uint32_t model_extent(uint32_t v, float scale) {
    if (scale == 1.0f) return v;
    uint32_t m = uint32_t(std::lround(double(v) * scale));
    m = (m + 4) / 8 * 8;
    return std::min(std::max(m, 8u), v);
}

struct Temporal {
    struct LumaPush { uint32_t dst_w, dst_h, src_w, src_h, mode; };
    struct FlowPush {
        uint32_t level_w, level_h, coarse_w, coarse_h;
        int32_t radius; uint32_t first, last; float reject;
    };

    bool enabled{};
    uint32_t lw[kTemporalLevels]{}, lh[kTemporalLevels]{};
    // Ping-pong: this frame's pyramid becomes next frame's previous one. The
    // descriptor sets are baked at creation, so each parity needs its own
    // pipelines; they are four tiny dispatches each and cost nothing to hold.
    nrvk::Context::Image luma[2][kTemporalLevels]{};
    nrvk::Context::Image flow[kTemporalLevels]{};
    nrvk::Context::Image flow0_sampled{};   // the same image, sampled alias
    VkSampler motion_sampler{};             // owned here; the alias does not own its image
    VkSampler motion_point_sampler{};       // RuntimeConfig::motion_point: the game's vectors sampled in place
    nrvk::Context::Image history{};
    // One pass: two history images in turn. The post variant writes the model's
    // image straight into the one the next frame reads (pre/post read
    // hist(cur), post writes hist(cur ^ 1)), instead of into surf1 and a copy -
    // a whole RGBA32F frame read and written once a frame (1080p 0.085 ms).
    nrvk::Context::Image history_b{};
    nrvk::Kernel pre_pp[2], post_pp[2];
    // What pre_pp/post_pp were built over, so a set can be made that swaps the
    // colour (and depth) for the caller's own images; see Impl::DirectSrc.
    std::vector<VkBuffer> pre_buffers, post_buffers;
    nrvk::Context::Image hist_store[2]{};
    // the post block's history, reconstructed by the pre block (RGBA32F storage,
    // model extent) when the package has the *hp variants; else an empty image.
    nrvk::Context::Image hpass{};
    bool hp{};
    // (research NR_HIST16=1): the one-pass history images in RGBA16F.
    bool h16{};
    bool pingpong{};
    uint32_t hcur{};
    nrvk::Context::Image& hist(uint32_t i) { return i ? history_b : history; }
    // Where the engine's depth is put, so the consumer has one descriptor
    // whether a game supplies depth or not. Allocated always and left cleared
    // when it is not; the shader is told which by the parameter buffer rather
    // than by reading the contents, because an all-zero depth is a legitimate
    // depth.
    nrvk::Context::Image depth{};
    nrvk::Buffer params;
    nrvk::Kernel luma_kernel[2][kTemporalLevels];
    nrvk::Kernel flow_kernel[2][kTemporalLevels];
    nrvk::Kernel pre, post;
    // post / post_pp with the Style grade (temporal_post_*_grade.spv), when the folder has them.
    nrvk::Kernel post_grade, post_grade_pp[2];
    const nrvk::Kernel* original_pre{};
    const nrvk::Kernel* original_post{};
    float history_strength{1.0f};
    uint32_t parity{};
    // The first-frame latch, the original's plan +0x68. Set after any recorded
    // frame, never cleared by `reset` - reset suppresses consumption, it does
    // not un-write the history buffer.
    bool latch{};
    // The pre block's noise seed for the next frame: frames since the first one
    // or the last reset (FeatureState::seed per feature).
    uint32_t seed{};

    void destroy(nrvk::Context& ctx) {
        for (unsigned p = 0; p < 2; ++p)
            for (unsigned k = 0; k < kTemporalLevels; ++k) {
                if (luma_kernel[p][k].device) luma_kernel[p][k].destroy();
                if (flow_kernel[p][k].device) flow_kernel[p][k].destroy();
                ctx.destroy(luma[p][k]);
            }
        for (unsigned k = 0; k < kTemporalLevels; ++k) ctx.destroy(flow[k]);
        ctx.destroy(depth);
        if (pre.device) pre.destroy();
        if (post.device) post.destroy();
        if (post_grade.device) post_grade.destroy();
        for (unsigned c = 0; c < 2; ++c) {
            if (pre_pp[c].device) pre_pp[c].destroy();
            if (post_pp[c].device) post_pp[c].destroy();
            if (post_grade_pp[c].device) post_grade_pp[c].destroy();
        }
        if (history_b.handle) ctx.destroy(history_b);
        if (hpass.handle) ctx.destroy(hpass);
        // flow0_sampled aliases flow[0]; only its sampler is ours to free.
        if (motion_sampler) vkDestroySampler(ctx.device, motion_sampler, nullptr);
        if (motion_point_sampler) vkDestroySampler(ctx.device, motion_point_sampler, nullptr);
        ctx.destroy(history);
        ctx.destroy(params);
    }
};

struct Runtime::Impl {
    NetworkModel network = NetworkModel::Original;
    float post_blend_scale = kPostBlendScale;   // this network's blend_scale
    NrSession session;
    nrvk::Kernel alpha, mask_pre, mask_resolve;
    // Compute write-back for an R8G8B8A8_UNORM frame (runtime_pack_u8.spv), used
    // only while compute_recording; graphics recordings keep the blit.
    nrvk::Kernel pack_u8; nrvk::Context::Image pack_img{}; bool pack_built{};
    nrvk::Kernel unpack_f16, pack_f16;
    nrvk::Context::Image input_f16{}, output_f16{};
    bool f16_built{};
    // compute_downscale: the scaled frame needs no blit (keep_native input, and store_native, an RGBA32F
    // frame, or scaled_pack's compute write-back of the full-size answer).
    bool compute_downscale{}; bool scaled_pack{};
    const char* convert_refusal{}; // compute_convert_colour: why a listed format kept the blits (menu status)
    bool compute_recording{}, graphics_family{true};
    void graphics_only(const char* what) const {
        if (compute_recording)
            throw std::logic_error(std::string("NR compute-family recording reached a graphics-only ") + what);
    }
    // The linear-light path: the proxy is made in place on tex_in and the
    // original kept here for the decode. Only when RuntimeConfig::linear_input.
    nrvk::Kernel encode, transfer_pass, first_transfer;
    nrvk::Context::Image keep{}, first_full{};
    nrvk::Kernel first_pack; nrvk::Context::Image first_packed{}; // RuntimeConfig::compute_export
    bool linear{};
    // Preprocess (RuntimeConfig::preprocess): runtime_prep.comp's meter,
    // forward and back modes, the frame as it came in (`prep_keep`, model
    // sized) and the meter's state. `prep_back` is `prep` itself unless later
    // passes overwrite tex_in, when the first pass's input is shown_keep and
    // the frame is put back there for the transfer pass. `transfer_prep` is the
    // transfer pass reading prep_keep as what the network was shown.
    bool prep{}, prep_unknee{};
    nrvk::Kernel prep_k, prep_back_k, transfer_prep;
    nrvk::Context::Image prep_keep{};
    nrvk::Buffer prep_state{};
    // What the last frame asked for. A change resets the meter and bumps
    // prep_gen; each history made under another generation (in the other
    // domain) is not consumed, per feature, as OptiScaler runs each pass as
    // its own feature.
    bool prep_was_on{};
    Preprocess prep_last{};
    std::chrono::steady_clock::time_point prep_metered{};
    uint32_t prep_gen{}, prep_gen_single{};
    // The pre block's noise seed on frames without a motion source (Runtime::record, Controls::noise_every_frame):
    // Advance the frame sequence once per launch, independently of motion inputs.
    // Reset restarts the sequence.
    uint32_t plain_seed{};
    float white_point{1.0f};
    // Model Resolution: the network runs at mw x mh, the frame is width x
    // height. When they differ, `keep_full` holds the frame as handed over
    // (linear or sRGB, whichever it is) and `full_out` is where the transfer
    // pass writes the full-resolution answer; both are absent otherwise.
    uint32_t mw{}, mh{};
    bool scaled{};
    // Why this runtime runs another network than it was asked for ("" when it runs the one asked for).
    std::string network_note;
    // The resize path: this runtime's images in a network memory's heap (nrvk::ImageHeap), when built inside a
    // recording; stale once another runtime took the same region (Runtime::stale).
    std::shared_ptr<nrvk::ImageHeap> heap;
    uint32_t heap_region{};
    uint64_t heap_gen{};
    // The resize path (Runtime::share_for): the build's arguments without the source extent, its working extent
    // (the native plan's padded one) and Model Resolution.
    std::vector<std::string> share_arguments;
    uint32_t work_w{}, work_h{};
    float model_scale{1.0f};
    bool native_compose{};   // RuntimeConfig::native_compose
    // The post block restores the frame's alpha (and the 8-bit rounding) in its
    // own store, so the alpha pass is skipped: native compose, one pass, no
    // Model Resolution - there the answer is the post block's output as it is.
    bool post_alpha{};
    VkFilter downscale_filter{VK_FILTER_LINEAR};
    nrvk::Context::Image keep_full{}, full_out{};
    // `keep_native`: keep_full in the frame's own format (an *_SRGB frame's UNORM twin) rather than
    // RGBA32F. The frame is copied into it as it is and runtime_downscale.comp makes the model's
    // input from it, sampling where the linear blit would (`taps`, measured from a probe blit at
    // build). `store_native`: the transfer pass (runtime_transfer_store.spv) also writes the answer
    // back into keep_full, each pixel after reading it, with the alpha pass's rounding; then there
    // is no full_out and no alpha pass, and the write-back is a copy.
    bool keep_native{}, store_native{}, compact_rgba{};
    nrvk::Kernel downscale;
    nrvk::Buffer taps{};
    // store_native with the caller's frame itself (usage SAMPLED and STORAGE, keep_full's format):
    // runtime_downscale.comp reads it and the transfer pass writes the answer back into it, so
    // neither the copy into keep_full nor the copy back is made. Views and sets per recording,
    // in a ring retired like DirectSrc's. transfer_binds / transfer_prep_binds: the images the two
    // transfer kernels were created with, [2] (keep) and [3] (answer) replaced by the frame's.
    struct FrameRw { VkImageView view{}; VkDescriptorPool pool{}; VkDescriptorSet down{}, transfer{}, transfer_prep{}; };
    std::vector<FrameRw> frame_ring;
    bool frame_rw_failed{}, frame_rw_logged{};
    std::vector<nrvk::Context::Image> transfer_binds, transfer_prep_binds;
    const FrameRw* frame_rw_source(VkImage image, VkFormat format) {
        if (frame_rw_failed) return nullptr;
        const VkDevice dev = session.ctx.device;
        if (!recording_resources && frame_ring.empty()) frame_ring.resize(size_t(kRetireAfter) + 2);
        FrameRw& f = recording_resources ? recording_resources->frames.emplace_back()
                                        : frame_ring[size_t(recordings % frame_ring.size())];
        if (f.view) { vkDestroyImageView(dev, f.view, nullptr); f.view = VK_NULL_HANDLE; }
        f.down = f.transfer = f.transfer_prep = VK_NULL_HANDLE;
        bool ok = true;
        if (!f.pool) {
            const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6},
                                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = 3; pi.poolSizeCount = 3; pi.pPoolSizes = sizes;
            ok = vkCreateDescriptorPool(dev, &pi, nullptr, &f.pool) == VK_SUCCESS;
        } else {
            ok = vkResetDescriptorPool(dev, f.pool, 0) == VK_SUCCESS;
        }
        if (ok) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ok = vkCreateImageView(dev, &vi, nullptr, &f.view) == VK_SUCCESS;
        }
        if (ok) {
            nrvk::Context::Image sampled = keep_full;   // its sampler; texelFetch ignores filtering
            sampled.handle = image; sampled.view = f.view; sampled.layout = VK_IMAGE_LAYOUT_GENERAL;
            nrvk::Context::Image storage = sampled;
            storage.sampler = VK_NULL_HANDLE;
            nrvk::Context::Image tin = session.tex_in;
            tin.sampler = VK_NULL_HANDLE; tin.layout = VK_IMAGE_LAYOUT_GENERAL;
            f.down = kernel_set(dev, f.pool, downscale, {taps.handle}, {&sampled, &tin});
            auto set_for = [&](const nrvk::Kernel& k, std::vector<nrvk::Context::Image> binds) {
                binds[2] = sampled; binds[3] = storage;
                std::vector<const nrvk::Context::Image*> p;
                for (const auto& b : binds) p.push_back(&b);
                return kernel_set(dev, f.pool, k, {}, p);
            };
            f.transfer = set_for(transfer_pass, transfer_binds);
            if (transfer_prep.device) f.transfer_prep = set_for(transfer_prep, transfer_prep_binds);
            ok = f.down && f.transfer && (!transfer_prep.device || f.transfer_prep);
        }
        if (!ok) {
            if (f.view) { vkDestroyImageView(dev, f.view, nullptr); f.view = VK_NULL_HANDLE; }
            frame_rw_failed = true;
            nr::logf("[nr] reading and writing the frame in place failed; copying instead");
            return nullptr;
        }
        if (!frame_rw_logged) {
            frame_rw_logged = true;
            nr::logf("[nr] Model resolution: frame read and written in place (VkFormat %u)", unsigned(format));
        }
        return &f;
    }
    // Classic with a scaler other than bilinear: the model's answer enlarged to the frame by
    // runtime_upscale.comp. The shown image stays the sampled model input.
    nrvk::Context::Image up_result{};
    nrvk::Kernel upscale_pass, upscale_prep;
    // Multi-pass: what the first pass was shown, kept for the transfer pass
    // because later passes overwrite tex_in; and one history per pass.
    uint32_t max_passes{1};
    nrvk::Context::Image shown_keep{};
    // The detail-only cascade (RuntimeConfig::cascade_detail_only): the log
    // luminance ratio, its separable blur (through tmp), and the pass that
    // writes the next input. Model-sized, GENERAL for life.
    bool cascade_detail{};
    int cascade_radius{4};
    nrvk::Context::Image lograt{}, cascade_tmp{}, lowpass{};
    nrvk::Kernel cascade_lograt, cascade_blur_h, cascade_blur_v, cascade_feed;
    std::vector<nrvk::Context::Image> history_store;
    // ---- what the pass costs, measured on the GPU ----------------------------
    //
    // A timestamp pair around everything `record_all` puts in the caller's
    // command buffer. A **ring** of them, read several recordings behind and
    // never with the WAIT bit: the point of the number is to be free to ask for,
    // and a query that is still in flight must not stall the render thread that
    // is asking. `timing_ok` is false on a queue family with no timestamp bits,
    // and then every accessor answers zero rather than a wrong number.
    static constexpr uint32_t kTimingSlots = 4;
    VkQueryPool timing{};
    bool timing_ok{};
    bool recording_timing_available{};
    struct RecordingTiming {
        VkDevice device;
        VkQueryPool pool{};
        uint32_t passes{};
        uint8_t stages{};
        explicit RecordingTiming(VkDevice dev) : device(dev) {}
        ~RecordingTiming() { if (pool) vkDestroyQueryPool(device, pool, nullptr); }
    };
    std::vector<std::weak_ptr<RecordingTiming>> pending_recording_timings;
    double timestamp_ns{};       // VkPhysicalDeviceLimits::timestampPeriod
    uint32_t timing_slot{};      // the slot the next recording writes
    uint64_t timings_recorded{};
    float gpu_ms{}, gpu_ms_avg{};
    // Each pass's network is timed too: a pair around its dispatches, after the pair for the
    // whole. network_ms is their sum, the rest of gpu_ms is the route's own work (input,
    // enlargement and composition, copies).
    bool split_timing{};
    uint32_t timing_stride{2};             // queries per slot: 2 + 2 x max_passes + kStageQueries
    uint32_t slot_passes[kTimingSlots]{};  // passes timed in each slot
    // Optional stage pairs (set_stage_timing, off by default): the linear-input encode and the
    // transfer (composition) pass, after the network pairs of each slot. Written only while on;
    // slot_stages says which pairs a slot holds. -1 until a pair is read, and while a stage does not run.
    static constexpr uint32_t kStageQueries = 4;
    bool stage_timing{};
    uint32_t stage_base{2};
    uint8_t slot_stages[kTimingSlots]{};
    float encode_ms{-1.0f}, transfer_ms{-1.0f};
    float stage_pair_ms(VkQueryPool pool, uint32_t first, uint32_t pair) const {
        uint64_t st[2] = {};
        if (vkGetQueryPoolResults(session.ctx.device, pool, first + stage_base + 2 * pair, 2,
                                  sizeof st, st, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return -1.0f;
        const uint64_t d = (st[1] - st[0]) & timestamp_mask;
        return float(double(d) * timestamp_ns / 1e6);
    }
    float net_ms{}, net_ms_avg{};
    uint64_t gpu_timing_serial{};
    uint64_t recording_timing_reads{}, recording_timing_rejected{};
    uint64_t timestamp_mask{~uint64_t(0)};
    float network_ms{}, other_ms{};
    // The same acceptance rules for the ordinary ring and recipe-owned pools.
    // False means not ready; true means the sample was consumed (possibly invalid).
    bool read_timing_sample(VkQueryPool pool, uint32_t first, uint32_t passes, uint8_t stages) {
        const uint32_t n = 2 + 2 * passes;
        struct Query { uint64_t stamp, available; } queries[2 + 2 * 30] = {};
        uint64_t stamps[2 + 2 * 30] = {};
        if (n > sizeof stamps / sizeof stamps[0]) return true;
        if (vkGetQueryPoolResults(session.ctx.device, pool, first, n, n * sizeof(Query), queries,
                                  sizeof(Query), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS)
            return false;   // VK_NOT_READY: still running, ask again next frame
        for (uint32_t i = 0; i < n; ++i) {
            if (!queries[i].available) return false;
            stamps[i] = queries[i].stamp;
        }
        {
            encode_ms = (stages & 1) ? stage_pair_ms(pool, first, 0) : -1.0f;
            transfer_ms = (stages & 2) ? stage_pair_ms(pool, first, 1) : -1.0f;
        }
        const uint64_t total = (stamps[1] - stamps[0]) & timestamp_mask;
        if (!total || !passes) return true;
        uint64_t net = 0;
        for (uint32_t p = 0; p < passes; ++p) {
            const uint64_t d = (stamps[3 + 2 * p] - stamps[2 + 2 * p]) & timestamp_mask;
            if (!d) return true;
            net += d;
        }
        if (net > total) return true;
        gpu_ms = float(double(total) * timestamp_ns / 1e6);
        network_ms = float(double(net) * timestamp_ns / 1e6);
        other_ms = float(double(total - net) * timestamp_ns / 1e6);
        net_ms = network_ms;
        net_ms_avg = net_ms_avg > 0.0f ? net_ms_avg + (net_ms - net_ms_avg) * 0.1f : net_ms;
        ++gpu_timing_serial;
        // A plain exponential average. Ten frames is enough to stop the last
        // digit dancing and short enough that a resolution change shows up
        // immediately rather than being dragged in from before it.
        gpu_ms_avg = gpu_ms_avg > 0.0f ? gpu_ms_avg + (gpu_ms - gpu_ms_avg) * 0.1f : gpu_ms;
        return true;
    }
    // Read the oldest slot in the ring, if its recording has finished.
    void read_timing() {
        if (!timing_ok || timings_recorded <= kTimingSlots) return;
        const uint32_t oldest = timing_slot;
        read_timing_sample(timing, oldest * timing_stride, slot_passes[oldest], slot_stages[oldest]);
    }
    void read_recording_timings() {
        for (auto it = pending_recording_timings.begin(); it != pending_recording_timings.end();) {
            auto sample = it->lock();
            if (!sample || read_timing_sample(sample->pool, 0, sample->passes, sample->stages))
                it = pending_recording_timings.erase(it);
            else
                ++it;
        }
    }
    // ---- one temporal state per feature -------------------------------------
    //
    // NVIDIA's DLL gives every NGX feature its own object, and OptiScaler builds
    // on that: one feature per model pass, features at two extents alive at
    // once, features destroyed and rebuilt whenever a control moves. What makes
    // a feature independent is its temporal state and nothing else - the
    // weights, the pipelines and the activation arena are properties of the
    // network and the extent, not of the handle.
    //
    // So a feature here is exactly this: one history image, the first-frame
    // latch and the estimator's parity. `temporal.history` stays the image the
    // pre/post descriptor sets are baked against - it is the *bound* history -
    // and a feature's own image is copied into it on the way in and back out on
    // the way out, but only when the bound feature actually changes. With one
    // feature, which is every caller that does not use this, nothing is copied
    // and the recording is byte-identical to what it always was.
    struct FeatureState {
        nrvk::Context::Image history{};
        std::shared_ptr<void> history_owner;   // Created only when a scoped recording references it.
        bool latch{};
        uint32_t parity{};
        bool cleared{};
        uint32_t prep_gen{};   // Impl::prep_gen its history was made under
        uint32_t seed{};       // Temporal::seed
    };
    std::map<uint64_t, FeatureState> features;
    uint64_t bound_feature{};
    // Released features, and the recording count at which their images may be
    // destroyed. Never a wait: the render thread that releases a feature must
    // not block on the GPU, and OptiScaler has already parked the feature for 32
    // evaluates before it gets here (DlssNr_Dx12.cpp ParkNrFeature).
    static constexpr uint64_t kRetireAfter = 64;
    std::vector<std::pair<nrvk::Context::Image, uint64_t>> retiring;
    std::vector<std::pair<std::shared_ptr<void>, uint64_t>> retiring_owned;
    uint64_t recordings{};
    // A runtime built inside a recording (RuntimeConfig::deferred): its images' first layout transitions as the first
    // recording made them, kept to make again when a recording is dropped (drop_history).
    std::vector<VkImageMemoryBarrier> first_transitions;
    // **The tone control is a frame edit and belongs to one pass.** NVIDIA's own
    // description of `LocalToneStrength` is "low-frequency details such as
    // broader lighting and colour response", and it enters the network as five
    // constant features on the pre block - the same constant on every pixel.
    // Running the network again on its own answer therefore applies that whole
    // low-frequency edit a second time, and it compounds: measured on the
    // TR2013 frame against NVIDIA's own output, the block-mean luminance ratio
    // runs 1.052 -> 1.102 -> 1.136 over three passes with the tone control on
    // each time, and 1.052 -> 1.055 -> 1.056 when only the first pass carries
    // it. That drift is the red skin the user sees at more than one pass.
    // Structure is what a later pass can legitimately add more of, so only the
    // tone feature is dropped. One alternate push blob per dispatch, parallel to
    // `session.disp`, empty for every dispatch that is not the pre block.
    //
    // **Not under `cascade_detail_only`.** That feed hands the next pass the
    // first pass's *tone* with this pass's structure on it, so the tone edit has
    // been taken back out of the picture and the network has to make it again;
    // dropping the control there moves the two-pass answer away from the
    // one-pass one instead of towards it (block drift mean 0.0163 -> 0.0273 in
    // `linear_frame_test`). The two are alternative ways to stop the same
    // stacking, so exactly one of them is active at a time.
    // [pass][dispatch]; empty means "use pass 1's push". Pass 0's entry is
    // unused - that pass writes session.disp[i].push in place.
    std::vector<std::vector<std::vector<uint8_t>>> pass_push;
    nrvk::Context::Image mask_image;
    nrvk::Buffer mask_rect;
    // the plain path's pre and post (the graph's own pipelines), for sampling the
    // caller's colour in place without the temporal path (Impl::DirectSrc, plain sets).
    const nrvk::Kernel* plain_pre{};
    const nrvk::Kernel* plain_post{};
    const nrvk::Kernel* original_pre{};
    // The post block with the Style grade (g_fswinimagepost32_grade.spv, NR_STYLE_GRADE): dispatched in place of
    // the graph's fswinimagepost32 for a pass whose Style is Natural or Cinematic, so Standard runs the pipeline
    // without the grade's code. `graph_post` is the graph's own; no grade when the shader folder has none.
    const nrvk::Kernel* graph_post{};
    nrvk::Kernel post_grade;
    bool style_grade{};
    // [Network] Style Grade (Runtime::set_style_grade): false = the post block without the grade, as 0.0.6 ran it
    bool style_grade_on{true};
    nrvk::Context::Image encoded;
    // An 8-bit frame: the answer is rounded as frame_image.py does before the
    // format-converting blit (alpha pass or the post block's store).
    bool round_u8{};
    Transfer transfer{Transfer::Identical};
    std::function<VkCommandBuffer(VkCommandBuffer, bool)> network_segments;
    // The network's input image is in the frame's own format (its UNORM twin
    // for *_SRGB) and a plain copy fills it; see the graph's --tex-in-format.
    bool direct_in{};
#if NR_INT4
    bool int4_mixed{};   // the int4 mixed network (Runtime::int4_mixed)
#endif
    // The post block stores into an image in the frame's (UNORM) format and the
    // write-back is a copy; see the graph's --out0-format.
    bool direct_out{};
    // The engine path with one pass, native compose and ping-pong history can
    // sample the caller's colour and depth in place: the pre and post blocks
    // only sample them (texel centres, the same samplers), so a view of the
    // caller's image gives the values the copy into our own image gave, and
    // the copies go.
    //
    // **The views are made every frame, from the image handed over that frame,
    // and never looked up by handle.** A game (OptiScaler, toggling its NR
    // mode) destroys and recreates these resources, and the driver hands the
    // same VkImage value back for a new image - a cached view keyed by handle
    // then samples memory the old image no longer owns (in game: vertical
    // bands of another frame). Each recording takes the next slot of a ring
    // and retires what that slot held kRetireAfter + 2 recordings ago, the
    // rule the retired feature histories already live by.
    //
    // The same slots carry the game's motion vectors (mview), which the pre
    // and post blocks sample in place whenever the frame allows it, whether or
    // not the colour is: the original reads them per pixel at full resolution,
    // and the estimator's quarter-resolution field they used to be blitted into
    // blended foreground and background vectors at every silhouette and left the
    // five-tap depth choice a quarter of a texel to work with.
    struct DirectSrc {
        VkImageView cview{}, dview{}, mview{}, tview{};
        VkDescriptorPool pool{};
        VkDescriptorSet pre[2]{}, post[2]{};
        // the post block stores into the caller's image through cview
        // (a storage descriptor) and the write-back copy goes.
        bool store{};
    };
    std::vector<DirectSrc> direct_ring;
    bool direct_src_failed{}, direct_src_logged{};
    bool motion_src_failed{}, motion_src_logged{};
    bool motion_all_passes{};   // RuntimeConfig::motion_in_place_passes with max_passes > 1
    // Formats whose sampled view filters linearly (the motion sampler is linear).
    std::map<VkFormat, bool> linear_ok;
    bool filters_linearly(VkFormat f) {
        auto it = linear_ok.find(f);
        if (it != linear_ok.end()) return it->second;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(session.ctx.physical, f, &fp);
        const bool ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        linear_ok[f] = ok;
        return ok;
    }
    // A depth/stencil depth buffer is read through a depth-aspect view by
    // runtime_depth.comp (see there). The view is over the caller's image and
    // made per recording, in a ring retired like DirectSrc's.
    nrvk::Kernel depth_copy;
    struct DepthSrc { VkImageView view{}; VkDescriptorPool pool{}; };
    std::vector<DepthSrc> depth_ring;
    bool depth_src_failed{}, depth_src_logged{};
    bool depth_unread{};   // this recording's depth/stencil buffer could not be read
    bool motion_without_depth{};   // [Network] Motion Without Depth: the pre block ignores the depth, as the DLL
    // Separate slots for every scoped call: no ring can reset their descriptor
    // pools, including when one scope contains more than one record operation.
    struct RecordingResources {
        VkDevice device;
        std::deque<DirectSrc> direct;
        std::deque<FrameRw> frames;
        std::deque<DepthSrc> depth;
        std::vector<std::shared_ptr<void>> histories;
        std::vector<std::shared_ptr<RecordingTiming>> timings;
        explicit RecordingResources(VkDevice dev) : device(dev) {}
        ~RecordingResources() {
            for (auto& d : direct) {
                if (d.cview) vkDestroyImageView(device, d.cview, nullptr);
                if (d.dview) vkDestroyImageView(device, d.dview, nullptr);
                if (d.mview) vkDestroyImageView(device, d.mview, nullptr);
                if (d.tview) vkDestroyImageView(device, d.tview, nullptr);
                if (d.pool) vkDestroyDescriptorPool(device, d.pool, nullptr);
            }
            for (auto& f : frames) {
                if (f.view) vkDestroyImageView(device, f.view, nullptr);
                if (f.pool) vkDestroyDescriptorPool(device, f.pool, nullptr);
            }
            for (auto& d : depth) {
                if (d.view) vkDestroyImageView(device, d.view, nullptr);
                if (d.pool) vkDestroyDescriptorPool(device, d.pool, nullptr);
            }
        }
    };
    std::shared_ptr<RecordingResources> recording_resources;
    struct OwnedHistory {
        nrvk::Context& ctx;
        nrvk::Context::Image image;
        OwnedHistory(nrvk::Context& context, const nrvk::Context::Image& im) : ctx(context), image(im) {}
        ~OwnedHistory() { ctx.destroy(image); }
    };
    void retain_feature_history(FeatureState& feature) {
        if (!recording_resources || !feature.history.handle) return;
        if (!feature.history_owner)
            feature.history_owner = std::make_shared<OwnedHistory>(session.ctx, feature.history);
        auto& histories = recording_resources->histories;
        if (std::find(histories.begin(), histories.end(), feature.history_owner) == histories.end())
            histories.push_back(feature.history_owner);
    }
    VkDescriptorSet depth_source(VkImage image, VkFormat format) {
        if (depth_src_failed || !depth_copy.device) return VK_NULL_HANDLE;
        auto& t = temporal;
        const VkDevice dev = session.ctx.device;
        if (!recording_resources && depth_ring.empty()) depth_ring.resize(size_t(kRetireAfter) + 2);
        DepthSrc& d = recording_resources ? recording_resources->depth.emplace_back()
                                         : depth_ring[size_t(recordings % depth_ring.size())];
        if (d.view) { vkDestroyImageView(dev, d.view, nullptr); d.view = VK_NULL_HANDLE; }
        bool ok = true;
        if (!d.pool) {
            const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = 1; pi.poolSizeCount = 2; pi.pPoolSizes = sizes;
            ok = vkCreateDescriptorPool(dev, &pi, nullptr, &d.pool) == VK_SUCCESS;
        } else {
            ok = vkResetDescriptorPool(dev, d.pool, 0) == VK_SUCCESS;
        }
        if (ok) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            ok = vkCreateImageView(dev, &vi, nullptr, &d.view) == VK_SUCCESS;
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (ok) {
            nrvk::Context::Image src = t.depth;           // its sampler; texelFetch ignores filtering
            src.handle = image; src.view = d.view; src.layout = VK_IMAGE_LAYOUT_GENERAL;
            nrvk::Context::Image dst = t.depth;
            dst.sampler = VK_NULL_HANDLE;                  // the storage alias
            set = kernel_set(dev, d.pool, depth_copy, {}, {&src, &dst});
            ok = set != VK_NULL_HANDLE;
        }
        if (!ok) {
            if (d.view) { vkDestroyImageView(dev, d.view, nullptr); d.view = VK_NULL_HANDLE; }
            depth_src_failed = true;
            nr::logf("[nr] the depth/stencil depth buffer could not be read (VkFormat %u); running without depth",
                     unsigned(format));
            return VK_NULL_HANDLE;
        }
        if (!depth_src_logged) {
            depth_src_logged = true;
            nr::logf("[nr] depth read from a depth/stencil buffer (VkFormat %u)", unsigned(format));
        }
        return set;
    }
    void direct_slot_release(DirectSrc& d) {
        const VkDevice dev = session.ctx.device;
        if (d.cview) vkDestroyImageView(dev, d.cview, nullptr);
        if (d.dview) vkDestroyImageView(dev, d.dview, nullptr);
        if (d.mview) vkDestroyImageView(dev, d.mview, nullptr);
        if (d.tview) vkDestroyImageView(dev, d.tview, nullptr);
        d.cview = d.dview = d.mview = d.tview = VK_NULL_HANDLE;
        d.pre[0] = d.pre[1] = d.post[0] = d.post[1] = VK_NULL_HANDLE;
        d.store = false;
    }
    // Any of colour (with its depth) and motion may be null: that one is our
    // own image, bound as in the sets made at creation.
    const DirectSrc* direct_source(VkImage colour, VkFormat format, VkImage depth, VkImage motion,
                                   VkFormat mformat, uint32_t c, bool store = false, VkImage target = VK_NULL_HANDLE,
                                   bool plain = false) {
        if (!colour && !motion) return nullptr;
        auto& s = session; auto& t = temporal;
        const VkDevice dev = s.ctx.device;
        if (!recording_resources && direct_ring.empty()) direct_ring.resize(size_t(kRetireAfter) + 2);
        DirectSrc& d = recording_resources ? recording_resources->direct.emplace_back()
                                          : direct_ring[size_t(recordings % direct_ring.size())];
        direct_slot_release(d);
        auto view = [&](VkImage im, VkFormat f, VkImageView* out) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = im; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            return vkCreateImageView(dev, &vi, nullptr, out) == VK_SUCCESS;
        };
        bool ok = true;
        if (!d.pool) {
            const VkDescriptorPoolSize sizes[] = {
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16},
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = 2; pi.poolSizeCount = 3; pi.pPoolSizes = sizes;
            ok = vkCreateDescriptorPool(dev, &pi, nullptr, &d.pool) == VK_SUCCESS;
        } else {
            ok = vkResetDescriptorPool(dev, d.pool, 0) == VK_SUCCESS;
        }
        const bool colour_ok = ok && (!colour || (view(colour, format, &d.cview) &&
                                                  (!depth || view(depth, VK_FORMAT_R32_SFLOAT, &d.dview))));
        const bool motion_ok = ok && (!motion || view(motion, mformat, &d.mview));
        ok = colour_ok && motion_ok && (!(colour && store && target) || view(target, format, &d.tview));
        if (ok) {
            // The caller's images, with our own samplers, in GENERAL; this
            // frame's history parity only.
            nrvk::Context::Image cim = s.tex_in, dim = t.depth, mim = t.flow0_sampled;
            if (colour) { cim.handle = colour; cim.view = d.cview; cim.layout = VK_IMAGE_LAYOUT_GENERAL; }
            if (depth) { dim.handle = depth; dim.view = d.dview; dim.layout = VK_IMAGE_LAYOUT_GENERAL; }
            if (motion) {
                mim.handle = motion; mim.view = d.mview; mim.layout = VK_IMAGE_LAYOUT_GENERAL;
                if (t.motion_point_sampler) mim.sampler = t.motion_point_sampler;   // RuntimeConfig::motion_point
            }
            nrvk::Context::Image surf0 = s.surf0;
            // the answer straight into the caller's image (same format as our own
            // answer image, storage usage): the post samples its own pixel's centre with
            // a NEAREST sampler before it stores that pixel, so nothing reads a stored texel.
            if (colour && store) {
                surf0.handle = target ? target : colour; surf0.view = target ? d.tview : d.cview;
                surf0.layout = VK_IMAGE_LAYOUT_GENERAL; surf0.sampler = VK_NULL_HANDLE; d.store = true;
            }
            if (plain) {
                // The graph's own pre and post: the buffers and images they were created with
                // (nr_graph.cpp), the caller's colour for tex_in.
                const VkBuffer a = s.act.handle, w = s.wgt.handle;
                nrvk::Context::Image surf1 = s.surf1;
                d.pre[0] = kernel_set(dev, d.pool, *plain_pre, {a, a, w, w, w, a}, {&cim});
                d.post[0] = kernel_set(dev, d.pool, *plain_post, {a, a, w, w, w}, {&surf0, &surf1, &cim});
                ok = d.pre[0] && d.post[0];
            } else {
            // One pass: this parity's ping-pong pre and post. More (no ping-pong, `c` 0): the pre and post
            // every pass runs, the model's write going to surf1 as in their own sets.
            const nrvk::Kernel& pre_k = t.pingpong ? t.pre_pp[c] : t.pre;
            const nrvk::Kernel& post_k = t.pingpong ? t.post_pp[c] : t.post;
            nrvk::Context::Image surf1 = s.surf1;
            nrvk::Context::Image* written = t.pingpong ? &t.hist_store[c ^ 1u] : &surf1;
            d.pre[c] = t.hp ? kernel_set(dev, d.pool, pre_k, t.pre_buffers, {&cim, &mim, &t.hist(c), &dim, &t.hpass})
                            : kernel_set(dev, d.pool, pre_k, t.pre_buffers, {&cim, &mim, &t.hist(c), &dim});
            d.post[c] = t.hp ? kernel_set(dev, d.pool, post_k, t.post_buffers,
                                          {&surf0, written, &cim, &mim, &t.hist(c), &t.hpass})
                             : kernel_set(dev, d.pool, post_k, t.post_buffers,
                                          {&surf0, written, &cim, &mim, &t.hist(c)});
            ok = d.pre[c] && d.post[c];
            }
        }
        if (!ok) {
            direct_slot_release(d);
            // Whichever view could not be made stops being tried; a set that
            // could not be written (both views made) stops both.
            if (colour && (!colour_ok || motion_ok)) direct_src_failed = true;
            if (motion && (!motion_ok || colour_ok)) motion_src_failed = true;
            nr::logf("[nr] sampling the caller's %s in place failed; copying instead",
                     colour && motion ? "colour and motion vectors" : colour ? "colour" : "motion vectors");
            return nullptr;
        }
        if (colour && !direct_src_logged) {
            direct_src_logged = true;
            nr::logf("[nr] colour%s sampled in place", depth ? " and depth" : "");
        }
        if (motion && !motion_src_logged) {
            motion_src_logged = true;
            nr::logf("[nr] motion vectors sampled in place at full resolution (VkFormat %u)", unsigned(mformat));
        }
        return &d;
    }
    Temporal temporal;
    VkFormat colour_format{};
    uint32_t width{}, height{};

    // ---- the in-game check (diagnostic) --------------------------------------
    //
    // For the in-game green screen (2026-09): a feature that stays wrong until
    // the next one, and nothing in the log. Every kEvery recordings the
    // recording also copies, after the network, the tile-counter region (frame
    // word, the waits' error words, the counters), each persistent run's epoch
    // and error word, and an 8x8 grid of the history the next frame reads, into
    // a host-visible buffer. kLag recordings later - that submission has long
    // run - the render thread reads it and logs whatever is off: a wait that
    // ran out of its bound (it then reads whatever is there), a counter that is
    // not need x frames, a run whose epoch is not the frame count, a history
    // holding NaN/Inf. Never a wait. Off unless NR_GAME_CHECK=1.
    struct Check {
        static constexpr uint64_t kEvery = 32, kLag = 16, kBeat = 1920;
        static constexpr uint32_t kGrid = 8;
        bool off{};
        nrvk::Buffer buf{};
        VkDeviceSize persist_at{}, hist_at{};
        uint64_t written_at{~0ull};   // the recording that copied, until read
        uint64_t feature{};           // the feature bound at that recording
        bool hist{};                  // that copy took the history
        size_t tc_errors{}, persist_errors{};
        size_t off_count{~size_t(0)};
        uint64_t beat_at{};
        std::set<uint64_t> hist_bad;
        std::map<uint64_t, int> feature_checks;
    };
    Check check;
    // NR_INPUT_CHECK (nr_input_check.hpp): what the engine path is handed and hands back.
    incheck::State incheck;
    void check_record(VkCommandBuffer cmd) {
        auto& k = check;
        auto& s = session;
        if (k.off) return;
        if (!k.buf.handle) {
            const char* e = std::getenv("NR_GAME_CHECK");
            if (!e || std::atoi(e) != 1) { k.off = true; return; }
            VkDeviceSize n = VkDeviceSize(s.tc_words) * 4;
            k.persist_at = n;
            n += VkDeviceSize(s.persist_err.size()) * 8;
            k.hist_at = n = (n + 15) & ~VkDeviceSize(15);
            n += VkDeviceSize(Check::kGrid) * Check::kGrid * 16;
            try {
                k.buf = s.ctx.buffer(n, true);
            } catch (const std::exception& x) {
                nr::logf("[nr] check: no host-visible buffer (%s), off", x.what());
                k.off = true;
                return;
            }
            nr::logf("[nr] check on (%ux%u): %zu chained pairs, %zu counters, %zu persistent runs, history %s",
                     width, height, s.tc_pair.size(), s.tc_expect.size(), s.persist_err.size(),
                     temporal.enabled ? "8x8 texels" : "none");
        }
        if (recordings % Check::kEvery) return;
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        std::vector<VkBufferCopy> r;
        if (s.tc_words) r.push_back({VkDeviceSize(s.tc_base) * 4, 0, VkDeviceSize(s.tc_words) * 4});
        for (size_t i = 0; i < s.persist_err.size(); ++i)   // epoch word, error word
            r.push_back({VkDeviceSize(s.persist_err[i]) - 4, k.persist_at + VkDeviceSize(i) * 8, 8});
        if (!r.empty()) vkCmdCopyBuffer(cmd, s.act.handle, k.buf.handle, uint32_t(r.size()), r.data());
        k.hist = temporal.enabled;
        if (k.hist) {
            const auto& h = temporal.hist(temporal.hcur);   // what the next frame reads
            std::vector<VkBufferImageCopy> ir;
            for (uint32_t y = 0; y < Check::kGrid; ++y)
                for (uint32_t x = 0; x < Check::kGrid; ++x) {
                    VkBufferImageCopy c{};
                    c.bufferOffset = k.hist_at + VkDeviceSize(y * Check::kGrid + x) * 16;
                    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    c.imageOffset = {int32_t((2 * x + 1) * h.w / (2 * Check::kGrid)),
                                     int32_t((2 * y + 1) * h.h / (2 * Check::kGrid)), 0};
                    c.imageExtent = {1, 1, 1};
                    ir.push_back(c);
                }
            vkCmdCopyImageToBuffer(cmd, h.handle, VK_IMAGE_LAYOUT_GENERAL, k.buf.handle,
                                   uint32_t(ir.size()), ir.data());
        }
        mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
        k.written_at = recordings;
        k.feature = bound_feature;
    }
    void check_read() {
        auto& k = check;
        auto& s = session;
        if (!k.buf.handle || k.written_at == ~0ull || recordings < k.written_at + Check::kLag) return;
        const uint64_t at = k.written_at;
        k.written_at = ~0ull;
        const unsigned long long fid = (unsigned long long)k.feature;
        const uint32_t* w = k.buf.as<uint32_t>();
        const std::vector<uint32_t> v(w, w + s.tc_words);
        const uint32_t* p = w + k.persist_at / 4;
        const bool have_f = s.tc_words != 0;
        const uint32_t f = have_f ? v[0] : (s.persist_err.empty() ? 0u : p[0]);
        // Waits that ran out: sticky words, so the count only grows.
        size_t te = 0;
        std::string names;
        for (size_t i = 0; i < s.tc_err.size(); ++i)
            if (v[s.tc_err[i]]) {
                ++te;
                if (names.size() < 300 && i < s.tc_pair.size()) names += (names.empty() ? "" : ", ") + s.tc_pair[i];
            }
        if (te > k.tc_errors) {
            nr::logf("[nr] check: %zu tile-counter waits ran out by frame %u (recording %llu, feature %llx): %s",
                     te, f, (unsigned long long)at, fid, names.c_str());
            k.tc_errors = te;
        }
        size_t pe = 0;
        std::string layers;
        for (size_t i = 0; i < s.persist_err.size(); ++i)
            if (p[2 * i + 1]) {
                ++pe;
                layers += " run " + std::to_string(i) + " layer " + std::to_string(p[2 * i + 1] - 1u);
            }
        if (pe > k.persist_errors) {
            nr::logf("[nr] check: %zu persistent-run waits ran out by frame %u (recording %llu, feature %llx):%s",
                     pe, f, (unsigned long long)at, fid, layers.c_str());
            k.persist_errors = pe;
        }
        // After whole frames every counter holds need x frames and every run's epoch is the frame count.
        size_t bad = 0, epoch_bad = 0;
        std::string ex;
        if (have_f) {
            for (const auto& e : s.tc_expect) {
                const uint32_t want = e.need * f;
                if (v[e.word] == want) continue;
                if (bad < 4) {
                    char b[160];
                    std::snprintf(b, sizeof b, " [%s: counter %u = %u, want %u x %u]",
                                  e.pair < s.tc_pair.size() ? s.tc_pair[e.pair].c_str() : "?", e.word, v[e.word], e.need, f);
                    ex += b;
                }
                ++bad;
            }
            for (size_t i = 0; i < s.persist_err.size(); ++i)
                if (p[2 * i] != f) {
                    if (epoch_bad < 4) ex += " [run " + std::to_string(i) + " epoch " + std::to_string(p[2 * i]) + "]";
                    ++epoch_bad;
                }
        }
        if (have_f && bad + epoch_bad != k.off_count) {
            if (bad + epoch_bad)
                nr::logf("[nr] check: frame %u (recording %llu, feature %llx): %zu of %zu counters and %zu of %zu "
                         "run epochs off:%s", f, (unsigned long long)at, fid, bad, s.tc_expect.size(), epoch_bad,
                         s.persist_err.size(), ex.c_str());
            else
                nr::logf("[nr] check: frame %u: all %zu counters = need x frames, %zu run epochs = frames%s", f,
                         s.tc_expect.size(), s.persist_err.size(), k.off_count == ~size_t(0) ? "" : " again");
            k.off_count = bad + epoch_bad;
        }
        // The history the next frame reads.
        if (!k.hist) return;
        const float* h = reinterpret_cast<const float*>(w + k.hist_at / 4);
        const uint32_t n = Check::kGrid * Check::kGrid;
        uint32_t nonfinite = 0;
        float peak = 0.0f;
        double mean[3] = {};
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t c = 0; c < 4; ++c) {
                const float x = h[i * 4 + c];
                if (!std::isfinite(x)) { ++nonfinite; continue; }
                peak = std::max(peak, std::fabs(x));
                if (c < 3) mean[c] += x / n;
            }
        const bool weird = nonfinite || peak > 1024.0f;
        int& seen = k.feature_checks[k.feature];
        ++seen;
        if (weird && k.hist_bad.insert(k.feature).second)
            nr::logf("[nr] check: history of feature %llx at frame %u: %u NaN/Inf of %u values, max |v| %g, "
                     "mean %.4f %.4f %.4f", fid, f, nonfinite, n * 4, double(peak), mean[0], mean[1], mean[2]);
        else if (seen <= 2 || at >= k.beat_at + Check::kBeat) {
            // A new feature's first two looks, then a heartbeat.
            nr::logf("[nr] check: frame %u, feature %llx: history mean %.4f %.4f %.4f, max %.4g; waits ran out "
                     "%zu+%zu, counters off %zu", f, fid, mean[0], mean[1], mean[2], double(peak), k.tc_errors,
                     k.persist_errors, k.off_count == ~size_t(0) ? size_t(0) : k.off_count);
            if (seen > 2) k.beat_at = at;
        }
        if (k.feature_checks.size() > 256) k.feature_checks.clear();
    }

    ~Impl() {
        // Caller has already completed its submitted command buffers.
        auto& s = session;
        if (!s.ctx.device) return;
        recording_resources.reset();
        for (auto& d : direct_ring) {
            direct_slot_release(d);
            if (d.pool) vkDestroyDescriptorPool(s.ctx.device, d.pool, nullptr);
        }
        for (auto& d : depth_ring) {
            if (d.view) vkDestroyImageView(s.ctx.device, d.view, nullptr);
            if (d.pool) vkDestroyDescriptorPool(s.ctx.device, d.pool, nullptr);
        }
        if (depth_copy.device) depth_copy.destroy();
        if (alpha.device) alpha.destroy();
        if (unpack_f16.device) unpack_f16.destroy();
        if (pack_f16.device) pack_f16.destroy();
        if (input_f16.handle) session.ctx.destroy(input_f16);
        if (output_f16.handle) session.ctx.destroy(output_f16);
        if (pack_u8.device) pack_u8.destroy();
        if (pack_img.handle) session.ctx.destroy(pack_img);
        if (encode.device) encode.destroy();
        if (transfer_pass.device) transfer_pass.destroy();
        if (first_transfer.device) first_transfer.destroy();
        if (first_full.handle) session.ctx.destroy(first_full);
        if (first_pack.device) first_pack.destroy();
        if (first_packed.handle) session.ctx.destroy(first_packed);
        if (prep_k.device) prep_k.destroy();
        if (prep_back_k.device) prep_back_k.destroy();
        if (transfer_prep.device) transfer_prep.destroy();
        if (prep_keep.handle) session.ctx.destroy(prep_keep);
        if (prep_state.handle) session.ctx.destroy(prep_state);
        if (keep.handle) session.ctx.destroy(keep);
        if (keep_full.handle) session.ctx.destroy(keep_full);
        if (downscale.device) downscale.destroy();
        if (taps.handle) session.ctx.destroy(taps);
        for (auto& f : frame_ring) {
            if (f.view) vkDestroyImageView(s.ctx.device, f.view, nullptr);
            if (f.pool) vkDestroyDescriptorPool(s.ctx.device, f.pool, nullptr);
        }
        if (up_result.handle) session.ctx.destroy(up_result);
        if (upscale_pass.device) upscale_pass.destroy();
        if (upscale_prep.device) upscale_prep.destroy();
        if (full_out.handle) session.ctx.destroy(full_out);
        if (shown_keep.handle) session.ctx.destroy(shown_keep);
        if (lograt.handle) session.ctx.destroy(lograt);
        if (cascade_tmp.handle) session.ctx.destroy(cascade_tmp);
        if (lowpass.handle) session.ctx.destroy(lowpass);
        if (cascade_lograt.device) cascade_lograt.destroy();
        if (cascade_blur_h.device) cascade_blur_h.destroy();
        if (cascade_blur_v.device) cascade_blur_v.destroy();
        if (cascade_feed.device) cascade_feed.destroy();
        for (auto& h : history_store) session.ctx.destroy(h);
        for (auto& f : features) {
            if (f.second.history_owner) f.second.history_owner.reset();
            else if (f.second.history.handle) session.ctx.destroy(f.second.history);
        }
        for (auto& r : retiring) if (r.first.handle) session.ctx.destroy(r.first);
        retiring_owned.clear();
        if (mask_pre.device) mask_pre.destroy();
        if (post_grade.device) post_grade.destroy();
        if (mask_resolve.device) mask_resolve.destroy();
        if (timing) { vkDestroyQueryPool(s.ctx.device, timing, nullptr); timing = VK_NULL_HANDLE; }
        if (check.buf.handle) s.ctx.destroy(check.buf);
        incheck.destroy(s.ctx);
        temporal.destroy(s.ctx);
        if (s.ctx.pipeline_cache) {
            vkDestroyPipelineCache(s.ctx.device, s.ctx.pipeline_cache, nullptr);
            s.ctx.pipeline_cache = VK_NULL_HANDLE;
        }
        s.ctx.destroy(mask_image);
        s.ctx.destroy(mask_rect);
        if (s.runner.ctx) s.runner.destroy();
        for (auto& entry : s.kern) if (entry.second.device) entry.second.destroy();
        s.ctx.destroy(s.tex_in); s.ctx.destroy(s.surf0); s.ctx.destroy(s.surf1);
        s.ctx.destroy(encoded);
        // The arenas go with the last runtime that uses them (NrSession::arenas, the resize path).
        s.share.reset(); s.core.reset(); s.arenas.reset();
        s.act = {}; s.wgt = {};
        // Do not destroy the host's instance, device or queue.
    }

    // One pass's resolved model controls. A later pass inherits pass 1 with the
    // tone zeroed - that is what the original's own hosts do, and re-applying
    // the tone control every pass compounds it (x1.052 -> x1.102 -> x1.136 over
    // three) - unless the caller gave that pass its own set.
    struct Resolved { int style; float intensity, tone, structure, skin_in; bool mask; };

    Resolved resolve_pass(const Controls& c, uint32_t pass) const {
        Resolved r{c.style, c.intensity, c.local_tone, c.local_structure, c.skin_structure,
                   c.automatic_mask};
        if (pass == 0) return r;
        if (!cascade_detail) r.tone = 0.0f;
        const size_t k = size_t(pass) - 1;
        if (k < c.per_pass.size() && c.per_pass[k].used) {
            const PassControls& o = c.per_pass[k];
            r.style = o.style; r.intensity = o.intensity; r.tone = o.local_tone;
            r.structure = o.local_structure; r.skin_in = o.skin_structure; r.mask = o.automatic_mask;
        }
        return r;
    }

    // Which dispatches carry a control at all. Everything else uses pass 1's
    // push, so a later pass costs four small blobs rather than a copy of the
    // whole table.
    static bool carries_controls(const std::string& kern) {
        return kern.rfind("fswinimagepreds32", 0) == 0 || kern.rfind("imgin", 0) == 0 ||
               kern == "fswinimagepost32" || kern.rfind("imgout", 0) == 0;
    }

    void patch_push(std::vector<uint8_t>& blob, const std::string& kern, const Resolved& r) const {
        const float skin = r.mask ? (r.skin_in < 0 ? r.structure : r.skin_in) : -1.0f;
        const float background = r.mask ? r.structure : -1.0f;
        if (kern.rfind("fswinimagepreds32", 0) == 0) {
            PushPreImage p{};
            std::memcpy(&p, blob.data() + sizeof(PushFSwin), sizeof p);
            p.style = float(r.style) / 128; p.tone = r.tone;
            p.structure = r.mask ? 1 : r.structure;
            p.skin = skin; p.other = background;
            std::memcpy(blob.data() + sizeof(PushFSwin), &p, sizeof p);
        } else if (kern.rfind("imgin", 0) == 0) {
            PushImgIn p{}; std::memcpy(&p, blob.data(), sizeof p);
            p.s434 = float(r.style) / 128; p.aux0 = r.tone;
            p.aux1 = r.mask ? 1 : r.structure;
            p.gate0 = skin; p.gate1 = background;
            std::memcpy(blob.data(), &p, sizeof p);
        } else if (kern == "fswinimagepost32") {
            constexpr size_t offset = sizeof(PushFSwin) + sizeof(PushUps);
            PushImageTail p{}; std::memcpy(&p, blob.data() + offset, sizeof p);
            // Native compose (the OptiScaler and ReShade routes): the fused
            // tail's own blend IS the DLL's post-block Intensity. **Not clamped
            // to 1**: that clamp came from one post variant's `sat(Intensity)`
            // in the PTX, which governs that variant's lerp and not the
            // control - the DLL copies Intensity out of its settings
            // unsaturated (18001cf07) and the Toolkit documents 0..2. The tail
            // takes 0..1 as the lerp and past it as a luminance-ratio power;
            // see windows/shaders/rdna4/fswin_t.comp.
            // Every route, every pass: each pass carries its own Intensity
            // into this blend, as each OptiScaler DLSS-NR pass is its own DLL
            // feature with its own Intensity.
            p.intensity = std::clamp(r.intensity, 0.0f, 2.0f);
            // Bits 28-29: the Style. For Natural and Cinematic the DLL grades the composed answer after the post
            // block (cg2r_post_process_kernel); the *_grade post pipelines do the same
            // (windows/shaders/rdna4/include/nr_style_grade.glsl). Non-zero bits select that pipeline at dispatch;
            // they stay 0 without it. Controls are checked to 0..2 before they get here; the DLL clamps to its
            // style count, 3.
            p.w_off = (p.w_off & 0x0FFFFFFFu) |
                      (post_alpha ? 0x80000000u | (round_u8 ? 0x40000000u : 0u) : 0u) |
                      (style_grade && style_grade_on ? uint32_t(std::clamp(r.style, 0, 2)) << 28 : 0u);
            std::memcpy(blob.data() + offset, &p, sizeof p);
        } else if (kern.rfind("imgout", 0) == 0) {
            PushImgOut p{}; std::memcpy(&p, blob.data(), sizeof p);
            p.nr_intensity = std::clamp(r.intensity, 0.0f, 2.0f);
            std::memcpy(blob.data(), &p, sizeof p);
        }
    }

    void controls(const Controls& c) {
        const uint32_t npass = std::max<uint32_t>(max_passes, 1u);
        pass_push.assign(npass, {});
        for (auto& d : session.disp) patch_push(d.push, d.kern, resolve_pass(c, 0));
        for (uint32_t p = 1; p < npass; ++p) {
            const Resolved r = resolve_pass(c, p);
            pass_push[p].assign(session.disp.size(), {});
            for (size_t i = 0; i < session.disp.size(); ++i) {
                if (!carries_controls(session.disp[i].kern)) continue;
                pass_push[p][i] = session.disp[i].push;
                patch_push(pass_push[p][i], session.disp[i].kern, r);
            }
        }
    }
};

// What RuntimeConfig::share carries: a full build's lowered network (GraphCore in nr_graph.cpp) and what the build
// that takes it must match - the same arguments but for the source extent, the same model and working extent, and a
// source no taller than the one the network was built for.
struct RuntimeCore {
    std::shared_ptr<const GraphCore> graph;
    std::shared_ptr<GraphArenas> slot;   // the network memory it runs in (null: the graph's own build's)
    std::vector<std::string> arguments;
    NetworkModel network{};
    uint32_t work_w{}, work_h{}, source_w{}, source_h{};
};

Runtime::Runtime(const HostDevice& host, const RuntimeConfig& config) : Runtime(host,config,ControlMaskConfig{}) {}
Runtime::Runtime(const HostDevice& host, const RuntimeConfig& config, const ControlMaskConfig& mask_config)
    : Runtime(host,config,mask_config,TemporalConfig{}) {}
Runtime::Runtime(const HostDevice& host, const RuntimeConfig& config, const ControlMaskConfig& mask_config,
                 const TemporalConfig& temporal_config)
    : impl_(std::make_unique<Impl>()) {
    runtime_dispatch::Scope physical_scope(host.instance, host.physical_dispatch);
    if (!host.instance || !host.physical || !host.device || !host.queue || !config.width || !config.height)
        throw std::invalid_argument("NR requires complete host device and source extent");
    if (config.accumulation != "fp32" && config.accumulation != "round-fp16")
        throw std::invalid_argument("unsupported accumulation policy");
    const Transfer transfer = transfer_mode(config.colour_format);
    const VkFormat unorm = encoded_format(config.colour_format);
    impl_->transfer = transfer;
    impl_->round_u8 = config.colour_format == VK_FORMAT_R8G8B8A8_UNORM || config.colour_format == VK_FORMAT_R8G8B8A8_SRGB ||
                      config.colour_format == VK_FORMAT_B8G8R8A8_UNORM || config.colour_format == VK_FORMAT_B8G8R8A8_SRGB;
    impl_->colour_format = config.colour_format;
    const auto root = std::filesystem::canonical(config.root);
    // An installed copy keeps everything it loads in dlssnr-amd/ beside the DLL:
    // the model pack and shaders/ (network, runtime/ passes, temporal/). A
    // development tree has build/ and artifacts/ at its root instead.
    const auto data = std::filesystem::is_directory(root / "dlssnr-amd") ? root / "dlssnr-amd" : root;
    const bool installed = data != root;

    int initial_rdna3 = config.rdna3 >= 0 ? config.rdna3 : host.rdna3;
    if (initial_rdna3 < 0) {
        VkPhysicalDeviceProperties hp{};
        vkGetPhysicalDeviceProperties(host.physical, &hp);
        VkPhysicalDeviceProperties real_hp = hp;
        VkPhysicalDeviceDriverProperties real_drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        if (nr::driver::amd_reported(host.physical, &real_hp, &real_drv)) {
            hp.vendorID = real_hp.vendorID;
            hp.deviceID = real_hp.deviceID;
            std::memcpy(hp.deviceName, real_hp.deviceName, sizeof(hp.deviceName));
        }
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(host.physical, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> have(ne);
        vkEnumerateDeviceExtensionProperties(host.physical, nullptr, &ne, have.data());
        bool have_fp8 = false;
        for (const auto& h : have)
            if (std::strcmp(h.extensionName, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME) == 0) have_fp8 = true;
        initial_rdna3 = nrvk::is_rdna3_device(hp.vendorID, hp.deviceID, have_fp8) ? 1 : 0;
    }
    impl_->session.ctx.rdna3 = (initial_rdna3 == 1);

    // Only binary import needs device enablement. Compile-only keeps the Linux graph directory.
    const bool compile_only = !nr::binary::import_enabled();
    if (!compile_only && !nr::binary::directory().empty() && !nr::binary::enabled(host.device))
        nr::binary::disable("the device was created without pipeline binaries");
    const std::string aco = nr::binary::directory();
    const auto native_shaders = installed
        ? (std::filesystem::is_directory(data / "shaders" / impl_->session.ctx.target_arch_dir())
               ? data / "shaders" / impl_->session.ctx.target_arch_dir()
               : data / "shaders")
        : root / "build";
    const auto network_shaders = nr::detail::network_shader_directory(native_shaders, aco);
    // Shaders are read from inside the NR root, or from the ACO bundle when one is in use.
    const auto inside = [&](const std::filesystem::path& p) {
        for (const auto& base : {root, aco.empty() ? root : std::filesystem::canonical(aco)}) {
            const auto rel = p.lexically_relative(base);
            if (!rel.empty() && *rel.begin() != "..") return true;
        }
        return false;
    };
    if (compile_only && aco.empty())
        throw std::runtime_error("Linux SPIR-V driver compile requires a configured bundle; Windows network shaders are not a fallback");
    auto selected_network_shaders = network_shaders;
    if (config.accumulation == "round-fp16") selected_network_shaders /= "round-fp16";
    const auto temporal_shaders = temporal_config.shaders.empty()
        ? (!aco.empty() ? network_shaders / "temporal"
           : installed ? (std::filesystem::is_directory(data / "shaders" / impl_->session.ctx.target_arch_dir() / "temporal")
                              ? data / "shaders" / impl_->session.ctx.target_arch_dir() / "temporal"
                              : data / "shaders/temporal")
                       : root / "build/windows" / impl_->session.ctx.target_arch_dir() / "network/temporal")
        : std::filesystem::path(temporal_config.shaders);
    if (compile_only) {
        nr::detail::require_linux_shader_bundle(selected_network_shaders, config.accumulation, host_shader_constants());
        if (config.require_style_grade)
            nr::detail::require_shader_assets(selected_network_shaders, {"g_fswinimagepost32_grade.spv"});
        if (mask_config.width) {
            const auto mask_shaders = network_shaders / "runtime";
            std::ifstream mask_manifest(mask_shaders / "shader-constants.txt");
            std::ifstream network_manifest(selected_network_shaders / "shader-constants.txt");
            if (!mask_manifest || nr::detail::read_shader_manifest(mask_manifest) != nr::detail::read_shader_manifest(network_manifest))
                throw std::runtime_error("Linux SPIR-V control-mask shader constants differ from the network");
            nr::detail::require_shader_assets(mask_shaders,
                {config.accumulation == "fp32" ? "mask_pre_fp32.spv" : "mask_pre_round.spv"});
        }
        if (temporal_config.enable) {
            // A caller override must still resolve to this Linux bundle, not native Windows image variants.
            if (std::filesystem::canonical(temporal_shaders) != std::filesystem::canonical(network_shaders / "temporal"))
                throw std::runtime_error("Linux SPIR-V temporal shaders must come from the configured bundle");
            std::ifstream manifest(temporal_shaders / "shader-constants.txt");
            std::ifstream network_manifest(selected_network_shaders / "shader-constants.txt");
            if (!manifest || nr::detail::read_shader_manifest(manifest) != nr::detail::read_shader_manifest(network_manifest))
                throw std::runtime_error("Linux SPIR-V temporal shader constants differ from the network");
            std::vector<std::string> required = {"temporal_pre_fp32.spv", "temporal_post_fp32.spv",
                                                  "motion_luma.spv", "motion_estimate.spv"};
            if (config.require_style_grade) required.push_back("temporal_post_fp32_grade.spv");
            nr::detail::require_shader_assets(temporal_shaders, required);
            if (std::filesystem::exists(temporal_shaders / "temporal_pre_fp32nh.spv"))
                nr::detail::require_shader_assets(temporal_shaders, {"temporal_pre_fp32nh.spv"});
            const bool hp_requested = std::getenv("NR_HPASS") && std::atoi(std::getenv("NR_HPASS"));
            const bool h16_requested = std::getenv("NR_HIST16") && std::atoi(std::getenv("NR_HIST16")) && config.max_passes == 1;
            for (const char* name : {"temporal_pre_fp32hp.spv", "temporal_pre_fp32nhhp.spv",
                                     "temporal_post_fp32hp.spv", "temporal_post_fp32h16.spv"}) {
                const bool requested = std::string(name).find("h16") != std::string::npos ? h16_requested : hp_requested;
                if (!requested || !std::filesystem::exists(temporal_shaders / name)) continue;
                nr::detail::require_shader_assets(temporal_shaders, {name});
                if (config.require_style_grade && std::string(name).rfind("temporal_post_", 0) == 0) {
                    auto grade = std::string(name);
                    grade.insert(grade.size() - 4, "_grade");
                    nr::detail::require_shader_assets(temporal_shaders, {grade});
                }
            }
        }
    }
    if (!std::isfinite(config.model_scale) || config.model_scale <= 0.f || config.model_scale > 1.f)
        throw std::invalid_argument("model_scale must be in (0, 1]");
    if (config.max_passes < 1 || config.max_passes > 30)
        throw std::invalid_argument("max_passes must be 1..30");
    // The model extent. At scale 1 it is the frame's own, unaligned, as every
    // game has run so far; below 1 it is rounded to a multiple of 8 (the
    // network's tile) and never above the frame.
    const uint32_t mw = model_extent(config.width, config.model_scale), mh = model_extent(config.height, config.model_scale);
    impl_->mw = mw; impl_->mh = mh; impl_->scaled = mw != config.width || mh != config.height;
    // The model: the caller's choice over DLSSNR_MODEL over dlssnr-amd.ini [Network] Model; none of them: the
    // small model. Its weights (dlssnr-small.bin, made by install.ps1 from the 310.9 DLL) are only in an installed
    // tree; missing: the original model, and the log says why.
    NetworkModel network = NetworkModel::Small;
    {
        std::string env;
#ifdef _WIN32
        { char b[16] = {}; const unsigned long n = GetEnvironmentVariableA("DLSSNR_MODEL", b, sizeof b); if (n && n < sizeof b) env = b; }
#endif
        if (env.empty()) if (const char* e = std::getenv("DLSSNR_MODEL")) env = e;
        const int ev = env.empty() ? -1 : nr::network_value(env);
        if (!env.empty() && ev < 0) nr::logf("[nr] DLSSNR_MODEL=%s is not small or original: ignored", env.c_str());
        const int iv = config.network >= 0 || ev >= 0 ? -1 : nr::ini_network((data.parent_path() / "dlssnr-amd.ini").string());
        const char* from = config.network >= 0 ? "caller" : ev >= 0 ? "DLSSNR_MODEL" : iv >= 0 ? "dlssnr-amd.ini" : "default";
        const int v = config.network >= 0 ? config.network : ev >= 0 ? ev : iv;
        if (v == 0) network = NetworkModel::Original;
        std::error_code ec;
        const char* missing = network != NetworkModel::Small ? nullptr
            : !installed ? "not an installed tree"
            : !std::filesystem::is_regular_file(data / "dlssnr-small.bin", ec) ? "dlssnr-amd/dlssnr-small.bin is missing (run Setup with nvngx_dlssnr.dll 310.9.16)"
            : nullptr;
        if (missing) {
            nr::logf("[nr] small model requested (%s) but %s: unavailable", from, missing);
            throw std::runtime_error(std::string("small model unavailable (") + from + "): " + missing);
        } else {
            nr::logf("[nr] %s model (%s)", network == NetworkModel::Small ? "small" : "original", from);
        }
    }
    impl_->network = network;
    impl_->post_blend_scale = network == NetworkModel::Small ? kPostBlendScaleSmall : kPostBlendScale;
    // [Network] Motion Without Depth = 1, the default: the pre block reads each pixel's own motion vector, as NVIDIA's
    // DLLs (310.8, 310.9) do - their host never binds the depth, so the kernel's nearest-of-five depth pick never runs
    // there (measured on an RTX 5090); it is also the closer and steadier one at moving edges (edge band vs NVIDIA,
    // panning disc with jittered vectors: 43.6 dB, against 33.6 with the pick). 0: the depth picks where the vector
    // is read when there is one.
    if (config.motion_without_depth < -1 || config.motion_without_depth > 1)
        throw std::invalid_argument("motion_without_depth must be -1, 0 or 1");
    if (config.motion_without_depth >= 0) {
        impl_->motion_without_depth = config.motion_without_depth != 0;
    } else {
        std::string v = nr::ini_network_key((data.parent_path() / "dlssnr-amd.ini").string(), "motion without depth");
        for (auto& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        impl_->motion_without_depth = !(v == "0" || v == "false" || v == "off" || v == "no");
        if (!impl_->motion_without_depth)
            nr::logf("[nr] motion vectors read where the depth picks (dlssnr-amd.ini [Network] Motion Without Depth = 0)");
    }
    impl_->native_compose = config.native_compose;
    if (config.native_compose && impl_->scaled)
        throw std::runtime_error("native_compose needs model_scale 1: the DLL has no transfer pass to resample through");
    impl_->max_passes = config.max_passes;
    const auto native = make_native_plan(mw, mh, network);
    std::string plan_name = "compiled native descriptor";
    if (!config.plan.empty()) {
        const auto path = std::filesystem::canonical(config.plan);
        const auto rel = path.lexically_relative(root);
        if (rel.empty() || *rel.begin() == "..") throw std::invalid_argument("plan must be inside NR root");
        auto tokens = [](std::istream& input) {
            std::vector<std::string> result; std::string line, word;
            while (std::getline(input, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream words(line); while (words >> word) result.push_back(word);
            }
            return result;
        };
        std::ifstream provided(path); std::istringstream expected(native.text);
        if (!provided || tokens(provided) != tokens(expected))
            throw std::invalid_argument("explicit plan does not match native source-sized descriptor plan");
        plan_name = path.string();
    }
    std::istringstream plan_stream(native.text);
    const auto prepared_plan = parse_plan(plan_stream);
    nr::PhaseTimer timer;
    // One build at a time (the graph's layout globals); a runtime built inside a recording from a lowered network
    // (RuntimeConfig::deferred) touches none of them and must not wait for a background build.
    std::unique_lock<std::mutex> lock(build_mutex, std::defer_lock);
    if (!config.deferred) lock.lock();
    auto& s = impl_->session;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(host.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(host.physical, &count, props.data());
    if (host.queue_family >= count || !(props[host.queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT))
        throw std::invalid_argument("NR host queue must support compute");
    // A compute-only host family is refused later, once the direct paths are
    // known, and only when a frame would still need a format-converting blit.
    impl_->graphics_family = (props[host.queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    std::string why;
    if (!s.ctx.adopt(host.instance, host.physical, host.device, host.queue, host.queue_family, &why))
        throw std::runtime_error("NR device unsupported: " + why);
    s.ctx.queue_lock = host.queue_lock; s.ctx.queue_unlock = host.queue_unlock;
    if (host.secondary_family != VK_QUEUE_FAMILY_IGNORED &&
        (host.secondary_family >= count || !(props[host.secondary_family].queueFlags & VK_QUEUE_COMPUTE_BIT)))
        throw std::invalid_argument("NR secondary queue must support compute");
    s.ctx.secondary_family = host.secondary_family;
    s.ctx.buffer_device_address = host.buffer_device_address;
    s.ctx.defer_transitions = config.deferred;
    if (config.deferred && !config.share_pipelines)
        throw std::invalid_argument("deferred construction requires shared pipelines");
    s.ctx.registry_only = config.deferred;
    s.ctx.share_pipelines = config.share_pipelines;
    // Built inside a recording in another runtime's network memory: the images from that memory's heap, in the region
    // the runtime built there before this one did not use (nrvk::ImageHeap). Made, or made again bigger, here.
    if (config.deferred && config.share && config.share->slot) {
        // Never allocated here (a big allocation is a long stall on this thread): the heap is made in the background
        // (Runtime::image_heap_wanted, allocate_image_heap); until it is there, or when it is too small for this size,
        // the images take memory of their own.
        GraphArenas& sl = *config.share->slot;
        const VkDeviceSize want = VkDeviceSize(sl.bytes_per_px * double(config.width) * double(config.height));
        if (sl.images && sl.images->region_cap < want)
            sl.px_unfit = std::max(sl.px_unfit, double(config.width) * double(config.height));
        if (sl.images && sl.bytes_per_px > 0 && sl.images->region_cap >= want) {
            const uint32_t r = sl.next_region;
            sl.next_region ^= 1u;
            sl.images->gen[r] += 1; sl.images->cursor[r] = 0; sl.images->need[r] = 0;
            impl_->heap = sl.images; impl_->heap_region = r; impl_->heap_gen = sl.images->gen[r];
            s.ctx.heap = sl.images.get(); s.ctx.heap_region = r;
        }
    }
    s.ctx.require_matrix_config();
#if NR_INT4
    // int4_mixed ([Int4Mixed] in dlssnr-amd.ini; DLSSNR_INT4 wins over the ini, the caller's choice - the in-game
    // switch - over both). Installed, it needs dlssnr-amd/int4/ (shaders, quantisers, settings.txt), the weights the
    // installer made (dlssnr-amd/dlssnr-int4.bin) and ACO Mode enabled on a device supporting VK_KHR_pipeline_binary;
    // anything missing: the default network, and the log says why.
    bool int4 = false;
    std::string int4_env;
#ifdef _WIN32
    { char b[16] = {}; const unsigned long n = GetEnvironmentVariableA("DLSSNR_INT4", b, sizeof b); if (n && n < sizeof b) int4_env = b; }
#endif
    if (int4_env.empty()) if (const char* e = std::getenv("DLSSNR_INT4")) int4_env = e;
    if (config.int4_mixed >= 0) int4_env = config.int4_mixed ? "1" : "0";
    const char* int4_from = config.int4_mixed >= 0 ? "switch" : int4_env.empty() ? "dlssnr-amd.ini" : "DLSSNR_INT4";
    if (int4_env.empty()) {
        // no Enabled: on (nr_ini_int4.hpp) - but only an installed tree has int4 data; a development tree stays off
        const int iv = nr::ini_int4((data.parent_path() / "dlssnr-amd.ini").string()).enabled;
        int4_env = iv == 0 ? "0" : iv == 1 || installed ? "1" : "";
    }
    // What the build reads for int4 (nr_build_cfg.hpp): installed, only this map - empty for the default network, so
    // nothing in the environment reaches it; a development tree keeps reading the environment.
    std::map<std::string, std::string> int4_cfg;
    const std::filesystem::path i4root = data / "int4";
    if (int4_env == "1") {
        std::error_code ec;
        const char* missing = compile_only ? "INT4 is unavailable with Linux SPIR-V driver compilation"
            : network == NetworkModel::Small ? "the small model has no int4 network"
            : !installed ? "not an installed tree"
            : !std::filesystem::is_directory(i4root / "shaders" / "temporal", ec) || !std::filesystem::is_directory(i4root / "vit", ec) ||
                      !std::filesystem::is_regular_file(i4root / "settings.txt", ec) ? "dlssnr-amd/int4/ is incomplete"
            : !std::filesystem::is_regular_file(data / "dlssnr-int4.bin", ec) ? "dlssnr-amd/dlssnr-int4.bin is missing (run install.ps1 again)"
            : nr::binary::directory().empty() || !nr::binary::enabled(host.device) ? "ACO Mode is off or pipeline binaries are unavailable"
            : nullptr;
        if (missing) {
            nr::logf("[nr] int4_mixed requested (%s) but %s: default network", int4_from, missing);
            impl_->network_note = missing;
        } else {
            if (!std::filesystem::is_directory(std::filesystem::path(aco) / "int4/records", ec))
                throw std::runtime_error("the ACO INT4 records are missing");
            int4 = true;
            int4_cfg["NR_I4_DIR"] = (i4root / "vit").string();
            if (std::filesystem::is_directory(i4root / "swin", ec)) int4_cfg["NR_SWI4_DIR"] = (i4root / "swin").string();
            int4_cfg["NR_I4_WEIGHTS"] = (data / "dlssnr-int4.bin").string();
            // the rewritten pipeline binaries (nrvk.hpp create_iu4), keyed by driver like pipeline.cache
            std::filesystem::create_directories(i4root / "pipeline-binaries", ec);
            if (!ec) int4_cfg["NR_I4_CACHE"] = (i4root / "pipeline-binaries").string();
            const auto t = slurp((i4root / "settings.txt").string());
            nr::int4_settings(std::string(t.begin(), t.end()), int4_cfg);
            nr::logf("[nr] int4_mixed network (%s)", int4_from);
        }
    }
    impl_->int4_mixed = int4;
#endif
    // Where the pass's own cost is measured. Two queries per ring slot. A queue
    // family is allowed to report no timestamp bits - then there is no pool, and
    // every accessor answers zero rather than a number made up from a clock.
    if (props[host.queue_family].timestampValidBits && s.ctx.timestamp_period > 0.0f) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        impl_->split_timing = true;
        impl_->stage_base = 2 + 2 * config.max_passes;
        impl_->timing_stride = impl_->stage_base + Impl::kStageQueries;
        qi.queryCount = Impl::kTimingSlots * impl_->timing_stride;
        if (vkCreateQueryPool(s.ctx.device, &qi, nullptr, &impl_->timing) == VK_SUCCESS) {
            impl_->timing_ok = true;
            impl_->timestamp_ns = s.ctx.timestamp_period;
            const uint32_t bits = props[host.queue_family].timestampValidBits;
            impl_->timestamp_mask = bits < 64 ? (uint64_t(1) << bits) - 1 : ~uint64_t(0);
        }
    }
    // Values share arena memory by lifetime (nr_graph.cpp, `reuse`): same
    // arithmetic, same bytes out, a fraction of the memory. NR_ARENA_REUSE=0
    // gives every value its own bytes again, for an A/B.
    auto arena_reuse = [] {
        const char* e = std::getenv("NR_ARENA_REUSE");
        return e && *e ? std::atoi(e) != 0 : kArenaReuseDefault;
    };
    std::vector<std::string> arguments = {"nr_runtime", "--plan", plan_name,
        "--unpacked", (installed ? data / "model" : root / "artifacts").string(),
        "--spv-dir", network_shaders.string(),
        "--host-boundary", arena_reuse() ? "--reuse" : "--no-reuse", "--source-width", std::to_string(mw),
        "--source-height", std::to_string(mh), "--accumulation", config.accumulation,
        "--pipeline-cache", (installed ? data / "pipeline.cache" : root / "dlssnr-amd.pipelinecache").string()};
#if NR_INT4
    if (int4)   // the int4 network: its own shaders and its own pipeline cache
        for (size_t i = 0; i + 1 < arguments.size(); ++i) {
            if (arguments[i] == "--spv-dir") arguments[i + 1] = (i4root / "shaders").string();
            if (arguments[i] == "--pipeline-cache") arguments[i + 1] = (i4root / "pipeline.cache").string();
        }
#endif
    // The weights as one file (windows/build/pack_model.py); the names inside are
    // the relative paths the graph asks for under --unpacked.
    if (installed) {
        const auto pack = data / (network == NetworkModel::Small ? "dlssnr-small.bin" : "dlssnr.bin");
        if (!std::filesystem::is_regular_file(pack))
            throw std::runtime_error("missing " + pack.string() + " (run install.ps1 again)");
        arguments.push_back("--model-pack"); arguments.push_back(pack.string());
        arguments.push_back("--model-family");
        arguments.push_back(network == NetworkModel::Small ? "small" : "original");
    }
    // One pass at the frame's own extent, not linear: every kernel only samples
    // the input image, so it can be the frame's own format filled by a copy.
    // Sampling converts exactly as the blit into RGBA32F did (the blit is a
    // sampling shader too) - the same values for a quarter of the bytes, and
    // no conversion pass. Anything else keeps RGBA32F: later passes copy their
    // answer into it, and the linear path writes it as storage.
    {
        const VkFormat in_format = transfer == Transfer::Encoded ? unorm : config.colour_format;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(host.physical, in_format, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        impl_->direct_in = NR_DIRECT_IN && transfer != Transfer::Identical && !impl_->scaled &&
                           !config.linear_input && !config.preprocess && config.max_passes == 1 &&
                           mask_config.width == 0 &&
                           (fp.optimalTilingFeatures & need) == need;
        if (impl_->direct_in) {
            arguments.push_back("--tex-in-format");
            arguments.push_back(std::to_string(unsigned(in_format)));
        }
        // The post block's own store: native compose folds the alpha pass into
        // it, so nothing reads the answer image but the write-back, and the
        // store's conversion is the blit's for these formats (byte-compared at
        // 1080p, 6 temporal frames: 8-bit UNORM/SRGB - the post block rounds
        // those to k/255 itself - FP16, 16-bit UNORM, 11/11/10 float). Not the
        // 10-bit UNORM ones: there the blit comes out one step lower on ~18%
        // of channels. Formatless storage writes: the post SPIR-V declares no
        // format (NR_OUT0_NOFORMAT), which Vulkan 1.3 allows for a format with
        // STORAGE_WRITE_WITHOUT_FORMAT.
        const bool store_matches_blit = impl_->round_u8 || config.colour_format == VK_FORMAT_R16G16B16A16_SFLOAT ||
            config.colour_format == VK_FORMAT_R16G16B16A16_UNORM || config.colour_format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        VkFormatProperties3 f3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 f2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &f3};
        vkGetPhysicalDeviceFormatProperties2(host.physical, unorm, &f2);
        impl_->direct_out = NR_DIRECT_OUT && NR_POST_ALPHA && impl_->direct_in && config.native_compose && store_matches_blit &&
                            mask_config.width == 0 &&
                            (f3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT) &&
                            (f3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT);
        nr::logf("[nr] input %s, write-back %s", impl_->direct_in ? "copied in the frame's format"
                 : transfer == Transfer::Identical ? "copied (RGBA32F)" : "converted",
                 impl_->direct_out ? "copied from the post block's own store" : "converted");
        if (impl_->direct_out) {
            arguments.push_back("--out0-format");
            arguments.push_back(std::to_string(unsigned(unorm)));
        }
    }
    // The resize path (RuntimeConfig::share): the network of another source extent with this one's working extent,
    // built with these same arguments but for the source extent, and this model.
    {
        // The colour format only sizes the input and answer images (the kernels sample one and store into the other
        // without a format), so a network serves frames of another format too: a game that switches its colour
        // buffer between R11G11B10 and RGBA16F at camera cuts (FF7 Rebirth) gets a runtime from the same network.
        std::vector<std::string> same = arguments;
        for (size_t i = 0; i + 1 < same.size(); ++i)
            if (same[i] == "--source-width" || same[i] == "--source-height" || same[i] == "--tex-in-format" ||
                same[i] == "--out0-format")
                same[i + 1].clear();
        if (config.share) {
            const RuntimeCore& c = *config.share;
            if (c.arguments == same && c.network == network && c.work_w == native.width && c.work_h == native.height &&
                mh <= c.source_h) {
                s.share = c.graph;
                s.share_slot = c.slot;
            }
            else if (config.deferred)   // inside a recording: no whole build here (the caller builds in the background)
                throw std::runtime_error("the network cannot be shared at this extent or format");
            else
                nr::logf("[nr] the %ux%u network cannot be shared at %ux%u; building one", c.source_w, c.source_h, mw, mh);
        }
        impl_->share_arguments = std::move(same);
    }
    impl_->work_w = native.width; impl_->work_h = native.height;
    impl_->model_scale = config.model_scale;
    std::vector<char*> argv;
    for (auto& a : arguments) argv.push_back(a.data());
    timer.mark("device");
#if NR_INT4
    const nr::BuildCfgScope cfg_scope(installed ? &int4_cfg : nullptr);   // what this build reads (nr_build_cfg.hpp)
#endif
    s.plan_only = config.plan_only;
    s.capacity = config.capacity;
    s.given_act = config.act_arena ? config.act_arena->act : nullptr;
    s.act_slack = config.act_slack;
    const int built = s.build(int(argv.size()), argv.data(), &prepared_plan);
    if (built != (config.plan_only ? 4 : 0) || !s.missing.empty())
        throw std::runtime_error("NR graph initialization failed");
    timer.mark("graph");
    if (config.plan_only) {   // the lowering only (Runtime::plan); nothing else is made
        impl_->width = config.width; impl_->height = config.height;
        nr::logf("[nr] plan for %ux%u (working extent %ux%u) in %.2fs", mw, mh, native.width, native.height, timer.total());
        return;
    }
    impl_->width = config.width; impl_->height = config.height;
    // The runtime passes are the driver's compile in either case (the ACO bundle's are the same SPIR-V).
    const auto adapters = std::filesystem::canonical(config.adapter_shaders.empty()
        ? native_shaders / "runtime" : std::filesystem::path(config.adapter_shaders));
    if (!inside(adapters))
        throw std::invalid_argument("adapter shaders must be inside NR root");
    if (impl_->scaled) {
        if (mask_config.width) throw std::invalid_argument("the control mask path does not support model_scale below 1");
        // The downscale is a blit from the RGBA32F keep; linear filtering on a
        // 32-bit float format is a feature bit, not a given.
        VkFormatProperties fp{}; vkGetPhysicalDeviceFormatProperties(host.physical, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
        impl_->downscale_filter = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
            ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        // The frame kept in its own format (Impl::keep_native) for the formats whose texelFetch is
        // the blit's own conversion; the store into it where that store is the blit's conversion
        // too (the formats the post block's own store covers, see direct_out below).
        // NR_KEEP_RGBA32F=1: the RGBA32F keep and the blits, as before.
        const VkFormat keep_format = transfer == Transfer::Encoded ? unorm : config.colour_format;
        const VkFormat cf = config.colour_format;
        const bool listed = transfer == Transfer::Encoded || cf == VK_FORMAT_R8G8B8A8_UNORM ||
            cf == VK_FORMAT_B8G8R8A8_UNORM || cf == VK_FORMAT_R16G16B16A16_SFLOAT || cf == VK_FORMAT_R16G16B16A16_UNORM ||
            cf == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || cf == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ||
            cf == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        const bool store_listed = impl_->round_u8 || cf == VK_FORMAT_R16G16B16A16_SFLOAT ||
            cf == VK_FORMAT_R16G16B16A16_UNORM || cf == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        VkFormatProperties3 k3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 k2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &k3};
        vkGetPhysicalDeviceFormatProperties2(host.physical, keep_format, &k2);
        const VkFormatFeatureFlags2 kf = k3.optimalTilingFeatures;
        const char* old_keep = std::getenv("NR_KEEP_RGBA32F");
        impl_->keep_native = !(old_keep && std::atoi(old_keep)) && listed && impl_->downscale_filter == VK_FILTER_LINEAR &&
            s.tex_in.format == VK_FORMAT_R32G32B32A32_SFLOAT && (kf & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) &&
            (kf & VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT) && (kf & VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT);
        impl_->store_native = impl_->keep_native && store_listed && (kf & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) &&
            (kf & VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT);
        if (config.separate_answer) impl_->store_native = false;
        impl_->compact_rgba = config.compact_transfer && !impl_->store_native && !impl_->keep_native &&
            !impl_->round_u8 && !config.native_compose;
        impl_->keep_full = s.ctx.image(config.width, config.height,
                                       impl_->keep_native ? keep_format : VK_FORMAT_R32G32B32A32_SFLOAT, true, true,
                                       !impl_->keep_native || impl_->store_native);
        s.ctx.transition(impl_->keep_full, VK_IMAGE_LAYOUT_GENERAL);
        if (!impl_->store_native && !impl_->compact_rgba) {
            impl_->full_out = s.ctx.image(config.width, config.height, VK_FORMAT_R32G32B32A32_SFLOAT, false);
            s.ctx.transition(impl_->full_out, VK_IMAGE_LAYOUT_GENERAL);
        }
        impl_->up_result = s.ctx.image(config.width, config.height, VK_FORMAT_R32G32B32A32_SFLOAT, false);
        s.ctx.transition(impl_->up_result, VK_IMAGE_LAYOUT_GENERAL);
        if (impl_->keep_native) {
            // Where the blit samples: a frame-sized probe holding each texel's column and row,
            // blitted down as the frame would be, read back into `taps` on the GPU. Frame and
            // model sized RGBA32F for the length of one submission.
            const uint32_t W = config.width, H = config.height;
            impl_->taps = s.ctx.buffer(VkDeviceSize(mw + mh) * 8);
            auto probe = s.ctx.image(W, H, VK_FORMAT_R32G32B32A32_SFLOAT, false, false);
            auto probed = s.ctx.image(mw, mh, VK_FORMAT_R32G32B32A32_SFLOAT, false, false);
            probe.layout = probed.layout = VK_IMAGE_LAYOUT_GENERAL;
            nrvk::Kernel taps_k;
            taps_k.create(s.ctx, (adapters / "runtime_taps.spv").string(), {impl_->taps.handle}, 12, {&probe, &probed});
            s.ctx.one_shot([&](VkCommandBuffer cmd) {
                barrier(cmd, probe.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
                const uint32_t fill[] = {W, H, 0u};
                dispatch(cmd, taps_k, (W + 63) / 64, H, 1, fill, sizeof fill);
                barrier(cmd, probe.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                barrier(cmd, probed.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkImageBlit down{};
                down.srcSubresource = down.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                down.srcOffsets[1] = {int32_t(W), int32_t(H), 1};
                down.dstOffsets[1] = {int32_t(mw), int32_t(mh), 1};
                vkCmdBlitImage(cmd, probe.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               probed.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &down, impl_->downscale_filter);
                barrier(cmd, probed.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                const uint32_t read[] = {mw, mh, 1u};
                dispatch(cmd, taps_k, (mw + mh + 63) / 64, 1, 1, read, sizeof read);
                compute_barrier(cmd);
            });
            taps_k.destroy();
            s.ctx.destroy(probe); s.ctx.destroy(probed);
            nrvk::Context::Image tex_in_storage = s.tex_in;
            tex_in_storage.sampler = VK_NULL_HANDLE;
            tex_in_storage.layout = VK_IMAGE_LAYOUT_GENERAL;
            impl_->downscale.create(s.ctx, (adapters / "runtime_downscale.spv").string(), {impl_->taps.handle}, 8,
                                    {&impl_->keep_full, &tex_in_storage});
        }
        nr::logf("[nr] Model resolution %ux%u: frame kept %s, answer %s", mw, mh,
                 impl_->keep_native ? "in its own format" : "as RGBA32F",
                 impl_->store_native ? "stored in its own format"
                 : impl_->compact_rgba ? "reused RGBA32F keep" : "RGBA32F, converted on the way back");
    }
    // At the frame's extent, one pass, the input copied in the frame's own format (direct_in), no
    // native compose: the transfer pass stores the answer in that format too, into keep_full, with the
    // alpha pass's rounding (store_native, as below 100%), so there is no alpha pass and the write-back
    // is a copy. The formats whose store is the write-back blit's own conversion (direct_out's).
    // NR_UNSCALED_STORE=0: the RGBA32F answer, the alpha pass and the blit, as before.
    if (!config.separate_answer && !impl_->scaled && impl_->direct_in && !config.native_compose && config.max_passes == 1 &&
        !config.linear_input && !config.preprocess && !mask_config.width) {
        const char* env = std::getenv("NR_UNSCALED_STORE");
        const VkFormat cf = config.colour_format;
        const VkFormat keep_format = transfer == Transfer::Encoded ? unorm : cf;
        const bool listed = impl_->round_u8 || cf == VK_FORMAT_R16G16B16A16_SFLOAT ||
            cf == VK_FORMAT_R16G16B16A16_UNORM || cf == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        VkFormatProperties3 k3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 k2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &k3};
        vkGetPhysicalDeviceFormatProperties2(host.physical, keep_format, &k2);
        const VkFormatFeatureFlags2 kf = k3.optimalTilingFeatures;
        if (!(env && !std::atoi(env)) && listed && (kf & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) &&
            (kf & VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT) && (kf & VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT)) {
            impl_->store_native = true;
            impl_->keep_full = s.ctx.image(config.width, config.height, keep_format, true, true, true);
            s.ctx.transition(impl_->keep_full, VK_IMAGE_LAYOUT_GENERAL);
            nr::logf("[nr] answer stored in the frame's format by the transfer pass: no alpha pass, write-back copied");
        }
    }
    if (config.max_passes > 1) {
        if (mask_config.width) throw std::invalid_argument("the control mask path does not support more than one pass");
        impl_->shown_keep = s.ctx.image(mw, mh, VK_FORMAT_R32G32B32A32_SFLOAT, true);
        s.ctx.transition(impl_->shown_keep, VK_IMAGE_LAYOUT_GENERAL);
        if (config.cascade_detail_only) {
            impl_->cascade_detail = true;
            impl_->cascade_radius = std::max(4, int(mh / 60));
            for (auto* im : {&impl_->lograt, &impl_->cascade_tmp, &impl_->lowpass}) {
                *im = s.ctx.image(mw, mh, VK_FORMAT_R32_SFLOAT, false);
                s.ctx.transition(*im, VK_IMAGE_LAYOUT_GENERAL);
            }
            nrvk::Context::Image tex_in_storage = s.tex_in;
            tex_in_storage.sampler = VK_NULL_HANDLE;
            tex_in_storage.layout = VK_IMAGE_LAYOUT_GENERAL;
            impl_->cascade_lograt.create(s.ctx, (adapters / "cascade_lograt.spv").string(), {}, 12,
                                         {&s.surf0, &impl_->shown_keep, &impl_->lograt});
            impl_->cascade_blur_h.create(s.ctx, (adapters / "cascade_blur.spv").string(), {}, 16,
                                         {&impl_->lograt, &impl_->cascade_tmp});
            impl_->cascade_blur_v.create(s.ctx, (adapters / "cascade_blur.spv").string(), {}, 16,
                                         {&impl_->cascade_tmp, &impl_->lowpass});
            impl_->cascade_feed.create(s.ctx, (adapters / "cascade_feed.spv").string(), {}, 12,
                                       {&impl_->shown_keep, &impl_->lograt, &impl_->lowpass, &tex_in_storage});
        }
    }
    // The alpha pass restores the frame's own alpha onto the answer: the answer
    // is full_out when scaled, and the frame's alpha lives wherever the frame
    // was kept.
    // store_native: the answer is written into keep_full (a storage alias of it).
    nrvk::Context::Image keep_full_storage = impl_->keep_full;
    keep_full_storage.sampler = VK_NULL_HANDLE;
    nrvk::Context::Image* answer = impl_->store_native || impl_->compact_rgba ? &keep_full_storage
                                  : impl_->scaled ? &impl_->full_out : &s.surf0;
    nrvk::Context::Image* frame_alpha = impl_->scaled ? &impl_->keep_full
                                        : (config.max_passes > 1 ? &impl_->shown_keep : &s.tex_in);
    if (!impl_->store_native && !impl_->compact_rgba)
        impl_->alpha.create(s.ctx, (adapters / "runtime_alpha.spv").string(), {}, 12, {answer, frame_alpha});
    if (impl_->transfer == Transfer::DirectBlit && impl_->round_u8 && !impl_->direct_out &&
        config.colour_format == VK_FORMAT_R8G8B8A8_UNORM && !impl_->store_native && !impl_->compact_rgba &&
        !impl_->scaled) {
        const auto pack = adapters / "runtime_pack_u8.spv";
        if (std::filesystem::exists(pack)) {
            impl_->pack_img = s.ctx.image(config.width, config.height, VK_FORMAT_R32_UINT, false);
            s.ctx.transition(impl_->pack_img, VK_IMAGE_LAYOUT_GENERAL);
            impl_->pack_u8.create(s.ctx, pack.string(), {}, 8, {answer, &impl_->pack_img});
            impl_->pack_built = true;
        } else {
            nr::logf("[nr] runtime_pack_u8.spv missing; compute-family UNORM8 write-back unavailable");
        }
    }
    // Optional compute conversions preserve FP16/HDR values. Private images keep
    // descriptors stable across caller recordings; only copies touch caller images.
    // compute_convert_colour: the same input conversion for the listed formats (texelFetch of the
    // sampled frame copy is the blit's own conversion; an *_SRGB frame is copied into its UNORM
    // twin, as the blit path does, so the network still sees the encoded values). Write-back is
    // direct_out / store_native, or one compute pack per format into a private image, then a copy:
    //   B10G11R11              runtime_pack_r11      (r11f_g11f_b10f storage image)
    //   R8G8B8A8 UNORM/SRGB    runtime_pack_u8       (R32_UINT; the answer already holds k/255)
    //   B8G8R8A8 UNORM/SRGB    runtime_pack_u8_bgra  (R32_UINT, R/B swapped)
    //   A2B10G10R10 UNORM      runtime_pack_u10      (R32_UINT, round to nearest)
    //   A2R10G10B10 UNORM      runtime_pack_u10_bgra (R32_UINT, R/B swapped)
    const VkFormat cfmt = config.colour_format;
    const bool f16_colour = cfmt == VK_FORMAT_R16G16B16A16_SFLOAT;
    const char* pack_name = nullptr; VkFormat pack_format = VK_FORMAT_R32_UINT;
    switch (cfmt) {
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: pack_name = "runtime_pack_r11.spv"; pack_format = cfmt; break;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: pack_name = "runtime_pack_u8.spv"; break;
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: pack_name = "runtime_pack_u8_bgra.spv"; break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: pack_name = "runtime_pack_u10.spv"; break;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: pack_name = "runtime_pack_u10_bgra.spv"; break;
        default: break;
    }
    const bool listed_colour = config.compute_convert_colour && pack_name != nullptr;
    const VkFormat input_format = transfer == Transfer::Encoded ? unorm : cfmt;
    bool colour_pack = false, input_ok = false;
    if (listed_colour && !impl_->scaled) {
        VkFormatProperties ip{}; vkGetPhysicalDeviceFormatProperties(host.physical, input_format, &ip);
        input_ok = impl_->direct_in || (ip.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        if (!(impl_->direct_out || impl_->store_native)) {
            VkFormatProperties pp{}; vkGetPhysicalDeviceFormatProperties(host.physical, pack_format, &pp);
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
            colour_pack = (pp.optimalTilingFeatures & need) == need && std::filesystem::exists(adapters / pack_name);
        }
    }
    const bool colour_store = impl_->direct_out || impl_->store_native || colour_pack;
    // compute_downscale on a scaled frame: the downscale path below decides and logs (no false refusal here).
    const bool downscale_owns = impl_->scaled && config.compute_downscale;
    if (listed_colour && !downscale_owns && (impl_->scaled || !colour_store || !input_ok)) {
        impl_->convert_refusal = impl_->scaled ? "model scale below 1 needs a downscale blit"
                                : !input_ok ? "the frame format cannot be sampled for the compute input"
                                            : "the write-back has no compute store for this format";
        nr::logf("[nr] colour format %u compute conversion not used: %s", unsigned(cfmt), impl_->convert_refusal);
    }
    if ((host.secondary_family != VK_QUEUE_FAMILY_IGNORED || !impl_->graphics_family) &&
        !impl_->scaled && (f16_colour || (listed_colour && colour_store && input_ok)) &&
        std::filesystem::exists(adapters / "runtime_unpack_f16.spv") &&
        (!f16_colour || std::filesystem::exists(adapters / "runtime_pack_f16.spv"))) {
        if (!impl_->direct_in) {
            // Sampled only outside FP16: storage support is optional and the input never needs it.
            impl_->input_f16 = s.ctx.image(config.width, config.height, f16_colour ? cfmt : input_format, true, true, f16_colour);
            s.ctx.transition(impl_->input_f16, VK_IMAGE_LAYOUT_GENERAL);
            auto input = s.tex_in; input.sampler = VK_NULL_HANDLE; input.layout = VK_IMAGE_LAYOUT_GENERAL;
            auto sampled = impl_->input_f16; sampled.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            impl_->unpack_f16.create(s.ctx, (adapters / "runtime_unpack_f16.spv").string(), {}, 8,
                                    {&sampled, &input});
        }
        if (!impl_->direct_out && (f16_colour || colour_pack)) {
            impl_->output_f16 = s.ctx.image(config.width, config.height, f16_colour ? cfmt : pack_format, false);
            impl_->pack_f16.create(s.ctx, (adapters / (colour_pack ? pack_name : "runtime_pack_f16.spv")).string(), {}, 8,
                                  {answer, &impl_->output_f16});
        }
        impl_->f16_built = true;
        if (listed_colour)
            nr::logf("[nr] colour format %u input converted on compute (runtime_unpack_f16), write-back %s",
                     unsigned(cfmt), impl_->direct_out ? "direct" : colour_pack ? pack_name : "stored in its own format");
    }
    if (config.compute_downscale && impl_->scaled) {
        const char* why = nullptr;
        if (!impl_->keep_native) why = "the frame is not kept in its own format";
        else if (!impl_->store_native && transfer != Transfer::Identical) {
            const char* name = f16_colour ? "runtime_pack_f16.spv" : pack_name;
            const VkFormat fmt = f16_colour ? cfmt : pack_format;
            VkFormatProperties pp{}; vkGetPhysicalDeviceFormatProperties(host.physical, fmt, &pp);
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
            if (!name || (pp.optimalTilingFeatures & need) != need || !std::filesystem::exists(adapters / name))
                why = "the write-back has no compute store for this format";
            else {
                impl_->output_f16 = s.ctx.image(config.width, config.height, fmt, false);
                impl_->pack_f16.create(s.ctx, (adapters / name).string(), {}, 8, {answer, &impl_->output_f16});
                impl_->scaled_pack = true;
            }
        }
        impl_->compute_downscale = !why;
        if (why) nr::logf("[nr] model scale %ux%u compute downscale not used: %s", mw, mh, why);
        else nr::logf("[nr] model scale %ux%u downscaled on compute (runtime_downscale), write-back %s", mw, mh,
                      impl_->store_native ? "stored in its own format" : impl_->scaled_pack ? "packed on compute" : "copied");
    }
    if (!impl_->graphics_family && compute_refusal(false, VK_FORMAT_UNDEFINED, 0, 1))
        throw std::invalid_argument("NR colour format conversion requires a graphics-capable queue");
    impl_->compute_recording = !impl_->graphics_family;
    if (config.linear_input) {
        impl_->linear = true;
        impl_->white_point = config.white_point;
        // Sampled, because the transfer reads it; the encode writes it through
        // a storage alias (same image and view, no sampler, GENERAL).
        impl_->keep = s.ctx.image(mw, mh, VK_FORMAT_R32G32B32A32_SFLOAT, true);
        s.ctx.transition(impl_->keep, VK_IMAGE_LAYOUT_GENERAL);
        nrvk::Context::Image keep_storage = impl_->keep;
        keep_storage.sampler = VK_NULL_HANDLE;
        // tex_in as a storage image, in GENERAL for the one pass that writes it.
        // Same VkImage and view as the sampled descriptor every kernel reads;
        // the alias owns nothing and is never destroyed.
        nrvk::Context::Image tex_in_storage = s.tex_in;
        tex_in_storage.sampler = VK_NULL_HANDLE;
        tex_in_storage.layout = VK_IMAGE_LAYOUT_GENERAL;
        impl_->encode.create(s.ctx, (adapters / "runtime_encode.spv").string(), {}, 16,
                             {&tex_in_storage, &keep_storage});
    }
    // The transfer runs on every frame at the FRAME extent: `result` and
    // `shown` are model-sized (sampled bilinearly when scaled), `keep` is the
    // frame as handed over - the full-resolution copy when scaled, the
    // untouched linear original on the linear path, the frame itself
    // otherwise - and `out` is the full-resolution answer.
    const char* const transfer_spv = impl_->store_native ? "runtime_transfer_store.spv" : "runtime_transfer.spv";
    {
        nrvk::Context::Image* shown = config.max_passes > 1 ? &impl_->shown_keep : &s.tex_in;
        nrvk::Context::Image* keep = impl_->scaled ? &impl_->keep_full
                                     : impl_->linear ? &impl_->keep : shown;
        // Unscaled, the enlargement binding is never read; the model's answer stands in (an
        // RGBA32F image, as they are declared).
        nrvk::Context::Image* up_r = impl_->scaled ? &impl_->up_result : &s.surf0;
        impl_->transfer_pass.create(s.ctx, (adapters / transfer_spv).string(), {}, sizeof(TransferPush),
                               {&s.surf0, shown, keep, answer, up_r});
        impl_->transfer_binds = {s.surf0, *shown, *keep, *answer, *up_r};
        if (impl_->scaled)
            impl_->upscale_pass.create(s.ctx, (adapters / "runtime_upscale.spv").string(), {}, 20,
                                       {&s.surf0, shown, &impl_->up_result});
        if (config.first_pass_output) {
            if (config.native_compose || mask_config.width || config.max_passes < 2)
                throw std::runtime_error("first pass output requires ordinary multi-pass transfer");
            impl_->first_full = s.ctx.image(config.width, config.height, VK_FORMAT_R32G32B32A32_SFLOAT, false);
            s.ctx.transition(impl_->first_full, VK_IMAGE_LAYOUT_GENERAL);
            impl_->first_transfer.create(s.ctx, (adapters / "runtime_transfer.spv").string(), {}, sizeof(TransferPush),
                                         {&s.surf0, shown, keep, &impl_->first_full, up_r});
            if (config.compute_export) {
                const char* name = nullptr; VkFormat fmt = VK_FORMAT_R32_UINT;
                switch (config.colour_format) {
                    case VK_FORMAT_R16G16B16A16_SFLOAT: name = "runtime_pack_f16.spv"; fmt = config.colour_format; break;
                    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: name = "runtime_pack_r11.spv"; fmt = config.colour_format; break;
                    case VK_FORMAT_R8G8B8A8_UNORM: name = "runtime_pack_u8.spv"; break;
                    case VK_FORMAT_B8G8R8A8_UNORM: name = "runtime_pack_u8_bgra.spv"; break;
                    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: name = "runtime_pack_u10.spv"; break;
                    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: name = "runtime_pack_u10_bgra.spv"; break;
                    default: break;
                }
                VkFormatProperties pp{}; vkGetPhysicalDeviceFormatProperties(host.physical, fmt, &pp);
                const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
                if (name && (pp.optimalTilingFeatures & need) == need && std::filesystem::exists(adapters / name)) {
                    impl_->first_packed = s.ctx.image(config.width, config.height, fmt, false);
                    s.ctx.transition(impl_->first_packed, VK_IMAGE_LAYOUT_GENERAL);
                    impl_->first_pack.create(s.ctx, (adapters / name).string(), {}, 8,
                                             {&impl_->first_full, &impl_->first_packed});
                    nr::logf("[nr] first-pass export packed on compute (%s)", name);
                } else {
                    nr::logf("[nr] first-pass export compute pack not used: colour format %u has no compute store",
                             unsigned(config.colour_format));
                }
            }
        }
    }
    if (config.preprocess && !mask_config.width) {
        impl_->prep = true;
        impl_->prep_unknee = config.preprocess_unknee;
        impl_->prep_keep = s.ctx.image(mw, mh, VK_FORMAT_R32G32B32A32_SFLOAT, true);
        s.ctx.transition(impl_->prep_keep, VK_IMAGE_LAYOUT_GENERAL);
        // Host visible: 16 bytes the log reads (Runtime::preprocess_meter).
        impl_->prep_state = s.ctx.buffer(16, true);
        // Storage aliases, as the linear path's: same image and view, no sampler.
        auto storage = [](const nrvk::Context::Image& im) {
            nrvk::Context::Image alias = im; alias.sampler = VK_NULL_HANDLE; alias.layout = VK_IMAGE_LAYOUT_GENERAL;
            return alias;
        };
        nrvk::Context::Image tex_in_st = storage(s.tex_in), keep_st = storage(impl_->prep_keep),
                             surf0_st = storage(s.surf0);
        const auto spv = (adapters / "runtime_prep.spv").string();
        impl_->prep_k.create(s.ctx, spv, {impl_->prep_state.handle}, sizeof(PrepPush), {&tex_in_st, &keep_st, &surf0_st});
        if (config.max_passes > 1) {
            nrvk::Context::Image shown_st = storage(impl_->shown_keep);
            impl_->prep_back_k.create(s.ctx, spv, {impl_->prep_state.handle}, sizeof(PrepPush), {&shown_st, &keep_st, &surf0_st});
        } else if (!config.native_compose) {
            // One pass: tex_in keeps what the network saw and the transfer pass
            // reads the frame from prep_keep instead, as `shown` and, when
            // tex_in was also the frame as handed over, as `keep`.
            nrvk::Context::Image* keep = impl_->scaled ? &impl_->keep_full
                                         : impl_->linear ? &impl_->keep : &impl_->prep_keep;
            nrvk::Context::Image* answer = impl_->store_native || impl_->compact_rgba ? &keep_full_storage
                                           : impl_->scaled ? &impl_->full_out : &s.surf0;
            nrvk::Context::Image* up_r = impl_->scaled ? &impl_->up_result : answer;
            impl_->transfer_prep.create(s.ctx, (adapters / transfer_spv).string(), {},
                                        sizeof(TransferPush), {&s.surf0, &impl_->prep_keep, keep, answer, up_r});
            impl_->transfer_prep_binds = {s.surf0, impl_->prep_keep, *keep, *answer, *up_r};
            if (impl_->scaled)
                impl_->upscale_prep.create(s.ctx, (adapters / "runtime_upscale.spv").string(), {}, 20,
                                           {&s.surf0, &impl_->prep_keep, &impl_->up_result});
        }
    }
    auto require_variant_profile = [&](const std::filesystem::path& directory) {
        std::ifstream manifest(directory / "shader-constants.txt");
        uint32_t profile = 0;
        if (manifest) {
            const auto values = nr::detail::read_shader_manifest(manifest);
            if (values.count("math_profile")) profile = values.at("math_profile");
        } else if (s.math_profile != 0) {
            throw std::runtime_error("fast graph requires a versioned image-variant manifest");
        }
        if (profile != s.math_profile)
            throw std::runtime_error("image variant and graph math profiles differ");
    };
    if (bool(mask_config.width) != bool(mask_config.height))
        throw std::invalid_argument("both control mask dimensions are required");
    const auto mask_shaders = compile_only ? network_shaders / "runtime" : adapters;
    if (mask_config.width) {
        if (config.accumulation == "fp32") require_variant_profile(mask_shaders);
        impl_->mask_image=s.ctx.image(mask_config.width,mask_config.height,VK_FORMAT_R32G32B32A32_SFLOAT,true);
        impl_->mask_rect=s.ctx.buffer(24);
        auto& k=impl_->mask_pre;
        // "fswinimagepreds32nh" is the same kernel without the (never reached) upper
        // exponent clamp; the mask variant keeps the clamp, which is the same function.
        const std::string pre_name = s.kern.count("fswinimagepreds32nh") ? "fswinimagepreds32nh" : "fswinimagepreds32";
        if (s.kern.count(pre_name)) {
            impl_->original_pre=&s.kern.at(pre_name);
            k.create(s.ctx,(mask_shaders/"mask_pre_fp32.spv").string(),
                {s.act.handle,s.act.handle,s.wgt.handle,s.wgt.handle,s.wgt.handle,s.act.handle,impl_->mask_rect.handle},
                sizeof(PushFSwin)+sizeof(PushPreImage),{&s.tex_in,&impl_->mask_image});
        } else if (s.kern.count("imgin16fast")) {
            impl_->original_pre=&s.kern.at("imgin16fast");
            k.create(s.ctx,(mask_shaders/"mask_pre_round.spv").string(),
                {s.act.handle,s.wgt.handle,impl_->mask_rect.handle},sizeof(PushImgIn),{&s.tex_in,&impl_->mask_image});
        } else throw std::runtime_error("control mask pre-input variant unavailable");
        impl_->mask_resolve.create(s.ctx,(adapters/"runtime_control_mask.spv").string(),
            {impl_->mask_rect.handle},12,{&s.surf0,&s.tex_in,&impl_->mask_image});
    }
    {
        const std::string pn = s.kern.count("fswinimagepreds32nh") ? "fswinimagepreds32nh" : "fswinimagepreds32";
        if (s.kern.count(pn) && s.kern.count("fswinimagepost32")) {
            impl_->plain_pre = &s.kern.at(pn); impl_->plain_post = &s.kern.at("fswinimagepost32");
        }
    }
    // The post block with the Style grade, built over the same bindings as the graph's; it is always bound with
    // the set the plain pipeline would use (identically defined layouts), so only the pipeline differs.
    std::filesystem::path grade_spv;
    for (size_t i = 0; i + 1 < arguments.size(); ++i)
        if (arguments[i] == "--spv-dir") grade_spv = std::filesystem::path(arguments[i + 1]) / "g_fswinimagepost32_grade.spv";
    if (config.require_style_grade && !std::filesystem::is_regular_file(grade_spv))
        throw std::runtime_error("Mochizuki style-grade shaders are incomplete; reinstall the full OptScaler NR package");
    if (s.kern.count("fswinimagepost32") && std::filesystem::exists(grade_spv)) {
        impl_->graph_post = &s.kern.at("fswinimagepost32");
        impl_->post_grade.create(s.ctx, grade_spv.string(),
                                 {s.act.handle, s.act.handle, s.wgt.handle, s.wgt.handle, s.wgt.handle},
                                 impl_->graph_post->push_range, {&s.surf0, &s.surf1, &s.tex_in});
        impl_->style_grade = true;
    }
    if (temporal_config.enable) {
        auto& t = impl_->temporal;
        // The temporal pre/post blocks are network kernels: the ACO bundle's own SPIR-V when it is in use.
        auto shaders = std::filesystem::canonical(temporal_shaders);
#if NR_INT4
        // The int4 network's temporal pre/post variants are built with its defines (hard-swish, whose scale
        // NR_HS_FOLDPP folds into the pre/post weights): int4/shaders/temporal, never the default ones.
        if (int4) shaders = std::filesystem::canonical(i4root / "shaders" / "temporal");
#endif
        require_variant_profile(shaders);
        if (!inside(shaders))
            throw std::invalid_argument("temporal shaders must be inside NR root");
        const bool pre_nh = s.kern.count("fswinimagepreds32nh") != 0;
        const std::string pre_name = pre_nh ? "fswinimagepreds32nh" : "fswinimagepreds32";
        if (!s.kern.count(pre_name) || !s.kern.count("fswinimagepost32"))
            throw std::runtime_error("the temporal path requires the fused FP32 image kernels");
        t.original_pre = &s.kern.at(pre_name);
        t.original_post = &s.kern.at("fswinimagepost32");
        t.lw[0] = (mw + kTemporalBase - 1) / kTemporalBase;
        t.lh[0] = (mh + kTemporalBase - 1) / kTemporalBase;
        for (unsigned k = 1; k < kTemporalLevels; ++k) {
            t.lw[k] = (t.lw[k - 1] + 1) / 2;
            t.lh[k] = (t.lh[k - 1] + 1) / 2;
        }
        for (unsigned p = 0; p < 2; ++p)
            for (unsigned k = 0; k < kTemporalLevels; ++k)
                t.luma[p][k] = s.ctx.image(t.lw[k], t.lh[k], VK_FORMAT_R32_SFLOAT, false);
        // level 0 with the sampled usage too: flow0_sampled reads it through a sampler (the temporal pre and post
        // kernels). The image itself stays a storage image to the kernels that write it (no sampler of its own).
        for (unsigned k = 0; k < kTemporalLevels; ++k) {
            t.flow[k] = s.ctx.image(t.lw[k], t.lh[k], VK_FORMAT_R32G32_SFLOAT, k == 0, k != 0);
            if (k == 0) {   // in GENERAL like the others (a sampled image settles in SHADER_READ_ONLY_OPTIMAL)
                vkDestroySampler(s.ctx.device, t.flow[k].sampler, nullptr);
                t.flow[k].sampler = VK_NULL_HANDLE;
                s.ctx.transition(t.flow[k], VK_IMAGE_LAYOUT_GENERAL);
            }
        }
        // The five-tap reconstruction's merged centre tap is only valid under
        // linear filtering, and the motion field wants the same. nrvk creates
        // NEAREST + MIRRORED_REPEAT because that is the shipping colour
        // kernel's addressing; replace the sampler here rather than change a
        // production header.
        auto linear_sampler = [&] {
            VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = si.minFilter = VK_FILTER_LINEAR;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            VkSampler sampler{};
            NRVK_CHECK(vkCreateSampler(s.ctx.device, &si, nullptr, &sampler));
            return sampler;
        };
        t.h16 = std::getenv("NR_HIST16") && std::atoi(std::getenv("NR_HIST16")) && config.max_passes == 1 &&
                std::filesystem::exists(shaders / "temporal_post_fp32h16.spv");
        const VkFormat hist_format = t.h16 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
        t.history = s.ctx.image(mw, mh, hist_format, true);
        vkDestroySampler(s.ctx.device, t.history.sampler, nullptr);
        t.history.sampler = linear_sampler();
        // Kept in GENERAL for its whole life: it is a transfer destination once
        // a frame and a sampled source once a frame, and GENERAL is valid for
        // both. The alternative is two layout transitions per frame to save
        // nothing measurable on an image this size.
        s.ctx.transition(t.history, VK_IMAGE_LAYOUT_GENERAL);
        // The finest flow level is written as a storage image and read as a
        // sampled one. Same VkImage and view, two descriptors; the alias does
        // not own the image, so it must never be handed to ctx.destroy.
        t.motion_sampler = linear_sampler();
        if (config.motion_point) {
            VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = si.minFilter = VK_FILTER_NEAREST;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            NRVK_CHECK(vkCreateSampler(s.ctx.device, &si, nullptr, &t.motion_point_sampler));
        }
        t.flow0_sampled = t.flow[0];
        t.flow0_sampled.sampler = t.motion_sampler;
        // Single channel and float: a depth buffer's value is all this reads, and
        // the blit converts whatever the game hands over.
        t.depth = s.ctx.image(mw, mh, VK_FORMAT_R32_SFLOAT, true);
        vkDestroySampler(s.ctx.device, t.depth.sampler, nullptr);
        t.depth.sampler = linear_sampler();
        s.ctx.transition(t.depth, VK_IMAGE_LAYOUT_GENERAL);
        {
            // runtime_depth.comp's layout: a sampled source, then t.depth as storage.
            nrvk::Context::Image depth_store = t.depth;
            depth_store.sampler = VK_NULL_HANDLE;
            impl_->depth_copy.create(s.ctx, (adapters / "runtime_depth.spv").string(), {}, 24,
                                     {&t.depth, &depth_store});
        }
        t.params = s.ctx.buffer(48);

        for (unsigned p = 0; p < 2; ++p) {
            for (unsigned k = 0; k < kTemporalLevels; ++k) {
                // Binding 1 is the finer level this one halves; in mode 0 it is
                // not read, and the coarsest level is bound there as a valid
                // descriptor that nothing touches.
                auto* src = k ? &t.luma[p][k - 1] : &t.luma[p][kTemporalLevels - 1];
                t.luma_kernel[p][k].create(s.ctx, (shaders / "motion_luma.spv").string(), {},
                                           sizeof(Temporal::LumaPush),
                                           {&s.tex_in, src, &t.luma[p][k]});
                // Likewise, the coarsest flow pass runs with first=1 and never
                // reads flow_in, so it may point at its own output.
                const bool first = k == kTemporalLevels - 1;
                t.flow_kernel[p][k].create(s.ctx, (shaders / "motion_estimate.spv").string(), {},
                                           sizeof(Temporal::FlowPush),
                                           {&t.luma[p][k], &t.luma[1 - p][k],
                                            first ? &t.flow[k] : &t.flow[k + 1], &t.flow[k]});
            }
        }
        t.pingpong = config.max_passes == 1;
        impl_->motion_all_passes = config.motion_in_place_passes && config.max_passes > 1;
        if (t.pingpong) {
            t.history_b = s.ctx.image(mw, mh, t.history.format, true);
            vkDestroySampler(s.ctx.device, t.history_b.sampler, nullptr);
            t.history_b.sampler = linear_sampler();
            s.ctx.transition(t.history_b, VK_IMAGE_LAYOUT_GENERAL);
        }
        // The storage view of a history image the post variant writes: the same
        // image and view, no sampler (an alias; it owns nothing).
        auto storage_of = [](const nrvk::Context::Image& im) {
            nrvk::Context::Image a = im; a.sampler = VK_NULL_HANDLE; return a;
        };
        {
            const bool tp_nh0 = pre_nh && std::filesystem::exists(shaders / "temporal_pre_fp32nh.spv");
            t.hp = std::getenv("NR_HPASS") && std::atoi(std::getenv("NR_HPASS")) &&
                   std::filesystem::exists(shaders / (tp_nh0 ? "temporal_pre_fp32nhhp.spv" : "temporal_pre_fp32hp.spv")) &&
                   std::filesystem::exists(shaders / "temporal_post_fp32hp.spv");
            if (t.hp) {
                t.hpass = s.ctx.image(mw, mh, VK_FORMAT_R32G32B32A32_SFLOAT, false);
                s.ctx.transition(t.hpass, VK_IMAGE_LAYOUT_GENERAL);
            }
        }
        nrvk::Context::Image (&hist_store)[2] = t.hist_store;
        hist_store[0] = storage_of(t.hist(0)); hist_store[1] = storage_of(t.hist(1));
        t.pre_buffers = {s.act.handle, s.act.handle, s.wgt.handle, s.wgt.handle, s.wgt.handle,
                         s.act.handle, t.params.handle};
        t.post_buffers = {s.act.handle, s.act.handle, s.wgt.handle, s.wgt.handle, s.wgt.handle,
                          t.params.handle};
        for (unsigned c = 0; c < (t.pingpong ? 2u : 1u); ++c) {
            nrvk::Kernel& pre = t.pingpong ? t.pre_pp[c] : t.pre;
            nrvk::Kernel& post = t.pingpong ? t.post_pp[c] : t.post;
            // the no-upper-clamp temporal pre when the graph's pre qualified and the
            // package has it (the clamped one is the same function there).
            const bool tp_nh = pre_nh && std::filesystem::exists(shaders / "temporal_pre_fp32nh.spv");
            // (research NR_HPASS=1): the pre block hands the post its history.
            const std::string pre_spv = std::string(tp_nh ? "temporal_pre_fp32nh" : "temporal_pre_fp32") + (t.hp ? "hp.spv" : ".spv");
            pre.create(s.ctx, (shaders / pre_spv).string(),
                       {s.act.handle, s.act.handle, s.wgt.handle, s.wgt.handle, s.wgt.handle,
                        s.act.handle, t.params.handle},
                       sizeof(PushFSwin) + sizeof(PushPreImage),
                       t.hp ? std::vector<nrvk::Context::Image*>{&s.tex_in, &t.flow0_sampled, &t.hist(c), &t.depth, &t.hpass}
                            : std::vector<nrvk::Context::Image*>{&s.tex_in, &t.flow0_sampled, &t.hist(c), &t.depth});
            // The post variant reads the same parameter buffer, the motion field and
            // the history the pre variant does: its blend is gated by the same term.
            const std::string post_name = t.hp ? "temporal_post_fp32hp" : t.h16 ? "temporal_post_fp32h16" : "temporal_post_fp32";
            const std::vector<nrvk::Context::Image*> post_images = t.hp
                ? std::vector<nrvk::Context::Image*>{&s.surf0, t.pingpong ? &hist_store[c ^ 1u] : &s.surf1, &s.tex_in,
                                                     &t.flow0_sampled, &t.hist(c), &t.hpass}
                : std::vector<nrvk::Context::Image*>{&s.surf0, t.pingpong ? &hist_store[c ^ 1u] : &s.surf1, &s.tex_in,
                                                     &t.flow0_sampled, &t.hist(c)};
            post.create(s.ctx, (shaders / (post_name + ".spv")).string(),
                        {s.act.handle, s.act.handle, s.wgt.handle, s.wgt.handle, s.wgt.handle,
                         t.params.handle},
                        sizeof(PushFSwin) + sizeof(PushUps) + sizeof(PushImageTail), post_images);
            // Its Style-grade twin (bound with post's sets at dispatch); without it no pass is graded.
            if (impl_->style_grade && std::filesystem::exists(shaders / (post_name + "_grade.spv")))
                (t.pingpong ? t.post_grade_pp[c] : t.post_grade).create(s.ctx, (shaders / (post_name + "_grade.spv")).string(),
                        {s.act.handle, s.act.handle, s.wgt.handle, s.wgt.handle, s.wgt.handle,
                         t.params.handle},
                        sizeof(PushFSwin) + sizeof(PushUps) + sizeof(PushImageTail), post_images);
            else impl_->style_grade = false;
        }
        t.history_strength = temporal_config.history_strength;
        t.enabled = true;
        // One history per pass. Pass k reads and writes store[k] through the
        // bound history image; the copies are model-sized and cost nothing
        // next to a pass.
        if (config.max_passes > 1) {
            impl_->history_store.resize(config.max_passes);
            for (auto& h : impl_->history_store) {
                h = s.ctx.image(mw, mh, VK_FORMAT_R32G32B32A32_SFLOAT, false);
                s.ctx.transition(h, VK_IMAGE_LAYOUT_GENERAL);
            }
        }
    }
    if(transfer != Transfer::Identical) {
        for(VkFormat format : {transfer == Transfer::Encoded ? unorm : config.colour_format,
                               VK_FORMAT_R32G32B32A32_SFLOAT}) {
            VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(host.physical,format,&properties);
            const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;
            if((properties.optimalTilingFeatures&need)!=need)
                throw std::runtime_error("NR format conversion requires source/destination blit support");
        }
    }
    // Only the 8-bit path needs the private transfer image: it exists to keep an
    // *_SRGB view from applying its transfer function to the bits, not to
    // convert anything. The wide Wayland formats blit straight through.
    // (Not with the answer stored in the frame's format at its extent: in and out are copies there.)
    if(transfer == Transfer::Encoded && !(impl_->store_native && !impl_->scaled)) {
        auto& im=impl_->encoded;im.w=config.width;im.h=config.height;im.format=unorm;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType=VK_IMAGE_TYPE_2D;info.format=unorm;info.extent={im.w,im.h,1};
        info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;
        info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        NRVK_CHECK(vkCreateImage(host.device,&info,nullptr,&im.handle));
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(host.device,im.handle,&req);
        uint32_t type=s.ctx.mem.memoryTypeCount;
        for(uint32_t i=0;i<s.ctx.mem.memoryTypeCount;++i)
            if((req.memoryTypeBits&(1u<<i))&&(s.ctx.mem.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {type=i;break;}
        if(type==s.ctx.mem.memoryTypeCount)throw std::runtime_error("no device-local transfer image memory");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;
        NRVK_CHECK(vkAllocateMemory(host.device,&allocation,nullptr,&im.memory));
        NRVK_CHECK(vkBindImageMemory(host.device,im.handle,im.memory,0));
        s.ctx.transition(im,VK_IMAGE_LAYOUT_GENERAL);
    }
    impl_->post_alpha = NR_POST_ALPHA && impl_->native_compose && !impl_->scaled &&
                        impl_->max_passes == 1 && s.kern.count("fswinimagepost32");
    if (config.require_style_grade && !impl_->style_grade)
        throw std::runtime_error("Mochizuki temporal style-grade shaders are incomplete; reinstall the full OptScaler NR package");
    timer.mark("adapters");
    // Built: from here on the caller serialises this runtime's submits itself.
    s.ctx.queue_lock = nullptr; s.ctx.queue_unlock = nullptr;
    s.ctx.defer_transitions = false;   // its pending transitions stay for the first recording
    s.ctx.registry_only = false;
    // A whole network (its own memory): the image bytes a runtime of it takes per frame pixel, with the feature's
    // history made at its first recording, and the biggest frame its memory runs - what an image heap for the
    // runtimes built inside frames in this memory has to hold (Runtime::image_heap_wanted).
    if (!config.share && s.arenas && !config.plan_only) {
        const double px = double(config.width) * double(config.height);
        const double hist = double(impl_->temporal.history.w) * impl_->temporal.history.h * 16;
        s.arenas->bytes_per_px = (double(s.ctx.image_bytes) + hist) / px * 1.05;
        s.arenas->max_px = px * std::max(1.0, config.capacity);   // the frames its arena has room for; px_unfit beyond
    }
    nr::logf("[nr] runtime %ux%u built in %.2fs: %s", config.width, config.height, timer.total(),
             timer.text().c_str());
}

Runtime::~Runtime() = default;

std::shared_ptr<const RuntimeCore> Runtime::share_for(uint32_t width, uint32_t height) const {
    const auto& s = impl_->session;
    if (!s.core || !width || !height) return nullptr;
    const uint32_t mw = model_extent(width, impl_->model_scale), mh = model_extent(height, impl_->model_scale);
    if (impl_->native_compose && (mw != width || mh != height)) return nullptr;
    const auto native = make_native_plan(mw, mh, impl_->network);
    if (native.width != impl_->work_w || native.height != impl_->work_h || mh > s.core->source_h) return nullptr;
    auto c = std::make_shared<RuntimeCore>();
    c->graph = s.core; c->slot = s.arenas; c->arguments = impl_->share_arguments; c->network = impl_->network;
    c->work_w = impl_->work_w; c->work_h = impl_->work_h;
    c->source_w = s.core->source_w; c->source_h = s.core->source_h;
    return c;
}

void plan_extent(const RuntimeCore& plan, uint32_t* work_w, uint32_t* work_h, uint32_t* source_h) {
    *work_w = plan.work_w; *work_h = plan.work_h; *source_h = plan.source_h;
}

void working_extent(NetworkModel model, float scale, uint32_t width, uint32_t height, uint32_t* work_w,
                    uint32_t* work_h) {
    const auto native = make_native_plan(model_extent(width, scale), model_extent(height, scale), model);
    *work_w = native.width; *work_h = native.height;
}

uint32_t plan_height(NetworkModel model, float scale, uint32_t width, uint32_t height) {
    uint32_t ww = 0, wh = 0, tw = 0, th = 0;
    working_extent(model, scale, width, height, &ww, &wh);
    uint32_t best = height;
    // A working extent is at most a reduction step (128 rows) taller than the frame; frames up to it are tried.
    for (uint32_t h = height + 1; h <= wh && h <= height + 256; ++h) {
        working_extent(model, scale, width, h, &tw, &th);
        if (tw == ww && th == wh) best = h;
    }
    return best;
}

std::shared_ptr<const RuntimeCore> Runtime::plan() const {
    const auto& s = impl_->session;
    if (!s.core) return nullptr;
    auto c = std::make_shared<RuntimeCore>();
    c->graph = s.core; c->arguments = impl_->share_arguments; c->network = impl_->network;
    c->work_w = impl_->work_w; c->work_h = impl_->work_h;
    c->source_w = s.core->source_w; c->source_h = s.core->source_h;
    return c;
}

std::shared_ptr<const RuntimeCore> Runtime::with_slot_of(const std::shared_ptr<const RuntimeCore>& plan) const {
    const auto& s = impl_->session;
    if (!plan || !plan->graph || !s.arenas || plan->arguments != impl_->share_arguments || plan->network != impl_->network)
        return nullptr;
    const GraphCore& g = *plan->graph;
    const GraphArenas& a = *s.arenas;
    if (a.weights_hash != g.weights_hash || a.weights_end != g.weights_end || a.act->cap < g.act_total ||
        a.wgt_cap < g.weights_end + g.tail.size())
        return nullptr;
    auto c = std::make_shared<RuntimeCore>(*plan);
    c->slot = s.arenas;
    return c;
}

const void* Runtime::network_id() const { return impl_->session.arenas.get(); }

std::shared_ptr<ActArenaRef> Runtime::act_arena() const {
    const auto& s = impl_->session;
    if (!s.arenas || !s.arenas->act) return nullptr;
    return std::make_shared<ActArenaRef>(ActArenaRef{s.arenas->act});
}
const void* Runtime::act_id() const {
    const auto& s = impl_->session;
    return s.arenas ? static_cast<const void*>(s.arenas->act.get()) : nullptr;
}
uint64_t act_arena_bytes(const ActArenaRef& a) { return a.act ? a.act->cap : 0; }
const void* act_arena_id(const ActArenaRef& a) { return a.act.get(); }

const std::string& Runtime::network_note() const { return impl_->network_note; }
bool Runtime::motion_without_depth() const { return impl_->motion_without_depth; }

// What Runtime::image_heap_wanted / allocate_image_heap / adopt_image_heap pass around.
struct ImageHeapRef { std::shared_ptr<nrvk::ImageHeap> heap; };

uint64_t Runtime::image_heap_wanted() const {
    const auto& a = impl_->session.arenas;
    if (!a || a->bytes_per_px <= 0 || a->max_px <= 0) return 0;
    const VkDeviceSize region =
        (VkDeviceSize(a->bytes_per_px * std::max(a->max_px, a->px_unfit * 1.1)) + 65535) / 65536 * 65536;
    return a->images && a->images->region_cap >= region ? 0 : region;
}

std::shared_ptr<ImageHeapRef> allocate_image_heap(const HostDevice& host, uint64_t region) {
    runtime_dispatch::Scope physical_scope(host.instance, host.physical_dispatch);
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // the memory type our images take
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R32G32B32A32_SFLOAT; ii.extent = {8, 8, 1};
    ii.mipLevels = ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage probe{};
    if (vkCreateImage(host.device, &ii, nullptr, &probe) != VK_SUCCESS) return nullptr;
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(host.device, probe, &req);
    vkDestroyImage(host.device, probe, nullptr);
    VkPhysicalDeviceMemoryProperties mem{}; vkGetPhysicalDeviceMemoryProperties(host.physical, &mem);
    uint32_t type = mem.memoryTypeCount;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i; break;
        }
    if (type == mem.memoryTypeCount) return nullptr;
    auto heap = std::make_shared<nrvk::ImageHeap>();
    heap->device = host.device; heap->type = type; heap->region_cap = region;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = region * 2; alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(host.device, &alloc, nullptr, &heap->memory) != VK_SUCCESS) return nullptr;
    auto ref = std::make_shared<ImageHeapRef>();
    ref->heap = std::move(heap);
    return ref;
}

void Runtime::adopt_image_heap(const std::shared_ptr<ImageHeapRef>& ref) {
    auto& a = impl_->session.arenas;
    if (!a || !ref || !ref->heap) return;
    if (a->images && a->images->region_cap >= ref->heap->region_cap) return;
    a->images = ref->heap;   // the old one stays with the runtimes in it
    nr::logf("[nr] image heap for the network's frame sizes: 2 x %.0f MiB", double(a->images->region_cap) / (1 << 20));
}

const void* core_network(const RuntimeCore& core) {
    return core.slot ? static_cast<const void*>(core.slot.get())
                     : core.graph ? static_cast<const void*>(core.graph->arenas.get()) : nullptr;
}
std::shared_ptr<ActArenaRef> core_act_arena(const RuntimeCore& core) {
    GraphArenas* a = core.slot ? core.slot.get() : core.graph ? core.graph->arenas.get() : nullptr;
    return a && a->act ? std::make_shared<ActArenaRef>(ActArenaRef{a->act}) : nullptr;
}
bool network_offloaded(const RuntimeCore& core) {
    const GraphArenas* a = core.slot ? core.slot.get() : core.graph ? core.graph->arenas.get() : nullptr;
    return a && !a->wgt.handle && !a->host_weights.empty();
}
uint64_t network_ram_bytes(const RuntimeCore& core) {
    const GraphArenas* a = core.slot ? core.slot.get() : core.graph ? core.graph->arenas.get() : nullptr;
    return a ? a->host_weights.size() : 0;
}

static bool adopt_for(nrvk::Context& ctx, const HostDevice& host) {
    if (host.rdna3 >= 0) ctx.rdna3 = (host.rdna3 == 1);
    std::string why;
    if (!ctx.adopt(host.instance, host.physical, host.device, host.queue, host.queue_family, &why)) {
        nr::logf("[nr] the network's memory cannot be moved: %s", why.c_str());
        return false;
    }
    ctx.queue_lock = host.queue_lock; ctx.queue_unlock = host.queue_unlock;
    ctx.buffer_device_address = host.buffer_device_address;
    return true;
}

// Offload in two halves: the read-back on a worker thread (nothing of the network changes), the release on the thread
// that records (the render thread reads these arenas while it picks an activation arena for a build).
bool download_network_weights(const HostDevice& host, const RuntimeCore& core, std::vector<uint8_t>& out) {
    const GraphArenas* a = core.slot ? core.slot.get() : core.graph ? core.graph->arenas.get() : nullptr;
    if (!a || !a->wgt.handle || !a->weights_end) return false;
    nrvk::Context ctx;
    if (!adopt_for(ctx, host)) return false;
    try {
        std::vector<uint8_t> w(a->weights_end);
        ctx.download(const_cast<nrvk::Buffer&>(a->wgt), 0, w.data(), w.size());
        out = std::move(w);
    } catch (const std::exception& e) {
        nr::logf("[nr] the network's weights could not be read back: %s", e.what());
        return false;
    }
    return true;
}

void release_network(const RuntimeCore& core, std::vector<uint8_t>&& weights) {
    GraphArenas* a = core.slot ? core.slot.get() : core.graph ? core.graph->arenas.get() : nullptr;
    if (!a || !a->wgt.handle || weights.size() != a->weights_end) return;
    a->host_weights = std::move(weights);
    if (a->noise.device) a->noise.destroy();
    a->noise = nrvk::Kernel{};
    if (a->wgt.mapped) vkUnmapMemory(a->device, a->wgt.memory);
    vkDestroyBuffer(a->device, a->wgt.handle, nullptr);
    vkFreeMemory(a->device, a->wgt.memory, nullptr);
    a->wgt = nrvk::Buffer{};
    if (a->wgt_e.mapped) vkUnmapMemory(a->device, a->wgt_e.memory);
    if (a->wgt_e.handle) vkDestroyBuffer(a->device, a->wgt_e.handle, nullptr);
    if (a->wgt_e.memory) vkFreeMemory(a->device, a->wgt_e.memory, nullptr);
    a->wgt_e = nrvk::Buffer{};
    a->act_want = a->act_need ? a->act_need : a->act ? a->act->cap : a->act_want;
    a->act.reset();
    a->loaded = 0;
}

bool offload_network(const HostDevice& host, const RuntimeCore& core) {
    std::vector<uint8_t> w;
    if (!download_network_weights(host, core, w)) return false;
    release_network(core, std::move(w));
    return network_offloaded(core);
}

bool reload_network(const HostDevice& host, const RuntimeCore& core, const std::shared_ptr<ActArenaRef>& act, double slack) {
    GraphArenas* a = core.slot ? core.slot.get() : core.graph ? core.graph->arenas.get() : nullptr;
    if (!a || a->wgt.handle || a->host_weights.empty()) return false;
    nrvk::Context ctx;
    if (!adopt_for(ctx, host)) return false;
    // everything made first, committed together: a failure leaves the network as it was (offloaded)
    std::shared_ptr<GraphAct> arena = a->act;
    if (!arena) {
        if (act && act->act && act->act->device == a->device && act->act->cap >= a->act_want) {
            arena = act->act;
            if (ctx.rdna3 && arena && !arena->twin.handle && arena->cap) {
                arena->twin = ctx.buffer(VkDeviceSize(arena->cap) * 2);
            }
        } else {
            auto g = std::make_shared<GraphAct>();
            g->device = a->device; g->cap = size_t(double(a->act_want) * std::max(1.0, slack) + 255) / 256 * 256;
            g->buf = ctx.buffer(g->cap, false, /*device_address=*/true);   // freed by ~GraphAct if what follows throws
            if (ctx.rdna3) {
                g->twin = ctx.buffer(VkDeviceSize(g->cap) * 2);
            }
            arena = std::move(g);
        }
    } else if (ctx.rdna3 && !arena->twin.handle && arena->cap) {
        arena->twin = ctx.buffer(VkDeviceSize(arena->cap) * 2);
    }
    nrvk::Buffer w{}, w_e{};
    try {
        w = ctx.buffer(a->wgt_cap);
        if (ctx.rdna3) {
            w_e = ctx.buffer(VkDeviceSize(a->wgt_cap) * 2);
        }
        ctx.upload(w, 0, a->host_weights.data(), a->host_weights.size());
        if (ctx.rdna3) {
            std::array<uint16_t, 256> to_f16{};
            for (int i = 0; i < 256; ++i) to_f16[size_t(i)] = tin::f_to_f16(tin::e4m3_to_f(uint8_t(i)));
            std::vector<uint8_t> f16_buf(a->host_weights.size() * 2);
            uint16_t* out = reinterpret_cast<uint16_t*>(f16_buf.data());
            for (size_t i = 0; i < a->host_weights.size(); ++i) out[i] = to_f16[a->host_weights[i]];
            ctx.upload(w_e, 0, f16_buf.data(), f16_buf.size());
        }
    } catch (...) {
        if (w.handle) ctx.destroy(w);
        if (w_e.handle) ctx.destroy(w_e);
        throw;
    }
    a->wgt = w;
    if (ctx.rdna3) a->wgt_e = w_e;
    a->act = std::move(arena);
    a->loaded = 0;   // the tail and the noise fields: the first recording (retarget)
    std::vector<uint8_t>().swap(a->host_weights);
    return true;
}

bool core_runs(const RuntimeCore& c, uint32_t width, uint32_t height, float scale, bool native_compose) {
    const uint32_t mw = model_extent(width, scale), mh = model_extent(height, scale);
    if (native_compose && (mw != width || mh != height)) return false;
    const auto native = make_native_plan(mw, mh, c.network);
    return native.width == c.work_w && native.height == c.work_h && mh <= c.source_h;
}

void release_image_heap(const RuntimeCore& core) {
    GraphArenas* a = core.slot ? core.slot.get() : core.graph ? core.graph->arenas.get() : nullptr;
    if (a) a->images.reset();
}

bool image_heap_fits(const RuntimeCore& core, uint32_t width, uint32_t height) {
    const GraphArenas* a = core.slot ? core.slot.get() : core.graph ? core.graph->arenas.get() : nullptr;
    return a && a->images && a->bytes_per_px > 0 &&
           a->images->region_cap >= VkDeviceSize(a->bytes_per_px * double(width) * double(height));
}

bool Runtime::owns_image_memory() const {
    return impl_->session.share && !impl_->heap;
}

void Runtime::begin_recording_resources() {
    if (impl_->recording_resources) throw std::logic_error("nested NR recording resource scope");
    impl_->recording_resources = std::make_shared<Impl::RecordingResources>(impl_->session.ctx.device);
}

std::shared_ptr<void> Runtime::end_recording_resources() {
    return std::move(impl_->recording_resources);
}

bool Runtime::carry_history(VkCommandBuffer cmd, const Runtime& from, uint64_t feature) {
    auto& t = impl_->temporal;
    auto& ft = from.impl_->temporal;
    if (!cmd || !feature || !t.enabled || !ft.enabled || impl_->max_passes != 1 || from.impl_->max_passes != 1 ||
        from.stale() || stale())
        return false;
    const auto fit = from.impl_->features.find(feature);
    if (fit == from.impl_->features.end() || !fit->second.latch) return false;
    // What the feature's next frame would have read there: the bound history, or its own image.
    const nrvk::Context::Image& src = from.impl_->bound_feature == feature ? ft.hist(ft.hcur) : fit->second.history;
    if (!src.handle) return false;
    auto& s = impl_->session;
    auto it = impl_->features.find(feature);
    if (it == impl_->features.end()) {
        Impl::FeatureState made{};
        // sampled as well, as temporal.history is: the temporal kernels read a feature's history through a sampler
        made.history = s.ctx.image(t.history.w, t.history.h, t.history.format, true, false);
        it = impl_->features.emplace(feature, made).first;
    }
    Impl::FeatureState& fs = it->second;
    // Into what this runtime's next recording of the feature reads: the bound history when it is bound, else its
    // own image (record_all copies that in when it binds it).
    const bool bound = impl_->bound_feature == feature;
    nrvk::Context::Image& dst = bound ? t.hist(t.hcur) : fs.history;
    if (impl_->recording_resources) {
        if (from.impl_->bound_feature != feature) {
            auto& source = fit->second;
            if (!source.history_owner)
                source.history_owner = std::make_shared<Impl::OwnedHistory>(from.impl_->session.ctx, source.history);
            impl_->recording_resources->histories.push_back(source.history_owner);
        }
        impl_->retain_feature_history(fs);
    }
    const VkImageLayout dst_from = bound || fs.cleared ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier(cmd, src.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(cmd, dst.handle, dst_from, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {int32_t(src.w), int32_t(src.h), 1};
    b.dstOffsets[1] = {int32_t(dst.w), int32_t(dst.h), 1};
    const VkFilter filter = impl_->filters_linearly(src.format) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    vkCmdBlitImage(cmd, src.handle, VK_IMAGE_LAYOUT_GENERAL, dst.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &b, filter);
    barrier(cmd, dst.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    barrier(cmd, src.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0);
    dst.layout = VK_IMAGE_LAYOUT_GENERAL;
    if (!bound) fs.cleared = true;
    // The feature goes on as it was: history consumed, the noise counter where it was.
    fs.latch = true;
    fs.seed = fit->second.seed;
    fs.parity = fit->second.parity;
    fs.prep_gen = impl_->prep_gen;
    return true;
}

bool Runtime::stale() const {
    const auto& h = impl_->heap;
    return h && h->gen[impl_->heap_region] != impl_->heap_gen;
}

uint32_t Runtime::model_width() const { return impl_->mw; }
uint32_t Runtime::model_height() const { return impl_->mh; }
NetworkModel Runtime::network() const { return impl_->network; }
#if NR_INT4
bool Runtime::int4_mixed() const { return impl_->int4_mixed; }
#endif

void Runtime::set_style_grade(bool on) { impl_->style_grade_on = on; }

void Runtime::set_network_segments(std::function<VkCommandBuffer(VkCommandBuffer, bool)> callback) {
    impl_->network_segments = std::move(callback);
}
const char* Runtime::compute_refusal(bool motion, VkFormat motion_format, VkImageUsageFlags motion_usage,
                                     int passes, bool mask) const {
    auto& m = *impl_;
    if (m.scaled && !m.compute_downscale) return "model scale below 1 needs a downscale blit";
    if (!m.compute_downscale && m.transfer != Transfer::Identical && !m.direct_in && !m.f16_built)
        return "colour input conversion needs a blit";
    if (!m.compute_downscale && m.transfer != Transfer::Identical && !m.direct_out && !m.store_native && !m.pack_built &&
        !m.f16_built)
        return "colour write-back conversion needs a blit";
    if (motion && !(NR_DIRECT_MOTION && !m.motion_src_failed && !mask &&
                    ((passes == 1 && m.temporal.pingpong) || (!m.temporal.pingpong && m.motion_all_passes)) &&
                    (motion_usage & VK_IMAGE_USAGE_SAMPLED_BIT) && m.filters_linearly(motion_format)))
        return "motion vectors need a blit";
    return nullptr;
}
void Runtime::set_compute_recording(bool on) {
    impl_->compute_recording = on || !impl_->graphics_family;
}
void Runtime::set_white_point(float v) {
    if (impl_->linear) impl_->white_point = v > 1e-4f ? v : 1e-4f;
}

void Runtime::set_history_strength(float v) {
    if (impl_->temporal.enabled) impl_->temporal.history_strength = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

void Runtime::release_feature(uint64_t feature) {
    if (!feature) return;
    auto it = impl_->features.find(feature);
    if (it == impl_->features.end()) return;
    impl_->retain_feature_history(it->second);
    if (it->second.history_owner)
        impl_->retiring_owned.emplace_back(it->second.history_owner, impl_->recordings + Impl::kRetireAfter);
    else if (it->second.history.handle)
        impl_->retiring.emplace_back(it->second.history,
                                     impl_->recordings + Impl::kRetireAfter);
    impl_->features.erase(it);
    // The bound history keeps whatever this feature left in it; the next
    // feature to record copies its own state over the top, and a feature that
    // is gone has no state to write back.
    if (impl_->bound_feature == feature) impl_->bound_feature = 0;
}

void Runtime::drop_history() {
    // A recording that advanced the temporal state (latch, parity, seed, the bound feature's copies)
    // never ran. Every feature starts again as on its first frame: the gate stays closed until a
    // recording that does run has written its history, and a fresh image is cleared before use.
    impl_->temporal.latch = false;
    for (auto& f : impl_->features) { f.second.latch = false; f.second.cleared = false; }
    impl_->bound_feature = 0;
    // Nor did what it made ready (the bridge: a job runs only when the game runs its command list): a runtime built
    // inside a recording records its images' first transitions again (from UNDEFINED: their content is not wanted,
    // the history starts again anyway), and the network memory is zeroed and loaded for this plan again (retarget).
    auto& s = impl_->session;
    if (!impl_->first_transitions.empty()) s.ctx.pending = impl_->first_transitions;
    if (s.arenas) {
        s.arenas->loaded = 0;
        if (s.arenas->act) s.arenas->act->loaded = 0;
    }
}

void Runtime::forget_history() {
    impl_->temporal.latch = false;
    for (auto& f : impl_->features) f.second.latch = false;
}

uint32_t Runtime::live_features() const { return uint32_t(impl_->features.size()); }

Runtime::TemporalHistoryView Runtime::temporal_history() const {
    TemporalHistoryView v{};
    const auto& t = impl_->temporal;
    if (!t.enabled) return v;
    const auto& h = const_cast<Temporal&>(t).hist(t.hcur);
    v.image = h.handle; v.view = h.view;
    v.width = h.w; v.height = h.h;
    v.format = h.format; v.layout = h.layout;
    return v;
}

RecordResult Runtime::record(VkCommandBuffer cmd, const ColourFrame& frame, const Controls& c) {
    return record(cmd,frame,c,nullptr).frame;
}

RecordResult Runtime::record(VkCommandBuffer cmd, const ColourFrame& frame, const Controls& c, bool reset) {
    if (reset) impl_->plain_seed = 0;
    return record(cmd,frame,c,nullptr).frame;
}

ControlMaskResult Runtime::record(VkCommandBuffer cmd, const ColourFrame& frame, const Controls& c,
                             const ControlMaskFrame* mask) {
    return record_all(cmd,frame,c,mask,nullptr,nullptr,nullptr);
}

TemporalResult Runtime::record_temporal(VkCommandBuffer cmd, const ColourFrame& frame,
                                        const Controls& c, const TemporalFrame& temporal) {
    if (!impl_->temporal.enabled)
        throw std::invalid_argument("this NR runtime was not built with the temporal path");
    TemporalResult result{};
    bool consumed = false;
    result.frame = record_all(cmd,frame,c,nullptr,&temporal,&consumed,nullptr).frame;
    result.history_consumed = consumed;
    // The only provider that exists today. When the upscaler interception lands
    // it fills the same texture and this becomes "engine" - an estimate must
    // never be reported as engine data.
    result.motion_provider = "estimated";
    return result;
}

bool Runtime::takes_target(const Controls& c) const {
    return c.enabled && c.apply_model && impl_->temporal.enabled &&
           std::min<uint32_t>(std::max(c.passes, 1), impl_->max_passes) == 1;
}

EngineResult Runtime::record_engine(VkCommandBuffer cmd, const EngineFrame& frame,
                                    const Controls& c) {
    auto& t = impl_->temporal;
    if (!t.enabled)
        throw std::invalid_argument("this NR runtime was not built with the temporal path");
    if (!frame.motion.image)
        throw std::invalid_argument("the engine provider requires a motion-vector image");
    EngineResult result{};
    bool consumed = false;
    // The engine's motion texture is blitted into the same image the fallback
    // estimator writes, so exactly one pipeline and one descriptor set serve
    // both providers. It costs one blit and it means the consumer cannot drift
    // between the two paths - which was the point of splitting them.
    TemporalFrame temporal{frame.reset, frame.feature};
    result.frame = record_all(cmd, frame.colour, c, nullptr, &temporal, &consumed, &frame).frame;
    result.history_consumed = consumed;
    result.motion_provider = "engine";
    return result;
}

ControlMaskResult Runtime::record_all(VkCommandBuffer cmd, const ColourFrame& frame, const Controls& c,
                             const ControlMaskFrame* mask, const TemporalFrame* temporal,
                             bool* history_consumed, const EngineFrame* engine) {
    if (stale()) throw std::logic_error("a runtime whose images another one has taken over (Runtime::stale)");
    validate(c);
    if (!cmd || !frame.image || frame.format != impl_->colour_format ||
        frame.width != impl_->width || frame.height != impl_->height ||
        !frame.before_stage || !frame.after_stage || frame.before == VK_IMAGE_LAYOUT_UNDEFINED ||
        frame.before == VK_IMAGE_LAYOUT_PREINITIALIZED || frame.after == VK_IMAGE_LAYOUT_UNDEFINED ||
        frame.after == VK_IMAGE_LAYOUT_PREINITIALIZED)
        throw std::invalid_argument("invalid NR colour frame");
    uint32_t mw=0,mh=0;
    if (mask) {
        const auto& t=mask->texture;
        mw=mask->width?mask->width:t.width;mh=mask->height?mask->height:t.height;
        if (!impl_->original_pre || !t.image || t.image==frame.image ||
            t.format!=VK_FORMAT_R32G32B32A32_SFLOAT ||
            t.width!=impl_->mask_image.w || t.height!=impl_->mask_image.h ||
            mask->x>=t.width || mask->y>=t.height || mw>t.width-mask->x || mh>t.height-mask->y ||
            !t.before_stage || !t.after_stage || t.before==VK_IMAGE_LAYOUT_UNDEFINED ||
            t.before==VK_IMAGE_LAYOUT_PREINITIALIZED || t.after==VK_IMAGE_LAYOUT_UNDEFINED ||
            t.after==VK_IMAGE_LAYOUT_PREINITIALIZED)
            throw std::invalid_argument("invalid/unconfigured external control mask");
    }
    // the one fallible allocation of a recording - a new feature's
    // history image - before anything is recorded, so a failure leaves the caller's
    // images in the layouts it gave and its fallback (seed copy, in place) sound.
    if (c.enabled && temporal && impl_->temporal.enabled && temporal->feature && !impl_->features.count(temporal->feature)) {
        const auto& th = impl_->temporal.history;
        Impl::FeatureState made{};
        made.history = impl_->session.ctx.image(th.w, th.h, th.format, true, false);
        impl_->features.emplace(temporal->feature, made);
    }
    ControlMaskResult result{};
    auto& info=result.frame;
    info.effective_skin = c.automatic_mask ? (c.skin_structure < 0 ? c.local_structure : c.skin_structure) : -1;
    info.effective_background = c.automatic_mask ? c.local_structure : -1;
    if (!c.enabled) {
        if (mask && mask->texture.before!=mask->texture.after) {
            const auto& t=mask->texture;
            barrier(cmd,t.image,t.before,t.after,t.before_stage,t.before_access,t.after_stage,t.after_access);
        }
        if (frame.before != frame.after)
            barrier(cmd, frame.image, frame.before, frame.after, frame.before_stage,
                    frame.before_access, frame.after_stage, frame.after_access);
        return result;
    }
    // Timed from here: a disabled pass records nothing and is not a cost.
    impl_->read_recording_timings();
    std::shared_ptr<Impl::RecordingTiming> recording_timing;
    VkQueryPool timing_pool = impl_->recording_resources ? VK_NULL_HANDLE : impl_->timing;
    if (impl_->recording_resources && impl_->timing_ok) {
        try {
            auto sample = std::make_shared<Impl::RecordingTiming>(impl_->session.ctx.device);
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = impl_->timing_stride;
            if (vkCreateQueryPool(impl_->session.ctx.device, &qi, nullptr, &sample->pool) == VK_SUCCESS && sample->pool) {
                impl_->recording_resources->timings.push_back(sample);
                impl_->pending_recording_timings.push_back(sample);
                recording_timing = std::move(sample);
                timing_pool = recording_timing->pool;
            }
        } catch (...) {}
    }
    const bool timing_active = impl_->timing_ok && timing_pool != VK_NULL_HANDLE;
    impl_->recording_timing_available = timing_active;
    const uint32_t timing_slot = recording_timing ? 0 : impl_->timing_slot;
    const bool time_stages = timing_active && impl_->split_timing && impl_->stage_timing;
    const uint32_t stage_query = timing_slot * impl_->timing_stride + impl_->stage_base;
    uint8_t stages_written = 0;
    if (timing_active) {
        vkCmdResetQueryPool(cmd, timing_pool, timing_slot * impl_->timing_stride, impl_->timing_stride);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timing_pool, timing_slot * impl_->timing_stride);
    }
    auto& s = impl_->session;
    Controls effective=c;
    if (mask) {
        effective.automatic_mask=false;effective.intensity=1;
        info.effective_skin=info.effective_background=-1;
        result.consumed=true;
        const auto& t=mask->texture;
        auto& im=impl_->mask_image;
        const float rect[]={float(mask->x),float(mask->y),float(mw),float(mh),1.f/t.width,1.f/t.height};
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=impl_->mask_rect.handle;b.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&b,0,nullptr);
        vkCmdUpdateBuffer(cmd,impl_->mask_rect.handle,0,sizeof rect,rect);
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&b,0,nullptr);
        barrier(cmd,t.image,t.before,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,t.before_stage,t.before_access,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        barrier(cmd,im.handle,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.extent={t.width,t.height,1};
        vkCmdCopyImage(cmd,t.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,im.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier(cmd,im.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
        barrier(cmd,t.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,t.after,VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,t.after_stage,t.after_access);
    }
    // A runtime built inside a recording (RuntimeConfig::deferred): its images' first layout transitions, here.
    if (!s.ctx.pending.empty()) {
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, uint32_t(s.ctx.pending.size()), s.ctx.pending.data());
        if (impl_->first_transitions.empty()) impl_->first_transitions = s.ctx.pending;
        s.ctx.pending.clear();
    }
    // The resize path: the network memory made ready for this runtime's plan when another one used it last.
    s.retarget(cmd);
    impl_->controls(effective);
    impl_->check_read();
    if (engine && temporal)
        impl_->incheck.begin(s.ctx, cmd, frame, *engine, temporal->feature, impl_->recordings,
                             impl_->temporal.lw[0], impl_->temporal.lh[0], impl_->mw, impl_->mh);
    // The network extent (Model Resolution); the frame is frame.width x frame.height.
    const uint32_t nw = impl_->mw, nh = impl_->mh;
    const uint32_t passes = std::min<uint32_t>(std::max(effective.passes, 1), impl_->max_passes);
    // a separate answer image (EngineFrame::target).
    const ColourFrame* tgt = engine && engine->target.image && engine->target.image != frame.image ? &engine->target : nullptr;
    if (tgt && (!c.apply_model || passes != 1 || mask || tgt->format != frame.format ||
                tgt->width != frame.width || tgt->height != frame.height || !tgt->before_stage || !tgt->after_stage ||
                tgt->before == VK_IMAGE_LAYOUT_PREINITIALIZED || tgt->after == VK_IMAGE_LAYOUT_UNDEFINED ||
                tgt->after == VK_IMAGE_LAYOUT_PREINITIALIZED))
        throw std::invalid_argument("invalid NR target frame");
    const VkImage wb = tgt ? tgt->image : frame.image;
    // The caller's colour (and depth) sampled in place, when this frame allows
    // it: see Impl::DirectSrc. Null means the copies below, as always.
    const Impl::DirectSrc* ds = nullptr;
    // post_alpha: native compose, one pass, scale 1, and no alpha pass - after
    // the network only the write-back touches the frame.
    const bool colour_direct =
        NR_DIRECT_SAMPLE && !impl_->direct_src_failed && impl_->post_alpha && !impl_->prep && engine &&
        temporal && !mask && passes == 1 &&
        impl_->temporal.pingpong && impl_->transfer != Transfer::Encoded &&
        (frame.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && frame.width == nw && frame.height == nh;
    // the depth in place is a separate question. A depth that still needs its blit or
    // conversion (render-resolution guides after SR, a subrect, depth/stencil) goes through
    // t.depth as before, and the colour is sampled in place (and the answer stored) anyway.
    // The game's depth, when the network reads it: with Motion Without Depth (the default, as NVIDIA's DLL) it is
    // neither bound nor copied.
    const bool game_depth = engine && engine->depth.image && !impl_->motion_without_depth;
    const bool depth_in_place = colour_direct && game_depth &&
        engine->depth.format == VK_FORMAT_R32_SFLOAT && (engine->depth.usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
        engine->depth_x == 0 && engine->depth_y == 0 &&
        engine->depth.width == impl_->temporal.depth.w && engine->depth.height == impl_->temporal.depth.h;
    // The motion vectors in place: any engine frame with a sampled image in a
    // linearly filterable format, whatever the pass count (every pass reads the
    // frame's vectors). Multipass whole-chain still needs motion_in_place_passes for
    // compute_refusal; recording itself follows the 0.0.5 default below.
    const bool motion_direct =
        NR_DIRECT_MOTION && !impl_->motion_src_failed && engine && temporal && !mask && engine->motion.image &&
        (engine->motion.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && impl_->filters_linearly(engine->motion.format);
    // (research NR_DIRECT_STORE=1): the post block stores into the frame itself.
    // With a separate target (storage-capable) the post block stores into it and the
    // write-back copy goes (4K RGBA16F -0.15 ms). In place (the post samples only its own
    // pixel, NEAREST, before it stores it) for 64-bit frames: 4K RGBA16F -0.05 ms; bgra8
    // ties (its copy stays in the cache). NR_DIRECT_STORE=0: never; =1: in place for any format.
    static const int direct_store_env = std::getenv("NR_DIRECT_STORE") ? std::atoi(std::getenv("NR_DIRECT_STORE")) : -1;
    const bool wide = frame.format == VK_FORMAT_R16G16B16A16_SFLOAT || frame.format == VK_FORMAT_R16G16B16A16_UNORM;
    const bool store_direct = colour_direct && impl_->direct_out && c.apply_model && frame.format == s.surf0.format &&
        !impl_->scaled && direct_store_env != 0 &&
        (tgt ? (tgt->usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0
             : (direct_store_env == 1 || wide) && (frame.usage & VK_IMAGE_USAGE_STORAGE_BIT));
    // the plain path (no temporal, no engine frame) samples the caller's colour in place
    // too - the input copy goes. Research switch NR_PLAIN_DIRECT=0 turns it off.
    static const bool plain_env = !(std::getenv("NR_PLAIN_DIRECT") && !std::atoi(std::getenv("NR_PLAIN_DIRECT")));
    const bool plain_direct = plain_env && !temporal && !engine && !mask && passes == 1 && impl_->plain_pre &&
        impl_->plain_post && !impl_->direct_src_failed && impl_->post_alpha && !impl_->prep && !impl_->linear &&
        impl_->transfer != Transfer::Encoded && (frame.usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
        frame.width == nw && frame.height == nh;
    const bool plain_store = plain_direct && impl_->direct_out && c.apply_model && frame.format == s.surf0.format &&
        !impl_->scaled && direct_store_env != 0 && (direct_store_env == 1 || wide) &&
        (frame.usage & VK_IMAGE_USAGE_STORAGE_BIT);
    const Impl::DirectSrc* sets = nullptr;
    if (plain_direct)
        sets = impl_->direct_source(frame.image, frame.format, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                    VK_FORMAT_UNDEFINED, 0, plain_store, VK_NULL_HANDLE, true);
    else if (colour_direct || motion_direct)
        sets = impl_->direct_source(colour_direct ? frame.image : VK_NULL_HANDLE, frame.format,
                                    depth_in_place ? engine->depth.image : VK_NULL_HANDLE,
                                    motion_direct ? engine->motion.image : VK_NULL_HANDLE,
                                    engine->motion.format, impl_->temporal.hcur, store_direct,
                                    tgt ? tgt->image : VK_NULL_HANDLE);
    if (sets && sets->cview) ds = sets;
    // Model resolution with the frame read and written in place (Impl::FrameRw). NR_FRAME_RW=0: never.
    static const bool frame_rw_env = !(std::getenv("NR_FRAME_RW") && !std::atoi(std::getenv("NR_FRAME_RW")));
    const Impl::FrameRw* fr = frame_rw_env && impl_->store_native && impl_->scaled && !tgt && !mask &&
        (frame.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && (frame.usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
        frame.format == impl_->keep_full.format && frame.width == impl_->keep_full.w && frame.height == impl_->keep_full.h
        ? impl_->frame_rw_source(frame.image, frame.format) : nullptr;
    const bool stored = (ds && ds->store) || fr;
    const bool depth_direct = ds && ds->dview != VK_NULL_HANDLE;   // the caller's depth bound in place
    const bool mv_direct = sets && sets->mview;
    barrier(cmd, frame.image, frame.before, ds || fr ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            frame.before_stage, frame.before_access,
            ds || fr ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
            stored && !tgt ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                   : ds ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT);
    if (stored && tgt)
        barrier(cmd, tgt->image, tgt->before, VK_IMAGE_LAYOUT_GENERAL, tgt->before_stage, tgt->before_access,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    // Where tex_in stands once the frame is in it: a copy or blit leaves it a transfer
    // destination, runtime_downscale.comp (keep_native) a storage image.
    const bool down_native = impl_->scaled && impl_->keep_native;
    const bool convert_f16 = impl_->compute_recording && impl_->f16_built && !impl_->direct_in;
    const bool compute_input = down_native || convert_f16;
    const VkImageLayout tin_layout = compute_input ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkPipelineStageFlags tin_stage = compute_input ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkAccessFlags tin_access = compute_input ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    if (!ds)
    barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, tin_layout,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, tin_stage, tin_access);
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {frame.width, frame.height, 1};
    VkImageBlit blit{};
    blit.srcSubresource=blit.dstSubresource=region.srcSubresource;
    blit.srcOffsets[1]=blit.dstOffsets[1]={int32_t(frame.width),int32_t(frame.height),1};
    auto& encoded=impl_->encoded;
    // Model Resolution: the frame lands in keep_full at its own size first,
    // then is filtered down into tex_in. Whatever the frame's format, the
    // downscale is one RGBA32F -> RGBA32F blit.
    VkImage colour_dst = s.tex_in.handle;
    if (impl_->scaled && !fr) {
        colour_dst = impl_->keep_full.handle;
        barrier(cmd, impl_->keep_full.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    if (fr) {
        // Nothing to copy: the downscale and the transfer pass read the frame itself.
    } else if (down_native) {
        // The frame's bits as they are (an *_SRGB frame into its UNORM twin).
        vkCmdCopyImage(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       colour_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else if (ds) {
        // Nothing to copy: the pre and post blocks sample the frame itself.
    } else if (impl_->direct_in) {
        // The bits as they are (an *_SRGB frame into its UNORM twin: the same
        // bits the private transfer image held); the kernels' sampling does
        // the conversion the blit used to.
        vkCmdCopyImage(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       colour_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else if(encoded.handle && !(impl_->compute_recording && impl_->f16_built)) {
        barrier(cmd,encoded.handle,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdCopyImage(cmd,frame.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        barrier(cmd,encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        impl_->graphics_only("blit (encoded.handle)");
        vkCmdBlitImage(cmd,encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       colour_dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
        barrier(cmd,encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
    } else if (impl_->transfer == Transfer::DirectBlit || (impl_->compute_recording && impl_->f16_built)) {
        // FP16 / 16-bit UNORM / 10-bit packed (and, compute-converted, the listed 8-bit/sRGB formats): no sRGB transfer function to route
        // around, so the blit's own format conversion is the whole of it.
        if (impl_->compute_recording && impl_->f16_built) {
            auto image = impl_->input_f16.handle;
            barrier(cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdCopyImage(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            const uint32_t size[] = {frame.width, frame.height};
            dispatch(cmd, impl_->unpack_f16, (frame.width + 7) / 8, (frame.height + 7) / 8, 1, size, sizeof size);
            barrier(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        } else {
            impl_->graphics_only("blit (frame.image)");
            vkCmdBlitImage(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           colour_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        }
    } else {
        vkCmdCopyImage(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       colour_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    if (down_native) {
        if (!fr)
            barrier(cmd, impl_->keep_full.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        const uint32_t model[] = {nw, nh};
        dispatch(cmd, impl_->downscale, (nw + 7) / 8, (nh + 7) / 8, 1, model, sizeof model, fr ? fr->down : VK_NULL_HANDLE);
    } else if (impl_->scaled) {
        barrier(cmd, impl_->keep_full.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkImageBlit down{};
        down.srcSubresource = down.dstSubresource = region.srcSubresource;
        down.srcOffsets[1] = {int32_t(frame.width), int32_t(frame.height), 1};
        down.dstOffsets[1] = {int32_t(nw), int32_t(nh), 1};
        impl_->graphics_only("blit (impl_->keep_full.handle)");
        vkCmdBlitImage(cmd, impl_->keep_full.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s.tex_in.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &down, impl_->downscale_filter);
        barrier(cmd, impl_->keep_full.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    if (impl_->linear) {
        // Linear light: make the proxy in place and keep the original. The
        // network reads tex_in in SHADER_READ_ONLY like every other frame.
        barrier(cmd, s.tex_in.handle, tin_layout, VK_IMAGE_LAYOUT_GENERAL, tin_stage, tin_access,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        struct { uint32_t w, h; float white, knee; } push{nw, nh, impl_->white_point, 0.75f};
        if (time_stages) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_pool, stage_query);
        dispatch(cmd, impl_->encode, (nw + 7) / 8, (nh + 7) / 8, 1, &push, sizeof push);
        if (time_stages) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_pool, stage_query + 1);
            stages_written |= 1;
        }
        barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    } else if (!ds) {
        barrier(cmd, s.tex_in.handle, tin_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, tin_stage, tin_access,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    // Preprocess: meter the frame, keep it, and put what the network is to see
    // in tex_in. A change of settings resets the meter and, below, the history.
    const Preprocess& pp = c.preprocess;
    const bool prep_on = impl_->prep && pp.active() && !mask;
    const Preprocess& was = impl_->prep_last;
    if (impl_->prep && (prep_on != impl_->prep_was_on || (prep_on && pp != was))) ++impl_->prep_gen;
    PrepPush prep_push{};
    if (prep_on) {
        const uint32_t curve = uint32_t(std::clamp(pp.curve, 0, 6));
        const bool metered = pp.exposure == 1;
        prep_push = {nw, nh, 0u,
                     (metered ? kPrepAuto : 0u) | (impl_->prep_unknee ? kPrepUnknee : 0u) |
                         (impl_->prep_was_on ? 0u : kPrepReset),
                     curve, pp.exposure == 0 ? 0.0f : std::clamp(pp.bias_ev, -8.0f, 8.0f),
                     std::clamp(pp.contrast, 0.5f, 2.0f), std::clamp(pp.saturation, 0.05f, 2.0f),
                     kPrepAnchor[curve], 4.0f, 0.0f};
        const auto now = std::chrono::steady_clock::now();
        if (impl_->prep_was_on)
            prep_push.dt = std::chrono::duration<float>(now - impl_->prep_metered).count();
        impl_->prep_metered = now;
        barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        if (metered) {
            dispatch(cmd, impl_->prep_k, 1, 1, 1, &prep_push, sizeof prep_push);
            compute_barrier(cmd);
        }
        prep_push.mode = 1;
        dispatch(cmd, impl_->prep_k, (nw + 15) / 16, (nh + 15) / 16, 1, &prep_push, sizeof prep_push);
        barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    impl_->prep_was_on = prep_on;
    impl_->prep_last = pp;
    // A GENERAL -> GENERAL copy between two of our own images, fenced for the
    // compute work on either side. Model-sized, used by the multi-pass loop.
    auto copy_general = [&](const nrvk::Context::Image& src, const nrvk::Context::Image& dst) {
        barrier(cmd, src.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier(cmd, dst.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {src.w, src.h, 1};
        vkCmdCopyImage(cmd, src.handle, VK_IMAGE_LAYOUT_GENERAL, dst.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        barrier(cmd, dst.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        barrier(cmd, src.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    };
    if (impl_->max_passes > 1) {
        // What the first pass sees, for the transfer pass and the alpha
        // restore; tex_in is overwritten by every pass after the first.
        barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier(cmd, impl_->shown_keep.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {nw, nh, 1};
        vkCmdCopyImage(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       impl_->shown_keep.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        barrier(cmd, impl_->shown_keep.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    compute_barrier(cmd);
    auto& t = impl_->temporal;
    // This frame's feature, if the caller named one. Its state is host-side
    // bookkeeping plus one image; nothing is recorded yet.
    Impl::FeatureState* fs = nullptr;
    if (temporal && t.enabled && temporal->feature) {
        auto it = impl_->features.find(temporal->feature);
        if (it == impl_->features.end()) {
            Impl::FeatureState made{};
            // No settling transition: that is a submit and a fence wait, and
            // this is the render thread inside a recording. The barrier below
            // does it in the command buffer instead.
            made.history = s.ctx.image(t.history.w, t.history.h, t.history.format, true, false);
            it = impl_->features.emplace(temporal->feature, made).first;
        }
        fs = &it->second;
    }
    bool gate = false;
    uint32_t pre_seed = 0;
    if (temporal) {
        // The original's gate, all three terms, resolved here and nowhere else:
        // the first-frame latch, DLSSNR.Reset, and a motion source existing at
        // all.
        uint32_t& gen = fs ? fs->prep_gen : impl_->prep_gen_single;
        gate = (fs ? fs->latch : t.latch) && !temporal->reset && gen == impl_->prep_gen;
        gen = impl_->prep_gen;
        // The pre block's noise seed, the DLL's rule: a counter in the pre node
        // (+0xc8) handed to the kernel and then incremented (0x180060f44), zeroed
        // by Reset (0x1800616e0). The first frame and a reset use seed 0, so
        // their noise is the precomputed field's; it moves every frame after.
        uint32_t& counter = fs ? fs->seed : t.seed;
        if (!(fs ? fs->latch : t.latch) || temporal->reset) counter = 0;
        pre_seed = counter++;
        // diagnostics (wrong picture): price the per-frame noise and the history.
        if (std::getenv("NR_DIAG_SEED0")) pre_seed = 0;
        if (std::getenv("NR_DIAG_TGATE0")) gate = false;
        const uint32_t p = fs ? fs->parity : t.parity;
        if (engine) {
            // Engine motion: sampled in place (mv_direct, put back after the
            // passes), else one blit into the estimator's quarter-resolution
            // field, converting whatever format the game handed us.
            if (mv_direct) {
                barrier(cmd, engine->motion.image, engine->motion.before, VK_IMAGE_LAYOUT_GENERAL,
                        engine->motion.before_stage, engine->motion.before_access,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            } else {
            barrier(cmd, engine->motion.image, engine->motion.before,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, engine->motion.before_stage,
                    engine->motion.before_access, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);
            barrier(cmd, t.flow[0].handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageBlit mv{};
            mv.srcSubresource = mv.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            mv.srcOffsets[0] = {int32_t(engine->motion_x), int32_t(engine->motion_y), 0};
            mv.srcOffsets[1] = {int32_t(engine->motion_x + engine->motion.width),
                                int32_t(engine->motion_y + engine->motion.height), 1};
            mv.dstOffsets[1] = {int32_t(t.lw[0]), int32_t(t.lh[0]), 1};
            impl_->graphics_only("blit (engine->motion.image)");
            vkCmdBlitImage(cmd, engine->motion.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           t.flow[0].handle, VK_IMAGE_LAYOUT_GENERAL, 1, &mv, VK_FILTER_LINEAR);
            barrier(cmd, t.flow[0].handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            barrier(cmd, engine->motion.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    engine->motion.after, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, engine->motion.after_stage,
                    engine->motion.after_access);
            }
            // Depth, the same way and for the same reason: one blit into one
            // image, so the consumer has a single descriptor whether or not the
            // game supplies depth this frame.
            impl_->depth_unread = false;
            if (game_depth && depth_direct) {
                // Sampled in place (Impl::DirectSrc); put back after the passes.
                barrier(cmd, engine->depth.image, engine->depth.before, VK_IMAGE_LAYOUT_GENERAL,
                        engine->depth.before_stage, engine->depth.before_access,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            } else if (game_depth && depth_stencil_aspects(engine->depth.format)) {
                // A depth/stencil buffer: sampled through a depth-aspect view
                // (runtime_depth.comp), because a blit may not convert it.
                const VkImageAspectFlags aspects = depth_stencil_aspects(engine->depth.format);
                const bool sampled = !engine->depth.usage || (engine->depth.usage & VK_IMAGE_USAGE_SAMPLED_BIT);
                const VkDescriptorSet set =
                    sampled ? impl_->depth_source(engine->depth.image, engine->depth.format) : VK_NULL_HANDLE;
                if (set) {
                    barrier(cmd, engine->depth.image, engine->depth.before, VK_IMAGE_LAYOUT_GENERAL,
                            engine->depth.before_stage, engine->depth.before_access,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, aspects);
                    barrier(cmd, t.depth.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
                    const uint32_t push[6] = {t.depth.w, t.depth.h, engine->depth.width, engine->depth.height,
                                              engine->depth_x, engine->depth_y};
                    dispatch(cmd, impl_->depth_copy, (t.depth.w + 7) / 8, (t.depth.h + 7) / 8, 1, push,
                             sizeof push, set);
                    barrier(cmd, t.depth.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                    barrier(cmd, engine->depth.image, VK_IMAGE_LAYOUT_GENERAL, engine->depth.after,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                            engine->depth.after_stage, engine->depth.after_access, aspects);
                } else {
                    impl_->depth_unread = true;
                }
            } else if (game_depth) {
                barrier(cmd, engine->depth.image, engine->depth.before,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, engine->depth.before_stage,
                        engine->depth.before_access, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);
                barrier(cmd, t.depth.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkImageBlit d{};
                // A colour-format depth (R32_FLOAT and the like) only: depth/stencil
                // formats took the branch above, since a blit may not convert them.
                d.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                d.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                d.srcOffsets[0] = {int32_t(engine->depth_x), int32_t(engine->depth_y), 0};
                d.srcOffsets[1] = {int32_t(engine->depth_x + engine->depth.width),
                                   int32_t(engine->depth_y + engine->depth.height), 1};
                d.dstOffsets[1] = {int32_t(t.depth.w), int32_t(t.depth.h), 1};
                // NEAREST, not LINEAR: interpolating across a silhouette invents
                // a depth that is on neither surface, which is precisely the
                // edge this tap exists to get right. Vulkan also forbids linear
                // filtering on a depth aspect.
                impl_->graphics_only("blit (engine->depth.image)");
                vkCmdBlitImage(cmd, engine->depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               t.depth.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &d, VK_FILTER_NEAREST);
                barrier(cmd, t.depth.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                barrier(cmd, engine->depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        engine->depth.after, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT, engine->depth.after_stage,
                        engine->depth.after_access);
            }
        }
        for (unsigned k = 0; !engine && k < kTemporalLevels; ++k) {
            Temporal::LumaPush lp{t.lw[k], t.lh[k],
                                  k ? t.lw[k-1] : nw, k ? t.lh[k-1] : nh,
                                  k ? 1u : 0u};
            dispatch(cmd, t.luma_kernel[p][k], (t.lw[k]+7)/8, (t.lh[k]+7)/8, 1, &lp, sizeof lp);
            compute_barrier(cmd);
        }
        if (gate && !engine) {
            for (int k = int(kTemporalLevels) - 1; k >= 0; --k) {
                const bool first = unsigned(k) == kTemporalLevels - 1, last = k == 0;
                Temporal::FlowPush fp{t.lw[k], t.lh[k],
                                      first ? 1u : t.lw[k+1], first ? 1u : t.lh[k+1],
                                      kTemporalRadius[k], first ? 1u : 0u, last ? 1u : 0u,
                                      kTemporalReject};
                dispatch(cmd, t.flow_kernel[p][k], (t.lw[k]+7)/8, (t.lh[k]+7)/8, 1, &fp, sizeof fp);
                compute_barrier(cmd);
            }
        }
        // The estimator writes normalized backward motion, so the consumer's
        // MVecScale pair is 1.0.
        const bool have_depth = engine && engine->depth.image && !impl_->depth_unread && !impl_->motion_without_depth;
        // [8..11]: where the frame's uv lands in the motion texture, uv * [8..9]
        // + [10..11] - the region's share of the allocation and its base, when
        // it is sampled in place; else 1 and 0 (the field covers the frame).
        const uint32_t mtw = engine && engine->motion_texture_width ? engine->motion_texture_width
                             : engine ? engine->motion_x + engine->motion.width : 1;
        const uint32_t mth = engine && engine->motion_texture_height ? engine->motion_texture_height
                             : engine ? engine->motion_y + engine->motion.height : 1;
        const float params[12] = {gate ? 1.0f : 0.0f,
                                  engine ? engine->motion_scale_x : 1.0f,
                                  engine ? engine->motion_scale_y : 1.0f,
                                  t.history_strength * impl_->post_blend_scale,
                                  float(t.history.w), float(t.history.h),
                                  have_depth ? 1.0f : 0.0f,
                                  have_depth && engine->depth_inverted ? 1.0f : 0.0f,
                                  mv_direct ? float(engine->motion.width) / float(mtw) : 1.0f,
                                  mv_direct ? float(engine->motion.height) / float(mth) : 1.0f,
                                  mv_direct ? float(engine->motion_x) / float(mtw) : 0.0f,
                                  mv_direct ? float(engine->motion_y) / float(mth) : 0.0f};
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=t.params.handle;b.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&b,0,nullptr);
        vkCmdUpdateBuffer(cmd,t.params.handle,0,sizeof params,params);
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&b,0,nullptr);
    }
    if (!temporal && !mask) {
        // No motion source: the seed is 0 unless Controls::noise_every_frame, then the DLL's counter.
        if (c.noise_every_frame) pre_seed = impl_->plain_seed++;
        else impl_->plain_seed = 0;
    }
    // Bind this feature's history: copy the previously bound feature's state out
    // and this one's in. Only on a change, so the single-feature case records
    // nothing at all here.
    if (fs && impl_->bound_feature != temporal->feature) {
        impl_->retain_feature_history(*fs);
        if (impl_->bound_feature) {
            auto prev = impl_->features.find(impl_->bound_feature);
            if (prev != impl_->features.end()) {
                impl_->retain_feature_history(prev->second);
                copy_general(t.hist(t.hcur), prev->second.history);
            }
        }
        if (!fs->cleared) {
            // A fresh feature's image is whatever the allocator left there. The
            // gate is closed on its first frame so nothing reads it, but an
            // unwritten float image can hold NaN and a NaN multiplied by a zero
            // gate is still NaN. This is also where it reaches GENERAL, which
            // it will stay in for the rest of its life.
            barrier(cmd, fs->history.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkClearColorValue zero{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, fs->history.handle, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            barrier(cmd, fs->history.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            fs->history.layout = VK_IMAGE_LAYOUT_GENERAL;
            fs->cleared = true;
        }
        copy_general(fs->history, t.hist(t.hcur));
        impl_->bound_feature = temporal->feature;
    }
    // The passes. Pass k > 0 takes pass k-1's answer (surf0, the blended
    // output) as its colour, the same motion and depth, and its own history:
    // store[k] is copied into the bound history image before the pass and
    // the model's write (surf1) copied back after it. With one pass the
    // history round trip is the single copy it always was.
    for (uint32_t pass = 0; pass < passes; ++pass) {
        if (pass > 0 && impl_->cascade_detail) {
            // Detail only: the first pass's input with the previous pass's
            // local structure on it (windows/shaders/passes/cascade_feed.comp).
            barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            struct { uint32_t w, h; float floor_; } lp{nw, nh, 1.0f / 512.0f};
            dispatch(cmd, impl_->cascade_lograt, (nw + 7) / 8, (nh + 7) / 8, 1, &lp, sizeof lp);
            compute_barrier(cmd);
            struct { uint32_t w, h; int32_t radius; uint32_t dir; } bp{nw, nh, impl_->cascade_radius, 0u};
            dispatch(cmd, impl_->cascade_blur_h, (nw + 7) / 8, (nh + 7) / 8, 1, &bp, sizeof bp);
            compute_barrier(cmd);
            bp.dir = 1u;
            dispatch(cmd, impl_->cascade_blur_v, (nw + 7) / 8, (nh + 7) / 8, 1, &bp, sizeof bp);
            compute_barrier(cmd);
            struct { uint32_t w, h; float guard; } fp{nw, nh, 2.0f};
            dispatch(cmd, impl_->cascade_feed, (nw + 7) / 8, (nh + 7) / 8, 1, &fp, sizeof fp);
            barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            compute_barrier(cmd);
            if (temporal) copy_general(impl_->history_store[pass], t.history);
        } else if (pass > 0) {
            barrier(cmd, s.surf0.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy c{};
            c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.extent = {nw, nh, 1};
            vkCmdCopyImage(cmd, s.surf0.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s.tex_in.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            barrier(cmd, s.surf0.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            if (temporal) copy_general(impl_->history_store[pass], t.history);
        }
        if (impl_->network_segments) cmd = impl_->network_segments(cmd, true);
        const bool time_net = timing_active && impl_->split_timing;
        const uint32_t net_query = timing_slot * impl_->timing_stride + 2 + 2 * pass;
        if (time_net) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_pool, net_query);
        for (size_t i = 0; i < s.steps.size(); ++i) {
            const auto& step = s.steps[i];
            const nrvk::Kernel* use = step.k;
            if (mask && step.k==impl_->original_pre) use = &impl_->mask_pre;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (mask && step.k==impl_->original_pre) {}
            else if (!temporal && ds && step.k==impl_->plain_pre) set = ds->pre[0];
            else if (!temporal && ds && step.k==impl_->plain_post) set = ds->post[0];
            else if (temporal && step.k==t.original_pre) {
                use = t.pingpong ? &t.pre_pp[t.hcur] : &t.pre;
                if (sets) set = sets->pre[t.hcur];
            } else if (temporal && step.k==t.original_post) {
                use = t.pingpong ? &t.post_pp[t.hcur] : &t.post;
                if (sets) set = sets->post[t.hcur];
            }
            // Each pass has its own controls; see `pass_push`. An empty entry
            // means that dispatch carries none, so pass 1's push stands.
            const void* push = step.push;
            if (pass > 0 && pass < impl_->pass_push.size() &&
                i < impl_->pass_push[pass].size() && !impl_->pass_push[pass][i].empty())
                push = impl_->pass_push[pass][i].data();
            // A Style in the post block's push (bits 28-29, set only when the grade pipelines exist): its grade
            // twin, over the set the plain one would bind.
            if (impl_->style_grade && step.k == impl_->graph_post) {
                uint32_t w_off = 0;
                std::memcpy(&w_off, static_cast<const uint8_t*>(push) + sizeof(PushFSwin) + sizeof(PushUps), 4);
                if (w_off & 0x30000000u) {
                    if (!set) set = use->set;
                    if (use == impl_->graph_post) use = &impl_->post_grade;
                    else if (use == &t.post) use = &t.post_grade;
                    else if (use == &t.post_pp[0] || use == &t.post_pp[1]) use = &t.post_grade_pp[use - t.post_pp];
                    else throw std::runtime_error("Style grade: no grade pipeline for this post block");
                }
            }
            // This frame's noise seed, computed in the shader: the precomputed
            // field is seed 0's.
            uint8_t pre_push[kPrePushBytes];
            if ((temporal ? step.k == t.original_pre : !mask && step.k == impl_->plain_pre) && pre_seed) {
                if (step.push_bytes != kPrePushBytes)
                    throw std::runtime_error("temporal pre block: unexpected push size");
                const uint32_t no_field = 0;
                std::memcpy(pre_push, push, kPrePushBytes);
                std::memcpy(pre_push + kPreSeedAt, &pre_seed, 4);
                std::memcpy(pre_push + kPreNoiseFieldAt, &no_field, 4);
                push = pre_push;
            }
            dispatch(cmd, *use, step.gx, step.gy, step.gz, push, step.push_bytes, set);
            // Execution-only between network steps when the arena is coherent,
            // invalidate-only when it is not; the barrier after the last step
            // stays full, because the passes that follow it are not part of
            // either contract - they were not built with device-scoped loads
            // and the arena is not the only thing they read.
            const bool last = i + 1 >= s.steps.size();
            // C=512 dispatches joined by tile counters (nr_graph.cpp NR_TCHAIN) have no barrier.
            if (!last && i < s.runner.no_barrier_after.size() && s.runner.no_barrier_after[i]) continue;
            compute_barrier(cmd, s.runner.exec_barrier && !last,
                            s.runner.inv_barrier && !last);
        }
        if (time_net) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_pool, net_query + 1);
        if (impl_->network_segments) cmd = impl_->network_segments(cmd, false);
        if (temporal && !t.pingpong) {
            // History is a copy of what the MODEL wrote - the temporal post variant
            // puts that on the second surface, before the application strength -
            // and it is taken independently of whatever composition follows.
            const nrvk::Context::Image& dst = passes > 1 ? impl_->history_store[pass] : t.history;
            barrier(cmd, s.surf1.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            barrier(cmd, dst.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy history_copy{};
            history_copy.srcSubresource = history_copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            history_copy.extent = {nw, nh, 1};
            vkCmdCopyImage(cmd, s.surf1.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &history_copy);
            barrier(cmd, dst.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            barrier(cmd, s.surf1.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        if (pass == 0 && impl_->first_transfer.device && !prep_on && !mask && c.apply_model) {
            // Before later passes overwrite surf0. Classic enlargement fills up_result
            // from this pass; the final transfer runs the upscale again on the last pass.
            const int enlargement = std::clamp(c.transfer, 0, 2);
            const uint32_t mode = kShaderMode[enlargement];
            const uint32_t scaler = uint32_t(std::clamp(c.classic_scaler, 0, 3));
            const bool enlarge = impl_->scaled && enlargement == kEnlargeClassic && scaler != 0;
            if (enlarge) {
                const uint32_t up[] = {frame.width, frame.height, nw, nh, scaler};
                dispatch(cmd, impl_->upscale_pass, (frame.width + 7) / 8, (frame.height + 7) / 8, 1, up, sizeof up);
                compute_barrier(cmd);
            }
            const TransferPush push{frame.width, frame.height, nw, nh, impl_->linear ? 0u : 1u,
                                    c.detail_strength, c.colour_strength, c.max_ratio, impl_->white_point, mode,
                                    (enlarge ? kTransferEnlarged : 0u) | (c.transfer_guided ? kTransferGuided : 0u),
                                    c.transfer_sigma};
            dispatch(cmd, impl_->first_transfer, (frame.width + 7) / 8, (frame.height + 7) / 8, 1, &push, sizeof push);
            compute_barrier(cmd);
        }
    }
    if (temporal) {
        // Leave the bound history holding the FIRST pass's, which is what the
        // next frame's first pass reads.
        if (passes > 1) copy_general(impl_->history_store[0], t.history);
        // The latch is set after every recorded frame and `reset` never clears
        // it: reset suppresses consumption, it does not un-write the buffer.
        if (fs) { fs->latch = true; fs->parity ^= 1u; }
        else { t.latch = true; t.parity ^= 1u; }
        // The post variant wrote this frame's history into the other image.
        if (t.pingpong) t.hcur ^= 1u;
        if (history_consumed) *history_consumed = gate;
    }
    impl_->check_record(cmd);
    // Retired features, destroyed once enough recordings have gone by that the
    // submission holding their last use cannot still be executing.
    ++impl_->recordings;
    for (size_t i = impl_->retiring.size(); i-- > 0;) {
        if (impl_->recordings < impl_->retiring[i].second) continue;
        s.ctx.destroy(impl_->retiring[i].first);
        impl_->retiring.erase(impl_->retiring.begin() + long(i));
    }
    for (size_t i = impl_->retiring_owned.size(); i-- > 0;) {
        if (impl_->recordings < impl_->retiring_owned[i].second) continue;
        impl_->retiring_owned.erase(impl_->retiring_owned.begin() + long(i));
    }
    info.network_dispatches = uint32_t(s.steps.size()) * passes;
    if (prep_on) {
        // The answer back into the frame's own domain. With later passes the
        // first pass's input is in shown_keep, and the frame goes back there.
        const bool multi = impl_->max_passes > 1;
        if (!multi)
            barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        compute_barrier(cmd);
        prep_push.mode = 2;
        prep_push.flags = (prep_push.flags & ~kPrepReset) | (multi ? kPrepRestore : 0u);
        dispatch(cmd, multi ? impl_->prep_back_k : impl_->prep_k, (nw + 15) / 16, (nh + 15) / 16, 1,
                 &prep_push, sizeof prep_push);
        compute_barrier(cmd);
        if (!multi)
            barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    if (c.apply_model) {
        if (mask) {
            struct { uint32_t w,h;float intensity; } push{frame.width,frame.height,c.intensity};
            dispatch(cmd,impl_->mask_resolve,(frame.width+7)/8,(frame.height+7)/8,1,&push,sizeof push);
            compute_barrier(cmd);
        }
        if (!mask && !impl_->native_compose) {
            // The model's answer onto the frame, as OptiScaler DLSS-NR's
            // resolve; see the shader. At the frame extent; the model-sized
            // answer is sampled.
            const bool prep_variant = prep_on && impl_->transfer_prep.device;
            const int enlargement = std::clamp(c.transfer, 0, 2);
            const uint32_t mode = kShaderMode[enlargement];
            const uint32_t scaler = uint32_t(std::clamp(c.classic_scaler, 0, 3));
            const bool enlarge = impl_->scaled && enlargement == kEnlargeClassic && scaler != 0;
            if (enlarge) {
                const uint32_t up[] = {frame.width, frame.height, nw, nh, scaler};
                dispatch(cmd, prep_variant ? impl_->upscale_prep : impl_->upscale_pass,
                         (frame.width + 7) / 8, (frame.height + 7) / 8, 1, up, sizeof up);
                compute_barrier(cmd);
            }
            const TransferPush push{frame.width, frame.height, nw, nh, impl_->linear ? 0u : 1u,
                                    c.detail_strength, c.colour_strength, c.max_ratio, impl_->white_point, mode,
                                    (enlarge ? kTransferEnlarged : 0u) | (c.transfer_guided ? kTransferGuided : 0u) |
                                        (impl_->store_native && impl_->round_u8 ? kTransferRoundU8 : 0u),
                                    c.transfer_sigma};
            const nrvk::Kernel& transfer = prep_variant ? impl_->transfer_prep : impl_->transfer_pass;
            if (time_stages) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_pool, stage_query + 2);
            dispatch(cmd, transfer, (frame.width + 7) / 8, (frame.height + 7) / 8, 1, &push, sizeof push,
                     fr ? (prep_variant ? fr->transfer_prep : fr->transfer) : VK_NULL_HANDLE);
            if (time_stages) {
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_pool, stage_query + 3);
                stages_written |= 2;
            }
            compute_barrier(cmd);
        }
        const VkImage out = impl_->store_native || impl_->compact_rgba ? impl_->keep_full.handle
                            : impl_->scaled ? impl_->full_out.handle : s.surf0.handle;
        const uint32_t extent[] = {frame.width, frame.height, uint32_t(impl_->round_u8)};
        if ((!impl_->post_alpha || mask) && !impl_->store_native && !impl_->compact_rgba)
            dispatch(cmd, impl_->alpha, (frame.width + 7) / 8, (frame.height + 7) / 8, 1, extent, sizeof extent);
        if (stored) {
            // The post block's store was the write-back.
        } else {
        barrier(cmd, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        if (tgt)
            barrier(cmd, tgt->image, tgt->before, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    tgt->before_stage, tgt->before_access, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        else
        barrier(cmd, frame.image, ds ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                ds ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                ds ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (impl_->direct_out || impl_->store_native) {
            // Already the frame's bits (an *_SRGB frame takes its UNORM twin's).
            vkCmdCopyImage(cmd, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           wb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        } else if(encoded.handle && !((impl_->pack_built || impl_->f16_built || impl_->scaled_pack) && impl_->compute_recording)) {
            barrier(cmd,encoded.handle,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
            impl_->graphics_only("blit (out)");
            vkCmdBlitImage(cmd,out,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
            barrier(cmd,encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdCopyImage(cmd,encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           wb,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
            barrier(cmd,encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
        } else if ((impl_->pack_built || impl_->f16_built || impl_->scaled_pack) && impl_->compute_recording) {
            // Convert to the original format on compute, then copy into the frame.
            const bool own = impl_->f16_built || impl_->scaled_pack;
            const auto packed = own ? impl_->output_f16.handle : impl_->pack_img.handle;
            const auto& pack = own ? impl_->pack_f16 : impl_->pack_u8;
            barrier(cmd, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            barrier(cmd, packed, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            const uint32_t pe[] = {frame.width, frame.height};
            dispatch(cmd, pack, (frame.width + 7) / 8, (frame.height + 7) / 8, 1, pe, sizeof pe);
            barrier(cmd, packed, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdCopyImage(cmd, packed, VK_IMAGE_LAYOUT_GENERAL,
                           wb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrier(cmd, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        } else if (impl_->transfer == Transfer::DirectBlit) {
            impl_->graphics_only("blit (out)");
            vkCmdBlitImage(cmd, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           wb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        } else {
            vkCmdCopyImage(cmd, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           wb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        barrier(cmd, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        info.applied = true;
    }
    if (tgt) {
        barrier(cmd, frame.image, ds ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame.after,
                ds ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                ds ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT, frame.after_stage, frame.after_access);
        if (stored)
            barrier(cmd, tgt->image, VK_IMAGE_LAYOUT_GENERAL, tgt->after, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, tgt->after_stage, tgt->after_access);
        else
        barrier(cmd, tgt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, tgt->after, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, tgt->after_stage, tgt->after_access);
    } else if (stored)
        barrier(cmd, frame.image, VK_IMAGE_LAYOUT_GENERAL, frame.after, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, frame.after_stage, frame.after_access);
    else
    barrier(cmd, frame.image, c.apply_model ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                              : ds ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            frame.after, c.apply_model || !ds ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            c.apply_model ? VK_ACCESS_TRANSFER_WRITE_BIT : ds ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT,
            frame.after_stage, frame.after_access);
    if (depth_direct)
        barrier(cmd, engine->depth.image, VK_IMAGE_LAYOUT_GENERAL, engine->depth.after,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                engine->depth.after_stage, engine->depth.after_access);
    if (mv_direct)
        barrier(cmd, engine->motion.image, VK_IMAGE_LAYOUT_GENERAL, engine->motion.after,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                engine->motion.after_stage, engine->motion.after_access);
    if (engine && temporal)
        impl_->incheck.end(s.ctx, cmd, tgt && (tgt->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ? *tgt : frame, *engine, impl_->temporal.flow[0], impl_->temporal.depth,
                           depth_direct, game_depth && !impl_->depth_unread,
                           history_consumed && *history_consumed, impl_->mw, impl_->mh, mv_direct);
    if (timing_active) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_pool,
                            timing_slot * impl_->timing_stride + 1);
        if (recording_timing) {
            recording_timing->passes = impl_->split_timing ? passes : 0;
            recording_timing->stages = stages_written;
        } else {
            impl_->slot_passes[timing_slot] = impl_->split_timing ? passes : 0;
            impl_->slot_stages[timing_slot] = stages_written;
            ++impl_->timings_recorded;
            impl_->timing_slot = (timing_slot + 1) % Impl::kTimingSlots;
            // Collect whatever has finished. Never waits; the caller is a render
            // thread and this is a diagnostic.
            impl_->read_timing();
        }
    }
    return result;
}

void set_input_check(bool on, const std::string& folder, int pictures) {
    incheck::g_folder = folder;
    incheck::g_dumps_left = pictures;
    incheck::g_on = on;
    if (on) nr::logf("[nr] input check on: every ~600 recordings the engine inputs and the output are read back "
                     "and scored; pictures of the first %d in which the picture moves in %s", pictures,
                     folder.empty() ? "(none)" : folder.c_str());
}

float Runtime::last_gpu_ms() const { return impl_->gpu_ms; }
float Runtime::average_gpu_ms() const { return impl_->gpu_ms_avg; }
float Runtime::average_network_ms() const { return impl_->split_timing ? impl_->net_ms_avg : 0.0f; }
uint64_t Runtime::gpu_timing_serial() const { return impl_->gpu_timing_serial; }
void Runtime::harvest_recording_timings(const std::shared_ptr<void>& resources) {
    if (!resources) return;
    const auto& scope = *std::static_pointer_cast<Impl::RecordingResources>(resources);
    for (const auto& sample : scope.timings) {
        // Retained command lists rewrite their pools on every replay. Remove
        // the recording-time queue entry and read this certified execution.
        auto& pending = impl_->pending_recording_timings;
        pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const auto& entry) {
            const auto owner = entry.lock();
            return !owner || owner == sample;
        }), pending.end());
        ++impl_->recording_timing_reads;
        const auto serial = impl_->gpu_timing_serial;
        impl_->read_timing_sample(sample->pool, 0, sample->passes, sample->stages);
        if (serial == impl_->gpu_timing_serial) ++impl_->recording_timing_rejected;
    }
}
uint64_t Runtime::recording_timing_reads() const { return impl_->recording_timing_reads; }
uint64_t Runtime::recording_timing_rejected() const { return impl_->recording_timing_rejected; }
bool Runtime::recording_timing_available() const { return impl_->recording_timing_available; }
void Runtime::set_stage_timing(bool on) { impl_->stage_timing = on; }
float Runtime::last_encode_ms() const { return impl_->encode_ms; }
float Runtime::last_transfer_ms() const { return impl_->transfer_ms; }
float Runtime::last_network_ms() const { return impl_->network_ms; }
float Runtime::last_other_ms() const { return impl_->other_ms; }
VkImage Runtime::first_pass_output() const { return impl_->first_full.handle; }
VkImage Runtime::first_pass_packed() const { return impl_->first_packed.handle; }
void Runtime::record_first_pack(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    auto& m = *impl_;
    if (!m.first_packed.handle) return;
    barrier(cmd, m.first_full.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    barrier(cmd, m.first_packed.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    const uint32_t extent[] = {w, h};
    dispatch(cmd, m.first_pack, (w + 7) / 8, (h + 7) / 8, 1, extent, sizeof extent);
    barrier(cmd, m.first_packed.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    // WAR: pass 0 is written again by the next recording's network
    barrier(cmd, m.first_full.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
}
std::pair<float, float> Runtime::preprocess_meter() const {
    const float* s = static_cast<const float*>(impl_->prep_state.mapped);
    if (!s || s[1] != 1.0f) return {NAN, NAN};
    return {s[0], s[3]};
}
}  // namespace nr
