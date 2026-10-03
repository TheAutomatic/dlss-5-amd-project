#!/usr/bin/env python3
"""Unpack the CCVit1D* layers into canonical [N][K] FP8 tensors.

Layout, recovered in session 3:

  * A slice is **16 output channels x 32 K**, the same fragment order as the
    fused Swin family. An earlier "8 x 64" reading was an artefact of the ViT
    MMAs accumulating in place (D == C), which let one chain's two entries come
    from two different runtime bases; separating the bases resolves it.
  * Slice order is **N-fastest**: `slice = k_group * (N/16) + n_group`. The K
    step is therefore `N/16` slices, and for `cc_vit_1d_projection_fp8`
    (N = 1024) that is 64 slices = 32768 bytes - exactly the difference between
    the two weight bases in the PTX. Deduction and statistics agree.
  * `CCVit1DQKV` carries a **128-byte header**; its matrix starts at byte 128.
    Across the 8 ViT blocks that is 1024 bytes, which is what the coverage model
    had been reporting as slack.

Verification, per-output-channel L2 norm on the two tensors that are unit
normalised:

    CCVit1DQKV        3072 x 1024   norm 0.9996   CV 0.0015
    CCVit1DFfnExpand  4096 x 1024   norm 0.9995   CV 0.0016

an order of magnitude sharper than any alternative ordering. `CCVit1DProjection`
and `CCVit1DFfnContract` are not unit normalised, because they feed a residual
add: their norm is the mp_sum branch weight, partner to the trailing FP16[cout],
`|W| / sqrt(1 - rms(tail)^2)` = 0.934 to 0.965 over three blocks each. That tail
is the **skip weight**, not a dequantisation scale.

Standard library only. The original DLL is never executed.
"""
import argparse
import json
import math
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

# type -> (N, K, header bytes, trailing FP16 count, sub-tensor split)
LAYOUT = {
    "CCVit1DQKV":         (3072, 1024, 128, 0, 3),
    "CCVit1DFfnExpand":   (4096, 1024, 0, 0, 1),
    "CCVit1DFfnContract": (1024, 4096, 0, 1024, 1),
    "CCVit1DProjection":  (1024, 1024, 0, 1024, 1),
}


def deswizzle(blob, base, N, K):
    """Slice `k_group*(N/16) + n_group`, each 16 channels x 32 K in B-fragment order."""
    out = bytearray(N * K)
    ngrp, kgrp = N // 16, K // FRAG_K
    for s in range(ngrp * kgrp):
        ng, kg = s % ngrp, s // ngrp
        off = base + s * SLICE
        for half in range(2):
            for lane in range(32):
                n = ng * 16 + half * FRAG_N + lane // 4
                src = off + lane * 16 + half * 8
                # A B fragment lays k out the way the accumulator lays out n:
                # k = 8*(byte>>1) + 2*(lane%4) + (byte&1). The 8 bytes a lane
                # holds are NOT 8 consecutive k. A per-output-channel norm is
                # blind to a permutation inside a row, which is how the wrong
                # order survived every statistical check until an oracle existed.
                #
                # This order is now **verified**, not deduced. Reading out single
                # weight columns with a one-hot activation says activation
                # channel 5 is weight column 5 and channel 37 is column 37, and
                # with it the CPU reference reproduces NVIDIA's own output for
                # cc_vit_1d_projection_fp8 to 98.7% of bytes exactly (the rest
                # being one-ULP MMA reduction-order differences).
                #
                # `artifacts/unpacked-vit/` predates this and was written by an
                # older version with the two four-valued components swapped;
                # regenerate it rather than trusting what is on disk.
                for byte in range(8):
                    k = 8 * (byte >> 1) + 2 * (lane % 4) + (byte & 1)
                    out[n * K + kg * FRAG_K + k] = blob[src + byte]
    return bytes(out)


def deswizzle_qkv(blob):
    """Q/K/V alternate in 1024-byte tiles (cc_vit_1d_qkv_fp8)."""
    out = bytearray(3 * 1024 * 1024)
    for which in range(3):
        for row in range(1024):
            for k in range(1024):
                b = ((k & 1) | ((k >> 1 & 1) << 4) | ((k >> 2 & 1) << 5)
                     | ((k >> 3 & 1) << 1) | ((k >> 4 & 1) << 2)
                     | ((row & 1) << 6) | ((row >> 1 & 1) << 7)
                     | ((row >> 2 & 1) << 8) | ((row >> 3 & 1) << 3)
                     | ((row >> 4 & 1) << 9))
                tile = (row >> 5) | ((k >> 5) << 5)
                out[(which * 1024 + row) * 1024 + k] = blob[128 + (3 * tile + which) * 1024 + b]
    return bytes(out)


