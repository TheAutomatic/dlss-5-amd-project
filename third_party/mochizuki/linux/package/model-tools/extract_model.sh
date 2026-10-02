#!/usr/bin/env bash
# Make dlssnr.bin from NVIDIA's nvngx_dlssnr.dll (310.8.0).
#
#   bash extract_model.sh <nvngx_dlssnr.dll or a .zip containing it> <output .bin>
#
# Only reads the weight data in the DLL; never loads or runs it. Every entry is checked
# against model-files.sha256, and the file is written only if it matches the tested model
# byte for byte.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
src=${1:?usage: extract_model.sh <nvngx_dlssnr.dll or .zip> <output .bin>}
out=${2:?usage: extract_model.sh <nvngx_dlssnr.dll or .zip> <output .bin>}
[[ -f "$src" ]] || { echo "no such file: $src" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 1; }

want=e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p -- "$work/graph"
cp -- "$here/descriptor.json" "$work/graph/"

if ! python3 "$here/inspect_nr.py" "$src" --output "$work/inventory" --extract > "$work/inspect.json" 2> "$work/inspect.err"; then
    echo "Cannot read nvngx_dlssnr weights from $src (needs nvngx_dlssnr.dll version 310.8.0, or its zip)." >&2
    exit 1
fi
got=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["dll_sha256"])' "$work/inspect.json")
if [[ "$got" != "$want" ]]; then
    echo "This is not nvngx_dlssnr 310.8.0 (SHA256 $got). The 310.8.0 DLL is required." >&2
    exit 1
fi

cd -- "$here"
python3 unpack_swin_family.py --artifacts "$work" --output "$work/unpacked" --max-c 256 > /dev/null
python3 unpack_splitswin.py --artifacts "$work" --output "$work/unpacked-splitswin" > /dev/null
python3 unpack_vit.py --artifacts "$work" --output "$work/unpacked-vit" > /dev/null
python3 unpack_preblock.py "$work/inventory/weights/block0.layer0.layer.bin" "$work/unpacked-preblock" > /dev/null
python3 unpack_postblock.py "$work/inventory/weights/block70.layer0.layer.bin" "$work/unpacked-postblock" > /dev/null
python3 pack_model.py --root "$work" --list model-files.txt --out "$out" --verify model-files.sha256
