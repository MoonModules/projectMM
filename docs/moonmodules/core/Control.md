# Control

A named, typed value a MoonModule exposes to the UI. Kept as lightweight as possible.

## Design

Controls bind to a class variable **by reference** — the descriptor stores a pointer, hot-path code reads the variable directly (zero overhead, no getter/setter). The value lives in the class variable (1–4 bytes); the descriptor is just the metadata UI rendering and persistence need.

## Types

Each `controls_.addX(name, var, …)` call (signatures in `Control.h`) binds one of:

| Type | Storage | UI | DMX |
|------|---------|-----|-----|
| Uint8 | 1 byte, min/max | Slider (0–255) | Yes — preferred default |
| Uint16 | 2 bytes | Number input | Yes (universe, port) |
| Int16 | 2 bytes, min/max | Slider (bounded; unbounded → ±percentage slider) | Yes |
| Pin | 1 byte (`int8_t`), −1 = unused | Number input | No |
| Bool | 1 byte | Toggle | Yes (0/1) |
| Text | `char[N]` | Text input | No |
| Password | `char[N]` | Masked input, hold-to-peek | No |
| ReadOnly | `char[N]` | Display text | No |
| ReadOnlyInt | 1 byte + unit string in `aux` | Display `<value> <unit>` | No |
| Select | uint8_t index + options in `aux` | Dropdown | Yes (mode) |
| Progress | uint32_t value + total | Progress bar | No |
| IPv4 | uint8_t[4] | Dotted-quad text input | No |

Notes on the non-obvious ones (the rest are self-describing):

- **Password** serializes XOR-obfuscated + base64 over `/api/state`, not plaintext — a first line of defence, trivially reversible by design (the XOR key is shared with `app.js`), not encryption.
- **Int16** is for coordinate-style values where negatives are legal — e.g. a Layer's `startX`/`endX` dragged outside the visible grid by a modifier. Default bounds are the full int16 range; pass explicit bounds for a tighter one. The UI renders it as a slider (an unbounded int16 falls back to a ±percentage slider for Layer positions).
- **Pin** is a GPIO number — `int8_t` (one byte; a GPIO never exceeds ~54), `−1` = unused/default. Distinct from Int16 so the UI renders a plain **number input** (a GPIO has no meaningful range to drag) and to keep the byte. `min`/`max` are the valid-GPIO span, used only as a server-side write-clamp. [NetworkModule](NetworkModule.md)'s eth pin controls are the first users; LED-driver pins follow.
- **ReadOnlyInt** stores 1 byte + a unit suffix instead of a ~10-byte string — see [coding-standards § Prefer integers](../../coding-standards.md#prefer-integers-store-values-in-their-native-shape). [NetworkModule](NetworkModule.md)'s `rssi` (`-58 dBm`) and `txPower` (`19 dBm`) are the first users.
- **IPv4** stores 4 bytes but converts to/from the dotted-quad string at the JSON boundary (`parseDottedQuad`/`formatDottedQuad` in `Control.h`, used by API, persistence, and scenario set-control). Used for [NetworkModule](NetworkModule.md)'s static-IP fields.

No RGB color-picker type — effects use a palette index (uint8_t) instead. `float` and `Coord3D` exist but are used minimally; prefer uint8_t.

## Memory footprint

Target: under 16 bytes per descriptor (variable pointer + flash name pointer + type enum + type-dependent min/max). Descriptors live in a fixed-capacity per-module array — no per-control heap allocation. A module that overflows the default capacity is probably too complex.

## Persistence and dynamic rebuild

Control values persist via [FilesystemModule](FilesystemModule.md), which overlays loaded values through each control's pointer during `onBuildControls()`. Calling `onBuildControls()` again at runtime (e.g. when a Select changes) clears and rebuilds the set, so only controls relevant to the current mode are shown — this is how conditional `hidden` flags re-evaluate.

## Tests

[Unit tests: MoonModule](../../tests/unit-tests.md#moonmodule) — control binding by reference, pointer read/write, clear and rebuild.

## Prior art

### MoonLight — addControl ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Nodes.h#L80))

Binds via `reinterpret_cast<uintptr_t>(&variable)`; UI types "slider"/"select"/"toggle"/"text"/"display".

### projectMM v1 — addControl ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/core/StatefulModule.h))

Same pattern; also "display"/"progress"/"button".

### projectMM v2 — ControlDescriptor ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/core/MoonModule.h#L40))

Richer but heavier (default, options array, ownsOptions, system flags) — not all that weight is justified here. Persisted values applied via an `applyPending_` overlay during `onBuildControls()`; projectMM keeps the same timing.

## Source

[Control.cpp](../../../src/core/Control.cpp) · [Control.h](../../../src/core/Control.h)
