"""Command line entry: ``python3 -m launcher [--no-browser] [--port N] [--package DIR] ...``."""
from __future__ import annotations

import argparse
import os
import signal
import sys
import threading
from pathlib import Path

from . import APP_NAME, desktop
from .package import Package, find_package
from .server import App, make_server, url_for
from .state import State


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="OptiScalerNR-Launcher.sh",
                                 description="%s - Linux launcher (local web UI for DLSSNR-AMD install.sh)" % APP_NAME)
    ap.add_argument("--no-browser", action="store_true", help="do not open a window; print the address instead")
    ap.add_argument("--port", type=int, default=0, help="listen on this port (default: random)")
    ap.add_argument("--package", metavar="DIR", help="package folder holding install.sh (default: next to the launcher)")
    ap.add_argument("--install-desktop", action="store_true", help="add a menu entry (.desktop) for this package and exit")
    ap.add_argument("--remove-desktop", action="store_true", help="remove the menu entry and exit")
    ap.add_argument("--version", action="store_true", help="print the version and exit")
    args = ap.parse_args(argv)

    pkg = Package(find_package(args.package))
    entry = Path(os.environ.get("OPTISCALER_NR_ENTRY") or (pkg.root / "OptiScalerNR-Launcher.sh" if pkg.root else ""))

    if args.version:
        print("%s %s (%s)" % (APP_NAME, pkg.version, pkg.arch))
        return 0
    if args.remove_desktop:
        for p in desktop.remove_desktop():
            print("removed", p)
        return 0
    if args.install_desktop:
        if not pkg.root or not entry.is_file():
            print("OptiScalerNR-Launcher.sh not found; run this from an extracted package.", file=sys.stderr)
            return 1
        path = desktop.install_desktop(entry, Path(__file__).resolve().parent / "web" / "icon.svg", pkg.version, pkg.arch)
        print("Menu entry created:", path)
        return 0

    if not pkg.ok:
        print("Warning: the package payload (install.sh, dlssnr-amd/) was not found; installing is disabled. "
              "Use --package <folder>.", file=sys.stderr)

    app = App(pkg, State(), wait_for_first_contact=args.no_browser, entry_script=entry if entry.is_file() else None)
    try:
        httpd = make_server(app, args.port)
    except OSError as e:
        print("Cannot listen on 127.0.0.1:%d: %s" % (args.port, e), file=sys.stderr)
        return 1
    url = url_for(app)
    print("%s %s (%s) - %s" % (APP_NAME, pkg.version, pkg.arch, pkg.root or "no package"), flush=True)
    print("UI: %s" % url, flush=True)

    def stop(*_):
        app.shutdown()
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    threading.Thread(target=app.watchdog, name="watchdog", daemon=True).start()
    if not args.no_browser:
        used = desktop.open_ui(url)
        if used:
            print("Opened with %s. Closing the window quits the launcher." % used, flush=True)
        else:
            app.wait_for_first_contact = True
    try:
        httpd.serve_forever(poll_interval=0.5)
    finally:
        app.quit.set()
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