def stats(mat, N, K, lo=0, hi=None):
    hi = N if hi is None else hi
    norms, nan = [], 0
    for n in range(lo, hi):
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
    return {"norm_mean": mean, "norm_cv": (var ** 0.5 / mean) if mean else None,
            "nan_bytes": nan}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--artifacts", type=Path, default=Path("artifacts"))
    ap.add_argument("--output", type=Path, default=Path("artifacts/unpacked-vit"))
    ap.add_argument("--stats", action="store_true",
                    help="also compute per-output-channel norms (slow)")
    args = ap.parse_args()

    inv = json.loads((args.artifacts / "inventory/manifest.json").read_text())
    graph = json.loads((args.artifacts / "graph/descriptor.json").read_text())
    blob = (args.artifacts / "inventory/weights_ht.bin").read_bytes()
    layers = {(b["index"], l["index"]): l
              for b in graph["blocks"] for l in b["layers"]}
    args.output.mkdir(parents=True, exist_ok=True)

    manifest, skipped, slack = [], 0, 0
    for r in inv["weights"]:
        m = re.fullmatch(r"block(\d+)\.layer(\d+)\.layer", r["name"])
        if not m:
            continue
        l = layers[(int(m[1]), int(m[2]))]
        if l["type"] not in LAYOUT:
            skipped += 1
            continue
        N, K, head, tail, split = LAYOUT[l["type"]]
        p = blob[r["payload_offset"]:r["payload_offset"] + r["payload_size"]]
        want = head + N * K + 2 * tail
        rest = len(p) - want
        slack += rest
        entry = {"record": r["name"], "type": l["type"], "N": N, "K": K,
                 "header_bytes": head, "payload_size": len(p),
                 "unaccounted_bytes": rest, "tensors": []}

        if head:
            fn = "%s.header.bin" % r["name"]
            (args.output / fn).write_bytes(p[:head])
            entry["tensors"].append({"name": "header", "kind": "raw",
                                     "bytes": head, "file": fn})
        mat = deswizzle_qkv(p) if split == 3 else deswizzle(p, head, N, K)
        step = N // split
        for i in range(split):
            nm = ["q", "k", "v"][i] if split == 3 else "weight"
            fn = "%s.%s.bin" % (r["name"], nm)
            (args.output / fn).write_bytes(mat[i * step * K:(i + 1) * step * K])
            t = {"name": nm, "kind": "matrix", "layout": "canonical [N][K] e4m3",
                 "N": step, "K": K, "file": fn}
            if args.stats:
                t.update(stats(mat, N, K, i * step, (i + 1) * step))
            entry["tensors"].append(t)
        if tail:
            fn = "%s.skip_weight.bin" % r["name"]
            raw = p[head + N * K: head + N * K + 2 * tail]
            (args.output / fn).write_bytes(raw)
            v = struct.unpack("<%de" % tail, raw)
            rms = math.sqrt(sum(x * x for x in v) / len(v))
            entry["tensors"].append({
                "name": "skip_weight", "kind": "fp16", "count": tail, "file": fn,
                "rms": rms, "mp_sum_branch": math.sqrt(max(0.0, 1 - rms * rms)),
                "note": "residual skip weight; |W| should be about "
                        "sqrt(1 - rms^2)"})
        manifest.append(entry)

    (args.output / "manifest.json").write_text(json.dumps(
        {"layout_note": "slice = 16 channels x 32 K, N-fastest: "
                        "slice = k_group*(N/16) + n_group.",
         "layers": manifest}, indent=1) + "\n")
    print("unpacked %d CCVit1D* layers, skipped %d others" % (len(manifest), skipped))
    print("unaccounted bytes across the family: %d" % slack)
    seen = set()
    for e in manifest:
        if e["type"] in seen:
            continue
        seen.add(e["type"])
        print("\n%-20s %-22s N=%-5d K=%-5d header=%-4d unaccounted=%d"
              % (e["type"], e["record"], e["N"], e["K"], e["header_bytes"],
                 e["unaccounted_bytes"]))
        for t in e["tensors"]:
            if t["kind"] == "matrix" and "norm_mean" in t:
                print("   %-12s N=%-5d norm %.5f  CV %.5f"
                      % (t["name"], t["N"], t["norm_mean"], t["norm_cv"]))
            elif t["kind"] == "fp16":
                print("   %-12s rms %.4f -> mp_sum branch %.4f"
                      % (t["name"], t["rms"], t["mp_sum_branch"]))
    print("\nwrote", args.output / "manifest.json")


if __name__ == "__main__":
    main()
