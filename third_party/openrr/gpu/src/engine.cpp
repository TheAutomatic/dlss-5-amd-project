// SPDX-License-Identifier: MIT
#include "opennr_rr/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace onr {

namespace {

constexpr uint32_t kBlockBytes = 1088;

uint32_t bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

// 4x4 row-major products and inverse (row vectors: v' = v M)
void mul(const double* a, const double* b, double* r) {
    double t[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
            t[i * 4 + j] = s;
        }
    memcpy(r, t, sizeof t);
}

bool invert(const double* m, double* r) {
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int i = c + 1; i < 4; ++i)
            if (std::fabs(a[i][c]) > std::fabs(a[p][c])) p = i;
        if (std::fabs(a[p][c]) < 1e-30) return false;
        if (p != c)
            for (int j = 0; j < 8; ++j) std::swap(a[p][j], a[c][j]);
        double d = a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int i = 0; i < 4; ++i)
            if (i != c) {
                double f = a[i][c];
                for (int j = 0; j < 8; ++j) a[i][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r[i * 4 + j] = a[i][j + 4];
    return true;
}

void to_d(const float* f, double* d) {
    for (int i = 0; i < 16; ++i) d[i] = f[i];
}

bool zero_matrix(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (m[i] != 0) return false;
    return true;
}

// bicubic phase table of rr2_post (FASE3C section 7): Catmull-Rom least squares with the middle tap's offset
// quantised to 8 bits
std::vector<float> bicubic_table() {
    std::vector<float> t(513 * 4);
    for (int k = 0; k <= 512; ++k) {
        double f = k / 512.0;
        double c0 = f * (-0.5 + f * (1 - f / 2)), c1 = 1 + f * f * (-2.5 + 1.5 * f), c2 = f * (0.5 + f * (2 - 1.5 * f)),
               c3 = f * f * (-0.5 + f / 2);
        // oi = floor(256 c2 / (c1 + c2) + 1/2) in exact rationals: c1 + c2 > 0 and the products are exact in fp64
        int oi = int(std::floor(c2 / (c1 + c2) * 256 + 0.5));
        double r[4];
        if (k == 512) r[0] = 0, r[1] = 1, r[2] = 0, r[3] = 1;
        else if (oi == 0 || oi == 256) r[0] = 0, r[1] = 1, r[2] = 0, r[3] = std::min(oi, 255) / 256.0;
        else {
            double o = oi / 256.0, q = (1 - o) * (1 - o) + o * o, p = c1 * (1 - o) + c2 * o;
            double lam = (c1 + c2 - p / q) / (2 + 1 / q);
            r[0] = c0 + lam, r[1] = (lam + p) / q, r[2] = c3 + lam, r[3] = o;
        }
        for (int i = 0; i < 4; ++i) t[k * 4 + i] = float(r[i]);
    }
    return t;
}

// f32 texel values as the reference reads them: unorm10, snorm8 by byte, unorm8
std::vector<float> value_tables() {
    std::vector<float> t(1536);
    for (int i = 0; i < 1024; ++i) t[i] = float(i) / 1023.0f;
    for (int i = 0; i < 256; ++i) t[1024 + i] = std::max(float(int8_t(uint8_t(i))) / 127.0f, -1.0f);
    for (int i = 0; i < 256; ++i) t[1280 + i] = float(i) / 255.0f;
    return t;
}

I2 half_up(I2 s) { return {(s.x + 1) / 2, (s.y + 1) / 2}; }
I2 shr(I2 s, int l) { return {s.x >> l, s.y >> l}; }
uint32_t windows(const EncBlockPC& b) { return uint32_t(b.nwx * ((b.size.y + b.shift.y + 7) / 8)); }

}  // namespace

