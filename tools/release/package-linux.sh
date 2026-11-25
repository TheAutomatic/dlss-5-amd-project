#!/usr/bin/env bash
# Build the OptiScaler NR Linux release packages.
#
#   bash tools/release/package-linux.sh [--version V] [--arch x86_64|i686|all] [--out DIR] [--cache DIR] [--jobs N]
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
# Defaults: version from ./VERSION, both architectures, out = dist/, cache = cache/linux/, jobs = nproc.
# --cache may point at a folder with already downloaded archives under any name: they are matched by
# SHA-256, so nothing is downloaded twice. Needs bash, curl, tar, gzip (or pigz), python3, sha256sum.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
version="" arch="all" out="$repo/dist" cache="$repo/cache/linux"
jobs="${JOBS:-}"
gzip_level="${GZIP_LEVEL:-6}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) version=${2:?--version needs a value}; shift 2;;
        --arch) arch=${2:?--arch needs a value}; shift 2;;
        --out) out=${2:?--out needs a folder}; shift 2;;
        --cache) cache=${2:?--cache needs a folder}; shift 2;;
        --jobs|-j) jobs=${2:?--jobs needs a positive integer}; shift 2;;
        -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
        *) echo "unknown argument: $1" >&2; exit 2;;
    esac
done
[[ -n "$version" ]] || version=$(tr -d ' \t\r\n' < "$repo/VERSION")
[[ "$version" =~ ^[0-9A-Za-z._-]+$ ]] || { echo "invalid version: $version" >&2; exit 1; }
if [[ -z "$jobs" ]]; then
    jobs=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
fi
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "invalid --jobs / JOBS value: $jobs" >&2; exit 2; }
[[ "$gzip_level" =~ ^[1-9]$ ]] || { echo "invalid GZIP_LEVEL (expected 1-9): $gzip_level" >&2; exit 2; }

case "$arch" in
    all) arches=(x86_64 i686);;
    x86_64|i686) arches=("$arch");;
    *) echo "--arch takes x86_64, i686 or all" >&2; exit 2;;
esac
for tool in curl tar gzip python3 sha256sum; do
    command -v "$tool" > /dev/null || { echo "$tool is required" >&2; exit 1; }
done
have_pigz=0
if command -v pigz > /dev/null 2>&1; then
    have_pigz=1
