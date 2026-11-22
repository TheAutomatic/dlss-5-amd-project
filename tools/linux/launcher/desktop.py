"""Opening the UI as an app window, and the optional .desktop entry."""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

from .state import cache_dir

WINDOW_SIZE = "1180,760"

# Chromium-family browsers that support --app (a chromeless window that looks like an application)
_NATIVE = ("chromium", "chromium-browser", "google-chrome", "google-chrome-stable", "microsoft-edge",
           "microsoft-edge-stable", "brave-browser", "brave")
_FLATPAK = ("org.chromium.Chromium", "com.google.Chrome", "com.microsoft.Edge", "com.brave.Browser")


def _flatpak_installed(app_id: str) -> bool:
    home = Path.home()
    for base in (Path("/var/lib/flatpak/app"), home / ".local/share/flatpak/app"):
        if (base / app_id).is_dir():
            return True
    return False


def candidates() -> list:
    """Ordered list of (label, argv-prefix, supports_profile)."""
    out = []
    for name in _NATIVE:
        path = shutil.which(name)
        if path:
            out.append((name, [path], True))
    if shutil.which("flatpak"):
        for app_id in _FLATPAK:
            if _flatpak_installed(app_id):
                # the sandbox cannot necessarily see our cache dir, so no private profile there
                out.append((app_id, ["flatpak", "run", app_id], False))
    return out


def open_ui(url: str, log=print) -> str | None:
    """Open url in an app-mode window; fall back to xdg-open. Returns what was used (or None)."""
    profile = cache_dir() / "browser-profile"
    for label, argv, supports_profile in candidates():
        args = list(argv) + ["--app=" + url, "--window-size=" + WINDOW_SIZE, "--no-first-run",
                             "--no-default-browser-check", "--class=OptiScalerNR", "--name=OptiScalerNR"]
        if supports_profile:
            try:
                profile.mkdir(parents=True, exist_ok=True)
                args.append("--user-data-dir=" + str(profile))
            except OSError:
                pass
        if _spawn(args):
            return label
    opener = shutil.which("xdg-open") or shutil.which("gio")
    if opener:
        args = [opener, url] if opener.endswith("xdg-open") else [opener, "open", url]
        if _spawn(args):
            return os.path.basename(opener)
    log("No browser could be started. Open this address yourself: %s" % url)
    return None


def _spawn(args) -> bool:
    try:
        subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True, close_fds=True)
        return True
    except OSError:
        return False


def open_path(path: str) -> bool:
    """Show a folder in the file manager."""
    opener = shutil.which("xdg-open")
    if opener:
        return _spawn([opener, str(path)])
    gio = shutil.which("gio")
    return bool(gio) and _spawn([gio, "open", str(path)])


# -------------------------------------------------------------------------------------------------
# .desktop entry
# -------------------------------------------------------------------------------------------------
DESKTOP_NAME = "optiscaler-nr-launcher.desktop"
ICON_NAME = "optiscaler-nr-launcher"


def _data_home() -> Path:
    return Path(os.environ.get("XDG_DATA_HOME") or (Path.home() / ".local" / "share"))


def desktop_paths() -> tuple:
    data = _data_home()
    return (data / "applications" / DESKTOP_NAME,
            data / "icons" / "hicolor" / "scalable" / "apps" / (ICON_NAME + ".svg"))


def _desktop_quote(arg: str) -> str:
    """Quote an Exec argument per the Desktop Entry spec."""
    out = str(arg)
    for ch in ("\\", '"', "`", "$"):
        out = out.replace(ch, "\\" + ch)
    return '"%s"' % out


def install_desktop(entry_script: Path, icon_svg: Path, version: str, arch: str) -> Path:
    desktop, icon = desktop_paths()
    desktop.parent.mkdir(parents=True, exist_ok=True)
    icon.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copyfile(str(icon_svg), str(icon))
        icon_value = ICON_NAME
    except OSError:
        icon_value = "applications-games"
    text = "\n".join([
        "[Desktop Entry]",
        "Type=Application",
        "Version=1.0",
        "Name=OptiScaler NR (%s)" % arch,
        "GenericName=DLSSNR-AMD installer",
        "Comment=Install OptiScaler NR / DLSSNR-AMD into Proton games (%s)" % version,
        "Comment[zh_CN]=为 Proton 游戏安装 OptiScaler NR / DLSSNR-AMD (%s)" % version,
        "Exec=%s" % _desktop_quote(str(entry_script)),
        "Icon=%s" % icon_value,
        "Terminal=false",
        "Categories=Game;Utility;",
        "StartupWMClass=OptiScalerNR",
        "",
    ])
    desktop.write_text(text, encoding="utf-8")
    desktop.chmod(0o755)
    return desktop


def remove_desktop() -> list:
    removed = []
    for p in desktop_paths():
        try:
            p.unlink()
            removed.append(str(p))
        except OSError:
            pass
    return removed
