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

## File shape: header-only vs `.h` + `.cpp`

- **Light-domain modules and the `MoonModule` base: header-only.** Every effect, modifier, driver, layout, the light-domain containers (`Layouts`, `Layers`, `Drivers`, `Layer`), and the `MoonModule` base class live in a single `.h` with implementation inline. The benefit is concrete: a contributor copies `RainbowEffect.h`, edits, saves as `MyEffect.h`, registers one line in `main.cpp` — no "where does the `.cpp` go, what does CMake need" friction. The chain `RainbowEffect.h → EffectBase.h → MoonModule.h` is uniform; readers don't pivot to a different file shape at the base. When a light-domain file outgrows one concern, extract a helper into its own header (`BlendMap`, `MappingLUT`) rather than splitting to `.h` + `.cpp`. Header-only is a feature of the light domain.
- **Core service modules: `.h` + `.cpp`.** Core modules that bridge to the platform layer or implement substantial infrastructure (`HttpServerModule`, `FilesystemModule`, `NetworkModule`, `Scheduler`, `SystemModule`) ship as a `.h` (interface) plus a `.cpp` (implementation). Three reasons that compound: (a) implementation changes recompile only the `.cpp`, not every TU that includes the header — incremental builds are 2–5× faster on the kind of edits that happen in development; (b) readers want the interface separately from the body; (c) symbol bloat and link-time stay bounded. Small core utilities that are *almost entirely declarations or inline accessors* — `types.h`, `color.h`, `version.h`, `PreviewFrame.h`, `JsonUtil.h`, `Control.h`, `JsonSink.h`, `Sha1.h`, `Base64.h` — stay header-only. Templates (e.g. `ModuleFactory::registerType<T>`) also must stay in the header because of C++ instantiation rules; a module that's mostly template can therefore stay header-only.
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
- **`teardown`** — parent work first, then chain. The parent shuts down its own state before the base reverse-iterates children.

Option B (children first on `loop`) or a sandwich pattern is allowed only when a specific reason justifies it; add a one-line comment at the override explaining why.

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
