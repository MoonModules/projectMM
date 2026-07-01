# FilesystemModule

![FilesystemModule controls](../../assets/core/FilesystemModule.png)

Persists control values to flash so settings survive a reboot. Always loaded, runs first in the scheduler so its load hook fires before any other module's `setup()`.

## Storage layout

One flat JSON file per top-level module under `/.config/`:

```text
/.config/
  SystemModule.json     → {"deviceName":"MM-TEST","enabled":true}
  NetworkModule.json    → {"ssid":"home","password":"...","addressing":1,
                           "mDNS":true,"ip":"192.168.1.55","gateway":"",
                           "subnet":"255.255.255.0","dns":"","enabled":true}
  Layer.json            → {"channelsPerLight":3,"enabled":true,
                           "0.type":"NoiseEffect","0.scale":12,"0.bpm":60,
                           "0.enabled":true,...}
  Drivers.json      → {"enabled":true,
                           "0.type":"NetworkSendDriver","0.ip":"192.168.1.70",
                           "0.fps":50,"0.enabled":true,...}
```

Filename comes from `MoonModule::typeName()`. Child modules are encoded **positionally** with a `<index>.` key prefix — no nested objects, no arrays. The `type` field per child drives **structural reconciliation** at load time: when the JSON describes a child type at position N that differs from the live tree's child at N (built by `main.cpp`), the loader factory-creates the JSON type, calls its `onBuildControls()`, and swaps it into place. Children present in the live tree but missing from the JSON are torn down and deleted; children in the JSON beyond the live tree's end are appended. Phases 3+4 (`setup`, `onBuildState`) cascade into the reconciled tree, so newly-created children are fully initialized like any other.

`ReadOnly` and `Progress` controls are never persisted — they are derived values, not state.

## Lifecycle

`Scheduler::setup()` runs in four phases:

```text
phase 1  onBuildControls()    every module binds its full control set (incl. hidden)
phase 2  loadAllHook()         FilesystemModule reads files, overlays bound variables
phase 2b rebuildControls()     re-runs onBuildControls so conditional hidden flags see
                               the persisted values (e.g. NetworkModule's static-IP
                               fields become visible after a persisted addressing=1)
phase 3  setup()               modules' own init runs with persisted values in members
phase 4  onBuildState()    buffers sized to final values
```

The Scheduler exposes `setLoadAllHook(LoadAllFn fn)` as a function pointer so it stays independent of FilesystemModule's type (no circular include). FilesystemModule wires the hook from `setScheduler()`.

## Save trigger

HttpServerModule calls `target->markDirty()` and `FilesystemModule::noteDirty()` on every successful mutation: control changes, **and tree-shape changes** (add / delete / move a module — the parent is marked dirty so its file is rewritten with the new child set). `noteDirty()` stamps `lastDirtyMs_` and sets `dirtyPending_`. In `loop1s()`, FilesystemModule waits `DEBOUNCE_MS` (2000ms) after the last dirty mark, then walks the module tree; any subtree with a dirty descendant is serialized to a flat JSON blob and written atomically (write to `.tmp` then rename).

A subtree's dirty flag is cleared only after its write succeeds; a failed write leaves it set so `loop1s()` retries. Losing power before the debounce expires loses the in-flight change — the cost of debouncing for fewer flash writes. `FilesystemModule::flushPending()` forces all dirty subtrees through synchronously; `POST /api/reboot` calls it so an add-then-reboot doesn't lose the change.

The `lastSaved` read-only control shows how long ago the last write happened (`"never"`, `"5s ago"`, `"3m ago"`), refreshed each `loop1s()`.

The `filesystem` progress control shows the config-partition usage (bytes used / total), refreshed each `loop1s()` from `platform::filesystemUsed()` / `filesystemTotal()`. It is bound only when the platform reports a real partition (a chip without a data partition, or desktop, reports 0 and the bar is omitted). This bar lives here, on the module that owns the filesystem — not on SystemModule.

## Conditional visibility (`hidden` flag)

Modules with conditional controls (e.g. NetworkModule's static-IP fields under `addressing=Static`) bind their full control set unconditionally and toggle a `hidden` flag per descriptor:

```cpp
controls_.addText("ip", staticIp_, sizeof(staticIp_));
controls_.setHidden(controls_.count() - 1, addressing_ != 1);
```

This means the persistence layer can find and overlay `ip` regardless of the live conditional state, while the UI honors the hidden flag (`if (ctrl.hidden) continue` in `renderCards`). When a Select changes at runtime, HttpServerModule calls `rebuildControls()` to re-evaluate the flags.

## Platform layer

Filesystem access goes through `platform::fs*` (mount, mkdir, read, atomic write-then-rename, used/total). ESP32 uses LittleFS (`joltwallet/esp_littlefs`) on a dedicated partition; desktop uses `std::filesystem` rooted at `build/` (overridable via `fsSetRoot` for test isolation) so config doesn't clutter the repo root. Save/load shares one 2 KB buffer (`MAX_FILE_BYTES`); a subtree that serializes larger than that fails the write.

## Tests

- Unit test (`test_filesystem_persistence.cpp`):
  - **Value round-trip**: set `deviceName` → save → fresh `Scheduler` + modules → load → assert. Uses `platform::fsSetRoot()` for test isolation. Wall time ~2.3s (the debounce window dominates).
  - **Structural reconciliation**: hand-write a `Layer.json` with one child (RainbowEffect). Build a live tree with two children (NoiseEffect + MultiplyModifier). After load, assert the tree reconciled — RainbowEffect at position 0, the modifier trimmed.

## First boot

No files exist → load is a no-op. Modules run with their default member-initialized values. After the first UI change, FilesystemModule debounces 2s and creates the file. Subsequent boots overlay the persisted values.

## Out of scope

- Presets (`/.config/presets/`)
- Migration between schema versions (e.g. renaming a control). Today, an unknown JSON key is silently ignored and a missing key keeps the default.
- Runtime add/remove via UI (the underlying mechanism is in place — `replaceChildAt`, factory creation, lifecycle propagation — but no UI endpoint yet calls into it).

## Source

[FilesystemModule.cpp](../../../src/core/FilesystemModule.cpp) · [FilesystemModule.h](../../../src/core/FilesystemModule.h)
