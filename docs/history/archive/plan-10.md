# Plan-10 — Control-list-driven JSON persistence (item 11)

## Context

projectMM v3 has no persistence today. Settings (deviceName, ssid/password, effect parameters, mDNS state, ArtNet target IP, grid size) reset on every reboot. The foundation commit `7f9afa3` shipped the partition layout (4MB classic + 16MB S3) and the platform fs API (`fsMount`, `fsRead`, `fsWriteAtomic`, `fsList`, `fsSetRoot`, etc.) plus `MoonModule::dirty_` + `markDirty()` and HttpServerModule's `markDirty()` hooks. Nothing reads/writes config files yet — that's this plan.

Plan-09 attempted this and was abandoned (see `docs/history/plan-09.md`). The failure modes were: nested JSON parser (~250 lines), 4-phase Scheduler reorder + re-load pass, recursive `rebuildControls` during boot, per-module `loadInto(this)` boilerplate, and SystemModule needing a `deviceName_[0] == 0` guard. ~1700 lines of code for the JSON path alone.

This plan stays minimal by:
1. Keeping JSON files **flat** (children encoded with `<index>.` key prefix, not nested objects)
2. Treating conditional controls as **always-bound with a `hidden` flag**, not "add or skip"
3. One Scheduler phase swap (no re-load, no rebuild-during-boot)
4. Reusing the existing flat JSON parser from HttpServerModule

Intended outcome: device boots → reads `/.config/<TypeName>.json` per top-level module → overlays values onto bound control variables → modules run their `setup()` with persisted state in their member vars. Modules themselves remain unaware that persistence exists.

## Decisions already locked

