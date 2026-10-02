// NVIDIA's activation layout, the numeric formats around it, and the two
// conversions between it and this project's canonical raster NHWC.
//
// The shipping kernels store activations in the "tinlayout" of their names -
// 4x4-pixel tiles of 16 pixels x 32 channels, with the mma.m16n8k32 D fragment
// written straight out. Verified byte-exactly and bijectively against a
// `scalars` capture, 512 of 512:
//
//     byte    = tile*512 + lane*16 + i          (lane 0..31, i 0..15)
//     tile    = (y/4)*(width/4) + (x/4)
//     token   = lane/4 + 8*((i/4) % 2)
//     channel = 8*(2*(i/8) + ((i/2) % 2)) + 2*(lane % 4) + (i % 2)
//
// At C > 32 a tile is one 512-byte block per head, each block exactly the C=32
// layout with the head supplying the top channel bits (measured at C=64 and
// confirmed at C=128, not assumed).
//
// Decision 1 chose raster NHWC f16 for our runtime, so this conversion is a
// harness boundary and not something a kernel ever does.
//
// The intra-tile token order is **measured**, 2026-09-10, and it is plain
// row-major:
//
//     slot = 4*(y % 4) + (x % 4)
//
// bijective over all sixteen slots, checked again at 32x8 where the tile grid
// is not square. Two independent probes agree - a slot probe one-hots
// a pixel of an `_inpview` kernel's input and reads which slot moves, and the
// `_ds` kernels' half-resolution output independently pins the high bit of
// each coordinate. Until this was measured the header treated a token as a
// flat index, which every finished layer is pointwise enough not to notice.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tin {

inline float e4m3_to_f(uint8_t b) {
    const float s = (b & 0x80) ? -1.0f : 1.0f;
    const int e = (b >> 3) & 0xF, m = b & 0x7;
    if (e == 0xF && m == 0x7) return NAN;                       // the two NaNs, 0x7F / 0xFF
    if (e == 0) return s * float(m) * 0.001953125f;             // 2^-9
    return s * (1.0f + float(m) / 8.0f) * std::ldexp(1.0f, e - 7);
}

// Round-to-nearest-even against the finite table, saturating at +/-448. This is
// `cvt.rn.satfinite.e4m3x2.f16x2`, and it must be fed an f16 value: the
// shipping quantiser narrows from f16, so quantising an f32 directly would
// double-round differently.
inline uint8_t f_to_e4m3(float x) {
    if (std::isnan(x)) return 0x7F;
    if (x > 448.0f) return 0x7E;
    if (x < -448.0f) return 0xFE;
    int best = 0;
    float bd = INFINITY;
    for (int i = 0; i < 256; ++i) {
        if (i == 0x7F || i == 0xFF) continue;
        const float d = std::fabs(e4m3_to_f(uint8_t(i)) - x);
        if (d < bd || (d == bd && (i & 1) == 0)) { bd = d; best = i; }
    }
    return uint8_t(best);
}

inline float f16_to_f(uint16_t h) {
    const uint32_t s = uint32_t(h & 0x8000u) << 16;
    const int e = (h >> 10) & 0x1F;
    const uint32_t m = h & 0x3FFu;
    if (e == 0) { const float v = std::ldexp(float(m), -24); return s ? -v : v; }
    if (e == 31) return m ? NAN : (s ? -INFINITY : INFINITY);
    float out;
    const uint32_t o = s | (uint32_t(e - 15 + 127) << 23) | (m << 13);
    std::memcpy(&out, &o, 4);
    return out;
}

