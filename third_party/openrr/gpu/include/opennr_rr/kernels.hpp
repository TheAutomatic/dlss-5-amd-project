// SPDX-License-Identifier: MIT
// Push-constant blocks of the rr2 kernels (must match gpu/shaders/*.comp, scalar layout) and the
// recording helpers. Pointers are buffer device addresses.
#pragma once
#include "opennr_rr/vk.hpp"

#include <map>
#include <string>
#include <vector>

namespace onr {

struct I2 { int32_t x, y; };

struct ClearRangePC { uint64_t ptr; uint32_t count; };
struct PooledHistory2PC { uint64_t src, dst; I2 in_size, out_size; };
struct AeSumPC { uint64_t color, acc; I2 size, origin, maxv; };
struct AeFinalPC { uint64_t acc, next_acc, exposure, prev, block; float inv_count; uint32_t reset, next_words; };
// ae: ae_sum + ae_final in one dispatch (the last block); acc is +0 on entry and left +0
struct AePC { uint64_t color, acc; I2 size, origin, maxv; uint64_t exposure, prev, block; float inv_count; uint32_t reset, blocks; };
struct DownsamplePC { uint64_t rg, b, dst, exposure; I2 in_size, out_size, out_origin, out_end, slice, slice_x; };
// downsample_tl (rr2_downsample_kernel_static_hdr): the transparency layer composited over the output
struct DownsampleTlPC {
    DownsamplePC d;
    uint64_t tl, pmv, hist_in, hist_out;
    I2 lr_size;
    float scale[2], offset[2], offset_out[2], mv_scale[2];
    float weight;
    uint32_t reset;
    // the layer's own motion vectors (rg16f, rows of tlmv_size.x, 0 = none) and its opacity (rgba16f, render size) with
    // the opacity's history (downsample_tl.comp)
    uint64_t tlmv = 0;
    I2 tlmv_size{};
    float tlmv_scale[2] = {};
    uint64_t op = 0, op_hist_in = 0, op_hist_out = 0;
    uint32_t op_reset = 0;
};
struct PostTilePC { uint64_t dfn, ajab, aniso, tile; I2 lr_size; int32_t dfn_stride; };
struct DfnExportPC { uint64_t dfn, exposure, dst; I2 size; int32_t dfn_stride; uint32_t pitch, half_out; };
struct DfnImportPC { uint64_t src, exposure, dfn, tile; I2 size; int32_t dfn_stride; uint32_t pitch, fmt, to_tile; };
struct PostPC {
    uint64_t tile, mvf, hist_in, mv, mv_eroded, mv_nondil, exposure, bicubic;
    uint64_t hist_out, out_rg, out_b, down, fix;
    float gate_scale; float scale[2]; float offset[2]; float inv_size[2]; float lr_inv_size[2];
    I2 size, lr_size, origin, end, slice;
    uint32_t reset;
};
// post_dlt: the display's delta history (reset bit 2: it starts from this frame's delta; bit 3: no edit this frame)
struct PostDltPC {
    PostPC p;
    uint64_t dh_in, dh_out;
    float dh_alpha;
};
struct EncBlockPC {
    uint64_t x, y, in_a, in_b, wout, out_b, fc1, fc1_b, fc2, fc2_b, bias;
    I2 size, shift;
    uint32_t ls;
    int32_t nwx;
    uint64_t mask;   // k8 variant: two 16x16 fp16 A masks (K 0..7, K 8..15)
};
// enc_stage_<C>_<X>_<Co>: EncBlockPC followed by the stage outputs (k8 path)
struct EncStagePC {
    EncBlockPC b;
    uint64_t dfn_w, dfn_b, mrg_w, mrg_b, dfn_in, dfn_out, dfn_pool, pooled;   // enc1: mrg_* = merge_out then merge_in
    uint32_t s_dfn;
    I2 in_size;   // enc1: size of the enc0 tensor
};
// enc0_stage_32_64 (rr2_enc0, network half): EncStagePC with mrg_w = embed fc1 [64][64] then fc2 [32][64],
// mrg_b = fc1_b [64] then fc2_b [32]; no PooledOutput
struct Enc0StagePC {
    EncStagePC s;
    uint64_t attr, lut;
    uint32_t frame8;
    int32_t img_h;
    int32_t img_w;
};
// enc0_attr (rr2_enc0, per-pixel half): attributes, DFN image, motion vector surfaces
struct Enc0AttrPC {
    uint64_t color, albedo, spec, normals, rough, depth, mv, hit, pm1, hidden, ds, ds2, expo, prm, tab;
    uint64_t attr, pmv2, ero, nond, dfn;
    I2 size, ds_size, ds2_size;
    uint32_t hist_off;
    I2 pad, hist_size;
    uint64_t disocc, sss;   // optional r32f guides, 0 = none
    uint64_t lut;           // fast variant: the dither's LUT and frame % 8 (the record is dithered, ATTR64)
    uint32_t frame8;
};
// dec_up_<C>_<X>_<Cp> (rr2_decN_kernel_0): b.x is the skip tensor, xlo the tensor of the level below
struct DecUpPC {
    EncBlockPC b;
    uint64_t xlo, up_w, up_b;
    I2 lo_size;
    uint32_t s_skip;
    int32_t live_h;   // fast dec0_0: rows of the TensorOutput that dec0_1 reads (0: all)
};
// dec_dfn_<C>_<X> (rr2_decN_kernel_1) and dec_head_32_64 (rr2_dec0_kernel_1, no tensor output)
struct DecDfnPC {
    EncBlockPC b;
    uint64_t dfn_w, dfn_b, below, skip, dfn_out;
    I2 lo_size;
    uint32_t s_dfn;
    int32_t out_h;
    uint64_t ajab, mvf, hidden, aniso;
    float k_scale, k_tanh;
    int32_t out_w;   // dec0_1: real width of the head surfaces
    uint64_t tile;   // dec0_1 (k8 / _f kernels): writes the post tile too when non-zero (post_tile fused)
};
static_assert(sizeof(DecDfnPC) <= 256 && sizeof(EncStagePC) <= 256, "push constants are limited to 256 bytes");
struct EncQvFp8PC { uint64_t x, qv, in_a, in_b; I2 size, shift; int32_t nwx; };
static_assert(sizeof(PostDltPC) <= 256, "push constants are limited to 256 bytes");

inline uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
inline size_t post_fix_bytes(I2 size) { return 16 + size_t((size.x + 1) / 2) * ((size.y + 1) / 2) * 8; }

class Kernels {
public:
    void load(Gpu& dev, const std::string& spv_dir);
    // only the named kernels (<dir>/<name>.spv); throws when one is missing
    void load(Gpu& dev, const std::string& spv_dir, const std::vector<std::string>& names);
    // the named kernels that have a <name>_lt.spv, each under its plain name (Engine::set_lite)
    void load_lite(Gpu& dev, const std::string& spv_dir, const std::vector<std::string>& names);
    std::vector<std::string> names() const;
    void unload(Gpu& dev);
    const Kernel& operator[](const std::string& n) const { return k_.at(n); }

