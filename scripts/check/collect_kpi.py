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
import re
import subprocess
import sys
import os
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_DIR = ROOT / "build"
ESP32_DIR = ROOT / "esp32"

# Hard performance floor for the ESP32 render loop, expressed as an FPS ×
# light-count throughput so it scales to any grid size. Anchored at the 128×128
# reference: 18 FPS × 16384 lights. A smaller grid must run proportionally
# faster (e.g. 64×64 = 4096 lights → 72 FPS floor).
#
# The device reports tick *time* (µs), not FPS — FPS is only ever derived by
# integer division, which loses precision. So the gate compares the measured
# tick_us directly against a per-grid *max tick time*:
#   max_tick_us = lights × 1e6 / MIN_ESP32_FPS_LED_PRODUCT
# Enforced in --commit mode so a regression fails pre-commit.
MIN_ESP32_FPS_LED_PRODUCT = 18 * 16384  # 294912

sys.path.insert(0, str(ROOT / "scripts" / "build"))

def run(cmd, cwd=None, timeout=30):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, timeout=timeout)
    return r.stdout + r.stderr, r.returncode

# ---------------------------------------------------------------------------
# Collectors — return dicts of KPI data
# ---------------------------------------------------------------------------

def collect_desktop():
    kpi = {}

    projectMM = BUILD_DIR / "projectMM"
    if projectMM.exists():
        kpi["binary_kb"] = projectMM.stat().st_size // 1024

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
        tick_values = []
        buffer_lights = []
        for line in out.splitlines():
            if "tick:" in line:
                # Format: "tick: 108us (FPS: 9259)"
                m = re.search(r'tick:\s*(\d+)us', line)
                if m:
                    tick_values.append(int(m.group(1)))
            if "Buffer:" in line and "lights" in line:
                parts = line.strip().split()
                for i, p in enumerate(parts):
                    if p == "Buffer:" and i + 1 < len(parts):
                        try:
                            buffer_lights.append(int(parts[i + 1]))
                        except ValueError:
                            pass
            if "scenario(s)" in line:
                kpi["scenarios"] = line.strip()
        if tick_values:
            kpi["tick_us"] = tick_values
            kpi["fps"] = [1000000 // t if t > 0 else 0 for t in tick_values]
        if buffer_lights:
            kpi["lights"] = max(buffer_lights)

    check = ROOT / "scripts" / "check" / "check_platform_boundary.py"
    if check.exists():
        _, rc = run([sys.executable, str(check)])
        kpi["boundary"] = "PASS" if rc == 0 else "FAIL"

    specs = ROOT / "scripts" / "check" / "check_specs.py"
    if specs.exists():
        out, rc = run([sys.executable, str(specs)])
        kpi["specs_check"] = "PASS" if rc == 0 else "FAIL"
        for line in out.splitlines():
            if "Spec check:" in line:
                kpi["specs_summary"] = line.strip()

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
                    m = re.search(r'\((\d+)%\)\s+free', line)
                    if m:
                        kpi["partition_free_pct"] = int(m.group(1))
                if "Total image size" in line:
                    m = re.search(r'(\d+)\s+bytes', line)
                    if m:
                        kpi["image_bytes"] = int(m.group(1))
            if flash_total:
                kpi["flash_kb"] = flash_total // 1024
    except Exception:
        pass

    # Read tick/FPS from monitor.log. Do a live capture against the canonical
    # port (scripts/moondeck.json) when the log is stale OR when it exists but
    # has no parseable tick line — instead of silently skipping ESP32 KPI.
    log = ESP32_DIR / "monitor.log"
    stale = (not log.exists()) or (time.time() - log.stat().st_mtime) > 300

    # Only extract from a log we trust: a non-stale file, or one a live capture
    # just refreshed. If the file is stale and the capture fails (port absent,
    # garbled output), do NOT fall back to the old file — a stale tick reported
    # as fresh is worse than no ESP32 KPI at all.
    if stale:
        if _live_capture(log):
            _extract_esp32_tick(log, kpi)
    else:
        if not _extract_esp32_tick(log, kpi):
            # File is fresh but unparseable (no tick line) — one capture retry.
            if _live_capture(log):
                _extract_esp32_tick(log, kpi)

    return kpi

def _extract_esp32_tick(log, kpi):
    if not log.exists():
        return False
    for line in reversed(log.read_text().splitlines()):
        if "tick:" not in line:
            continue
        # Format: "tick: 108us (FPS: 9259)  free: 215180  maxBlock: 63488"
        m = re.search(r'tick:\s*(\d+)us', line)
        if m:
            tick_us = int(m.group(1))
            kpi["tick_us"] = tick_us
            kpi["fps"] = 1000000 // tick_us if tick_us > 0 else 0
        m = re.search(r'free:\s*(\d+)', line)
        if m:
            kpi["heap_free"] = int(m.group(1))
        return "tick_us" in kpi
    return False

def _live_capture(log, seconds=15):
    """Capture ESP32 serial output to monitor.log for ~seconds. Returns True on success."""
    import json
    cfg = ROOT / "scripts" / "moondeck.json"
    if not cfg.exists():
        return False
    try:
        port = json.loads(cfg.read_text()).get("port")
    except Exception:
        return False
    if not port or not Path(port).exists():
        print(f"  ESP32 KPI: configured port {port} not present, skipping live capture")
        return False
    try:
        import serial
    except ImportError:
        return False
    print(f"  ESP32 KPI: capturing {seconds}s from {port}...")
    try:
        ser = serial.Serial(port, 115200, timeout=1)
    except Exception as e:
        print(f"  ESP32 KPI: cannot open {port}: {e}")
        return False
    end = time.time() + seconds
    try:
        with open(log, "w") as f:
            while time.time() < end:
                line = ser.readline().decode("utf-8", errors="replace").rstrip("\r\n")
                if line:
                    f.write(line + "\n")
                    f.flush()
    finally:
        ser.close()
    return True

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
    if "lights" in desktop:
        parts.append(f"{desktop['lights']}lights")
    if "binary_kb" in desktop:
        parts.append(f"PC:{desktop['binary_kb']}KB")
    if "tick_us" in desktop:
        ticks = '/'.join(str(t) for t in desktop['tick_us'])
        fps = '/'.join(str(f) for f in desktop.get('fps', []))
        parts.append(f"tick:{ticks}us(FPS:{fps})")
    if "flash_kb" in esp32:
        parts.append(f"ESP32:{esp32['flash_kb']}KB")
    if "tick_us" in esp32:
        parts.append(f"tick:{esp32['tick_us']}us(FPS:{esp32.get('fps', '?')})")
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
    if "lights" in desktop:
        lines.append(f"    Lights: {desktop['lights']:,}")
    if "binary_kb" in desktop:
        lines.append(f"    Binary: {desktop['binary_kb']} KB")
    if "tests" in desktop:
        lines.append(f"    {desktop['tests']}")
    if "tick_us" in desktop:
        ticks = ', '.join(f"{t}us" for t in desktop['tick_us'])
        fps = ', '.join(str(f) for f in desktop.get('fps', []))
        lines.append(f"    tick: {ticks} (FPS: {fps}) (per scenario)")
    if "scenarios" in desktop:
        lines.append(f"    {desktop['scenarios']}")
    if "boundary" in desktop:
        lines.append(f"    Platform boundary: {desktop['boundary']}")
    if "specs_check" in desktop:
        lines.append(f"    Specs: {desktop.get('specs_summary', desktop['specs_check'])}")

    if esp32:
        lines.append("  ESP32:")
        if "image_bytes" in esp32:
            lines.append(f"    Image: {esp32['image_bytes']:,} bytes ({esp32.get('partition_free_pct', '?')}% partition free)")
        if "flash_kb" in esp32:
            lines.append(f"    Flash (code+data): {esp32['flash_kb']} KB")
        if "dram_used" in esp32:
            total = esp32["dram_used"] + esp32.get("dram_free", 0)
            lines.append(f"    DRAM: {esp32['dram_used']:,} / {total:,} ({esp32.get('dram_free', 0):,} free)")
        if "tick_us" in esp32:
            lines.append(f"    tick: {esp32['tick_us']}us (FPS: {esp32.get('fps', '?')})  heap free: {esp32.get('heap_free', '?')}")

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
        print("(see KPI Details at bottom)")
        print()
        print(format_full(desktop, esp32, code))
    else:
        # Full interactive report
        print("=" * 50)
        print("  projectMM KPI Report")
        print("=" * 50)
        print()
        print(format_oneliner(desktop, esp32, code))
        print()
        print(format_full(desktop, esp32, code))
        print()
        print("=" * 50)

    # Hard throughput gate: if a live ESP32 tick was captured, it must clear the
    # floor. The device reports tick *time* (µs), so compare that directly — no
    # lossy FPS division. The per-grid budget scales with light count:
    #   max_tick_us = lights × 1e6 / MIN_ESP32_FPS_LED_PRODUCT
    # so a smaller grid gets proportionally less time (18 FPS at 128×128).
    # An absent ESP32 reading is not a failure — the caller decides whether a
    # missing measurement is acceptable (see CLAUDE.md pre-commit step 8).
    # Only --commit mode aborts on a breach; a plain interactive report just
    # warns, so viewing KPIs on an unlucky slow sample does not exit non-zero.
    esp32_tick = esp32.get("tick_us")
    lights = desktop.get("lights")
    if esp32_tick is not None and lights:
        max_tick = round(lights * 1_000_000 / MIN_ESP32_FPS_LED_PRODUCT)
        if esp32_tick > max_tick:
            print()
            label = "FAIL" if args.commit else "WARN"
            print(f"{label}: ESP32 render tick {esp32_tick}us exceeds the {max_tick}us "
                  f"budget for {lights} lights (18 FPS at the 128×128 / 16384-light "
                  f"reference).")
            if args.commit:
                sys.exit(1)

if __name__ == "__main__":
    main()
