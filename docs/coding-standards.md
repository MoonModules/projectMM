# Coding standards

How code in this repo is written. Hard rules and process live in [CLAUDE.md](../CLAUDE.md); how to build and run lives in [building.md](building.md); what is tested lives in [testing.md](testing.md). Design rationale for the rules below lives in [architecture.md](architecture.md).

## Conventions

Decided once; not re-derived per file.

- **`#pragma once`** for header guards. No `#ifndef … #define … #endif`.
- **`constexpr`** over `#define` for compile-time constants. `#define` is reserved for build-system flags (e.g. `MM_NO_WIFI`) that need to be visible to the preprocessor.
- **`std::span`** over pointer + length pair in function signatures. The span carries the bound; the caller can't desync.
- **Namespace `mm`** for everything in the project. Platform code lives in `mm::platform`. Tests live in `mm` (no `mm::test` — keeps the same names visible to test code as to library code).
- **No `using namespace` in headers.** In a `.cpp`, `using namespace mm;` is allowed at file scope. In a header it pollutes every translation unit that includes it.
- **Semantic variable names.** Name variables for what they represent, not just their type. `availableHeap` not `available`, `internalHeap` not `internal`, `lutBytes` not `bytes`. A reader should understand the variable without looking at its assignment.
- **No hard line wraps in markdown.** Let the editor soft-wrap. Hard wraps make diffs noisier than they need to be.

## Prefer integers, store values in their native shape

**Default to integers.** Pick the smallest unsigned type that fits the natural range — `uint8_t` for percentages and small counts, `uint16_t` for pixels and ports, `uint32_t` for timestamps and byte counts, `int8_t` for signed RSSI-style values. Integers are faster, predictable, branch-free in the hot path, and one byte where they need to be.

**Use `float` only when the value is intrinsically fractional** — geometry positions on a normalised grid, audio amplitudes, ratios that would lose meaning if rounded. Even then, ask whether scaled integers (e.g. fixed-point `uint16_t` for 0..65535 mapping to 0.0..1.0) work. The render loop hits every light every frame; an integer multiply-and-shift dominates a float multiply on ESP32.

**Never use `double`** in firmware code. Xtensa (ESP32 classic) has no hardware FPU for `double` — every `double` operation runs in software emulation, ~30× slower than the same op on `float`. `1.0` is a `double` literal; write `1.0f` if you really meant float. A `double` slipping into the render path silently tanks FPS.

**Store values in their native shape.** When the value is intrinsically numeric, store it as a number. When it is four octets, store it as four octets. **Don't keep a long-lived string buffer just because the UI eventually shows the value as text** — format to string at the output boundary, on the stack, then throw the buffer away. Every "I'll just `char foo[12]` it now and snprintf into it" decision freezes a few bytes into the module's permanent footprint, where the cheaper alternative is one int and a `snprintf` on a local stack buffer at serialization time. On ESP32 with ~180 KB free heap, dozens of those add up.

Guidelines:

- **RSSI, TX power, frame counts, percentages, temperatures, voltages** — store as `int8_t` / `uint8_t` / `uint16_t`. If the UI needs a unit suffix, carry the suffix in the control descriptor (`ControlType::ReadOnlyInt` does this — see [Control.h](../src/core/Control.h)), not in a per-instance string.
- **IPv4 addresses** — store as `uint8_t[4]`, not `char[16]` dotted-quad. The wire format stays a string at the JSON boundary; the storage stays 4 bytes. See `ControlType::IPv4` for the pattern.
- **Mode / status labels from a small fixed set** — a `char[20]` buffer is acceptable when the label is short and `snprintf`'d at a transition; for purely constant labels (`"Idle"`, `"Connected"`) a `const char*` pointed at a static literal is even cheaper. Don't combine the two: don't `snprintf` a literal into a buffer.
- **Dynamic display strings (uptime, FPS, heap KB)** — `char[N]` buffer is the established pattern (see [SystemModule.h](../src/core/SystemModule.h)) because the value changes every second and the UI reads it by stable pointer. Size the buffer to the longest possible value; oversized buffers are waste.

Counter-example to avoid: storing `char rssiStr_[12]` and re-`snprintf`'ing `"-58 dBm"` into it every tick. The right shape is `int8_t rssi_` (1 byte) plus a control type that knows the unit. Saves 11 bytes per metric, scales linearly across the codebase.

