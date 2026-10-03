#!/usr/bin/env python3
"""Unpack CCTinlayoutFusedSwin{1,2}H layers into canonical [N][K] FP8 tensors.

Supersedes an earlier C=32-only unpacker. The region map and
the intra-region slice order are those recovered by an address trace, whose
address model reproduces the hand-derived C=32 map exactly and covers 99.5% of
the C=64 payload - everything except the two scalar regions, which are read by
scalar loads rather than fragment loads.

Region law (same order at every C; heads = C/32):

    conv                    C*H  +  C*H/heads  +  C*C     (H = 4C)   [1]
    residual_scale          2C + 32                       MMA C operand, at +16
    QKV                     3*C*C
    attn_pos_bias           heads * 8192                  MMA C operand, FP16
    scalars_b               16                            one FP32
    attn_out_proj           C*C
    out_gain                2C + 16                       FP16[C], max exactly 1

    [1] at heads == 1 the middle grouped conv is absent and conv = 2*C*H, which
        is why the C=32 operator trace has four convolutions and C=64 has five.

Slice order inside a matrix region, uniform across the family:

    off = base + n_inner*512 + k_group*(512*n_inner_count)
               + n_outer*(512*n_inner_count*K/32)

`n_inner_count` is the number of 16-channel output tiles in one head's share of
that matrix; `n_outer` indexes the heads. Verified by accumulator-chain analysis
in the PTX, not assumed.

Standard library only. The original DLL is never executed.
"""
import argparse
import json
import re
import struct
from pathlib import Path

SLICE = 512
FRAG_N, FRAG_K = 8, 32

E4M3 = []
for _b in range(256):
    _s = -1.0 if _b & 0x80 else 1.0
    _e, _m = (_b >> 3) & 0xF, _b & 0x7
    if _e == 0xF and _m == 0x7:
        E4M3.append(float("nan"))
    elif _e == 0:
        E4M3.append(_s * _m * 2.0 ** -9)
    else:
        E4M3.append(_s * (1.0 + _m / 8.0) * 2.0 ** (_e - 7))