    void clear_range(Gpu& d, const ClearRangePC& p) const;
    void pooled_history2(Gpu& d, const PooledHistory2PC& p) const;
    void auto_exposure(Gpu& d, const AeSumPC& s, const AeFinalPC& f, I2 grid) const;
    void downsample(Gpu& d, const DownsamplePC& p) const;
    void post_tile(Gpu& d, const PostTilePC& p) const;
    // main pass, then the exact recomputation of the listed 2x2 blocks; fix: post_fix_bytes(size). fast: post_f only.
    void post(Gpu& d, const PostPC& p, const Buffer& fix, bool fast = false) const;
    // the same with the display's delta history (post_dlt / post_dlt_f, post_fixup_dlt)
    void post_dlt(Gpu& d, const PostDltPC& p, const Buffer& fix, bool fast = false) const;
    // enc_block_<C>_<X>, or enc_block_<C>_<X>_k8 (fp16 accumulation in 8-K steps, as the reference HMMA)
    void enc_block(Gpu& d, const EncBlockPC& p, uint32_t windows, int C, int X, bool k8 = false) const;
    void enc_qv_fp8(Gpu& d, const EncQvFp8PC& p, uint32_t windows) const;
    void enc_stage(Gpu& d, const EncStagePC& p, uint32_t windows, int C, int X, int Co) const;
    bool has(const std::string& n) const { return k_.count(n) != 0; }
    void run(Gpu& d, const std::string& n, const void* pc, uint32_t size, uint32_t groups) const;

private:
    std::map<std::string, Kernel> k_;
};

}  // namespace onr
