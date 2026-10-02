#!/usr/bin/env python3
"""Unpack the pre-block image adapter's weight record.

`unpack_swin_family.py` skips this layer twice over: it filters the type
to `CCTinlayoutFusedSwin\\dH`, and its resampling test wants
`max(ci, co) == 2 * min(ci, co)` where this layer reports ci=3, co=32.

The record is the C=32 equal-width fused Swin block with **one extra 1024-byte
region inserted after `mlp_contract`** - the position the region law gives a
resample in its *upsample* branch, i.e. a prologue rather than an epilogue:

    4096 + 4096 + 1024 + 96 + 3072 + 8192 + 16 + 1024 + 80 = 21696

and 21696 is the record. The plan's ci=3 counts the *texture's* colour channels;
the tensor the block itself consumes is 32 wide.

**The extra region is FP16, not e4m3**, which is the whole point of it: every
other matrix in the record is e4m3 because its operand is a quantised
activation, and this one's operand comes out of a texture. 512 halves in
[-0.9497, +0.8687], mean -0.022, 259 of them negative - a trained matrix, read
as **[32][16]**: sixteen input features lifted to the block's 32 channels.

## How the map was found, and how to check one

Two placements of a 1024-byte region close on 21696 - after `mlp_contract` and
after `attn_residual_scale` - so closing is not evidence. What settles it is a
**calibrated scan**: slide a [96][32] window over the record and keep the
offsets whose column L2 norms look like a trained QKV. The same `deswizzle` on a
known-good C=32 record (`artifacts/gold/inputs/w_full.bin`) gives median 1.56 and
max 2.65, matching `block1.layer0.layer.qkv.bin` from the verified path, so the
decoder is calibrated before it is trusted. On this record exactly one offset
matches: **+9312**, which is the prologue placement.

Read at the epilogue placement instead, QKV lands on the FP16 region and its
column norms come out at 593 even / 3.4 odd - a 150x alternation that is what an
f16 array looks like when it is read as bytes, and which no trained matrix does.

**The NaN census is the cheap check.** 0x7F and 0xFF are e4m3 NaN and a trained
matrix has none - `w_full.bin` has zero in every matrix region. Under the map
below all four e4m3 matrices are clean; the FP16 region has four, as an f16
array naturally does.

usage: unpack_preblock.py [record.bin] [outdir]
"""
import math
import pathlib
import sys

sys.path[:0] = [str(pathlib.Path(__file__).resolve().parents[1] / d) for d in ('build', 'analysis', 'reference', 'rounds')]
from unpack_swin_family import deswizzle  # noqa: E402

C, HEADS = 32, 1
H = 4 * C

# (name, start, end, N, K, inner, kind)
REGIONS = [
    ("mlp_expand",          0,     4096,  H,     C,     H // 16,     "matrix"),
    ("mlp_contract",        4096,  8192,  C,     H,     C // 16,     "matrix"),
    ("input_lift",          8208,  9232,  32,    16,    None,        "fp16"),
    ("residual_scale",      9232,  9312,  None,  None,  None,        "scalar"),
    ("qkv",                 9312,  12384, 3 * C, C,     3 * C // 16, "matrix"),
    ("attn_pos_bias",       12384, 20576, None,  None,  None,        "fp16"),
    ("scalars_b",           20576, 20592, None,  None,  None,        "f32"),
    ("attn_out_proj",       20592, 21616, C,     C,     C // 16,     "matrix"),
    ("attn_residual_scale", 21616, 21696, None,  None,  None,        "scalar"),
]

E4 = []
for _b in range(256):
    _s = -1.0 if _b & 0x80 else 1.0
    _e, _m = (_b >> 3) & 0xF, _b & 7
    E4.append(0.0 if (_e == 15 and _m == 7)
              else (_s * _m * 2 ** -9 if _e == 0 else _s * (1 + _m / 8.0) * 2 ** (_e - 7)))


def main():
    src = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                       else "artifacts/oracle/block0.layer0.layer.bin")
    out = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "artifacts/unpacked-preblock")
    out.mkdir(parents=True, exist_ok=True)
    p = src.read_bytes()
    assert REGIONS[-1][2] == len(p), (
        "region map gives %d, record is %d" % (REGIONS[-1][2], len(p)))

    base = "block0.layer0.layer"
    print("%-20s %6s %6s  %-10s %10s %s" % ("region", "start", "bytes", "shape",
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
            print("%-20s %6d %6d  [%d][%d]%s %10.3f %d%s"
                  % (name, a, b - a, N, K, " " * max(0, 6 - len(str(N)) - len(str(K))),
                     col[len(col) // 2], nan, "   <-- BAD" if nan else ""))
        else:
            (out / ("%s.%s.bin" % (base, name))).write_bytes(p[a:b])
            print("%-20s %6d %6d  %-10s" % (name, a, b - a, kind))
    print()
    print("wrote %d files to %s" % (len(REGIONS), out))
    if bad:
        print("** %d e4m3 matrix regions contain NaN: the region map is wrong **" % bad)
        return 1
    print("no NaN in any e4m3 matrix region, and the column norms are a trained "
          "matrix's - the boundaries hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
