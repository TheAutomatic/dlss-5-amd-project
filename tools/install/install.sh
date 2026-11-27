#!/usr/bin/env bash
# OptScaler(NR) Linux / SteamOS (Proton) installer
# Modelled on Mochizuki's linux/package/install.sh + dual ASI plugin selection (dlssnr-amd.asi & openrr-amd.asi)
#
# Usage:
#   bash install.sh <game folder or game.exe> [install|remove|logs]
#                   [--proxy dxgi.dll|winmm.dll|d3d12.dll|winhttp.dll|wininet.dll|dbghelp.dll|version.dll]
#                   [--nr on|off] [--openrr on|off] [--openrr-spoof full|auto] [--optipatcher on|off] [--overwrite-ini]
#                   [--dll <nvngx_dlssnr.dll or .zip>] [--dlssd <nvngx_dlssd.dll>]
#                   [--model CC_SMALL_FP8|CC_SMALL_FP8_OPTIMIZED|CC_SILVER_AARDWOLD|CC_SILVER_AARDWOLD_OPTIMIZED]
#                   [--renodx|--wilsjo2]
set -euo pipefail

here=$(cd -- "$(dirname -- "$0")" && pwd)
pkg_model="$here/dlssnr-amd/CC_SILVER_AARDWOLD.bin"
pkg_small="$here/dlssnr-amd/CC_SMALL_FP8.bin"
model_sha256="2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd"
small_sha256="7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33"

sha_ok() { [[ -f "$1" && "$(sha256sum -- "$1" | cut -d' ' -f1 | tr 'A-F' 'a-f')" == "$2" ]]; }
model_ok() { sha_ok "$1" "$model_sha256"; }
small_ok() { sha_ok "$1" "$small_sha256"; }

target="" action="" proxy=""
want_nr="" want_openrr="" want_optipatcher=""
openrr_spoof="full" openrr_spoof_explicit=0
dll="" dlssd="" model="" renodx="" overwrite_ini=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        install|remove|uninstall|logs)
            action="$1"; [[ "$action" == "uninstall" ]] && action="remove"; shift;;
        --proxy) proxy=${2:?--proxy needs a DLL name}; shift 2;;
        --nr) want_nr=${2:?--nr takes on or off}; shift 2;;
        --no-nr) want_nr=off; shift;;
        --openrr) want_openrr=${2:?--openrr takes on or off}; shift 2;;
        --no-openrr) want_openrr=off; shift;;
        --openrr-spoof) openrr_spoof=${2:?--openrr-spoof takes full or auto}; openrr_spoof_explicit=1; shift 2;;
        --optipatcher) want_optipatcher=${2:?--optipatcher takes on or off}; shift 2;;
        --overwrite-ini|--reset-ini) overwrite_ini=1; shift;;
        --dll) dll=${2:?--dll needs a file path}; shift 2;;
        --dlssd) dlssd=${2:?--dlssd needs a file path}; shift 2;;
        --model) model=${2:?--model needs a network name}; shift 2;;
        --renodx) renodx=1; shift;;
        --no-renodx|--wilsjo2) renodx=0; shift;;
        -h|--help)
            sed -n '2,11p' "$0"; exit 0;;
        *)
            if [[ -z "$target" ]]; then target="$1"
            elif [[ -z "$action" ]]; then action="$1"; [[ "$action" == "uninstall" ]] && action="remove"
            else echo "unexpected argument: $1" >&2; exit 1; fi
            shift;;
    esac
done

openrr_spoof=${openrr_spoof,,}
case "$openrr_spoof" in
    full|auto) ;;
    *) echo "Invalid --openrr-spoof: $openrr_spoof (expected full or auto)" >&2; exit 1;;
esac

if [[ -z "$target" ]]; then
    if [[ -t 0 ]]; then
        read -r -p "Enter path to the game folder (or game .exe): " target
    else
        sed -n '2,11p' "$0"; exit 1
    fi
fi
target="${target%\"}"
target="${target#\"}"
if [[ -f "$target" ]]; then
    game=$(cd -- "$(dirname -- "$target")" && pwd)
elif [[ -d "$target" ]]; then
    game=$(cd -- "$target" && pwd)
else
    echo "Not found: $target" >&2; exit 1
fi

manifest="$game/dlssnr-amd-install.txt"
backup="$game/dlssnr-amd-backup"

if compgen -G "$game/*/Binaries/Win64/*.exe" > /dev/null; then
    echo "Note: this looks like the top folder of an Unreal Engine game; install into <project>/Binaries/Win64 (the *-Shipping.exe) instead." >&2
fi

# Inspect game EXE bitness & PE imports for proxy recommendation
probe_info=$(python3 - "$game" <<'PY' 2>/dev/null || echo "64 dxgi.dll"
import sys, pathlib, struct
game = pathlib.Path(sys.argv[1])
candidates = ["dxgi.dll", "winmm.dll", "d3d12.dll", "winhttp.dll", "wininet.dll", "dbghelp.dll", "version.dll"]
bits_seen = set()
rec = "dxgi.dll"
for exe in sorted(game.glob("*.exe"), key=lambda p: p.stat().st_size, reverse=True):
    if exe.name.lower() in ("nr_present_probe.exe", "optscalernr.launcher.exe", "unins000.exe") or "crash" in exe.name.lower():
        continue
    try:
        b = exe.read_bytes()
        if b[:2] != b"MZ": continue
        pe = struct.unpack_from("<I", b, 0x3C)[0]
        if b[pe:pe+4] != b"PE\x00\x00": continue
        mach = struct.unpack_from("<H", b, pe + 4)[0]
        bits_seen.add({0x14C: "32", 0x8664: "64"}.get(mach, "?"))
        text_low = b[:262144].lower()
        for c in candidates:
            if c.encode("ascii") in text_low:
                rec = c
                break
        break
    except Exception:
        pass
print(f"{','.join(sorted(bits_seen)) or '64'} {rec}")
PY
)
exe_bits="${probe_info%% *}"
rec_proxy="${probe_info#* }"

