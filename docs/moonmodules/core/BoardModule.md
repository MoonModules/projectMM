# BoardModule

Owns the physical-hardware identity name — e.g. `Olimex ESP32-Gateway Rev G`, `LOLIN D32`. The device cannot self-identify the board (no readable PCB ID on classic ESP32), so the value is *injected* by an outside tool. Today that's MoonDeck and tomorrow the web installer (Step 2 of the multi-commit board-injection plan). The board name is metadata, not user input — the `board` Text control on the device is bound with the `readonly` UI flag so the device's web UI renders it display-only (HTTP writes still succeed — that's how the injectors push). The catalog (`docs/install/boards.json`) is the source of truth, and MoonDeck mirrors the picked / deduced value back on next discover if it drifts.

BoardModule is a code-wired child of SystemModule, mirroring the way `ImprovProvisioningModule` lives under `NetworkModule`. `markWiredByCode()` keeps it alive on devices whose persisted `/.config/SystemModule.json` predates the child addition — without it, the first FilesystemModule load would trim the unknown child.

Today this is a single-control module — a deliberate seed for the planned [Runtime board presets](../../plan.md) work that will grow per-board pin maps (Ethernet RMII clock GPIO, MDIO/MDC, LED-data pins), default module-config overrides, and the board key as the catalog index. The control set grows when the first runtime consumer reads the new fields, not before.

## Controls

| Name | Type | Description |
|---|---|---|
| `board` | text (24 chars), `readonly` flag | Physical board name. Empty by default; injected by MoonDeck on first reach via `POST /api/control { "module":"Board", "control":"board", "value":"<name>" }`. Bound `Text` so FilesystemModule auto-persists it to `/.config/BoardModule.json`; the `readonly` UI hint makes the device's own web UI render it display-only (the framework's `/api/control` write path still applies — that's how the injectors push). Survives reboot. |

## Injection path

Two transports, one data store. Both end up writing to the same `boardKey_` buffer and arming FilesystemModule's debounced save.

### MoonDeck — HTTP `POST /api/control`

MoonDeck deduces the board from the device's `firmware` control whenever a single catalog entry claims that firmware (e.g. `esp32-eth` / `esp32-eth-wifi` → Olimex Gateway). For ambiguous firmwares (`esp32` runs on LOLIN D32, Generic ESP32 DevKit, …) the user picks via the per-device dropdown in MoonDeck's UI. After every `/api/discover` or `/api/refresh`, and after every dropdown change in MoonDeck (`POST /api/push-board` on the MoonDeck side, which calls `_push_board_to_device`), MoonDeck pushes the value to the device. The HTTP write goes through the standard control-write dispatcher; the `readonly` flag on the `board` control is a UI-rendering hint and does not gate the HTTP write.

### Web installer — Improv vendor RPC SET_BOARD (0xFE)

The web installer's custom orchestrator (`docs/install/install-orchestrator.js`) owns the SerialPort end-to-end: it flashes via esptool-js, provisions WiFi via the Improv standard `SEND_WIFI_CREDENTIALS` (0x01), then sends our vendor RPC `SET_BOARD` (0xFE) carrying the user's picked board name. The device-side handler at `src/platform/esp32/platform_esp32_improv.cpp::improvHandleSetBoard` validates the payload, signals the scheduler thread via a buffer + atomic flag, and the module's `loop1s()` calls `BoardModule::setBoard()`. Same dirty-flag + debounced-save chain MoonDeck triggers; the only difference is the transport.

This commit replaced ESP Web Tools' install button. EWT 10.x held the SerialPort exclusively and fired its `state-changed` event inside its dialog's shadow DOM (verified by reading `esp-web-tools/src/install-dialog.ts`), which made post-PROVISIONED board injection from the installer page structurally impossible and also silently broke `devices.js`'s "Your devices" auto-add. Owning the SerialPort across flash + provision + RPC lets both fixes land — same dispatcher seeds future Step-4+ injectables (device name override, MQTT broker URL, DMX universe, …) each adding a new vendor command ID + dispatcher case.

### SET_BOARD wire contract

- **Frame type:** `0x03` (RPC) for request, `0x04` (RpcResponse) for success, `0x02` (ErrorState) for failure. Standard Improv framing (see `src/core/ImprovFrame.h`).
- **RPC command ID:** `0xFE` — high end of the conventional `0x80..0xFE` vendor extension range, chosen to maximize headroom against future Improv-spec expansion into the low vendor range.
- **Request payload** (within the Improv frame's `payload` field):
  - `[0xFE]` command
  - `[data_len]` number of bytes that follow (= 1 + str_len)
  - `[str_len]` 1..23, length of board name in bytes
  - `[board_name bytes]` UTF-8, validated as ASCII-printable (0x20..0x7E) only
- **Success response:** type `0x04`, payload `[0xFE][0x00]` (command + zero-length data).
- **Error response:** type `0x02`, single-byte payload `0x80 ERROR_INVALID_BOARD` for validation failures (empty, over-length, non-printable, malformed length prefix), or `improv::ERROR_UNKNOWN_RPC` if the device build doesn't have a BoardModule wired.
- **Behavior:** can be sent any time after the Improv task is running, including post-PROVISIONED. No state-machine restriction. RpcResponse fires immediately from the Improv task; the actual `BoardModule::setBoard()` call is deferred to the next `loop1s()` tick on the scheduler thread (same producer/consumer pattern as `SEND_WIFI_CREDENTIALS`).

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
