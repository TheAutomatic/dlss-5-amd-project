// SPDX-License-Identifier: MIT
#include "opennr_rr/weights.hpp"

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <immintrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

#pragma comment(lib, "bcrypt.lib")

namespace onr {

namespace {

struct LayoutRecord { uint32_t slot, count, offset; };
struct DllLayout {
    const char* version;
    const LayoutRecord* records;
    size_t nrecords;
    const LayoutRecord* imms;   // count = bytes
    size_t nimms;
    uint32_t lut_offset, lut_bytes;
    const char* sha256;         // of the records, the immediates and the LUT, in that order
};
#include "dlssd_layout_310_9_1_0.inc"
const DllLayout* const kLayouts[] = {&kLayout_310_9_1_0};

constexpr uint32_t kStride = 0x238;
constexpr int kWidth[11] = {32, 64, 96, 128, 160, 160, 160, 128, 96, 64, 32};

std::string sha256_hex(const std::vector<uint8_t>& d) {
    uint8_t h[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(d.data()), ULONG(d.size()), h, 32);
    char s[65];
    for (int i = 0; i < 32; ++i) snprintf(s + 2 * i, 3, "%02x", h[i]);
    return s;
}

std::string pe_version(const std::vector<uint8_t>& d) {
    const char16_t key[] = u"VS_VERSION_INFO";
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);
    auto it = std::search(d.begin(), d.end(), k, k + 30);
    if (it == d.end()) return "unknown";
    const uint8_t sig[4] = {0xBD, 0x04, 0xEF, 0xFE};
    auto s = std::search(it, d.end(), sig, sig + 4);
    if (s == d.end() || size_t(d.end() - s) < 16) return "unknown";
    uint32_t ms, ls;
    memcpy(&ms, &*s + 8, 4);
    memcpy(&ls, &*s + 12, 4);
    char b[64];
    snprintf(b, sizeof b, "%u.%u.%u.%u", ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF);
    return b;
}

// FP16 -> E4M3 (round to nearest even, saturate to 448) -> FP16, as the uploads store matrices
uint16_t q8(uint16_t h) {
    // built once, thread-safe: two features build their weights at the same time
    static const std::array<float, 0x7F> pos = [] {
        std::array<float, 0x7F> p{};
        for (int b = 0; b < 0x7F; ++b) {
            int e = (b >> 3) & 15, m = b & 7;
            p[b] = e ? std::ldexp(1.0f + float(m) / 8.0f, e - 7) : float(m) / 8.0f * std::ldexp(1.0f, -6);
        }
        return p;
    }();
    float x = h2f(h);
    if (std::isnan(x)) return 0x7E00;
    double a = std::min(std::fabs(double(x)), 448.0);
    int i = int(std::lower_bound(pos.begin(), pos.end(), a) - pos.begin());   // pos[i-1] < a <= pos[i]
    i = std::clamp(i, 1, 0x7E);
    double lo = pos[i - 1], hi = pos[i];
    bool pick_hi = (a - lo > hi - a) || ((a - lo == hi - a) && (i & 1) == 0);
    int code = a == 0 ? 0 : (pick_hi ? i : i - 1);
    float v = pos[code];
    return f2h(std::signbit(x) ? -v : v);
}

struct Table {
    std::map<uint32_t, H16> T;
    std::map<uint32_t, uint32_t> imm;   // slot -> immediate bits
    const H16& at(uint32_t slot) const {
        auto it = T.find(slot);
        if (it == T.end()) throw std::runtime_error("weight table has no slot " + std::to_string(slot));
        return it->second;
    }
    uint32_t im(uint32_t slot) const {
        auto it = imm.find(slot);
        if (it == imm.end()) throw std::runtime_error("no inline scalar for slot " + std::to_string(slot));
        return it->second;
    }
};

// rows x K matrix (q8 = E4M3 rounding of the stored value)
H16 mat(const Table& t, uint32_t slot, bool q = true) {
    H16 v = t.at(slot);
    if (q)
        for (auto& x : v) x = q8(x);
    return v;
}

void attn(const Table& t, uint32_t base, int C, BlockWeights& b, uint32_t rq, uint32_t ro, uint32_t rob, uint32_t rbias) {
    H16 qv = mat(t, base + rq);
    int X = int(qv.size() / C) / 2;
    b.C = C, b.X = X;
    b.in_a.assign(qv.begin(), qv.begin() + size_t(X) * C);
    b.in_b.assign(qv.begin() + size_t(X) * C, qv.end());
    b.out = mat(t, base + ro);
    b.out_b = t.at(base + rob);
    const H16& A = t.at(base + rbias);   // [H][64][64]
    int H = X / 32;
    b.bias.resize(size_t(H) * 4096);
    const float r2 = float(std::sqrt(2.0));
    for (int h = 0; h < H; ++h)
        for (int i = 0; i < 64; ++i)
            for (int j = 0; j < 64; ++j) {
                float a = h2f(A[size_t(h) * 4096 + i * 64 + j]), c = h2f(A[size_t(h) * 4096 + j * 64 + i]);
                volatile float s = a + c;   // fp32 sum, then fp32 product, then one rounding to fp16
                volatile float p = s * r2;
                b.bias[size_t(h) * 4096 + i * 64 + j] = f2h(p);
            }
}

void mlp(const Table& t, uint32_t base, BlockWeights& b, uint32_t r1, uint32_t rb1, uint32_t r2, uint32_t rb2) {
    b.fc1 = mat(t, base + r1);
    b.fc1_b = t.at(base + rb1);
    b.fc2 = mat(t, base + r2);
    b.fc2_b = t.at(base + rb2);
}

// Conv2d(C, Co, 2, stride 2) [Co][C][2][2] -> [Co][pos][C], K = pos * C + ci
void merge(const Table& t, uint32_t base, int C, H16& w, H16& bias, int& Co) {
    const H16& b = t.at(base + 0x190);
    Co = int(b.size());
    H16 src = mat(t, base + 0x180);
    size_t o = w.size();
    w.resize(o + size_t(Co) * 4 * C);
    for (int co = 0; co < Co; ++co)
        for (int ci = 0; ci < C; ++ci)
            for (int pos = 0; pos < 4; ++pos) w[o + size_t(co) * 4 * C + pos * C + ci] = src[(size_t(co) * C + ci) * 4 + pos];
    bias.insert(bias.end(), b.begin(), b.end());
}

// Linear(Cp, 4C) + PixelShuffle(2): [C][pos][Cp] -> [pos][C][Cp]; bias [C][pos] -> [pos][C]
void up(const Table& t, uint32_t base, int C, int Cp, StageWeights& s) {
    H16 w = mat(t, base + 0x1a0);
    const H16& b = t.at(base + 0x1b0);
    s.up_w.resize(w.size());
    s.up_b.resize(b.size());
    for (int c = 0; c < C; ++c)
        for (int pos = 0; pos < 4; ++pos) {
            for (int k = 0; k < Cp; ++k) s.up_w[(size_t(pos) * C + c) * Cp + k] = w[(size_t(c) * 4 + pos) * Cp + k];
            s.up_b[size_t(pos) * C + c] = b[size_t(c) * 4 + pos];
        }
    s.Cp = Cp;
}

void dfn(const Table& t, uint32_t base, int C, StageWeights& s) {
    const H16& b = t.at(base + 0x1d0);
    H16 w = mat(t, base + 0x1c0);
    s.dfn_w.assign(size_t(16) * C, 0);
    s.dfn_b.assign(16, 0);
    std::copy(w.begin(), w.end(), s.dfn_w.begin());
    std::copy(b.begin(), b.end(), s.dfn_b.begin());
}

}  // namespace