## Per-type behaviour lives with the type

When a struct or enum is the semantic owner of some data — a control descriptor, a packet, a module role — the functions that interpret, serialise, validate, or otherwise operate on it should live next to the type, not at the call sites that use it. Free functions in the same `.cpp` count; member methods on the owning class are stronger; virtual methods on a base class are strongest. The wrong shape is the same `switch (type)` repeated in every consumer — adding a variant means hunting across N files for switches to extend, and the compiler can't tell you when one gets missed.

Three concrete patterns, all already common in this codebase:

- **Discriminator + free functions in the type's own file.** `ControlType` + `writeControlValue` / `applyControlValue` / `controlTypeName` in [Control.cpp](../src/core/Control.cpp); `parseDottedQuad` / `formatDottedQuad` next to `ControlType::IPv4` in [Control.h](../src/core/Control.h); `LightPreset` + `rebuild()` in [Correction.h](../src/light/Correction.h). Best when the discriminator is a plain enum and the operations are small.
- **Methods on the owning class.** [Buffer.h](../src/light/layers/Buffer.h)'s `allocate` / `free` / `clear`; [Scheduler.h](../src/core/Scheduler.h)'s `addModule` / `tick` / `buildState`; [ControlList](../src/core/Control.h)'s `addX` family. Best when the class has identity and the operations naturally form a small interface.
- **Virtual methods on a base class.** [MoonModule.h](../src/core/MoonModule.h)'s lifecycle (`setup`, `loop`, `loop1s`, `onBuildControls`, `onBuildState`, …). Best when polymorphism is already in play.

Counter-example to avoid: a `switch (c.type)` on `ControlType` duplicated in HttpServerModule, FilesystemModule, and scenario_runner — what the codebase used to look like before [Control.cpp](../src/core/Control.cpp). Adding a new ControlType meant edits in four places and the compiler couldn't catch a missed switch on a non-exhaustive enum. The fix was to move the per-type dispatch next to `ControlType`; consumers now call `writeControlValue(sink, c)` and don't need to know the enum's shape.

When a `switch (type)` outside the type's home file is legitimate: the caller has a genuinely different concern (HttpServerModule mapping `ApplyResult` to HTTP status codes is a transport policy, not per-type behaviour; scenario_runner's `switch (JsonVal::type)` dispatches on *its own* discriminator, not `ControlType`). The rule is "per-type dispatch lives with the type", not "switches are banned".

## File shape: header-only vs `.h` + `.cpp`

- **Light-domain modules and the `MoonModule` base: header-only.** Every effect, modifier, driver, layout, the light-domain containers (`Layouts`, `Layers`, `Drivers`, `Layer`), and the `MoonModule` base class live in a single `.h` with implementation inline. The benefit is concrete: a contributor copies `RainbowEffect.h`, edits, saves as `MyEffect.h`, registers one line in `main.cpp` — no "where does the `.cpp` go, what does CMake need" friction. The chain `RainbowEffect.h → EffectBase.h → MoonModule.h` is uniform; readers don't pivot to a different file shape at the base. When a light-domain file outgrows one concern, extract a helper into its own header (`BlendMap`, `MappingLUT`) rather than splitting to `.h` + `.cpp`. Header-only is a feature of the light domain.
- **Core service modules: `.h` + `.cpp`.** Core modules that bridge to the platform layer or implement substantial infrastructure (`HttpServerModule`, `FilesystemModule`, `NetworkModule`, `Scheduler`, `SystemModule`, `Control`) ship as a `.h` (interface) plus a `.cpp` (implementation). Three reasons that compound: (a) implementation changes recompile only the `.cpp`, not every TU that includes the header — incremental builds are 2–5× faster on the kind of edits that happen in development; (b) readers want the interface separately from the body; (c) symbol bloat and link-time stay bounded. Small core utilities that are *almost entirely declarations or inline accessors* — `types.h`, `color.h`, `version.h`, `PreviewFrame.h`, `JsonUtil.h`, `JsonSink.h`, `Sha1.h`, `Base64.h` — stay header-only. Templates (e.g. `ModuleFactory::registerType<T>`) also must stay in the header because of C++ instantiation rules; a module that's mostly template can therefore stay header-only.
- **Exceptions need a one-line comment at the top of the file naming the reason.** Without a stated reason the file is expected to follow the default for its category. When in doubt: light → header-only, core → `.h` + `.cpp`.