std::array<uint32_t, 272> enc0_block(const Enc0BlockInput& in) {
    std::array<uint32_t, 272> b{};
    auto F = [&](uint32_t cb, float v) { b[(cb - 0x160) / 4] = bits(v); };
    auto I = [&](uint32_t cb, int32_t v) { b[(cb - 0x160) / 4] = uint32_t(v); };
    float rx = float(in.render.x), ry = float(in.render.y), ix = float(in.internal.x), iy = float(in.internal.y);
    F(0x2e8, in.prev_jitter[0]), F(0x2ec, in.prev_jitter[1]);
    F(0x2f0, in.jitter[0]), F(0x2f4, in.jitter[1]);
    F(0x2f8, rx / ix), F(0x2fc, ry / iy);
    F(0x300, ix / rx), F(0x304, iy / ry);
    F(0x308, 1.0f / rx), F(0x30c, 1.0f / ry);
    F(0x310, 1.0f / ix), F(0x314, 1.0f / iy);
    F(0x318, in.prev_jitter[0] / rx), F(0x31c, in.prev_jitter[1] / ry);
    F(0x320, in.mv_scale[0] * (ix / rx)), F(0x324, in.mv_scale[1] * (iy / ry));
    F(0x328, 0), F(0x32c, 0);
    I(0x350, in.ds.x), I(0x354, in.ds.y);
    F(0x358, 1.0f / ix), F(0x35c, 1.0f / iy);
    F(0x360, 1.0f / float(in.ds.x)), F(0x364, 1.0f / float(in.ds.y));
    for (int i = 0; i < 4; ++i) F(0x39c + 4 * i, in.scalars[i]);
    // p588 / p592: the responsivity mask's scale and bias (the captures' values)
    F(0x3ac, 1.0f), F(0x3b0, 0.0f);
    F(0x590, 1.0f);

    double w2v[16], v2c[16], pw2v[16], pv2c[16], iw2v[16], iv2c[16], E[16], Fm[16];
    to_d(in.w2v, w2v), to_d(in.v2c, v2c), to_d(in.prev_w2v, pw2v), to_d(in.prev_v2c, pv2c);
    if (!invert(w2v, iw2v)) memset(iw2v, 0, sizeof iw2v), iw2v[0] = iw2v[5] = iw2v[10] = iw2v[15] = 1;
    if (!invert(v2c, iv2c)) memset(iv2c, 0, sizeof iv2c), iv2c[0] = iv2c[5] = iv2c[10] = iv2c[15] = 1;
    // camera position (world), the current projection, pixel -> world and world -> previous pixel
    F(0x3d8, float(iw2v[12])), F(0x3dc, float(iw2v[13])), F(0x3e0, float(iw2v[14]));
    // the projection as the depth guide reads it (row 2 takes view z)
    for (int i = 0; i < 16; ++i) F(0x424 + 4 * i, float(v2c[i] * (i / 4 == 2 ? in.depth_sign : 1.0)));
    const double pix2ndc[16] = {2 / double(rx), 0, 0, 0, 0, -2 / double(ry), 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
    mul(pix2ndc, iv2c, E);
    mul(E, iw2v, E);
    for (int i = 0; i < 16; ++i) F(0x4e4 + 4 * i, float(E[i]));
    const double ndc2pix[16] = {rx / 2.0, 0, 0, 0, 0, -ry / 2.0, 0, 0, 0, 0, 1, 0, rx / 2.0, ry / 2.0, 0, 1};
    mul(pw2v, pv2c, Fm);
    mul(Fm, ndc2pix, Fm);
    for (int i = 0; i < 16; ++i) F(0x524 + 4 * i, float(Fm[i]));
    return b;
}

Buffer Engine::make(size_t bytes) {
    Buffer b = dev_->create(bytes + 64);
    dev_->fill(b, 0);
    return b;
}

namespace {
thread_local std::vector<uint8_t>* t_pack_blob = nullptr;
thread_local std::vector<Buffer*>* t_pack_refs = nullptr;

Buffer pack_bytes(Gpu* dev, std::vector<Buffer>& fixed, const void* data, size_t bytes, Buffer* slot = nullptr) {
    if (!bytes) return {};
    if (t_pack_blob && slot) {
        const size_t off = (t_pack_blob->size() + 255) & ~size_t(255);
        t_pack_blob->resize(off + bytes, 0);
        std::memcpy(t_pack_blob->data() + off, data, bytes);
        slot->addr = off;
        slot->size = bytes;
        t_pack_refs->push_back(slot);
        return *slot;
    }
    Buffer b = dev->create(bytes);
    dev->upload(b, data, bytes);
    fixed.push_back(b);
    return b;
}
}  // namespace

Buffer Engine::upload(const H16& v) {
    if (v.empty()) return {};
    return pack_bytes(dev_, fixed_, v.data(), v.size() * 2);
}

// in_a / in_b: the fp16 weights, then the same values as OCP e4m3 codes (they are exact e4m3) for the FP8 Q/V
// products of the fast kernels
std::vector<uint8_t> qv_bytes(const uint16_t* v, size_t n) {
    std::vector<uint8_t> d(n * 3);
    std::memcpy(d.data(), v, n * 2);
    for (size_t i = 0; i < n; ++i) {
        uint32_t h = v[i], e = (h >> 10) & 31, m = h & 1023, c = 0;
        if (e >= 9) {
            if ((m & 127) || e > 23) throw std::runtime_error("Q/V weight is not an e4m3 value");
            c = ((e - 8) << 3) | (m >> 7);
        } else if (e || m) {
            uint32_t k = 1024 + m, sh = 16 - e;   // value k 2^(e - 25) = (k >> sh) 2^-9
            if (!e || (k & ((1u << sh) - 1))) throw std::runtime_error("Q/V weight is not an e4m3 value");
            c = k >> sh;
        }
        d[n * 2 + i] = uint8_t(c | ((h >> 8) & 0x80));
    }
    return d;
}

std::vector<uint16_t> with_tiles(const std::vector<uint16_t>& v, size_t K) {
    size_t n = v.size();
    if (K % 16 || n % (16 * K)) throw std::runtime_error("weight matrix not in 16x16 tiles");
    std::vector<uint16_t> d(n * 2);
    std::copy(v.begin(), v.end(), d.begin());
    for (size_t r = 0; r < n / K; ++r)
        for (size_t k = 0; k < K; ++k) d[n + (r / 16) * 16 * K + (k / 16) * 256 + (r % 16) * 16 + k % 16] = v[r * K + k];
    return d;
}

// S_h = Q_h Q_h^T + bias depends on in_a only through A^T A, so rotating the rows of A_h changes no score. Rows made
// orthogonal (Hestenes) and sorted by falling norm: at C = 32 the 16 strongest hold 98.6-99.8% of the energy, and the fast
// mode's kernels (LT_Q16) use only those
static H16 lr_keys(const H16& a, int C, int X) {
    H16 o(a.size());
    for (int h = 0; h < X / 32; ++h) {
        double m[32][32];
        for (int r = 0; r < 32; ++r)
            for (int c = 0; c < 32; ++c) m[r][c] = h2f(a[size_t(h) * 32 * C + r * C + c]);
        for (int sweep = 0; sweep < 40; ++sweep) {
            bool any = false;
            for (int p = 0; p < 32; ++p)
                for (int q = p + 1; q < 32; ++q) {
                    double aa = 0, bb = 0, ab = 0;
                    for (int i = 0; i < 32; ++i) aa += m[p][i] * m[p][i], bb += m[q][i] * m[q][i], ab += m[p][i] * m[q][i];
                    if (ab == 0 || std::fabs(ab) <= 1e-13 * std::sqrt(aa * bb)) continue;
                    any = true;
                    double z = (bb - aa) / (2 * ab), t = (z >= 0 ? 1 : -1) / (std::fabs(z) + std::sqrt(1 + z * z)), co = 1 / std::sqrt(1 + t * t), si = co * t;
                    for (int i = 0; i < 32; ++i) { double x = m[p][i], y = m[q][i]; m[p][i] = co * x - si * y, m[q][i] = si * x + co * y; }
                }
            if (!any) break;
        }
        int ord[32];
        double nrm[32];
        for (int r = 0; r < 32; ++r) {
            ord[r] = r, nrm[r] = 0;
            for (int i = 0; i < 32; ++i) nrm[r] += m[r][i] * m[r][i];
        }
        std::stable_sort(ord, ord + 32, [&](int x, int y) { return nrm[x] > nrm[y]; });
        for (int r = 0; r < 32; ++r)
            for (int c = 0; c < 32; ++c) o[size_t(h) * 32 * C + r * C + c] = f2h(float(m[ord[r]][c]));
    }
    return o;
}

Buffer Engine::upload_qv(const H16& v, const H16& lr) {
    if (v.empty()) return {};
    auto d = qv_bytes(v.data(), v.size());
    d.resize(v.size() * 3 + lr.size() * 2);   // LT_Q16 reads lr at 3 n bytes; the regular kernels never do
    std::memcpy(d.data() + v.size() * 3, lr.data(), lr.size() * 2);
    return pack_bytes(dev_, fixed_, d.data(), d.size());
}

void Engine::upload_stage(const StageWeights& s, StageBufs& b) {
    auto U = [&](Buffer& dst, const H16& v) { if (!v.empty()) pack_bytes(dev_, fixed_, v.data(), v.size() * 2, &dst); else dst = {}; };
    auto UQ = [&](Buffer& dst, const H16& v, const H16& lr) {
        if (v.empty()) { dst = {}; return; }
        auto d = qv_bytes(v.data(), v.size());
        d.resize(v.size() * 3 + lr.size() * 2);
        std::memcpy(d.data() + v.size() * 3, lr.data(), lr.size() * 2);
        pack_bytes(dev_, fixed_, d.data(), d.size(), &dst);
    };
    UQ(b.in_a, s.b.in_a, s.b.C == 32 ? lr_keys(s.b.in_a, s.b.C, s.b.X) : H16{});
    UQ(b.in_b, s.b.in_b, {});
    U(b.out, with_tiles(s.b.out, s.b.X));
    U(b.out_b, s.b.out_b);
    U(b.fc1, with_tiles(s.b.fc1, s.b.C));
    U(b.fc1_b, s.b.fc1_b);
    U(b.fc2, with_tiles(s.b.fc2, 4 * s.b.C));
    U(b.fc2_b, s.b.fc2_b);
    U(b.bias, s.b.bias);
    U(b.dfn_w, s.dfn_w);
    U(b.dfn_b, s.dfn_b);
    U(b.mrg_w, s.Co && s.mrg_w.size() == size_t(s.Co) * 4 * s.b.C ? with_tiles(s.mrg_w, 4 * s.b.C) : s.mrg_w);
    U(b.mrg_b, s.mrg_b);
    U(b.up_w, s.up_w);
    U(b.up_b, s.up_b);
}

void Engine::init(Gpu& dev, const Kernels& k, const Weights& w) {
    dev_ = &dev, k_ = &k, w_ = &w;
    std::vector<uint8_t> blob;
    std::vector<Buffer*> refs;
    blob.reserve(24ull << 20);
    t_pack_blob = &blob;
    t_pack_refs = &refs;
    for (int s = 0; s < 6; ++s) upload_stage(w.enc[s], enc_[s]);
    for (int l = 0; l < 5; ++l) upload_stage(w.dec0[l], dec0_[l]), upload_stage(w.dec1[l], dec1_[l]);
    pack_bytes(dev_, fixed_, w.embed_w.data(), w.embed_w.size() * 2, &embed_w_);
    pack_bytes(dev_, fixed_, w.embed_b.data(), w.embed_b.size() * 2, &embed_b_);
    pack_bytes(dev_, fixed_, w.lut.data(), w.lut.size() * 2, &lut_);
    H16 masks(512);
    for (int i = 0; i < 256; ++i) masks[i] = (i % 16) < 8 ? 0x3c00 : 0, masks[256 + i] = (i % 16) < 8 ? 0 : 0x3c00;
    pack_bytes(dev_, fixed_, masks.data(), masks.size() * 2, &masks_);
    auto tab = value_tables(), bic = bicubic_table();
    pack_bytes(dev_, fixed_, tab.data(), tab.size() * 4, &tab_);
    pack_bytes(dev_, fixed_, bic.data(), bic.size() * 4, &bicubic_);
    t_pack_blob = nullptr;
    t_pack_refs = nullptr;
    if (!blob.empty()) {
        Buffer wb = dev.create(blob.size() + 64);
        dev.upload(wb, blob.data(), blob.size());
        fixed_.push_back(wb);
        for (Buffer* r : refs) r->addr += wb.addr;
    }
    flow_ = dev.create(10 * 512 * 4), fixed_.push_back(flow_);
    gtab_ = dev.create(sizeof(GuideTable)), fixed_.push_back(gtab_);
    hist_smp_ = dev.sampler(true, true);
}

void Engine::drop_display_history() {
    for (auto& b : dh_)
        if (b.addr) dev_->destroy(b);
    nr_frame_ = ~0ull;
}

void Engine::drop_transparency_history() {
    for (auto& b : tlh_)
        if (b.addr) dev_->destroy(b);
    for (auto& b : toh_)
        if (b.addr) dev_->destroy(b);
    tl_last_ = to_last_ = false;
}

void Engine::destroy() {
    if (!dev_) return;
    drop_display_history();
    drop_transparency_history();
    for (auto& i : hist_img_) dev_->destroy(i);
    for (auto& h : hist_)
        if (h.addr) dev_->destroy(h);
    if (hist_smp_) dev_->destroy(hist_smp_), hist_smp_ = VK_NULL_HANDLE;
    for (auto& b : sized_) dev_->destroy(b);
    for (auto& b : fixed_) dev_->destroy(b);
    sized_.clear(), fixed_.clear();
    alloc_render_ = alloc_output_ = {};
    dev_ = nullptr;
}

std::vector<std::string> Engine::flow_kernel_names() {
    return {"enc_stage_96_128_128_fl", "enc_stage_128_128_160_fl", "enc_stage_160_256_160_fl", "enc_stage_160_256_0_fl", "dec_up_160_256_160_fl",
            "dec_dfn_160_256_fl", "dec_up_128_128_160_fl", "dec_dfn_128_128_fl", "dec_up_96_128_128_fl", "dec_dfn_96_128_fl"};
}

std::vector<std::string> Engine::kernel_names(const Weights& w, bool fast) {
    std::string f = fast ? "_f" : "";
    auto d = [](const BlockWeights& b) { return std::to_string(b.C) + "_" + std::to_string(b.X); };
    std::vector<std::string> n = {"pooled_history2", "ae", "enc0_attr" + f, "post" + f, "downsample", "downsample_tl", "post_tile",
                                  "dfn_export", "dfn_import", "post_dlt" + f, "enc0_stage_32_64" + f};
    if (!fast) n.insert(n.end(), {"post_fixup", "post_fixup_dlt"});   // the k8 post's fixup pass (_f has none)
    for (int s = 1; s <= 5; ++s) n.push_back("enc_stage_" + d(w.enc[s].b) + "_" + std::to_string(w.enc[s].Co) + f);
    for (int l = 4; l >= 0; --l) {
        n.push_back("dec_up_" + d(w.dec0[l].b) + "_" + std::to_string(w.dec0[l].Cp) + f);
        n.push_back((l ? "dec_dfn_" : "dec_head_") + d(w.dec1[l].b) + f);
    }
    return n;
}

void Engine::resize(I2 render, I2 output, I2 max_render) {
    I2 cap = {std::max({render.x, max_render.x, alloc_render_.x}),
              std::max({render.y, max_render.y, alloc_render_.y})};
    const bool out_changed = output.x != alloc_output_.x || output.y != alloc_output_.y;
    const bool need_alloc = sized_.empty() || out_changed || sized_fast_ != fast_ ||
                            cap.x > alloc_render_.x || cap.y > alloc_render_.y;
    if (out_changed) {
        drop_display_history();
        drop_transparency_history();
    } else {
        nr_frame_ = ~0ull;
        tl_last_ = to_last_ = false;
    }
    render_ = render, output_ = output;
    internal_ = {output.x * 3 / 2, output.y * 3 / 2};
    ds_ = half_up(internal_), ds2_ = half_up(ds_);
    pad_ = {(render.x + 31) & ~31, (render.y + 31) & ~31};
    const size_t rp = size_t(render.x) * render.y, ip = size_t(internal_.x) * internal_.y;
    if (need_alloc) {
        for (auto& b : sized_) dev_->destroy(b);
        sized_.clear();
        alloc_render_ = cap;
        alloc_output_ = output;
        const I2 apad = {(cap.x + 31) & ~31, (cap.y + 31) & ~31};
        const size_t arp = size_t(cap.x) * cap.y;
        auto S = [&](size_t n) { Buffer b = make(n); sized_.push_back(b); return b; };
        auto align256 = [](size_t n) { return (n + 255) & ~size_t(255); };
        size_t attrScratchBytes = 0;
        size_t dec0ScratchBytes = 0;
        if (fast_) {
            for (int s = 1; s < 6; ++s) {
                I2 z = shr(apad, s);
                size_t px = size_t(z.x) * z.y;
                int C = w_->enc[s].b.C, Co = w_->enc[s].Co;
                attrScratchBytes = align256(attrScratchBytes) + px * C;
                attrScratchBytes = align256(attrScratchBytes) + px * 8;
                if (s == 1) {
                    dec0ScratchBytes = align256(dec0ScratchBytes) + px / 4 * std::max(Co, 32);
                    dec0ScratchBytes = align256(dec0ScratchBytes) + px / 4 * 8;
                } else {
                    attrScratchBytes = align256(attrScratchBytes) + px / 4 * std::max(Co, 32);
                    attrScratchBytes = align256(attrScratchBytes) + px / 4 * 8;
                }
            }
            for (int l = 1; l < 5; ++l) {
                I2 z = shr(apad, l);
                size_t px = size_t(z.x) * z.y;
                dec0ScratchBytes = align256(dec0ScratchBytes) + px * w_->dec0[l].b.C;
                if (l == 1)
                    attrScratchBytes = align256(attrScratchBytes) + px * w_->dec1[l].b.C;
            }
            const size_t px0 = size_t(apad.x) * apad.y;
            dec0ScratchBytes = std::max(dec0ScratchBytes, px0 / 4 * 8);
        }
        for (int s = 0; s < 6; ++s) {
            I2 z = shr(apad, s);
            size_t px = size_t(z.x) * z.y;
            int C = w_->enc[s].b.C, Co = w_->enc[s].Co;
            if (s >= 1 && fast_) {
                enc_l_[s] = {};
                continue;
            }
            enc_l_[s].tensor = S(px * C), enc_l_[s].dfn = S(px * 8);
            enc_l_[s].pooled = s ? S(px / 4 * std::max(Co, 32)) : Buffer{};
            enc_l_[s].dfn_pool = fast_ ? Buffer{} : S(px / 4 * 8);
        }
        for (int l = 0; l < 5; ++l) {
            I2 z = shr(apad, l);
            size_t px = size_t(z.x) * z.y;
            const size_t dec0Bytes = px * w_->dec0[l].b.C;
            const size_t postScratch = (!l && fast_) ? std::max((((ip * 4 + 255) & ~size_t(255)) + ip * 2), dec0ScratchBytes) : 0;
            dec0_l_[l].tensor = (l >= 1 && fast_) ? Buffer{} : S(std::max(dec0Bytes, postScratch));
            dec1_l_[l].tensor = (l && !(l == 1 && fast_)) ? S(px * w_->dec1[l].b.C) : Buffer{};
            dec1_l_[l].dfn = (!l && fast_) ? Buffer{} : S(px * 8);
        }
        // fast: one 48-byte record per grid pixel (enc0_attr.comp, ATTR64); else 48 fp16 attributes per image pixel
        const size_t attrBytes = fast_ ? size_t(apad.x) * apad.y * 48 : arp * 96;
        const size_t headScratch = fast_ ? (((arp * 24 + 255) & ~size_t(255)) + ((arp * 8 + 255) & ~size_t(255)) + ((arp * 4 + 255) & ~size_t(255)) * 2) : 0;
        attr_ = S(std::max({attrBytes, headScratch, attrScratchBytes})), img_ = S(size_t(apad.x) * apad.y * 8), ero_ = S(arp * 4), nond_ = S(arp * 4);
        for (int i = 0; i < 2; ++i) {
            pmv2_[i] = S(arp * 8), hidden_[i] = S(arp * 4), down_[i] = S(size_t(ds_.x) * ds_.y * 4);
            expo_[i] = S(4);
        }
        pdst_ = S(size_t(ds2_.x) * ds2_.y * 4), acc_ = S(16), blk_ = S(16);
        if (!fast_) {
            tile_ = S(arp * 24);
            fix_ = S(post_fix_bytes(internal_)), frg_ = S(ip * 4), fb_ = S(ip * 2);
            ajab_ = S(arp * 4), mvf_ = S(arp * 4), aniso_ = S(arp * 8);
        }
        prm_ = S(kBlockBytes + 64);
        attr_wide_ = {};
        if (out_changed || !hist_[0].addr) {
            for (auto& i : hist_img_) dev_->destroy(i);
            for (auto& h : hist_)
                if (h.addr) dev_->destroy(h);
            for (int i = 0; i < 2; ++i) {
                hist_[i] = make(ip * 8);
                hist_img_[i] = dev_->alias_image(hist_[i], uint32_t(internal_.x), uint32_t(internal_.y), VK_FORMAT_R16G16B16A16_SFLOAT, 8);
                if (hist_img_[i].img) dev_->set_texture(uint32_t(i), hist_img_[i], hist_smp_);
            }
        }
        sized_fast_ = fast_;
    }
    if (fast_) {
        const uint64_t t0 = attr_.addr;
        const uint64_t t1 = t0 + ((rp * 24 + 255) & ~size_t(255));
        const uint64_t t2 = t1 + ((rp * 8 + 255) & ~size_t(255));
        const uint64_t t3 = t2 + ((rp * 4 + 255) & ~size_t(255));
        tile_ = Buffer{}, tile_.addr = t0, tile_.size = rp * 24;
        aniso_ = Buffer{}, aniso_.addr = t1, aniso_.size = rp * 8;
        ajab_ = Buffer{}, ajab_.addr = t2, ajab_.size = rp * 4;
        mvf_ = Buffer{}, mvf_.addr = t3, mvf_.size = rp * 4;
        const uint64_t f0 = dec0_l_[0].tensor.addr;
        const uint64_t f1 = f0 + ((ip * 4 + 255) & ~size_t(255));
        dec1_l_[0].dfn = Buffer{}, dec1_l_[0].dfn.addr = img_.addr, dec1_l_[0].dfn.size = size_t(pad_.x) * pad_.y * 8;
        frg_ = Buffer{}, frg_.addr = f0, frg_.size = ip * 4;
        fb_ = Buffer{}, fb_.addr = f1, fb_.size = ip * 2;
        fix_ = Buffer{};
        auto sub = [](uint64_t base, size_t& cur, size_t sz) {
            cur = (cur + 255) & ~size_t(255);
            Buffer b{};
            b.addr = base + cur;
            b.size = sz;
            cur += sz;
            return b;
        };
        size_t aOff = 0, dOff = 0;
        for (int s = 1; s < 6; ++s) {
            I2 z = shr(pad_, s);
            size_t px = size_t(z.x) * z.y;
            int C = w_->enc[s].b.C, Co = w_->enc[s].Co;
            enc_l_[s].tensor = sub(t0, aOff, px * C);
            enc_l_[s].dfn = sub(t0, aOff, px * 8);
            if (s == 1) {
                enc_l_[s].pooled = sub(f0, dOff, px / 4 * std::max(Co, 32));
                enc_l_[s].dfn_pool = sub(f0, dOff, px / 4 * 8);
            } else {
                enc_l_[s].pooled = sub(t0, aOff, px / 4 * std::max(Co, 32));
                enc_l_[s].dfn_pool = sub(t0, aOff, px / 4 * 8);
            }
        }
        // enc_l_[0].dfn_pool is written in launch 3 and consumed in launch 4 (enc1); dec0_l_[1..4] are written in launches 10..20.
        const size_t dOffAfterEnc1Pool = (dOff + 255) & ~size_t(255);
        enc_l_[0].dfn_pool = Buffer{}, enc_l_[0].dfn_pool.addr = f0 + dOffAfterEnc1Pool, enc_l_[0].dfn_pool.size = size_t(pad_.x) * pad_.y / 4 * 8;
        for (int l = 1; l < 5; ++l) {
            I2 z = shr(pad_, l);
            size_t px = size_t(z.x) * z.y;
            dec0_l_[l].tensor = sub(f0, dOff, px * w_->dec0[l].b.C);
            if (l == 1)
                dec1_l_[l].tensor = sub(t0, aOff, px * w_->dec1[l].b.C);
        }
    }
    fresh_ = true;
}

EncBlockPC Engine::block_pc(const StageWeights& s, const StageBufs& b, uint64_t x, uint64_t y, I2 size, I2 shift) const {
    return EncBlockPC{x, y, b.in_a.addr, b.in_b.addr, b.out.addr, b.out_b.addr, b.fc1.addr, b.fc1_b.addr, b.fc2.addr, b.fc2_b.addr,
                      b.bias.addr, size, shift, s.b.ls, (size.x + shift.x + 7) / 8, masks_.addr};
}

void Engine::record(const EngineFrame& f, const Guides& g, uint64_t out) {
    record_net(f, g);
    record_post(f, out);
}

void Engine::record_net(const EngineFrame& f, const Guides& g, uint64_t dfn_out, uint32_t dfn_pitch, DfnEdit edit, bool dfn_half) {
    if (f.render.x != render_.x || f.render.y != render_.y || f.output.x != output_.x || f.output.y != output_.y)
        throw std::runtime_error("Engine::record: sizes changed without resize()");
    Gpu& d = *dev_;
    const Kernels& K = *k_;
    const Weights& W = *w_;
    const bool reset = f.reset || fresh_;
    const int cur = int(frame_ & 1), old = cur ^ 1;
    const bool odd = (frame_ & 1) != 0;
    frame_lite_ = lite_on_ && lk_ && fast_;
    const I2 sh_enc = odd ? I2{6, 2} : I2{2, 6}, sh_a = odd ? I2{0, 4} : I2{4, 0}, sh_b = odd ? I2{4, 0} : I2{0, 4};
    if (fresh_) {
        memcpy(prev_w2v_, f.world_to_view, sizeof prev_w2v_);
        memcpy(prev_v2c_, f.view_to_clip, sizeof prev_v2c_);
        prev_jitter_[0] = prev_jitter_[1] = 0;
    }
    // without matrices: identity view and a plain perspective, so the reflection vectors stay finite
    const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float persp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, -0.1f, 0};
    const float* w2v = zero_matrix(f.world_to_view) ? ident : f.world_to_view;
    const float* pw2v = zero_matrix(prev_w2v_) ? ident : prev_w2v_;
    const float* v2c = zero_matrix(f.view_to_clip) ? persp : f.view_to_clip;
    const float* pv2c = zero_matrix(prev_v2c_) ? persp : prev_v2c_;
    Enc0BlockInput bi{f.render, internal_, ds_, {f.jitter[0], f.jitter[1]}, {prev_jitter_[0], prev_jitter_[1]},
                      {f.mv_scale[0], f.mv_scale[1]}, w2v, v2c, pw2v, pv2c, W.enc0_scalars, f.depth_sign};
    auto blk = enc0_block(bi);
    if (fresh_) d.fill(expo_[old], 0, 4, 0);
    d.update(prm_, 0, kBlockBytes, blk.data());
    const bool dg = g.direct && direct_guides_ok();
    if (dg) d.update(gtab_, 0, sizeof(GuideTable), g.direct);
    // the small levels (enc2 .. dec2_1) on row flags instead of barriers (the _fl kernels), while level 2 has at most
    // 240 window rows (onr_flow.glsl FLOW_ROWS; the shifts add at most one)
    bool flow = flow_on_ && fast_ && (pad_.y >> 2) / 8 + 1 <= 240;
    for (const auto& n : flow_kernel_names()) flow = flow && K.has(n);
    if (flow) d.fill(flow_, 0, 10 * 512 * 4, 0);

    // launches 0 and 1: DownsampledHistory2 and the exposure (one dispatch; its last block leaves acc_ at +0)
    if (!fresh_) (frame_lite_ && lk_->has("pooled_history2") ? *lk_ : K).pooled_history2(d, PooledHistory2PC{down_[old].addr, pdst_.addr, ds_, ds2_});
    AePC ae{dg ? gtab_.addr : g.color, acc_.addr, f.render, {0, 0}, {f.render.x - 1, f.render.y - 1}, expo_[cur].addr, expo_[old].addr, blk_.addr,
            float(1.0 / (429496.71875 * double(f.render.x) * double(f.render.y))), reset ? 1u : 0u,
            uint32_t(div_up(f.render.x, 16) * div_up(f.render.y, 16))};
    d.dispatch(K[dg ? "d3d_ae" : "ae"], &ae, sizeof ae, div_up(f.render.x, 16), div_up(f.render.y, 16));
    d.barrier();

    // rr2_enc0
    const std::string fx = sfx();
    auto dims = [](const BlockWeights& b) { return std::to_string(b.C) + "_" + std::to_string(b.X); };
    Enc0AttrPC a{g.color, g.albedo, g.spec, g.normals, g.rough, g.depth, g.mv, g.hit, pmv2_[old].addr, hidden_[old].addr,
                 down_[old].addr, pdst_.addr, blk_.addr, prm_.addr, tab_.addr, attr_.addr, pmv2_[cur].addr, ero_.addr, nond_.addr,
                 img_.addr, f.render, ds_, ds2_, reset ? 1u : 0u, pad_, internal_, f.disocclusion, f.sss, lut_.addr, uint32_t(frame_ % 8)};
    if (dg) {
        // d3d_enc0_attr_f: dwords 0..3 unused, the guide table in place of spec
        a.color = a.albedo = a.normals = a.rough = a.depth = a.mv = a.hit = 0;
        a.spec = gtab_.addr;
    }
    // the optional guide table after the block (enc0_attr.comp, OPT_*), only when something is in it; attributes 32 / 33
    // need the 64-byte record in the fast mode, without its kernels those two guides are left out
    std::string attr_k = dg ? "d3d_enc0_attr_f" : "enc0_attr" + fx;
    bool wide = false;
    {
        const bool want = f.depth_of_field || f.responsivity;
        wide = want && fast_ && K.has(attr_k + "_w") && K.has("enc0_stage_32_64_f_w");
        const uint64_t keep = !fast_ || wide;
        uint64_t t[8] = {f.responsivity * keep, f.color_before_transparency, f.depth_of_field * keep, f.specular_motion, f.no_hit ? 1u : 0u};
        if (t[0] || t[1] || t[2] || t[3] || t[4]) {
            d.update(prm_, kBlockBytes, sizeof t, t);
            a.hist_off |= 2u;
        }
        if (wide) {
            const I2 apad = {(alloc_render_.x + 31) & ~31, (alloc_render_.y + 31) & ~31};
            const size_t needWide = size_t(apad.x) * apad.y * 64;
            if (attr_.size < needWide) {
                const uint64_t oldBase = attr_.addr;
                Buffer nw = make(needWide);
                for (auto& sb : sized_) {
                    if (sb.addr == oldBase) {
                        dev_->destroy(sb);
                        sb = nw;
                        break;
                    }
                }
                attr_ = nw;
                const uint64_t delta = nw.addr - oldBase;
                tile_.addr += delta, aniso_.addr += delta, ajab_.addr += delta, mvf_.addr += delta;
                for (int s = 1; s < 6; ++s) {
                    enc_l_[s].tensor.addr += delta;
                    enc_l_[s].dfn.addr += delta;
                    if (s >= 2) {
                        enc_l_[s].pooled.addr += delta;
                        enc_l_[s].dfn_pool.addr += delta;
                    }
                }
                dec1_l_[1].tensor.addr += delta;
            }
            attr_wide_ = attr_; a.attr = attr_.addr;
            attr_k += "_w";
        }
    }
    run(d, attr_k, &a, sizeof a, div_up(uint32_t(pad_.x * pad_.y), 64));
    d.barrier();
    {
        const StageWeights& s = W.enc[0];
        const StageBufs& b = enc_[0];
        Enc0StagePC e{};
        e.s = EncStagePC{block_pc(s, b, 0, enc_l_[0].tensor.addr, pad_, sh_enc), b.dfn_w.addr, b.dfn_b.addr, embed_w_.addr, embed_b_.addr,
                         img_.addr, enc_l_[0].dfn.addr, enc_l_[0].dfn_pool.addr, 0, s.s_dfn, I2{}};
        e.attr = wide ? attr_wide_.addr : attr_.addr, e.lut = lut_.addr, e.frame8 = uint32_t(frame_ % 8), e.img_h = f.render.y, e.img_w = f.render.x;
        run(d, "enc0_stage_32_64" + fx + (wide ? "_w" : ""), &e, sizeof e, windows(e.s.b));
        d.barrier();
    }
    // enc1..enc5
    for (int s = 1; s <= 5; ++s) {
        const StageWeights& sw = W.enc[s];
        const StageBufs& b = enc_[s];
        const Level& lo = enc_l_[s - 1];
        uint64_t x = s == 1 ? lo.tensor.addr : lo.pooled.addr;
        EncStagePC p{block_pc(sw, b, x, enc_l_[s].tensor.addr, shr(pad_, s), s == 5 ? sh_a : sh_enc), b.dfn_w.addr, b.dfn_b.addr,
                     b.mrg_w.addr, b.mrg_b.addr, lo.dfn_pool.addr, enc_l_[s].dfn.addr, enc_l_[s].dfn_pool.addr, enc_l_[s].pooled.addr,
                     sw.s_dfn, s == 1 ? pad_ : I2{}};
        const bool fl = flow && s >= 2;
        if (fl) p.b.mask = flow_.addr;
        run(d, "enc_stage_" + dims(sw.b) + "_" + std::to_string(sw.Co) + (fl ? "_fl" : fx), &p, sizeof p, windows(p.b));
        if (!fl) d.barrier();
    }
    // dec4..dec0
    uint64_t bt = enc_l_[5].tensor.addr, bd = enc_l_[5].dfn.addr;
    for (int l = 4; l >= 0; --l) {
        I2 z = shr(pad_, l);
        const StageWeights &s0 = W.dec0[l], &s1 = W.dec1[l];
        DecUpPC u{block_pc(s0, dec0_[l], enc_l_[l].tensor.addr, dec0_l_[l].tensor.addr, z, sh_a), bt, dec0_[l].up_w.addr,
                  dec0_[l].up_b.addr, shr(z, 1), s0.s98, 0};
        // dec0_1 reads the rows of its windows below out_h; the dec0_0 windows past them are skipped
        const int lh = 8 * ((f.render.y + sh_b.y + 7) / 8) - sh_b.y;
        if (!l && lh < z.y) u.live_h = lh;
        const bool fl = flow && l >= 2;
        if (fl) u.b.mask = flow_.addr;
        run(d, "dec_up_" + dims(s0.b) + "_" + std::to_string(s0.Cp) + (fl ? "_fl" : fx), &u, sizeof u, windows(u.b));
        if (!fl) d.barrier();
        DecDfnPC p{};
        p.b = block_pc(s1, dec1_[l], dec0_l_[l].tensor.addr, l ? dec1_l_[l].tensor.addr : 0, z, sh_b);
        p.dfn_w = dec1_[l].dfn_w.addr, p.dfn_b = dec1_[l].dfn_b.addr;
        p.below = bd, p.skip = enc_l_[l].dfn.addr, p.dfn_out = dec1_l_[l].dfn.addr;
        p.lo_size = shr(z, 1), p.s_dfn = s1.s_dfn, p.out_h = l ? z.y : f.render.y;
        if (!l) {
            p.ajab = ajab_.addr, p.mvf = mvf_.addr, p.hidden = hidden_[cur].addr, p.aniso = aniso_.addr;
            p.k_scale = W.head_k_scale, p.k_tanh = W.head_k_tanh, p.out_w = f.render.x;
            // the head writes the post tile, unless a loop edit replaces the DFN first (post_tile then, from the DFN,
            // ajab and aniso, which the head only writes without the tile)
            tile_late_ = edit == DfnEdit::loop;
            p.tile = tile_late_ ? 0 : tile_.addr;
            // with the tile written, only the export reads the DFN image
            if (fast_ && p.tile && !dfn_out) p.dfn_out = 0;
        }
        if (fl) p.b.mask = flow_.addr;
        run(d, (l ? "dec_dfn_" : "dec_head_") + dims(s1.b) + (fl ? "_fl" : fx), &p, sizeof p, windows(p.b));
        // dec2_1 ends the flow: everything after it reads behind a barrier
        if (!(fl && l >= 3)) d.barrier();
        bt = dec1_l_[l].tensor.addr, bd = dec1_l_[l].dfn.addr;
    }
    if (dfn_out) {
        DfnExportPC e{dec1_l_[0].dfn.addr, blk_.addr, dfn_out, f.render, pad_.x, dfn_pitch, dfn_half ? 1u : 0u};
        d.dispatch((frame_lite_ && lk_->has("dfn_export") ? *lk_ : K)["dfn_export"], &e, sizeof e, div_up(f.render.x, 16), div_up(f.render.y, 16));
    }
}

