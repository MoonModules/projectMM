# BoardModule

Owns the physical-hardware identity name — e.g. `Olimex ESP32-Gateway Rev G`, `LOLIN D32`. The device cannot self-identify the board (no readable PCB ID on classic ESP32), so the value is *injected* by an outside tool — MoonDeck over `POST /api/control`, or the web installer over Improv RPC (with an HTTP fallback through the *Visit* link when Improv isn't available). The board name is metadata, not user input — the `board` Text control on the device is bound with the `readonly` UI flag so the device's web UI renders it display-only (HTTP writes still succeed — that's how the injectors push). The catalog (`docs/install/boards.json`) is the source of truth, and MoonDeck mirrors the picked / deduced value back on next discover if it drifts.

BoardModule is a code-wired child of SystemModule, mirroring the way `ImprovProvisioningModule` lives under `NetworkModule`. `markWiredByCode()` keeps it alive on devices whose persisted `/.config/SystemModule.json` predates the child addition — without it, the first FilesystemModule load would trim the unknown child.

Today this is a single-control module — a deliberate seed for the planned [Runtime board presets](../../plan.md) work that will grow per-board pin maps (Ethernet RMII clock GPIO, MDIO/MDC, LED-data pins), default module-config overrides, and the board key as the catalog index. The control set grows when the first runtime consumer reads the new fields, not before.

## Controls

| Name | Type | Description |
|---|---|---|
| `board` | text (32-byte buffer, max 31 chars), `readonly` flag | Physical board name. Empty by default; injected by MoonDeck on first reach via `POST /api/control { "module":"Board", "control":"board", "value":"<name>" }`. Bound `Text` so FilesystemModule auto-persists it to `/.config/BoardModule.json`; the `readonly` UI hint makes the device's own web UI render it display-only (the framework's `/api/control` write path still applies — that's how the injectors push). Survives reboot. |

## Injection path

Three transports, one data store. All three end up writing the same `boardKey_` buffer and arming FilesystemModule's debounced save.

### MoonDeck — HTTP `POST /api/control`

MoonDeck deduces the board from the device's `firmware` control whenever a single catalog entry claims that firmware (e.g. `esp32-eth` / `esp32-eth-wifi` → Olimex Gateway). For ambiguous firmwares (`esp32` runs on LOLIN D32, Generic ESP32 DevKit, …) the user picks via the per-device dropdown in MoonDeck's UI. After every `/api/discover` or `/api/refresh`, and after every dropdown change in MoonDeck (`POST /api/push-board` on the MoonDeck side, which calls `_push_board_to_device`), MoonDeck pushes the *full* `controls.<Module>.<control>` block from the matching catalog entry — same generic fan-out shape the web installer and the device-side `?board=` Inject path use. A custom / unknown board name (no catalog entry) falls back to pushing just the bare `Board.board` field. All HTTP writes go through the standard control-write dispatcher; the `readonly` flag on the `board` control is a UI-rendering hint and does not gate the HTTP write.

### Web installer — Improv vendor RPC SET_BOARD (0xFE)

The web installer's custom orchestrator (`docs/install/install-orchestrator.js`) owns the SerialPort end-to-end: it flashes via esptool-js, provisions WiFi via the Improv standard `SEND_WIFI_CREDENTIALS` (0x01), then sends our vendor RPC `SET_BOARD` (0xFE) carrying the user's picked board name. The device-side handler at `src/platform/esp32/platform_esp32_improv.cpp::improvHandleSetBoard` validates the payload, signals the scheduler thread via a buffer + atomic flag, and the module's `loop1s()` calls `BoardModule::setBoard()`. Same dirty-flag + debounced-save chain MoonDeck triggers; the only difference is the transport.

The orchestrator owns the SerialPort end-to-end (flash via esptool-js → Improv provisioning → SET_BOARD), so post-PROVISIONED board injection lands in the same session as the flash and the "Your devices" auto-add records the URL atomically. The vendor-command-dispatcher pattern is extensible: each new per-control injectable (device name override, MQTT broker URL, DMX universe) adds one command ID + one dispatcher case — see the design rationale in [docs/history/decisions.md](../../history/decisions.md).

The Improv RPC pushes **only** the board name (`Board.board`) — it does not consult the boards.json `controls.*` payload. The full catalog fan-out lives in the HTTP paths (MoonDeck `_push_board_to_device`, the orchestrator's post-Improv HTTP push, the device-side `?board=` consumer). The RPC stays single-purpose because everything else can wait until WiFi is up.

**Pre-WiFi limitation.** SET_BOARD runs before WiFi association; every other catalog field rides HTTP which runs after. Per-board controls that need to land *before* association (country code, antenna selector, pre-association TX-power) can't use this pipeline as-is — see [docs/history/decisions.md](../../history/decisions.md) for the design rationale and the escape hatches (new vendor RPC vs sdkconfig bake).

### Web installer — HTTP fallback via Inject button (`?board=…`)

The Improv-RPC path needs the firmware to actually run an Improv listener. Two real cases break that assumption: the device boots into `STATE_PROVISIONED` because it kept WiFi credentials from a previous flash (the SDK throws "Improv Wi-Fi Serial not detected" instead of completing the handshake); and Ethernet-only builds (`esp32-eth`, `desktop-*`) compile without the Improv module at all. The orchestrator catches the SDK error and falls through to a "Device IP or hostname" prompt in the install modal; the user types the IP or `MM-XXXX.local`, the orchestrator records it in *Your devices* with the picked board name marked `pendingBoard`, and an **Inject** button renders next to the row's Visit/Erase/Forget actions.

Clicking Inject opens the device URL with `?board=<encoded-name>` in a new tab. The device's HTTP server strips the query string before route matching (see `HttpServerModule::handleRequest`) and serves the UI as normal; the UI's `init()` then consumes the param via `consumePendingBoardParam()`:

1. Strip `?board=` from `location` via `history.replaceState` (so a mid-fetch refresh doesn't double-push).
2. Fetch `https://ewowi.github.io/projectMM/install/boards.json` (the canonical catalog on Pages, served with `Access-Control-Allow-Origin: *`).
3. Look up the entry whose `name` matches the param.
4. For every `controls.<ModuleName>.<controlName>: <value>` in the entry, send a sequential `POST /api/control` — same channel MoonDeck uses, same module-level validation, same FilesystemModule debounced save.

The fan-out is sequential rather than parallel: each module's dirty-flag dance and rebuild reaction need to finish before the next write hits, otherwise two writes to the same module could race the dirty flag. For the single-control case today (just `Board.board`) it doesn't matter; the moment a second field per module lands (Ethernet pin maps will populate `Board.ethRmiiClock` etc.) the order discipline pays off.

The Inject button is explicit, not bundled into Visit, because configuration injection and "browse the device UI" are different intents — a contributor reading the UI should see the affordance, not have it hidden inside a routine click. The button shows the board name (`Inject Olimex ESP32-Gateway Rev G`) and prompts for confirmation before opening the new tab. It renders for any device that has a `board` field on it, not only the just-flashed ones: re-clicking is idempotent (the device re-writes the same `controls.*` values), so the button stays reachable after popup-blocker rejections, mistyped IPs, or a follow-up boards.json edit. The `pendingBoard` hint only flavours the styling (primary-coloured) until the user has Injected at least once.

In the localhost preview (`scripts/run/preview_installer.py`, page on `http://`), the orchestrator additionally tries the HTTP push from the install page itself before the user clicks Inject — exactly the dev-loop convenience MoonDeck offers on the LAN. The production install page on `https://ewowi.github.io` cannot do that (mixed-content blocker stops `https://` pages from fetching `http://192.168.1.X`); the in-orchestrator push silently no-ops on HTTPS and the *Inject* handoff is the only injection path. End users hit the Inject handoff; the in-orchestrator push exists for dev parity.

### SET_BOARD wire contract

- **Frame type:** `0x03` (RPC) for request, `0x04` (RpcResponse) for success, `0x02` (ErrorState) for failure. Standard Improv framing (see `src/core/ImprovFrame.h`).
- **RPC command ID:** `0xFE` — high end of the conventional `0x80..0xFE` vendor extension range, chosen to maximize headroom against future Improv-spec expansion into the low vendor range.
- **Request payload** (within the Improv frame's `payload` field):
  - `[0xFE]` command
  - `[data_len]` number of bytes that follow (= 1 + str_len)
  - `[str_len]` 1..31, length of board name in bytes (capped by `BoardModule::boardKey_`'s 32-byte buffer)
  - `[board_name bytes]` UTF-8, validated as ASCII-printable (0x20..0x7E) only
- **Success response:** type `0x04`, payload `[0xFE][0x00]` (command + zero-length data).
- **Error response:** type `0x02`, single-byte payload `0x80 ERROR_INVALID_BOARD` for validation failures (empty, over-length, non-printable, malformed length prefix), or `improv::ERROR_UNKNOWN_RPC` if the device build doesn't have a BoardModule wired.
- **Behavior:** can be sent any time after the Improv task is running, including post-PROVISIONED. No state-machine restriction. RpcResponse fires immediately from the Improv task; the actual `BoardModule::setBoard()` call is deferred to the next `loop1s()` tick on the scheduler thread (same producer/consumer pattern as `SEND_WIFI_CREDENTIALS`).

## Catalog

`docs/install/boards.json` is the single source of truth for valid board names AND for any per-board control values the device should adopt when the Inject button fans them out. Each entry:

```json
{ "name": "Olimex ESP32-Gateway Rev G",
  "firmwares": ["esp32-eth", "esp32-eth-wifi"],
  "default_firmware": "esp32-eth-wifi",
  "controls": {
    "Board": { "board": "Olimex ESP32-Gateway Rev G" }
  }
}
```

`name` is both the identifier and the display label — human-readable strings, no slug/label split (the device stores them opaquely, MoonDeck and the web installer show them as-is). `firmwares[]` lists which compiled-binary variants run on this board; the picker uses it to narrow the firmware dropdown after a board is picked. `default_firmware` is which variant to pre-select for this board.

`controls` is the device-injectable payload — a two-level map `{ "<ModuleName>": { "<controlName>": <value>, ... }, ... }`. The shape mirrors the `/api/control` POST body the device already accepts: each leaf becomes one `{module, control, value}` write. Every fan-out path (MoonDeck `_push_board_to_device`, the web-installer orchestrator's post-Improv HTTP push, the device-side `?board=` consumer) iterates this map generically — a new leaf in a board entry takes effect everywhere with no device-side, MoonDeck-side, or installer-side code change. Per-board state lives as additional leaves: `Board.board` (the identity name, present in every entry), `Network.txPowerSetting` (e.g. `LOLIN S3 N16R8 → 8`), and any other module/control pair the device exposes — Ethernet RMII clock GPIO, MDIO/MDC pins, default module-config overrides all use the same shape. Catalog fields (`name`, `firmwares`, `default_firmware`) stay top-level so they're never mistaken for control writes.

The device does NOT validate against this catalog — every control accepts any value the injector writes (subject to per-control validation: e.g. the `board` Text control's 32-byte buffer, no NUL inside). The catalog is the injector's responsibility; the device just stores what it's told. This keeps the device decoupled from catalog updates: adding a new board, or a new field for an existing board, doesn't require a firmware change.

A board name that doesn't appear in the catalog renders in MoonDeck's picker as `<name> (unknown)`. MoonDeck falls back to pushing just `Board.board` for unknown names (no other fields exist to push); the web installer's Inject button logs a "no controls for board" warning in the device UI's console and skips the fan-out.

## Files

- `/.config/BoardModule.json` — persisted state (`{ "board": "<name>" }`). Written by FilesystemModule's debounced save when the control is dirty; absent on a fresh device until MoonDeck pushes a value.

## Prior art

This is the device-side seed of a feature that started in MoonLight as the "IO module" (a grab-bag for board presets + I²S peripherals + sensor inputs). projectMM splits the lifecycle: BoardModule owns boot-time hardware identity; a future `PeripheralsModule` or similar will own runtime-attached devices. Conflating the two under "IO" makes the lifecycle layer (boot-time set once vs runtime hot-pluggable) hard to reason about.
