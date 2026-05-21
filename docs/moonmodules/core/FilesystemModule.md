# FilesystemModule

Persists control values to flash so settings survive a reboot. Always loaded, runs first in the scheduler so its load hook fires before any other module's `setup()`.

## Storage layout

One flat JSON file per top-level module under `/.config/`:

```
/.config/
  SystemModule.json     → {"deviceName":"MM-TEST","enabled":true}
  NetworkModule.json    → {"ssid":"home","password":"...","addressing":1,
                           "mDNS":true,"ip":"192.168.1.55","gateway":"",
                           "subnet":"255.255.255.0","dns":"","enabled":true}
  Layer.json            → {"channelsPerLight":3,"enabled":true,
                           "0.type":"NoiseEffect","0.scale":12,"0.bpm":60,
                           "0.enabled":true,...}
  DriverGroup.json      → {"enabled":true,
                           "0.type":"ArtNetSendDriver","0.ip":"192.168.1.70",
                           "0.fps":50,"0.enabled":true,...}
```

Filename comes from `MoonModule::typeName()`. Child modules are encoded **positionally** with a `<index>.` key prefix — no nested objects, no arrays. The `type` field per child drives **structural reconciliation** at load time: when the JSON describes a child type at position N that differs from the live tree's child at N (built by `main.cpp`), the loader factory-creates the JSON type, calls its `onBuildControls()`, and swaps it into place. Children present in the live tree but missing from the JSON are torn down and deleted; children in the JSON beyond the live tree's end are appended. Phases 3+4 (`setup`, `onAllocateMemory`) cascade into the reconciled tree, so newly-created children are fully initialized like any other.

`ReadOnly` and `Progress` controls are never persisted — they are derived values, not state.

## Lifecycle

`Scheduler::setup()` runs in four phases:

```
phase 1  onBuildControls()    every module binds its full control set (incl. hidden)
phase 2  loadAllHook()         FilesystemModule reads files, overlays bound variables
phase 2b rebuildControls()     re-runs onBuildControls so conditional hidden flags see
                               the persisted values (e.g. NetworkModule's static-IP
                               fields become visible after a persisted addressing=1)
phase 3  setup()               modules' own init runs with persisted values in members
phase 4  onAllocateMemory()    buffers sized to final values
```

The Scheduler exposes `setLoadAllHook(LoadAllFn fn)` as a function pointer so it stays independent of FilesystemModule's type (no circular include). FilesystemModule wires the hook from `setScheduler()`.

## Save trigger

HttpServerModule calls `target->markDirty()` and `FilesystemModule::noteDirty()` on every successful control mutation. `noteDirty()` stamps `lastDirtyMs_` and sets `dirtyPending_`. In `loop1s()`, FilesystemModule waits `DEBOUNCE_MS` (2000ms) after the last dirty mark, then walks the module tree; any subtree with a dirty descendant is serialized to a flat JSON blob and written atomically (write to `.tmp` then rename).

After writing, `clearSubtreeDirty()` clears the dirty flags. Tearing down or losing power before debounce expires loses the in-flight change — that's the cost of debouncing in exchange for fewer flash writes.

## Conditional visibility (`hidden` flag)

Modules with conditional controls (e.g. NetworkModule's static-IP fields under `addressing=Static`) bind their full control set unconditionally and toggle a `hidden` flag per descriptor:

```cpp
controls_.addText("ip", staticIp_, sizeof(staticIp_));
controls_.setHidden(controls_.count() - 1, addressing_ != 1);
```

This means the persistence layer can find and overlay `ip` regardless of the live conditional state, while the UI honors the hidden flag (`if (ctrl.hidden) continue` in `renderCards`). When a Select changes at runtime, HttpServerModule calls `rebuildControls()` to re-evaluate the flags.

## Platform requirements

- `platform::fsMount()` — mount the filesystem (idempotent)
- `platform::fsMkdir(path)` — create `/.config/` on first boot
- `platform::fsRead(path, buf, bufLen)` — read file into buffer, returns size or ≤0
- `platform::fsWriteAtomic(path, buf, len)` — write to `<path>.tmp` then rename
- `platform::filesystemUsed/filesystemTotal()` — for SystemModule's progress bar
- `platform::fsSetRoot(path)` — desktop-only: redirect root for test isolation

ESP32 uses LittleFS via the `joltwallet/esp_littlefs` component on a dedicated partition. Desktop uses `std::filesystem` rooted at `build/` (overridable via `fsSetRoot`) — config lives under the gitignored build dir so it doesn't clutter the repo root.

## Sizing

- `MAX_FILE_BYTES = 2048` — max serialized subtree size; write returns false on overflow
- `MAX_PATH = 64`, `MAX_KEY = 48` — stack buffers for path/key construction
- All save/load buffers are stack-allocated; no heap allocation in the hot path

## Tests

- Unit test (`test_filesystem_persistence.cpp`):
  - **Value round-trip**: set `deviceName` → save → fresh `Scheduler` + modules → load → assert. Uses `platform::fsSetRoot()` for test isolation. Wall time ~2.3s (the debounce window dominates).
  - **Structural reconciliation**: hand-write a `Layer.json` with one child (RainbowEffect). Build a live tree with two children (NoiseEffect + MirrorModifier). After load, assert the tree reconciled — RainbowEffect at position 0, Mirror trimmed.

## First boot

No files exist → load is a no-op. Modules run with their default member-initialized values. After the first UI change, FilesystemModule debounces 2s and creates the file. Subsequent boots overlay the persisted values.

## Out of scope

- Presets (`/.config/presets/`)
- Migration between schema versions (e.g. renaming a control). Today, an unknown JSON key is silently ignored and a missing key keeps the default.
- Runtime add/remove via UI (the underlying mechanism is in place — `replaceChildAt`, factory creation, lifecycle propagation — but no UI endpoint yet calls into it).
