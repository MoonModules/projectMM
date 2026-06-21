# Plan: Live Scenario Testing (Item 8)

## Context

Add live scenario testing: a Python runner that replays scenario JSON files via HTTP against a running device (desktop or ESP32). Same JSON format as the in-process runner. Includes per-step performance measurements (FPS, heap) and baseline regression detection. Full module CRUD via REST API.

## What needs to happen

### 1. HTTP API additions (HttpServerModule)

New endpoints:
- `GET /api/system` — returns FPS, heap free, heap max block, uptime. Needed for performance measurements after each step.
- `POST /api/modules` — create a module: `{"type":"NoiseEffect","id":"noise","parent_id":"layer"}`. HttpServerModule creates the module and wires it into the tree. Triggers pipeline rebuild.
- `DELETE /api/modules/{name}` — remove a module by name. Teardown, unwire, rebuild.

These require a **module registry** — a way to create modules by type name at runtime. Currently modules are stack-allocated in main.cpp. For dynamic creation, they need to be heap-allocated with a factory.

### 2. Module Factory

A simple registry mapping type name → create function. Lives in core (domain-neutral):
```cpp
// In main.cpp or a new ModuleFactory.h
using CreateFn = MoonModule*(*)();
struct ModuleFactory {
    static MoonModule* create(const char* type);
    static void registerType(const char* type, CreateFn fn);
};
```

Registration happens in main.cpp:
```cpp
ModuleFactory::registerType("NoiseEffect", []() -> MoonModule* { return new NoiseEffect(); });
ModuleFactory::registerType("RainbowEffect", []() -> MoonModule* { return new RainbowEffect(); });
// etc.
```

HttpServerModule calls `ModuleFactory::create(type)` in `POST /api/modules`. The factory returns a heap-allocated module. The caller (HttpServerModule) adds it to the appropriate parent via `childCount()`/`child()` — but wait, we need an `addChild()` method too.

### 3. Generic addChild on MoonModule

Currently `addEffect()`, `addModifier()`, `addDriver()`, `addLayout()` are type-specific. For dynamic add from HTTP, we need a generic `addChild(MoonModule*)` that each container overrides:

```cpp
// MoonModule base
virtual bool addChild(MoonModule*) { return false; }
virtual bool removeChild(MoonModule*) { return false; }
```

Overridden in Layer (adds as effect or modifier based on type), DriverGroup (adds as driver), LayoutGroup (adds as layout). The HTTP handler calls `parent->addChild(newModule)`.

But how does addChild know if it's an effect or modifier? The module itself knows — EffectBase vs ModifierBase. The container can try: if `dynamic_cast<EffectBase*>` succeeds, add as effect. But RTTI is disabled on ESP32.

Alternative: the factory also stores the "role" (effect/modifier/driver/layout). Or: addChild uses a type tag.

Simplest: add a virtual `moduleRole()` to MoonModule:
```cpp
enum class ModuleRole : uint8_t { Generic, Effect, Modifier, Driver, Layout };
virtual ModuleRole role() const { return ModuleRole::Generic; }
```

EffectBase returns Effect, ModifierBase returns Modifier, etc. Then `addChild` switches on role.

### 4. System metrics endpoint

`GET /api/system` returns:
```json
{
    "fps": 15,
    "freeHeap": 124316,
    "maxBlock": 63488,
    "uptime": 12345
}
```

HttpServerModule tracks FPS by counting frames in loop() — but HttpServerModule uses loop20ms, not loop. Better: read from the main loop's frame counter. Or: add a simple counter to the Scheduler.

Simplest: Scheduler already has `elapsed()`. Add `fps()` that tracks frames per second. The main loop in mm_main already counts frames — expose that.

Actually, for live scenarios we just need the values. The Python runner calls `GET /api/system` after each step, waits for settle time, then reads. The FPS and heap come from the platform.

### 5. Python live scenario runner

`scripts/scenario/run_live_scenario.py`:
- Connects to a device via HTTP (host:port)
- Reads scenario JSON (same format as in-process)
- Executes steps:
  - `add_module` → POST /api/modules
  - `set_control` → POST /api/control
  - After each step with `"measure": true`:
    - Wait settle time (1-2 seconds)
    - GET /api/system → record FPS, heap
    - Check bounds
- Reports results
- Baseline support: `--compare-baseline`, `--update-baseline`

### 6. MoonDeck Live tab

- Device discovery: scan subnet, probe `/api/state`
- Device selector (checkboxes)
- Run scenario against selected device
- Show results

## Files

```
src/core/MoonModule.h                 # MODIFY: add ModuleRole, addChild, removeChild
src/core/ModuleFactory.h              # NEW: type registry, create by name
src/core/HttpServerModule.h           # MODIFY: POST /api/modules, DELETE, GET /api/system
src/core/Scheduler.h                  # MODIFY: add fps tracking
src/light/EffectBase.h                # MODIFY: role() returns Effect
src/light/ModifierBase.h              # MODIFY: role() returns Modifier
src/light/DriverGroup.h               # MODIFY: addChild/removeChild, role()
src/light/LayoutGroup.h               # MODIFY: addChild/removeChild, role()
src/light/Layer.h                     # MODIFY: addChild/removeChild, role()
src/main.cpp                          # MODIFY: register module types with factory
scripts/scenario/run_live_scenario.py # NEW: Python HTTP scenario runner
scripts/moondeck_config.json          # MODIFY: add Live tab entries
scripts/moondeck_ui/index.html        # MODIFY: Live tab content
scripts/moondeck_ui/app.js            # MODIFY: device discovery UI
test/scenarios/control-change.json    # NEW: scenario with set_control steps
docs/moonmodules/core/HttpServerModule.md # MODIFY: new endpoints
docs/testing.md                       # MODIFY: live scenario section
```

## Implementation Steps

### Step 1: ModuleRole + addChild/removeChild

Add virtual `role()` and `addChild()`/`removeChild()` to MoonModule base. Override in containers (Layer, DriverGroup, LayoutGroup) and base classes (EffectBase, ModifierBase, DriverBase, LayoutBase). Lifecycle-aware: addChild calls setup/onBuildControls/onAllocateMemory on new child if parent is already running.

### Step 2: ModuleFactory

Simple static registry. `registerType(name, createFn)`. `create(name)` returns heap-allocated module. Registration in main.cpp for all known types.

### Step 3: HTTP endpoints

- `GET /api/system` — FPS (from Scheduler), freeHeap, maxAllocBlock, uptime
- `POST /api/modules` — parse JSON, create via factory, find parent, addChild, rebuild
- `DELETE /api/modules/{name}` — find module, parent->removeChild, teardown, delete

### Step 4: Scheduler FPS tracking

Add frame counter and FPS to Scheduler, updated in `tick()`.

### Step 5: Python live scenario runner

Adapted from v1's `scenario.py`. HTTP client using urllib. Per-step measurements: wait, GET /api/system, record, check bounds. Baseline JSON file.

### Step 6: MoonDeck integration

Live tab: device discovery (subnet scan + /api/state probe), scenario execution against selected device.

### Step 7: New scenario + docs

`control-change.json` — scenario that changes controls and measures impact. Update testing.md and HttpServerModule.md.

## Verification

1. Desktop build + tests pass
2. In-process scenarios still pass
3. Start mmv3, run live scenario against localhost:8080 — steps execute, measurements collected
4. POST /api/modules creates a new effect visible in UI
5. DELETE removes it
6. GET /api/system returns valid FPS/heap
7. ESP32: run live scenario against device IP
8. Platform boundary check passes