inline uint16_t f_to_f16(float x) {
    uint32_t u;
    std::memcpy(&u, &x, 4);
    const uint32_t sg = (u >> 16) & 0x8000u;
    const int32_t ex = int32_t((u >> 23) & 0xFF) - 127;
    const uint32_t mn = u & 0x7FFFFFu;
    if (ex == 128) return uint16_t(sg | 0x7C00u | (mn ? 0x200u : 0u));
    if (ex < -127) return uint16_t(sg);
    int32_t he = ex + 15;
    const uint32_t m24 = mn | 0x800000u;
    int shift = 13;
    if (he <= 0) { shift = 14 - he; he = 0; if (shift > 24) return uint16_t(sg); }
    uint32_t hi = m24 >> shift;
    const uint32_t lo = m24 & ((1u << shift) - 1u), half = 1u << (shift - 1);
    if (lo > half || (lo == half && (hi & 1u))) ++hi;
    if (he == 0) return uint16_t(hi >= 0x400u ? (sg | 0x400u) : (sg | hi));
    if (hi >= 0x800u) { hi >>= 1; ++he; }
    if (he >= 31) return uint16_t(sg | 0x7C00u);
    return uint16_t(sg | (uint32_t(he) << 10) | (hi & 0x3FFu));
}

inline float round_f16(float x) { return f16_to_f(f_to_f16(x)); }

// Element index of (token, channel) in the tinlayout, in units of the element
// size. e4m3 tensors index bytes directly; f16 tensors of the same shape use
// twice the stride, which is `2 * byte_of(...)` for every layer measured so far
// except `cc_dec_input_upsample`, which has a map of its own.
inline size_t byte_of(size_t token, size_t ch, size_t C) {
    const size_t head = ch / 32, sub = ch % 32;
    const size_t hi = sub / 8;
    const size_t lane = (token % 16) % 8 * 4 + (sub % 8) / 2;
    return (token / 16) * (16 * C) + head * 512 + lane * 16
         + ((hi / 2) * 8 + ((token % 16) / 8) * 4 + (hi % 2) * 2 + sub % 2);
}

// tinlayout e4m3 -> canonical [token][channel] f16. Exact: every e4m3 value is
// representable in f16, so the kernel's narrowing back to e4m3 is the identity
// and the comparison tests the kernel rather than the round trip.
inline std::vector<uint16_t> to_canonical_f16(const uint8_t* src, size_t M, size_t C) {
    std::vector<uint16_t> out(M * C);
    for (size_t t = 0; t < M; ++t)
        for (size_t c = 0; c < C; ++c)
            out[t * C + c] = f_to_f16(e4m3_to_f(src[byte_of(t, c, C)]));
    return out;
}

// canonical [token][channel] e4m3 -> tinlayout, for scoring against a gold.
inline std::vector<uint8_t> from_canonical_e4m3(const uint8_t* src, size_t M, size_t C) {
    std::vector<uint8_t> out(M * C, 0);
    for (size_t t = 0; t < M; ++t)
        for (size_t c = 0; c < C; ++c)
            out[byte_of(t, c, C)] = src[t * C + c];
    return out;
}

// Flat token index of pixel (x, y) in an image `width` pixels across. The tile
// grid is row-major over 4x4 tiles and the sixteen slots inside a tile are
// row-major too, so this is the whole spatial story of the tinlayout.
inline size_t token_of(size_t x, size_t y, size_t width) {
    return ((y / 4) * (width / 4) + x / 4) * 16 + 4 * (y % 4) + (x % 4);
}

// The *other* activation layout in this network, and it is not the tinlayout.
//
// A `_ds` kernel writes its half-resolution output through a second pointer
// (`param_0+64`, dims at `+72`) in a different format, and the `_inpview`
// kernel that consumes it reads that format at `param_0+0`. Every downsample
// in the graph is followed by an `inpview` block - blocks 0->1, 4->5, 8->9 -
// so this is the boundary format between resolution levels, not a one-off:
//
//     byte = (ch / 16) * (width * height * 16)      // 16-channel plane
//          + (y * width + x) * 16                   // raster within the plane
//          + view_sub(ch % 16)
//
// The pixel order is plain raster - verified by moving the width, where the
// plane stride and the row stride both follow - and the sixteen channels of a
// group are permuted, measured one byte at a time and bijective:
//
//     sub    0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
//     chan   0  1  8  9  2  3 10 11  4  5 12 13  6  7 14 15
//
// which is the bit permutation below. Both of these are e4m3; a `_ds` output
// and an `_inpview` input are the same bytes.
inline size_t view_sub(size_t ch) {
    const size_t c = ch % 16;
    return (c & 1) | (((c >> 3) & 1) << 1) | (((c >> 1) & 1) << 2) | (((c >> 2) & 1) << 3);
}

