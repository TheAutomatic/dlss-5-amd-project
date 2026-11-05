// SPDX-License-Identifier: MIT
// D3D12 path (D3D_GUIDES: d3d_enc0_attr_f, d3d_ae): a guide the game gave as a texture view of set 0 is read there,
// through the conversion of ingest.comp, so every value is the one the ingest would have stored in its guide buffer and
// the ingest does not run for it. A guide that had to be copied is still ingested and read from its buffer; an absent
// one is zeros, as the ingest writes it.
#extension GL_EXT_samplerless_texture_functions : require
layout(set = 0, binding = 0) uniform texture2D tex[16];
// GuideTable (engine.hpp): ingest's words per guide (view, pitch, format | kind << 8 | flags << 16 | channel << 24,
// origin), the depth projection and sign, the guide buffers of the ingested ones
layout(buffer_reference, scalar, buffer_reference_align = 8) restrict readonly buffer GuideTab {
    uvec4 g[8];
    vec4 proj;
    float depth_sign;
    uint pad;
    uint64_t buf[8];
};
#define GS_COLOR 0u
#define GS_ALBEDO 1u
#define GS_SPEC 2u
#define GS_NORMALS 3u
#define GS_ROUGH 4u
#define GS_DEPTH 5u
#define GS_MV 6u
#define GS_HIT 7u

uint gflags(GuideTab t, uint s) { return (t.g[s].z >> 16) & 255u; }
vec4 gtex(GuideTab t, uint s, ivec2 p) {
    uvec4 g = t.g[s];
    if ((((g.z >> 16) & 255u) & 4u) != 0u) return vec4(0.0);
    return texelFetch(tex[g.x], ivec2(g.w & 65535u, g.w >> 16) + p, 0);
}
// the guide is read from the game's texture (or is absent), not from its buffer
bool gdirect(GuideTab t, uint s) { return (gflags(t, s) & 12u) != 0u; }

// K_RGBA16F
f16vec4 g_rgba16f(GuideTab t, uint s, ivec2 p, int w) {
    if (gdirect(t, s)) return f16vec4(clamp(gtex(t, s, p), -65504.0, 65504.0));
    return F16x4Buf(t.buf[s]).v[p.y * w + p.x];
}
// K_RGBA8
uint g_rgba8(GuideTab t, uint s, ivec2 p, int w) {
    if (gdirect(t, s)) return packUnorm4x8(gtex(t, s, p));
    return U32Buf(t.buf[s]).v[p.y * w + p.x];
}
// K_NORMALS
f16vec4 g_normals(GuideTab t, ivec2 p, int w) {
    if (!gdirect(t, GS_NORMALS)) return F16x4Buf(t.buf[GS_NORMALS]).v[p.y * w + p.x];
    vec4 c = gtex(t, GS_NORMALS, p);
    if ((gflags(t, GS_NORMALS) & 1u) != 0u) c.xyz = c.xyz * 2.0 - 1.0;
    return f16vec4(clamp(c, -65504.0, 65504.0));
}
// K_R16F
float16_t g_r16f(GuideTab t, uint s, ivec2 p, int w) {
    if (gdirect(t, s)) return float16_t(clamp(gtex(t, s, p)[t.g[s].z >> 24], -65504.0, 65504.0));
    return F16Buf(t.buf[s]).v[p.y * w + p.x];
}
// K_DEPTH
float g_depth(GuideTab t, ivec2 p, int w) {
    if (!gdirect(t, GS_DEPTH)) return F32Buf(t.buf[GS_DEPTH]).v[p.y * w + p.x];
    float d = gtex(t, GS_DEPTH, p).x;
    if ((gflags(t, GS_DEPTH) & 2u) != 0u) {
        float den = d * t.proj.y - t.proj.x;
        d = abs(den) > 1e-20 ? (t.proj.z - d * t.proj.w) / den * t.depth_sign : 1e30;
    }
    return d;
}
// K_RG16F
f16vec2 g_rg16f(GuideTab t, uint s, ivec2 p, int w) {
    if (gdirect(t, s)) return f16vec2(clamp(gtex(t, s, p).xy, -65504.0, 65504.0));
    return F16x2Buf(t.buf[s]).v[p.y * w + p.x];
}
