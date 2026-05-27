#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["playwright", "requests"]
# ///
"""Capture UI screenshots and preview GIFs of every projectMM module.

Connects to a running projectMM server, adds each module to a minimal
pipeline via REST, screenshots its card in the web UI, then removes it.
For effects and modifiers also captures a 3-second GIF of the preview canvas.

Also captures MoonDeck tab screenshots and the web installer page, and
inserts them into the appropriate docs files.

Saves to:
  docs/assets/screenshots/<TypeName>.png     — module card screenshot
  docs/assets/screenshots/<TypeName>.gif     — preview animation (effects/modifiers)
  docs/assets/screenshots/ui_overview.png    — projectMM full-page screenshot
  docs/assets/screenshots/moondeck_pc.png    — MoonDeck PC tab
  docs/assets/screenshots/moondeck_esp32.png — MoonDeck ESP32 tab
  docs/assets/screenshots/moondeck_live.png  — MoonDeck Live tab
  docs/assets/screenshots/installer.png      — web installer page

Usage:
    uv run scripts/docs/screenshot_modules.py [--host localhost:8080] [--force]

Prerequisites:
    uv run playwright install chromium   # one-time
    ffmpeg must be on PATH (brew install ffmpeg)
    For installer: uv run scripts/run/preview_installer.py  (serves on port 8000)
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import requests
from playwright.sync_api import sync_playwright, Page

ROOT = Path(__file__).resolve().parent.parent.parent
OUT_DIR = ROOT / "docs" / "assets" / "screenshots"

# ---------------------------------------------------------------------------
# Module catalogue
# Each entry: (type_name, parent_type, extra_props, capture_gif)
#   parent_type: "Layer" = child of Layer, "Drivers" = child of Drivers,
#                "Layouts" = child of Layouts
#   capture_gif: True = also record a preview GIF
# ---------------------------------------------------------------------------
MODULES = [
    # Layouts
    ("GridLayout",          "Layouts",  {}, False),
    # Effects
    ("RainbowEffect",       "Layer",    {}, True),
    ("NoiseEffect",         "Layer",    {}, True),
    ("FireEffect",          "Layer",    {}, True),
    ("PlasmaEffect",        "Layer",    {}, True),
    ("PlasmaPaletteEffect", "Layer",    {}, True),
    ("LinesEffect",         "Layer",    {}, True),
    ("MetaballsEffect",     "Layer",    {}, True),
    ("ParticlesEffect",     "Layer",    {}, True),
    ("GlowParticlesEffect", "Layer",    {}, True),
    ("CheckerboardEffect",  "Layer",    {}, True),
    ("RipplesEffect",       "Layer",    {}, True),
    ("LavaLampEffect",      "Layer",    {}, True),
    ("SpiralEffect",        "Layer",    {}, True),
    # Modifiers — added as children of Layer
    ("MirrorModifier",      "Layer",    {}, True),
    # Drivers
    ("ArtNetSendDriver",    "Drivers",  {}, False),
    ("PreviewDriver",       "Drivers",  {}, False),
]

# Container types that exist in the pipeline but are not added via REST
CONTAINERS = ["Layouts", "Layers", "Drivers"]

# Core modules: always present in the pipeline, never added/deleted via REST.
# Each entry: type_name — the module's type string as reported by /api/types.
# The screenshot navigates to the module by its live name from /api/state.
CORE_MODULES = [
    "FilesystemModule",
    "SystemModule",
    "FirmwareUpdateModule",
    "NetworkModule",
    "HttpServerModule",
    "ImprovProvisioningModule",  # ESP32-only — skipped if not in state
]

# ---------------------------------------------------------------------------
# Extra shots: MoonDeck tabs + web installer
# Each entry: (filename, url, wait_selector, doc_files, anchor_text)
#   filename:      saved as docs/assets/screenshots/<filename>
#   url:           full URL to load
#   wait_selector: CSS selector to wait for before screenshotting (or "")
#   doc_files:     list of repo-relative paths to insert image into
#   anchor_text:   text of the line/heading after which to insert (or "")
# ---------------------------------------------------------------------------
MOONDECK_URL   = "http://localhost:8420"
INSTALLER_URL  = "http://localhost:8000"

EXTRA_SHOTS = [
    (
        "moondeck_pc.png",
        f"{MOONDECK_URL}/?tab=pc",
        ".tab-content.active",
        ["README.md", "scripts/MoonDeck.md"],
        "## PC Tab",
    ),
    (
        "moondeck_esp32.png",
        f"{MOONDECK_URL}/?tab=esp32",
        ".tab-content.active",
        ["scripts/MoonDeck.md"],
        "## ESP32 Tab",
    ),
    (
        "moondeck_live.png",
        f"{MOONDECK_URL}/?tab=live",
        ".tab-content.active",
        ["scripts/MoonDeck.md"],
        "## Live Tab",
    ),
    (
        "installer.png",
        INSTALLER_URL,
        "body",
        ["README.md", "scripts/MoonDeck.md"],
        "### preview_installer",
    ),
]

# GIF capture settings
GIF_DURATION_S  = 3      # seconds to record
GIF_FPS         = 10     # frames per second captured
GIF_OUTPUT_FPS  = 10     # frames per second in output GIF

# ---------------------------------------------------------------------------
# REST helpers
# ---------------------------------------------------------------------------

def _get(url: str, **kwargs) -> requests.Response:
    """GET with retries — the embedded server resets connections under load."""
    for attempt in range(4):
        try:
            return requests.get(url, **kwargs)
        except requests.exceptions.ConnectionError:
            if attempt == 3:
                raise
            time.sleep(0.5 * (attempt + 1))
    raise RuntimeError("unreachable")


def _post(url: str, **kwargs) -> requests.Response:
    """POST with retries."""
    for attempt in range(4):
        try:
            return requests.post(url, **kwargs)
        except requests.exceptions.ConnectionError:
            if attempt == 3:
                raise
            time.sleep(0.5 * (attempt + 1))
    raise RuntimeError("unreachable")


def _delete(url: str, **kwargs) -> requests.Response:
    """DELETE with retries."""
    for attempt in range(4):
        try:
            return requests.delete(url, **kwargs)
        except requests.exceptions.ConnectionError:
            if attempt == 3:
                raise
            time.sleep(0.5 * (attempt + 1))
    raise RuntimeError("unreachable")


def _collect_names(modules: list) -> set[str]:
    """Recursively collect all module names from a state tree."""
    names: set[str] = set()
    for m in modules:
        names.add(m.get("name", ""))
        names |= _collect_names(m.get("children", []))
    return names


def add_module(host: str, id_: str, type_: str, parent_id: str | None,
               props: dict) -> str | None:
    """Add a module and return the actual name the server assigned.

    Snapshots state before and after the POST; the new module is whichever
    name appears after that wasn't there before and has the right type.
    Handles server-side name truncation and deduplication transparently.
    """
    before = _get(f"http://{host}/api/state", timeout=5)
    names_before = _collect_names(before.json().get("modules", [])) if before.ok else set()

    body: dict = {"id": id_, "type": type_}
    if parent_id:
        body["parent_id"] = parent_id
    if props:
        body["props"] = props
    r = _post(f"http://{host}/api/modules", json=body, timeout=5)
    if not r.ok:
        return None

    time.sleep(0.15)
    sr = _get(f"http://{host}/api/state", timeout=5)
    if not sr.ok:
        return id_[:16]

    def find_new(modules: list) -> str | None:
        for m in modules:
            n = m.get("name", "")
            if n not in names_before and m.get("type") == type_:
                return n
            found = find_new(m.get("children", []))
            if found:
                return found
        return None

    return find_new(sr.json().get("modules", [])) or id_[:16]


def delete_module(host: str, id_: str) -> bool:
    r = _delete(f"http://{host}/api/modules/{id_}", timeout=5)
    return r.ok


def get_types(host: str) -> set[str]:
    r = _get(f"http://{host}/api/types", timeout=5)
    if not r.ok:
        return set()
    data = r.json()
    types = data.get("types", data) if isinstance(data, dict) else data
    return {t["name"] if isinstance(t, dict) else t for t in types}


# ---------------------------------------------------------------------------
# Pipeline discovery
# ---------------------------------------------------------------------------

def find_parent_ids(host: str) -> tuple[dict[str, str], dict[str, str]]:
    """Return (parents, nav_roots) for the existing pipeline containers.

    parents maps role → module name to use as parent_id when adding a child.
    nav_roots maps role → top-level nav sidebar name (what to click).
    Layer is nested inside Layers, so its nav root is "Layers" not "Layer".
    """
    r = _get(f"http://{host}/api/state", timeout=5)
    if not r.ok:
        return {}, {}

    parents: dict[str, str] = {}
    nav_roots: dict[str, str] = {}

    def walk(modules: list, nav_root: str | None = None) -> None:
        for m in modules:
            t = m.get("type", "")
            n = m.get("name", "")
            current_nav = nav_root if nav_root else n
            if t == "Layer" and "Layer" not in parents:
                parents["Layer"] = n
                nav_roots["Layer"] = current_nav
            elif t == "Drivers" and "Drivers" not in parents:
                parents["Drivers"] = n
                nav_roots["Drivers"] = current_nav
            elif t == "Layouts" and "Layouts" not in parents:
                parents["Layouts"] = n
                nav_roots["Layouts"] = current_nav
            if m.get("children"):
                walk(m["children"], current_nav)

    walk(r.json().get("modules", []))
    return parents, nav_roots


def find_container_nav_names(host: str) -> dict[str, str]:
    """Return a map of container type → module name for top-level containers.

    Used to screenshot Layouts, Layers, Drivers cards directly.
    """
    r = _get(f"http://{host}/api/state", timeout=5)
    if not r.ok:
        return {}
    result: dict[str, str] = {}
    for m in r.json().get("modules", []):
        t = m.get("type", "")
        if t in CONTAINERS and t not in result:
            result[t] = m.get("name", "")
    return result


def find_core_module_names(host: str) -> dict[str, str]:
    """Return a map of type → live module name for core (always-present) modules."""
    r = _get(f"http://{host}/api/state", timeout=5)
    if not r.ok:
        return {}
    result: dict[str, str] = {}
    for m in r.json().get("modules", []):
        t = m.get("type", "")
        if t in CORE_MODULES:
            result[t] = m.get("name", "")
    return result


# ---------------------------------------------------------------------------
# Screenshot helpers
# ---------------------------------------------------------------------------

def _load_page(page: Page, host: str) -> None:
    """Reload the UI and wait for it to settle.

    Retries on socket errors — the embedded server can momentarily drop
    connections when hammered with rapid reloads.
    """
    url = f"http://{host}/"
    for attempt in range(4):
        try:
            page.goto(url, wait_until="networkidle", timeout=15000)
            page.wait_for_timeout(800)
            return
        except Exception:
            if attempt == 3:
                raise
            page.wait_for_timeout(1000 * (attempt + 1))


def _click_nav(page: Page, nav_root: str) -> None:
    nav_btn = page.query_selector(f'button.nav-item[data-module="{nav_root}"]')
    if nav_btn:
        nav_btn.click()
        page.wait_for_timeout(500)


def _screenshot_card(page: Page, module_id: str, out_path: Path) -> bool:
    """Screenshot the card for module_id. Page must already show the right nav."""
    card_sel = f'.card[data-module="{module_id}"]'
    try:
        page.wait_for_selector(card_sel, timeout=5000)
    except Exception:
        return False
    card = page.query_selector(card_sel)
    if not card:
        return False
    card.scroll_into_view_if_needed()
    page.wait_for_timeout(300)
    box = card.bounding_box()
    if not box:
        return False
    page.screenshot(path=str(out_path), clip=box)
    return True


def screenshot_module(page: Page, host: str, module_id: str,
                      nav_root: str, out_path: Path) -> bool:
    """Reload the UI, click nav, screenshot the module card."""
    _load_page(page, host)
    _click_nav(page, nav_root)
    return _screenshot_card(page, module_id, out_path)


def screenshot_container(page: Page, host: str, container_name: str,
                         out_path: Path) -> bool:
    """Screenshot a top-level container card (Layouts, Layers, Drivers)."""
    _load_page(page, host)
    _click_nav(page, container_name)
    return _screenshot_card(page, container_name, out_path)


def screenshot_fullpage(page: Page, host: str, out_path: Path,
                        nav_root: str = "") -> bool:
    """Capture a full-page screenshot of the UI."""
    _load_page(page, host)
    if nav_root:
        _click_nav(page, nav_root)
    page.screenshot(path=str(out_path), full_page=False)
    return True


def screenshot_url(page: Page, url: str, wait_selector: str,
                   out_path: Path) -> bool:
    """Load an arbitrary URL and screenshot it."""
    for attempt in range(4):
        try:
            page.goto(url, wait_until="networkidle", timeout=15000)
            break
        except Exception:
            if attempt == 3:
                return False
            page.wait_for_timeout(1000 * (attempt + 1))
    if wait_selector:
        try:
            page.wait_for_selector(wait_selector, timeout=5000)
        except Exception:
            pass
    page.wait_for_timeout(600)
    page.screenshot(path=str(out_path), full_page=False)
    return True



# ---------------------------------------------------------------------------
# GIF capture
# ---------------------------------------------------------------------------

def capture_preview_gif(page: Page, host: str, module_id: str,
                        nav_root: str, out_path: Path) -> bool:
    """Capture a GIF of the WebGL preview canvas while module_id is active.

    Reloads the page, navigates to the module, then grabs canvas frames via
    page.screenshot(clip=...) at GIF_FPS for GIF_DURATION_S seconds.
    toDataURL() returns black for WebGL in headless mode; page.screenshot()
    uses the compositor and captures the rendered output correctly.
    """
    if shutil.which("ffmpeg") is None:
        print("(ffmpeg not found, skipping GIF)", end=" ", flush=True)
        return True  # not a hard failure

    _load_page(page, host)
    _click_nav(page, nav_root)

    # Wait for the card to be visible (confirms module is rendering).
    card_sel = f'.card[data-module="{module_id}"]'
    try:
        page.wait_for_selector(card_sel, timeout=5000)
    except Exception:
        return False

    # Locate the preview canvas bounding box.
    canvas = page.query_selector("#preview")
    if not canvas:
        return False
    canvas_box = canvas.bounding_box()
    if not canvas_box:
        return False

    # Wait for the effect to warm up before recording.
    page.wait_for_timeout(800)

    n_frames = GIF_DURATION_S * GIF_FPS
    interval_ms = 1000 // GIF_FPS

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        for i in range(n_frames):
            frame_path = tmp_path / f"frame{i:04d}.png"
            page.screenshot(path=str(frame_path), clip=canvas_box)
            page.wait_for_timeout(interval_ms)

        frames = sorted(tmp_path.glob("frame*.png"))
        if not frames:
            return False

        result = subprocess.run(
            [
                "ffmpeg", "-y",
                "-framerate", str(GIF_FPS),
                "-i", str(tmp_path / "frame%04d.png"),
                "-vf", f"fps={GIF_OUTPUT_FPS},scale=320:-1:flags=lanczos,"
                       "split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse",
                str(out_path),
            ],
            capture_output=True,
        )
        return result.returncode == 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="localhost:8080",
                        help="projectMM server host:port (default: localhost:8080)")
    parser.add_argument("--force", action="store_true",
                        help="Re-capture even if screenshot already exists")
    parser.add_argument("--gif", action="store_true",
                        help="Also capture animated GIF previews for effects/modifiers")
    parser.add_argument("--filter", default="",
                        help="Only capture modules whose type name contains this substring (case-insensitive)")
    args = parser.parse_args()

    try:
        _get(f"http://{args.host}/api/state", timeout=3)
    except Exception as e:
        print(f"Cannot reach projectMM at {args.host}: {e}")
        print("Start the server first: uv run scripts/moondeck.py  (then build+run from PC tab)")
        return 1

    server_types = get_types(args.host)
    if server_types:
        print(f"Server reports {len(server_types)} module types.")

    # Known name prefixes for orphan sweep (first 16 chars of each type name).
    known_prefixes = {t[:16] for t, _, _, _ in MODULES}

    # Sweep orphans from a previous aborted run.
    try:
        sr = _get(f"http://{args.host}/api/state", timeout=5)
        if sr.ok:
            def _sweep_orphans(modules: list) -> None:
                for m in modules:
                    n = m.get("name", "")
                    if any(n.startswith(p) for p in known_prefixes):
                        delete_module(args.host, n)
                    _sweep_orphans(m.get("children", []))
            _sweep_orphans(sr.json().get("modules", []))
    except Exception:
        pass

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    captured, gif_captured, skipped, failed = [], [], [], []

    print("Discovering pipeline containers …")
    parents, nav_roots = find_parent_ids(args.host)
    missing = [r for r in ("Layer", "Drivers", "Layouts") if r not in parents]
    if missing:
        print(f"Pipeline containers not found: {missing}")
        print("Build and run projectMM first (PC tab → Build → Run).")
        return 1
    print(f"  Layer={parents['Layer']!r} (nav={nav_roots['Layer']!r})")
    print(f"  Drivers={parents['Drivers']!r} (nav={nav_roots['Drivers']!r})")
    print(f"  Layouts={parents['Layouts']!r} (nav={nav_roots['Layouts']!r})")

    container_names = find_container_nav_names(args.host)
    core_names = find_core_module_names(args.host)

    filt = args.filter.lower()

    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        page = browser.new_page(viewport={"width": 1280, "height": 900})

        added_ids: list[str] = []

        try:
            # --- Full-page UI overview screenshot ---
            overview_path = OUT_DIR / "ui_overview.png"
            if (not overview_path.exists() or args.force) and (not filt or filt in "ui_overview"):
                print("  ui_overview …", end=" ", flush=True)
                # Use Layers nav to show a populated view
                nav = nav_roots.get("Layer", "")
                ok = screenshot_fullpage(page, args.host, overview_path, nav_root=nav)
                print(f"saved → {overview_path.relative_to(ROOT)}" if ok else "failed")
                if ok:
                    captured.append("ui_overview")
                else:
                    failed.append(("ui_overview", "screenshot failed"))
            else:
                print("  skip ui_overview (already captured)")
                skipped.append(("ui_overview", "already exists"))

            # --- Extra shots: MoonDeck tabs + installer ---
            for filename, url, wait_sel, _doc_files, _anchor in EXTRA_SHOTS:
                if filt and filt not in filename.lower():
                    continue
                out_path = OUT_DIR / filename
                if out_path.exists() and not args.force:
                    print(f"  skip {filename} (already captured)")
                    skipped.append((filename, "already exists"))
                    continue
                print(f"  {filename} …", end=" ", flush=True)
                ok = screenshot_url(page, url, wait_sel, out_path)
                if ok:
                    print(f"saved → {out_path.relative_to(ROOT)}")
                    captured.append(filename)
                else:
                    print("failed (is the server running?)")
                    failed.append((filename, "screenshot failed"))

            # --- Container cards (Layouts, Layers, Drivers) ---
            for container_type in CONTAINERS:
                if filt and filt not in container_type.lower():
                    continue
                cname = container_names.get(container_type, "")
                if not cname:
                    continue
                out_path = OUT_DIR / f"{container_type}.png"
                if out_path.exists() and not args.force:
                    print(f"  skip {container_type} (already captured)")
                    skipped.append((container_type, "already exists"))
                    continue
                print(f"  {container_type} …", end=" ", flush=True)
                ok = screenshot_container(page, args.host, cname, out_path)
                print(f"saved → {out_path.relative_to(ROOT)}" if ok else "failed")
                if ok:
                    captured.append(container_type)
                else:
                    failed.append((container_type, "screenshot failed"))

            # --- Core module cards (always present, never added/deleted) ---
            for type_name in CORE_MODULES:
                if filt and filt not in type_name.lower():
                    continue
                cname = core_names.get(type_name, "")
                if not cname:
                    print(f"  skip {type_name} (not in state — ESP32-only?)")
                    skipped.append((type_name, "not in state"))
                    continue
                out_path = OUT_DIR / f"{type_name}.png"
                if out_path.exists() and not args.force:
                    print(f"  skip {type_name} (already captured)")
                    skipped.append((type_name, "already exists"))
                    continue
                print(f"  {type_name} …", end=" ", flush=True)
                ok = screenshot_container(page, args.host, cname, out_path)
                print(f"saved → {out_path.relative_to(ROOT)}" if ok else "failed")
                if ok:
                    captured.append(type_name)
                else:
                    failed.append((type_name, "screenshot failed"))

            # --- Individual module cards ---
            for type_name, parent_type, extra_props, want_gif in MODULES:
                if filt and filt not in type_name.lower():
                    continue
                out_path = OUT_DIR / f"{type_name}.png"
                gif_path = OUT_DIR / f"{type_name}.gif"
                need_png = not out_path.exists() or args.force
                need_gif = want_gif and args.gif and (not gif_path.exists() or args.force)

                if not need_png and not need_gif:
                    print(f"  skip {type_name} (already captured)")
                    skipped.append((type_name, "already exists"))
                    continue

                parent_id = parents.get(parent_type)
                nav_root = nav_roots.get(parent_type, "")
                req_id = type_name[:16]
                print(f"  {type_name} …", end=" ", flush=True)

                actual_name = add_module(args.host, req_id, type_name,
                                         parent_id, extra_props)
                if not actual_name:
                    print("add failed")
                    failed.append((type_name, "add_module failed"))
                    continue

                added_ids.append(actual_name)

                if need_png:
                    ok = screenshot_module(page, args.host, actual_name,
                                           nav_root, out_path)
                    if ok:
                        print(f"png ", end="", flush=True)
                        captured.append(type_name)
                    else:
                        print("png-failed ", end="", flush=True)
                        failed.append((type_name, "screenshot failed"))

                if need_gif:
                    ok = capture_preview_gif(page, args.host, actual_name,
                                             nav_root, gif_path)
                    if ok:
                        print(f"gif ", end="", flush=True)
                        gif_captured.append(type_name)
                    else:
                        print("gif-failed ", end="", flush=True)
                        failed.append((type_name, "gif failed"))

                print(f"→ {OUT_DIR.relative_to(ROOT)}/")
                delete_module(args.host, actual_name)
                added_ids.remove(actual_name)
                time.sleep(0.5)

        finally:
            for mid in added_ids:
                delete_module(args.host, mid)
            try:
                sr = _get(f"http://{args.host}/api/state", timeout=5)
                if sr.ok:
                    def _sweep(modules: list) -> None:
                        for m in modules:
                            n = m.get("name", "")
                            if any(n.startswith(p) for p in known_prefixes):
                                delete_module(args.host, n)
                            _sweep(m.get("children", []))
                    _sweep(sr.json().get("modules", []))
            except Exception:
                pass
            browser.close()

    print(f"\n{'─'*50}")
    print(f"Captured : {len(captured)} PNGs, {len(gif_captured)} GIFs")
    print(f"Skipped  : {len(skipped)}")
    print(f"Failed   : {len(failed)}")
    if failed:
        print("\nFailed:")
        for name, reason in failed:
            print(f"  {name}: {reason}")
    if captured:
        print("\nNext steps:")
        print("  Add module screenshots: uv run scripts/docs/update_module_docs.py")
        if "ui_overview" in captured:
            print("  Add UI overview to docs/architecture.md # Web UI section:")
            print("  ![UI overview](assets/screenshots/ui_overview.png)")

    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