inline size_t view_byte_of(size_t x, size_t y, size_t ch, size_t W, size_t H) {
    return (ch / 16) * (W * H * 16) + (y * W + x) * 16 + view_sub(ch);
}

// view e4m3 -> canonical [y][x][channel] f16, and back. Same exactness
// argument as the tinlayout pair above: every e4m3 value is an f16 value.
inline std::vector<uint16_t> view_to_canonical_f16(const uint8_t* src, size_t W, size_t H, size_t C) {
    std::vector<uint16_t> out(W * H * C);
    for (size_t y = 0; y < H; ++y)
        for (size_t x = 0; x < W; ++x)
            for (size_t c = 0; c < C; ++c)
                out[(y * W + x) * C + c] = f_to_f16(e4m3_to_f(src[view_byte_of(x, y, c, W, H)]));
    return out;
}

inline std::vector<uint8_t> view_from_canonical_e4m3(const uint8_t* src, size_t W, size_t H, size_t C) {
    std::vector<uint8_t> out(W * H * C, 0);
    for (size_t y = 0; y < H; ++y)
        for (size_t x = 0; x < W; ++x)
            for (size_t c = 0; c < C; ++c)
                out[view_byte_of(x, y, c, W, H)] = src[(y * W + x) * C + c];
    return out;
}

// Row-major [rows][K] -> tile-blocked [rows/16][K/16][16 rows][16 k], the
// backend-preferred form decision 4 always specified and the runtime was always
// supposed to produce at load time. One 16x16 cooperative-matrix fragment
// becomes 256 contiguous bytes, so a fragment load touches two cache lines and
// uses all of both; in the canonical layout the fragment stride is K, so the
// same load touches sixteen lines and uses 16 bytes of each.
//
// Nothing driver-specific is in here: the block order is a property of the
// matrix shape, not of a lane mapping, so the weight *file* stays portable and
// this is a runtime transform.
inline std::vector<uint8_t> tile_blocked(const uint8_t* src, size_t rows, size_t K) {
    std::vector<uint8_t> out(rows * K, 0);
    const size_t ktiles = K / 16;
    for (size_t r = 0; r < rows; ++r)
        for (size_t k = 0; k < K; ++k)
            out[((r / 16) * ktiles + k / 16) * 256 + (r % 16) * 16 + (k % 16)] = src[r * K + k];
    return out;
}

// The same block order, in f16. `NR_F16_MMA` reads its weights out of the f16
// arena (binding 4): the tile order is a property of the matrix shape and does
// not move, only the element width does, so the formula is the one above and
// the caller passes **half-indices** in the push block instead of byte offsets
// (`NR_TILE` computes element offsets in units of the array's element type).
//
// e4m3 -> f16 is exact - 8 significant bits into 11 - so this loses nothing the
// e4m3 weight file has not already lost.
inline std::vector<uint16_t> tile_blocked_f16(const uint8_t* src, size_t rows, size_t K) {
    std::vector<uint16_t> out(rows * K, 0);
    const size_t ktiles = K / 16;
    for (size_t r = 0; r < rows; ++r)
        for (size_t k = 0; k < K; ++k)
            out[((r / 16) * ktiles + k / 16) * 256 + (r % 16) * 16 + (k % 16)] =
                f_to_f16(e4m3_to_f(src[r * K + k]));
    return out;
}

// The inverse of tile_blocked, for reading a producer's tile-blocked output
// back into row-major so it can be scored.
inline std::vector<uint8_t> un_tile_blocked(const uint8_t* src, size_t rows, size_t K) {
    std::vector<uint8_t> out(rows * K, 0);
    const size_t ktiles = K / 16;
    for (size_t r = 0; r < rows; ++r)
        for (size_t k = 0; k < K; ++k)
            out[r * K + k] = src[((r / 16) * ktiles + k / 16) * 256 + (r % 16) * 16 + (k % 16)];
    return out;
}

}  // namespace tin
