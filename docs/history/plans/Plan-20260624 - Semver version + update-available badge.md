# Plan — Semver-clean version + "firmware update available" badge

## Context

The Firmware card's `version` control shows `2.0.0 (v2.0.0)` — a semver (`kVersion`) concatenated with a release-channel tag (`kRelease`). For a stable build the tag is just `v` + the semver, so it's redundant *and* non-semver. The product owner wants `version` to be **industry-standard semver, always** — and the channel **derivable from the semver itself**, not stored as separate metadata. The semver-correct way (semver.org §9/§11) to express "a moving `latest`/dev build that is ahead of the last stable but not itself a release" is a **prerelease identifier**: `2.1.0-dev`. So a stable build shows `2.0.0`; the moving `latest` build shows `2.1.0-dev`. Channel = "has a prerelease suffix → not stable."

On top of that clean version, add a status-bar **"firmware update available" badge**: the browser compares the device's running semver to the newest GitHub **stable** release and, when newer, shows a badge that opens the Firmware card. Modelled on ESP32-sveltekit's `UpdateIndicator.svelte` (the upstream firmware lineage MoonLight forks) — *carry the idea forward, write our own code* (CLAUDE.md *Industry standards, our own code*).

## Git tag vs firmware version (important distinction)
`2.1.0-dev` is the **semver burned into the firmware** (`MM_VERSION`), NOT a new git tag. The moving build keeps its **`latest`** GitHub tag — only the version *inside* it changes. So: stable release → tag `v2.0.0`, firmware version `2.0.0`; moving build → tag `latest` (unchanged), firmware version `2.1.0-dev`; next stable → tag `v2.1.0`, firmware version `2.1.0` (the `-dev` suffix dropped at release time). The badge compares the device's firmware version against `releases/latest` (newest stable, `latest` excluded), so a `2.1.0-dev` device shows no badge — it is correctly *ahead* of the latest stable.

## Decisions made with the PO
- Moving/latest builds carry **`2.1.0-dev`** (library.json bumped to the next dev version right after each release — standard "develop on a prerelease" flow).
- Badge fetches GitHub **cached in localStorage, re-fetch only if > 1 hour stale, PLUS** a fresh check when the Firmware module is opened (don't slow page load).
- Semver comparison via a **reusable `src/ui/semver.js`** (our own code, no npm dep), JS unit test. Improves the codebase's semver story (today releases sort by *date*; no semver compare exists).
- **Badge click → open the Firmware card with the new release pre-selected** (lands the user one click from Install). Reuses the picker's `PREF_RELEASE_KEY` restore + `selectModule()`; no new popup.

## Approach (3 pieces)

### 1. Semver-clean version (build pipeline + firmware)
- `library.json`: version `2.0.0` → `2.1.0-dev`. `build_info.h` is gitignored + generated from this, so `MM_VERSION` follows.
- `scripts/build/verify_version.py`: a stable `vX.Y.Z` tag matches `library.json` **with any prerelease suffix stripped** (so `v2.1.0` ↔ `2.1.0-dev` passes — the release of what was in dev; a wrong *core* like `v2.2.0` ↔ `2.1.0-dev` still fails). Keep the `latest`-skips behaviour. Doc the ritual.
- `src/core/FirmwareUpdateModule.h` (`setup()`): `version` control = **just `kVersion`** (pure semver). Drop the `(kRelease)` concatenation. Update inline comment + spec doc.
- `docs/moonmodules/core/FirmwareUpdateModule.md`: `version` description → "pure semver; a `-dev`/prerelease suffix marks a moving/pre-release build."

### 2. Reusable semver module (`src/ui/semver.js`, NEW)
- Dependency-free, textbook: `parse(v)` (strip leading `v`) → `{major,minor,patch,prerelease[]}`; `compare(a,b)` → -1/0/1 per semver.org §11 (numeric core, then prerelease-present < absent, identifiers field-by-field, numeric < non-numeric); `isNewer(candidate,current)` = `compare===1`.
- One home for the comparison (CLAUDE.md *Complexity lives in core*). ESM, importable by app.js + the picker.

### 3. "Update available" badge (status bar)
- `src/ui/index.html`: `<a id="fw-update-badge" class="fw-update-badge" hidden>` in `#status-bar`, before `#ws-dot`.
- `src/ui/style.css`: small amber-ish badge (reuse existing palette), hidden by default.
- `src/ui/app.js`:
  - Cache + fetch (reuse picker's `safeLocalGet/Set` + TTL pattern; key `projectMM.update.latest.v1`; TTL 1 h; serve stale on failure). `getLatestStableRelease({force})` → fetches `api.github.com/repos/MoonModules/projectMM/releases/latest` only if stale or forced.
  - Compare: device `version` + `firmware` key from `/api/state`; `isNewer(latest.tag_name, deviceVersion)` AND a compatible `firmware-<key>-v<ver>.bin` exists in the release assets (mirrors sveltekit's asset-target check).
  - When: cache-first check on load; `{force:true}` when the Firmware module opens.
  - Click: `safeLocalSet("projectMM.picker.releaseTag", tag)` then `selectModule(<firmware module>)` → Firmware card opens with the new release pre-selected, Install ready.
  - Graceful: any failure → badge hidden, `console.warn` only.

## Files
- `library.json` · `scripts/build/verify_version.py` · `src/core/FirmwareUpdateModule.h` · `docs/moonmodules/core/FirmwareUpdateModule.md`
- `src/ui/semver.js` (NEW) · `src/ui/index.html` · `src/ui/style.css` · `src/ui/app.js`
- `test/js/semver.test.mjs` (NEW)

## Verification
- Host: `node --test "test/js/**/*.test.mjs"`; `node --check` the JS + extract-check index.html; `cmake --build build` + `ctest`; `uv run scripts/scenario/run_scenario.py`; `uv run scripts/check/check_specs.py`; a `test/python` verify_version case (`v2.1.0` ↔ `2.1.0-dev` OK; `v2.2.0` ↔ `2.1.0-dev` fails).
- Bench/preview: Firmware card shows clean semver; badge appears on an older device, opens Firmware pre-selected; no badge on newest stable; no error offline.

## Existing releases — already semver-compatible (no migration)
Only `v1.0.0` + `v2.0.0` (both clean semver) + `latest` (moving prerelease channel, excluded by `releases/latest`). Badge input is always a clean `vX.Y.Z`; nothing to migrate.

## Risks / notes
- Release ritual: next stable bumps `library.json` `2.1.0-dev` → `2.1.0` before tagging. Keep verify_version's suffix-strip exact so a wrong core still fails. Call out in the PR.
- GitHub rate limit (60/h/IP): 1 h cache + serve-stale keeps it well under; badge is best-effort.
- No npm toolchain: semver.js is plain ESM, `node:test` only. Our own code, no `compare-versions` dep.
- `release.yml` `paths:` change currently uncommitted on this branch is a *separate* installer-deploy fix — commit independently.
