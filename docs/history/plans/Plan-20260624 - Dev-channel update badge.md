# Plan — Per-build `latest` version (`2.1.0-dev.<N>`) + dev-channel update badge

Lands on `next-iteration` (joins PR #27), building on the semver-clean version + update-badge work.

## Context

The semver-clean version work gave the device a clean `version`, but the moving `latest` build has **no distinct version identity**: every `latest` build reports `2.1.0-dev`, and its published manifest/assets are stamped with the old stable `2.0.0` (everything keys off `library.json`'s bare version). So two devices from different `latest` builds report the same version, and the update badge can't tell a stale `latest` device that a newer `latest` exists.

Fix (semver.org §9/§11, also corrects the mislabeled-`2.0.0` manifest): give each `latest` build a monotonic prerelease version `2.1.0-dev.<N>`, `<N>` = commit count since the last tag (`git rev-list --count`, git-describe style). `semver.js` already compares these numerically (§11). Then extend the badge so a device on a `-dev` build also lights up when a newer `latest` exists.

## Decisions (PO)
- `<N>` = commit count since last tag.
- Lands on `next-iteration` (PR #27).

## Approach

### 1. Per-build `latest` version through the pipeline
One computed `V`, consistent across binary (`MM_VERSION`), asset names (`firmware-<F>-v<V>.bin`), manifest (`generate_manifest.py --version`):
- Stable `vX.Y.Z`: `V` = library.json core (drop `-dev`). Unchanged.
- `latest` (main push): `V` = `<core>-dev.<commit-count-since-last-tag>`.
- Local/dev: library.json verbatim — `2.1.0-dev` sorts *below* any published `2.1.0-dev.N`, so a local build never falsely claims newer.

Reuse the existing `-D` override pattern (`MM_FIRMWARE_NAME`/`MM_RELEASE` already do this):
- `generate_build_info.py`: `MM_VERSION` becomes an `#ifndef` default (= library.json), overridable by `-DMM_VERSION`.
- `build_esp32.py` `firmware_cmake_args(...)`: optional `version` → `-DMM_VERSION`; add `--version` CLI arg.
- `compute_version.py` (NEW): the `V` computation as a testable helper (stable core vs latest `-dev.<count>`, tag-less fallback to `rev-list --count HEAD`).
- `release.yml`: call compute_version once, `fetch-depth: 0`, thread `V` into build matrix + asset-name step + manifest step.

### 2. Dev-channel update badge (`app.js`)
Extend `checkFirmwareUpdate`: if the device version is a prerelease (`parse(...).prerelease.length > 0`) and no stable update is shown, fetch the `latest` release (cache key `projectMM.update.dev.v1`), read its version from `manifest-<firmware>.json` (`.version`), `isNewer(latestDev, deviceVersion)` → badge → click opens Firmware with `latest` pre-selected. Stable update takes precedence. Best-effort, cached, compatible-`.bin` check applies.

## Files
- `scripts/build/compute_version.py` (NEW) + `test/python/test_compute_version.py` (NEW)
- `scripts/build/generate_build_info.py` — `MM_VERSION` overridable `#ifndef`
- `scripts/build/build_esp32.py` — `--version` → `-DMM_VERSION`
- `.github/workflows/release.yml` — compute V, fetch-depth 0, thread through
- `src/ui/app.js` — dev-channel branch
- `src/ui/semver.js` tests — `2.1.0-dev.7 > 2.1.0-dev.6`, `2.1.0-dev.1 > 2.1.0-dev`
- `docs/moonmodules/core/FirmwareUpdateModule.md` — note `-dev.<N>` for latest

## Verification
- Host: node/python tests, build, ctest, scenarios, spec check.
- Smoke: `build_esp32.py --version 2.1.0-dev.7` → device reports it.
- Bench S3: flash `-dev.5`, latest manifest reports higher `-dev.N` → badge appears, click opens Firmware/latest. Newest `-dev` → no badge.
- CI dry: compute_version → `2.1.0-dev.<n>` on main, `2.1.0` on tag; verify-version passes (compares cores).

## Risks
- Python helper over inline YAML shell (testable). `fetch-depth: 0` required for git history. Tag-less fallback. One extra fetch for `-dev` devices, cached.
