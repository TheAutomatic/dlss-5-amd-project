#!/usr/bin/env bash
# Build the OptiScaler NR Linux release packages.
#
#   bash tools/release/package-linux.sh [--version V] [--arch x86_64|i686|all] [--out DIR] [--cache DIR]
#
# For each architecture: download Mochizuki's pinned upstream DLSSNR-AMD Linux release (and the CJK
# language pack) - see "mochizukiLinux" in tools/build/upstream-pins.json -, verify their SHA-256,
# extract them, merge the language pack into dlssnr-amd/lang/, overlay OpenRR (x86_64, without
# Windows-only aco/) and OptiPatcher (x86_64), plus the launcher, uninstall.sh, licences and package
# metadata, refuse proprietary files, and write
#   <out>/OptiScaler-NR-Linux-<arch>-<version>.tar.gz (+ .sha256)
# with a single top-level folder OptiScaler-NR-Linux-<arch>-<version>/.
#
# Upstream install.sh is shipped unmodified: it is the authoritative installer, the launcher drives it.
# Defaults: version from ./VERSION, both architectures, out = dist/, cache = cache/linux/.
# --cache may point at a folder with already downloaded archives under any name: they are matched by
# SHA-256, so nothing is downloaded twice. Needs bash, curl, tar, gzip, python3, sha256sum.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
version="" arch="all" out="$repo/dist" cache="$repo/cache/linux"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) version=${2:?--version needs a value}; shift 2;;
        --arch) arch=${2:?--arch needs a value}; shift 2;;
        --out) out=${2:?--out needs a folder}; shift 2;;
        --cache) cache=${2:?--cache needs a folder}; shift 2;;
        -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
        *) echo "unknown argument: $1" >&2; exit 2;;
    esac
done
[[ -n "$version" ]] || version=$(tr -d ' \t\r\n' < "$repo/VERSION")
[[ "$version" =~ ^[0-9A-Za-z._-]+$ ]] || { echo "invalid version: $version" >&2; exit 1; }
case "$arch" in
    all) arches=(x86_64 i686);;
    x86_64|i686) arches=("$arch");;
    *) echo "--arch takes x86_64, i686 or all" >&2; exit 2;;
esac
for tool in curl tar gzip python3 sha256sum; do
    command -v "$tool" > /dev/null || { echo "$tool is required" >&2; exit 1; }
done
mkdir -p -- "$out" "$cache"
out=$(cd -- "$out" && pwd)
cache=$(cd -- "$cache" && pwd)
pins="$repo/tools/build/upstream-pins.json"

# pin_root <json path...>: one value from upstream-pins.json
pin_root() {
    python3 - "$pins" "$@" <<'PY'
import json, sys
node = json.load(open(sys.argv[1], encoding="utf-8"))
for key in sys.argv[2:]:
    node = node[key]
print(node if not isinstance(node, bool) else ("true" if node else "false"))
PY
}

# pin <json path...>: one value from upstream-pins.json -> mochizukiLinux
pin() { pin_root mochizukiLinux "$@"; }

sha() { sha256sum -- "$1" | cut -d' ' -f1; }

is_pe64() {
    python3 - "$1" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
if len(data) < 4096 or data[:2] != b"MZ":
    sys.exit(1)
(pe_off,) = struct.unpack_from("<I", data, 0x3C)
if pe_off + 6 > len(data) or data[pe_off:pe_off+4] != b"PE\x00\x00":
    sys.exit(1)
(machine,) = struct.unpack_from("<H", data, pe_off + 4)
sys.exit(0 if machine == 0x8664 else 1)
PY
}

