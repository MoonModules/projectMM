# Installer / 3-layer device model — initiative plan (closed)

This initiative is **complete for the `next-iteration` branch**. It built the
catalog-driven installer end to end; the durable content has moved to permanent
homes, and the only remaining pieces are explicitly gated future work tracked in
[backlog.md](backlog.md). This doc is a closing index — keep it until the branch
merges, then it can be deleted.

## Shipped

| # | What | Where it lives now |
|---|------|--------------------|
| 1 | Partition-table dedup (three 16 MB CSVs → `ota_16mb.csv`) | `esp32/partitions/` |
| 2 | `firmwares.json` generator (`FIRMWARES` → generated, drift-guarded; CI + MoonDeck read it) | [generate_firmwares.py](../../scripts/build/generate_firmwares.py), [firmwares.json](../install/firmwares.json) |
| 3 | Catalog-driven module provisioning (`modules` units, add-then-configure, drivers catalog-added, `loopbackTxPin`) | [install/README.md](../install/README.md), [decisions.md](../history/decisions.md) |
| 5 | Picture board picker — **folded into `/install/`** (the picture grid IS the installer; `install-alt/` deleted, the build-beside swap done) | [install/index.html](../install/index.html), [README § Picture board picker](../install/README.md#picture-board-picker) |
| 8 | Release-asset dedup (`shared-ota-data.bin` ×1, `partition-table-<size>.bin` ×3) | [generate_manifest.py](../../scripts/build/generate_manifest.py), `release.yml` |
| 10 | Detected-chip → family normalisation (fixes the false flash-mismatch warning + board filter) — resolved as **Option A** (normalise at compare time; `chipFamily()` + the single `TARGET_TO_FAMILY` map) | [install-orchestrator.js](../install/install-orchestrator.js), [build_esp32.py](../../scripts/build/build_esp32.py) |
| 4 | MoonDeck device-profile (save/restore a device's pin set) | [moondeck.py](../../scripts/moondeck.py) |

Durable concepts already in permanent homes: the **MCU → Board → Device provenance
model** → [architecture.md § Config provenance](../architecture.md#config-provenance-mcu--board--device);
the **catalog schema** → [install/README.md](../install/README.md); the **hard-won
lessons** → [decisions.md](../history/decisions.md).

## Deferred (gated future work — tracked in backlog.md)

These are **not** finishable now by design; each has a permanent home in
[backlog.md](backlog.md):

- **#4 remainder — annotated-pin images, `devices.json`/MCU layer.** Gated by the
  sequencing rule (*a catalog field earns its keep only once a consumer reads it*):
  annotated-pin images need an annotation approach + a UI consumer; the `devices.json`
  split lands only when the flat catalog gets unwieldy. **The direction is decided** —
  "board = devkit, device = product (a board with its Device level filled in)", split
  via a `kind` tag or `devices.json` + `extends` when scale forces it; see
  [architecture.md § Board vs Device](../architecture.md#config-provenance-mcu--board--device).
  → backlog [§ Runtime board presets](backlog.md#runtime-board-presets-multi-commit-partially-landed).
- **#6 — shared JS helpers across device-UI and web-installer.** Worth the build-glue
  (`embed_ui.cmake` + generator + routing + staging, ~5 files) only when ≥2 helpers
  earn it. → backlog [§ shared JS helpers](backlog.md#open-follow-up-shared-js-helpers-across-device-ui-and-web-installer).
- **#7 — firmware-variant collapse (classic + P4).** Hard-blocked on runtime PHY/pin
  config (the `.eth` fragment bakes in Olimex RMII pins; collapsing without runtime PHY
  would silently grab GPIO 5 on every WiFi-only board). → backlog
  [§ Release 2.0 / Runtime PHY](backlog.md#release-20--distribution-catches-up-to-the-source-tree).
- **#9 — EIM coordination.** A build-side track (ESP-IDF Installation Manager) kept
  coordinated with this end-user installer as the firmware matrix grows. →
  [building.md § ESP-IDF version](../building.md#esp-idf-version).
- **CI hardening — pin GitHub Actions to commit SHAs** (the `persist-credentials: false`
  half is done). → backlog [§ CI: pin GitHub Actions to commit SHAs](backlog.md).