- **Storage:** one flat JSON file per top-level MoonModule under `/.config/`. Children encoded with `<idx>.` key prefix. Reuse existing flat JSON parser.
- **Conditional visibility:** ControlList gains `bool hidden` per descriptor. Modules build their full control set unconditionally; conditional logic flips `hidden`. UI honors the flag.
- **Hidden API:** `ControlList::setHidden(uint8_t index, bool hidden)`. Called right after `addX(...)`. No change to `addX` signatures.
- **JSON helpers:** extract `parseJsonString`/`parseJsonInt`/`parseJsonBool` from HttpServerModule (private static) into a new minimal `src/core/JsonUtil.h` (~50 lines, those three functions ONLY — plan-09 grew this to 256 lines, don't repeat).
- **Lifecycle:** swap Scheduler::setup phase order from `setup→onBuildControls→onAllocateMemory` to `onBuildControls→load→setup→onAllocateMemory`. Single load pass. No re-load. No rebuildControls during boot.
- **Save trigger:** existing `markDirty()` hooks in HttpServerModule. FilesystemModule::loop1s() debounces 2s, walks tree, serializes any subtree with a dirty descendant, atomic write-and-rename, clears dirty flags.
- **First boot:** built-in defaults; files appear lazily after first save.
- **Test scope:** one doctest covering set→save→fresh-instance→load→assert using `platform::fsSetRoot()` for isolation. No persistence-roundtrip live scenario (live runner has no reboot op — documented in CLAUDE.md).
- **No POD constraint:** modules can have any member types. Persistence touches only what was declared via `controls_.addX()`.
- **Constants live in code, not config:** initial defaults remain in member initializers (`uint8_t scale = 4`). Load OVERLAYS those; missing keys keep the default.

## Storage layout (final)

```
/.config/
  SystemModule.json        → {"deviceName":"MM-3A7F","enabled":true}
  NetworkModule.json       → {"ssid":"home","password":"...","addressing":1,
                              "mDNS":true,"ip":"...","gateway":"...",
                              "subnet":"...","dns":"...","enabled":true}
  Layer.json               → {"channelsPerLight":3,"enabled":true,
                              "0.type":"NoiseEffect","0.scale":12,"0.bpm":60,"0.enabled":true,
                              "1.type":"MirrorModifier","1.mirrorX":false,
                              "1.mirrorY":true,"1.mirrorZ":false,"1.enabled":true}
  DriverGroup.json         → {"enabled":true,
                              "0.type":"ArtNetSendDriver","0.ip":"192.168.1.70","0.universe_start":0,
                              "0.fps":50,"0.enabled":true,
                              "1.type":"PreviewDriver","1.fps":20,"1.enabled":true}
  LayoutGroup.json         → {"enabled":true,
                              "0.type":"GridLayout","0.width":128,"0.height":64,"0.depth":1,
                              "0.enabled":true}
```

Filename uses `typeName()` directly. Children identified by position (`0.`, `1.`, etc.) — the `type` field is informational and used to detect tree-shape mismatches (skip-load if the live tree's child[N] is a different type than the persisted blob's child[N]).

`ReadOnly` and `Progress` controls are not persisted (they're derived values).

## Lifecycle

`Scheduler::setup()` runs four phases:

```
phase 1: onBuildControls()    — every module binds its FULL control set
phase 2: loadAllHook()         — FilesystemModule reads files, overlays bound variables
phase 3: setup()               — modules' own init runs with persisted values in member vars
phase 4: onAllocateMemory()    — buffers sized to final values
```

Scheduler exposes `setLoadAllHook(LoadAllFn fn)` taking a function pointer, so it stays independent of FilesystemModule's type (no circular include). FilesystemModule wires it in `setScheduler()`.

NetworkModule's setup() reads `ssid_`/`password_` for the cascade — by then they're already overlaid. SystemModule's `deviceName_` is set from MAC in setup(); since setup() runs AFTER load, we need a guard: only derive from MAC if `deviceName_[0] == 0`. This is the SAME guard plan-09 added; it's correct here because the lifecycle is correct. (Plan-09's problem was the secondary re-load pass overwriting things.)

## Conditional visibility — example

NetworkModule today:
```cpp
void onBuildControls() override {
    controls_.addReadOnly("status", statusStr_, sizeof(statusStr_));
    controls_.addText("ssid", ssid_, sizeof(ssid_));
    controls_.addText("password", password_, sizeof(password_));
    controls_.addSelect("addressing", addressing_, addressingOptions_, 2);
    controls_.addBool("mDNS", mdnsEnabled_);
    if (addressing_ == 1) {
        controls_.addText("ip", staticIp_, sizeof(staticIp_));
        controls_.addText("gateway", staticGateway_, sizeof(staticGateway_));
        controls_.addText("subnet", staticSubnet_, sizeof(staticSubnet_));
        controls_.addText("dns", staticDns_, sizeof(staticDns_));
    }
}
```

After plan-10:
```cpp
void onBuildControls() override {
    controls_.addReadOnly("status", statusStr_, sizeof(statusStr_));
    controls_.addText("ssid", ssid_, sizeof(ssid_));
    controls_.addText("password", password_, sizeof(password_));
    controls_.addSelect("addressing", addressing_, addressingOptions_, 2);
    controls_.addBool("mDNS", mdnsEnabled_);
    controls_.addText("ip", staticIp_, sizeof(staticIp_));
    controls_.setHidden(controls_.count() - 1, addressing_ != 1);
    controls_.addText("gateway", staticGateway_, sizeof(staticGateway_));
    controls_.setHidden(controls_.count() - 1, addressing_ != 1);
    controls_.addText("subnet", staticSubnet_, sizeof(staticSubnet_));
    controls_.setHidden(controls_.count() - 1, addressing_ != 1);
    controls_.addText("dns", staticDns_, sizeof(staticDns_));
    controls_.setHidden(controls_.count() - 1, addressing_ != 1);
}
```

Persistence load can find `ip` etc. because they're always bound. Toggling `addressing` triggers a Select-change in HttpServerModule which already calls `rebuildControls()` — that re-runs `onBuildControls`, flipping the hidden flags fresh.

## File-by-file change list

**New files:**
- `src/core/JsonUtil.h` — ~50 lines. Contains EXACTLY `parseJsonString`, `parseJsonInt`, `parseJsonBool` (moved verbatim from HttpServerModule's private statics into `mm::json` namespace). **STRICT: no other functions. plan-09 grew this to 256 lines and that was a warning sign.**
- `src/core/FilesystemModule.h` — ~200 lines. Header-only per CLAUDE.md style. Contains:
  - `setScheduler()` — wires the load hook into Scheduler
  - `setup()` — mounts the filesystem
  - `loop1s()` — debounced save walk
  - `loadAllHook_` (static C-function) — Scheduler calls this between phase 1 and phase 3
  - `loadSubtree()`, `applyNode()`, `applyValue()` — load path
  - `saveSubtree()`, `writeNode()`, `writeValue()` — save path
  - `subtreeDirty()`, `clearSubtreeDirty()` — dirty walking
  - `instance_` singleton + `noteDirty()` static API (the existing `target->markDirty()` is enough; FilesystemModule polls dirty flags in loop1s, no need for noteDirty)
- `test/test_filesystem_persistence.cpp` — ~80 lines. One TEST_CASE: set deviceName → save → recreate Scheduler+modules → load → assert deviceName matches.

**Modified files:**
- `src/core/Control.h` — add `bool hidden = false;` to `ControlDescriptor` struct; add `void setHidden(uint8_t i, bool h)` method to `ControlList`.
- `src/core/Scheduler.h` — swap phase order in `setup()`. Add `LoadAllFn` typedef + `setLoadAllHook()` + private `loadAllHook_` field.
- `src/core/HttpServerModule.h` — remove the three flat parseJsonX helpers from private statics (or leave them as thin delegates calling `mm::json::*`). Add `,"hidden":%s` field to `writeControls()` per-type branches.
- `src/core/NetworkModule.h` — convert conditional `if (addressing_ == 1)` block to "always add + setHidden". Remove the `rebuildLocalControlsAndPipeline` if no longer needed (the runtime Select-change path uses HttpServerModule's `rebuildControls()` which already does this).
- `src/core/SystemModule.h` — add `if (deviceName_[0] == 0)` guard around the MAC-derived default in setup().
- `src/core/MoonModule.h` — no change. `rebuildControls()` + `clearControlsRecursive()` from pile A remain useful for the Select-change path; they're NOT called during boot.
- `src/main.cpp` — create FilesystemModule first (`factory.create("FilesystemModule")`), `setScheduler(&scheduler)`, `setName("Filesystem")`, register it as the first scheduler module.
- `src/ui/app.js` — in `renderCards()` skip `if (ctrl.hidden) continue` when iterating controls.
- `test/CMakeLists.txt` — add `test_filesystem_persistence.cpp`.

**Documentation:**
- `docs/moonmodules/core/FilesystemModule.md` — new spec doc, ~80 lines. Storage layout, lifecycle, save trigger, hidden flag, ESP32 partition, platform API. Match the existing module spec doc style.
- `docs/moonmodules/core/MoonModule.md` — note the `hidden` flag and `setHidden`.
- `docs/architecture.md` — short Persistence section between Controls and Rebuild Propagation: describes the 4-phase Scheduler setup, the load hook pattern, the hidden flag, debounced save.
- `docs/testing.md` — entry for `test_filesystem_persistence.cpp`.
- `docs/plan.md` — remove item 11 once complete.

## Save/load flow (pseudocode)

```text
FilesystemModule::setup():
    platform::fsMount()
    cleanupTmpFiles_("/.config")    # one-shot recursive .tmp removal
    platform::fsMkdir("/.config")

FilesystemModule::loadAllHook_(Scheduler* s):     # called by Scheduler in phase 2
    if (!instance_) return
    for each top-level module m in s:
        instance_->loadSubtree(m)

loadSubtree(m):
    char path[64]
    snprintf(path, "/.config/%s.json", m->typeName())
    char buf[2048]
    if platform::fsRead(path, buf, sizeof(buf)) > 0:
        applyNode(m, buf, prefix="")

applyNode(m, json, prefix):
    char key[48]
    for each control c in m->controls():
        if c.type in (ReadOnly, Progress): continue
        snprintf(key, "%s%s", prefix, c.name)
        applyValue(c, json, key)         # parseJsonInt/Bool/String based on c.type
    snprintf(key, "%senabled", prefix)
    if hasKey(json, key):
        m->setEnabled(parseJsonBool(json, key))
    for i in m->childCount():
        snprintf(childPrefix, "%s%u.", prefix, i)
        applyNode(m->child(i), json, childPrefix)

FilesystemModule::loop1s():
    if !mounted_ or scheduler_ == nullptr: return
    if no module has dirty(): return
    if (millis() - lastDirtyMs_) < 2000: return   # debounce
    for each top-level m in scheduler_:
        if subtreeDirty(m): saveSubtree(m); clearSubtreeDirty(m)
    lastDirtyMs_ = 0

saveSubtree(m):
    char buf[2048]; int pos = 0
    pos += snprintf("{")
    pos += writeNode(m, buf+pos, ..., prefix="")
    pos += snprintf("}")
    char path[64]; snprintf(path, "/.config/%s.json", m->typeName())
    platform::fsWriteAtomic(path, buf, pos)
```

`markDirty` is set by HttpServerModule on every successful control mutation (already in place). FilesystemModule never sees the mutation directly — it just polls `dirty()` in loop1s. No `noteDirty` callback API needed.

## Critical files for implementation

- [src/core/JsonUtil.h](src/core/JsonUtil.h) (new)
- [src/core/FilesystemModule.h](src/core/FilesystemModule.h) (new)
- [src/core/Control.h](src/core/Control.h)
- [src/core/Scheduler.h](src/core/Scheduler.h)
- [src/core/HttpServerModule.h](src/core/HttpServerModule.h)
- [src/core/NetworkModule.h](src/core/NetworkModule.h)
- [src/core/SystemModule.h](src/core/SystemModule.h)
- [src/main.cpp](src/main.cpp)
- [src/ui/app.js](src/ui/app.js)
- [test/test_filesystem_persistence.cpp](test/test_filesystem_persistence.cpp) (new)

## Existing utilities to reuse (do NOT duplicate)

- Flat JSON parsers in HttpServerModule.h (private statics today) → move to JsonUtil.h, then HttpServerModule + FilesystemModule both use them
- `MoonModule::dirty_` / `markDirty()` / `clearDirty()` / `dirty()` — already on every module from foundation commit 7f9afa3
- `MoonModule::typeName()` — used for filename construction
- `MoonModule::rebuildControls()` / `clearControlsRecursive()` — used by HttpServerModule's Select-change path (no change there)
- `platform::fsMount/fsRead/fsWriteAtomic/fsList/fsMkdir/fsSetRoot` — from foundation commit
- `Buffer::clear()` and `Buffer::data()` — unchanged
- HttpServerModule's `writeControls()` JSON-emit pattern — mirror it for the save path's `writeValue()` per-ControlType branches

## Sequencing inside the PR

1. Move 3 parseJsonX helpers from HttpServerModule.h to `src/core/JsonUtil.h`. Update HttpServerModule to use `mm::json::*` (or thin delegates). Build + tests green.
2. Add `bool hidden` to `ControlDescriptor`. Add `ControlList::setHidden`. Add `,"hidden":%s` to writeControls output. Update app.js to skip hidden. Verify desktop UI still works.
3. Convert NetworkModule's conditional block to always-add + setHidden. Verify UI shows static-IP fields with hidden flag flipping correctly.
4. Add `Scheduler::setLoadAllHook` + `LoadAllFn` typedef. Swap phase order. No hook wired yet — but the new order should still work because all modules' setup() is robust to being called with default-or-overlaid values.
5. Add `SystemModule` `deviceName_[0] == 0` guard.
6. Add `FilesystemModule.h` with the full save/load implementation. Register in `main.cpp` BEFORE SystemModule. Verify the load hook gets called and the file paths line up.
7. Add `test/test_filesystem_persistence.cpp`. Run with `platform::fsSetRoot()` isolation.
8. Add docs: spec, architecture section, testing entry.
9. Full pre-commit checklist (10 steps).

## Pre-commit checklist (CLAUDE.md mandatory order)

| # | Check | Command |
|---|-------|---------|
| 1 | Desktop build | `cmake --build /Users/ewoud/Developer/GitHub/ewowi/projectMM-v3/build` (zero warnings) |
| 2 | Unit tests | `cd build && ctest --output-on-failure` |
| 3 | Scenario tests | `./build/test/mm_scenarios` (SIGABRT exit pre-existing on HEAD — accept) |
| 4 | Platform boundary | `python3 scripts/check/check_platform_boundary.py` — verify no platform leakage in FilesystemModule.h |
| 5 | Spec check | `python3 scripts/check/check_specs.py` — confirms FilesystemModule.md describes the implemented API |
| 6 | ESP32 build | `python3 scripts/build/build_esp32.py` — clean. Verify partition + LittleFS still work. |
| 7 | Reviewer agent | Opus reviewer over staged diff. Flag: no heap alloc in `loop1s()` save path (only stack buffers); platform boundary clean; no duplication of JSON helpers; JsonUtil.h stays at ~50 lines (not growing into a JSON library). |
| 8 | KPI collection | `python3 scripts/check/collect_kpi.py --commit` |
| 9 | Live scenarios | Run on ESP32 hardware: existing 7 scenarios pass. Manual: set deviceName via REST → reboot → verify deviceName persisted. |
| 10 | Documentation | spec + architecture + testing updated; item 11 removed from `docs/plan.md`. |

## Verification end-to-end

After implementation, on real ESP32 hardware:

1. `esptool.py erase_flash` (one-time cleanup — should not be needed since the partition layout didn't change from foundation commit, but a fresh start removes any leftover state)
2. `idf.py build flash monitor` — boots cleanly, log shows `FilesystemModule: mounted`, `/.config/` empty, default deviceName `MM-XXXX`
3. Open `http://<ip>/`, change deviceName to `MM-TEST`
4. Wait ≥3 seconds; serial monitor shows save log line `FilesystemModule: saved /.config/SystemModule.json`
5. Power-cycle the board
6. UI top bar shows `MM-TEST`; `/api/state` confirms deviceName = `MM-TEST`
7. Set Network.ssid to a real WiFi network, set addressing = Static, set ip/gateway/subnet, reboot
8. After reboot: Network controls show the static-IP fields visible (because addressing == Static was persisted); device connects with static IP

If any step fails, do not commit; investigate.

## Out of scope (explicit follow-ups)

- **PSRAM-backed config cache** for fast preset switching (when PSRAM is detected)
- **Structural persistence** (add/remove children) — current scope only persists control values + enabled flag
- **Live scenario runner reboot support** — needed for an automated persistence-roundtrip live test
- **Presets** — `/.config/presets/` for named bundles of control values
- **`platform::ethPresent()` / `wifiPresent()`** — deferred with WiFi runtime-disable backlog
- **Plan 11.5** (free-then-allocate pipeline rebuild) — fully separate, does not block this plan
