#!/usr/bin/env python3
"""Pack the weight files the network reads into one model file.

Format (little-endian): b"NRMODEL1", u32 count, u32 0, then per entry
u32 name length, name (UTF-8, '/' separators, relative to the weights root),
u64 offset, u64 size; then the data, each entry 256-byte aligned. The bytes
are the files' bytes, unchanged; linux/src/core/nr_graph.cpp reads entries on
demand (slurp).

    pack_model.py --root artifacts --list model-files.txt --out dlssnr.bin
    pack_model.py --root DIR --list LIST --out dlssnr.bin --verify model-files.sha256

--verify checks every entry against a "sha256  name" list and fails on any
difference, which is how install-time extraction proves it rebuilt the same
model the package was tested with.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--list", type=Path, required=True, help="relative names, one a line")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--sums", type=Path, help="write 'sha256  name' for every entry here")
    ap.add_argument("--verify", type=Path, help="fail unless every entry matches this sha256 list")
    a = ap.parse_args()

    names = [l.strip() for l in a.list.read_text().splitlines() if l.strip() and not l.startswith("#")]
    blobs = []
    for n in names:
        p = a.root / n
        if not p.is_file():
            sys.exit(f"missing {p}")
        blobs.append(p.read_bytes())
    sums = {n: hashlib.sha256(b).hexdigest() for n, b in zip(names, blobs)}
    if a.verify:
        want = {}
        for line in a.verify.read_text().splitlines():
            if line.strip():
                h, n = line.split(None, 1)
                want[n.strip()] = h
        bad = [n for n in names if want.get(n) != sums[n]] + [n for n in want if n not in sums]
        if bad:
            sys.exit(f"{len(bad)} weight files differ from the tested model, first: {bad[0]}")

    header = bytearray(b"NRMODEL1" + struct.pack("<II", len(names), 0))
    for n in names:
        header += struct.pack("<I", len(n.encode())) + n.encode() + b"\0" * 16
    off = (len(header) + 255) // 256 * 256
    index = bytearray(b"NRMODEL1" + struct.pack("<II", len(names), 0))
    offsets = []
    for n, b in zip(names, blobs):
        offsets.append(off)
        index += struct.pack("<I", len(n.encode())) + n.encode() + struct.pack("<QQ", off, len(b))
        off = (off + len(b) + 255) // 256 * 256
    assert len(index) == len(header)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(index)
        for o, b in zip(offsets, blobs):
            f.seek(o)
            f.write(b)
    if a.sums:
        a.sums.write_text("".join(f"{sums[n]}  {n}\n" for n in names))
    print(f"{a.out}: {len(names)} entries, {a.out.stat().st_size / 2**20:.1f} MiB")


if __name__ == "__main__":
    main()
