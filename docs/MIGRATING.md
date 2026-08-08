# Migrating

The log of **breaking changes** — what changed between versions, and the action to take.

projectMM ships **no migration code**: the persistence layer is robust by default (an absent key keeps the control's default, a stale value clamps to the new bounds, an unknown key is ignored), which absorbs almost all schema drift with zero migration-specific code. The rare change that a robust reader *cannot* absorb is **documented here instead of migrated** — see [ADR-0013](adr/0013-no-migration-code-robust-persistence-plus-documented-breaks.md) for the decision and its rationale.

**Read this when upgrading a device that already holds persisted state.** Entries are newest first. Each says what changed and what to do; most need nothing at all, because the lost value re-populates on next use.

**Action legend** — how much work an entry costs you:

| Action | Meaning |
|---|---|
| *nothing* | Self-heals. The value re-populates on next use, or the default is correct. |
| *re-set a control* | One value resets to its default; set it again in the UI if you had changed it. |
| *re-add a module* | The module vanishes from the tree on boot; add it again and re-enter its controls. |
| *update a file* | An on-device file must be edited or replaced. |
| *erase flash* | A full flash erase is required (the heaviest — a full reconfigure follows). |

---

## Unreleased (`next-iteration`)

### The `Layers` container is renamed to `Effects` (2026-08-08)

The three top-level light containers are now **Layouts, Effects, Drivers** — L.E.D. The old name sat one character from its own child (`Layers` holding `Layer`s) and read as a near-twin of `Layouts`, which is the pair a newcomer actually has to tell apart. The tree is unchanged in shape: `Effects` → `Layer`s → effects and modifiers.

**Action: *re-add a module* and *re-save presets*.**

The type name is the persisted filename and the preset capture key, so two things do not survive the update:

| What | Why | What to do |
|---|---|---|
| The saved light tree | The device looks for `/.config/Effects.json` and the old file is `Layers.json`, so the light tree boots empty | Re-add your Layer, effect and modifiers, then let it save |
| Presets that capture the look | A preset file records `"captures": ["Layers"]`, a key no module now answers to | Re-save each preset once the tree is rebuilt |

A preset also records the ROLE it covers, and that role is now named after the container rather than after a module inside it: `"layer"` becomes `"effects"`. A preset carrying the old role still loads, but shows no tint or emoji on its pad until it is re-saved — the UI has no `layer` role to colour it by.

Renaming the file on the device works if you would rather not rebuild by hand: `Layers.json` → `Effects.json`, and `"Layers"` → `"Effects"` inside each `/.config/presets/*.json`. Nothing else in either file changes.

The child `Layer` keeps its name, as does everything under it.


### The `peripheral` options are renamed to name the peripheral, not the bus protocol (2026-07-30)

The `peripheral` dropdown no longer says `i80` / `MoonI80`. "i80" is the Intel 8080 bus shape `esp_lcd` speaks — it is not a peripheral any ESP32 datasheet lists, and it matched nothing a user could look up: on the classic ESP32 that backend **is the I2S peripheral**, on the S3/P4/S31 it is the **LCD** peripheral. The new labels name the silicon block plus who drives it, which is the actual choice being made.

| Old | New (classic ESP32) | New (S3 / P4 / S31) |
|---|---|---|
| `i80` | `I2S-IDF` | `LCD-IDF` |
| `MoonI80` | — (not available) | `LCD-MM` |
| `Parlio` | — | `Parlio` (unchanged — it *is* the peripheral's name) |

`-IDF` = driven through ESP-IDF's `esp_lcd`; `-MM` = driven by our own GDMA layer below it, which is what buys the streaming ring and the 74HCT595 pin expander.

**Action: re-set the `peripheral` control** — but only on a device that already holds a persisted parallel driver AND had a non-default peripheral selected. The stored string no longer matches any option, so the loader falls back to the board's default backend; if that was already your choice, nothing changes. The web installer's board catalog ships the new names, so a fresh install or catalog re-inject is correct without action.

### The three parallel LED drivers merge into one `ParallelLedDriver` with a `peripheral` selector (2026-07-23)

`MultiPinLedDriver`, `MoonLedDriver`, and `ParlioLedDriver` are now one registered module, **`ParallelLedDriver`**, whose `peripheral` control picks which DMA peripheral drives the parallel WS2812 bus. They were always the same driver with a different bus backend; the merge makes that one card with a dropdown, offering only the peripherals the chip supports.

| Old registered type | New |
|---|---|
| `MultiPinLedDriver` | `ParallelLedDriver` + `peripheral` = `i80` (esp_lcd: LCD_CAM on S3/P4, I2S on classic) — renamed again below |
| `MoonLedDriver` | `ParallelLedDriver` + `peripheral` = `MoonI80` (own-GDMA below esp_lcd, LCD_CAM) — renamed again below |
| `ParlioLedDriver` | `ParallelLedDriver` + `peripheral` = `Parlio` (P4) |

**Action: re-add the driver.** A persisted module whose type is one of the three old names no longer resolves (the type isn't registered), so the robust loader drops it on boot — the driver, and its pins/settings, vanish from the tree. Add a **Parallel LED** driver again, choose the `peripheral` your board uses (the same backend the old type named — see the table), and re-enter its `pins` / `ledsPerPin` plus whatever the chosen peripheral needs: `i80` has `clockPin`/`dcPin`, `MoonI80` has `clockPin` + the ring/expander controls, `Parlio` has no clock or DC pins at all. The web installer's board catalog already names the new type, so a fresh install or a catalog re-inject wires it correctly; only a device carrying an OLD persisted tree needs the manual re-add.

### The per-driver `preset` control is renamed to `lightPreset` (2026-07-23)

The correction Select every driver exposes (channel order / RGBW synthesis) is renamed `preset` → `lightPreset`, so the UI label reads unambiguously next to a driver's other controls.

| Old | New |
|---|---|
| control `preset` | `lightPreset` |

**Action: nothing.** The saved value survives the rename (see the `lightPreset` [persistence contract](moonmodules/light/drivers.md#led-driver-details) for how a driver's preset reference is stored and re-resolved). Only an external script or automation that POSTs the control by name (`/api/control` with `"control":"preset"`) must switch to `lightPreset`.

### `AudioService`: the `sync` control becomes `mode` + `send audio`, and `simulate` is renumbered (2026-07-22)

The audio module's identity is now a single `mode` control (Local audio / Receive network / Simulate), each showing only its own detail controls, replacing the separate `sync` (off / send / receive) toggle. Broadcasting the locally-analyzed frame moved to a `send audio` switch, meaningful only in Local mode. `simulate` was also renumbered, from a five-option list to two.

| Old | New |
|---|---|
| control `sync` (Select: `off`/`send`/`receive`) | `mode` (Select: `local audio`/`receive network`/`simulate`) + `send audio` (a switch, Local mode only) |
| control `simulate` (Select, 5 options incl. a mic-fill-on-silence mode) | `simulate` (Select: 2 options) — used only when `mode` is Simulate |

**Action: re-set `mode` (and `send audio`) if you had `sync` on `send` or `receive`; re-set `simulate` if you had chosen a non-default option.**

`sync` and the old `simulate` value read as absent → ignored, so `mode` takes its default (**Local audio**) and `send audio` its default (**off**). A device that was on `sync=receive` therefore comes up as Local audio — set `mode` to Receive network again. One that broadcast (`sync=send`) comes up not broadcasting — turn `send audio` on. The five-option `simulate` collapsed to two, so a device on one of the dropped options (e.g. the mic-fill-on-silence mode, a removed capability) takes the new default; re-pick if needed. Receive network and every sync control exist only on network-capable targets.

### `MoonLedDriver`: `forceRing` → `useRing`, and the ring's geometry is now settable (2026-07-17)

The pin-expander path selector was a three-option Select (`auto` / `ring` / `wholeFrame`) named for a *diagnostic override*. The auto-router is gone — at the size the expander exists for (48 strands × 256 lights) a whole frame never fits internal DMA RAM, so "auto" had exactly one right answer while presenting itself as a choice, and its silent fallback hid which path was actually running. What remains is the honest question, as a switch:

| Old | New |
|---|---|
| control `forceRing` (Select: `auto`/`ring`/`wholeFrame`) | `useRing` (a switch: on = ring, off = whole frame) |
| — | `ringRows` (new: lights per DMA buffer, 1..64) |
| — | `ringBufs` (new: buffers the DMA circulates, 2..32) |

**Action: re-set `useRing` if you had `forceRing` on `wholeFrame`.**

`forceRing` reads as absent → ignored, and `useRing` takes its default (**on**, the ring). A device that had explicitly selected whole-frame therefore comes up on the ring; flip `useRing` off to get it back. `ringRows`/`ringBufs` default to 16 and 12 — the geometry the driver effectively ran. (It shipped with a pool of 16, but 16 buffers never fit the S3's internal DMA heap, so the ring build failed its own fit check and the driver quietly fell back to whole-frame; 12 is what actually held. A config on the old defaults may therefore start *ringing* where it used to fall back.) They exist so the RAM / encode-overhead / interrupt-rate / lap-time trade-off can be swept on a live board rather than fixed at compile time.

### LED driver + control rename — a human-readable UI (2026-07-16)

The LED driver module types and several controls were renamed so the UI reads in plain language rather than peripheral jargon (the UI shows a control's name verbatim, so the name *is* the label).

| Old | New |
|---|---|
| module type `I80LedDriver` | `MultiPinLedDriver` |
| module type `MoonI80LedDriver` | `MoonLedDriver` |
| control `shiftRegister` | `pinExpander` |
| control `asyncTransmit` | `doubleBuffer` |
| read-only `wireUs` | `frameTime` |
| read-only `stall` (Drivers) | `renderWait` |

**Action: re-add the module, then re-set `pinExpander` / `doubleBuffer` if you had changed them.**

A device whose persisted config names the old module type loads a module type that no longer exists — the unknown type is ignored, so **the driver is absent from the tree on boot**. Re-add a **Parallel LED** driver (the single type the two later merged into — see the 2026-07-23 entry above for the `peripheral` value that matches the old `I80LedDriver` / `MoonI80LedDriver`) and re-enter its controls. Within a re-added driver, the two renamed *settable* controls (`pinExpander`, `doubleBuffer`) read as absent → they take their defaults (`pinExpander` off, `doubleBuffer` on); set them again if your board needs otherwise. `frameTime` and `renderWait` are read-only KPIs — nothing to restore.

This rename left `RmtLedDriver` untouched, and `ParlioLedDriver` untouched *at the time*; the later 2026-07-23 entry above then merges `ParlioLedDriver` into `ParallelLedDriver` along with the other two. The `pins` / `ledsPerPin` / `clockPin` / `latchPin` / `loopback*` controls are unchanged by this rename.

---

## Earlier

These pre-date this log and were recorded in ADR-0013's Consequences list. A device that persisted state on an older build and loads a newer one loses only the noted value, which re-populates on next use.

### Container config filenames (`LayoutGroup.json` → `Layouts.json`)

`.config/LayoutGroup.json` → `Layouts.json`, `.config/DriverGroup.json` → `Drivers.json` (container type rename). The stale files are ignored (unknown-type config isn't loaded); they linger harmlessly on disk until a flash erase.

**Action: nothing.** Lost: the container's `enabled` flag (defaults back on).

### UI last-selected module (`mm.selectedModule` → `mm_selected`)

The browser localStorage key for the UI's last-selected module.

**Action: nothing.** Lost: the remembered selection resets to the first module.

### Device-list `color` → `color` (US-spelling rename)

The DevicesModule persisted-list key for a Hue bridge's color-capable light count (`DevicesModule::restoreList()`). A device list persisted under the old key reads the count as absent → 0.

**Action: nothing.** The cached bridge count resets to 0 until the bridge is re-heard live and re-populates it.
