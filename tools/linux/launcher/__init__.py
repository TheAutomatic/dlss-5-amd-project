"""OptiScaler NR - Linux launcher.

A small, dependency-free (Python 3.8+ standard library only) local web UI that drives the
upstream DLSSNR-AMD Linux installer (``install.sh`` in the package root) non-interactively.
The server binds to 127.0.0.1 only, requires a random per-session token on every API call and
shuts itself down when the UI window goes away.
"""

APP_ID = "optiscaler-nr"
APP_NAME = "OptiScaler NR"
