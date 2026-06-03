# Control

A named, typed value exposed by a MoonModule to the UI. Must be the lightest weight possible.

## Design

Controls bind to class variables by reference. The control stores a pointer to the variable. Hot-path code reads the variable directly — zero overhead. No getter/setter.

## Types

| Type | Storage | UI | DMX-compatible |
|------|---------|-----|----------------|
| uint8_t | 1 byte, min/max | Slider (0-255) | Yes — preferred default |
| uint16_t | 2 bytes | Number input | Yes (universe number, port) |
| int16_t | 2 bytes, min/max | Number input (signed) | Yes |
| bool | 1 byte | Toggle | Yes (0/1) |
| char[N] | fixed buffer | Text input | No |
| Password | char[N] buffer | Password input (masked) | No |
| ReadOnly | char[N] buffer | Display text (read-only) | No |
| ReadOnlyInt | 1 byte (int8_t) + unit string in aux | Display int + unit suffix (read-only) | No |
| Select | uint8_t index | Dropdown | Yes (mode selection) |
| Progress | uint32_t value + total | Progress bar | No |
| IPv4 | 4 bytes (uint8_t[4]) | Text input (dotted-quad) | No |

Additional types — add only when needed:
- float — use minimally, prefer uint8_t where possible
- Coord3D — for position controls

### Password

Text whose value is a secret. Binds to a `char[]` buffer like `Text`, but `/api/state` serializes it XOR-obfuscated + base64-encoded rather than in plaintext — a first line of defence so the password is not plainly readable in a raw API response. The obfuscation key is a shared constant (firmware + `app.js`), so it is trivially reversible by design, not encryption. The UI renders a masked input with a hold-to-peek button. Persists to flash like any text control.

`controls_.addPassword("password", buf, sizeof(buf));`

### ReadOnly

Display-only text value. Binds to a `char[]` buffer. The module updates the buffer in `loop1s()` and the UI renders it as static text. Cannot be set by the user or via API. Used for status strings (uptime, IP address, chip info).

`controls_.addReadOnly("status", buf, sizeof(buf));`

### Select

Dropdown with named options. Binds to a `uint8_t` index. Options are stored as a `const char* const*` array pointer in `aux`. Option count is stored in `max`. Used for mode selection (DHCP/Static, effect type). When the selected value changes, `onBuildControls()` can rebuild to show/hide dependent controls.

`controls_.addSelect("mode", modeVar, optionsArray, optionCount);`

### Progress

Bar showing value/total. Binds to a `uint32_t` value. Total capacity is stored in `aux`. Used for heap usage, buffer fill level. Read-only — value updated by module code.

`controls_.addProgress("freeHeap", heapVar, totalHeap);`

### Int16

Signed 16-bit integer. Used for coordinate-style values where negatives are legal — e.g. a Layer's `startX` / `startY` / `startZ` / `endX` / `endY` / `endZ` controls, which can fall outside the visible grid when a modifier translates the layer. Stored in 2 bytes. UI renders a bounded number input (the default `min` = `INT16_MIN`, `max` = `INT16_MAX`; pass explicit bounds for a tighter range like `addInt16("width", w, 1, 512)`).

`controls_.addInt16("startX", startX, -1000, 1000);`

### ReadOnlyInt

Display-only signed int (1 byte) with a unit-suffix string carried in the descriptor's `aux` slot. UI renders `"<value> <unit>"` verbatim. Used for numeric telemetry that the UI shouldn't bloat into a per-instance char buffer — see [coding-standards.md § Prefer integers](../../coding-standards.md#prefer-integers-store-values-in-their-native-shape). [NetworkModule](NetworkModule.md)'s `rssi` (`-58 dBm`) and `txPower` (`19 dBm`) are the first users. 1 byte storage where a string would be ~10 bytes — used for RSSI, TX power, future numeric telemetry.

`controls_.addReadOnlyInt("rssi", rssi_, "dBm");`

### IPv4

Dotted-quad IP address. Binds to a `uint8_t[4]` array. The wire format stays a string (`"192.168.1.1"`) at the JSON boundary — `parseDottedQuad` / `formatDottedQuad` in `Control.h` handle the conversion at the API + persistence + scenario set-control sites — but device-side storage is 4 bytes instead of ~16 for the dotted-quad string. Used for [NetworkModule](NetworkModule.md)'s static-IP / gateway / subnet / DNS fields.

`controls_.addIPv4("ip", octets_);`  // octets_ is `uint8_t[4]`

No color picker (RGB) control type — effects use palette index (uint8_t) instead.

## Memory footprint

Controls must be as small as possible. Each control descriptor stores: pointer to variable (4 bytes on ESP32), name pointer to flash (4 bytes), type enum (1 byte), min/max (type-dependent). Target: under 16 bytes per control descriptor.

This 16-byte target is for the in-memory runtime descriptor only. The current approach: control VALUE lives in the class variable (1-4 bytes, no overhead), control DESCRIPTOR is the lightweight metadata for UI rendering and persistence.

Fixed-capacity array per module. No heap allocation per control. Capacity chosen at compile time — if a module needs more controls than the default capacity, it's probably too complex.

## Persistence

Control values must be saved/loaded persistently (filesystem). The mechanism:
- **Save:** iterate control descriptors, read each variable via its pointer, write key:value to a file. Format: compact binary or minimal JSON — TBD based on what's simplest.
- **Load:** read the file at setup, before `onBuildControls()` runs, into a pending-values store; the values are then written through each control's pointer during `onBuildControls()` (the overlay-in-onBuildControls step).
- **When:** save on control change (debounced).
- **Scope:** one file per module instance, or one file for all modules — decide based on filesystem constraints (LittleFS on ESP32 has limited file count).

## Dynamic controls

`onBuildControls()` in MoonModule supports rebuilding the control set at runtime. When a mode selector changes, call `onBuildControls()` again — it clears and rebuilds, showing only controls relevant to the current mode.

## Tests

[Unit tests: MoonModule](../../tests/unit-tests.md#moonmodule) — control binding by reference, pointer read/write, clear and rebuild.

## Prior art

### MoonLight — addControl ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Nodes.h#L80))

- Binds via `reinterpret_cast<uintptr_t>(&variable)`. Types: uint8_t, int8_t, uint16_t, uint32_t, int, float, bool, Coord3D. UI types: "slider", "select", "toggle", "text", "display". Select via `addControlValue()`.

### projectMM v1 — addControl ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/core/StatefulModule.h))

- Same pattern. Also supports "display", "progress", "button".

### projectMM v2 — ControlDescriptor ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/MoonModule.h#L40))

- Richer but heavier: key, uiType, CtrlType enum, ptr, min/max, default, options array, ownsOptions flag, system flag. Not all of this weight is justified here.
- Persisted values applied via an `applyPending_` step during `onBuildControls()` — projectMM follows the same overlay-in-onBuildControls timing.