# fetch <name> <url> <sha256>: print the path of a verified copy in the cache (download if needed)
fetch() {
    local name=$1 url=$2 want=$3 f
    if [[ -f "$cache/$name" && "$(sha "$cache/$name")" == "$want" ]]; then
        echo "$cache/$name"; return
    fi
    # any file in the cache with the right checksum (e.g. downloaded earlier under another name)
    for f in "$cache"/*; do
        [[ -f "$f" ]] || continue
        case "$f" in *.part) continue;; esac
        if [[ "$(sha "$f")" == "$want" ]]; then echo "$f"; return; fi
    done
    echo "Downloading $name ..." >&2
    curl -fL --retry 3 --retry-delay 2 --connect-timeout 30 -o "$cache/$name.part" "$url" >&2
    if [[ "$(sha "$cache/$name.part")" != "$want" ]]; then
        rm -f -- "$cache/$name.part"
        echo "SHA-256 mismatch for $name (expected $want); refusing to package." >&2
        exit 1
    fi
    mv -f -- "$cache/$name.part" "$cache/$name"
    echo "$cache/$name"
}

# OptiPatcher uses a rolling upstream release URL (matching Windows fetch-upstream.ps1).
# Prefer a SHA-256 match against pinnedSha256; if allowRollingRefresh is true and the rolling binary
# moved, accept any valid AMD64 PE file downloaded from the pinned URL.
fetch_optipatcher() {
    local name url want rolling
    name=$(pin_root optipatcher fileName)
    url=$(pin_root optipatcher url)
    want=$(pin_root optipatcher pinnedSha256)
    rolling=$(pin_root optipatcher allowRollingRefresh)
    if [[ -f "$cache/$name" ]]; then
        if [[ "$(sha "$cache/$name")" == "$want" ]] || [[ "$rolling" == "true" && $(is_pe64 "$cache/$name"; echo $?) -eq 0 ]]; then
            echo "$cache/$name"; return
        fi
    fi
    echo "Downloading $name ..." >&2
    curl -fL --retry 3 --retry-delay 2 --connect-timeout 30 -o "$cache/$name.part" "$url" >&2
    if [[ "$(sha "$cache/$name.part")" != "$want" ]]; then
        if [[ "$rolling" != "true" ]] || ! is_pe64 "$cache/$name.part"; then
            rm -f -- "$cache/$name.part"
            echo "OptiPatcher verification failed (not matching pinned SHA-256 or valid PE64)." >&2
            exit 1
        fi
        echo "Note: OptiPatcher rolling binary updated (validated PE64, sha256=$(sha "$cache/$name.part"))." >&2
    fi
    mv -f -- "$cache/$name.part" "$cache/$name"
    echo "$cache/$name"
}

# Same pattern as the Windows package guard in .github/workflows/release.yml: NVIDIA DLLs, extracted
# model weights and other user-supplied/proprietary files must never be published.
forbidden='^(nvngx.*\.dll|dlssnr_amd_pass.*\.dll|dlssnr_on_amd_weights\.bin|dlssnr(-int4|-small)?\.bin|CC_(SMALL_FP8|SMALL_FP8_EXPERIMENTAL|SILVER_AARDWOLD|SILVER_AARDWOLD_OPTIMIZED|SILVER_AARDWOLD_OPTIMIZED_INT4)\.bin|pipeline\.cache|version\.dll|dlssnr_on_amd_setup\.exe)$'

upstream_version=$(pin version)
lang_zip=$(fetch "$(pin langArchiveName)" "$(pin langArchiveUrl)" "$(pin langArchiveSha256)")
openrr_version=$(pin_root openrr version)
openrr_zip=""
optipatcher_asi=""
for a in "${arches[@]}"; do
    if [[ "$a" == x86_64 ]]; then
        openrr_zip=$(fetch "$(pin_root openrr archiveName)" "$(pin_root openrr archiveUrl)" "$(pin_root openrr archiveSha256)")
        optipatcher_asi=$(fetch_optipatcher)
        break
    fi
done
epoch=${SOURCE_DATE_EPOCH:-$(git -C "$repo" log -1 --format=%ct 2> /dev/null || date +%s)}
work=$(mktemp -d "${TMPDIR:-/tmp}/optiscaler-nr-linux.XXXXXX")
trap 'rm -rf -- "$work"' EXIT

for a in "${arches[@]}"; do
    name="OptiScaler-NR-Linux-$a-$version"
    stage="$work/$name"
    echo "== $name (DLSSNR-AMD $upstream_version$([[ "$a" == x86_64 ]] && echo ", OpenRR $openrr_version" || true))"
    tgz=$(fetch "$(pin archives "$a" archiveName)" "$(pin archives "$a" archiveUrl)" "$(pin archives "$a" archiveSha256)")

    # refuse absolute paths, .. components and links that point outside before extracting anything
    python3 - "$tgz" <<'PY'
import sys, tarfile, posixpath
with tarfile.open(sys.argv[1]) as t:
    for m in t.getmembers():
        n = m.name
        if n.startswith("/") or ".." in n.split("/") or m.isdev():
            sys.exit("unsafe member in upstream archive: %s" % n)
        if (m.issym() or m.islnk()) and (m.linkname.startswith("/") or ".." in posixpath.normpath(posixpath.join(posixpath.dirname(n), m.linkname)).split("/")):
            sys.exit("unsafe link in upstream archive: %s -> %s" % (n, m.linkname))
PY
    mkdir -p -- "$stage"
    tar -xzf "$tgz" -C "$stage" --no-same-owner
    [[ -f "$stage/install.sh" && -d "$stage/dlssnr-amd" ]] || { echo "unexpected upstream layout in $tgz" >&2; exit 1; }

    # CJK language pack: lang/* -> dlssnr-amd/lang/
    python3 - "$lang_zip" "$stage/dlssnr-amd" <<'PY'
import os, sys, zipfile, shutil
src, dest = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(src) as z:
    for info in z.infolist():
        n = info.filename
        if n.endswith("/"):
            continue
        if n.startswith("/") or ".." in n.split("/") or not n.startswith("lang/"):
            sys.exit("unexpected entry in the language pack: %s" % n)
        target = os.path.join(dest, *n.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with z.open(info) as r, open(target, "wb") as w:
            shutil.copyfileobj(r, w)
PY

    # OpenRR (v0.1.7plus) + OptiPatcher for 64-bit OptiScaler route (Linux Wine / Proton, Mesa RADV >= 26.3).
    # Windows-only openrr-amd/aco/ (amdxc64.dll D3D12 ELFs) is omitted on Linux.
    if [[ "$a" == x86_64 ]]; then
        python3 - "$openrr_zip" "$stage" <<'PY'
import os, sys, zipfile, shutil
src, stage = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(src) as z:
    for info in z.infolist():
        n = info.filename
        if n.endswith("/"):
            continue
        parts = n.split("/")
        if n.startswith("/") or ".." in parts:
            sys.exit("unsafe entry in OpenRR archive: %s" % n)
        if n == "OptiScaler/plugins/openrr-amd.asi":
            target = os.path.join(stage, "optiscaler", "asi", "openrr-amd.asi")
        elif n == "openrr-ngx-host.dll":
            target = os.path.join(stage, "optiscaler", "asi", "openrr-ngx-host.dll")
        elif n == "openrr-amd.ini":
            target = os.path.join(stage, "openrr-amd.ini")
        elif n.startswith("openrr-amd/"):
            if len(parts) >= 2 and parts[1] == "aco":
                continue
            target = os.path.join(stage, *parts)
        else:
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with z.open(info) as r, open(target, "wb") as w:
            shutil.copyfileobj(r, w)
PY
        cp -- "$optipatcher_asi" "$stage/optiscaler/asi/OptiPatcher.asi"
    fi

    # our overlay
    cp -r -- "$repo/tools/linux/launcher" "$stage/launcher"
    find "$stage/launcher" -name '__pycache__' -type d -prune -exec rm -rf {} +
    cp -- "$repo/tools/linux/OptiScalerNR-Launcher.sh" "$repo/tools/linux/uninstall.sh" "$stage/"
    cp -- "$repo/tools/linux/README-Linux.md" "$stage/README-Linux.md"
    cp -- "$repo/LICENSE" "$repo/NOTICE" "$stage/"
    mkdir -p -- "$stage/licenses"
    cp -- "$repo/assets/licenses/Mochizuki_MIT.txt" "$stage/licenses/Mochizuki_MIT.txt"
    printf '%s\n' "$version" > "$stage/VERSION"
    python3 - "$stage/OptiScaler-NR-package.json" "$a" "$version" "$upstream_version" "$(pin releaseUrl)" "$openrr_version" <<'PY'
import json, sys
path, arch, version, upstream, url, openrr_ver = sys.argv[1:]
is64 = arch != "i686"
meta = {
    "schema": 1,
    "name": "OptiScaler NR (Linux)",
    "arch": arch,
    "bits": 64 if is64 else 32,
    "version": version,
    "upstream": "DLSSNR-AMD (Mochizuki)",
    "upstreamVersion": upstream,
    "upstreamRelease": url,
    "openrrVersion": openrr_ver if is64 else None,
    "optipatcher": is64,
    "routes": (["optiscaler"] if is64 else []) + ["reshade-feeder", "reshade-feeder-vulkan", "reshade-feeder-dx9"],
}
open(path, "w", encoding="utf-8").write(json.dumps(meta, indent=2) + "\n")
PY

    # permissions: keep upstream's exec bits (install.sh checks int4/dlssnr-int4-weights with -x),
    # make everything world-readable and nothing group/world-writable; our entry scripts executable
    chmod -R u+rwX,go+rX,go-w "$stage"
    chmod 755 "$stage/install.sh" "$stage/OptiScalerNR-Launcher.sh" "$stage/uninstall.sh"
    find "$stage/launcher" -type f -exec chmod 644 {} +

    # sanity: what must be there, and what must never be
    required=(install.sh README.txt OptiScalerNR-Launcher.sh uninstall.sh README-Linux.md LICENSE NOTICE VERSION
              OptiScaler-NR-package.json licenses/Mochizuki_MIT.txt launcher/__main__.py launcher/server.py
              launcher/addons.py launcher/web/index.html launcher/web/app.js launcher/web/app.css
              dlssnr-amd/lang/zh-CN.txt dlssnr-amd/lang/fonts/NotoSansCJKsc-Regular.ttf
              reshade/ReShade.ini vulkan/winevulkan.dll ini/dlssnr-amd-addon.ini model-tools/extract_model.sh)
    if [[ "$a" == x86_64 ]]; then
        required+=(optiscaler/release/OptiScaler.dll optiscaler/asi/dlssnr-amd.asi reshade/dlssnr_amd.addon64
                   optiscaler/asi/openrr-amd.asi optiscaler/asi/OptiPatcher.asi openrr-amd.ini
                   openrr-amd/shaders/ae.spv openrr-amd/shaders-gfx11/ae.spv openrr-amd/lang/zh-CN.txt
                   openrr-amd/OpenRR_LICENSE.txt)
        [[ ! -d "$stage/openrr-amd/aco" ]] || { echo "x86_64 Linux package unexpectedly contains Windows-only openrr-amd/aco/" >&2; exit 1; }
    else
        required+=(reshade/dlssnr_amd.addon32)
        [[ ! -d "$stage/optiscaler" ]] || { echo "i686 package unexpectedly contains optiscaler/" >&2; exit 1; }
        [[ ! -d "$stage/openrr-amd" ]] || { echo "i686 package unexpectedly contains openrr-amd/" >&2; exit 1; }
    fi
    for r in "${required[@]}"; do
        [[ -e "$stage/$r" ]] || { echo "REQUIRED COMPONENT MISSING: $r" >&2; exit 1; }
    done
    bad=$(find "$stage" -type f -printf '%P\n' | while IFS= read -r p; do
        b=${p##*/}
        if printf '%s\n' "$b" | grep -qiE "$forbidden"; then printf '%s\n' "$p"; fi
    done)
    if [[ -n "$bad" ]]; then
        printf '%s\n' "$bad" | sed 's/^/FORBIDDEN: /' >&2
        echo "Refusing to package: proprietary/user-supplied files found." >&2
        exit 1
    fi
    if ! cmp -s "$stage/install.sh" <(tar -xzOf "$tgz" ./install.sh); then
        echo "install.sh differs from upstream" >&2; exit 1
    fi

    # deterministic-ish archive: sorted names, fixed owner and mtime, no gzip timestamp
    tarball="$out/$name.tar.gz"
    rm -f -- "$tarball" "$tarball.sha256"
    tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" --format=gnu \
        -C "$work" -cf - "$name" | gzip -n -9 > "$tarball.part"
    mv -f -- "$tarball.part" "$tarball"
    (cd -- "$out" && sha256sum -- "$name.tar.gz" > "$name.tar.gz.sha256")
    echo "   $(du -h -- "$tarball" | cut -f1)  $tarball"
    echo "   sha256 $(cut -d' ' -f1 "$tarball.sha256")"
    rm -rf -- "$stage"
done