if [[ "$exe_bits" == "32" ]]; then
    echo "This is a 32-bit game ($game); OptiScaler and our ASI plugins support 64-bit games only." >&2
    exit 1
fi

if [[ -z "$action" ]]; then
    echo
    echo "Game folder: $game"
    echo "Choose an action:"
    echo "  1) Install / Update (Official OptiScaler + ASI plugins)"
    echo "  2) Uninstall"
    echo "  3) Collect logs (packs logs & system info into ~/dlssnr-amd-logs-*.tar.gz)"
    read -r -p "1-3 [Enter = 1]: " pick || pick=""
    case "${pick:-1}" in
        1) action="install";;
        2) action="remove";;
        3) action="logs";;
        *) echo "Invalid choice." >&2; exit 1;;
    esac
fi

record() { printf '%s\n' "$1" >> "$manifest"; }
save_existing() {
    local rel="$1"
    [[ -e "$game/$rel" ]] || return 0
    if [[ -e "$backup/$rel" ]] && ! cmp -s -- "$game/$rel" "$backup/$rel"; then
        mv -f -- "$backup/$rel" "$backup/$rel.conflict-$(date +%Y%m%d%H%M%S)"
    fi
    mkdir -p -- "$(dirname -- "$backup/$rel")"
    mv -f -- "$game/$rel" "$backup/$rel"
    record "B $rel"
}
put_file() {
    save_existing "$2"
    mkdir -p -- "$(dirname -- "$game/$2")"
    cp -f -- "$1" "$game/$2"
    record "F $2"
}
put_tree() {
    local src="$1" dst_rel="$2" d f rel
    [[ -d "$src" ]] || return 0
    while IFS= read -r -d '' d; do
        rel="${d#"$src"/}"
        [[ "$d" == "$src" ]] && rel=""
        if [[ "$dst_rel" == "openrr-amd" && ( "$rel" == "aco" || "$rel" == aco/* ) ]]; then
            continue
        fi
        local target_dir="$dst_rel${rel:+/$rel}"
        mkdir -p -- "$game/$target_dir"
        record "D $target_dir"
    done < <(find "$src" -type d -print0)
    while IFS= read -r -d '' f; do
        rel="${f#"$src"/}"
        if [[ "$dst_rel" == "OptiScaler" ]]; then
            case "$rel" in
                plugins/dlssnr-amd.asi|plugins/openrr-amd.asi|plugins/openrr.asi|plugins/openrr-ngx-host.dll|plugins/OptiPatcher.asi) continue;;
            esac
        elif [[ "$dst_rel" == "openrr-amd" ]]; then
            case "$rel" in
                aco/*) continue;;
            esac
        fi
        put_file "$f" "$dst_rel/$rel"
    done < <(find "$src" -type f -print0)
}

remove_legacy_tree() {
    local rel="$1" f sub
    [[ -d "$game/$rel" ]] || return 0
    if [[ -d "$here/$rel" ]]; then
        while IFS= read -r -d '' f; do
            sub="${f#"$here/$rel"/}"
            rm -f -- "$game/$rel/$sub"
        done < <(find "$here/$rel" -type f -print0)
    fi
    case "$rel" in
        OptiScaler)
            for sub in \
                amd_fidelityfx_dx12.dll amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll \
                amd_fidelityfx_framegeneration_dx12.dll amd_fidelityfx_denoiser_dx12.dll \
                amd_fidelityfx_vk.dll amd_fidelityfx_loader_vk.dll amd_fidelityfx_upscaler_vk.dll \
                amd_fidelityfx_framegeneration_vk.dll dlssg_to_fsr3_amd_is_better.dll \
                Streamlined_fetcher_windows.bat libxess.dll libxess_dx11.dll libxess_fg.dll libxell.dll \
                D3D12_OptiScaler/D3D12Core.dll D3D12_OptiScaler/d3d12SDKLayers.dll \
                Licenses/DirectX_LICENSE.txt Licenses/FidelityFX_v1_LICENSE.md Licenses/FidelityFX_v2_LICENSE.md Licenses/XeSS_LICENSE.txt \
                plugins/dlssnr-amd.asi plugins/openrr-amd.asi plugins/openrr.asi plugins/openrr-ngx-host.dll plugins/OptiPatcher.asi \
                plugins/OptScaler-NR-plugins-README.txt; do
                rm -f -- "$game/OptiScaler/$sub"
            done
            ;;
        dlssnr-amd)
            rm -rf -- "$game/dlssnr-amd/shaders" "$game/dlssnr-amd/aco" "$game/dlssnr-amd/lang" "$game/dlssnr-amd/int4-ultra" "$game/dlssnr-amd/int4" \
                "$game/dlssnr-amd/experimental" "$game/dlssnr-amd/optimized" "$game/dlssnr-amd/int4-performance" "$game/dlssnr-amd/aco-cache" \
                "$game/dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED" "$game/dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED-ultra" \
                "$game/dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED-performance" "$game/dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED-balanced" \
                "$game/dlssnr-amd/CC_SMALL_FP8_OPTIMIZED" "$game/dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-ultra" \
                "$game/dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-performance" "$game/dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-balanced"
            rm -f -- "$game/dlssnr-amd/nr_present_probe.exe" "$game/dlssnr-amd/dlssnr-amd.default.ini"
            ;;
        openrr-amd|opennr-rr)
            rm -rf -- "$game/$rel/shaders" "$game/$rel/shaders-gfx11" "$game/$rel/lang" "$game/$rel/cache" "$game/$rel/aco"
            rm -f -- "$game/$rel/nvngx_dlssd.dll" "$game/$rel/OpenRR_LICENSE.txt" "$game/$rel/OpenRR_THIRD_PARTY.md"
            ;;
    esac
    find "$game/$rel" -depth -type d -exec rmdir -- {} + 2>/dev/null || true
}

remove_installed() {
    if [[ ! -f "$manifest" && ! -f "$game/amd-presr-install.txt" ]]; then
        echo "No installation record found in $game, nothing to uninstall."
        return 0
    fi
    local keep_backup=0
    if [[ -f "$manifest" ]]; then
        local is_v2_manifest=0
        grep -q '^# format=v2-per-file$' "$manifest" 2>/dev/null && is_v2_manifest=1
        while IFS= read -r line || [[ -n "$line" ]]; do
            line="${line#$'\xef\xbb\xbf'}"
            [[ ${#line} -ge 3 && "${line:0:1}" != "#" ]] || continue
            kind="${line:0:1}"
            rel="${line:2}"
            case "$rel" in /*|..*|*/..*) continue;; esac
            case "$kind" in
                F|L) rm -f -- "$game/$rel";;
                D)
                    if [[ $is_v2_manifest == 0 ]] && ! grep -q -F "F $rel/" "$manifest" 2>/dev/null; then
                        remove_legacy_tree "$rel"
                    fi
                    rmdir --ignore-fail-on-non-empty -p -- "$game/$rel" 2>/dev/null || rmdir -- "$game/$rel" 2>/dev/null || true
                    ;;
                B)
                    if [[ -d "$backup/$rel" ]]; then
                        local bf bsub
                        while IFS= read -r -d '' bf; do
                            bsub="${bf#"$backup/$rel"/}"
                            mkdir -p -- "$(dirname -- "$game/$rel/$bsub")"
                            if [[ ! -e "$game/$rel/$bsub" ]]; then
                                mv -f -- "$bf" "$game/$rel/$bsub"
                            elif cmp -s -- "$bf" "$game/$rel/$bsub"; then
                                rm -f -- "$bf"
                            else
                                echo "Preserved modified file $game/$rel/$bsub (prior backup kept at $bf)"
                                keep_backup=1
                            fi
                        done < <(find "$backup/$rel" -type f -print0)
                        find "$backup/$rel" -depth -type d -exec rmdir -- {} + 2>/dev/null || true
                    elif [[ -e "$backup/$rel" ]]; then
                        mkdir -p -- "$(dirname -- "$game/$rel")"
                        if [[ ! -e "$game/$rel" ]]; then
                            mv -f -- "$backup/$rel" "$game/$rel"
                        elif cmp -s -- "$backup/$rel" "$game/$rel"; then
                            rm -f -- "$backup/$rel"
                        else
                            echo "Preserved modified file $game/$rel (prior backup kept at $backup/$rel)"
                            keep_backup=1
                        fi
                    fi
                    ;;
            esac
        done < <(tac "$manifest" 2>/dev/null)
        rm -f -- "$manifest"
    else
        rm -rf -- "$game/openrr-amd" "$game/opennr-rr" "$game/openrr-amd.ini" "$game/openrr-amd.log" "$game/openrr-control.bin" "$game/openrr-amd-crash.dmp" "$game/openrr-ngx-host.dll"
    fi
    if [[ -d "$backup" ]]; then
        find "$backup" -depth -type d -exec rmdir -- {} + 2>/dev/null || true
    fi
    rm -rf -- "$game/amd-presr-install.txt" "$game/mochizuki-owned.txt" \
              "$game/Uninstall_OptiScaler_NR.ps1" "$game/Uninstall_OptiScaler_NR.bat" "$game/xess-compat.ps1"
    echo "Uninstalled from $game."
}

