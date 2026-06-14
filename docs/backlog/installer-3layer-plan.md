# Installer / 3-layer device model — initiative plan

A living plan tracking the **remaining** installer increments. As each is built,
its durable content moves to a permanent home and its entry is removed here — this
doc shrinks toward empty by the end of the branch. (Forward-looking, so it lives in
`backlog/`.)

**The durable parts already migrated** (don't re-add them here):

- **MCU → Board → Device config-provenance model** + the "default only at the level
  that fixes it" rule + `txPowerSetting` example →
  [architecture.md § Config provenance](../architecture.md#config-provenance-mcu--board--device).
- **Catalog schema** (`modules` units, add-then-configure, `image`/`url`, the
  image-host-not-hotlink rule, partial injection, one-entry-type) →
  [install/README.md](../install/README.md).
- **Hard-won principles** — inject-vs-buried, investigate-before-building, the
  `loopbackTxPin` lane-0 lesson, Board-vs-Device completeness spectrum + the
  three-category pin model, the per-board capability spec'n'test loop, drivers
  catalog-added + the OTA-keeps-config nuance →
  [decisions.md § catalog-driven installer branch](../history/decisions.md).
- **Deployment / `install-alt` build-beside-swap** and the non-breaking-vs-breaking
  rule → [install/README.md § Picture board picker](../install/README.md#picture-board-picker-install-alt).

**The single hard sequencing rule** that governs the rest:

> **A catalog field only earns its keep once a device-side consumer reads it.** A
> first board catalog was built and rolled back once for exactly this reason
> ([backlog § Runtime board presets](backlog.md#runtime-board-presets-multi-commit-partially-landed)).
> So data (firmwares.json pin fields, devices.json, MCU pin maps) never lands before
> the consumer that reads it.

## Done (kept until committed, then deleted)

- **#1 Partition-table dedup** — three identical 16 MB CSVs collapsed into
  [`ota_16mb.csv`](../../esp32/partitions/ota_16mb.csv); the three sdkconfig fragments
  repointed. Committed.
- **#3 Catalog-driven module provisioning** — `modules` units + the three clients'
  add-then-configure pass + drivers catalog-added + `loopbackTxPin`. Schema in the
  [installer README](../install/README.md), lessons in
  [decisions.md](../history/decisions.md). Hardware-verified on S3 / classic / P4.
- **#5 Picture board picker** — built in [`install-alt/`](../install-alt/) per
  build-beside-swap; documented in the [installer README](../install/README.md#picture-board-picker-install-alt).
  **Pending the swap** (fold into `/install/`, delete `install-alt/`) once approved.

## Remaining workstreams

### 2. firmwares.json — GENERATE, don't hand-author

The firmware list is the **`FIRMWARES` dict** in
[scripts/build/build_esp32.py](../../scripts/build/build_esp32.py) (line ~60) — the
single source of truth (chip + sdkconfig fragments + eth-only flag + description;
validates `--firmware`; burns the key into the binary via `-DMM_FIRMWARE_NAME`).
The list is currently duplicated **4×**: the dict + three hand-copied lists in
[.github/workflows/release.yml](../../.github/workflows/release.yml) (CI matrix,
release manifest loop, Pages manifest loop).

Decision: **emit `firmwares.json` from the `FIRMWARES` dict at release time** (the
same step that builds the ESP Web Tools manifests), not a 5th hand-kept copy. The
same generator can emit the matrix JSON and collapse the duplicated lists too — one
source, zero drift.

- **Caveat:** `FIRMWARES` has 8 entries but only 7 *ship* (`esp32p4-eth-wifi` is out
  of the CI matrix — it doesn't build reproducibly yet, see
  [backlog § ESP32-P4 rounds 3-4](backlog.md#esp32-p4-support--rounds-3-4-in-progress)).
  The generator needs a per-entry "shipping" flag, not just "exists."

### 4. Catalog + picture model — remaining: `devices.json`, MCU layer, annotated pins

The board-level catalog (`boards.json` with `image`/`url`/`modules`) and the picture
picker (#5) are done. What remains of the fuller model:

- **Annotated-pin images** — board *and* device images with pins marked (e.g. INMP441
  with WS/SD/SCK labelled) so a user reads wiring off the picture. Needs a consistent
  annotation approach. Wiring diagrams (device-pin → board-GPIO) are a later, deeper
  increment.
- **`devices.json` / MCU layer** — only if the flat one-entry-type model
  ([install/README.md](../install/README.md)) stops sufficing. Today a Device is a
  Board entry with more filled in; a separate file lands only when a real need (e.g.
  the `extends` carrier→device composition) earns it, per the sequencing rule.
- **MoonDeck device-profile** — save/restore the Device-level pin set for a named
  device (in `moondeck.json`), so re-flashing or adding a second identical rig doesn't
  re-type GPIOs.

Gated behind workstream 3 (done) and, for board-level Ethernet pins, workstream 7's
runtime PHY config.

### 6. Shared installer code across the three clients

Web installer (`docs/install/`), MoonDeck (`scripts/`), and the on-board OTA picker
share `install-picker.js` (installer ↔ OTA). As the picker grows device/board/MCU
pictures + pin injection, factor the shared logic so all three stay in sync. The
friction is the **build-context split**: device UI is embedded as a C string via
`embed_ui.cmake`; the web installer is served from Pages — a shared extract needs
build-glue across ~5 files
([backlog § shared JS helpers](backlog.md#open-follow-up-shared-js-helpers-across-device-ui-and-web-installer)).
Do it when ≥2 helpers earn the glue cost, not for one.

### 7. Firmware variant collapse (classic ESP32 + P4) — gated on runtime PHY

Collapse the three classic variants to two
([backlog § Release 2.0](backlog.md#release-20--distribution-catches-up-to-the-source-tree)):

- **`esp32`** — default: WiFi **and** Ethernet, Eth brought up only when a PHY is
  detected/configured. Replaces both today's `esp32` and `esp32-eth-wifi`.
- **`esp32-eth-only`** (today's `esp32-eth`) — Eth only, WiFi compiled out.

Why two-not-three: **Ethernet is a light addition** (the `.eth` fragment is just EMAC
+ RMII + ~1.5 KB DMA), so it needs no separate "+wifi" variant — fold it into the
default. **WiFi is heavy** (~670 KB flash + ~30 KB DRAM, measured), so the Eth-only
build that strips it still earns its own firmware. Same shape for the P4.

**Hard precondition — do NOT merge yet.** The `.eth` fragment hardwires Olimex Gateway
pins (`PHY_ADDR=0`, `RST_GPIO=5`, LAN8720 RMII). If the default `esp32` baked that in,
every WiFi-only classic board would enable EMAC and **reserve GPIO 5** for a PHY that
isn't there — a silent pin grab the base `sdkconfig.defaults` comment guards against.
The prerequisite is
[backlog § Runtime PHY / pin config for Ethernet](backlog.md#release-20--distribution-catches-up-to-the-source-tree):
`ethInit()` selects pins/PHY at runtime and no-ops when no PHY is present.

### 8. Release-asset dedup — share the boring per-firmware files

Each firmware ships **4** release assets: `bootloader.bin`, `partition-table.bin`,
`ota-data.bin`, app `.bin`. Measured across all 7 builds, only app + bootloader are
truly per-firmware:

| Asset | Distinct across 7 | Shareable? |
|---|---|---|
| `ota-data.bin` | **1** — byte-identical | ✅ **globally** (a fixed 0x2000 `0xFF` region, chip-/app-independent) |
| `partition-table.bin` | **3** — one per CSV group (4/16/8 MB) | ◐ **per partition group** (mirrors the CSVs) |
| `bootloader.bin` | **7** — all differ | ❌ never (chip + flash-size/mode baked in) |
| app `.bin` | 7 | ❌ the unique payload |

So the dedup removes redundant **ota-data** (7→1) and **partition-table** (7→3) — ~10
files today, and **it pays off as the firmware count grows** (one-per-MCU → 15+
variants → ~25+ redundant files). Cost: the web installer flashes a manifest of
`{bootloader, partition-table, ota-data, app}` at fixed offsets, so sharing those
filenames means every generated manifest points at the shared name — slightly weakening
the "one self-contained asset set per firmware" property. Done via the **manifest
generator** (#2's machinery), not by hand, so the shared-name references stay
drift-free. Sequence **after** the firmwares.json generator exists.

### 9. EIM coordination — disambiguate the two "installs"

Two **unrelated "installers"**, worth stating as the firmware-variant work grows:

- **EIM — ESP-IDF Installation Manager** is a **developer host-side** toolchain
  installer, replacing the legacy `install.sh` / `idf_tools.py` that `setup_esp_idf.py`
  drives ([building.md § ESP-IDF version](../building.md#esp-idf-version)). Unrelated to
  flashing an end-user's device.
- **This plan's installer** is **end-user device-side** (web installer / MoonDeck / OTA).

They're orthogonal in function but coordinated for two reasons: (1) **naming** — keep
specs/UI explicit about which "install" is meant; (2) **EIM scales with this plan** —
its multi-IDF-version management keeps building *many* firmware variants reproducible as
the matrix grows toward one-per-MCU, the build-side companion to #8's dedup and #2's
generator. EIM itself is tracked in
[building.md](../building.md#esp-idf-version) / [backlog § ESP-IDF version pinning](backlog.md#esp-idf-version-pinning-pending);
this workstream's job is only to keep the two tracks coordinated — adopt EIM early.

## Suggested build order

1. **firmwares.json generator** (#2) — small, removes the 4× duplication; the machinery
   #8 reuses.
2. **Release-asset dedup** (#8) — share ota-data + partition-table via #2's generator.
3. **Annotated-pin model** (#4 remainder) — once its consumer exists.
4. **Shared-code factoring** (#6) — when ≥2 helpers earn the glue.
5. **Runtime PHY config**, then **variant collapse** (#7) — its own branch.

Cross-cutting / parallel: **EIM adoption** (#9), a build-side track, adopted early so
the matrix is ready for one-firmware-per-MCU growth. None of the catalog steps land
data ahead of its consumer.
