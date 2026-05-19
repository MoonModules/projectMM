#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Collect KPI data for commit descriptions.

Usage:
  uv run scripts/check/collect_kpi.py           # full report
  uv run scripts/check/collect_kpi.py --commit   # commit message format
"""

import argparse
import subprocess
import sys
import os
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_DIR = ROOT / "build"
ESP32_DIR = ROOT / "esp32"

sys.path.insert(0, str(ROOT / "scripts" / "build"))

def run(cmd, cwd=None, timeout=30):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, timeout=timeout)
    return r.stdout + r.stderr, r.returncode

# ---------------------------------------------------------------------------
# Collectors — return dicts of KPI data
# ---------------------------------------------------------------------------

def collect_desktop():
    kpi = {}

    mmv3 = BUILD_DIR / "mmv3"
    if mmv3.exists():
        kpi["binary_kb"] = mmv3.stat().st_size // 1024

    test_exe = BUILD_DIR / "test" / "mm_tests"
    if test_exe.exists():
        out, rc = run([str(test_exe)], cwd=BUILD_DIR)
        for line in out.splitlines():
            if "test cases:" in line:
                kpi["tests"] = line.strip()
                break

    scenarios = BUILD_DIR / "test" / "mm_scenarios"
    if scenarios.exists():
        out, rc = run([str(scenarios)], cwd=ROOT)
        fps_values = []
        for line in out.splitlines():
            if "FPS:" in line:
                parts = line.strip().split()
                for i, p in enumerate(parts):
                    if p == "FPS:" and i + 1 < len(parts):
                        try: fps_values.append(int(parts[i + 1]))
                        except ValueError: pass
            if "scenario(s)" in line:
                kpi["scenarios"] = line.strip()
        if fps_values:
            kpi["fps"] = fps_values

    check = ROOT / "scripts" / "check" / "check_platform_boundary.py"
    if check.exists():
        _, rc = run([sys.executable, str(check)])
        kpi["boundary"] = "PASS" if rc == 0 else "FAIL"

    return kpi

def collect_esp32():
    kpi = {}
    esp32_build = ESP32_DIR / "build"
    if not esp32_build.exists():
        return kpi

    try:
        from build_esp32 import find_idf, idf_env, idf_cmd
        idf_path = find_idf()
        if idf_path:
            env = idf_env(idf_path)
            cmd = idf_cmd(idf_path)
            r = subprocess.run(cmd + ["size"], capture_output=True, text=True,
                               cwd=ESP32_DIR, env=env, timeout=60)
            out = r.stdout + r.stderr

            flash_total = 0
            for line in out.splitlines():
                parts = [p.strip() for p in line.split("│") if p.strip()]
                if len(parts) >= 2:
                    name = parts[0]
                    try: used = int(parts[1])
                    except ValueError: continue
                    if "Flash Code" in name or "Flash Data" in name:
                        flash_total += used
                    if name == "DRAM" and len(parts) >= 4:
                        kpi["dram_used"] = used
                        try: kpi["dram_free"] = int(parts[3])
                        except ValueError: pass
                if "app partition" in line and "free" in line:
                    # Extract percentage: "0x9fee0 bytes (62%) free"
                    import re
                    m = re.search(r'\((\d+)%\)\s+free', line)
                    if m:
                        kpi["partition_free_pct"] = int(m.group(1))
                if "Total image size" in line:
                    import re
                    m = re.search(r'(\d+)\s+bytes', line)
                    if m:
                        kpi["image_bytes"] = int(m.group(1))
            if flash_total:
                kpi["flash_kb"] = flash_total // 1024
    except Exception:
        pass

    log = ESP32_DIR / "monitor.log"
    if log.exists():
        age_min = (time.time() - log.stat().st_mtime) / 60
        if age_min < 60:
            for line in reversed(log.read_text().splitlines()):
                if "FPS:" in line and "free:" in line:
                    parts = line.strip().split()
                    for i, p in enumerate(parts):
                        if p == "FPS:" and i + 1 < len(parts):
                            try: kpi["fps"] = int(parts[i + 1])
                            except ValueError: pass
                        if p == "free:" and i + 1 < len(parts):
                            try: kpi["heap_free"] = int(parts[i + 1])
                            except ValueError: pass
                    break

    return kpi

def collect_code():
    kpi = {}
    src_dir = ROOT / "src"
    test_dir = ROOT / "test"

    src_files = list(src_dir.rglob("*.h")) + list(src_dir.rglob("*.cpp"))
    kpi["src_files"] = len(src_files)
    kpi["src_lines"] = sum(f.read_text().count("\n") for f in src_files)

    test_files = [f for f in test_dir.rglob("*.cpp") if f.name != "doctest.h"]
    kpi["test_files"] = len(test_files)
    kpi["test_lines"] = sum(f.read_text().count("\n") for f in test_files)

    kpi["specs"] = len(list((ROOT / "docs" / "moonmodules").rglob("*.md")))
    kpi["scenarios"] = len(list((ROOT / "test" / "scenarios").rglob("*.json")))

    # Lizard
    out, _ = run(
        ["uv", "run", "--with", "lizard", "python3", "-m", "lizard",
         "src/", "-l", "cpp", "-T", "nloc=60", "-T", "cyclomatic_complexity=10",
         "-x", "src/ui/*"],
        cwd=ROOT, timeout=30
    )
    warnings = []
    in_warnings = False
    for line in out.splitlines():
        if "Warning" in line and "!!!!" in line:
            in_warnings = True
            continue
        if in_warnings and "---" in line:
            continue
        if in_warnings and line.strip() and "NLOC" not in line and "Total" not in line and "====" not in line:
            warnings.append(line.strip())
        if in_warnings and "Total" in line:
            in_warnings = False
    kpi["lizard_warnings"] = len(warnings)
    kpi["lizard_details"] = warnings

    return kpi

# ---------------------------------------------------------------------------
# Formatters
# ---------------------------------------------------------------------------

def format_oneliner(desktop, esp32, code):
    parts = []
    if "binary_kb" in desktop:
        parts.append(f"PC:{desktop['binary_kb']}KB")
    if "fps" in desktop:
        parts.append(f"FPS:{'/'.join(str(f) for f in desktop['fps'])}")
    if "flash_kb" in esp32:
        parts.append(f"ESP32:{esp32['flash_kb']}KB")
    if "fps" in esp32:
        parts.append(f"FPS:{esp32['fps']}")
    if "heap_free" in esp32:
        parts.append(f"heap:{esp32['heap_free']//1024}KB")
    parts.append(f"src:{code['src_files']}({code['src_lines']})")
    parts.append(f"test:{code['test_files']}({code['test_lines']})")
    if code["lizard_warnings"] > 0:
        parts.append(f"lizard:{code['lizard_warnings']}w")
    return "KPI: " + " | ".join(parts)

def format_full(desktop, esp32, code):
    lines = []
    lines.append("KPI Details:")

    lines.append("  Desktop:")
    if "binary_kb" in desktop:
        lines.append(f"    Binary: {desktop['binary_kb']} KB")
    if "tests" in desktop:
        lines.append(f"    {desktop['tests']}")
    if "fps" in desktop:
        lines.append(f"    FPS: {', '.join(str(f) for f in desktop['fps'])} (per scenario)")
    if "scenarios" in desktop:
        lines.append(f"    {desktop['scenarios']}")
    if "boundary" in desktop:
        lines.append(f"    Platform boundary: {desktop['boundary']}")

    if esp32:
        lines.append("  ESP32:")
        if "image_bytes" in esp32:
            lines.append(f"    Image: {esp32['image_bytes']:,} bytes ({esp32.get('partition_free_pct', '?')}% partition free)")
        if "flash_kb" in esp32:
            lines.append(f"    Flash (code+data): {esp32['flash_kb']} KB")
        if "dram_used" in esp32:
            total = esp32["dram_used"] + esp32.get("dram_free", 0)
            lines.append(f"    DRAM: {esp32['dram_used']:,} / {total:,} ({esp32.get('dram_free', 0):,} free)")
        if "fps" in esp32:
            lines.append(f"    FPS: {esp32['fps']}  heap free: {esp32.get('heap_free', '?')}")

    lines.append("  Code:")
    lines.append(f"    {code['src_files']} source files ({code['src_lines']} lines)")
    lines.append(f"    {code['test_files']} test files ({code['test_lines']} lines)")
    lines.append(f"    {code['specs']} specs, {code['scenarios']} scenarios")
    lines.append(f"    Lizard: {code['lizard_warnings']} warnings")
    for w in code.get("lizard_details", []):
        lines.append(f"      {w}")

    return "\n".join(lines)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--commit", action="store_true",
                        help="Output in commit message format (one-liner + details)")
    args = parser.parse_args()

    desktop = collect_desktop()
    esp32 = collect_esp32()
    code = collect_code()

    if args.commit:
        print(format_oneliner(desktop, esp32, code))
        print()
        print(format_full(desktop, esp32, code))
    else:
        # Full interactive report
        print("=" * 50)
        print("  mmv3 KPI Report")
        print("=" * 50)
        print()
        print(format_oneliner(desktop, esp32, code))
        print()
        print(format_full(desktop, esp32, code))
        print()
        print("=" * 50)

if __name__ == "__main__":
    main()
