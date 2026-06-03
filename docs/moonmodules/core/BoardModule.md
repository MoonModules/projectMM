# BoardModule

Owns the physical-hardware identity name — e.g. `Olimex ESP32-Gateway Rev G`, `LOLIN D32`. The device cannot self-identify the board (no readable PCB ID on classic ESP32), so the value is *injected* by an outside tool. Today that's MoonDeck and tomorrow the web installer (Step 2 of the multi-commit board-injection plan). The board name is metadata, not user input — the `board` Text control on the device is bound with the `readonly` UI flag so the device's web UI renders it display-only (HTTP writes still succeed — that's how the injectors push). The catalog (`docs/install/boards.json`) is the source of truth, and MoonDeck mirrors the picked / deduced value back on next discover if it drifts.

BoardModule is a code-wired child of SystemModule, mirroring the way `ImprovProvisioningModule` lives under `NetworkModule`. `markWiredByCode()` keeps it alive on devices whose persisted `/.config/SystemModule.json` predates the child addition — without it, the first FilesystemModule load would trim the unknown child.

Today this is a single-control module — a deliberate seed for the planned [Runtime board presets](../../plan.md) work that will grow per-board pin maps (Ethernet RMII clock GPIO, MDIO/MDC, LED-data pins), default module-config overrides, and the board key as the catalog index. The control set grows when the first runtime consumer reads the new fields, not before.

## Controls

| Name | Type | Description |
|---|---|---|
| `board` | text (24 chars), `readonly` flag | Physical board name. Empty by default; injected by MoonDeck on first reach via `POST /api/control { "module":"Board", "control":"board", "value":"<name>" }`. Bound `Text` so FilesystemModule auto-persists it to `/.config/BoardModule.json`; the `readonly` UI hint makes the device's own web UI render it display-only (the framework's `/api/control` write path still applies — that's how the injectors push). Survives reboot. |

## Injection path

Injection is a regular `POST /api/control` write to the `board` control on BoardModule. The control is bound `Text` with the `readonly` UI flag — so the framework's standard dispatcher copies the value into the buffer, marks the module dirty, and FilesystemModule's debounced save (2 s) writes the new value to disk. No bespoke setter, no bespoke route, no bespoke persistence. The `readonly` flag affects only the device's own UI rendering; it doesn't gate HTTP writes.

MoonDeck deduces the board from the device's `firmware` control whenever a single catalog entry claims that firmware (e.g. `esp32-eth` / `esp32-eth-wifi` → Olimex Gateway). For ambiguous firmwares (`esp32` runs on LOLIN D32, Generic ESP32 DevKit, …) the user picks via the per-device dropdown in MoonDeck's UI. After every `/api/discover` or `/api/refresh`, and after every dropdown change in MoonDeck (`POST /api/push-board` on the MoonDeck side, which calls the same `_push_board_to_device` helper), MoonDeck pushes the value to the device — devices stay in sync over the next probe cycle even if a push transiently fails.

The Step 3 commit will add Improv RPC injection so the web installer can set the board over Web Serial during the same modal that provisions WiFi. The device-side handler will call the same control-write path; the only difference is the transport.

## Catalog

`docs/install/boards.json` is the single source of truth for valid board names. Each entry:

```json
{ "name": "Olimex ESP32-Gateway Rev G",
  "firmwares": ["esp32-eth", "esp32-eth-wifi"],
  "default_firmware": "esp32-eth-wifi" }
```

`name` is both the identifier and the display label — human-readable strings, no slug/label split (the device stores them opaquely, MoonDeck and the web installer show them as-is). `firmwares[]` lists which compiled-binary variants run on this board; the web installer Step 2 will use it to narrow the firmware dropdown after a board is picked. `default_firmware` is which variant to pre-select for this board.

The device does NOT validate against this catalog — the `board` control accepts any value the injector writes (subject to the standard Text control validation: 24-byte buffer, no NUL inside). The catalog is the injector's responsibility; the device just stores what it's told. This keeps the device decoupled from catalog updates: adding a new board doesn't require a firmware change.

A board name that doesn't appear in the catalog renders in MoonDeck's picker as `<name> (unknown)` — the value survives the round-trip rather than silently snapping to empty. The web installer (Step 2) will surface the same fallback.

## Files

- `/.config/BoardModule.json` — persisted state (`{ "board": "<name>" }`). Written by FilesystemModule's debounced save when the control is dirty; absent on a fresh device until MoonDeck pushes a value.

## Prior art

This is the device-side seed of a feature that started in MoonLight as the "IO module" (a grab-bag for board presets + I²S peripherals + sensor inputs). projectMM splits the lifecycle: BoardModule owns boot-time hardware identity; a future `PeripheralsModule` or similar will own runtime-attached devices. Conflating the two under "IO" makes the lifecycle layer (boot-time set once vs runtime hot-pluggable) hard to reason about.
