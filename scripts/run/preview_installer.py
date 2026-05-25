#!/usr/bin/env python3
"""Locally preview the web installer at docs/install/index.html.

Stages a small directory (the install page + the shared release-picker
module) and serves it with `python -m http.server`. Equivalent to
"Recipe A" in docs/install/README.md — render-only: the picker
populates against the real GitHub Releases API, dropdowns work, but
clicking Install fails because the local server has no `releases/`
tree. Useful for iterating on HTML/CSS/JS without tagging a release.

Long-running. MoonDeck shows a Stop button while this is up; pressing
Stop kills the python process (matched by script name via pkill).
"""

import http.server
import socketserver
import sys
import shutil
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
INSTALL_DIR = ROOT / "docs" / "install"
PICKER_JS = ROOT / "src" / "ui" / "release-picker.js"
STAGE_DIR = ROOT / "build" / "install-preview"
PORT = 8000


def stage():
    """Refresh the preview dir with the current source files.

    Re-staging on every run keeps the local copy honest — if you edit
    index.html or release-picker.js while the server is up, restart
    via the Stop/Run cycle to pick up changes (or rely on the
    `?nocache=1` query parameter the picker honours).
    """
    if STAGE_DIR.exists():
        shutil.rmtree(STAGE_DIR)
    STAGE_DIR.mkdir(parents=True)

    if not INSTALL_DIR.exists():
        print(f"ERROR: install source not found at {INSTALL_DIR}", file=sys.stderr)
        sys.exit(1)
    if not PICKER_JS.exists():
        print(f"ERROR: release-picker.js not found at {PICKER_JS}", file=sys.stderr)
        sys.exit(1)

    shutil.copy(INSTALL_DIR / "index.html", STAGE_DIR / "index.html")
    shutil.copy(PICKER_JS, STAGE_DIR / "release-picker.js")


def main() -> int:
    stage()

    print(f"==> staged {STAGE_DIR}")
    print(f"==> serving at http://localhost:{PORT}/")
    print("    open in Chrome / Edge / Opera (Web Serial requires one of these)")
    print("    add ?nocache=1 to bypass the picker's 5-min sessionStorage cache")
    print("    Install button fails on render-only preview (no releases/ tree)")
    print()
    sys.stdout.flush()

    handler = http.server.SimpleHTTPRequestHandler

    class QuietTCPServer(socketserver.ThreadingTCPServer):
        allow_reuse_address = True   # avoids "Address already in use" on quick restart
        daemon_threads = True

    # cwd=STAGE_DIR equivalent: serve files from STAGE_DIR by chdir'ing first.
    # SimpleHTTPRequestHandler serves from the current working directory.
    import os
    os.chdir(STAGE_DIR)

    try:
        with QuietTCPServer(("", PORT), handler) as httpd:
            httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n==> stopped")
    except OSError as e:
        if e.errno == 48 or "Address already in use" in str(e):
            print(f"ERROR: port {PORT} is in use. Another preview already running?",
                  file=sys.stderr)
            return 1
        raise
    return 0


if __name__ == "__main__":
    sys.exit(main())
