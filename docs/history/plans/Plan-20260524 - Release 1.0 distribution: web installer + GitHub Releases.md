# Plan-17 — Release 1.0 distribution: web installer + GitHub Releases

> **Post-implementation note.** Two divergences from the original draft:
> 1. Of the desktop matrix this plan called for, only the macOS arm64 binary ships in 1.0. The Windows x64 build failed in CI on the first source file because `src/platform/desktop/platform_desktop.cpp` uses POSIX socket headers (`sys/socket.h`, `sendmsg`, `fcntl`, …) that have no MSVC equivalent. The `build-windows` job and the `dist/projectMM-*.zip` upload are removed from `release.yml` until the Windows platform-layer port lands; see `docs/plan.md` "Windows desktop port".
> 2. The original draft's `esp_idf_version: v5.4` in `release.yml` fails to compile `platform_esp32.cpp` — the v5.x EMAC config has `emac_rmii_clock_gpio_t clock_gpio` (strong enum), v6 has `int clock_gpio`. Per the plan's risk-1 fallback, CI is pinned to the same v6.1-dev line the local project uses (`esp_idf_version: v6.1-dev` — the rolling Docker tag on `espressif/idf`). The plan's v5.4 references below are historical.
>
> Everything else in this plan ships as described: 4 ESP32 board variants, macOS arm64 desktop, install page on Pages, RC tag dry-run flow.

## Context

projectMM v3 ships today as "clone the repo and run MoonDeck." That works for developers but blocks the end user the README promises: "plug in your ESP32, open a browser, see lights." This plan delivers the missing pieces — pre-built binaries on GitHub Releases for 4 ESP32 board variants + macOS + Windows, an ESP Web Tools installer page on GitHub Pages, and a tag-triggered CI pipeline that produces and publishes everything.

The shape is anchored on projectMM-v1's release flow (matrix CI → GitHub Releases) and on WLED's installer pattern (ESP Web Tools + per-variant manifests). What v3 picks up vs v1: ESP Web Tools (v1 didn't have it), board-selector dropdown (v1 didn't have multiple ESP32 binaries beyond dev + S3). What v3 defers vs v1 to 2.0: OTA, nightly channel, Linux desktop.

Closed scope (decided before this plan, not revisited here):

