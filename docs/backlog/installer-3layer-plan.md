# Installer / 3-layer device model — initiative plan

A living plan that ties together several related threads into one buildable
sequence: the **MCU → Board → Device** config-provenance model, the installer
catalogs and picture-based picker that expose it, the device-side mechanism that
*injects* per-level pins during install, shared installer code across the three
clients (web installer, MoonDeck, on-board OTA), the classic-ESP32 **firmware
variant collapse**, and the **partition-table dedup**. Each is its own increment;
this doc is the map and the running status, not a spec — promote detail into the
relevant section / module spec as each increment is picked up.

This is **forward-looking** (it belongs in `backlog/`, not `history/`). It does
not duplicate the existing backlog entries it depends on; it links them and
records the cross-cutting sequencing. The single hard sequencing rule from those
entries governs everything below:

> **A catalog field only earns its keep once a device-side consumer reads it.**
> A first board catalog was built and rolled back once for exactly this reason
> ([backlog § Runtime board presets](backlog.md#runtime-board-presets-multi-commit-partially-landed),
> line "A first attempt at this catalog landed and was rolled back…"). So data
> (firmwares.json pin fields, devices.json, MCU pin maps) never lands before the
> consumer that reads it.

## The model: MCU → Board → Device (config provenance)

A first-class hierarchy for *where a pin default legitimately comes from*,
surfaced through the installer and MoonDeck so a user picks their hardware
instead of hand-typing every GPIO. Three levels, each a layer that may fix some
settings:

- **MCU → firmware.** The chip (esp32 classic / S3 / P4). Fixes the silicon-wired
  pins: the RMII **Ethernet** pin map, native-radio presence, PSRAM. These are
  chip-facts — already today's compile-time `platform::ethPins` / `hasI2sMic` /
  `hasWiFi` constants in `platform_config.h`. The firmware variant IS the MCU
  choice. (On the S3, which has no EMAC, Ethernet is an external SPI PHY — also
  runtime-selected, not a separate firmware; see workstream 7.)
- **Board → MCU on a PCB.** e.g. Wemos D1-mini, **Waveshare ESP32-P4-Nano**
  (https://www.waveshare.com/esp32-p4-nano.htm). Fixes board-soldered pins: the
  onboard status LED, the board's own Ethernet PHY pins, the C6 co-processor SDIO
  pins, button pins. A board is a named bundle of "these GPIOs are already
  committed by the PCB." Carry a board catalog (id, name, image, **product link
  next to the image**, the pins it fixes).
- **Device → board + peripherals.** board + what the *user* soldered on: the mic,
  the LED strands, a 4↔5 rxtx loopback jumper. These are **user choices** — the
  same board yields different devices. A device is a saved profile of Device-level
  pin assignments the user confirmed once. They default to **unset** — any guess
  can drive a pin the user wired elsewhere.

**The provenance rule applies to *settings*, not just pins — `txPowerSetting` is
the worked example.** WiFi TX power is a Device-level setting, not MCU or Board:
the radio chip can do the full 21 dBm (an MCU fact), but whether the assembled
rig can *sustain* the current spike of full-power TX depends on the board's power
regulation **and how the user powers it** (USB port, PSU, cable) — a brownout
property of the whole device, not the silicon. Confirmed in-tree:
[`boards.json`](../install/boards.json) injects `"Network": { "txPowerSetting": 8 }`
for the **LOLIN S3 N16R8** specifically (the chip's default is 21), because that
board browns out at full power on typical USB supply — a per-board floor the user
may still need to lower further on a weak supply. So a board profile *suggests* a
safe `txPowerSetting` (like it suggests board-fixed pins), and the device profile
can override it for the actual power source. The general lesson: any setting whose
safe value depends on the physical assembly or its power, not on the chip,
defaults at the Device level.

**Why this is the right model — it's the runtime form of the pin-default rule.**
The rule "**assign defaults only when they cannot do harm**"
([decisions.md](../history/decisions.md)) is exactly "**a pin may be defaulted
only at the level that actually fixes it.**" MCU- and Board-level pins are
hardware-fixed → a default cannot do harm (and *omitting* it harms: a no-WiFi
board with un-defaulted Ethernet pins can never connect). Device-level pins are
user-soldered → any default is a guess that *can* drive a pin the user wired
elsewhere → must stay empty until set. So the empty Device-level defaults shipping
now (AudioModule mic pins, the LED-driver pins) are the **correct baseline a
device profile layers onto** — a profile *fills* blanks from confirmed hardware,
it doesn't *override* a wrong guess. Had we kept bench defaults, every profile
would start by undoing a guess.

Prior art to study (not copy): MoonLight's per-board pin database
([ModuleIO.h](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Modules/ModuleIO.h))
already models ~25 boards across D0/S3/P4 with a `pins[]` array + board-level
`maxPower`/`ethernetType`/PHY config.

### Worked classification: where real products land (QuinLED / Serg)

The deciding question is **not** the product name — it's *"does this PCB fix a
set of GPIOs by its physical wiring (→ **Board**), or is it a complete rig the
user has finished assembling with their own peripherals (→ **Device**)?"* The
**carrier / shield pattern** is the tell: a PCB an ESP32 *module* plugs into is a
Board (it fixes the LED/relay/button pins; the MCU and the user's strips are
chosen separately). A vendor-finished all-in-one with peripherals already soldered
is a Device.

| Product | ESP32 mounting | Layer | Why |
|---|---|---|---|
| **[QuinLED Dig-2-Go](https://quinled.info/quinled-dig2go/)** | soldered, all-in-one; **built-in MEMS mic + IR**, 1 output | **Device** | Self-contained — nothing left to wire. Fixed pins *and* a fixed peripheral (the mic). The vendor already turned a board into a device. |
| **[QuinLED Dig-Next-2](https://quinled.info/dig-next-2/)** | soldered ESP32-PICO (8 MB flash / 2 MB PSRAM), all-in-one in a case; **built-in mic**, 2 outputs | **Device** | Same as Dig-2-Go: complete standalone rig, vendor-fixed mic. Its mic pins *can* be defaulted (vendor fixed them), the degenerate vendor=user case where a Device carries a peripheral default. |
| **[QuinLED Dig-Uno V3](https://quinled.info/pre-assembled-quinled-dig-uno/)** | plug-in QuinLED-ESP32 top board (headers), 2 outputs | **Board** | Carrier PCB fixes the GPIOs; user picks the ESP32 module + wires strips. |
| **[QuinLED Dig-Quad V3](https://quinled.info/pre-assembled-quinled-dig-quad/)** | plug-in top board (headers), 4 outputs | **Board** | Same carrier pattern, more outputs. |
| **[QuinLED Dig-Octa Brainboard 32-8L](https://quinled.info/quinled-dig-octa-brainboard-32-8l/)** | ESP32 soldered on the **brainboard**; brainboard stacks onto powerboards, 8 outputs | **Board** = the brainboard | "Brainboard + carrier": the brainboard fixes the 8-output pinmap; the powerboard underneath is power distribution, **no GPIO provenance**, so it's outside the model. |
| **[Serg shields](https://www.tindie.com/stores/serg74/)** (mini / full WLED shield) | shield — a D1-Mini32 plugs in | **Board** | A shield is the textbook carrier: fixes pins, ESP32 separate. |
| **Serg pico / D1-Mini32 WLED board** | ESP32 integrated, bare | **Board** | Integrated but bare — the user still wires their own strips, so it's a Board to assemble into a Device, not a finished Device. |
| **LightCrafter 16** | ESP32-S3 (N8R8) integrated, 16 outputs | **Board** + optional peripherals | A 16-output S3 board with *optional* populated peripherals (W5500 Ethernet, IR, power-measurement). See "optional on-board peripherals" below. |
| **SE 16 V1** | ESP32-S3 (N8R8) integrated, 16 outputs | **Board** + optional peripherals | Same shape as LightCrafter 16 — Board with the same optional-peripheral set. |

Today's [boards.json](../install/boards.json) lists all the QuinLED entries flatly
under one schema; the model above is what splits them into the Board vs Device
catalogs once those exist (workstream 4). Note the two all-in-one Devices
(Dig-2-Go, Dig-Next-2) carry **vendor-fixed mic pins** — a Device peripheral that
*can* be defaulted because the vendor soldered it, unlike a bare board's mic pins
which stay unset.

**The carrier/shield pattern sharpens the MCU level, it doesn't break it.** A Serg
shield or a Dig-Uno carrier hosts *different* ESP32 modules (WROOM / WROVER /
Ethernet top board), so **one Board pairs with several MCUs**. That's the model
working: Board fixes its own pins, MCU choice is separate, **Device = Board + MCU +
user peripherals**. The all-in-one boards (Dig-2-Go) are the degenerate case where
vendor = user: Board, MCU and the soldered peripherals collapse into one Device
entry.

### Optional on-board peripherals — a third pin category

LightCrafter 16 and SE 16 V1 force a refinement the QuinLED set didn't: a Board
can carry **optional populated peripherals** — W5500 SPI Ethernet, an IR receiver,
power-measurement chips (e.g. INA-class). These are on the PCB, board-fixed *when
populated*, but absent on the un-populated variant of the *same board name*. So a
Board's pins fall into three categories, not two:

1. **Board-fixed always** — LED outputs, status LED, buttons. The PCB commits them
   on every unit. A default cannot do harm → default it.
2. **Board-optional** — the W5500 / IR / power-monitor pins. Board-fixed *if the
   chip is populated*, otherwise the GPIOs are free. You **can't default these
   blind**, because the same board ships with and without — defaulting the W5500
   CS/IRQ pins on a unit that doesn't have the chip would reserve GPIOs the user
   may want for LEDs. So a board profile carries them as an **opt-in peripheral
   block** the user (or a detected-present check) enables, not an always-on
   default.
3. **Device / user-soldered** — as before, always unset.

The W5500 case is not a new mechanism — it **is workstream 7's runtime SPI PHY**.
The backlog already says the S3 (no internal EMAC) gets Ethernet via a
runtime-selected W5500 SPI PHY, no separate firmware
([backlog § Release 2.0](backlog.md#release-20--distribution-catches-up-to-the-source-tree)).
"SE 16 with the W5500 populated" is exactly that: the board profile *offers* the
W5500 pinset (MISO/MOSI/SCK/CS/IRQ), and runtime detection/config decides whether
Ethernet comes up over it. IR and power-measurement are the same pattern for their
own future consumers (an IR-input module, a power-telemetry module) — each gets a
board-optional pin block only once its device-side consumer exists, per the hard
sequencing rule. Until then, don't add the block.

This keeps the catalog honest: the **always-fixed** pins default freely, the
**optional** ones are a named peripheral the user opts into, and nothing
speculative lands before its consumer.

## Deployment model — what goes live when, and the merge-goes-live trap

A cross-cutting constraint that shapes every workstream below: **the "installer"
is three clients that deploy very differently, and they are *not* uniformly
branch-isolated.** Doing this work on a branch does **not** uniformly mean "old
system until merge."

| Client | Where it lives | Branch isolation | Goes live when |
|---|---|---|---|
| **On-device OTA picker** | embedded *in the firmware binary* (`install-picker.js` via `embed_ui.cmake`) | ✅ full — a device only has the new picker if flashed/OTA'd with branch firmware | when a device is flashed from the branch |
| **Device `/api/preset` endpoint** (workstream 3) | firmware (`HttpServerModule.cpp`) | ✅ full — old firmware returns 404 for the new route | when a device runs branch firmware |
| **MoonDeck** | local repo checkout (`uv run scripts/moondeck.py`) | ✅ full — whoever's on the branch gets the new one | per-checkout |
| **Public web installer** | **GitHub Pages**, served from `main` | ⚠️ **partial** — see below | **the instant the branch merges to `main`** |

**The web-installer trap.** The Pages deploy job is gated
`if: github.ref == 'refs/heads/main'`
([release.yml § deploy-pages](../../.github/workflows/release.yml)). So while you
work on a branch, the live public installer keeps serving **current `main`** —
your `docs/install/*` changes never reach the public URL. But the **moment the
branch merges**, the next `main` push redeploys the public installer with the new
system for **all** end users immediately — **there is no version gate on the
installer page/logic itself, no staging, no intermediate.** Merge *is* the deploy.
(Only the firmware *binaries* the installer flashes are versioned, per-tag +
`latest`; the installer **UI/logic** is always whatever's on `main`.)

Two consequences to design around:

1. **You can't test the new web installer on the real Pages URL without merging.**
   Test it **locally** (`scripts/run/preview_installer.py` — stages
   `docs/install/` + `install-picker.js` into `build/install-preview/` and serves
   on `http://localhost:8000`; flash-ready when a `build/esp32-*/projectMM.bin`
   exists, since Web Serial works on `localhost` without the secure-origin gate)
   during the branch. Plan for a local verification step, not a "push and check
   the live site" loop. For a *live* branch installer without a fork or a second
   deploy target, see the **`install-alt/` build-beside-swap** approach below.
2. **Backward compatibility is a hard requirement at merge, not a nicety.**
   Because the installer page updates **independently of the binaries it flashes**,
   the moment it goes live it must still work against **already-released firmware**
   that predates this work — firmware with no `/api/preset` endpoint and no new
   `boards.json` fields. So: the new web installer must degrade gracefully when it
   talks to old firmware (feature-detect the preset endpoint, treat new catalog
   fields as optional, never hard-require a capability only branch firmware has).
   **Bake this into the workstream 3 / 4 / 5 specs explicitly** — the injection
   path and the catalog schema both need an "old firmware / old catalog still
   works" branch.

### `install-alt/` — build the new installer beside the old, swap at the end

The chosen way to get a **live** branch installer **without a fork and without a
second deploy target** — staying entirely in this repo on the normal `main`
deploy path. The Pages site already stages `docs/install/` → `pages/install/`
([release.yml § Stage GitHub Pages site](../../.github/workflows/release.yml));
the only single-artifact / `main`-only constraint is on *deploying*, not on *what's
in the artifact*. So:

- Add a **`docs/install-alt/`** folder (the new installer) + **one extra `cp` line**
  in the staging step (`cp -r docs/install-alt/. pages/install-alt/`). Now the one
  Pages artifact carries **both**: `…/projectMM/install/` (stable, untouched) and
  `…/projectMM/install-alt/` (the new branch version). Same deploy, same artifact,
  no race, no fork.
- **The honest catch:** Pages only deploys `main`, so for `-alt` to be *live* its
  files must be **on `main`**. The workflow: merge the **inert** `install-alt/`
  folder to `main` early — it's safe because `/install/` (what everyone uses) is
  unchanged, and `/install-alt/` is **unlinked** (only people handed the URL reach
  it). Iterate on `install-alt/` across the branch.
- **The swap is the finish line:** when the new installer is proven, one commit
  replaces `install/` contents with `install-alt/` and deletes `install-alt/` (and
  its `cp` line). That single commit flips every end user over, fully tested — the
  controlled cutover the "merge = instant live, no staging" trap otherwise denies
  us.

This turns the merge-goes-live trap into a **build-beside / swap-at-end** flow:
the stable installer never changes mid-branch, the live `-alt` URL is the shared
test ground, and the cutover is one reviewed commit instead of a surprise on
merge. (Local `preview_installer.py` is still the fast inner loop; `-alt` is the
live shareable check before the swap.)

**The rule — what goes where:** make as many **non-breaking, backward-compatible**
changes as possible directly in the **current `docs/install/`** folder; only put
**breaking / behaviour-changing** new work in `install-alt/`. A change is
"non-breaking" when it's a no-op for every existing catalog entry and end users on
old/released firmware see no difference. *Worked example — the catalog `modules`
provisioning increment* (workstream 3): edited `install-orchestrator.js`/`boards.json`
**in place**, because the `modules` pass is guarded `entry.modules ?? []` (a no-op
for all 19 existing entries) and only the Dig-2-Go entry gains an inert unset-pin
block — nothing changes for anyone on merge. The *picture-based installer UI rework*
(workstreams 4/5), which redesigns the visible page, is the kind of breaking change
that belongs in `install-alt/` until the swap.

## Workstreams and status

### 1. Partition-table dedup — DONE (uncommitted)

Three byte-identical 16 MB partition CSVs (`esp32_16mb.csv`, `esp32s3_n16r8.csv`,
`esp32p4_16mb.csv`) collapsed into one chip-neutral
[esp32/partitions/ota_16mb.csv](../../esp32/partitions/ota_16mb.csv); the three
sdkconfig fragments (`.16mb`, `.esp32s3-n16r8`, `.esp32p4-eth`) repointed at it.
A partition table is a flash-offset map and 16 MB is 16 MB on any of these SoCs,
so one layout serves all three. `esp32dev.csv` (4 MB) and `esp32s3_n8r8.csv`
(8 MB) are genuinely different and untouched.

- Net −2 files. Future 16 MB layout tweaks edit one file, not three (where two
  would silently diverge).
- Touches `esp32/` → a commit triggers the ESP32 build gate. Not yet committed.

### 2. firmwares.json — GENERATE, don't hand-author

The firmware list is the **`FIRMWARES` dict** in
[scripts/build/build_esp32.py](../../scripts/build/build_esp32.py) (line ~60) —
the single source of truth: it alone knows what a variant *is* (chip + sdkconfig
fragments + eth-only flag + description), validates `--firmware`, and burns the
key into the binary via `-DMM_FIRMWARE_NAME`. The list is currently duplicated
**4×**: the dict + three hand-copied lists in
[.github/workflows/release.yml](../../.github/workflows/release.yml) (the CI
matrix ~line 75, the release manifest loop ~241, the Pages manifest loop ~352).

Decision: **emit `firmwares.json` from the `FIRMWARES` dict at release time**
(the same step that builds the ESP Web Tools manifests), not a 5th hand-kept
copy. The same generator can emit the matrix JSON and collapse #2–4 too — one
source, zero drift.

- **Caveat for the generator:** `FIRMWARES` has 8 entries but only 7 *ship*
  (`esp32p4-eth-wifi` is deliberately out of the CI matrix — it doesn't build
  reproducibly yet, see [backlog § ESP32-P4 rounds 3-4](backlog.md#esp32-p4-support--rounds-3-4-in-progress)).
  So the generator needs a per-entry "shipping" flag, not just "exists."

### 3. Catalog-driven module provisioning — DONE (uncommitted)

**Reframed and implemented.** The original framing ("a device-side pin-preset
*consumer*", a `POST /api/preset` batch endpoint) was overturned by investigation:
the device side already accepts injected controls — three clients
(`install-orchestrator.js`, `src/ui/app.js`, `scripts/moondeck.py`) already fan a
catalog entry's `controls` map out as `POST /api/control` calls. A batch endpoint
would duplicate that and need a JSON-array parser `JsonUtil.h` forbids. **The real
gap** was that the fan-out can't configure a module that *doesn't exist* on a fresh
flash (a control write 404s) — solved by the **already-existing, idempotent
`POST /api/modules`**, which the install clients simply weren't driving.

What landed (no firmware change — both endpoints already existed and are generic):

- **Catalog schema** gained an optional **`modules: [{type, id, parent_id}]`** array
  beside `controls`, processed **add-then-configure** (modules first, then controls).
  Documented in [docs/install/README.md § Catalog schema](../install/README.md).
- **All three clients** gained a `modules` pre-loop before their controls fan-out
  (`entry.modules ?? []` — a no-op for every existing entry, so non-breaking; edited
  in place per the install-alt rule above).
- **Worked examples — three `projectMM testbench` entries** (real boards on the
  maintainer's desk): the **S3** adds `AudioModule` + its real verified mic pins
  (WS=4/SD=5/SCK=6) **and** presets the `RmtLed` loopback pins (`pins`=12, rx=13);
  the **ESP32-16MB** and **P4** siblings preset only the `RmtLed` loopback (4/5 and
  32/33 — no mic wired on those). Each injects only what's physically present, so
  the injects are testable end-to-end on hardware. The LED loopback presets the
  *existing* `pins`/`loopbackRxPin` controls (a considered `loopbackTxPin` control
  was dropped — it only fit the RMT single-pin loopback, not the Parlio/Lcd
  real-frame lane-array loopback; `pins[0]` is already the tx, uniform across all
  three drivers). Vendor entries (Dig-2-Go etc.) stay **bare** until spec'n'tested.
- **Scenario test**: `scenario_Audio_mutation.json` extended to the full three-pin
  install sequence (add module → wsPin → sdPin → sckPin), proving the self-correcting
  partial-fill keeps the pipeline rendering.

Resolved decisions recorded: **no batch endpoint**; **one entry type** (no
`devices.json` — a Device is a Board entry with more filled in); **`extends`
hierarchy reserved, resolver deferred** to the first shared-base collision;
**inject > buried-in-code** (vs MoonLight `ModuleIO.h`); **opportunistic/partial
injection** (only hardware-fixed values, user-soldered pins stay unset); **shared op
vocabulary** with scenarios (canonical field names = the API's `{module, control,
value}`). Generalises to every case raised: mic, gyro (future), ArtNet
(`NetworkSendDriver`), "starts with the Lines effect" (`LinesEffect` under `Layer`).

Still pairs with [backlog § Runtime board presets](backlog.md#runtime-board-presets-multi-commit-partially-landed)
(the `/api/system/board-preset` route) for the device re-applying a preset with no
host attached — a different locus, not needed for the install-time path.

### 4. Catalog + picture model (devices.json, MCU layer, annotated images)

The data model made real: a `devices.json` catalog and the MCU layer alongside
the shipped [boards.json](../install/boards.json), each carrying per-level pins
and **images**. Assets started in [docs/assets/boards/](../assets/boards/)
(esp32-d0-16mb, esp32-p4-nano, esp32-s3-n16r8v, P4-NANO-details).

Decision on picture depth: **annotated pins on images** — board *and* device
images (e.g. INMP441 with WS/SD/SCK marked) so a user can read wiring off the
picture. Needs a consistent annotation approach. Wiring diagrams
(device-pin → board-GPIO) are a later, deeper increment.

**Image sourcing — host our own, don't hotlink.** The product pages carry a
`url`/`product_link` field in the catalog anyway (the "product link next to the
image" the model already specifies), and those pages *have* good photos
(quinled.info, the Serg Tindie store, the Waveshare wiki). It's tempting to point
the picker straight at the vendor image URL, but **hotlinking is the wrong default**
for three reasons: (1) the picker would break whenever a vendor reshuffles their
CDN — the installer must keep working offline / same-origin, the same constraint
that drove [bundling firmware manifests into Pages](../../.github/workflows/release.yml);
(2) we'd want pin-annotated overlays anyway, which means a *derived* image, not the
raw vendor shot; (3) third-party product photos are the vendor's copyright —
redistributing them needs permission. So the workflow is: use the catalog's
product link to *find* the reference photo, then either shoot/redraw our own (the
[docs/assets/boards/](../assets/boards/) photos are this) **or** get explicit
permission and check a local copy into `docs/assets/`, annotate it, and serve it
same-origin. The catalog stores a **local asset path + a product link**, never a
remote image URL. (A one-off `scripts/` helper that fetches a vendor page and
pulls candidate image URLs to *seed* the manual step is fine — it assists sourcing,
it doesn't put a live remote URL in the shipped catalog.)

MoonDeck gains a **device-profile** concept: save/restore the Device-level pin
set for a named device, so re-flashing or adding a second identical rig doesn't
re-type GPIOs. `moondeck.json` (the device registry) is the obvious store. The
installer's picker grows from "which firmware" to "which **board/device**"
(auto-selecting the firmware/MCU), with product links beside each board image.

Gated behind workstream 3 (no catalog pin field without its consumer), and the
Board-level Ethernet pins behind workstream 7's runtime PHY config.

### 5. Picture-based installer UI

Rework the installer's text dropdowns into **device → board → MCU selection by
picture**, driven by the catalogs from #4, auto-selecting the firmware. The
text board picker already ships
([backlog § Board injection Step 2](backlog.md#board-injection--improv-as-a-general-data-injector-multi-commit-partially-landed)).
Visible UX win; depends on the catalog model existing.

### 6. Shared installer code across the three clients

Web installer (`docs/install/`), MoonDeck (`scripts/`), and the on-board OTA
picker already share `install-picker.js` (installer ↔ OTA). As the picker grows
device/board/MCU pictures + pin injection, factor the shared logic so all three
clients stay in sync rather than diverging. The known friction is the
**build-context split**: device UI is embedded as a C string via
`embed_ui.cmake`; the web installer is served from Pages — so a shared extract
needs build-glue across ~5 files
([backlog § shared JS helpers](backlog.md#open-follow-up-shared-js-helpers-across-device-ui-and-web-installer)).
Do it when ≥2 helpers earn the glue cost, not for one.

### 7. Firmware variant collapse (classic ESP32 + P4) — gated on runtime PHY

Collapse the three classic variants to two — **the recorded plan already in**
[backlog § Release 2.0](backlog.md#release-20--distribution-catches-up-to-the-source-tree)
(lines on "collapse the three classic variants to two"):

- **`esp32`** — default: WiFi **and** Ethernet, Eth brought up only when a PHY is
  detected/configured. Replaces both today's `esp32` and `esp32-eth-wifi`.
- **`esp32-eth-only`** (today's `esp32-eth`) — Eth only, WiFi compiled out.

Why two-not-three is optimal (the asymmetry): **Ethernet is a light addition**
(the `.eth` fragment is just EMAC + RMII + ~1.5 KB DMA buffers; `esp32-eth-wifi`
excludes nothing), so it needs no separate "+wifi" variant — fold it into the
default. **WiFi is heavy** (~670 KB flash + ~30 KB DRAM, measured —
[backlog § Runtime board presets](backlog.md#runtime-board-presets-multi-commit-partially-landed)),
so the Eth-only build that strips it still earns its own firmware. Same shape for
the P4.

**Hard precondition — do NOT merge yet.** The `.eth` fragment hardwires Olimex
Gateway pins (`PHY_ADDR=0`, `RST_GPIO=5`, LAN8720 RMII). If the default `esp32`
baked that in, every WiFi-only classic board (LOLIN D32, generic DevKit, the
QuinLED/Serg/MHC family — a dozen+ in boards.json) would enable EMAC and **reserve
GPIO 5** for a PHY that isn't there — a silent pin grab the base
`sdkconfig.defaults` comment explicitly guards against. The prerequisite is
**[backlog § Runtime PHY / pin config for Ethernet](backlog.md#release-20--distribution-catches-up-to-the-source-tree)**:
`ethInit()` selects pins/PHY at runtime and no-ops when no PHY is present. Only
then is the merge safe.

### 8. Release-asset dedup — share the boring per-firmware files

Each firmware ships **4** release assets: `bootloader.bin`, `partition-table.bin`,
`ota-data.bin`, and the app `.bin` (staged in
[release.yml § Stage release artifacts](../../.github/workflows/release.yml)).
Measured across all 7 current builds (md5 of the real CI-shaped outputs in
`build/esp32-*/`), only the app and bootloader are truly per-firmware:

| Asset | Distinct values across 7 firmwares | Shareable? |
|---|---|---|
| `ota-data.bin` | **1** — byte-identical everywhere | ✅ **globally** — it's the initial OTA selector, a fixed 0x2000 region of `0xFF` ("both slots empty, boot factory"), chip- and app-independent. |
| `partition-table.bin` | **3** — one per CSV group (4 MB / 16 MB / 8 MB) | ◐ **per partition group** — exactly mirrors the CSVs (the 16 MB binary confirms the [ota_16mb.csv](../../esp32/partitions/ota_16mb.csv) merge: 4 firmwares → 1 hash). |
| `bootloader.bin` | **7** — all differ | ❌ **never** — chip-specific code *and* flash-size/mode baked in; even same-chip variants differ. |
| app `.bin` | 7 | ❌ the unique payload. |

So the dedup removes the redundant **ota-data** (7→1) and **partition-table**
(7→3) copies — ~10 redundant assets today — while bootloader + app stay
per-firmware. **This pays off increasingly as the firmware count grows**: the plan
is one firmware *per supported MCU*, so 7 → 15+ variants turns ~10 redundant files
into ~25+. At that scale the dedup is worth the manifest indirection it costs.

**The cost to weigh:** the web installer flashes a manifest of
`{bootloader, partition-table, ota-data, app}` at fixed offsets per firmware
(ESP Web Tools). Sharing `ota-data` / `partition-table` filenames means every
generated manifest + `flasher-*.json` points at the shared name, which slightly
weakens the "one self-contained asset set per firmware" property that made the
Pages installer robust. So this is **release-plumbing**, done via the **manifest
generator** (the same generator that emits firmwares.json, #2) — not by hand —
so the shared-name references stay consistent and drift-free. Sequence it
**after** the firmwares.json generator exists, since they share that machinery.

### 9. EIM coordination — disambiguate the two "installs", adopt EIM as the build-side enabler

There are **two unrelated "installers"** in this project and the growing
firmware-variant work makes the naming collision worth stating explicitly:

- **EIM — ESP-IDF Installation Manager** is a **developer host-side** tool: it
  installs the *ESP-IDF toolchain* on a build machine, replacing the legacy
  `install.sh` / `idf_tools.py` that `setup_esp_idf.py` drives today
  ([building.md § v6.0 adoption, EIM row](../building.md#esp-idf-version)). It has
  nothing to do with flashing an end-user's device — building.md and the
  [backlog P4 note](backlog.md#esp32-p4-support--rounds-3-4-in-progress) both say
  so outright ("a host-machine installer, unrelated to device-side firmware").
- **This plan's installer** (workstreams 1–8) is **end-user device-side**: the web
  installer / MoonDeck / OTA picker that flashes firmware and injects board/pin
  config.

They are orthogonal in function but belong in one coordinated view for two
reasons. (1) **Naming** — "install" now means two things; keep specs and UI copy
explicit about which, so the two never get conflated. (2) **EIM is the build-side
enabler that scales with this plan:** its multi-IDF-version management
([building.md adoption step 1](../building.md#esp-idf-version) calls it the
highest-priority, lowest-risk v6.0-alignment item) is what keeps building *many*
firmware variants reproducible as the matrix grows from 7 toward one-per-MCU. So
EIM adoption is the natural **build-side companion** to #8's release-asset dedup
and #2's generator: more variants → more value from both EIM (clean multi-version
builds) and the dedup/generator (clean multi-variant artifacts). EIM itself is
tracked and specced in [building.md](../building.md#esp-idf-version) /
[backlog § ESP-IDF version pinning](backlog.md#esp-idf-version-pinning-pending);
this workstream's job is only to **keep the two install tracks coordinated** —
adopt EIM early (it's already the recommended path) so the build side is ready for
the variant explosion the device-side installer work assumes.

## Suggested build order

1. **Partition dedup** (done) — commit it; isolated, complete.
2. **Catalog-driven module provisioning** (#3) — DONE; unblocked #4, #5, MoonDeck.
3. **firmwares.json generator** (#2) — small, removes the 4× duplication; also the
   machinery #8 reuses.
4. **Release-asset dedup** (#8) — share ota-data + partition-table via the #3
   generator; pays off more as variants grow.
5. **Catalog + annotated-pin model** (#4) — once its consumer exists.
6. **Picture-based installer UI** (#5) + **shared-code factoring** (#6) — together.
7. **Runtime PHY config**, then **variant collapse** (#7) — its own branch.

Cross-cutting / parallel: **EIM adoption** (#9) is a build-side track (in
[building.md](../building.md#esp-idf-version)), adopted early and independently so
the build matrix is ready for the one-firmware-per-MCU growth the device-side work
assumes. Steps 1–4 are independent and low-risk; 5–6 build the visible installer;
7 is gated on runtime PHY work. None of the catalog steps land data ahead of its
consumer.