void Engine::record_post(const EngineFrame& f, uint64_t out, DfnEdit edit, const DfnImport& in) {
    Gpu& d = *dev_;
    const Kernels& K = *k_;
    const Weights& W = *w_;
    const bool reset = f.reset || fresh_;
    const int cur = int(frame_ & 1), old = cur ^ 1;
    if (!in.src) edit = DfnEdit::none;
    // loop: the edit goes into the DFN itself, then the tile is built from it; display: into the tile's three free
    // halves, after the tile (the head's or post_tile's)
    DfnImportPC im{in.src, blk_.addr, dec1_l_[0].dfn.addr, tile_.addr, f.render, pad_.x, in.pitch, in.fmt, edit == DfnEdit::display ? 1u : 0u};
    auto import = [&] {
        d.dispatch((frame_lite_ && lk_->has("dfn_import") ? *lk_ : K)["dfn_import"], &im, sizeof im, div_up(f.render.x, 16), div_up(f.render.y, 16));
        d.barrier();
    };
    if (edit == DfnEdit::loop) import();
    if (tile_late_ || edit == DfnEdit::loop) {
        K.post_tile(d, PostTilePC{dec1_l_[0].dfn.addr, ajab_.addr, aniso_.addr, tile_.addr, f.render, pad_.x});
        d.barrier();
    }
    tile_late_ = false;
    if (edit == DfnEdit::display) import();
    // post and downsample
    PostPC pp{};
    pp.tile = tile_.addr, pp.mvf = mvf_.addr, pp.hist_in = hist_[old].addr, pp.mv = pmv2_[cur].addr, pp.mv_eroded = ero_.addr;
    pp.mv_nondil = nond_.addr, pp.exposure = blk_.addr, pp.bicubic = bicubic_.addr, pp.hist_out = hist_[cur].addr;
    pp.out_rg = frg_.addr, pp.out_b = fb_.addr, pp.down = down_[cur].addr, pp.fix = fix_.addr;
    pp.gate_scale = W.post_gate;
    pp.scale[0] = float(f.render.x) / float(internal_.x), pp.scale[1] = float(f.render.y) / float(internal_.y);
    pp.offset[0] = f.jitter[0], pp.offset[1] = f.jitter[1];
    pp.inv_size[0] = 1.0f / float(internal_.x), pp.inv_size[1] = 1.0f / float(internal_.y);
    pp.lr_inv_size[0] = 1.0f / float(f.render.x), pp.lr_inv_size[1] = 1.0f / float(f.render.y);
    pp.size = internal_, pp.lr_size = f.render, pp.origin = {0, 0}, pp.end = internal_, pp.slice = internal_;
    pp.reset = reset ? 1u : 0u;
    // fast post: the history taps on the sampler (the same values as its hand blend)
    if (fast_ && sampler_ && hist_img_[0].img && hist_img_[1].img) pp.reset |= 16u | uint32_t(old) << 5;
    // display: the edit as a log delta over the network's display blend, with a history of its own; a frame without
    // an edit carries that history over
    if (edit == DfnEdit::display && !display_history()) {
        const size_t ip = size_t(internal_.x) * internal_.y * 4 + 64;
        for (auto& b : dh_) b = d.create(ip);   // never read before written: the first frame takes its own delta
        nr_frame_ = ~0ull;
    }
    if (display_history() && edit != DfnEdit::loop) {
        PostDltPC pn{pp, dh_[old].addr, dh_[cur].addr, dh_alpha_};
        if (nr_frame_ == ~0ull || nr_frame_ + 1 != frame_) pn.p.reset |= 4u;
        if (edit == DfnEdit::none) pn.p.reset |= 8u;
        (frame_lite_ && lk_->has("post_dlt_f") ? *lk_ : K).post_dlt(d, pn, fix_, fast_);
        nr_frame_ = frame_;
    } else {
        (frame_lite_ && lk_->has("post_f") ? *lk_ : K).post(d, pp, fix_, fast_);
    }
    d.barrier();
    const DownsamplePC ds{frg_.addr, fb_.addr, out, blk_.addr, internal_, f.output, {0, 0}, f.output, internal_, {0, internal_.x}};
    // out_image_: out is the origin x | y << 16 in the D3D12 output texture, written by the d3d_ variants
    const bool img = out_image_ && k_->has("d3d_downsample") && k_->has("d3d_downsample_tl");
    if (f.transparency && k_->has("downsample_tl")) {
        if (!tlh_[0].addr)   // never read before written: the layer's first frame takes no history
            for (auto& b : tlh_) b = d.create(size_t(f.output.x) * f.output.y * 8 + 64);
        // the parameters NGX gives the kernel (constant 0.25 weight and output / internal motion scale in its captures)
        DownsampleTlPC p{ds, f.transparency, pmv2_[cur].addr, tlh_[old].addr, tlh_[cur].addr, f.render};
        const float r[2] = {float(f.render.x), float(f.render.y)}, o[2] = {float(f.output.x), float(f.output.y)};
        const float ir[2] = {float(internal_.x), float(internal_.y)};
        for (int i = 0; i < 2; ++i) {
            p.scale[i] = r[i] / o[i], p.offset[i] = f.jitter[i], p.offset_out[i] = f.jitter[i] * (o[i] / r[i]);
            p.mv_scale[i] = o[i] / ir[i];
        }
        p.weight = 0.25f, p.reset = reset || !tl_last_ ? 1u : 0u;
        if (f.transparency_motion) {
            p.tlmv = f.transparency_motion, p.tlmv_size = f.transparency_motion_size;
            p.tlmv_scale[0] = f.mv_scale[0], p.tlmv_scale[1] = f.mv_scale[1];
        }
        if (f.transparency_opacity) {
            if (!toh_[0].addr)
                for (auto& b : toh_) b = d.create(size_t(f.output.x) * f.output.y * 8 + 64);
            p.op = f.transparency_opacity, p.op_hist_in = toh_[old].addr, p.op_hist_out = toh_[cur].addr;
            p.op_reset = p.reset || !to_last_ ? 1u : 0u;
        }
        const std::string tlk = img ? "d3d_downsample_tl" : "downsample_tl";
        d.dispatch((frame_lite_ && lk_->has(tlk) ? *lk_ : K)[tlk], &p, sizeof p, div_up(f.output.x, 16), div_up(f.output.y, 16));
    } else if (img) {
        d.dispatch((frame_lite_ && lk_->has("d3d_downsample") ? *lk_ : K)["d3d_downsample"], &ds, sizeof ds, div_up(f.output.x, 16), div_up(f.output.y, 16));
    } else {
        (frame_lite_ && lk_->has("downsample") ? *lk_ : K).downsample(d, ds);
    }
    tl_last_ = f.transparency != 0;
    to_last_ = tl_last_ && f.transparency_opacity != 0;

    memcpy(prev_w2v_, f.world_to_view, sizeof prev_w2v_);
    memcpy(prev_v2c_, f.view_to_clip, sizeof prev_v2c_);
    prev_jitter_[0] = f.jitter[0], prev_jitter_[1] = f.jitter[1];
    fresh_ = false;
    ++frame_;
}

}  // namespace onr
