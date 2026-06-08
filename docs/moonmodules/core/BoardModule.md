# BoardModule

Owns the physical-hardware identity name — e.g. `Olimex ESP32-Gateway Rev G`, `LOLIN D32`. The device cannot self-identify the board (no readable PCB ID on classic ESP32), so the value is *injected* by an outside tool — MoonDeck over HTTP, or the web installer over Improv RPC (with an HTTP fallback when Improv isn't available). The board name is metadata, not user input: the `board` control is bound `readonly` so the device's own UI renders it display-only, but HTTP writes still succeed — that's how the injectors push.

BoardModule is a code-wired child of SystemModule, mirroring how [ImprovProvisioningModule](ImprovProvisioningModule.md) lives under NetworkModule. `markWiredByCode()` keeps it alive on devices whose persisted `SystemModule.json` predates the child (see [Persistence — code-wired children](../../architecture.md#persistence)).

Single-control by design: the set grows only when a runtime consumer reads a new per-board field, not before. The catalog already carries per-board values beyond the name — they fan out generically (see Catalog).

## Controls

| Name | Type | Description |
|---|---|---|
| `board` | text (≤31 chars), `readonly` flag | Physical board name. Empty until injected. Auto-persisted to `BoardModule.json`; survives reboot. |

## Injection paths

Three transports, one data store — all end up writing the same buffer and arming FilesystemModule's debounced save.

### MoonDeck — HTTP `POST /api/control`

MoonDeck deduces the board from the device's `firmware` control when a single catalog entry claims that firmware (`esp32-eth*` → Olimex Gateway). For ambiguous firmwares (`esp32` runs on LOLIN D32, Generic DevKit, …) the user picks via a per-device dropdown. After each discover/refresh and each dropdown change, MoonDeck pushes the *full* `controls.<Module>.<control>` block from the matching catalog entry — same generic fan-out the installer and the device-side `?board=` consumer use. An unknown board name falls back to pushing just `Board.board`.

### Web installer — Improv vendor RPC SET_BOARD (0xFE)

The installer's orchestrator (`docs/install/install-orchestrator.js`) owns the SerialPort end-to-end: flash via esptool-js → Improv WiFi provisioning → vendor RPC `SET_BOARD` carrying the picked board name. The device-side handler (`platform_esp32_improv.cpp`) validates the payload and signals the scheduler thread; the module's `loop1s()` applies it.

The RPC pushes **only** the board name — it runs before WiFi association, while the rest of the catalog fan-out rides HTTP (post-association). Every per-board control shipped today applies post-association (e.g. `Network.txPowerSetting`), so the ordering is fine: a control that must take effect *before* association would need its own pre-association transport, not SET_BOARD's wire format.

### Web installer — HTTP fallback via Inject button (`?board=…`)

The RPC path needs a running Improv listener, which two cases break: a device that booted `STATE_PROVISIONED` from kept credentials (the SDK reports "Improv not detected"), and Ethernet-only builds compiled without Improv. The orchestrator catches that, prompts for the device IP/hostname, records it in *Your devices* with the board marked `pendingBoard`, and renders an **Inject** button.

Clicking Inject opens the device URL with `?board=<name>`. The UI's `init()` strips the param (via `history.replaceState`, so a refresh doesn't double-push), fetches the canonical [boards.json](https://ewowi.github.io/projectMM/install/boards.json) on Pages (CORS-open), looks up the entry, and sends one sequential `POST /api/control` per `controls.<Module>.<control>` leaf — same channel, validation, and debounced save as MoonDeck. Sequential, not parallel: each module's dirty-flag rebuild must finish before the next write to the same module, so multi-field-per-module entries can't race the flag. Inject is a separate affordance from Visit because injecting config and browsing the UI are different intents; it's idempotent, so re-clicking after a mistyped IP or a boards.json edit is safe.

The localhost preview (`preview_installer.py`, served over `http://`) also tries the HTTP push from the install page itself before Inject — dev-loop parity with MoonDeck. The production page on `https://` can't (mixed-content blocker), so end users always go through Inject.

### SET_BOARD wire contract

Standard Improv framing (see `src/core/ImprovFrame.h`). Command ID `0xFE` (high end of the `0x80..0xFE` vendor range).

- **Request payload:** `[0xFE][data_len][str_len][name…]` — name 1..31 bytes, UTF-8, validated ASCII-printable (0x20..0x7E) only.
- **Success:** type `0x04`, payload `[0xFE][0x00]`.
- **Error:** type `0x02`, `0x80 ERROR_INVALID_BOARD` (empty / over-length / non-printable / malformed length), or `ERROR_UNKNOWN_RPC` if the build has no BoardModule.
- **Timing:** accepted any time the Improv task is running (including post-PROVISIONED); the RpcResponse fires immediately, the actual write defers to the next `loop1s()` (same producer/consumer pattern as `SEND_WIFI_CREDENTIALS`).

## Catalog

`docs/install/boards.json` is the source of truth for valid board names AND any per-board control values the device adopts when fanned out. Each entry:

```json
{ "name": "Olimex ESP32-Gateway Rev G",
  "firmwares": ["esp32-eth", "esp32-eth-wifi"],
  "default_firmware": "esp32-eth-wifi",
  "controls": { "Board": { "board": "Olimex ESP32-Gateway Rev G" } } }
```

`name` is both identifier and display label (no slug/label split). `firmwares[]` narrows the picker's firmware dropdown; `default_firmware` pre-selects one. `controls` is the injectable payload — a `{ "<Module>": { "<control>": <value> } }` map mirroring the `/api/control` POST body, one leaf per write. Every fan-out path iterates it generically, so a new leaf (e.g. `Network.txPowerSetting`, Ethernet pin maps) takes effect everywhere with no code change on device, MoonDeck, or installer.

The device does **not** validate against the catalog — it stores whatever it's told (subject to per-control validation). This keeps the device decoupled: adding a board or a field needs no firmware change. An unknown name renders as `<name> (unknown)` in MoonDeck (which then pushes only `Board.board`); the installer's Inject logs "no controls for board" and skips the fan-out.

## Prior art

The device-side seed of a feature that started in MoonLight as the "IO module" (board presets + I²S peripherals + sensor inputs in one). projectMM splits by lifecycle: BoardModule owns boot-time hardware *identity* (set once), while runtime-attached devices are individual [peripherals](../../architecture.md#peripherals) — each its own MoonModule, user-add/deletable. Conflating set-once identity with hot-pluggable devices made the lifecycle hard to reason about, so they stay separate kinds.

## Source

[BoardModule.h](../../../src/core/BoardModule.h)