def regions(C, heads, c_in=None, c_out=None):
    """Byte layout of one fused Swin layer, equal-width or resampling.

    Returns [(name, start, end, N, K, n_inner_count, kind)]. `n_inner_count` is
    None for scalar regions. `C` is the working width, which for a resampling
    layer is min(c_in, c_out).

    A resampling layer is the equal-width layer plus one extra [c_out][c_in]
    matrix, and the two directions place it differently - recovered from the
    traced coverage of the `_ds` and `_upsample` kernels, not fitted:

      downsample  ... attn_out_proj, attn_residual_scale (2C, no padding),
                  resample, then 16 bytes only at C=32
      upsample    conv, resample, residual_scale, an extra FP16[C] gain,
                  then QKV onward as usual

    Both close exactly on all four widths in each direction.
    """
    H = 4 * C
    out, off = [], 0

    def add(name, size, N=None, K=None, inner=None, kind="matrix"):
        nonlocal off
        out.append((name, off, off + size, N, K, inner, kind))
        off += size

    # Only the MLP path is blocked into per-head chunks - that blocking is what
    # the kernel's 2-iteration loop walks. QKV, the position bias and the output
    # projection are stored flat, so their inner tile count is just N/16.
    if heads == 1:
        add("mlp_expand", C * H, H, C, H // 16)
        add("mlp_contract", C * H, C, H, C // 16)
    else:
        add("mlp_expand", C * H, H, C, (H // heads) // 16)
        add("mlp_mid", C * H // heads, C, H // heads, (C // heads) // 16)
        add("mlp_contract", C * C, C, C, C // 16)
    resample = (c_in * c_out) if (c_in and c_in != c_out) else 0
    up = bool(resample) and c_out < c_in
    if up:
        add("resample", resample, c_out, c_in, c_out // 16)
    add("residual_scale", 2 * C + 32, kind="scalar")
    if up:
        add("upsample_gain", 2 * C if C == 32 else 2 * C - 32, kind="scalar")
    add("qkv", 3 * C * C, 3 * C, C, (3 * C) // 16)
    add("attn_pos_bias", heads * 8192, kind="fp16")
    # One FP32 per head, read at `+base + 4*tid_y`, with a 16-byte floor: C=32
    # uses 1 of 4 slots, C=64 uses 2, C=128 uses all 4, and C=256 needs 8 = 32
    # bytes. That is exactly the 16 bytes by which the C=256 layers previously
    # failed to close.
    add("scalars_b", max(16, 4 * heads), kind="f32")
    add("attn_out_proj", C * C, C, C, C // 16)
    # Not a post-projection gain: a row trace shows this array is read
    # at +base + f*16 + (laneid%4)*4 and multiplies the *skip* on its way into
    # the output projection's MMA C operand, exactly as residual_scale does for
    # the MLP. The projection output is never multiplied by it.
    if resample and not up:
        # Downsample drops this region's 16 bytes of padding and puts the
        # resample matrix after it; the padding reappears as a tail only at
        # C=32. Traced from the `_ds` kernels' coverage at C=32 and C=64.
        add("attn_residual_scale", 2 * C, kind="scalar")
        add("resample", resample, c_out, c_in, c_out // 16)
        if C == 32:
            add("tail", 16, kind="scalar")
    else:
        add("attn_residual_scale", 2 * C + 16, kind="scalar")
    return out


def deswizzle(blob, base, N, K, inner, row_order="t-major"):
    """Fragment order -> canonical [N][K].

    Lane l of an mma.m16n8k32 B fragment holds output channel n = l/4 and
    k = (l%4)*8 .. +7; a 16 B/lane load carries two consecutive fragments
    interleaved at 8-byte granularity, so one 512 B slice is 16 channels x 32 K.

    The accumulator chains prove the two fragments of a slice are two *different*
    8-channel blocks, but not which of the two comes first. `t-major` is the
    answer, and it is **deduced, not chosen**: a row trace correlates
    each contract chain's C operand with its residual-scale offset, and the
    scale array is indexed by output channel, so the offset names the channels.

        tile 0, fragment 0 -> scale +8208 -> channels 0-7
        tile 0, fragment 1 -> scale +8224 -> channels 8-15
        tile 1, fragment 0 -> scale +8240 -> channels 16-23
        tile 1, fragment 1 -> scale +8256 -> channels 24-31

    i.e. row = t*16 + half*8 + n. `half-major` is kept only so the alternative
    can be re-tested; do not use it. A per-output-channel norm statistic cannot
    see this choice at all (it is a set property), and the rung-0 magnitude
    heuristic actively prefers the wrong one - a clean case of statistics
    corroborating a deduction rather than making it.
    """
    out = bytearray(N * K)
    kg = K // FRAG_K
    st_k = SLICE * inner
    st_o = st_k * kg
    for t in range(N // 16):
        for g in range(kg):
            off = base + (t % inner) * SLICE + g * st_k + (t // inner) * st_o
            for half in range(2):
                for n in range(FRAG_N):
                    for k in range(FRAG_K):
                        lane = 4 * n + (k % 8) // 2
                        byte = 2 * (k // 8) + (k % 2)
                        src = off + lane * 16 + half * 8 + byte
                        row = (t * 16 + half * FRAG_N + n
                               if row_order == "t-major"
                               else half * (N // 2) + t * FRAG_N + n)
                        out[row * K + g * FRAG_K + k] = blob[src]
    return bytes(out)


def stats(mat, N, K):
    norms, nan = [], 0
    for n in range(N):
        acc = 0.0
        for k in range(K):
            v = E4M3[mat[n * K + k]]
            if v != v:
                nan += 1
                continue
            acc += v * v
        norms.append(acc ** 0.5)
    mean = sum(norms) / len(norms)
    var = sum((x - mean) ** 2 for x in norms) / len(norms)
    return {"rows": N, "cols": K, "nan_bytes": nan, "norm_mean": mean,
            "norm_cv": (var ** 0.5 / mean) if mean else None}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--artifacts", type=Path, default=Path("artifacts"))
    ap.add_argument("--output", type=Path, default=Path("artifacts/unpacked"))
    ap.add_argument("--row-order", choices=("t-major", "half-major"),
                    default="t-major",
                    help="which 8-channel block a slice's two B fragments are")
    ap.add_argument("--max-c", type=int, default=64,
                    help="widest C to unpack; C>=128 is not yet resolved")
    args = ap.parse_args()

    inv = json.loads((args.artifacts / "inventory/manifest.json").read_text())
    graph = json.loads((args.artifacts / "graph/descriptor.json").read_text())
    blob = (args.artifacts / "inventory/weights_ht.bin").read_bytes()
    layers = {(b["index"], l["index"]): l
              for b in graph["blocks"] for l in b["layers"]}
    args.output.mkdir(parents=True, exist_ok=True)

    manifest, skipped = [], 0
    for r in inv["weights"]:
        m = re.fullmatch(r"block(\d+)\.layer(\d+)\.layer", r["name"])
        if not m:
            continue
        l = layers[(int(m[1]), int(m[2]))]
        ci, co = l["input_channels"], l["output_channels"]
        C = min(ci, co)
        if not re.fullmatch(r"CCTinlayoutFusedSwin\dH", l["type"]) or C > args.max_c \
                or (ci != co and max(ci, co) != 2 * C):
            skipped += 1
            continue
        heads = C // 32
        regs = regions(C, heads, ci, co)
        total = regs[-1][2]
        p = blob[r["payload_offset"]:r["payload_offset"] + r["payload_size"]]
        if total != len(p):
            print("SKIP %s: region law gives %d, payload is %d"
                  % (r["name"], total, len(p)))
            skipped += 1
            continue

        entry = {"record": r["name"], "block": int(m[1]), "layer_name": l["name"],
                 "type": l["type"], "C": C, "heads": heads,
                 "in_channels": ci, "out_channels": co,
                 "payload_size": len(p), "tensors": []}
        for name, a, b, N, K, inner, kind in regs:
            fn = "%s.%s.bin" % (r["name"], name)
            if kind == "matrix":
                canon = deswizzle(p, a, N, K, inner, args.row_order)
                (args.output / fn).write_bytes(canon)
                t = {"name": name, "kind": kind, "layout": "canonical [N][K] e4m3",
                     "N": N, "K": K, "bytes": len(canon), "file": fn,
                     "src_range": [a, b]}
                t.update(stats(canon, N, K))
            else:
                chunk = p[a:b]
                (args.output / fn).write_bytes(chunk)
                t = {"name": name, "kind": kind, "bytes": len(chunk), "file": fn,
                     "src_range": [a, b]}
                if kind == "f32":
                    t["values"] = list(struct.unpack("<%df" % (len(chunk) // 4), chunk))
                else:
                    v = struct.unpack("<%de" % (len(chunk) // 2), chunk)
                    t.update({"count": len(v), "min": min(v), "max": max(v),
                              "all_non_positive": all(x <= 0 for x in v)})
            entry["tensors"].append(t)
        manifest.append(entry)

    (args.output / "manifest.json").write_text(json.dumps(
        {"layout_note": "Region law and slice order recovered by an address trace. "
                        "C>=128 is not covered: one launch of "
                        "those kernels reads only part of the payload.",
         "layers": manifest}, indent=1) + "\n")

    print("unpacked %d equal-width fused Swin layers, skipped %d" % (len(manifest), skipped))
    seen = set()
    for e in manifest:
        key = (e["C"], e["in_channels"] == e["out_channels"],
               e["out_channels"] < e["in_channels"])
        if key in seen:
            continue
        seen.add(key)
        print("\n%s  %d->%d  W=%d heads=%d  payload=%d"
              % (e["record"], e["in_channels"], e["out_channels"], e["C"],
                 e["heads"], e["payload_size"]))
        print("  %-16s %-16s %-6s %-6s %-10s %-9s %s"
              % ("tensor", "range", "N", "K", "norm_mean", "norm_cv", "nan"))
        for t in e["tensors"]:
            if t["kind"] == "matrix":
                print("  %-16s [%6d,%6d) %-6d %-6d %-10.5f %-9.5f %d"
                      % (t["name"], t["src_range"][0], t["src_range"][1],
                         t["N"], t["K"], t["norm_mean"], t["norm_cv"], t["nan_bytes"]))
            else:
                print("  %-16s [%6d,%6d) %-6s %-6s %s"
                      % (t["name"], t["src_range"][0], t["src_range"][1], "-", "-",
                         t.get("values", "min=%.4g max=%.4g" %
                               (t.get("min", 0), t.get("max", 0)))))
    print("\nwrote", args.output / "manifest.json")


if __name__ == "__main__":
    main()
