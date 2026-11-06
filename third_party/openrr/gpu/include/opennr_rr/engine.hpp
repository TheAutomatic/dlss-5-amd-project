// SPDX-License-Identifier: MIT
// One rr2 frame on the GPU: the 26 launches of preset F (pooled_history2, auto exposure, enc0..enc5, dec4..dec0,
// post, downsample) with every recurrent state kept here (ping-pong by frame parity) and every launch parameter
// derived from the frame's inputs (FASE4.md section 3). Records into the Device's current command buffer.
#pragma once
#include "opennr_rr/kernels.hpp"
#include "opennr_rr/weights.hpp"

#include <array>
#include <vector>

namespace onr {

struct EngineFrame {
    I2 render{}, output{};
    float jitter[2] = {};       // render pixels
    float mv_scale[2] = {1, 1}; // raw motion vector -> render pixels
    float world_to_view[16] = {};
    float view_to_clip[16] = {};
    float depth_sign = 1;       // the depth guide holds view z * depth_sign (linear, positive ahead)
    bool reset = false;
    uint64_t transparency = 0;   // the transparency layer, rgba16f at the render size; 0 = none
    // DLSS.DisocclusionMask and the SSS guide (or ColorBeforeFog), r32f at the render size; 0 = none (rr2_enc0 slots 336 / 288)
    uint64_t disocclusion = 0, sss = 0;
    // more of rr2_enc0's optional guides, at the render size; 0 = none (docs/NOTA-ENTRADAS.md): DLSSD.ResponsivityMask
    // (r32f, slot 352), DLSSD.ColorBeforeTransparency (rgba16f, slot 320), DLSSD.DepthOfFieldGuide (r32f, slot 304),
    // GBuffer.SpecularMvec (rg16f raw, slot 208); no_hit: no specular hit distance (slot 272 null)
    uint64_t responsivity = 0, color_before_transparency = 0, depth_of_field = 0, specular_motion = 0;
    bool no_hit = false;
    // the downsample's: DLSS.TransparencyLayerOpacity (rgba16f, render size) and DLSS.TransparencyLayerMvecs (rg16f raw,
    // read at the output pixel: rows of transparency_motion_size.x, at most the output size)
    uint64_t transparency_opacity = 0, transparency_motion = 0;
    I2 transparency_motion_size{};
};

// device addresses of the frame's guides at the render size, tightly packed in the kernels' formats
struct Guides {
    uint64_t color = 0;     // rgba16f
    uint64_t albedo = 0;    // rgba8 unorm
    uint64_t spec = 0;      // rgba8 unorm
    uint64_t normals = 0;   // rgba16f
    uint64_t rough = 0;     // r16f
    uint64_t depth = 0;     // r32f, linear
    uint64_t mv = 0;        // rg16f, raw (mv_scale applies)
    uint64_t hit = 0;       // r16f
    // D3D12 (fast, d3d_enc0_attr_f / d3d_ae loaded): the guides read where the game left them through this table, with
    // the ingest's conversions (onr_guides.glsl); the addresses above are then unused
    const struct GuideTable* direct = nullptr;
};

// ingest.comp's words per guide (view, pitch, format | kind << 8 | flags << 16 | channel << 24, origin), the projection
// of the hardware depth and its sign, the guide buffers (read for the guides that are not views: copied ones, ingested)
struct GuideTable {
    uint32_t g[8][4];
    float proj[4];
    float depth_sign;
    uint32_t pad;
    uint64_t buf[8];
};

// The 1088-byte rr2_enc0 parameter block, the fields the kernels read (FASE4.md section 3).
struct Enc0BlockInput {
    I2 render, internal, ds;
    float jitter[2], prev_jitter[2], mv_scale[2];
    const float* w2v;
    const float* v2c;
    const float* prev_w2v;
    const float* prev_v2c;
    const float* scalars;   // Weights::enc0_scalars
    float depth_sign = 1;
};
std::array<uint32_t, 272> enc0_block(const Enc0BlockInput& in);

// What the post sees of an image edited between dec0_1 and the post (FASE5-NR.md).
enum class DfnEdit {
    none,      // the network's image
    loop,      // the edited image replaces it: history and display take the edit
    display,   // the network keeps its own image; the display adds the edit as a log delta with a history of its own
};

// The edited image the host hands back: tf_read format code (onr_texel.glsl), rows of pitch bytes, DFN / exposure.
struct DfnImport {
    uint64_t src = 0;
    uint32_t pitch = 0, fmt = 0;
};

// fp16 Q/V weights followed by their OCP e4m3 codes (the in_a / in_b buffers of the network kernels)
std::vector<uint8_t> qv_bytes(const uint16_t* v, size_t n);
// row-major [n / K][K] weights followed by the same values in 16x16 tiles (wout, fc1, fc2 of the network kernels)
std::vector<uint16_t> with_tiles(const std::vector<uint16_t>& v, size_t K);

class Engine {
public:
    void init(Gpu& dev, const Kernels& k, const Weights& w);
    void destroy();
    void set_fast(bool fast) { fast_ = fast; }
    void set_kernels(const Kernels* k, bool fast) { k_ = k, fast_ = fast; }
    const Kernels* kernels() const { return k_; }
    bool fast() const { return fast_; }
    // resize() sizes some buffers for the mode (the fast record, the faithful post's fixup list)
    bool sized_for_mode() const { return sized_fast_ == fast_; }
    // (re)allocates everything sized by the frame (outside a recording: it uploads); the history starts over
    void resize(I2 render, I2 output, I2 max_render = {});
    I2 render() const { return render_; }
    I2 output() const { return output_; }
    // out: rgba16f, output.x * output.y
    // D3D12: the downsample writes the output texture (descriptor 16 of set 0) through the d3d_ variants when they
    // are loaded; out is then the origin x | y << 16
    void set_output_image(bool on) { out_image_ = on; }
    bool output_image_ok() const { return k_ && k_->has("d3d_downsample") && k_->has("d3d_downsample_tl"); }
    // D3D12: the exposure and enc0_attr read the guides from the game's textures (Guides::direct)
    bool direct_guides_ok() const { return k_ && fast_ && k_->has("d3d_enc0_attr_f") && k_->has("d3d_ae"); }
    // the 64-byte record kernels, for attributes 32 / 33 (a depth of field guide, a responsivity mask)
    bool wide_attr_ok() const { return k_ && fast_ && k_->has("enc0_attr_f_w") && k_->has("enc0_stage_32_64_f_w"); }
    void record(const EngineFrame& f, const Guides& g, uint64_t out);
    // record() in two halves, for an edit of the denoised image in between. record_net stops after dec0_1 and, with
    // dfn_out, writes DFN / exposure there as rgba32f (dfn_half: rgba16f) rows of dfn_pitch bytes; record_post takes the
    // same frame. edit: the one planned for record_post (a loop edit moves the post tile out of the dec0_1 head into
    // record_post).
    void record_net(const EngineFrame& f, const Guides& g, uint64_t dfn_out = 0, uint32_t dfn_pitch = 0, DfnEdit edit = DfnEdit::none,
                    bool dfn_half = false);
    void record_post(const EngineFrame& f, uint64_t out, DfnEdit edit = DfnEdit::none, const DfnImport& in = {});
    // the InternalHistory and DownsampledHistory the last recorded frame wrote (tests)
    const Buffer& history() const { return hist_[(frame_ + 1) & 1]; }
    const Buffer& downsampled_history() const { return down_[(frame_ + 1) & 1]; }
    // the display's delta history (DfnEdit::display) is created on the first display frame and kept while there is one;
    // drop it while nothing recorded is in flight. After a gap it starts from that frame's delta.
    bool display_history() const { return dh_[0].addr != 0; }
    void drop_display_history();
    // the transparency layer's history: made on the layer's first frame, started over after a frame without it
    void drop_transparency_history();
    const Buffer& display_history_buf() const { return dh_[(frame_ + 1) & 1]; }
    // how fast the delta history follows this frame's delta (at least; the display gate a2 where it is larger)
    void set_delta_alpha(float a) { dh_alpha_ = a; }
    // the fast post's history taps on the sampler where the device has it (default); off: its hand blend, as on D3D12
    void set_history_sampler(bool on) { sampler_ = on; }
    // the kernel names one mode needs
    static std::vector<std::string> kernel_names(const Weights& w, bool fast);
    // the small levels without barriers between them (optional: without them every level keeps its barrier)
    static std::vector<std::string> flow_kernel_names();
    // off: the small levels keep their barriers even with the flow kernels loaded (their row flags spin-wait)
    void set_flow(bool on) { flow_on_ = on; }
    uint64_t frames() const { return frame_; }
    // the optional cheaper kernels (docs/FASE24-FASTMODE.md; "fast" alone means the _f kernels here): same buffers and
    // layouts as the _f ones, so the switch takes effect on the next record_net with every history kept. k: built
    // beforehand, kernels under their _f names, those missing there stay on the _f ones
    void set_lite(const Kernels* k) { lk_ = k; }
    void set_lite_on(bool on) { lite_on_ = on; }
    bool lite_active() const { return frame_lite_; }

private:
    struct StageBufs { Buffer in_a, in_b, out, out_b, fc1, fc1_b, fc2, fc2_b, bias, dfn_w, dfn_b, mrg_w, mrg_b, up_w, up_b; };
    Buffer make(size_t bytes);
    bool out_image_ = false;
    Buffer upload(const H16& v);
    Buffer upload_qv(const H16& v, const H16& lr);
    void upload_stage(const StageWeights& s, StageBufs& b);
    EncBlockPC block_pc(const StageWeights& s, const StageBufs& b, uint64_t x, uint64_t y, I2 size, I2 shift) const;
    std::string sfx() const { return fast_ ? "_f" : ""; }
    void run(Gpu& d, const std::string& n, const void* pc, uint32_t size, uint32_t groups) const {
        (frame_lite_ && lk_->has(n) ? *lk_ : *k_).run(d, n, pc, size, groups);
    }

