# Architecture

This document describes the system as it is. Coding conventions live in [coding-standards.md](coding-standards.md); how to build and run lives in [building.md](building.md); what is tested lives in [testing.md](testing.md).

## Contents

- [The problem](#the-problem)
- [Core and light domain](#core-and-light-domain)
- [Core](#core)
  - [MoonModules](#moonmodules)
  - [Controls](#controls)
  - [Persistence](#persistence)
  - [Parallelism](#parallelism)
  - [Data exchange between modules](#data-exchange-between-modules)
  - [Event triggering between modules](#event-triggering-between-modules)
  - [Hot path discipline](#hot-path-discipline)
  - [Platform abstraction](#platform-abstraction)
  - [Firmware vs board](#firmware-vs-board)
  - [Peripherals](#peripherals)
- [Light domain](#light-domain)
  - [The pipeline](#the-pipeline)
  - [3D from the start](#3d-from-the-start)
  - [Layouts and Layout](#layouts-and-layout)
  - [Layers and Layer](#layers-and-layer)
  - [Effects](#effects)
  - [Modifiers](#modifiers)
  - [Mapping and blending](#mapping-and-blending)
  - [Drivers](#drivers)
  - [Memory strategy](#memory-strategy)
  - [Multi-device sync](#multi-device-sync)
- [Web UI](#web-ui)
- [What we leave undesigned](#what-we-leave-undesigned)

## The problem

Build a modular runtime for resource-constrained embedded devices that the same source compiles for, unmodified, on ESP32, Teensy, desktop, and Raspberry Pi. The runtime must:

- Compose behaviour from small, uniform units (modules) that can be created, configured, reordered, and removed at runtime — including from a network API.
- Expose every module's parameters generically so a single web UI renders any module with zero per-module UI code.
- Run a hot loop with predictable timing and zero steady-state heap allocation on devices with as little as ~320 KB of RAM.
- Persist configuration across reboots, exploit multiple CPU cores where present, and keep all platform-specific code behind one boundary.

The first concrete use of this runtime is lighting: drive 10,000+ addressable LEDs and DMX fixtures (RGB(W) pars, moving heads, dimmers) across multiple synchronised devices at high frame rates. The runtime is general enough that other real-time domains — audio synthesis, motor control — could be layered on the same way; lighting is the only domain implemented today.

## Core and light domain

The system is two layers, separated as much as practical:

- **Core** — MoonModule base, controls, scheduling, persistence, platform abstraction, system services (HTTP, WiFi, filesystem). Domain-neutral. Knows nothing about lights.
- **Light domain** — light values, layouts, layers, mapping, blending, effects, modifiers, LED drivers, ArtNet/DDP. Built on top of the core.

When mixing is needed (for performance or simplicity), it must be an explicit decision — consciously choosing minimalism over separation, not accidentally blurring the boundary. Use domain-neutral naming in those cases ("producer buffer" not "LED buffer", "output driver" not "LED driver" in core interfaces) to keep the door open for future separation.

# Core

The core's job is the runtime: modules, their lifecycle, their parameters, how they're scheduled, how they're persisted, how they reach the platform underneath.

## MoonModules

The core building block is a **MoonModule**. Everything is a MoonModule — not just effects, modifiers, layouts, and drivers, but also system services (HTTP server, WebSocket server, file server, WiFi, mDNS, OTA updates) and [peripherals](#peripherals) (sensors and actuators bridging to hardware/network). The core itself is minimal: MoonModule base, buffer management, a scheduler.

This means:

- Every MoonModule shares the same class structure, lifecycle (`setup`, `loop`, `teardown`), and controls. Learn the pattern once, apply it everywhere.
- System services get controls for free — HTTP port, WiFi SSID, mDNS hostname are all configurable through the same UI as effect parameters.
- Capabilities are modular — no WiFi? don't load the WiFi MoonModule. No `#ifdef`s needed.
- System MoonModules that listen (HTTP, WebSocket) poll in their `loop()` — the standard pattern for embedded servers.
- The scheduler handles init-order dependencies between system MoonModules (e.g. WiFi before HTTP, HTTP before WebSocket).

Modules can be added, replaced, reordered, or removed at runtime. On removal (teardown), all allocated resources are cleaned up.

### Lifecycle propagation to children

A MoonModule that owns children gets the standard lifecycle methods propagated to them automatically:

- `setup()` and `teardown()` — chain into children. Teardown reverse-iterates so children clean up before the parent does.
- `loop()`, `loop20ms()`, `loop1s()` — tick each child gated by the same rule the Scheduler applies to top-level modules (`!respectsEnabled() || enabled()` — modules that opted out of the enabled gate keep ticking, the rest tick only when enabled), with per-child timing accumulated into the child's own `loopTimeUs()`.
- `onBuildControls()` and `onBuildState()` — chain into children.

This means a container module gets correct lifecycle handling for its children without writing the iteration itself. Leaf modules (no children) pay one predicted-not-taken branch per call — sub-nanosecond. When a container overrides one of these methods to add its own work, the chain-to-base convention (parent-before vs child-before per callback) lives in [coding-standards.md § Override-and-chain convention](coding-standards.md#override-and-chain-convention).

**ModuleFactory** is a static registry mapping type names (strings) to create functions. The HTTP API uses it to create modules at runtime (`POST /api/modules {"type":"NoiseEffect"}`); the main pipeline in `main.cpp` constructs modules directly. Registration captures `sizeof(T)` for memory reporting:

```cpp
ModuleFactory::registerType<NoiseEffect>("NoiseEffect");
```

ModuleFactory is core infrastructure (`src/core/ModuleFactory.h`), not itself a MoonModule.

**Dynamic over fixed-size.** Children, module lists, control sets — anything structural — grow on demand from the heap during `setup()`. Fixed-size arrays impose arbitrary limits, waste memory on instances that don't use the full capacity, and cost memory on instances that need none (e.g. leaf modules with zero children). The hot path only iterates these arrays — same pointer arithmetic as a fixed array, no performance difference.

Each MoonModule is documented in `docs/moonmodules/` as it is built.

## Controls

Every MoonModule exposes **controls** — runtime-configurable parameters visible in the web UI. A grid layout exposes width, height, depth. An ArtNet driver exposes destination IP and universe. A fire effect exposes speed, cooling, sparking.

Controls bind to MoonModule member variables. The variable's default is the control's default. The hot path reads the variable directly — no function call. When a control value changes, the system notifies the owning MoonModule for cold-path reactions (e.g. a layout size change triggers a LUT rebuild).

Controls are dynamic: when a value changes, the control set can be rebuilt. A select control that picks a mode can show/hide other controls based on the choice.

Prefer `uint8_t` (0–255) for slider controls. Minimises per-control memory, aligns with DMX channel values, keeps the UI range manageable.

Controls are the bridge between the UI and the engine. The web UI renders them automatically from what MoonModules declare. The exact control types (slider, toggle, colour picker, text input, dropdown) are defined in the [UI spec](moonmodules/core/ui.md). The principle: modules declare what they need, the UI renders it.

## Persistence

Control values and each module's `enabled` flag are persisted to flash so settings survive a reboot. The mechanism lives in [FilesystemModule](moonmodules/core/FilesystemModule.md):

- **Storage** — one flat JSON file per top-level module under `/.config/<TypeName>.json`. Children are encoded positionally with `<index>.` key prefixes — no nested objects, no arrays. The parser stays minimal (three flat-JSON helpers in `core/JsonUtil.h`).
- **Lifecycle** — `Scheduler::setup()` runs four phases: (1) `onBuildControls` binds every module's full control set, (2) the FilesystemModule load hook overlays persisted values onto the bound variables, (2b) `rebuildControls` re-evaluates conditional `hidden` flags against the loaded state, (3) each module's own `setup()` runs with persisted values already in member variables, (4) `onBuildState` sizes buffers. Modules themselves know nothing about persistence — they just bind their variables.
- **Save trigger** — HttpServerModule marks the target module dirty on every successful control mutation. FilesystemModule debounces 2 s in `loop1s()`, walks the tree, writes any subtree containing a dirty descendant via atomic write-and-rename.
- **Conditional controls** — every conditional control is always bound; the module sets a `hidden` flag (`controls_.setHidden(i, …)`) to tell the UI not to render it. The load path can therefore find persisted values regardless of the live conditional state.
- **Code-wired children survive a stale file** — modules attached by `main.cpp`'s boot wiring (today: `ImprovProvisioningModule` as a child of `NetworkModule`) call `markWiredByCode()` after `addChild()`. The persistence apply step preserves them even when the saved file pre-dates the addition. Without this, every release that added or moved a code-wired child would trim it on the first boot of an existing device, and the user would lose the child until the next save rewrote the file. Children added through the HTTP API or recreated from JSON stay unmarked — those follow the file's tree shape exactly so UI deletes still take effect.

The Scheduler stays independent of FilesystemModule's type via a function-pointer hook (`setLoadAllHook`) — no circular include, persistence is opt-in. With no FilesystemModule registered, the load phase is a no-op and the system runs with member-initialised defaults.

## Parallelism

On multi-core systems (ESP32 has 2 cores, desktop / RPi have many), the system exploits parallelism by assigning MoonModules to specific cores. Each MoonModule can declare a core affinity. The scheduler respects this when pinning tasks. On single-core or desktop systems, affinity is ignored and everything runs on available threads.

The model is **producers vs consumers**: producers generate data, consumers process and output it. They run on separate cores with double buffering at the boundary — no locks on the hot path. The light domain instantiates the model concretely: effects are producers, drivers are consumers. Which buffers play the double-buffer role is covered in [§ Memory strategy](#memory-strategy).

## Data exchange between modules

When one module produces data another module reads on the hot path, the pattern is the same throughout the codebase. Two shapes, both core-defined and domain-neutral:

**Shared-struct (pull).** The reader holds a pointer to the producer's data and reads it when it needs it.

- The **producer owns a small POD struct** as a member, overwritten in place each tick. No allocation per frame.
- A **plain-data header** declares the struct. Both producer and consumer include it; neither needs to know the other's class.
- The producer exposes the struct via a `const`-returning getter (or a `setX(const Foo*)` setter on the consumer).
- The **consumer holds a `const Foo*`** received once at wiring time in `main.cpp`, and reads it on the hot path each frame.

No registry, no subscription, no event bus. The consumer reads the latest value when it needs it; if the producer wrote nothing this tick, the consumer sees the previous value (acceptable for the kinds of data this exchanges — frame buffers, periodic captures). Producer and consumer can run on different cores: publisher-write / readers-read with a one-frame staleness tolerance that doesn't need a lock.

**Push through a domain-neutral sink.** When the producer should hand bytes to a generic core service rather than expose a struct, the core defines a narrow interface and the producer pushes to it. The producer owns the data and its wire format; the core sink (the interface's implementer) knows only "take these bytes and do my generic job" — it has zero knowledge of what the bytes mean or which domain produced them. `BinaryBroadcaster` (`HttpServerModule` implements it: "broadcast these bytes to all WebSocket clients") is the example — the producer side lives in the light domain (see [§ The pipeline](#the-pipeline)).

Both shapes extend without ceremony to any future producer/consumer pair (a sensor module owning a state struct, an effect reading it through a `const Foo*` set at wiring time; or a module pushing bytes to a core sink). Neither is pub/sub: there's one producer per data kind and the consumer explicitly wants that specific data — the registry overhead and listener-lifecycle complexity of pub/sub buy nothing.

## Event triggering between modules

A control changes, or the module tree is mutated (a child added, deleted, replaced, moved) — and other modules may need to react. The framework provides a three-tier split so each change costs only as much as it has to, from cheapest to most expensive:

1. **`onUpdate(controlName)`** — runs on *every* control change, but only on the module whose own control changed. A cheap, per-control reaction: recompute a small LUT, re-bind a socket. Default no-op. This is where a brightness change rebuilds the (256-entry) correction LUT — no reallocation, so dragging the slider stays fluent.
2. **`controlChangeTriggersBuildState(controlName)`** — a gate, default `false`. Only `true` for controls that change physical dimensions or mapping shape (a Layout's grid width/height/depth; a Modifier's toggles change the LUT shape). When `true`, the framework runs the pipeline-wide rebuild; when `false` (effect speed, driver fps, brightness), it doesn't.
3. **`onBuildState()`** — the module (re)builds its derived state (buffers, LUTs) for the current control values. Reached via `Scheduler::buildState()` — the coordinator-driven sweep that walks every module's `onBuildState`.

`Scheduler::buildState()` fires from two triggers: a tier-2 gate returning true after a control change, **and** any tree mutation (HTTP add/delete/replace/move handlers all call it unconditionally — a structural change is rare and unambiguously needs a rebuild). Both triggers funnel through the same sweep; each module's `onBuildState` is idempotent (e.g. an effect only reallocs when its grid count actually changed), so over-rebuilding is wasted work, not a correctness hazard.

This is the recognised layout/prepare-pass pattern: JUCE's `prepareToPlay` and UIKit's `layoutSubviews` work the same way — a framework-driven sweep over every object of the primary type, gated by per-object metadata (WPF's `AffectsMeasure`, here `controlChangeTriggersBuildState`). Not pub/sub: the publisher (HttpServerModule, or the mutation site) explicitly tells the coordinator to run the pass; the coordinator explicitly walks every module. The light domain consumes this mechanism for its mapping rebuild (see [§ Mapping and blending](#mapping-and-blending)) but the mechanism itself is core and applies to any module with derived state.

If a module needs to actively notify a specific other module of an event (rather than publish data for polling, or change its own controls), the pattern is a direct method call from the producer to a known consumer — `ImprovProvisioningModule::loop1s` calls `networkModule_->setWifiCredentials(...)` when credentials arrive over UART. No event bus; the producer holds a pointer to the consumer set at wiring time (`main.cpp`). Pub/sub becomes the right pattern only when there are multiple unknown subscribers per event — projectMM has none today.

## Hot path discipline

The render loop (`Scheduler::tick` and everything it calls — every effect, modifier, driver, layout) is the hot path. It runs roughly 50–10000 times per second depending on light count. Code there obeys three rules:

- **No heap allocations.** `new`, `malloc`, `push_back`, `std::string` constructors, `make_unique`, `make_shared` — none of them on the hot path. Heap fragmentation on a long-running ESP32 kills throughput in minutes. Allocate everything during `setup()` / `onBuildState()`; the loop only reads and writes pre-sized buffers.
- **No blocking.** No `delay`, no `sleep`, no `mutex.lock()`. If a mutex is unavoidable, use `try_lock` and skip the work this tick. Blocking the render task means a visible glitch on the LEDs.
- **Integer math preferred over `float` in per-light work.** ESP32's FPU is single-precision and not as cheap as integer ALU; per-light float compounds fast. Use fixed-point or scaled integer math where the visual difference doesn't justify the cost.

**Memory layout** is the corollary: allocate buffers as single contiguous blocks outside the hot path. Never allocate many small scattered objects in a loop — fragmentation catches up even off-path. On ESP32 with PSRAM, use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for large buffers; the `platform::alloc` wrapper does this automatically.

**Network input** follows the same discipline: process synchronously at a defined point in the frame loop. Async input with staging buffers is allowed when memory is plentiful (desktop, PSRAM-rich ESP32), but the default is synchronous to keep the loop's worst case predictable.

## Platform abstraction

Only abstract what you actually need. Currently:

- **Time** — `millis()`, `micros()`. Monotonic, microsecond resolution. (`esp_timer` / `std::chrono`)
- **Memory** — `alloc(size)`, `free(ptr)`. Prefers PSRAM on ESP32, falls back to regular heap. `freeHeap()`, `maxAllocBlock()` for diagnostics. (`heap_caps_malloc` / `std::malloc`)
- **Networking** — `UdpSocket` for ArtNet send. `TcpConnection` / `TcpServer` for HTTP + WebSocket; `TcpConnection::writeChunks` is a non-blocking scatter-gather write so a backpressured browser can't stall the render loop. (lwIP sockets / BSD sockets)
- **Scheduling** — `yield()` (cooperative yield to OS/RTOS), `delayMs(ms)` (blocking sleep, off-path only), `reboot()`. (`vTaskDelay` / `esp_restart` on ESP32; `std::this_thread::sleep_for` / `std::exit` on desktop)
- **Platform config** — `platform_config.h` per platform: compile-time constants like `hasPsram` and `hasWiFi`. Each platform provides its own version; `types.h` includes it without `#ifdef`. Core code branches on these via `if constexpr` (e.g. NetworkModule drops its WiFi cascade when `hasWiFi` is false), so the dead branch is removed from the binary with no `#ifdef` outside `src/platform/`.

Abstractions are added when a concrete implementation needs them, not pre-designed.

**Platform boundary (hard rule).** All `#ifdef`, `#if defined`, platform-specific `#include`s, and hardware API calls live exclusively in `src/platform/`. Everything outside `src/platform/` compiles on every target without modification. Compile-time platform branching uses `if constexpr` on `platform_config.h` flags — never a preprocessor `#ifdef`. The boundary is enforced by `scripts/check/check_platform_boundary.py`, a commit gate (see [CLAUDE.md § Lifecycle Events](../CLAUDE.md#lifecycle-events)).

## Firmware vs board

**Firmware** is the compiled binary — chip target plus which radios/peripherals/sdkconfig fragments are included. Today's variants: `esp32` (WiFi only), `esp32-eth` (Ethernet only, WiFi excluded), `esp32-eth-wifi` (both), `esp32s3-n16r8` (S3 with 16 MB flash + 8 MB PSRAM). Selected by `build_esp32.py --firmware <key>`, reported by `SystemModule.firmware`, used as the contract target key in scenarios.

**Board** is the physical hardware — chip + PCB + on-board peripherals (PHY, USB-serial, PSRAM, antenna). Examples: `Olimex ESP32-Gateway Rev G`, `LOLIN D32`, `Generic ESP32 DevKit`. The device cannot identify its own board (no readable PCB ID on classic ESP32), so MoonDeck deduces it from the firmware where unambiguous (`esp32-eth*` ⇒ Olimex) and otherwise lets the user pick. The board name is stored on the device by [BoardModule](moonmodules/core/BoardModule.md) — a code-wired child of SystemModule that holds a single `board` Text control with the `readonly` UI flag (renders display-only on the device's own UI; HTTP `/api/control` writes still apply). MoonDeck mirrors the picked / deduced value to the device via `POST /api/control` after each discover and after every dropdown change. The catalog of valid board names lives at [docs/install/boards.json](install/boards.json), shared between MoonDeck and the web installer — MoonDeck reads it for its dropdown and HTTP `/api/control` push; the web installer reads it for its picker, pushes the picked board via Improv RPC `SET_BOARD` on first flash, and provides an HTTP fallback (Inject button on *Your devices*) when Improv isn't available on the firmware variant.

A board can run multiple firmwares (Olimex runs all four); a firmware can run on multiple boards (`esp32` runs on any ESP32 dev board). The codebase reserves "board" exclusively for physical hardware and "firmware" exclusively for the compiled binary.

## Peripherals

A **peripheral** is a MoonModule (role `ModuleRole::Peripheral`) that bridges to the outside world — hardware or network — *independently of the light pipeline*. Examples: a gyro/IMU read over I²C, a microphone or line-in read over I²S, a relay or GPIO toggled out, a status push to Home Assistant. Peripherals are **domain-neutral and live in core** — they know nothing about lights; the platform transport they use (I²C, UART, GPIO) is itself a domain-neutral platform primitive.

The defining property is the **data relationship, not the connector**: a peripheral does not consume or produce the light pipeline's output buffer. This is the clean line between a peripheral and a driver — *does the module consume the light output buffer?* If yes it is a **driver** (ArtNet, DMX, SPI-LED all consume the buffer and differ only in transport); if no it is a **peripheral**. A DMX sender uses a peripheral-style transport (UART/RS-485) but is a driver, because it sends the rendered buffer.

Peripherals are **user-add/deletable children of SystemModule**. The firmware is identical whether or not the device has the peripheral wired, so the user manages it at runtime — solder a gyro on, add the module; remove it later. This reuses the generic child add/replace/delete + persistence machinery; SystemModule simply declares `acceptsChildRoles("peripheral")`, and `SystemModule::userEditable()` defaults to allowing deletion (BoardModule, also a System child, opts out with `userEditable() == false` because the board identity is code-wired, not user-discretionary). Automatic detection — probing a bus and adding the matching peripheral module — is a future convenience on top; the manual path is the foundation.

**Reader vs writer is a per-module decision, not a role.** A peripheral may read (gyro), write (relay), or both (read a mic, drive a relay on a beat). Core affinity (which core a module's polling runs on, for load balancing) is likewise a per-module choice. Encoding direction in the role would force a false binary and split modules that do both, so one `Peripheral` role spans the category and direction lives in the module's behaviour.

**File shape and location.** A peripheral is core (domain-neutral), so it follows the *core* file convention — `.h` + `.cpp` under `src/core/` — not the light-domain header-only convention. Its hardware access goes through a domain-neutral platform primitive (`platform::i2c*`, a future `platform::uart*`), never a direct device call (the [platform boundary](#platform-abstraction) applies). It polls in `loop20ms` / `loop1s` (the render hot path is for the light pipeline only) and formats its readings into controls. Each peripheral gets a spec in `docs/moonmodules/core/` like any other core module (enforced by `check_specs.py`).

**Reading a peripheral's data from an effect** uses the shared-struct pull pattern from [§ Data exchange between modules](#data-exchange-between-modules) — the same shape effects/drivers already use, no new mechanism:

- The peripheral owns a small POD struct (e.g. `ImuState { float pitch, roll, gyroX, … }`) declared in a plain-data header, overwritten in place each poll — no per-frame allocation.
- It exposes the struct via a `const`-returning getter: `const ImuState* state() const`.
- The consuming effect holds a `const ImuState*`, set once at wiring time in `main.cpp` (`effect->setImu(gyro->state())`), and reads it on the hot path each frame. Producer and consumer can sit on different cores — publisher-writes / reader-reads with one-poll staleness, no lock.

A peripheral that only *displays* its readings (controls, no effect consumer) simply skips the getter + wiring — it's the producer with no consumer yet. Today's peripherals are display-only; the struct-getter + effect-wiring is what an audio-reactive or motion-reactive effect adds on top (tracked in the backlog).

# Light domain

The light domain is everything specific to driving lights. **Light** here means any controllable light source: an addressable LED pixel (WS2812, APA102), a DMX fixture (RGB par, moving head, dimmer), or any other output that takes colour/intensity data. The term is used instead of "pixel" because the system controls both LEDs and conventional lighting fixtures.

## The pipeline

Modules in the light pipeline can be added, replaced, or removed dynamically at runtime.

```text
              Layouts (shared by every Layer in Layers)
                ├── GridLayout  ──→ coordinate iterator
                └── WheelLayout ──→ coordinate iterator
                        │
                    Layers
                ┌───────┼───────┐
                ▼       ▼       ▼
            Layer A  Layer B  Layer C
          Effect(s) Effect(s) Effect(s)
        Modifier(s) Modifier(s) Modifier(s)
        Buffer(own) Buffer(own) Buffer(own)
            LUT(own) LUT(own) LUT(own)
                │       │       │
                └── Blend+Map ──┘
                        │
                    Drivers          (owns Correction: brightness + lightPreset)
                ├── WS2812Driver  ─ apply Correction ─→ DMA buffer
                ├── ArtNetDriver  ─ apply Correction ─→ UDP packets
                └── PreviewDriver (raw buffer, no Correction) ─→ WebSocket
```

**Data flow.** The pipeline instantiates both core data-exchange shapes (see [§ Data exchange between modules](#data-exchange-between-modules)):

- *Shared-struct (pull):* `Drivers` hands every child driver a `Buffer*` (source) plus a `Correction*` (shared brightness/reorder/white), and `Layer` exposes its pixel buffer to `Drivers` directly on the identity-mapping fast path — each consumer holds a `const`-pointer set once at wiring time and reads it per frame.
- *Push to a core sink:* `PreviewDriver` owns the preview wire format (a one-time coordinate table + per-frame RGB point list) and pushes the bytes to a `BinaryBroadcaster` (the core HTTP server). The server broadcasts them over WebSocket without knowing they're a preview — the format and the light types stay entirely in the driver. See [PreviewDriver](moonmodules/light/drivers/PreviewDriver.md).

**Naming convention.** Capital `Layouts`, `Layers`, `Drivers` are class names (always capitalised when referring to the class). Lowercase "layouts", "layers", "drivers" is the English plural — used freely when context makes it clear. Singular "layout", "layer", "driver" is an individual instance.

## 3D from the start

The system is natively 3D. Coordinates, effects, layouts, and mappings all operate in 3D space (x, y, z). 2D and 1D are simply the case where one or two dimensions have size 1. There is no separate 2D mode — everything is 3D, and lower dimensions fall out naturally.

Two numeric typedefs keep memory tight in LUT tables:

- **`nrOfLightsType`** — total light count, light indices, LUT destinations, `width * height * depth` products. `uint16_t` on devices without PSRAM (max 65 K), `uint32_t` with PSRAM (supports large hub75 panels). Selected at compile time via `platform_config.h`.
- **`lengthType`** — coordinates and dimensions. Always `int16_t` (max 32767 per axis, supports negatives for out-of-bounds effects).

For 12 K LEDs with a 1:1 LUT, the smaller `nrOfLightsType` on no-PSRAM devices saves 24 KB. All code uses the typedefs consistently to avoid casting.

## Layouts and Layout

**Layouts** (a MoonModule) is the top-level container for one or more layouts, defining the physical topology of the installation. It is shared by every layer — there is one Layouts describing the physical setup, and every layer renders into it. When a layout changes, every layer rebuilds its LUT.

A **layout** (a `LayoutBase` MoonModule, child of Layouts) defines the physical positions of lights in 3D space. It is a **coordinate iterator** — it yields `(physicalIndex, x, y, z)` for each light it defines. A layout does not own or build any mapping LUT.

Layouts cover both addressable LEDs and DMX fixtures. An LED-strip layout yields one coordinate per LED; a DMX-fixture layout yields one coordinate per fixture (a moving head is one point in 3D space).

Positions are computed algorithmically, not stored. Grid is the most commonly used layout, but any geometry works: spheres, rings, cones, spirals, arbitrary point clouds. Grid is full-density (every position maps to a light); a wheel is sparse (only spoke positions are mapped, gaps are unmapped).

Multiple layouts can live in one Layouts container. Each layout describes one light type; mixing light types in a single Layouts (e.g. LED strips + par lights) is listed in [§ What we leave undesigned](#what-we-leave-undesigned).

## Layers and Layer

**Layers** (a MoonModule) is the top-level container for one or more layers. Each layer renders independently into its own buffer; the Drivers container composes those buffers downstream. With one layer wired (today's boot pipeline) Layers is a thin pass-through; multi-layer composition (alpha-blend / additive) is in [§ What we leave undesigned](#what-we-leave-undesigned).

A **Layer** (a MoonModule, child of Layers) owns:

- A **buffer** — the light data effects write into (logical space).
- A **mapping LUT** — built by the layer from the shared Layouts and the layer's static modifiers.
- **Effects** (ordered list) — write light values into the buffer.
- **Modifiers** (ordered list) — transform the LUT or light values.

A layer can have **multiple effects**. Effects are not blended — they write to the buffer sequentially in their listed order, each overwriting or adding to the previous. That allows stacked patterns (a base-colour effect followed by a sparkle effect).

A layer can have **multiple modifiers**. Static modifiers chain during LUT build; dynamic modifiers chain during rendering. Order matters: mirror-then-rotate differs from rotate-then-mirror.

Each layer references the shared Layouts. The layer builds its own LUT by iterating the Layouts container's coordinates and applying its static modifiers in order. Different layers in Layers can have different modifiers, producing different LUTs from the same Layouts.

## Effects

Effects produce light colours. They write into the Layer's buffer, which represents a logical grid. The Layer determines the buffer's dimensions (width, height, depth) from the Layouts, its own start/end percentages within the physical layout, and its modifiers. Effects receive these logical dimensions and elapsed time (millis) as their rendering context. They compute light positions from the buffer index (e.g. `x = i % width`, `y = i / width`).

Effects use elapsed time for animation, not frame count. Animation speed becomes frame-rate independent — an effect looks the same at 30 fps and 60 fps. For multi-device sync, the leader synchronises elapsed time across followers; same approach as syncing a frame counter, but frame-rate independent.

Effects know nothing about hardware, protocols, physical LED layout, or mapping. They only see the logical grid the layer provides.

**Speed convention.** Effects with a speed control use BPM (beats per minute). `uint8_t`, default 60 (= 1 beat per second). Human-readable, musically meaningful, DMX-compatible. The effect converts BPM to animation rate internally using elapsed millis.

### Dimensionality

Every effect declares its native dimensionality through `EffectBase::dimensions()`, returning `Dim::D1`, `Dim::D2`, or `Dim::D3` (default — "I iterate every axis the layer gives me"). The Layer uses this to **extrude** lower-dimensional output across the unused axes after each effect's `loop()`:

- **D1** — the effect writes only the row at `(y=0, z=0)`. Layer copies that row across every other y in z=0, then copies z=0 across every z.
- **D2** — the effect writes only the z=0 slice. Layer copies z=0 across every z.
- **D3** — the effect writes every axis itself. Extrude is a one-comparison no-op.

D1/D2 are **opt-in promises**: declaring them tells the framework it can fill the missing axes, saving the per-effect work of iterating z (or y and z). Effects that don't make that promise stay at the D3 default and iterate the whole buffer.

Hot-path cost: extrude pays one comparison and returns for the D3 case. For D1/D2 on a layer whose unused axes are size 1 (a D2 effect on a 2D layer, a D1 effect on a 1D layer) the inner loops are guarded by `depth_ > 1` / `height_ > 1` and never run. Real `memcpy` work happens only for a D1 or D2 effect on a layer with more dimensions than the effect writes — exactly the case where you wanted the framework to do the duplication.

Each effect's `dimensions()` is a claim about which axes its loop iterates, not which axes its math could in principle vary along. A "D2 fire" can in future be promoted to D3 by adding z-aware heat propagation; until then declaring it D2 honestly describes what the loop does today.

The `dim` int is also emitted in `/api/types` so the UI derives the dimensional emoji (📏/🟦/🧊) per module — modules don't put dimensional emoji in their own `tags()` strings.

### Robustness rules

**Effects must run at every grid size.** Modifiers can shrink the logical grid to any size including 0×0×0 (e.g. every layout child is disabled). An effect's `loop()` must produce a correct result for any `(width, height, depth)` — no crashes, no divide-by-zero, no out-of-bounds writes. On a zero grid the loop is a clean no-op. Effects either gate at the top (`if (w <= 0 || h <= 0) return;`) or write their loops so an empty range is naturally a no-op (`for (y = 0; y < h; ...)`).

**Effects must animate at every tick rate.** Per-tick phase math computed as `dt * bpm * K / 60000` truncates to 0 on devices where `dt < 234/bpm` ms — desktop ticks every 0–1 ms, so even bpm=60 freezes. The fix is to keep the raw `dt * bpm` numerator in the phase accumulator and divide only at the read site:

```cpp
phase_num_ += static_cast<uint64_t>(dt) * bpm;
uint8_t t = static_cast<uint8_t>((phase_num_ * 256) / 60000);
```

See NoiseEffect / MetaballsEffect for the canonical pattern. Animation speed must depend only on `bpm` and wallclock — not on tick rate or grid size.

## Modifiers

A modifier (MoonModule) lives inside a layer alongside its effects. Modifiers expose a virtual interface — the Layer calls modifier methods without knowing the concrete type (no `dynamic_cast`).

A modifier can:

- Transform the mapping LUT via `transformCoord()` — rebuilt on the cold path, zero render cost.
- Transform light values via `transformLights()` on the hot path — per-light cost, enables dynamic animations like rotation.

**Dimensionality** for modifiers defaults to `Dim::D3` (assumed to work in all three axes unless declared otherwise). Unlike for effects, this is purely advisory — the Layer doesn't extrude modifier output. It exists so the UI can render the 📏/🟦/🧊 chip on the card. **MirrorModifier** is D3 (it has independent mirrorX/Y/Z toggles).

## Mapping and blending

The blend+map step walks each layer in turn: reads each logical light, uses that layer's LUT to find the physical position(s), blends the colour into the physical output buffer. This is where logical space meets physical space.

Each mapping LUT is a flat, contiguous lookup table allocated outside the hot path. It is built in `Layer::onBuildState()` and rebuilt whenever a Layout or Modifier control changes (the controls' `controlChangeTriggersBuildState` returns true) or a Modifier/Layout child is added/removed/replaced/moved — both triggers flow through the same core mechanism, see [§ Event triggering between modules](#event-triggering-between-modules).

The LUT supports four mapping types:

- **1:1 identical** — logical index equals physical index. No table needed (`hasLUT()` returns false, `setIdentity()` mode). Grid without serpentine, no modifiers.
- **1:1 shuffled** — logical maps to one physical, but reordered. Table needed. Grid with serpentine.
- **1:0 unmapped** — logical light has no physical output. Table needed. Sparse layouts (wheel).
- **1:N multimap** — logical maps to multiple physical positions. Table needed (CSR format). Mirror / clone modifier.

Because mapping and blending happen in a single pass over each layer, there is no intermediate "mapped but unblended" buffer. The physical buffer is the only output-side allocation.

## Drivers

**Drivers** (a MoonModule) is the top-level container for one or more drivers. It is the consumer side of the pipeline. The Drivers container owns a shared output buffer and performs blend+map from every layer's buffer into it each frame. Individual drivers then read from this buffer to push to hardware / network.

The shared output buffer is necessary when blend+map writes to arbitrary physical positions via the LUT — the output is not filled sequentially, so a driver cannot read chunk-by-chunk until the full buffer is populated. It is *not* needed for the single-layer, no-blend case (identity or serpentine-shuffle mapping): there a driver can fuse map + output correction + protocol encode into one pass straight into its own output (DMA buffer / packet), skipping the shared buffer. Full detail in [the LED-driver design doc](moonmodules/light/leddriver-analysis-top-down.md).

Each driver (a MoonModule) speaks one protocol:

- **LED drivers** — WS2812 via RMT, APA102 via SPI. Platform-specific.
- **DMX / ArtNet** — sends DMX over UDP. Supports addressable LEDs and conventional DMX fixtures (pars, moving heads, dimmers).
- **Preview** — streams light data to the web UI via WebSocket.
- **Desktop output** — SDL2 or terminal for visual preview. Desktop also serves as a high-speed processing node, driving lights via ArtNet/DDP over the network.

Each driver child reads from the Drivers container's output buffer. Everything before the Drivers container is platform-independent.

**Output correction** turns logical RGB into the physical signal every physical driver needs: **brightness** scaling, channel **reorder** (RGB→GRB etc. via a *light preset*), and **white** derivation for RGBW fixtures. The Drivers container owns the shared correction state — a brightness lookup table plus the light-preset — exposed as `brightness` and `lightPreset` controls. Each *physical* driver applies the correction per-light as it reads its source buffer, into its own output buffer/packet. Preview is exempt: it shows the raw logical buffer (the effect's true output, not the dimmed/reordered wire signal). ArtNet consumes the correction today via a `const Correction*` set by `Drivers`; any other physical driver added to `Drivers` consumes the same pointer. The brightness LUT rebuilds on the cheap `onUpdate` tier (see [§ Event triggering between modules](#event-triggering-between-modules)), so the slider stays fluent.

Network-based drivers (ArtNet, E1.31, DDP) must pace their packet output — never blast all universe packets in a tight loop. Both FPS limiting (skip frames if called too fast) and inter-packet delay (microsecond pause between universes within a frame) are required. Without pacing, receivers drop packets and the output appears broken.

## Memory strategy

All buffers are allocated as single contiguous blocks outside the hot path — at startup or when configuration changes (LED count, layout size, layer count). They are then reused every frame with zero allocations in steady state. Measured per-module timing and memory for each platform: [performance.md](performance.md).

### Buffer types

- **Layer buffers** — one per active layer, holds the logical light data for one effect chain. Allocated in PSRAM when available. On memory-constrained devices, consumers may read from the layer buffer directly (no mapping, no blending, no physical buffer needed).
- **Physical buffer** — when present, holds the blended+mapped output. It is a *blend* buffer, needed only for compositing (>1 layer, or any alpha/additive blend); it is not what provides producer/consumer parallelism. Parallelism comes from the consumer's own working copy — the encoded DMA buffer for a clockless LED driver, or the kernel socket buffer for ArtNet — which decouples the producer (filling the next Layer frame) from the consumer (transmitting the previous one).
- **Mapping LUT** — flat lookup table for logical→physical. Read-only during rendering. PSRAM is fine — sequential reads are cache-friendly.

All buffers are raw `uint8_t*` arrays sized `channelsPerLight * nrOfLights`. Supports RGB (3 channels), RGBW (4 channels), and multi-channel DMX fixtures (up to 32 channels per light) without separate code paths. Channel layout is configured via offsets (see MoonLight's [LightsHeader](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/LightsHeader.h) pattern).

Network input (ArtNet receive, WebSocket) is processed synchronously at a defined point in the frame loop. Zero extra buffers, no race conditions. The trade-off is up to one frame of latency (~16 ms at 60 fps), imperceptible for LEDs.

### Adaptive allocation

The system checks available heap before each allocation and degrades gracefully when memory is insufficient. A minimum reserve (`HEAP_RESERVE = 32 KB`) is kept for stack, HTTP, WiFi, and overhead.

- **Mapping LUT** is created only if all of: modifiers exist on the layer; layout is not a simple non-serpentine grid (where physical == logical); enough heap available after the reserve.
- **Driver output buffer** (see [§ Drivers](#drivers) for what it's for) is created only when at least one layer has a mapping LUT actually allocated and enough heap is available.

### Degradation cascade

Best to worst:

1. **Full pipeline** — LUT + driver output buffer. Modifier applied, clean separation.
2. **Skip LUT + driver buffer** — modifier not applied, forced 1:1 mapping. No intermediate buffers. (A LUT without a driver buffer to map into is useless — they're always skipped together.)
3. **Reduce layer dimensions** — halve width/height until the buffer fits, minimum 8×8.

Each degradation is observable via `lutSkipped()` and reported in `/api/system` per-module metrics.

### Invariants

Non-negotiable:

- Effects always write to their layer's logical buffer. Never to output, never to physical coordinates.
- Drivers always own the output path (blending, mapping, brightness correction, channel reordering).
- Layer buffer is mandatory — if it doesn't fit, reduce dimensions until it does ("at least see something").

### Per-module reporting

Every MoonModule reports `classSize()` (sizeof the class instance) and `dynamicBytes()` (heap allocated during `onBuildState`). Visible in `/api/system`, console output, and scenario tests. Memory scenarios verify that 1:1 pipelines use zero intermediate buffers and that the cascade triggers at the right thresholds.

### Scaling to available memory

| Device | Memory | Typical capability |
|--------|--------|--------------------|
| ESP32 + OPI PSRAM | 2–8 MB | Many layers, 10K+ LEDs |
| ESP32, no PSRAM | ~320 KB internal | Full pipeline: double buffering, mapping, blending, parallelism. Proven up to 16 K lights (128×128 measured live on Olimex; see [performance.md](performance.md)). The degraded path (single Layer, 1:1 direct, no blending) is reserved for installations that grow beyond what the full pipeline fits. |
| Teensy 4.x | 1 MB internal, no PSRAM | Comfortable headroom for several layers; excellent DMA-based LED output (OctoWS2811). Ethernet built-in on 4.1, optional on 4.0. |
| Desktop / RPi | Abundant | No constraints |

The architecture does not assume PSRAM is present. Buffer counts and sizes are determined at runtime based on available memory and reallocated when configuration changes.

## Multi-device sync

For installations spanning multiple controllers:

1. **Discovery** — devices find each other via mDNS.
2. **Time sync** — one leader broadcasts its elapsed time (millis). Followers compute their offset. Target: sub-millisecond accuracy. Since effects use elapsed time for animation, synced time means synced visuals — regardless of each device's frame rate.
3. **Light distribution** — when one device sends light data to another, use ArtNet / E1.31 / DDP. These are the standards, no need to invent a protocol.

# Web UI

![UI overview](assets/screenshots/ui_overview.png)

The UI is three hand-maintained files: `index.html`, `app.js`, `style.css`. No frameworks, no build tools, no npm. Served directly by the embedded HTTP server.

The UI is **MoonModule-driven**. It contains no hard-coded knowledge of specific effects, layouts, or drivers. It queries the system for the current MoonModule tree (layers, effects, modifiers, layouts, drivers — each with their controls) and renders generically:

- Each MoonModule shows as a card with its name and declared controls.
- Controls are auto-rendered by type (slider, toggle, colour picker, text input, dropdown).
- Modules can be switched (change which effect a layer uses) and linked (assign a layout to a layer).

Adding a new MoonModule with controls needs **zero changes** to the UI files. This extends to the tree-mutation affordances: which modules accept children (and of what role) comes from each type's `acceptsChildRoles()`, and whether a module can be deleted/replaced comes from its `userEditable()` — both declared on the C++ side and reported in `/api/types` + `/api/state`. The UI hardcodes no list of "which types are containers" or "which roles are editable"; a new container type or a fixed child is a one-line C++ override.

The light domain plugs into the UI at three points: a fixed top-level tree (Layouts / Layers / Drivers pinned in `main.cpp`, root reorder disabled while child reorder works via drag-and-drop), a binary WebSocket preview channel ([PreviewDriver](moonmodules/light/drivers/PreviewDriver.md) — type byte `0x02`, 13-byte header `dw/dh/dd/ow/oh/od`, RGB triples), and emoji-key assignments for the chip filter (full table in [core/ui.md](moonmodules/core/ui.md)). Full UI spec: [docs/moonmodules/core/ui.md](moonmodules/core/ui.md).

## What we leave undesigned

Filled in by module docs before each module is built:

- **Multi-layer composition** — composing more than one Layer's buffer into the shared output (alpha blend, additive). Today's pipeline ships one Layer and passes through.
- **Sparse / non-grid 3D preview** — preview today derives `(x, y, z)` from a dense grid index; sparse layouts (rings, spheres, point clouds) render as a clump in the bounding box. The architecture's intended fix is a one-time coordinate message — see [PreviewDriver](moonmodules/light/drivers/PreviewDriver.md).
- **WiFi runtime disable** — today the eth-only build profile compiles WiFi out; runtime gating based on detected hardware presence is a future addition.