fi

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
    local name=$1 url=$2 want=$3 f part
    if [[ -f "$cache/$name" && "$(sha "$cache/$name")" == "$want" ]]; then
        echo "$cache/$name"; return
    fi
    # any file in the cache with the right checksum (e.g. downloaded earlier under another name)
    for f in "$cache"/*; do
        [[ -f "$f" ]] || continue
        case "$f" in *.part|*.part.*) continue;; esac
        if [[ "$(sha "$f")" == "$want" ]]; then echo "$f"; return; fi
    done
    part="$cache/$name.part.$$.$BASHPID"
    echo "Downloading $name ..." >&2
    if ! curl -fL --retry 3 --retry-delay 2 --connect-timeout 30 -o "$part" "$url" >&2; then
        rm -f -- "$part"
        exit 1
    fi
    if [[ "$(sha "$part")" != "$want" ]]; then
        rm -f -- "$part"
        echo "SHA-256 mismatch for $name (expected $want); refusing to package." >&2
        exit 1
    fi
    mv -f -- "$part" "$cache/$name"
    echo "$cache/$name"
}

# OptiPatcher uses a rolling upstream release URL (matching Windows fetch-upstream.ps1).
# Prefer a SHA-256 match against pinnedSha256; if allowRollingRefresh is true and the rolling binary
# moved, accept any valid AMD64 PE file downloaded from the pinned URL.
fetch_optipatcher() {
    local name url want rolling part
    name=$(pin_root optipatcher fileName)
    url=$(pin_root optipatcher url)
    want=$(pin_root optipatcher pinnedSha256)
    rolling=$(pin_root optipatcher allowRollingRefresh)
    if [[ -f "$cache/$name" ]]; then
        if [[ "$(sha "$cache/$name")" == "$want" ]] || [[ "$rolling" == "true" && $(is_pe64 "$cache/$name"; echo $?) -eq 0 ]]; then
            echo "$cache/$name"; return
        fi
    fi
    part="$cache/$name.part.$$.$BASHPID"
    echo "Downloading $name ..." >&2
    if ! curl -fL --retry 3 --retry-delay 2 --connect-timeout 30 -o "$part" "$url" >&2; then
        rm -f -- "$part"
        exit 1
    fi
    if [[ "$(sha "$part")" != "$want" ]]; then
        if [[ "$rolling" != "true" ]] || ! is_pe64 "$part"; then
            rm -f -- "$part"
            echo "OptiPatcher verification failed (not matching pinned SHA-256 or valid PE64)." >&2
            exit 1
        fi
        echo "Note: OptiPatcher rolling binary updated (validated PE64, sha256=$(sha "$part"))." >&2
    fi
    mv -f -- "$part" "$cache/$name"
    echo "$cache/$name"
}

compress_stream() {
    local threads=$1
    if [[ "$have_pigz" -eq 1 ]]; then
        pigz -n "-$gzip_level" -p "$threads"
    else
        gzip -n "-$gzip_level"
    fi
}

decompress_file() {
    local src=$1 threads=$2
    if [[ "$have_pigz" -eq 1 ]]; then
        pigz -dc -p "$threads" -- "$src"
    else
        gzip -dc -- "$src"
    fi
}

# Same pattern as the Windows package guard in .github/workflows/release.yml: NVIDIA DLLs, extracted
# model weights and other user-supplied/proprietary files must never be published.
forbidden='^(nvngx.*\.dll|dlssnr_amd_pass.*\.dll|dlssnr_on_amd_weights\.bin|dlssnr(-int4|-small)?\.bin|CC_(SMALL_FP8|SMALL_FP8_EXPERIMENTAL|SILVER_AARDWOLD|SILVER_AARDWOLD_OPTIMIZED|SILVER_AARDWOLD_OPTIMIZED_INT4)\.bin|pipeline\.cache|version\.dll|dlssnr_on_amd_setup\.exe)$'

upstream_version=$(pin version)
upstream_release_url=$(pin releaseUrl)
openrr_version=$(pin_root openrr version)
epoch=${SOURCE_DATE_EPOCH:-$(git -C "$repo" log -1 --format=%ct 2> /dev/null || date +%s)}
work=$(mktemp -d "${TMPDIR:-/tmp}/optiscaler-nr-linux.XXXXXX")

active_pids=()
cleanup() {
    local pid
    if [[ ${#active_pids[@]} -gt 0 ]]; then
        for pid in "${active_pids[@]}"; do
            kill "$pid" 2>/dev/null || true
        done
        for pid in "${active_pids[@]}"; do
            wait "$pid" 2>/dev/null || true
        done
    fi
    rm -rf -- "$work"
}
trap cleanup EXIT

# 1. Pre-fetch all required upstream archives concurrently (heaviest first, bounded by JOBS).
fetch_keys=()
fetch_kinds=()
fetch_names=()
fetch_urls=()
fetch_shas=()

for a in "${arches[@]}"; do
    fetch_keys+=("arch_$a")
    fetch_kinds+=("pinned")
    fetch_names+=("$(pin archives "$a" archiveName)")
    fetch_urls+=("$(pin archives "$a" archiveUrl)")
    fetch_shas+=("$(pin archives "$a" archiveSha256)")
done
for a in "${arches[@]}"; do
    if [[ "$a" == x86_64 ]]; then
        fetch_keys+=("openrr")
        fetch_kinds+=("pinned")
        fetch_names+=("$(pin_root openrr archiveName)")
        fetch_urls+=("$(pin_root openrr archiveUrl)")
        fetch_shas+=("$(pin_root openrr archiveSha256)")
        fetch_keys+=("optipatcher")
        fetch_kinds+=("optipatcher")
        fetch_names+=("")
        fetch_urls+=("")
        fetch_shas+=("")
        break
    fi
done
fetch_keys+=("lang")
fetch_kinds+=("pinned")
fetch_names+=("$(pin langArchiveName)")
fetch_urls+=("$(pin langArchiveUrl)")
fetch_shas+=("$(pin langArchiveSha256)")

fetch_pids=()
fetch_logs=()
fetch_done=()
fetch_rcs=()
active_fetch=0
fetch_failed=0

reap_finished_fetches() {
    local j rc
    for j in "${!fetch_pids[@]}"; do
        [[ "${fetch_done[$j]}" -eq 1 ]] && continue
        if ! kill -0 "${fetch_pids[$j]}" 2>/dev/null; then
            rc=0
            wait "${fetch_pids[$j]}" || rc=$?
            fetch_done[$j]=1
            fetch_rcs[$j]=$rc
            active_fetch=$((active_fetch - 1))
            if [[ "$rc" -ne 0 && "$fetch_failed" -eq 0 ]]; then
                fetch_failed=$rc
            fi
        fi
    done
}

for i in "${!fetch_keys[@]}"; do
    key="${fetch_keys[$i]}"
    kind="${fetch_kinds[$i]}"
    logf="$work/fetch-$key.log"
    outf="$work/fetch-$key.path"
    if [[ "$jobs" -le 1 ]]; then
        if [[ "$kind" == "optipatcher" ]]; then
            fetch_optipatcher > "$outf"
        else
            fetch "${fetch_names[$i]}" "${fetch_urls[$i]}" "${fetch_shas[$i]}" > "$outf"
        fi
        continue
    fi
    while [[ "$active_fetch" -ge "$jobs" ]]; do
        reap_finished_fetches
        if [[ "$fetch_failed" -ne 0 ]]; then
            break
        fi
        [[ "$active_fetch" -ge "$jobs" ]] && sleep 0.1
    done
    if [[ "$fetch_failed" -ne 0 ]]; then
        break
    fi
    fetch_logs+=("$logf")
    fetch_done+=(0)
    fetch_rcs+=(0)
    (
        if [[ "$kind" == "optipatcher" ]]; then
            fetch_optipatcher > "$outf"
        else
            fetch "${fetch_names[$i]}" "${fetch_urls[$i]}" "${fetch_shas[$i]}" > "$outf"
        fi
    ) 2> "$logf" &
    pid=$!
    fetch_pids+=("$pid")
    active_pids+=("$pid")
    active_fetch=$((active_fetch + 1))
done
if [[ ${#fetch_pids[@]} -gt 0 ]]; then
    for i in "${!fetch_pids[@]}"; do
        if [[ "${fetch_done[$i]}" -eq 0 ]]; then
            if [[ "$fetch_failed" -ne 0 ]]; then
                kill "${fetch_pids[$i]}" 2>/dev/null || true
            fi
            rc=0
            wait "${fetch_pids[$i]}" || rc=$?
            fetch_done[$i]=1
            fetch_rcs[$i]=$rc
            if [[ "$rc" -ne 0 && "$fetch_failed" -eq 0 ]]; then
                fetch_failed=$rc
            fi
        fi
        if [[ -s "${fetch_logs[$i]}" ]]; then
            cat "${fetch_logs[$i]}" >&2
        fi
    done
    active_pids=()
    if [[ "$fetch_failed" -ne 0 ]]; then
        exit "$fetch_failed"
    fi
fi

lang_zip=$(cat "$work/fetch-lang.path")
openrr_zip=""
optipatcher_asi=""
if [[ -f "$work/fetch-openrr.path" ]]; then
    openrr_zip=$(cat "$work/fetch-openrr.path")
fi
if [[ -f "$work/fetch-optipatcher.path" ]]; then
    optipatcher_asi=$(cat "$work/fetch-optipatcher.path")
fi

# 2. Build each architecture in an isolated staging dir (x86_64 scheduled first).
build_arch() {
    local a=$1 arch_jobs=$2
    local name="OptiScaler-NR-Linux-$a-$version"
    local stage_root="$work/stage-$a"
    local stage="$stage_root/$name"
    local tgz upstream_install_sha tarball
    tgz=$(cat "$work/fetch-arch_$a.path")

    echo "== $name (DLSSNR-AMD $upstream_version$([[ "$a" == x86_64 ]] && echo ", OpenRR $openrr_version" || true); jobs=$arch_jobs, $([[ "$have_pigz" -eq 1 ]] && echo "pigz -$gzip_level" || echo "gzip -$gzip_level"))"

    # Validate members (refuse absolute paths, .. components, device nodes, escaping links)
    # via fast streaming decompression before extracting anything.
    decompress_file "$tgz" "$arch_jobs" | python3 -c '
import posixpath, sys, tarfile
with tarfile.open(fileobj=sys.stdin.buffer, mode="r|") as t:
    for m in t:
        n = m.name
        if n.startswith("/") or ".." in n.split("/") or m.isdev():
            sys.exit("unsafe member in upstream archive: %s" % n)
        if (m.issym() or m.islnk()) and (m.linkname.startswith("/") or ".." in posixpath.normpath(posixpath.join(posixpath.dirname(n), m.linkname)).split("/")):
            sys.exit("unsafe link in upstream archive: %s -> %s" % (n, m.linkname))
'

    mkdir -p -- "$stage"
    decompress_file "$tgz" "$arch_jobs" | tar -C "$stage" --no-same-owner -xf -
    [[ -f "$stage/install.sh" && -d "$stage/dlssnr-amd" ]] || { echo "unexpected upstream layout in $tgz" >&2; exit 1; }
    upstream_install_sha=$(sha "$stage/install.sh")

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

    # OpenRR (v0.1.7plus6) + OptiPatcher for 64-bit OptiScaler route (Linux Wine / Proton, Mesa RADV >= 26.3).
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
    python3 - "$stage/OptiScaler-NR-package.json" "$a" "$version" "$upstream_version" "$upstream_release_url" "$openrr_version" <<'PY'
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

    # sanity: single Python pass checks required components and refuses forbidden/proprietary files
    python3 - "$stage" "$a" "$forbidden" <<'PY'
import os, re, sys
stage, arch, forbidden_re = sys.argv[1], sys.argv[2], re.compile(sys.argv[3], re.I)
required = [
    "install.sh", "README.txt", "OptiScalerNR-Launcher.sh", "uninstall.sh", "README-Linux.md",
    "LICENSE", "NOTICE", "VERSION", "OptiScaler-NR-package.json", "licenses/Mochizuki_MIT.txt",
    "launcher/__main__.py", "launcher/server.py", "launcher/addons.py",
    "launcher/web/index.html", "launcher/web/app.js", "launcher/web/app.css",
    "dlssnr-amd/lang/zh-CN.txt", "dlssnr-amd/lang/fonts/NotoSansCJKsc-Regular.ttf",
    "reshade/ReShade.ini", "vulkan/winevulkan.dll", "ini/dlssnr-amd-addon.ini", "model-tools/extract_model.sh",
]
if arch == "x86_64":
    required += [
        "optiscaler/release/OptiScaler.dll", "optiscaler/asi/dlssnr-amd.asi", "reshade/dlssnr_amd.addon64",
        "optiscaler/asi/openrr-amd.asi", "optiscaler/asi/OptiPatcher.asi", "openrr-amd.ini",
        "openrr-amd/shaders/ae.spv", "openrr-amd/shaders-gfx11/ae.spv", "openrr-amd/lang/zh-CN.txt",
        "openrr-amd/OpenRR_LICENSE.txt",
    ]
    if os.path.isdir(os.path.join(stage, "openrr-amd", "aco")):
        sys.exit("x86_64 Linux package unexpectedly contains Windows-only openrr-amd/aco/")
else:
    required += ["reshade/dlssnr_amd.addon32"]
    if os.path.isdir(os.path.join(stage, "optiscaler")):
        sys.exit("i686 package unexpectedly contains optiscaler/")
    if os.path.isdir(os.path.join(stage, "openrr-amd")):
        sys.exit("i686 package unexpectedly contains openrr-amd/")

for rel in required:
    if not os.path.exists(os.path.join(stage, rel)):
        sys.exit(f"REQUIRED COMPONENT MISSING: {rel}")

bad = []
for root, _, files in os.walk(stage):
    for fn in files:
        if forbidden_re.match(fn):
            bad.append(os.path.relpath(os.path.join(root, fn), stage))
if bad:
    for p in sorted(bad):
        print(f"FORBIDDEN: {p}", file=sys.stderr)
    sys.exit("Refusing to package: proprietary/user-supplied files found.")
PY

    if [[ "$(sha "$stage/install.sh")" != "$upstream_install_sha" ]]; then
        echo "install.sh differs from upstream" >&2; exit 1
    fi

    # deterministic-ish archive: sorted names, fixed owner and mtime, no gzip timestamp
    tarball="$out/$name.tar.gz"
    rm -f -- "$tarball" "$tarball.sha256"
    tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" --format=gnu \
        -C "$stage_root" -cf - "$name" | compress_stream "$arch_jobs" > "$tarball.part.$$"
    mv -f -- "$tarball.part.$$" "$tarball"
    (cd -- "$out" && sha256sum -- "$name.tar.gz" > "$name.tar.gz.sha256")
    echo "   $(du -h -- "$tarball" | cut -f1)  $tarball"
    echo "   sha256 $(cut -d' ' -f1 "$tarball.sha256")"
    rm -rf -- "$stage_root"
}

if [[ "${#arches[@]}" -eq 1 || "$jobs" -le 1 ]]; then
    for a in "${arches[@]}"; do
        build_arch "$a" "$jobs"
    done
else
    arch_keys=()
    arch_pids=()
    arch_logs=()
    i686_jobs=$(( jobs / 4 ))
    [[ "$i686_jobs" -ge 1 ]] || i686_jobs=1
    x64_jobs=$(( jobs - i686_jobs ))
    [[ "$x64_jobs" -ge 1 ]] || x64_jobs=1
    for a in "${arches[@]}"; do
        if [[ "$a" == x86_64 ]]; then
            per_jobs=$x64_jobs
        else
            per_jobs=$i686_jobs
        fi
        logf="$work/build-$a.log"
        arch_keys+=("$a")
        arch_logs+=("$logf")
        ( build_arch "$a" "$per_jobs" ) > "$logf" 2>&1 &
        pid=$!
        arch_pids+=("$pid")
        active_pids+=("$pid")
    done
    # Print logs to stdout in deterministic arch order while propagating failure
    failed=0
    for i in "${!arch_pids[@]}"; do
        rc=0
        wait "${arch_pids[$i]}" || rc=$?
        if [[ -s "${arch_logs[$i]}" ]]; then
            cat "${arch_logs[$i]}"
        fi
        if [[ "$rc" -ne 0 && "$failed" -eq 0 ]]; then
            failed=$rc
        fi
    done
    active_pids=()
    if [[ "$failed" -ne 0 ]]; then
        exit "$failed"
    fi
fi
