#!/usr/bin/env python3
"""Run the desktop executable as a detached background process.

Same shape as flashing ESP32: this script does the launch and exits cleanly.
The app keeps running until the user (or another `run_desktop` invocation)
stops it. A re-run kills any prior instance first, so "Run" is idempotent —
press it twice in a row and you get one fresh app, not two.

Why detached: if we waited (or `os.execv`d), MoonDeck would track this
process's PID and terminate it as soon as another script was launched, which
is exactly the behaviour the user asked us to drop. Detaching releases the
PID and lets the app outlive this script (mirroring how flashing an ESP32
leaves the device running independently).
"""

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
EXECUTABLE = ROOT / "build" / "projectMM"
if sys.platform == "win32":
    EXECUTABLE = EXECUTABLE.with_suffix(".exe")


def _is_running() -> bool:
    """True iff an instance of the executable is currently running."""
    name = EXECUTABLE.name
    if sys.platform == "win32":
        # EXECUTABLE already carries the .exe suffix on Windows (see the top of
        # this file); appending it a second time would produce "projectMM.exe.exe"
        # and tasklist would match nothing.
        r = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {name}"],
            capture_output=True, text=True)
        return name in r.stdout
    r = subprocess.run(["pgrep", "-f", str(EXECUTABLE)], capture_output=True)
    return r.returncode == 0


def _kill_running():
    """Terminate every running instance. Idempotent."""
    if sys.platform == "win32":
        subprocess.run(["taskkill", "/F", "/IM", EXECUTABLE.name],
                       capture_output=True)
    else:
        # Match the full path to avoid hitting unrelated programs called "projectMM".
        subprocess.run(["pkill", "-f", str(EXECUTABLE)], capture_output=True)


def main():
    if not EXECUTABLE.exists():
        print(f"Executable not found: {EXECUTABLE}")
        print("Run build_desktop.py first.")
        sys.exit(1)

    if _is_running():
        print(f"Existing {EXECUTABLE.name} found — stopping it first.")
        _kill_running()
        # Give the OS a moment to release sockets the app was holding (HTTP :8080).
        for _ in range(20):
            if not _is_running():
                break
            time.sleep(0.05)

    print(f"Launching {EXECUTABLE} (detached)...")
    sys.stdout.flush()

    # Detach: own session, stdio disconnected from this script, output redirected
    # to a log file the user can tail. The app survives this script exiting and
    # is unaffected by MoonDeck terminating other scripts.
    log_path = ROOT / "build" / "projectMM.log"
    log_fp = open(log_path, "wb")  # closed when this script exits; the child keeps the fd

    popen_kwargs = dict(
        stdin=subprocess.DEVNULL,
        stdout=log_fp,
        stderr=subprocess.STDOUT,
        cwd=str(ROOT),
        close_fds=True,
    )
    if sys.platform == "win32":
        popen_kwargs["creationflags"] = (
            subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
        )
    else:
        popen_kwargs["start_new_session"] = True  # own session, immune to our SIGTERM

    proc = subprocess.Popen([str(EXECUTABLE)], **popen_kwargs)
    print(f"PID {proc.pid} — log: {log_path}")
    print("Press the Run button again to restart; the app keeps running otherwise.")


if __name__ == "__main__":
    main()