float h2f(uint16_t h) { return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(h))); }
uint16_t f2h(float f) { return uint16_t(_mm_cvtsi128_si32(_mm_cvtps_ph(_mm_set_ss(f), _MM_FROUND_TO_NEAREST_INT))); }

bool build_weights(const std::wstring& path, Weights& w, std::string& err) {
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("cannot read the file");
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
        w.version = pe_version(d);
        const DllLayout* L = nullptr;
        std::vector<uint8_t> blob;
        for (const DllLayout* c : kLayouts) {
            blob.clear();
            bool fits = size_t(c->lut_offset) + c->lut_bytes <= d.size();
            for (size_t i = 0; fits && i < c->nrecords; ++i) {
                const auto& r = c->records[i];
                fits = size_t(r.offset) + 2ull * r.count <= d.size();
                if (fits) blob.insert(blob.end(), d.begin() + r.offset, d.begin() + r.offset + 2ull * r.count);
            }
            for (size_t i = 0; fits && i < c->nimms; ++i) {
                const auto& r = c->imms[i];
                fits = size_t(r.offset) + r.count <= d.size();
                if (fits) blob.insert(blob.end(), d.begin() + r.offset, d.begin() + r.offset + r.count);
            }
            if (!fits) continue;
            blob.insert(blob.end(), d.begin() + c->lut_offset, d.begin() + c->lut_offset + c->lut_bytes);
            if (sha256_hex(blob) == c->sha256) { L = c; break; }
        }
        if (!L) {
            std::string known;
            for (const DllLayout* c : kLayouts) known += std::string(known.empty() ? "" : ", ") + c->version;
            throw std::runtime_error("version " + w.version + " is not a build whose weight layout is known (" + known + ")");
        }
        Table t;
        for (size_t i = 0; i < L->nrecords; ++i) {
            const auto& r = L->records[i];
            H16 v(r.count);
            memcpy(v.data(), d.data() + r.offset, 2ull * r.count);
            t.T[r.slot] = std::move(v);
        }
        for (size_t i = 0; i < L->nimms; ++i) {
            uint32_t v = 0;
            memcpy(&v, d.data() + L->imms[i].offset, L->imms[i].count);
            t.imm[L->imms[i].slot] = v;
        }
        w.lut.resize(L->lut_bytes / 2);
        memcpy(w.lut.data(), d.data() + L->lut_offset, L->lut_bytes);

        // enc0: embed MLP (unquantized), block, DFN
        {
            const H16 &f1 = t.at(0x0), &f2 = t.at(0x20), &b1 = t.at(0x10), &b2 = t.at(0x30);
            w.embed_w.assign(64 * 64 + 32 * 64, 0);
            for (int r = 0; r < 64; ++r)
                for (int k = 0; k < 43; ++k) w.embed_w[r * 64 + k] = f1[r * 43 + k];
            std::copy(f2.begin(), f2.end(), w.embed_w.begin() + 64 * 64);
            w.embed_b = b1;
            w.embed_b.insert(w.embed_b.end(), b2.begin(), b2.end());
        }
        for (int s = 0; s < 6; ++s) {
            uint32_t base = s * kStride;
            StageWeights& st = w.enc[s];
            attn(t, base, kWidth[s], st.b, 0x50, 0x60, 0x70, 0x80);
            mlp(t, base, st.b, 0xa0, 0xb0, 0xc0, 0xd0);
            if (s >= 1 && s <= 4) merge(t, base, kWidth[s], st.mrg_w, st.mrg_b, st.Co);
            else st.mrg_w.assign(16, 0), st.mrg_b.assign(16, 0);
            if (s == 1) {
                int ci;
                merge(t, 0, kWidth[0], st.mrg_w, st.mrg_b, ci);
            }
            dfn(t, base, kWidth[s], st);
            st.s_dfn = t.at(base + 0x1e0)[0];
            st.b.ls = t.im(base + 0x220);
        }
        for (int s = 6; s <= 10; ++s) {
            uint32_t base = s * kStride;
            int lv = 10 - s, C = kWidth[s];
            StageWeights& a = w.dec0[lv];
            StageWeights& b = w.dec1[lv];
            up(t, base, C, kWidth[s - 1], a);
            attn(t, base, C, a.b, 0x50, 0x60, 0x70, 0x80);
            mlp(t, base, a.b, 0xa0, 0xb0, 0xc0, 0xd0);
            attn(t, base, C, b.b, 0xf0, 0x100, 0x110, 0x120);
            mlp(t, base, b.b, 0x140, 0x150, 0x160, 0x170);
            if (s < 10) dfn(t, base, C, b);
            a.s_dfn = b.s_dfn = t.at(base + 0x1e0)[0];
            a.b.ls = t.im(base + 0x220);
            b.b.ls = t.im(base + 0x224);
            a.s98 = b.s98 = s < 10 ? uint16_t(t.im(base + 0x228)) : uint16_t(t.im(0x1858) & 0xFFFF);
        }
        auto f32 = [](uint32_t b) { float x; memcpy(&x, &b, 4); return x; };
        w.enc0_scalars[0] = f32(t.im(0x230));
        w.enc0_scalars[1] = f32(t.im(0x234));
        w.enc0_scalars[2] = h2f(uint16_t(t.im(0x185c)));
        w.enc0_scalars[3] = h2f(uint16_t(t.im(0x1858) >> 16));
        w.head_k_tanh = std::exp(h2f(t.at(0x1830)[0]));
        w.post_gate = std::exp(h2f(t.at(0x1840)[0]));
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

}  // namespace onr