collect_logs() {
    local stamp tmp out f
    stamp=$(date +%Y%m%d-%H%M%S)
    tmp=$(mktemp -d)
    mkdir -p "$tmp/logs" "$tmp/settings"
    shopt -s nullglob nocaseglob
    for f in "$game"/*.log; do tail -c 20000000 -- "$f" > "$tmp/logs/$(basename -- "$f")"; done
    for f in "$game"/*.ini "$game"/dlssnr-amd-install.txt "$game"/amd-presr-install.txt; do
        [[ -f "$f" ]] && cp -- "$f" "$tmp/settings/" || true
    done
    for f in "$game"/*.dmp; do mkdir -p "$tmp/crash"; cp -- "$f" "$tmp/crash/"; done
    for f in "$HOME"/steam-*.log; do
        [[ -n "$(find "$f" -mtime -2 2>/dev/null)" ]] && tail -c 20000000 -- "$f" > "$tmp/logs/$(basename -- "$f")" || true
    done
    shopt -u nullglob nocaseglob
    {
        echo "game: $game"; echo "package: $here"; echo "exe bits: $exe_bits"; echo
        uname -a; echo
        cat /etc/os-release 2>/dev/null || true; echo
        command -v lspci >/dev/null && lspci -nnk 2>/dev/null | grep -A3 -E "VGA|Display|3D controller" || true; echo
        command -v vulkaninfo >/dev/null && vulkaninfo --summary 2>/dev/null | sed -n '/Devices:/,$p' || true; echo
        free -h 2>/dev/null || true; echo
        ls -la -- "$game"
    } > "$tmp/system.txt" 2>&1
    out="$HOME/dlssnr-amd-logs-$stamp.tar.gz"
    tar -czf "$out" -C "$tmp" . && echo "Logs packed: $out"
    rm -rf -- "$tmp"
}

case "$action" in
    remove) remove_installed; exit 0;;
    logs) collect_logs; exit 0;;
    install) ;;
    *) echo "Unknown action: $action (use install, remove, or logs)" >&2; exit 1;;
esac

# 1. Choose injection proxy DLL
if [[ -z "$proxy" ]]; then
    proxy="$rec_proxy"
    if [[ -t 0 ]]; then
        echo
        echo "Injection proxy DLL for OptiScaler (recommended by PE import scan: $rec_proxy):"
        echo "  1) dxgi.dll (default)   2) winmm.dll   3) d3d12.dll   4) winhttp.dll"
        echo "  5) wininet.dll          6) dbghelp.dll 7) version.dll"
        read -r -p "Proxy DLL [Enter = $rec_proxy, or 1-7 / name]: " pick || pick=""
        case "$pick" in
            "") proxy="$rec_proxy";;
            1) proxy="dxgi.dll";; 2) proxy="winmm.dll";; 3) proxy="d3d12.dll";;
            4) proxy="winhttp.dll";; 5) proxy="wininet.dll";; 6) proxy="dbghelp.dll";; 7) proxy="version.dll";;
            *.dll) proxy="$pick";;
            *) echo "Using $rec_proxy"; proxy="$rec_proxy";;
        esac
    fi
fi

# 2. Ask whether to install dlssnr-amd.asi (Mochizuki 0.1.0m2)
if [[ -z "$want_nr" ]]; then
    want_nr="on"
    if [[ -t 0 ]]; then
        echo
        read -r -p "Install dlssnr-amd.asi (Mochizuki 0.1.0m2 DLSS Neural Rendering)? [Y/n]: " pick || pick=""
        [[ "$pick" == "n" || "$pick" == "N" ]] && want_nr="off"
    fi
fi

# 3. Ask whether to install openrr-amd.asi (OpenRR v0.1.7.1plus Ray Reconstruction)
if [[ -z "$want_openrr" ]]; then
    want_openrr="on"
    if [[ -t 0 ]]; then
        echo
        read -r -p "Install openrr-amd.asi (OpenRR v0.1.7.1plus DLSS Ray Reconstruction)? [Y/n]: " pick || pick=""
        [[ "$pick" == "n" || "$pick" == "N" ]] && want_openrr="off"
    fi
fi

# 4. Ask whether to install OptiPatcher.asi
if [[ -z "$want_optipatcher" ]]; then
    want_optipatcher="off"
    if [[ -t 0 ]]; then
        echo
        read -r -p "Install OptiPatcher.asi (DLSS spoofing enhancement)? [y/N]: " pick || pick=""
        [[ "$pick" == "y" || "$pick" == "Y" ]] && want_optipatcher="on"
    fi
fi

# 5. Prepare Mochizuki models if want_nr == on
model_src="" small_src="" exp_tmp="" opt_tmp="" int4_tmp="" small_int4_tmp="" ini_preserve_tmp=""
exp_weights=0 opt_weights=0 int4_weights=0 small_int4_weights=0 small_int4_sink_weights=0 have_small=0
cleanup_temps() {
    [[ -n "$exp_tmp" ]] && rm -rf -- "$exp_tmp" || true
    [[ -n "$opt_tmp" ]] && rm -rf -- "$opt_tmp" || true
    [[ -n "$int4_tmp" ]] && rm -rf -- "$int4_tmp" || true
    [[ -n "$small_int4_tmp" ]] && rm -rf -- "$small_int4_tmp" || true
    [[ -n "$ini_preserve_tmp" ]] && rm -rf -- "$ini_preserve_tmp" || true
}
trap cleanup_temps EXIT

apply_xor_delta() {
    python3 - "$1" "$2" "$3" <<'PY'
import struct, sys
from pathlib import Path
pack_path, delta_path, out_path = map(Path, sys.argv[1:4])
pack = bytearray(pack_path.read_bytes())
if pack[:8] != b"NRMODEL1": sys.exit("Not NRMODEL1")
(count_pack,) = struct.unpack_from("<I", pack, 8)
index, at = {}, 16
for _ in range(count_pack):
    (n,) = struct.unpack_from("<I", pack, at)
    name = pack[at+4:at+4+n].decode()
    off, size = struct.unpack_from("<QQ", pack, at+4+n)
    index[name] = (off, size)
    at += 4 + n + 16
d = delta_path.read_bytes()
if d[:8] != b"NRXDIFF1": sys.exit("Not NRXDIFF1")
(count,) = struct.unpack_from("<I", d, 8)
at = 12
original = bytes(pack)
added = []
def bxor(a, b): return (int.from_bytes(a, "little") ^ int.from_bytes(b, "little")).to_bytes(len(a), "little")
for _ in range(count):
    (n,) = struct.unpack_from("<I", d, at)
    name = d[at+4:at+4+n].decode()
    (size,) = struct.unpack_from("<Q", d, at+4+n)
    at += 4 + n + 8
    if name not in index:
        base = name.split("/", 1)[1] if "/" in name else ""
        off = index[base][0]
        added.append((name, bxor(original[off:off+size], d[at:at+size])))
    else:
        off = index[name][0]
        pack[off:off+size] = bxor(bytes(pack[off:off+size]), d[at:at+size])
    at += size
if added:
    grow = sum(4 + len(n.encode()) + 16 for n, _ in added)
    head = bytearray(pack[:16]); struct.pack_into("<I", head, 8, count_pack + len(added))
    lines, at = bytearray(), 16
    for _ in range(count_pack):
        (n,) = struct.unpack_from("<I", pack, at)
        off, size = struct.unpack_from("<QQ", pack, at+4+n)
        lines += pack[at:at+4+n] + struct.pack("<QQ", off + grow, size)
        at += 4 + n + 16
    end = len(pack) + grow
    for n, b in added:
        lines += struct.pack("<I", len(n.encode())) + n.encode() + struct.pack("<QQ", end, len(b))
        end += len(b)
    pack = head + lines + pack[at:] + b"".join(b for _, b in added)
out_path.write_bytes(pack)
PY
}

if [[ "$want_nr" == "on" ]]; then
    if [[ -z "$dll" ]]; then
        for cand in "$here/nvngx_dlssnr.dll" "$game/nvngx_dlssnr.dll"; do
            [[ -f "$cand" ]] && { dll="$cand"; break; }
        done
    fi
    if ! model_ok "$pkg_model"; then
        if [[ -f "$game/dlssnr-amd/CC_SILVER_AARDWOLD.bin" ]] && model_ok "$game/dlssnr-amd/CC_SILVER_AARDWOLD.bin"; then
            cp -f -- "$game/dlssnr-amd/CC_SILVER_AARDWOLD.bin" "$pkg_model" || model_src="$game/dlssnr-amd/CC_SILVER_AARDWOLD.bin"
        fi
    fi
    if ! small_ok "$pkg_small"; then
        if [[ -f "$game/dlssnr-amd/CC_SMALL_FP8.bin" ]] && small_ok "$game/dlssnr-amd/CC_SMALL_FP8.bin"; then
            cp -f -- "$game/dlssnr-amd/CC_SMALL_FP8.bin" "$pkg_small" || small_src="$game/dlssnr-amd/CC_SMALL_FP8.bin"
        fi
    fi
    if [[ -n "$dll" ]] && ! { model_ok "$pkg_model" && small_ok "$pkg_small"; }; then
        if command -v wine >/dev/null 2>&1 && [[ -f "$here/model-tools/dlssnr_extract_model.exe" ]]; then
            echo "Extracting Mochizuki models from $dll via wine ..."
            WINEDEBUG=-all wine "$here/model-tools/dlssnr_extract_model.exe" "$dll" "$here/dlssnr-amd"
        else
            echo "Note: wine is needed on Linux to run model-tools/dlssnr_extract_model.exe on $dll (or place extracted CC_*.bin in dlssnr-amd/)." >&2
        fi
    fi
    if ! model_ok "${model_src:-$pkg_model}" && ! small_ok "${small_src:-$pkg_small}"; then
        echo "Missing Mochizuki model files in dlssnr-amd/ (provide --dll <nvngx_dlssnr.dll> with wine installed, or place CC_SMALL_FP8.bin / CC_SILVER_AARDWOLD.bin in dlssnr-amd/)." >&2
        exit 1
    fi
    small_ok "${small_src:-$pkg_small}" && have_small=1

    if [[ -z "$model" ]]; then
        model="CC_SMALL_FP8"; [[ $have_small == 0 ]] && model="CC_SILVER_AARDWOLD"
        if [[ -t 0 ]]; then
            echo
            echo "Choose the starting network for dlssnr-amd.asi (can be switched in game with End / Ctrl+F11):"
            echo "  1) CC_SMALL_FP8 (default)"
            echo "  2) CC_SMALL_FP8_OPTIMIZED (ACO Mode: RX 9000 / RX 7000)"
            echo "  3) CC_SILVER_AARDWOLD"
            echo "  4) CC_SILVER_AARDWOLD_OPTIMIZED (ACO Mode: RX 9000 / RX 7000)"
            def=1; [[ $have_small == 0 ]] && def=3
            read -r -p "Start with which one? [1-4, Enter = $def]: " pick || pick=""
            case "${pick:-$def}" in
                1) model="CC_SMALL_FP8";; 2) model="CC_SMALL_FP8_OPTIMIZED";;
                3) model="CC_SILVER_AARDWOLD";; 4) model="CC_SILVER_AARDWOLD_OPTIMIZED";;
            esac
        fi
    elif [[ "$model" == "CC_SMALL_FP8_EXPERIMENTAL" ]]; then
        model="CC_SMALL_FP8_OPTIMIZED"
    fi

    if [[ -z "$renodx" ]]; then
        renodx=1
        if [[ -t 0 ]]; then
            echo
            echo "Choose processing style before/after NR:"
            echo "  1) RenoDX style (default)   2) wilsjo2 style"
            read -r -p "Which one? [1/2, Enter = 1]: " pick || pick=""
            [[ "$pick" == "2" ]] && renodx=0
        fi
    fi

    if [[ $have_small == 1 && -f "$here/CC_SMALL_FP8_OPTIMIZED/CC_SMALL_FP8_OPTIMIZED.delta" ]]; then
        echo "Making CC_SMALL_FP8_OPTIMIZED weights ..."
        exp_tmp=$(mktemp -d)
        if apply_xor_delta "${small_src:-$pkg_small}" "$here/CC_SMALL_FP8_OPTIMIZED/CC_SMALL_FP8_OPTIMIZED.delta" "$exp_tmp/CC_SMALL_FP8_OPTIMIZED.bin"; then
            exp_weights=1
            if command -v wine >/dev/null 2>&1 && [[ -f "$here/model-tools/dlssnr-int4-weights.exe" && -d "$here/dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-ultra" ]]; then
                small_int4_tmp=$(mktemp -d)
                if WINEDEBUG=-all wine "$here/model-tools/dlssnr-int4-weights.exe" --network CC_SMALL_FP8_OPTIMIZED "$exp_tmp/CC_SMALL_FP8_OPTIMIZED.bin" "$here/dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-ultra" "$small_int4_tmp/CC_SMALL_FP8_OPTIMIZED_INT4.bin" >/dev/null 2>&1; then
                    small_int4_weights=1
                fi
                if WINEDEBUG=-all wine "$here/model-tools/dlssnr-int4-weights.exe" --network CC_SMALL_FP8_OPTIMIZED --sink "$exp_tmp/CC_SMALL_FP8_OPTIMIZED.bin" "$here/dlssnr-amd/CC_SMALL_FP8_OPTIMIZED-ultra" "$small_int4_tmp/CC_SMALL_FP8_OPTIMIZED_INT4_SINK.bin" >/dev/null 2>&1; then
                    small_int4_sink_weights=1
                fi
            fi
        fi
    fi
    if model_ok "${model_src:-$pkg_model}" && [[ -f "$here/CC_SILVER_AARDWOLD_OPTIMIZED/CC_SILVER_AARDWOLD_OPTIMIZED.delta" ]]; then
        echo "Making CC_SILVER_AARDWOLD_OPTIMIZED weights ..."
        opt_tmp=$(mktemp -d)
        if apply_xor_delta "${model_src:-$pkg_model}" "$here/CC_SILVER_AARDWOLD_OPTIMIZED/CC_SILVER_AARDWOLD_OPTIMIZED.delta" "$opt_tmp/CC_SILVER_AARDWOLD_OPTIMIZED.bin"; then
            opt_weights=1
            if command -v wine >/dev/null 2>&1 && [[ -f "$here/model-tools/dlssnr-int4-weights.exe" && -d "$here/dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED-ultra" ]]; then
                int4_tmp=$(mktemp -d)
                if WINEDEBUG=-all wine "$here/model-tools/dlssnr-int4-weights.exe" --network CC_SILVER_AARDWOLD_OPTIMIZED "$opt_tmp/CC_SILVER_AARDWOLD_OPTIMIZED.bin" "$here/dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED-ultra" "$int4_tmp/CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin" >/dev/null 2>&1; then
                    int4_weights=1
                fi
            fi
        fi
    fi
fi

is_update=0
# The install record remembers the last OpenRR spoof policy (written only while OpenRR is installed).
prior_openrr_spoof=$( { sed -n 's/^# openrr_spoof=\(full\|auto\)$/\1/Ip' "$manifest" 2>/dev/null || true; } | tail -n 1)
prior_openrr_spoof=${prior_openrr_spoof,,}
prior_openrr_installed=0
# Capture legacy installation evidence before remove_installed deletes the plugin/config.
if [[ -n "$prior_openrr_spoof" || -f "$game/openrr-amd.ini" ]] ||
   grep -Eiq '^F (OptiScaler/plugins/|plugins/)?openrr(-amd)?\.asi[[:space:]]*$' "$manifest" 2>/dev/null; then
    prior_openrr_installed=1
fi
for prior_plugin in OptiScaler/plugins/openrr-amd.asi OptiScaler/plugins/openrr.asi plugins/openrr-amd.asi plugins/openrr.asi; do
    if [[ -f "$game/$prior_plugin" ]]; then prior_openrr_installed=1; fi
done
if [[ $openrr_spoof_explicit == 0 && -n "$prior_openrr_spoof" ]]; then openrr_spoof=$prior_openrr_spoof; fi
if [[ -f "$manifest" ]]; then
    is_update=1
    if [[ $overwrite_ini == 0 ]]; then
        ini_preserve_tmp=$(mktemp -d)
        for ini_name in "OptiScaler.ini" "dlssnr-amd.ini" "openrr-amd.ini"; do
            if [[ -f "$game/$ini_name" ]]; then
                cp -f -- "$game/$ini_name" "$ini_preserve_tmp/$ini_name"
            fi
        done
    fi
    echo "Removing previous installation first ..."
    remove_installed
fi

: > "$manifest"
record "# format=v2-per-file"
if [[ "$want_openrr" == "on" ]]; then record "# openrr_spoof=${openrr_spoof}"; fi
record "F dlssnr-amd-install.txt"

# 6. Install Official OptiScaler
put_file "$here/OptiScaler.dll" "$proxy"
if [[ $is_update == 1 && $overwrite_ini == 0 && -n "$ini_preserve_tmp" && -f "$ini_preserve_tmp/OptiScaler.ini" ]]; then
    put_file "$ini_preserve_tmp/OptiScaler.ini" "OptiScaler.ini"
else
    put_file "$here/OptiScaler.ini" "OptiScaler.ini"
fi
put_tree "$here/OptiScaler" "OptiScaler"
mkdir -p -- "$game/OptiScaler/plugins"
record "D OptiScaler/plugins"

# Install selected ASI plugins into OptiScaler/plugins
if [[ "$want_nr" == "on" && -f "$here/OptiScaler/plugins/dlssnr-amd.asi" ]]; then
    put_file "$here/OptiScaler/plugins/dlssnr-amd.asi" "OptiScaler/plugins/dlssnr-amd.asi"
fi
if [[ "$want_openrr" == "on" && -f "$here/OptiScaler/plugins/openrr-amd.asi" ]]; then
    put_file "$here/OptiScaler/plugins/openrr-amd.asi" "OptiScaler/plugins/openrr-amd.asi"
elif [[ "$want_openrr" == "on" && -f "$here/OptiScaler/plugins/openrr.asi" ]]; then
    put_file "$here/OptiScaler/plugins/openrr.asi" "OptiScaler/plugins/openrr-amd.asi"
fi
# Proactively remove legacy openrr-ngx-host.dll left over from OpenRR <= v0.1.7plus6.
rm -f -- "$game/OptiScaler/plugins/openrr-ngx-host.dll" "$game/plugins/openrr-ngx-host.dll" "$game/openrr-ngx-host.dll"
if [[ "$want_optipatcher" == "on" && -f "$here/OptiScaler/plugins/OptiPatcher.asi" ]]; then
    put_file "$here/OptiScaler/plugins/OptiPatcher.asi" "OptiScaler/plugins/OptiPatcher.asi"
elif [[ "$want_optipatcher" == "on" && -f "$here/plugins/OptiPatcher.asi" ]]; then
    put_file "$here/plugins/OptiPatcher.asi" "OptiScaler/plugins/OptiPatcher.asi"
fi

# Update OptiScaler.ini in-place
python3 - "$game/OptiScaler.ini" "$want_nr" "$want_openrr" "$openrr_spoof" "$prior_openrr_installed" <<'PY'
import re, sys
path, want_nr, want_openrr, mode, prior = sys.argv[1:6]
raw = open(path, "rb").read().decode("utf-8", errors="surrogateescape")
nl = "\r\n" if "\r\n" in raw else "\n"
lines = raw.split(nl)
wanted = {("Plugins", "LoadAsiPlugins"): "true", ("Log", "LogToFile"): "true", ("Log", "LogLevel"): "2"}
if want_nr == "on": wanted[("Menu", "FGShortcutKey")] = "-1"
# OpenRR's policy choice authorizes these keys; disabling OpenRR hands spoofing back to OptiScaler (auto).
spoof = ("auto" if mode == "auto" else "true") if want_openrr == "on" else ("auto" if prior == "1" else None)
if spoof:
    for key in ("Dxgi", "Vulkan", "VulkanExtensionSpoofing"):
        wanted[("Spoofing", key)] = spoof
lookup = {(section.casefold(), key.casefold()): value for (section, key), value in wanted.items()}
sec, seen = None, set()
for i, line in enumerate(lines):
    m = re.match(r"^\s*\[([^\]]+)\]", line.lstrip("\ufeff"))
    if m: sec = m.group(1).casefold(); continue
    e = re.match(r"^\s*([A-Za-z0-9_]+)\s*=(.*)$", line)
    if e and sec:
        key_name, key_val = e.group(1), e.group(2)
        k = (sec, key_name.casefold())
        # Sanitize legacy OpenRR <= v0.1.7plus6 host DLL paths in [Libraries] NvngxPath.
        if k == ("libraries", "nvngxpath") and "openrr-ngx-host" in key_val.casefold():
            lines[i] = f"{key_name}=auto"; seen.add(k); continue
        if k in lookup and k not in seen:
            lines[i] = f"{key_name}={lookup[k]}"; seen.add(k)
# Kept/older INIs may omit the entire section or individual spoofing keys.
if spoof:
    missing = [key for key in ("Dxgi", "Vulkan", "VulkanExtensionSpoofing") if ("spoofing", key.casefold()) not in seen]
    if missing:
        start = next((i for i, line in enumerate(lines) if re.match(r"^\s*\[Spoofing\]\s*$", line.lstrip("\ufeff"), re.IGNORECASE)), None)
        if start is None:
            if lines and lines[-1] != "": lines.append("")
            lines.append("[Spoofing]")
            end = len(lines)
        else:
            end = next((i for i in range(start + 1, len(lines)) if re.match(r"^\s*\[", lines[i])), len(lines))
        lines[end:end] = [f"{key}={spoof}" for key in missing]
open(path, "wb").write(nl.join(lines).encode("utf-8", errors="surrogateescape"))
PY

# Detect system language: Chinese locales configure both plugin INIs to zh-CN; others stay default English.
sys_locale="${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}"
want_zh_plugins=0
if [[ "${sys_locale,,}" == zh* ]]; then
    want_zh_plugins=1
fi

set_ini_key() {
    python3 - "$1" "$2" "$3" "$4" "$5" <<'PY'
import re, sys
path, section, key, value, only_missing = sys.argv[1:6]
only_missing = (only_missing == "1")
raw = open(path, "rb").read().decode("utf-8", errors="surrogateescape")
nl = "\r\n" if "\r\n" in raw else "\n"
lines = raw.split(nl)
sec, found_sec, replaced = None, False, False
for i, line in enumerate(lines):
    m = re.match(r"^\s*\[([^\]]+)\]", line.lstrip("\ufeff"))
    if m:
        sec = m.group(1).casefold()
        if sec == section.casefold():
            found_sec = True
        continue
    if sec == section.casefold():
        e = re.match(rf"^(\s*{re.escape(key)}\s*=).*$", line, re.IGNORECASE)
        if e:
            if not only_missing:
                lines[i] = f"{e.group(1)} {value}"
            replaced = True
            break
if not replaced:
    if not found_sec:
        if lines and lines[-1] != "":
            lines.append("")
        lines.append(f"[{section}]")
        lines.append(f"{key} = {value}")
    else:
        start = next(i for i, l in enumerate(lines) if re.match(rf"^\s*\[{re.escape(section)}\]\s*$", l.lstrip("\ufeff"), re.IGNORECASE))
        end = next((i for i in range(start + 1, len(lines)) if re.match(r"^\s*\[", lines[i])), len(lines))
        lines.insert(end, f"{key} = {value}")
open(path, "wb").write(nl.join(lines).encode("utf-8", errors="surrogateescape"))
PY
}

# 7. Install dlssnr-amd.asi resources if enabled
if [[ "$want_nr" == "on" ]]; then
    put_tree "$here/dlssnr-amd" "dlssnr-amd"
    [[ -n "$model_src" ]] && put_file "$model_src" "dlssnr-amd/CC_SILVER_AARDWOLD.bin"
    [[ -n "$small_src" ]] && put_file "$small_src" "dlssnr-amd/CC_SMALL_FP8.bin"
    if [[ $exp_weights == 1 ]]; then
        put_file "$exp_tmp/CC_SMALL_FP8_OPTIMIZED.bin" "dlssnr-amd/CC_SMALL_FP8_OPTIMIZED.bin"
        rm -f -- "$game/dlssnr-amd/CC_SMALL_FP8_EXPERIMENTAL.bin" "$game/dlssnr-amd/dlssnr-small-experimental.bin"
    fi
    if [[ $small_int4_weights == 1 ]]; then
        put_file "$small_int4_tmp/CC_SMALL_FP8_OPTIMIZED_INT4.bin" "dlssnr-amd/CC_SMALL_FP8_OPTIMIZED_INT4.bin"
    fi
    if [[ $small_int4_sink_weights == 1 ]]; then
        put_file "$small_int4_tmp/CC_SMALL_FP8_OPTIMIZED_INT4_SINK.bin" "dlssnr-amd/CC_SMALL_FP8_OPTIMIZED_INT4_SINK.bin"
    fi
    if [[ $opt_weights == 1 ]]; then
        put_file "$opt_tmp/CC_SILVER_AARDWOLD_OPTIMIZED.bin" "dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED.bin"
    fi
    if [[ $int4_weights == 1 ]]; then
        put_file "$int4_tmp/CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin" "dlssnr-amd/CC_SILVER_AARDWOLD_OPTIMIZED_INT4.bin"
    fi

    ini_src="$here/dlssnr-amd-optiscaler-renodx.ini"
    [[ "$renodx" == "0" ]] && ini_src="$here/dlssnr-amd-optiscaler.ini"
    nr_ini_fresh=0
    if [[ $is_update == 1 && $overwrite_ini == 0 && -n "$ini_preserve_tmp" && -f "$ini_preserve_tmp/dlssnr-amd.ini" ]]; then
        put_file "$ini_preserve_tmp/dlssnr-amd.ini" "dlssnr-amd.ini"
        sed -i 's/CC_SMALL_FP8_EXPERIMENTAL/CC_SMALL_FP8_OPTIMIZED/g' "$game/dlssnr-amd.ini"
    else
        put_file "$ini_src" "dlssnr-amd.ini"
        sed -i "/^\[Network\]/,/^\[/ s/^Model = .*/Model = $model/" "$game/dlssnr-amd.ini"
        nr_ini_fresh=1
    fi
    if [[ $want_zh_plugins == 1 && -f "$game/dlssnr-amd.ini" ]]; then
        set_ini_key "$game/dlssnr-amd.ini" "NRControl" "Language" "zh-CN" "$([[ $nr_ini_fresh == 1 ]] && echo 0 || echo 1)"
    fi
    put_file "$game/dlssnr-amd.ini" "dlssnr-amd/dlssnr-amd.default.ini"
    record "L dlssnr-amd.log"; record "L dlssnr-amd-crash.dmp"
fi

# 8. Install openrr-amd.asi resources if enabled
if [[ "$want_openrr" == "on" ]]; then
    put_tree "$here/openrr-amd" "openrr-amd"
    rm -rf -- "$game/openrr-amd/aco"
    rr_ini_fresh=0
    if [[ $is_update == 1 && $overwrite_ini == 0 && -n "$ini_preserve_tmp" && -f "$ini_preserve_tmp/openrr-amd.ini" ]]; then
        put_file "$ini_preserve_tmp/openrr-amd.ini" "openrr-amd.ini"
    elif [[ -f "$here/openrr-amd.ini" ]]; then
        put_file "$here/openrr-amd.ini" "openrr-amd.ini"
        rr_ini_fresh=1
    fi
    if [[ $want_zh_plugins == 1 && -f "$game/openrr-amd.ini" ]]; then
        set_ini_key "$game/openrr-amd.ini" "OpenRR" "Language" "zh-CN" "$([[ $rr_ini_fresh == 1 ]] && echo 0 || echo 1)"
    fi
    if [[ -z "$dlssd" ]]; then
        for cand in "$here/nvngx_dlssd.dll" "$here/openrr-amd/nvngx_dlssd.dll" "$game/nvngx_dlssd.dll"; do
            [[ -f "$cand" ]] && { dlssd="$cand"; break; }
        done
    fi
    if [[ -n "$dlssd" && -f "$dlssd" ]]; then
        cp -f -- "$dlssd" "$game/openrr-amd/nvngx_dlssd.dll"
        record "F openrr-amd/nvngx_dlssd.dll"
    fi
    # Configure OptiScaler's native PLUGINS -> OpenRR menu font unconditionally when OpenRR is installed
    # (only when TTFFontPath / FontSize are currently unset or auto).
    if [[ -f "$game/OptiScaler.ini" && -f "$game/openrr-amd/lang/fonts/NotoSansCJKsc-Regular.ttf" ]]; then
        python3 - "$game/OptiScaler.ini" "$game/openrr-amd/lang/fonts/NotoSansCJKsc-Regular.ttf" <<'PY'
import os, re, sys
opti_ini, font_file = sys.argv[1:3]
def strip_q(v):
    v = v.strip()
    if len(v) >= 2 and ((v[0] == '"' and v[-1] == '"') or (v[0] == "'" and v[-1] == "'")):
        return v[1:-1].strip()
    return v
def is_unset(v):
    if v is None: return True
    v = strip_q(v)
    return (not v) or bool(re.match(r"^auto(?:\s*[;#].*)?$", v, re.IGNORECASE))
raw = open(opti_ini, "rb").read().decode("utf-8", errors="surrogateescape")
nl = "\r\n" if "\r\n" in raw else "\n"
lines = raw.split(nl)
sec, cur_ttf, cur_size = None, None, None
for line in lines:
    m = re.match(r"^\s*\[([^\]]+)\]", line.lstrip("\ufeff"))
    if m: sec = m.group(1).casefold(); continue
    if sec == "menu":
        e = re.match(r"^\s*([A-Za-z0-9_]+)\s*=(.*)$", line)
        if e:
            kl = e.group(1).casefold()
            if kl == "ttffontpath" and cur_ttf is None: cur_ttf = e.group(2)
            elif kl == "fontsize" and cur_size is None: cur_size = e.group(2)
if not is_unset(cur_ttf):
    sys.exit(0)
abs_font = os.path.realpath(font_file)
wine_font = ("Z:" + abs_font.replace("/", "\\")) if abs_font.startswith("/") else abs_font
updates = {"UseHQFont": "true", "TTFFontPath": wine_font}
if is_unset(cur_size):
    updates["FontSize"] = "16"
lookup = {k.casefold(): v for k, v in updates.items()}
sec, seen = None, set()
for i, line in enumerate(lines):
    m = re.match(r"^\s*\[([^\]]+)\]", line.lstrip("\ufeff"))
    if m: sec = m.group(1).casefold(); continue
    if sec == "menu":
        e = re.match(r"^\s*([A-Za-z0-9_]+)\s*=", line)
        if e:
            kl = e.group(1).casefold()
            if kl in lookup and kl not in seen:
                lines[i] = f"{e.group(1)}={lookup[kl]}"; seen.add(kl)
missing = [(k, v) for k, v in updates.items() if k.casefold() not in seen]
if missing:
    start = next((i for i, l in enumerate(lines) if re.match(r"^\s*\[Menu\]\s*$", l.lstrip("\ufeff"), re.IGNORECASE)), None)
    if start is None:
        if lines and lines[-1] != "": lines.append("")
        lines.append("[Menu]")
        end = len(lines)
    else:
        end = next((i for i in range(start + 1, len(lines)) if re.match(r"^\s*\[", lines[i])), len(lines))
    lines[end:end] = [f"{k}={v}" for k, v in missing]
open(opti_ini, "wb").write(nl.join(lines).encode("utf-8", errors="surrogateescape"))
PY
    fi
    record "L openrr-amd.log"; record "L openrr-control.bin"; record "L openrr-amd-crash.dmp"
fi

record "L OptiScaler.log"

proxy_stem="${proxy%.dll}"
echo
echo "Installed into: $game"
echo "  OptiScaler proxy:  $proxy"
echo "  dlssnr-amd.asi:    $want_nr$([[ "$want_nr" == "on" ]] && echo " (model: $model)")"
echo "  openrr-amd.asi:    $want_openrr"
echo "  OptiPatcher.asi:   $want_optipatcher"
echo
echo "Steam Launch Options (required for Proton to load $proxy):"
echo "  WINEDLLOVERRIDES=\"${proxy_stem}=n,b\" %command%"
echo
echo "In game: Insert opens OptiScaler (PLUGINS -> OpenRR); End opens NR Control."
echo "Uninstall: bash install.sh \"$game\" remove"
