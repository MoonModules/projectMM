#!/usr/bin/env python3
"""Build the ESP32 target."""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ESP32_DIR = ROOT / "esp32"

# Common ESP-IDF install locations
IDF_SEARCH_PATHS = [
    Path.home() / "esp" / "esp-idf",
    Path.home() / ".espressif" / "esp-idf",
    Path("/opt/esp-idf"),
]


def find_idf() -> Path | None:
    """Find ESP-IDF installation. Checks IDF_PATH env, then common locations."""
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        p = Path(idf_path)
        if (p / "tools" / "idf.py").exists():
            return p

    for p in IDF_SEARCH_PATHS:
        if (p / "tools" / "idf.py").exists():
            return p
    return None


def find_idf_python() -> Path | None:
    """Find the ESP-IDF Python venv. Prefers most recently modified."""
    venv_dir = Path.home() / ".espressif" / "python_env"
    if not venv_dir.exists():
        return None
    candidates = []
    for d in venv_dir.iterdir():
        python = d / "bin" / "python"
        if python.exists():
            candidates.append((d.stat().st_mtime, d))
    if candidates:
        candidates.sort(reverse=True)
        return candidates[0][1]
    return None


def idf_version(idf_path: Path) -> str:
    """Extract a clean semver from the IDF version string."""
    version_file = idf_path / "version.txt"
    if version_file.exists():
        raw = version_file.read_text().strip()
        # Extract major.minor.patch from strings like "v6.1-dev-399-gd1b91b79b5"
        m = re.match(r"v?(\d+\.\d+)(?:\.(\d+))?", raw)
        if m:
            return f"{m.group(1)}.{m.group(2) or '0'}"
    return "5.4.0"  # safe fallback


def idf_env(idf_path: Path) -> dict:
    """Build the environment for running idf.py."""
    env = dict(os.environ)
    env["IDF_PATH"] = str(idf_path)
    env["ESP_IDF_VERSION"] = idf_version(idf_path)

    venv_path = find_idf_python()
    if venv_path:
        env["IDF_PYTHON_ENV_PATH"] = str(venv_path)

    # Build PATH: venv bin + IDF tools + toolchains + existing PATH
    extra_paths = []

    if venv_path:
        extra_paths.append(str(venv_path / "bin"))

    extra_paths.append(str(idf_path / "tools"))

    # Add toolchain paths from ~/.espressif/tools
    tools_dir = Path.home() / ".espressif" / "tools"
    if tools_dir.exists():
        for tool in tools_dir.iterdir():
            if tool.is_dir():
                for version_dir in sorted(tool.iterdir(), reverse=True):
                    bin_dirs = list(version_dir.rglob("bin"))
                    if bin_dirs:
                        extra_paths.append(str(bin_dirs[0]))
                        break

    env["PATH"] = os.pathsep.join(extra_paths + [env.get("PATH", "")])
    return env


def idf_cmd(idf_path: Path) -> list[str]:
    """Return the command to invoke idf.py via the venv Python."""
    venv_path = find_idf_python()
    if venv_path:
        return [str(venv_path / "bin" / "python"), str(idf_path / "tools" / "idf.py")]
    return [str(idf_path / "tools" / "idf.py")]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--env", default="esp32", help="ESP32 chip type")
    args = parser.parse_args()

    if not ESP32_DIR.exists():
        print(f"ESP32 project directory not found: {ESP32_DIR}")
        sys.exit(1)

    idf_path = find_idf()
    if not idf_path:
        print("ESP-IDF not found. Install it or set IDF_PATH.")
        print("Searched: " + ", ".join(str(p) for p in IDF_SEARCH_PATHS))
        sys.exit(1)

    print(f"Using ESP-IDF at {idf_path}")
    env = idf_env(idf_path)
    cmd = idf_cmd(idf_path)

    build_dir = ESP32_DIR / "build"

    if not build_dir.exists():
        print(f"Setting target to {args.env}...")
        r = subprocess.run(cmd + ["set-target", args.env],
                           cwd=ESP32_DIR, env=env)
        if r.returncode != 0:
            sys.exit(r.returncode)

    print(f"Building for {args.env}...")
    r = subprocess.run(cmd + ["build"], cwd=ESP32_DIR, env=env)
    if r.returncode != 0:
        sys.exit(r.returncode)

    # Show flash/RAM usage summary
    subprocess.run(cmd + ["size"], cwd=ESP32_DIR, env=env)


if __name__ == "__main__":
    main()
