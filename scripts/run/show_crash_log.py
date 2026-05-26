#!/usr/bin/env python3
"""Print the most recent projectMM crash report from the OS diagnostic store.

macOS writes .ips crash reports to ~/Library/Logs/DiagnosticReports/.
This script finds the newest projectMM-*.ips, extracts the key fields
(exception type, faulting thread, call stack), and prints them so they
appear in MoonDeck's log stream next to projectMM.log.

If no crash report exists it prints the last 40 lines of projectMM.log
so the run log is always visible from one place.
"""

import json
import platform
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def _build_dir() -> Path:
    return ROOT / "build" / {
        "Darwin": "macos",
        "Linux": "linux",
        "Windows": "windows",
    }.get(platform.system(), platform.system().lower())


def _crash_reports_dir() -> Path:
    return Path.home() / "Library" / "Logs" / "DiagnosticReports"


def _find_latest_ips() -> Path | None:
    d = _crash_reports_dir()
    if not d.exists():
        return None
    candidates = sorted(d.glob("projectMM-*.ips"), key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0] if candidates else None


def _print_ips(path: Path) -> None:
    print(f"=== macOS crash report: {path.name} ===")
    try:
        data = json.loads(path.read_text())
    except Exception as e:
        print(f"(could not parse: {e})")
        print(path.read_text()[:2000])
        return

    exc = data.get("exception", {})
    print(f"Type    : {exc.get('type','?')} — {exc.get('signal','?')}")
    print(f"Subtype : {exc.get('subtype','?')}")
    print(f"PID     : {data.get('pid','?')}  uptime: {data.get('uptime','?')} ms")
    print(f"Captured: {data.get('captureTime','?')}")

    threads = data.get("threads", [])
    for t in threads:
        if not t.get("triggered"):
            continue
        print(f"\nFaulting thread {t.get('id','?')} ({t.get('queue','')}):")
        for i, frame in enumerate(t.get("frames", [])[:20]):
            sym = frame.get("symbol", "?")
            off = frame.get("symbolLocation", "")
            print(f"  #{i:<2} {sym}  +{off}")
        break


def _print_run_log() -> None:
    log = _build_dir() / "projectMM.log"
    if not log.exists():
        print("No projectMM.log found.")
        return
    lines = log.read_text(errors="replace").splitlines()
    print(f"=== Last {min(40, len(lines))} lines of projectMM.log ===")
    for line in lines[-40:]:
        print(line)


def main() -> None:
    ips = _find_latest_ips()
    if ips:
        _print_ips(ips)
        print()
    else:
        if _crash_reports_dir().exists():
            print("No projectMM crash reports found in DiagnosticReports.")
        else:
            print("DiagnosticReports directory not found (non-macOS or sandboxed).")
        print()

    _print_run_log()


if __name__ == "__main__":
    main()
