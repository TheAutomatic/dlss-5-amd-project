#!/usr/bin/env python3
"""Unpack the post-block record using its FP8 kernel's parameter accesses.

cc_tinlayout_fused_post_block_swin_1h_32_fp8 uses three FP16[32] vectors:
8208 is the MLP residual gain, 8272 scales upsampled main, and 8336 scales
enc0 skip. The main and skip products are separately rounded, added, and fed
to the Swin body. The old 8192..8304 "head" region split these arrays wrongly.

The 20784..21808 output projection is packed m16n8k16 B operands: two 16-K
blocks, each lane holding two 8-output fragments. Decode to FP16[16][32].
These offsets and arithmetic come from PTX; full image equivalence still
needs a matching NVIDIA post-block replay.

Usage: unpack_postblock.py [record.bin] [outdir]
"""
import math
import pathlib
import sys

sys.path[:0] = [str(pathlib.Path(__file__).resolve().parents[1] / d) for d in ('build', 'analysis', 'reference', 'rounds')]
from unpack_swin_family import deswizzle  # noqa: E402

C = 32
H = 4 * C

# (name, start, end, N, K, inner, kind)
REGIONS = [
    ("mlp_expand",          0,     4096,  H,     C,     H // 16,     "matrix"),
    ("mlp_contract",        4096,  8192,  C,     H,     C // 16,     "matrix"),
    ("residual_scale",      8208,  8272,  None,  None,  None,        "scalar"),
    ("main_gain",           8272,  8336,  None,  None,  None,        "fp16"),
    ("skip_gain",           8336,  8400,  None,  None,  None,        "fp16"),
    ("qkv",                 8400,  11472, 3 * C, C,     3 * C // 16, "matrix"),
    ("attn_pos_bias",       11472, 19664, None,  None,  None,        "fp16"),
    ("scalars_b",           19664, 19680, None,  None,  None,        "f32"),
    ("attn_out_proj",       19680, 20704, C,     C,     C // 16,     "matrix"),
    ("attn_residual_scale", 20704, 20784, None,  None,  None,        "scalar"),
    ("out_project",         20784, 21808, 16,    32,    None,        "fp16"),
]

E4 = []
for _b in range(256):
    _s = -1.0 if _b & 0x80 else 1.0
    _e, _m = (_b >> 3) & 0xF, _b & 7
    E4.append(0.0 if (_e == 15 and _m == 7)
              else (_s * _m * 2 ** -9 if _e == 0 else _s * (1 + _m / 8.0) * 2 ** (_e - 7)))


def f16_at(blob, i):
    h = (blob[2 * i + 1] << 8) | blob[2 * i]
    s = -1.0 if h & 0x8000 else 1.0
    e, m = (h >> 10) & 0x1F, h & 0x3FF
    if e == 0:
        return s * m * 2 ** -24
    if e == 31:
        return 0.0
    return s * (1 + m / 1024.0) * 2 ** (e - 15)


def main():
    src = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                       else "artifacts/oracle/block70.layer0.layer.bin")
    out = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "artifacts/unpacked-postblock")
    out.mkdir(parents=True, exist_ok=True)
    p = src.read_bytes()
    assert REGIONS[-1][2] == len(p), (
        "region map gives %d, record is %d" % (REGIONS[-1][2], len(p)))

    base = "block70.layer0.layer"
    print("%-20s %6s %6s  %-11s %8s %s" % ("region", "start", "bytes", "shape",
                                           "col-norm", "e4m3 NaN"))
    bad = 0
    for name, a, b, N, K, inner, kind in REGIONS:
        if kind == "matrix":
            blob = deswizzle(p, a, N, K, inner)
            (out / ("%s.%s.bin" % (base, name))).write_bytes(blob)
            nan = sum(1 for x in blob if x in (0x7F, 0xFF))
            col = sorted(math.sqrt(sum(E4[blob[n * K + k]] ** 2 for n in range(N)))
                         for k in range(K))
            bad += bool(nan)
            print("%-20s %6d %6d  [%d][%d]%s %8.3f %d%s"
                  % (name, a, b - a, N, K, " " * max(0, 7 - len(str(N)) - len(str(K))),
                     col[len(col) // 2], nan, "   <-- BAD" if nan else ""))
        elif name == "out_project":
            # Two 16-K blocks; each lane holds two m16n8k16 B fragments.
            raw=p[a:b];canon=bytearray(len(raw))
            for n in range(16):
                for k in range(32):
                    i=(k//16)*256+32*(n%8)+8*((k%8)//2)+4*(n//8)+k%2+2*((k%16)//8)
                    canon[2*(n*32+k):2*(n*32+k)+2]=raw[2*i:2*i+2]
            (out/("%s.%s.bin"%(base,name))).write_bytes(canon)
        else:
            (out / ("%s.%s.bin" % (base, name))).write_bytes(p[a:b])
            print("%-20s %6d %6d  %-11s" % (name, a, b - a, kind))

    decoded=(out/(base+".out_project.bin")).read_bytes()
    v = [f16_at(decoded, i) for i in range(512)]
    rows = [math.sqrt(sum(v[n * 32 + k] ** 2 for k in range(32))) for n in range(16)]
    live = [n for n, r in enumerate(rows) if r > 1e-6]
    print()
    print("out_project as FP16[16][32], row norms:")
    print("   " + " ".join("%5.2f" % r for r in rows))
    print("   live output features: %s  (%d of 16)" % (live, len(live)))
    print()
    print("wrote %d files to %s" % (len(REGIONS), out))
    if bad:
        print("** %d e4m3 matrix regions contain NaN: the region map is wrong **" % bad)
        return 1
    print("no NaN in any unpacked e4m3 matrix (not a validity proof)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
