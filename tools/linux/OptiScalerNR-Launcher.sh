#!/usr/bin/env bash
# OptiScaler NR - Linux launcher.
#
#   ./OptiScalerNR-Launcher.sh                 open the launcher window
#   ./OptiScalerNR-Launcher.sh --no-browser    print the local address instead of opening a window
#   ./OptiScalerNR-Launcher.sh --port 8765     use a fixed port (default: random)
#   ./OptiScalerNR-Launcher.sh --install-desktop   add "OptiScaler NR" to the application menu
#
# The launcher is a small local web UI (Python 3.8+ standard library only) that runs this package's
# install.sh for you. It listens on 127.0.0.1 only and stops when its window is closed.
set -euo pipefail

here=$(cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")" && pwd)
py=${PYTHON:-python3}

# Show a message also when started from the application menu (no terminal).
fail() {
    echo "OptiScaler NR: $1" >&2
    if [[ ! -t 2 ]]; then
        if command -v zenity > /dev/null 2>&1; then zenity --error --title="OptiScaler NR" --text="$1" 2> /dev/null || true
        elif command -v kdialog > /dev/null 2>&1; then kdialog --title "OptiScaler NR" --error "$1" 2> /dev/null || true
        elif command -v notify-send > /dev/null 2>&1; then notify-send "OptiScaler NR" "$1" 2> /dev/null || true
        fi
    fi
    exit 1
}

command -v "$py" > /dev/null 2>&1 ||
    fail "python3 was not found. Install it with your package manager (it is also needed by install.sh)."
"$py" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' 2> /dev/null ||
    fail "Python 3.8 or newer is needed (found: $("$py" -V 2>&1))."
[[ -f "$here/launcher/__main__.py" ]] || fail "launcher/ is missing next to $0; extract the whole package again."

export OPTISCALER_NR_ENTRY="$here/OptiScalerNR-Launcher.sh"
export PYTHONDONTWRITEBYTECODE=1
export PYTHONPATH="$here${PYTHONPATH:+:$PYTHONPATH}"
exec "$py" -m launcher --package "$here" "$@"
