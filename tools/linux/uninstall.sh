#!/usr/bin/env bash
# OptiScaler NR - uninstall helper (Linux).
#
#   bash uninstall.sh "<game folder>"     remove OptiScaler NR / DLSSNR-AMD from one game
#                                         (runs this package's install.sh "<game folder>" remove)
#   bash uninstall.sh --all               remove it from every game the launcher installed into
#   bash uninstall.sh --purge-settings    delete the launcher's settings and cache
#   bash uninstall.sh --desktop           remove the application menu entry
#
# Options can be combined, e.g.  bash uninstall.sh --all --purge-settings --desktop
# Only the files listed in each game's dlssnr-amd-install.txt are deleted. Delete the package folder
# yourself afterwards if you no longer need it (it may hold the extracted model).
set -euo pipefail

here=$(cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")" && pwd)
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/optiscaler-nr"
cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/optiscaler-nr"
data_dir="${XDG_DATA_HOME:-$HOME/.local/share}"
state="$config_dir/launcher.json"

usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; }

all=0 purge=0 desk=0
games=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --all) all=1;;
        --purge-settings) purge=1;;
        --desktop) desk=1;;
        -h|--help) usage; exit 0;;
        -*) echo "unknown option: $1" >&2; usage >&2; exit 2;;
        *) games+=("$1");;
    esac
    shift
done
if (( ! all && ! purge && ! desk )) && [[ ${#games[@]} -eq 0 ]]; then
    usage >&2; exit 2
fi
[[ -f "$here/install.sh" ]] || { echo "install.sh not found next to $0" >&2; exit 1; }

if (( all )); then
    if [[ -f "$state" ]]; then
        # game folders recorded by the launcher, one per line (NUL-free paths only)
        while IFS= read -r folder; do
            [[ -n "$folder" ]] && games+=("$folder")
        done < <(python3 - "$state" <<'PY'
import json, sys
try:
    data = json.load(open(sys.argv[1], encoding="utf-8"))
except Exception:
    sys.exit(0)
for folder in sorted((data.get("installs") or {}).keys()):
    if "\n" not in folder:
        print(folder)
PY
)
    fi
    [[ ${#games[@]} -gt 0 ]] || echo "The launcher has no recorded installations."
fi

status=0
for game in "${games[@]}"; do
    if [[ ! -d "$game" ]]; then
        echo "skipped (folder not found): $game" >&2
        continue
    fi
    if [[ ! -f "$game/dlssnr-amd-install.txt" ]]; then
        echo "nothing installed in: $game"
    elif ! bash "$here/install.sh" "$game" remove < /dev/null; then
        status=1
        continue
    fi
    # forget it in the launcher's state as well
    if [[ -f "$state" ]]; then
        python3 - "$state" "$game" <<'PY' || true
import json, os, sys, tempfile
path, game = sys.argv[1], os.path.realpath(sys.argv[2])
try:
    data = json.load(open(path, encoding="utf-8"))
except Exception:
    sys.exit(0)
installs = data.get("installs") or {}
for key in [k for k in installs if os.path.realpath(k) == game]:
    del installs[key]
fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path))
with os.fdopen(fd, "w", encoding="utf-8") as f:
    json.dump(data, f, ensure_ascii=False, indent=2, sort_keys=True)
os.replace(tmp, path)
PY
    fi
done

if (( desk )); then
    rm -f -- "$data_dir/applications/optiscaler-nr-launcher.desktop" \
             "$data_dir/icons/hicolor/scalable/apps/optiscaler-nr-launcher.svg"
    echo "Removed the application menu entry."
fi
if (( purge )); then
    rm -rf -- "$config_dir" "$cache_dir"
    echo "Removed the launcher settings ($config_dir) and cache ($cache_dir)."
fi
exit "$status"
