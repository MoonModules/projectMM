# projectMM web installer

This directory holds the source for the **custom installer page** (driven by
`install-orchestrator.js`, not ESP Web Tools) at
<https://moonmodules.org/projectMM/install/>.

End users land here, pick a channel + board, click Install. The browser flashes
the device over USB (Web Serial → ESP32), then runs Improv-Serial provisioning,
SET_BOARD, and HTTP control fan-out — all from the same orchestrator,
end-to-end, no ESP Web Tools dependency on the install path.

## What's in this directory

- [`index.html`](index.html) — the installer page. Imports the shared
  [`install-picker.js`](../../src/ui/install-picker.js) module and rewrites
  the picker's GitHub-release URLs to same-origin Pages URLs before handing
  them to the custom orchestrator (Web Serial is CORS-bound).
- [`install-orchestrator.js`](install-orchestrator.js) — owns the
  SerialPort across flash → reboot → Improv provision → SET_BOARD RPC.
  Replaces the ESP Web Tools install button so the post-provision board
  push works (EWT 10.x's `state-changed` event fires inside a shadow DOM
  that's invisible to the host page; the orchestrator side-steps that by
  owning the whole flow). Falls through to a "device IP" prompt when the
  device doesn't speak Improv back — see [BoardModule.md → Web installer
  HTTP fallback](../moonmodules/core/BoardModule.md#web-installer--http-fallback-via-visit-board).
- [`devices.js`](devices.js) — the *Your devices* list. Stores devices the
  user provisioned from this page so they can re-visit / erase / forget
  them. Renders a dedicated *Inject* button next to Visit for every entry
  with a `board` field; the button opens `<device>/?board=<name>` and the
  device UI fetches the matching `boards.json` entry from Pages to add its
  `modules` via `/api/modules`, then fan out each `controls.*` field via
  `/api/control` (add-then-configure; see the schema below). Idempotent — safe
  to re-click after a popup-blocker rejection or a follow-up catalog edit.
- [`boards.json`](boards.json) — the board catalog (name → firmware
  variants + the modules/controls to inject) the picker fetches and the
  installer / device-UI / MoonDeck injectors write from. Schema below.
- [`favicon.png`](favicon.png) — moon-man, same as the device UI.
- [`README.md`](README.md) — this file.

## Catalog schema (`boards.json`)

A flat JSON array of catalog entries. Each entry is the single source of truth
for one piece of hardware and what to set up on it at install time. Three
clients consume it identically — the web installer (`install-orchestrator.js`),
the device UI's `?board=` inject (`src/ui/app.js`), and MoonDeck
(`scripts/moondeck.py`) — so **adding a field that's just more
modules/controls needs no client change**.

```json
{
  "name": "projectMM testbench S3",
  "chip": "ESP32-S3",
  "firmwares": ["esp32s3-n16r8"],
  "default_firmware": "esp32s3-n16r8",
  "modules": [
    { "type": "AudioModule", "id": "Audio", "parent_id": "System" }
  ],
  "controls": {
    "Board": { "board": "projectMM testbench S3" },
    "Audio": { "wsPin": 4, "sdPin": 5, "sckPin": 6 },
    "RmtLed": { "pins": "12", "loopbackRxPin": 13 }
  }
}
```

The `RmtLed` block presets the LED-driver loopback self-test pins (`pins[0]` is
the TX the test transmits on, `loopbackRxPin` the jumpered RX) so a bench
operator just flips the `loopbackTest` switch — no need to retype pins. The
driver is boot-wired, so this is a plain control set (no `modules` entry).
`loopbackTest` is left off; presetting the pins doesn't auto-run the (blocking)
test. The sibling `projectMM testbench ESP32-16MB` / `…P4` entries carry only the
`RmtLed` loopback (no mic — only the S3 bench has one wired), each injecting only
what is actually on that board.

| Field | Required | Meaning |
|---|---|---|
| `name` | yes | identifier **and** display label (no key/label split) |
| `chip` | yes | the MCU family, for the picker's chip filter |
| `firmwares` / `default_firmware` | yes | the firmware variants flashable on this hardware |
| `modules` | no | modules to **create** before configuring (`POST /api/modules` payloads) |
| `controls` | no | controls to **set** after the modules exist (`POST /api/control`) |

**Add-then-configure.** Clients process `modules` **before** `controls`. A
fresh flash has no user-added modules (an `AudioModule`, an extra effect), so a
control write to one would 404 — the module must be created first.
`POST /api/modules` is idempotent (an existing `id` returns 200), so re-running
an inject is safe. Each `modules` entry needs an explicit **`id`** if its
controls are then set, because factory display names collide and get
disambiguated; address the controls by that same `id`.

**Injection is opportunistic and partial.** Both `modules` and `controls` are
optional — the catalog injects **only what is known and hardware-fixed**
(vendor-soldered mic pins, board-fixed Ethernet pins). A bare board whose LED
or mic pins the *user* wires omits them; the user adds the module and sets the
pins manually later. Absent array ⇒ inject nothing. (This is the
MCU/Board/Device provenance rule from
[`docs/backlog/installer-3layer-plan.md`](../backlog/installer-3layer-plan.md):
default a pin only at the level that fixes it.) The `projectMM testbench S3`
entry above injects an `AudioModule` with the real, verified INMP441 mic pins
(WS=4/SD=5/SCK=6, matching the bench wiring in
[`AudioModule.h`](../../src/core/AudioModule.h)) plus the `RmtLed` loopback pins —
a known-hardware Device on the maintainer's desk, so the inject is testable
end-to-end. The `ESP32-16MB` and `P4` testbench siblings inject only the `RmtLed`
loopback (no mic wired on those), each carrying only what is physically present.

**Specific boards/devices: spec 'n test first.** A real product entry only grows
a `modules`/pin block once that hardware has its own spec (with the product-page
link + grabbed images for installer selection and pin layout) and a test pinning
it — the project's *Specs before code* applied to catalog hardware. So vendor
entries (the QuinLED Dig-2-Go, the Serg shields, …) stay **bare** (`Board.board`
only) until spec'n'tested; e.g. whether the Dig-2-Go's *onboard* mic is even
supported is an open spec'n'test question, so its entry injects nothing.

**One entry type, no Board/Device split.** A "Device" (a finished rig like the
`projectMM testbench S3` — board + a wired mic) is just an entry with *more* of
`modules`/`controls` filled in than a bare "Board". Same schema; there is no
separate `devices.json`.

**`extends` (reserved, not yet resolved).** The carrier/shield pattern is
literal extension (a Serg shield = a D1-Mini32 board *plus* the shield's pins),
so a future optional **`extends: "<parent entry name>"`** lets an entry inherit
another's `modules`/`controls` (multi-level MCU→carrier→device; child overrides
parent at the same `{module, control}`; `modules` concatenate, deduped by `id`).
The resolver is a client-side pre-pass to be built at the first real shared-base
collision — today every entry is self-contained, so it isn't implemented yet.
Don't author duplicated pin blocks expecting `extends` to dedupe them until it
ships.

**Shared op vocabulary.** A catalog entry is a test
[scenario](../../test/scenarios/) minus the `measure` asserts — the same two
operations: `add_module {type, id, parent_id}` (== `POST /api/modules`) and
set-control (== `POST /api/control`, body `{module, control, value}` — the
canonical field names; scenario's internal `set_control` uses `{id, key, value}`
for the same fields). `scripts/scenario/run_live_scenario.py` already runs these
ops over HTTP against a live device, the same channel the installer uses.

## What's *not* in this directory

- **The install-picker module** (`install-picker.js`) lives at
  [`src/ui/install-picker.js`](../../src/ui/install-picker.js) because the
  same file is also embedded into the device firmware UI (see
  [`docs/moonmodules/core/FirmwareUpdateModule.md`](../moonmodules/core/FirmwareUpdateModule.md)).
  The release workflow copies it into the Pages root next to `index.html`
  on every deploy.

- **Per-release binaries and manifests.** The release workflow keeps the last
  five stable + five prerelease releases under `releases/<tag>/` on Pages,
  and the install page's `toLocalUrl()` rewrites the picker's
  `releases/download/<tag>/<file>` URLs to `./releases/<tag>/<file>`.
  Self-hosting is necessary because GitHub release-asset URLs don't return
  CORS headers, so the browser can't fetch them cross-origin during a Web
  Serial flash. The on-device OTA path doesn't have this problem and reads
  the assets directly from GitHub.

## Local preview

Three recipes, in increasing fidelity to production. Each runs the picker
against the real GitHub Releases API (CORS-friendly) but differs in whether
the install button can actually flash.

### Render-only (no flash)

Quickest. In MoonDeck: **PC tab → Preview Installer**. Or from the CLI:

```bash
uv run scripts/run/preview_installer.py
# open http://localhost:8000/ in Chrome / Edge / Opera
```

Both forms stage `index.html` + `install-picker.js` into
`build/install-preview/` and serve them. Picker populates, dropdowns
work, but clicking Install fails because the local server has no
`releases/` tree. Useful when iterating on the install page's
HTML/CSS/JS — the `?nocache=1` query parameter forces the picker to
bypass its 5-minute sessionStorage cache while you edit.

### End-to-end with CI-built firmware

The closest you can get without tagging. Pulls the latest branch CI run's
artifacts (the 4 firmware bundles produced by `release.yml`'s `build-esp32`
matrix), stages them under `releases/v<branch-tag>/` so the install page's
`toLocalUrl()` can find them.

```bash
DIST=/tmp/mm_install_ci
rm -rf "$DIST"; mkdir -p "$DIST/releases"

# Use the version from library.json as the pretend tag.
V=$(jq -r .version library.json)
TAG="v$V"
mkdir -p "$DIST/releases/$TAG"

# Pick the most recent successful release.yml run on this branch.
RUN_ID=$(gh run list --workflow=release.yml --branch=$(git branch --show-current) \
  --status=success --limit=1 --json databaseId --jq '.[0].databaseId')
[ -z "$RUN_ID" ] && { echo "no successful CI run on this branch"; exit 1; }
echo "Using run $RUN_ID"

# Download the 4 firmware artefacts, flatten, regenerate Pages-relative manifests.
for F in esp32 esp32-eth esp32-eth-wifi esp32s3-n16r8; do
  TMP=$(mktemp -d)
  gh run download "$RUN_ID" -n "esp32-$F" -D "$TMP"
  cp "$TMP"/*.bin "$DIST/releases/$TAG/"
  python3 scripts/build/generate_manifest.py \
    --firmware "$F" --version "$V" \
    --release-url . \
    --flasher-args "$TMP/flasher-$F.json" \
    --out "$DIST/releases/$TAG/manifest-$F.json"
  rm -rf "$TMP"
done

# Drop the install page + shared picker module in place.
cp docs/install/index.html "$DIST"/
cp src/ui/install-picker.js "$DIST"/

cd "$DIST" && python3 -m http.server 8000
# open http://localhost:8000/
```

Caveats:

- The picker fetches releases from api.github.com (the real list), so it
  shows every published release. Only the one you staged locally
  (`releases/$TAG/`) flashes — others 404. Switch the picker to "Pick
  specific release" and choose the local tag to avoid this.
- 60 API requests/hour anonymous rate limit; sessionStorage caches for 5
  minutes so dev iteration stays well under.

### Full release dry-run (RC tag)

When the local recipes pass, tag a release-candidate. Per the plan-18
design, RC tags now also publish to Pages (no `-rc` gate) so beta testers
can self-serve from the dropdown. End users default to Stable, so the RC
release is visible only to those who opt into the Pre-release channel.

```bash
# Bump library.json to "1.0.0-rcN", commit, tag v1.0.0-rcN, push tag.
# Workflow:
#   - 4 ESP32 builds + macOS build + Windows build run.
#   - release job stages cumulative content (last 5 stable + 5 prerelease)
#     under pages/install/releases/<tag>/ on Pages.
#   - Publishes a GitHub Release flagged "Pre-release".
#   - Deploys Pages with the new release available immediately.
```

Web Serial requires Chrome, Edge, or Opera on desktop. Firefox and Safari
don't ship the API.

## Deployment

`release.yml` does this automatically on every `v*` tag (stable + RC):

1. `build-esp32` matrix produces four firmware bundles + four
   `flasher_args.json` files. `build-macos` produces the macOS tarball;
   `build-windows` produces the Windows zip.
2. The `release` job:
   - Generates manifests in two flavours: absolute URLs (for GitHub release
     assets, used by the OTA picker) and relative URLs (for the Pages copy,
     used by the web installer).
   - Pulls the last 5 stable + 5 prerelease releases' assets via
     `gh release download` and stages them under
     `pages/install/releases/<tag>/`.
   - Publishes the GitHub Release (binaries + absolute manifests as assets).
   - Deploys Pages with `index.html` + `install-picker.js` + the staged
     `releases/` tree.

Manual setup, one-time per repo: **Settings → Pages → Source: GitHub Actions**.

No deploy-from-branch — the workflow is the only producer. A separate
`docs/install/`-only Pages deploy was considered and rejected: it would
have to re-run the same cumulative-content dance, so a docs-only deploy
buys nothing.
