# projectMM web installer

This directory holds the source for the **ESP Web Tools installer page** that
ships at <https://ewowi.github.io/projectMM/install/> after each release.

End users land here, pick a board, click Install. The browser flashes the
device over USB (Web Serial → ESP32).

## What's in this directory

- [`index.html`](index.html) — the installer page. Board dropdown +
  `<esp-web-install-button>`, recreated on each board change. Self-contained
  (inlined CSS, no build step).

## What's *not* in this directory

- `manifest-*.json` (one per board variant). These are **generated per
  release** by [`.github/workflows/release.yml`](../../.github/workflows/release.yml)
  using [`scripts/build/generate_manifest.py`](../../scripts/build/generate_manifest.py),
  then copied alongside `index.html` into the Pages deployment. They are not
  committed to git — cloning the repo doesn't give you working manifests
  locally, only the page that consumes them.

  This split is on purpose. The manifests reference the exact `.bin` filenames
  and offsets of one specific release; storing them in git would either go
  stale on every release or force a manifest-only commit per tag.

## Local preview

Three ways to test the page locally, in increasing closeness to the real release:

### Page-render only (no real firmware)

Useful when iterating on the install page itself. The browser loads the manifest
but the install button fails on the actual flash step because the `.bin` URLs
point at a release that doesn't exist yet.

```bash
# Build one firmware locally so flasher_args.json exists.
uv run scripts/build/build_esp32.py --board esp32

# Generate a manifest pointing at a (not-yet-existing) release's URLs.
mkdir -p /tmp/mm_install
cp docs/install/index.html /tmp/mm_install/
python scripts/build/generate_manifest.py \
  --board esp32 --version 0.1.0 \
  --release-url https://github.com/ewowi/projectMM/releases/download/v0.1.0 \
  --flasher-args esp32/build/flasher_args.json \
  --out /tmp/mm_install/manifest-esp32.json

# Serve from a real origin — file:// breaks Web Serial.
cd /tmp/mm_install && python -m http.server 8000
# open http://localhost:8000/
```

### End-to-end with CI-built firmware (no tag, no release)

The closest you can get without tagging. Pulls the latest branch CI run's
artifacts (the 4 firmware bundles produced by `release.yml`'s `build-esp32`
matrix), serves both the `.bin` files and a manifest pointing at them from a
local server, then exercises the real install button.

What this catches that "page-render only" doesn't: manifest schema errors,
ESP Web Tools format quirks, real Web Serial flash against a *real* binary
on every board. What it doesn't catch: the GitHub Pages deploy itself and
the `action-gh-release` upload (only the RC-tag flow catches those).

```bash
# Pick the most recent successful release.yml run on this branch.
RUN_ID=$(gh run list --workflow=release.yml --branch=$(git branch --show-current) \
  --status=success --limit=1 --json databaseId --jq '.[0].databaseId')
[ -z "$RUN_ID" ] && { echo "no successful CI run on this branch"; exit 1; }
echo "Using run $RUN_ID"

# Stage a local Pages root.
DIST=/tmp/mm_install_ci
rm -rf "$DIST"
mkdir -p "$DIST"

# Download the 4 board artefacts. Each artefact is a folder containing the
# four .bin files + flasher-<board>.json for that board.
for B in esp32 esp32-eth esp32-eth-wifi esp32s3-n16r8; do
  gh run download "$RUN_ID" -n "esp32-$B" -D "$DIST/_$B"
done

# Flatten the .bin and flasher_args.json files alongside index.html.
mv "$DIST"/_*/* "$DIST"/
rmdir "$DIST"/_*
cp docs/install/index.html "$DIST"/

# Regenerate manifests pointing at the local server.
for B in esp32 esp32-eth esp32-eth-wifi esp32s3-n16r8; do
  python scripts/build/generate_manifest.py \
    --board "$B" --version 0.1.0 \
    --release-url "http://localhost:8000" \
    --flasher-args "$DIST/flasher-$B.json" \
    --out "$DIST/manifest-$B.json"
done

# Serve. Web Serial needs HTTPS *or* localhost; localhost is fine.
cd "$DIST" && python -m http.server 8000
# open http://localhost:8000/
```

Tips:

- `gh run list` only sees runs you've already pushed; commit + push, wait for
  the run, then start the recipe.
- The `.bin` filenames in the artefact use the pattern
  `firmware-<board>-v<version>.bin`. `generate_manifest.py` writes paths that
  match this — if you change the naming, regenerate manifests.
- After flashing, the device boots normally — the SoftAP/Eth path doesn't know
  whether the firmware came from a release or a local server.

### Full release dry-run (RC tag)

When the local recipes pass and you want to validate the GitHub-side flow,
tag a release-candidate (see [`docs/history/plan-17.md`](../history/plan-17.md)
§ Step 10). RC tags are auto-flagged as pre-releases by the workflow, and
the Pages deploy step is skipped — the live installer URL only flips on a
stable `vX.Y.Z` tag, so an RC dry-run never publishes itself to end users.

ESP Web Tools requires Chrome / Edge / Opera on desktop. Firefox and Safari
don't ship Web Serial.

## Deployment

`release.yml` does this automatically on a `v*` tag:

1. `build-esp32` matrix produces four firmware bundles + four
   `flasher_args.json` files (one per board).
2. The `release` job runs `generate_manifest.py` four times, writing
   `manifest-<board>.json` into a Pages-staging directory next to a copy of
   `index.html`.
3. `actions/deploy-pages` publishes that directory to GitHub Pages.

Manual setup, one-time per repo: **Settings → Pages → Source: GitHub Actions**.
No deploy-from-branch — the workflow is the only producer.