## Override-and-chain convention

A MoonModule that owns children gets the standard lifecycle methods (`setup`, `loop`, `loop20ms`, `loop1s`, `teardown`, `onBuildControls`, `onBuildState`) propagated to children by the base class default — see [architecture.md § MoonModules](architecture.md#moonmodules). When a container overrides one of these to add its own work, the convention is **when in the override to call the base**:

- **`loop` / `loop20ms` / `loop1s`** — option A: parent work first, then chain. The parent prepares state that children consume.

  ```cpp
  void loop() override {
      if (layer_ && layer_->lut().hasLUT() && outputBuffer_.data()) {
          blendMap(...);                  // parent's own work
      }
      MoonModule::loop();                 // then tick children
  }
  ```

- **`setup`** — chain first, then parent work. Children must be initialised before the parent depends on them.
- **`onBuildControls`** — chain first, then parent work. Children register their controls before the parent appends or rewires its own. Lets a parent build a list whose order is "children's controls, then mine."
- **`onBuildState`** — chain first, then parent work. Children compute their dimensions and dynamic buffers before the parent reads or modifies the shared state (Layer reads child effect/modifier dimensions; Drivers reads Layer output sizing).
- **`teardown`** — parent work first, then chain. The parent shuts down its own state before the base reverse-iterates children.

Option B (children first on `loop`; parent first on `setup` / `onBuildControls` / `onBuildState`) or a sandwich pattern is allowed only when a specific reason justifies it; add a one-line comment at the override explaining why.

## Casts

Use project typedefs (`lengthType`, `nrOfLightsType`) consistently so types match and casts are unnecessary. When casts are needed:

- **`static_cast`** — converts a value between related types. Checked at compile time. Use only at system boundaries: byte protocol packing, OS API return values, overflow-prevention with wider intermediates. If you need `static_cast` between project types, make the types match instead.
- **`reinterpret_cast`** — reinterprets raw memory as a different type. No conversion, no safety. Avoid. The only legitimate use is raw byte / memory access (e.g. `reinterpret_cast<const sockaddr*>` for socket APIs).
- **`dynamic_cast`** — runtime-checked cast from base to derived. Requires RTTI, disabled on ESP32 (`-fno-rtti`). Not used.

## Compiler warnings

All targets build with `-Wall -Wextra -Werror`. No warning is "harmless" — fix it or silence it explicitly with a `-Wno-…` justified in code.

## Static checks

- **Platform boundary** (`scripts/check/check_platform_boundary.py`) — scans all files outside `src/platform/` for `#ifdef` / `#if defined` with platform macros and `#include` of platform-specific headers (`esp_*`, `freertos/*`, `driver/*`, `SDL.h`, `wiringPi.h`, …). Fails if any are found. The platform boundary rule itself: [architecture.md § Platform abstraction](architecture.md#platform-abstraction).
- **Hot path lint** — flags allocation calls (`new`, `malloc`, `make_unique`, `make_shared`, `push_back`, `std::string` constructors) inside functions identified as hot path (render loop and callees). Today a code-review convention; will become an automated clang-tidy check as the codebase grows. The hot path rule itself: [architecture.md § Hot path discipline](architecture.md#hot-path-discipline).
- **Code formatting** — `clang-format` with a project `.clang-format` file. Applied in CI; code that doesn't match fails the check. Run locally via editor integration or `clang-format -i`.

## When checks run

- **Pre-commit (selective).** Build, unit tests, scenario tests, platform-boundary check, spec check, ESP32 build, KPI capture — each runs if the change makes it applicable. See [CLAUDE.md § Lifecycle Events](../CLAUDE.md#lifecycle-events).
- **Pre-push.** Opus reviewer agent over the push range, scoped to domain boundary, unnecessary abstractions, duplicated patterns, hot-path violations, spec conformance, bloat, platform boundary.
- **PR merge.** Plan reconciliation, documentation sync, live perf snapshot, permission review, README refresh — applicability-gated, see CLAUDE.md.
- **CI** — the same set, mandatory on every PR push. Exact CI configuration is set up when the repository's pipeline lands.
