#!/usr/bin/env python3
"""Inspect an NGX NR PE/ZIP without loading or executing its code.

Weight envelope reconstructed from 310.8.0 x64 code at RVA 0x44cf0 and
0x44ee0. Unknown enum values remain numeric; packed layer data is opaque.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
import zipfile


class FormatError(ValueError):
    pass


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise FormatError(f"out of bounds: offset={offset:#x}, size={size}, buffer={len(data)}")
    return memoryview(data)[offset:offset + size]


def unpack(fmt, data, offset):
    return struct.unpack(fmt, checked(data, offset, struct.calcsize(fmt)))


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class PE:
    def __init__(self, data):
        self.data = data
        if bytes(checked(data, 0, 2)) != b"MZ":
            raise FormatError("not a PE file")
        pe, = unpack("<I", data, 0x3c)
        if bytes(checked(data, pe, 4)) != b"PE\0\0":
            raise FormatError("missing PE signature")
        self.machine, count = unpack("<HH", data, pe + 4)
        opt = pe + 24
        opt_size, = unpack("<H", data, pe + 20)
        magic, = unpack("<H", data, opt)
        if magic != 0x20b or opt_size < 144:
            raise FormatError("expected PE32+ optional header")
        self.image_base, = unpack("<Q", data, opt + 24)
        self.resource_rva, self.resource_size = unpack("<II", data, opt + 128)
        self.sections = []
        for i in range(count):
            o = opt + opt_size + 40 * i
            name = bytes(checked(data, o, 8)).rstrip(b"\0").decode("ascii")
            vs, va, size, raw = unpack("<IIII", data, o + 8)
            checked(data, raw, size)
            self.sections.append(dict(name=name, rva=va, virtual_size=vs,
                                      file_offset=raw, file_size=size))

    def offset(self, rva, size=1):
        for s in self.sections:
            delta = rva - s["rva"]
            if 0 <= delta and delta + size <= s["file_size"]:
                return s["file_offset"] + delta
        raise FormatError(f"RVA not backed by file: {rva:#x}, size={size}")

    def resources(self):
        if not self.resource_rva:
            return []
        root = self.offset(self.resource_rva, self.resource_size)
        tree = checked(self.data, root, self.resource_size)
        result = []
        active = set()

        def walk(rel, path):
            if rel in active or len(path) > 8:
                raise FormatError("cyclic or excessively deep resource tree")
            active.add(rel)
            named, ids = unpack("<HH", tree, rel + 12)
            for i in range(named + ids):
                name, child = unpack("<II", tree, rel + 16 + 8 * i)
                if name & 0x80000000:
                    o = name & 0x7fffffff
                    n, = unpack("<H", tree, o)
                    label = bytes(checked(tree, o + 2, n * 2)).decode("utf-16le")
                else:
                    label = str(name)
                if child & 0x80000000:
                    walk(child & 0x7fffffff, path + [label])
                else:
                    rva, size, codepage, _ = unpack("<IIII", tree, child)
                    offset = self.offset(rva, size)
                    result.append(dict(path=path + [label], rva=rva,
                                       file_offset=offset, size=size, codepage=codepage,
                                       sha256=sha256(checked(self.data, offset, size))))
            active.remove(rel)

        walk(0, [])
        return result


def parse_weights(data):
    """Validate the envelope, preserving dtype/layout enum IDs without guessing."""
    total, = unpack("<Q", data, 0)
    if total != len(data):
        raise FormatError(f"weight map size mismatch: {total} != {len(data)}")
    p = 8
    records = []
    names = set()
    while p < total:
        record_offset = p
        name_size, = unpack("<Q", data, p)
        p += 8
        if not 0 < name_size <= 4096:
            raise FormatError("invalid weight name length")
        try:
            name = bytes(checked(data, p, name_size)).decode("utf-8")
        except UnicodeDecodeError as exc:
            raise FormatError("invalid weight name") from exc
        if name in names:
            raise FormatError(f"duplicate weight name: {name}")
        names.add(name)
        p += name_size
        outer_size, = unpack("<Q", data, p)
        p += 8
        body = checked(data, p, outer_size)
        inner_size, raw_size, device = unpack("<QQI", body, 0)
        if inner_size != outer_size:
            raise FormatError(f"{name}: inner/outer sizes differ")
        if not raw_size or device not in (0, 1):
            raise FormatError(f"{name}: empty data or non-host device {device}")
        payload = checked(body, 20, raw_size)
        type_id, layout_id, ndim = unpack("<IIQ", body, 20 + raw_size)
        if ndim > 32:
            raise FormatError(f"{name}: implausible dimension count {ndim}")
        shape = list(unpack("<" + "I" * ndim, body, 36 + raw_size))
        if 36 + raw_size + 4 * ndim != inner_size:
            raise FormatError(f"{name}: trailing or missing metadata")
        records.append(dict(name=name, record_offset=record_offset, body_offset=p,
                            serialized_size=outer_size, payload_offset=p + 20,
                            payload_size=raw_size, device_id=device,
                            type_id=type_id, layout_id=layout_id, storage_shape=shape,
                            sha256=sha256(payload)))
        p += outer_size
    if p != total:
        raise FormatError("weight map did not end exactly at resource boundary")
    return records


def cuda_elfs(data):
    result = []
    for match in re.finditer(b"\x7fELF", data):
        start = match.start()
        if bytes(checked(data, start + 4, 2)) != b"\x02\x01":
            continue
        machine, = unpack("<H", data, start + 18)
        if machine != 190:
            continue
        shoff, = unpack("<Q", data, start + 40)
        flags, = unpack("<I", data, start + 48)
        entsize, count, strindex = unpack("<HHH", data, start + 58)
        if entsize != 64 or strindex >= count:
            raise FormatError("invalid CUDA ELF section table")
        table = [unpack("<IIQQQQIIQQ", data, start + shoff + 64 * i)
                 for i in range(count)]
        strings = bytes(checked(data, start + table[strindex][4], table[strindex][5]))
        kernels = []
        extent = shoff + 64 * count
        for s in table:
            if s[0] >= len(strings):
                raise FormatError("invalid ELF section name offset")
            end = strings.find(b"\0", s[0])
            if end < 0:
                raise FormatError("unterminated ELF section name")
            name = strings[s[0]:end].decode("ascii")
            if s[1] != 8:  # SHT_NOBITS has no bytes in the file.
                checked(data, start + s[4], s[5])
                extent = max(extent, s[4] + s[5])
            if name.startswith(".text."):
                kernels.append(dict(name=name[6:], file_offset=start + s[4], size=s[5]))
        result.append(dict(file_offset=start, size=extent, flags=flags,
                           section_count=count, kernels=kernels,
                           sha256=sha256(checked(data, start, extent))))
    return result


def load_input(path):
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            matches = [i for i in archive.infolist()
                       if Path(i.filename).name.lower() == "nvngx_dlssnr.dll"]
            if len(matches) != 1:
                raise FormatError("expected exactly one nvngx_dlssnr.dll ZIP member")
            if matches[0].file_size > 1024 ** 3:
                raise FormatError("DLL exceeds 1 GiB inspection limit")
            return archive.read(matches[0])
    return path.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--extract", action="store_true")
    args = parser.parse_args()
    data = load_input(args.input)
    pe = PE(data)
    resources = pe.resources()
    matches = [r for r in resources if "WEIGHTS_HT" in r["path"]]
    if len(matches) != 1:
        raise FormatError("expected exactly one WEIGHTS_HT resource")
    resource = matches[0]
    weights = checked(data, resource["file_offset"], resource["size"])
    records = parse_weights(weights)
    elfs = cuda_elfs(data)
    blocks = sorted({int(m.group(1)) for r in records
                     if (m := re.match(r"block(\d+)\.", r["name"]))})
    report = dict(schema_version=1, source_name=args.input.name,
                  dll_size=len(data), dll_sha256=sha256(data),
                  image_base=pe.image_base, sections=pe.sections, resources=resources,
                  weight_record_count=len(records), block_ids=blocks,
                  raw_weight_bytes=sum(r["payload_size"] for r in records),
                  weights=records, cuda_elfs=elfs,
                  caveats=["Packed layer blobs; scalar encoding/layout still unresolved.",
                           "Kernel inventory includes variants, not an execution trace."])
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    if args.extract:
        (args.output / "nvngx_dlssnr.dll").write_bytes(data)
        (args.output / "weights_ht.bin").write_bytes(weights)
        target = args.output / "weights"
        target.mkdir(exist_ok=True)
        for r in records:
            # Never let serialized names become path traversal components.
            if not re.fullmatch(r"[A-Za-z0-9_.-]+", r["name"]) or r["name"] in (".", ".."):
                raise FormatError("unsafe extraction name")
            (target / (r["name"] + ".bin")).write_bytes(
                checked(weights, r["payload_offset"], r["payload_size"]))
        target = args.output / "cubins"
        target.mkdir(exist_ok=True)
        for e in elfs:
            (target / f"{e['file_offset']:08x}.cubin").write_bytes(
                checked(data, e["file_offset"], e["size"]))
    print(json.dumps(dict(dll_sha256=report["dll_sha256"], records=len(records),
                          blocks=len(blocks), raw_weight_bytes=report["raw_weight_bytes"],
                          cuda_modules=len(elfs), kernels=sum(len(e["kernels"]) for e in elfs),
                          manifest=str(args.output / "manifest.json")), indent=2))


if __name__ == "__main__":
    main()