    Gpu* dev_ = nullptr;
    const Kernels* k_ = nullptr;
    const Kernels* lk_ = nullptr;
    bool lite_on_ = false, frame_lite_ = false;   // frame_lite_: this frame's, taken in record_net
    const Weights* w_ = nullptr;
    bool fast_ = true, sized_fast_ = true, flow_on_ = true;
    std::vector<Buffer> fixed_, sized_;
    StageBufs enc_[6], dec0_[5], dec1_[5];
    Buffer embed_w_, embed_b_, lut_, masks_, tab_, bicubic_, gtab_;
    Buffer flow_;   // row flags of the _fl kernels (onr_flow.glsl): 10 slots x 512 words
    I2 render_{}, output_{}, alloc_render_{}, alloc_output_{}, internal_{}, ds_{}, ds2_{}, pad_{};
    uint64_t frame_ = 0;
    uint64_t nr_frame_ = ~0ull;   // frame_ of the last post that wrote the delta history
    float dh_alpha_ = 0.05f;
    bool fresh_ = true;
    bool tile_late_ = false;   // record_post builds the post tile (post_tile) instead of the dec0_1 head
    bool sampler_ = true;
    float prev_jitter_[2] = {};
    float prev_w2v_[16] = {}, prev_v2c_[16] = {};

    // per-size buffers
    struct Level { Buffer tensor, dfn, pooled, dfn_pool; };
    Level enc_l_[6], dec0_l_[5], dec1_l_[5];
    Buffer dh_[2];
    Buffer tlh_[2];          // the transparency layer's history (output size, rgba16f), made on its first frame
    bool tl_last_ = false;   // the previous frame had the layer
    Buffer toh_[2];          // the layer opacity's history (output size, rgba16f), made on its first frame
    bool to_last_ = false;   // the previous frame had the layer and its opacity
    // InternalHistory as linear images over hist_ (texture slots 0 and 1) for the fast post's sampler taps; empty when the
    // driver lays them out otherwise
    Image hist_img_[2];
    VkSampler hist_smp_ = VK_NULL_HANDLE;
    Buffer attr_, img_, ero_, nond_, pmv2_[2], hidden_[2], down_[2], hist_[2], expo_[2];
    Buffer pdst_, acc_, blk_, tile_, fix_, frg_, fb_, ajab_, mvf_, aniso_, prm_;
    Buffer attr_wide_;   // the 64-byte record (enc0_attr_f_w), made on the first frame that needs it
};

}  // namespace onr