- **4 ESP32 board variants** keyed by chip + feature flags: `esp32` (classic, WiFi only), `esp32-eth` (classic, Ethernet only — WiFi compiled out, smaller image), `esp32-eth-wifi` (classic, Ethernet + WiFi both available), `esp32s3-n16r8` (ESP32-S3 N16R8, WiFi only). Eth variants bake in Olimex ESP32-Gateway pin defaults (LAN8720 @ MDIO 0, PHY RST GPIO 5). Boards with the same PHY but different pins (WT32-ETH01: reset on GPIO 16) need a local rebuild for 1.0; runtime PHY/pin selection is a 2.0 item. P4 → 2.0.
- **Distribution = GitHub Releases + ESP Web Tools page on GitHub Pages.** 4 manifests, manual board dropdown. No Improv WiFi (WiFi creds via the device's SoftAP fallback).
- **CI = single `release.yml`** triggered by `git tag v*` and `workflow_dispatch`. While iterating pre-1.0, also triggers on push to `main` / `next-iteration` so build breakage is caught before tagging; the release + Pages-deploy jobs stay gated on a tag ref. Remove the branches trigger once the pipeline is proven. No PR-CI, no nightly.
- **Desktop binaries = macOS arm64 + Windows x64.** No Linux, no macOS x64.
- **No OTA in 1.0.** Users re-flash via Web Tools when a new release lands.
- **Tag matches `library.json` version.** CI fails fast on drift — maintainer bumps version, commits, tags as one act.
- **MoonDeck developer flow unchanged.** `git clone` + `uv run scripts/moondeck.py` stays the dev bootstrap; the slow part is the prerequisite chain (uv, ESP-IDF), not the clone.

## Architecture

```text
git tag v1.0.0
  └─> .github/workflows/release.yml
        ├─ verify-version            (tag == library.json["version"]?)
        ├─ build-esp32 (matrix: esp32, esp32-eth, esp32-eth-wifi, esp32s3-n16r8)
        │     └─ scripts/build/build_esp32.py --board <key>
        ├─ build-macos               (macos-14, cmake Release, tar.gz)
        ├─ build-windows             (windows-latest, cmake/MSVC Release, zip)
        └─ release
              ├─ assemble per-board firmware bundles
              ├─ generate manifest-<board>.json
              ├─ gh release upload
              └─ deploy docs/install/ → GitHub Pages

docs/install/index.html
  ├─ board dropdown → manifest-<board>.json
  └─ <esp-web-install-button> flashes selected board
```

## Implementation steps

Estimated total **11–13 h**. Bulk of risk lives in steps 1, 5, 10 — start there, iterate on RC tags until step 10 passes cleanly, then bump to `1.0.0`.

### Step 1 — `build_esp32.py --board` (2–3 h)

Wire a `--board` flag that selects sdkconfig fragments, sets the chip target, and implies the WiFi-on/off cascade. Board names are `chip[-feature[-feature]]` — recognisable from Espressif's own `IDF_TARGET` (chip part) and feature-flag suffixes everyone reads at a glance.

**Board → sdkconfig + feature table:**

| `--board` | IDF target | `SDKCONFIG_DEFAULTS` (semicolon-joined) | WiFi compiled in? |
|---|---|---|---|
| `esp32` | `esp32` | `sdkconfig.defaults` | yes |
| `esp32-eth` | `esp32` | `sdkconfig.defaults;sdkconfig.defaults.eth` | **no** (EXCLUDE_COMPONENTS + `MM_ETH_ONLY=1`) |
| `esp32-eth-wifi` | `esp32` | `sdkconfig.defaults;sdkconfig.defaults.eth` | yes |
| `esp32s3-n16r8` | `esp32s3` | `sdkconfig.defaults;sdkconfig.defaults.esp32s3-n16r8` | yes |

**Why split the Eth lines out of `sdkconfig.defaults`:**

The base file used to carry 7 Olimex-specific Eth lines (`CONFIG_ETH_USE_ESP32_EMAC=y` … `CONFIG_ETH_DMA_TX_BUFFER_NUM=10`). That's wrong for the `esp32` (WiFi-only) board — RMII GPIOs are tied up at link time, the boot log complains. Move all 7 Eth lines from the base file into a feature-named fragment `sdkconfig.defaults.eth`, leaving the base file genuinely WiFi-only. After the move, `esp32` needs no extra fragment file — it uses `sdkconfig.defaults` alone.

**Naming note:** the Eth fragment is named for the feature (`.eth`), not the vendor — Olimex pins happen to be the default, but the fragment is the right place for a future PHY-runtime-config to read defaults from. The previous board-vendor-named `sdkconfig.defaults.olimex_gw` is renamed via `git mv` to `sdkconfig.defaults.eth`. The S3 fragment is renamed `sdkconfig.defaults.esp32s3_n16r8` → `sdkconfig.defaults.esp32s3-n16r8` (hyphen rather than underscore, matches the board key). Each S3 SKU keeps its own fragment because flash size, partition table, and PSRAM mode differ per SKU — flashing an `n16r8` binary onto a different module misaligns the partition table.

**Files to edit:**

- [scripts/build/build_esp32.py](../../scripts/build/build_esp32.py) — replace the `--profile` argument logic with a `BOARDS` dict + `--board` argument. `--profile` becomes a deprecated alias (`eth-only` → `esp32-eth`, `default` → `esp32`) for one release. Replace `profile_cmake_args()` with `board_cmake_args(board)`.
- [scripts/build/build_esp32_ethonly.py](../../scripts/build/build_esp32_ethonly.py) — forward `--board esp32-eth` instead of `--profile eth-only`. Kept for any external scripting that already calls the filename.
- [esp32/sdkconfig.defaults](../../esp32/sdkconfig.defaults) — remove the 7 Eth-block lines. File becomes board-neutral WiFi-default.
- [esp32/sdkconfig.defaults.eth](../../esp32/sdkconfig.defaults.eth) — renamed from `.olimex_gw`. Self-sufficient (carries every Eth setting the working Olimex build needs); comment names Olimex as the default pin map and points at the 2.0 PHY-runtime-config plan.
- [esp32/sdkconfig.defaults.esp32s3-n16r8](../../esp32/sdkconfig.defaults.esp32s3-n16r8) — renamed from `.esp32s3_n16r8` (hyphen instead of underscore — matches the board key). No content change.
- Profile-change marker: rename `esp32/build/.mm_profile` → `.mm_board`. Migrate on first run (if the legacy file exists, read it once, treat as the equivalent board, then write the new marker).
- [scripts/moondeck.py](../../scripts/moondeck.py) — add `extra_args` forwarding (3 lines) so a config entry can pass static flags to its script.
- [scripts/moondeck_config.json](../../scripts/moondeck_config.json) + [scripts/MoonDeck.md](../../scripts/MoonDeck.md) — replace the "Build" / "Build (Ethernet-only)" pair with four board buttons, each baking a `--board` arg via `extra_args`.
- [docs/moonmodules/core/NetworkModule.md](../moonmodules/core/NetworkModule.md) — update the Ethernet-only section to reference `--board esp32-eth`.

### Step 2 — Version-drift guard (0.5 h)

CI must verify `git tag == library.json["version"]` and fail before building. No CI write-back; maintainer-driven version bumps.

**Files to create:**

- [scripts/build/verify_version.py](../../scripts/build/verify_version.py) — short script that reads `GITHUB_REF_NAME` (without leading `v`) and `library.json` `"version"`, fails the workflow if they differ.

**Action at release time:**

Maintainer bumps `library.json` from `0.1.0` to `1.0.0`, commits, tags `v1.0.0`, pushes tag.

### Step 3 — Desktop packaging (1.5 h)

Static-link what we can; accept dynamic libc++ on macOS (Apple doesn't ship a static libc++.a). Use MSVC `/MT` on Windows to avoid the vcredist dependency.

**Files to create / edit:**

- [scripts/build/package_desktop.py](../../scripts/build/package_desktop.py) — new. Reads version from `library.json`, detects host platform, runs the right CMake invocation, packages.
  - macOS arm64: `cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 && cmake --build build --config Release`. Tarball as `dist/projectMM-macos-arm64-vX.Y.Z.tar.gz` with the binary + a short `README.txt`.
  - Windows x64: `cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded && cmake --build build --config Release`. Zip as `dist/projectMM-windows-x64-vX.Y.Z.zip`.
- [CMakeLists.txt](../../CMakeLists.txt) — gate the warning flags by compiler:
  ```cmake
  if(MSVC)
      add_compile_options(/W4 /WX)
      set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
      add_compile_options(-Wall -Wextra -Werror)
  endif()
  ```

**Honest note on MSVC `/WX`:** the code is unlikely to be clean under `/W4` warning-as-error on first try (signed/unsigned conversions, `[[maybe_unused]]` discipline, `snprintf` warnings differ). **Fallback if a clean build is days away:** disable `/WX` on Windows for 1.0 and file a follow-up. The ESP32 firmware is the primary product; the desktop binary is a convenience.

### Step 4 — Manifest generator (1 h)

ESP Web Tools manifest format (one file per board, referenced by the install page). Offsets are **chip-family-specific**:

- ESP32 (classic): bootloader at `0x1000` (4096).
- ESP32-S3: bootloader at `0x0` (0). The ROM expects it there — wrong offset bricks visibly.

Don't hardcode the offset table — read from `esp32/build/flasher_args.json` produced by the build (it already contains the correct offsets per chip). The CI build job copies `flasher_args.json` alongside the bins; the manifest generator parses it.

**Files to create:**

- [scripts/build/generate_manifest.py](../../scripts/build/generate_manifest.py) — takes `--board <key> --version <ver> --release-url <url> --flasher-args <path> --out <path>`, writes the manifest JSON with parts ordered by offset.

Schema:

```json
{
  "name": "projectMM",
  "version": "1.0.0",
  "home_assistant_domain": "projectMM",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "<release-url>/firmware-esp32-eth-v1.0.0-bootloader.bin",      "offset": 4096   },
        { "path": "<release-url>/firmware-esp32-eth-v1.0.0-partition-table.bin", "offset": 32768  },
        { "path": "<release-url>/firmware-esp32-eth-v1.0.0-ota-data.bin",        "offset": 57344  },
        { "path": "<release-url>/firmware-esp32-eth-v1.0.0.bin",                 "offset": 65536  }
      ]
    }
  ]
}
```

### Step 5 — CI release workflow (4 h)

**File: [.github/workflows/release.yml](../../.github/workflows/release.yml)** — new.

Job graph: `verify-version` → (`build-esp32` matrix × 4, `build-macos`, `build-windows`) → `release`. The final job collects artifacts from all five build jobs (4 ESP32 + 2 desktop), generates manifests, uploads to the release, and deploys Pages.

Key shape:

```yaml
name: Release
on:
  push:
    tags: ['v*']
  workflow_dispatch:
    inputs:
      tag: { description: 'Tag (must already exist, e.g. v1.0.0)', required: true }

jobs:
  verify-version:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: python scripts/build/verify_version.py

  build-esp32:
    needs: verify-version
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        board: [esp32, esp32-eth, esp32-eth-wifi, esp32s3-n16r8]
    steps:
      - uses: actions/checkout@v4
      - uses: actions/cache@v4
        with:
          path: |
            ~/.espressif
            ~/esp/esp-idf
          key: esp-idf-v5.4-${{ runner.os }}
      - uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: v5.4
          target: ${{ startsWith(matrix.board, 'esp32s3') && 'esp32s3' || 'esp32' }}
          path: 'esp32'
          command: python ../scripts/build/build_esp32.py --board ${{ matrix.board }}
      - name: Stage artifacts
        run: |
          mkdir -p dist
          V=$(jq -r .version library.json)
          B=esp32/build
          cp $B/projectMM.bin                       dist/firmware-${{ matrix.board }}-v$V.bin
          cp $B/bootloader/bootloader.bin           dist/firmware-${{ matrix.board }}-v$V-bootloader.bin
          cp $B/partition_table/partition-table.bin dist/firmware-${{ matrix.board }}-v$V-partition-table.bin
          cp $B/ota_data_initial.bin                dist/firmware-${{ matrix.board }}-v$V-ota-data.bin
          cp $B/flasher_args.json                   dist/flasher-${{ matrix.board }}.json
      - uses: actions/upload-artifact@v4
        with: { name: esp32-${{ matrix.board }}, path: dist/ }

  build-macos:
    needs: verify-version
    runs-on: macos-14
    steps:
      - uses: actions/checkout@v4
      - run: python scripts/build/package_desktop.py
      - uses: actions/upload-artifact@v4
        with: { name: desktop-macos, path: dist/ }

  build-windows:
    needs: verify-version
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - run: python scripts/build/package_desktop.py
      - uses: actions/upload-artifact@v4
        with: { name: desktop-windows, path: dist/ }

  release:
    needs: [build-esp32, build-macos, build-windows]
    runs-on: ubuntu-latest
    permissions: { contents: write, pages: write, id-token: write }
    steps:
      - uses: actions/checkout@v4
      - uses: actions/download-artifact@v4
        with: { path: artifacts }
      - name: Flatten artifacts
        run: mkdir -p dist && find artifacts -type f -exec mv {} dist/ \;
      - name: Generate manifests
        run: |
          V=$(jq -r .version library.json)
          BASE=https://github.com/${{ github.repository }}/releases/download/v$V
          for B in esp32 esp32-eth esp32-eth-wifi esp32s3-n16r8; do
            python scripts/build/generate_manifest.py \
              --board $B --version $V --release-url $BASE \
              --flasher-args dist/flasher-$B.json --out dist/manifest-$B.json
          done
      - name: Stage GitHub Pages
        run: |
          mkdir -p pages/install
          cp -r docs/install/* pages/install/
          cp dist/manifest-*.json pages/install/
      - uses: softprops/action-gh-release@v2
        with:
          files: |
            dist/firmware-*.bin
            dist/manifest-*.json
            dist/projectMM-*.tar.gz
            dist/projectMM-*.zip
          fail_on_unmatched_files: true
      - uses: actions/upload-pages-artifact@v3
        with: { path: pages }
      - uses: actions/deploy-pages@v4
```

ESP-IDF caching at `~/.espressif` + `~/esp/esp-idf` is ~2 GB, well under the 10 GB repo cache cap. First run ~10 min, subsequent restores ~30 s.

### Step 6 — Installer page (1.5 h)

WLED-style minimal page. URL after Pages deployment: `https://ewowi.github.io/projectMM/install/`.

**Files to create:**

- [docs/install/index.html](../install/index.html) — board dropdown + `<esp-web-install-button>`. On dropdown change, swap the `manifest` attribute on the button. Use the unpkg-hosted ESP Web Tools v10.
- [docs/install/README.md](../install/README.md) — one-paragraph note explaining the manifests are *generated per-release* by `release.yml`, not committed to git. Cloners won't see them locally.

Page structure:

```html
<label for="board">Board:</label>
<select id="board">
  <option value="esp32">ESP32 — WiFi only</option>
  <option value="esp32-eth">ESP32 — Ethernet only (Olimex pins)</option>
  <option value="esp32-eth-wifi">ESP32 — Ethernet + WiFi (Olimex pins)</option>
  <option value="esp32s3-n16r8">ESP32-S3 DevKitC-1 (N16R8) — WiFi only</option>
</select>
<esp-web-install-button id="installer" manifest="manifest-esp32.json"></esp-web-install-button>
```

After-flash UX text: "The device boots a SoftAP named `projectMM-xxxx`. Join it, open `http://4.3.2.1`, enter your WiFi credentials."

### Step 7 — Enable GitHub Pages (manual, 0.25 h)

One-time repo setting: **Settings → Pages → Source: GitHub Actions**. The `deploy-pages` action in `release.yml` publishes. No code change.

### Step 8 — README + building.md (0.75 h)

- [README.md](../../README.md) — replace the "From a release" subsection with two crisp paragraphs: ESP32 flash via the installer URL, desktop binaries via the Releases page. Drop the Teensy / RPi / Linux desktop bullets — those aren't shipped in 1.0. They go back in when the binaries exist.
- [docs/building.md](../building.md) — replace the "Build profiles" subsection with a "Boards" table mirroring step 1's table. Drop the obsolete `--profile` doc (or fold into the `esp32-eth` row note).

### Step 9 — plan.md → 2.0 stub (0.25 h)

- [docs/plan.md](../plan.md) — replace the "Release 1.0" milestone section with a forward-looking "Release 2.0" section:
  - ESP32-P4 board variant.
  - OTA / FirmwareUpdateModule (passive-observer pattern from v1).
  - Linux desktop binary.
  - Nightly CI / pre-release channel.
  - Improv WiFi for one-step flash-then-credentials.
- Strike item 13 (README quick-start) and the Release 1.0 milestone — both subsumed by this plan.

### Step 10 — End-to-end dry run (1 h)

Three test surfaces stacked from cheapest to most production-like. Each catches
problems the next one would also catch, but later and at higher cost.

**Test surface 1 — Local C+ recipe.** Documented in
[`docs/install/README.md`](../install/README.md) § "End-to-end with CI-built
firmware". Runs against the latest branch CI artifacts. Catches manifest
schema errors, page-render bugs, real Web Serial flash against a real binary
per board. Zero CI minutes, zero release-page noise. Run this *before* any RC
tag — it's a 5-minute loop. Doesn't exercise GitHub Pages deploy or the
gh-release action.

**Test surface 2 — RC tag (`vX.Y.Z-rcN`).** The full release pipeline minus
the Pages publish. Releases land as **pre-releases** (marked with the
GitHub "Pre-release" badge, sorted below stable releases, not picked up by
"latest" tooling). The Pages deploy step is skipped on RC tags so end users
visiting the installer URL keep seeing the previous stable release. Catches
everything surface 1 misses except the live Pages flip. Iterate `rcN → rcN+1`
as needed — RC releases are cheap to delete.

**Test surface 3 — Stable tag (`vX.Y.Z`).** The real release. Pages flips to
publish the installer page from this tag's manifests. Only run when surfaces
1 and 2 are clean.

**Procedure for the first 1.0:**

1. **Surface 1.** Push the branch, wait for branch CI green, run the local
   C+ recipe with the new artifacts. Flash each of the four boards locally.
   Fix any issues in source and repeat.
2. **Surface 2.** When the local recipe passes:
   - Bump `library.json` to `1.0.0-rc1`. Commit. Tag `v1.0.0-rc1`. Push tag.
   - Watch Actions: `verify-version` + 4 ESP32 jobs + 2 desktop jobs + release
     job all green. (Pages staging + deploy steps are correctly *skipped*.)
   - Visit the release page: 22 files (4×4 ESP32 bins + 4 manifests + macOS
     tarball + Windows zip). The release is flagged "Pre-release".
   - Manually point the local install page at the rc1 release URLs (edit the
     manifest's `release-url` and regenerate), flash each board, confirm the
     UI loads.
   - Issues found? Delete the RC: `gh release delete v1.0.0-rc1 --yes && git
     push --delete origin v1.0.0-rc1`. Fix on the branch, bump `library.json`
     to `1.0.0-rc2`, tag, push. Repeat until clean.
3. **Surface 3.** When the RC is fully green:
   - Bump `library.json` from `1.0.0-rcN` to `1.0.0`. Commit. Tag `v1.0.0`. Push.
   - Watch Actions: everything green, **including** Pages staging + deploy
     (no longer skipped because the tag has no `-rc`).
   - Visit `https://ewowi.github.io/projectMM/install/`. Page loads, dropdown
     has 4 options. The flash path uses the stable v1.0.0 release URLs.
   - Optional cleanup: delete the leftover RC tags + pre-releases.

If any step fails on a real board: don't ship. The installer page exists
precisely so end users don't need to read serial logs — its first-flash
experience has to be reliable.

## Per-release criteria

These run as the per-release additions to CLAUDE.md's Event 3 (Release tag) gates, on top of the always-on items (PR-merge gates passed, hardware test, no known critical bugs).

1. **Principles audit.** Sweep `docs/` (excluding `docs/plan.md` and `docs/history/`) and `src/` for present-tense violations and forward-looking language — "roadmap", "will be", "in the future", "planned", "todo", "currently lacks" outside the allowed locations. The reviewer agent can run this; a one-line `grep -rn "TODO\|will be\|going to\|in the future" docs/ src/` gives a starting list. Acceptable hits get one-line justifications; the rest get rewritten present-tense or moved to `docs/plan.md` / `docs/history/`.
2. **All Principles in CLAUDE.md** verified end-to-end: common patterns first (no bespoke conventions sneaking in), minimalism (nothing earning its place got added without paying for itself), data over objects, concrete first, domain-neutral core, present tense.
3. **Cross-board flash test.** All four ESP32 board variants flashed from the installer page on actual hardware (or two variants × the boards available — see dry-run § Step 10).
4. **Branch CI cleanup decision.** If the pre-1.0 `push.branches` trigger in `release.yml` has earned removal (the build path is proven across N tags), strike the `branches:` block before tagging 1.0. Otherwise carry it forward with a comment refresh.

## Critical files to be modified or created

**New:**

- [.github/workflows/release.yml](../../.github/workflows/release.yml)
- [scripts/build/package_desktop.py](../../scripts/build/package_desktop.py)
- [scripts/build/generate_manifest.py](../../scripts/build/generate_manifest.py)
- [scripts/build/verify_version.py](../../scripts/build/verify_version.py)
- [docs/install/index.html](../install/index.html)
- [docs/install/README.md](../install/README.md)

**Edited:**

- [scripts/build/build_esp32.py](../../scripts/build/build_esp32.py) — add `--board`, deprecate `--profile`.
- [scripts/build/build_esp32_ethonly.py](../../scripts/build/build_esp32_ethonly.py) — forwards to `--board esp32-eth`.
- [esp32/sdkconfig.defaults](../../esp32/sdkconfig.defaults) — drop the 7 Eth lines.
- [esp32/sdkconfig.defaults.eth](../../esp32/sdkconfig.defaults.eth) — renamed from `.olimex_gw`. Self-sufficient (carries the full Olimex pin set).
- [esp32/sdkconfig.defaults.esp32s3-n16r8](../../esp32/sdkconfig.defaults.esp32s3-n16r8) — renamed from `.esp32s3_n16r8`. No content change.
- [CMakeLists.txt](../../CMakeLists.txt) — MSVC-gated warning flags + static MSVC runtime.
- [library.json](../../library.json) — bump `0.1.0` → `1.0.0` at release time.
- [scripts/moondeck.py](../../scripts/moondeck.py) — `extra_args` forwarding.
- [scripts/moondeck_config.json](../../scripts/moondeck_config.json) + [scripts/MoonDeck.md](../../scripts/MoonDeck.md) — four board buttons.
- [docs/moonmodules/core/NetworkModule.md](../moonmodules/core/NetworkModule.md) — `--board esp32-eth` reference.
- [README.md](../../README.md) — Quick Start with installer URL.
- [docs/building.md](../building.md) — boards table.
- [docs/plan.md](../plan.md) — Release 1.0 → 2.0 stub.

**Manual:**

- Repo Settings → Pages → Source: GitHub Actions.

## Verification

- **Local board builds:** `python scripts/build/build_esp32.py --board esp32`, `--board esp32-eth`, `--board esp32-eth-wifi`, and `--board esp32s3-n16r8` all complete with zero warnings. Each writes `esp32/build/projectMM.bin` + `flasher_args.json` for the right chip.
- **Local desktop:** `python scripts/build/package_desktop.py` on macOS produces a tarball under `dist/` that runs on a fresh Mac.
- **Local installer (surface 1):** the C+ recipe in [`docs/install/README.md`](../install/README.md) — pull branch CI artifacts with `gh run download`, serve locally, flash each of the four boards over USB through the install page.
- **RC dry run (surface 2):** push `v1.0.0-rcN` tag, all workflow jobs green, pre-release page populated with 22 files, Pages staging + deploy correctly skipped, manual flash per board succeeds against the rcN release URLs.
- **Real release (surface 3):** tag `v1.0.0`, Pages flips, the live `https://ewowi.github.io/projectMM/install/` flashes each board cleanly.

## Risks and unknowns

1. **MSVC `/WX` cleanliness.** Likely a few warning fixes needed before Windows binary builds. Acceptable fallback: drop `/WX` for 1.0, file follow-up.
2. **ESP-IDF version pin (v5.4 vs the project's v6.x-dev SHA).** Project uses some v6-era APIs (`esp_eth_phy_new_generic`, mDNS component manager). If v5.4 doesn't compile, fall back to manual ESP-IDF checkout at the exact SHA via a CI step before `esp-idf-ci-action`.
3. **`<esp-web-install-button>` manifest swap on dropdown change.** Some versions cache the parsed manifest. Mitigation: if `setAttribute` doesn't pick up the new value live, recreate the element on dropdown change.
4. **GitHub Pages CORS for binaries on `objects.githubusercontent.com`.** Should be `Access-Control-Allow-Origin: *` already — WLED, ESPHome, dozens of projects use this exact pattern. Verify in the RC dry-run by manually rewriting the local install page's manifest to the rcN release URLs and flashing through it; if it fails, host binaries on the gh-pages branch instead of release assets.
5. **macOS arm64 Gatekeeper warning.** Unsigned binary triggers "downloaded from internet, allow?" on first run. Document in release notes for 1.0; code-signing is 2.0+ work.
6. **`new_install_prompt_erase: true` wipes saved config.** Right default for a beta product (avoids stale-config bugs). Document; revisit when the config schema is stable.

## Notes

- Per CLAUDE.md per-feature workflow, this plan is saved as `docs/history/plan-17.md` at the start of implementation.
- Per CLAUDE.md gate-3 rule, plan reconciliation lands on the branch before the merge commit. This branch already carries pending `decisions.md` + plan-archive moves from the previous merge — the implementer should let them ride in the same commit train as plan-17's work.
