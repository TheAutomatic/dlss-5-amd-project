#!/usr/bin/env python3
"""Unpack the CCSplitSwin16H* and CCDecInputUpsample layers to canonical [N][K].

Same slice order as the ViT family: a slice is 16 output channels x 32 K in
mma.m16n8k32 B-fragment order, and slices run **N-fastest**,
`slice = k_group*(N/16) + n_group`. Confirmed from the PTX, where the K step
appears directly as the immediate 16384 = 32 slices = N/16 for N=512
(the weight bases of `cc_split_swin_16h_proj_512_fp8` and
`cc_split_swin_16h_ffwd_512_fp8`), and empirically by per-output-channel norm.

Region layouts:

    CCSplitSwin16HFfwd       524288  two [512][512], at 0 and 262144
    CCSplitSwin16HFfwdProj   263168  [512][512] + FP16[512] skip weight
    CCSplitSwin16HProj       263168  same
    CCSplitSwin16HProjPool   263168  same
    CCSplitSwin16HQKVAttn    917568  [1536][512] QKV + 16 x 64x64 FP16 bias + 64
    CCSplitSwin16HFinalHead  524304  [1024][512] + 16

`CCSplitSwin16HFfwd` is a gated FFN and holds **two separate matrices**, not one
[1024][512]. Reading it as a single tensor mixes them: the merged row norm comes
out at sqrt((1^2 + 2.234^2)/2) = 1.731, which is exactly the 1.730 measured, and
splitting them gives 0.9996 (CV 0.2%) for the first and a reproducible 2.234 for
the second.

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

# type -> list of parts: (name, kind, offset, N, K) for matrices,
#                        (name, kind, offset, count, None) for fp16 / raw
LAYOUT = {
    "CCSplitSwin16HFfwd": [
        ("ffwd_a", "matrix", 0, 512, 512),
        ("ffwd_b", "matrix", 262144, 512, 512),
    ],
    "CCSplitSwin16HFfwdProj": [
        ("weight", "matrix", 0, 512, 512),
        ("skip_weight", "fp16", 262144, 512, None),
    ],
    "CCSplitSwin16HProj": [
        ("weight", "matrix", 0, 512, 512),
        ("skip_weight", "fp16", 262144, 512, None),
    ],
    "CCSplitSwin16HProjPool": [
        ("weight", "matrix", 0, 512, 512),
        ("skip_weight", "fp16", 262144, 512, None),
    ],
    "CCSplitSwin16HQKVAttn": [
        ("qkv", "matrix", 0, 1536, 512),
        ("attn_pos_bias", "fp16", 786432, 65536, None),
        ("tail", "raw", 917504, 64, None),
    ],
    "CCSplitSwin16HFinalHead": [
        ("weight", "matrix", 0, 1024, 512),
        ("tail", "raw", 524288, 16, None),
    ],
    # Same N-fastest rule: the K step 16384 = 32 slices = N/16 for N=512 appears
    # directly in cc_dec_input_upsample_1024_512_fp8's address immediates.
    "CCDecInputUpsample": [
        ("weight", "matrix", 0, 512, 1024),
        ("skip_weight", "fp16", 524288, 512, None),
    ],
}


def deswizzle(blob, base, N, K):
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
                # blind to a permutation inside a row, which is how this
                # survived every statistical check until the gold reference.
                for byte in range(8):
                    # Verified against the oracle, not deduced:
                    # `cc_split_swin_16h_ffwd_proj_512_fp8` reproduces NVIDIA's
                    # output for block23.layer1 at 98.2% of bytes with this k
                    # order and 2.2% with the two four-valued components
                    # transposed. Every per-output-channel norm is identical
                    # either way, so no statistic could have chosen between them.
                    #
                    # `artifacts/unpacked-splitswin/` predates this check and was
                    # written by a version with them transposed - regenerate it
                    # rather than trusting what is on disk.
                    k = 8 * (byte >> 1) + 2 * (lane % 4) + (byte & 1)
                    out[n * K + kg * FRAG_K + k] = blob[src + byte]
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
    return {"norm_mean": mean, "norm_cv": (var ** 0.5 / mean) if mean else None,
            "nan_bytes": nan}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--artifacts", type=Path, default=Path("artifacts"))
    ap.add_argument("--output", type=Path, default=Path("artifacts/unpacked-splitswin"))
    ap.add_argument("--stats", action="store_true")
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
        parts = LAYOUT[l["type"]]
        p = blob[r["payload_offset"]:r["payload_offset"] + r["payload_size"]]
        end = 0
        for _, kind, off, a, b in parts:
            end = max(end, off + (a * b if kind == "matrix" else
                                  a * (2 if kind == "fp16" else 1)))
        rest = len(p) - end
        slack += rest
        entry = {"record": r["name"], "type": l["type"], "payload_size": len(p),
                 "unaccounted_bytes": rest, "tensors": []}
        for name, kind, off, a, b in parts:
            fn = "%s.%s.bin" % (r["name"], name)
            if kind == "matrix":
                canon = deswizzle(p, off, a, b)
                (args.output / fn).write_bytes(canon)
                t = {"name": name, "kind": kind, "N": a, "K": b, "file": fn,
                     "layout": "canonical [N][K] e4m3", "src_offset": off}
                if args.stats:
                    t.update(stats(canon, a, b))
            elif kind == "fp16":
                raw = p[off:off + 2 * a]
                (args.output / fn).write_bytes(raw)
                v = struct.unpack("<%de" % a, raw)
                rms = math.sqrt(sum(x * x for x in v) / len(v))
                t = {"name": name, "kind": kind, "count": a, "file": fn,
                     "src_offset": off, "rms": rms, "min": min(v), "max": max(v),
                     "all_non_positive": all(x <= 0 for x in v)}
                if name == "skip_weight":
                    t["mp_sum_branch"] = math.sqrt(max(0.0, 1 - rms * rms))
            else:
                (args.output / fn).write_bytes(p[off:off + a])
                t = {"name": name, "kind": kind, "bytes": a, "file": fn,
                     "src_offset": off}
            entry["tensors"].append(t)
        manifest.append(entry)

    (args.output / "manifest.json").write_text(json.dumps(
        {"layout_note": "slice = 16 channels x 32 K, N-fastest: "
                        "slice = k_group*(N/16) + n_group.",
         "layers": manifest}, indent=1) + "\n")
    print("unpacked %d CCSplitSwin16H*/CCDecInputUpsample layers, skipped %d others"
          % (len(manifest), skipped))
    print("unaccounted bytes across the family: %d" % slack)
    seen = set()
    for e in manifest:
        if e["type"] in seen:
            continue
        seen.add(e["type"])
        print("\n%-26s %-22s unaccounted=%d" % (e["type"], e["record"],
                                                e["unaccounted_bytes"]))
        for t in e["tensors"]:
            if t["kind"] == "matrix" and "norm_mean" in t:
                print("   %-14s N=%-5d K=%-5d norm %.5f  CV %.5f"
                      % (t["name"], t["N"], t["K"], t["norm_mean"], t["norm_cv"]))
            elif t["kind"] == "matrix":
                print("   %-14s N=%-5d K=%-5d" % (t["name"], t["N"], t["K"]))
            elif t["kind"] == "fp16":
                extra = ("-> mp_sum branch %.4f" % t["mp_sum_branch"]) \
                    if "mp_sum_branch" in t else \
                    ("all non-positive=%s" % t["all_non_positive"])
                print("   %-14s n=%-6d rms %.4f  %s" % (t["name"], t["count"],
                                                        t["rms"], extra))
            else:
                print("   %-14s %d raw bytes at +%d" % (t["name"], t["bytes"],
                                                        t["src_offset"]))
    print("\nwrote", args.output / "manifest.json")


if __name__ == "__main__":
    main()
