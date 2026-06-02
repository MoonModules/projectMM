# Testing

What we test and how. The detailed inventory of every test lives in two auto-generated files:

- **[Unit tests](tests/unit-tests.md)** — one row per `TEST_CASE`, grouped by module. Generated from `test/unit/{core,light}/unit_*.cpp`.
- **[Scenario tests](tests/scenario-tests.md)** — one section per scenario JSON, grouped by module. Generated from `test/scenarios/{core,light}/scenario_*.json`.

Both are produced by `scripts/docs/generate_test_docs.py`; the source of truth is the test files themselves (see [Adding Tests](#adding-tests) below).

## Testing strategy

Three test categories, each with a clear purpose:

- **Unit tests** (desktop, `test/unit/{core,light}/unit_*.cpp`) — exercise individual MoonModules in isolation with doctest. Each test file declares `// @module <Name>` so it's categorised under that module in the generated inventory. Run via `ctest` or `./build/test/mm_tests`. Verify a module's API, edge cases, and output independent of how it's wired into a pipeline.
- **Scenario tests** (desktop, `test/scenarios/{core,light}/scenario_*.json`) — exercise the system as an integrated pipeline. Each scenario is a declarative JSON file with a sequence of steps (`add_module`, `set_control`, `delete_module`) and optional performance bounds. The scenario runner (`test/scenario_runner.cpp`) replays the steps in-process and checks output and timing. The same JSON files run against a live device through the HTTP API — that's the next tier.
- **Live scenario tests** — the same scenarios driven against a running device over REST. See [Live scenario tests](#live-scenario-tests) below.

**Regression rule:** when a bug is found, the fix includes a new unit test or scenario that reproduces the bug. A comment in the test references the root cause so the connection stays traceable.

**Performance checks** verify architectural rules at runtime:

- **Zero-allocation render loop** — N frames, intercept `malloc` / `free`, fail if any allocation occurs during steady-state rendering.
- **Frame time bounds** — scenarios include `"bounds": {"fps": {"min_pct": N}}` to catch performance regressions.

Per-module timing, memory, and sizeof measurements per platform live in [performance.md](performance.md).

## Standards

Two principles drive every standard below:

1. **The test file is the source of truth.** Every fact about a test (which module, which scenario, what each case verifies, what each step does) lives in the test file itself. The generated docs and MoonDeck views read from there — they don't get edited by hand.
2. **One pattern, easy to spot in 30 seconds.** Every unit test follows the same header shape. Every scenario follows the same JSON shape. A new contributor recognises an existing test, then writes the next one by analogy.

### File layout

```text
test/
├── CMakeLists.txt
├── scenario_runner.cpp                       # replays scenarios in-process
├── unit/
│   ├── core/      unit_<Module>[_<topic>].cpp        # mirrors src/core/
│   └── light/     unit_<Module>[_<topic>].cpp        # mirrors src/light/
└── scenarios/
    ├── core/      scenario_<Module>_<topic>.json     # mirrors src/core/
    └── light/     scenario_<Module>_<topic>.json     # mirrors src/light/
```

A test lives under the subfolder of its **primary** `@module`'s source domain (e.g. `Layer` lives in `src/light/`, so `unit_Layer_extrude.cpp` goes in `test/unit/light/`). Cross-domain awareness travels through the `@also` list, not the directory. There's no `platform/` subfolder today — `src/platform/` is a pure abstraction layer whose desktop backend every unit test implicitly exercises; ESP32 platform code never runs on the desktop, so there's nothing to put there yet.

### Naming convention

- **Unit tests:** `unit_<ExactModuleName>[_<topic>].cpp` — `<ExactModuleName>` is the **CamelCase** class name as it appears in `// @module` (and in the source: `Layer`, `MoonModule`, `MirrorModifier`, `ArtNetSendDriver`). The optional `<topic>` collapses when the file's the only test for its module (`unit_Color.cpp` is fine if `@module Color`); add it when one module has several test files (`unit_Layer_extrude.cpp`, `unit_Layer_zero_grid.cpp`, …) or when the topic genuinely clarifies what the file covers (`unit_FilesystemModule_persistence.cpp`).
- **Scenarios:** `scenario_<ExactModuleName>_<topic>.json` — same module-naming rule; the topic is always present because scenarios always cross multiple modules and the topic distinguishes the focus.
- The **`"name"` field inside each scenario JSON** matches the filename stem exactly (e.g. `"name": "scenario_Layer_base_pipeline"`). The runner, the MoonDeck dropdown, the generated docs and `--name` on the CLI all use this single identifier.

### Unit-test file shape

Every `unit_*.cpp` file starts with **`// @module <Name>`** as its only required metadata header, then the includes, then the TEST_CASEs. Each `TEST_CASE` gets a single `//` line of end-user description on the physical line directly above it. Reading that line should tell someone what the case verifies without opening the body.

```cpp
// @module Color

#include "doctest.h"
#include "core/color.h"

// Hue 0 is pure red.
TEST_CASE("hsvToRgb red at h=0") {
    auto c = mm::hsvToRgb(0, 255, 255);
    CHECK(c.r == 255);
    ...
}

// Zero saturation produces a grey of the given value, regardless of hue.
TEST_CASE("hsvToRgb white when saturation is zero") {
    ...
}
```

Header annotations recognised by the parser:

| Tag | Required? | Shape |
|-----|-----------|-------|
| `// @module <Name>` | **yes** | Exactly one. CamelCase, must match a real module class. |
| `// @also <A>, <B>, ...` | optional | Comma-separated peer modules this test also exercises. Surfaces as “Also touches” in the generated docs and lets the MoonDeck module filter include the test for either domain. |
| `// @description <one-line>` | optional | Short file-level summary. Multiple `@description` lines concatenate. Use only when the tests don't speak for themselves — usually omit. |

Per-`TEST_CASE` description rules:

- **One physical line above the `TEST_CASE`**, no hard-wrapping; the generator and MoonDeck handle layout. A second line is allowed only when the case does something genuinely non-obvious.
- **Missing description** → the generator italicises the raw `TEST_CASE("…")` name in its place.

### Scenario file shape

Every `scenario_*.json` carries top-level metadata plus a `description` per step:

```json
{
  "name": "scenario_Layer_base_pipeline",
  "module": "Layer",
  "also": ["GridLayout", "RainbowEffect", "Drivers", "ArtNetSendDriver"],
  "description": "Core pipeline: build Layouts→Grid→Layer→RainbowEffect→Drivers→ArtNetSendDriver from scratch and verify each module wires correctly. Drives the bounded FPS check at the end so a render-path regression is caught.",
  "steps": [
    {
      "name": "add-grid",
      "description": "Add a GridLayout child to Layouts (default 16x16x1).",
      "op": "add_module",
      "id": "Grid",
      "type": "GridLayout",
      "parent_id": "Layouts"
    },
    ...
  ]
}
```

`name` matches the filename stem exactly; `module` drives the doc grouping and the MoonDeck filter; `also` lists peer modules. Per-step `description` is recommended (a missing one falls back to the step's `name`/`op` in the generated view). Unknown JSON keys are ignored by both runners (C++ and Python), so adding a new field is safe.

### Generated docs and the shared parser

```text
scripts/docs/
├── _test_metadata.py        # one parser used by both consumers below
└── generate_test_docs.py    # writes docs/tests/unit-tests.md + scenario-tests.md
scripts/moondeck.py          # serves the same data as HTML in MoonDeck views
```

Both the markdown generator and the MoonDeck endpoints (`/api/test-modules`, `/api/unit-tests/<Module>`, `/api/scenarios/<name>`) import from `_test_metadata.py`. Adding a new metadata field (e.g. `@since`) means one edit there; both consumers pick it up.

Run the generator after touching any test file:

```bash
uv run scripts/docs/generate_test_docs.py            # writes both docs
uv run scripts/docs/generate_test_docs.py --check    # exits non-zero on drift (CI-friendly)
```

The check mode is the planned commit gate: if a contributor adds a `TEST_CASE` without re-running the generator, `--check` flags it before the commit goes out.

## Unit tests

Inventory: **[docs/tests/unit-tests.md](tests/unit-tests.md)** (auto-generated, grouped by `@module`).

Run them with:

```bash
ctest --test-dir build/macos --output-on-failure   # all
./build/macos/test/mm_tests -tc="<case-name>"      # one test case
uv run scripts/test/test_desktop.py --module Layer # filtered by module
```

Or via MoonDeck (PC tab → Unit Test card). Pick a module from the shared module dropdown above the card to filter the run. Tests button shows the per-module inventory.

Output is summary-only on a full run (the doctest `-s` flag is added only on filtered runs, where the assertion-level detail is small enough to be useful).

## Scenario tests

Inventory: **[docs/tests/scenario-tests.md](tests/scenario-tests.md)** (auto-generated, grouped by `module`).

Run them with:

```bash
uv run scripts/scenario/run_scenario.py                                      # all
uv run scripts/scenario/run_scenario.py --name scenario_Layer_base_pipeline  # one by stem
uv run scripts/scenario/run_scenario.py --module Layer                       # all for one module
```

Or via MoonDeck (PC tab → Scenarios card). The module dropdown is shared with the Unit Test card above it: pick a module once and both card's run-set narrows. Steps button shows the per-scenario step list.

## Live scenario tests

Live scenarios run the same JSON against a running device via the REST API.

```bash
uv run scripts/scenario/run_live_scenario.py --host localhost:8080       # desktop
uv run scripts/scenario/run_live_scenario.py --host 192.168.1.210        # ESP32
uv run scripts/scenario/run_live_scenario.py --update-baseline           # save
uv run scripts/scenario/run_live_scenario.py --compare-baseline          # check
```

All scenarios use relative FPS bounds (`min_pct`) so they pass on any device — desktop at 10K FPS or ESP32 at 17 FPS. Settle time is 3 seconds to let the pipeline stabilise after rebuilds.

Scenarios that add modules (e.g. `scenario_Layer_base_pipeline`, `scenario_Layer_memory_1to1`) create temporary modules on the running device and clean them up at the end (`- Rainbow (cleanup)`). Modules that already exist show `=` instead of `+`.

Memory tracking works on ESP32: `freeHeap` and `freeInternalHeap` report real values. Desktop returns 0 (unlimited). The control-change scenario verifies no memory leaks by checking that heap returns to baseline after a mirror toggle.

## Hardware Verification

All live scenarios pass on both desktop and ESP32 with `min_pct: 80` relative bounds. Per-module timing, memory allocation, and sizeof measurements for each platform are in [performance.md](performance.md).

### ESP32 — Olimex ESP32-Gateway Rev G (no PSRAM)

- 128×128 grid (16,384 lights) — all live scenarios pass.
- Memory tracking verified: mirror toggle shows heap changes, returns to baseline (no leaks).
- Ethernet (LAN8720 RMII) connects via DHCP.
- Device discovery from MoonDeck finds the ESP32 on port 80.

## Adding Tests

**Unit test:** add a `TEST_CASE` to the appropriate `test/unit/{core,light}/unit_<ExactModuleName>[_<topic>].cpp` file. Each file carries `// @module <ExactCamelCaseName>` at the top, plus a single `//` description line above each `TEST_CASE`. Add a new file when no existing test covers your module — pick the subfolder matching the module's `src/` domain. After adding cases, run `uv run scripts/docs/generate_test_docs.py` so the generated inventory matches.

**Scenario test:** create a JSON file under `test/scenarios/{core,light}/` named `scenario_<ExactModuleName>_<topic>.json`. The top-level needs `name` (matching the filename stem), `module`, optional `also`, and `description`. Each `steps[]` entry takes a `description` field that the doc generator picks up. The scenario runner auto-discovers all `.json` files under `test/scenarios/` recursively.

**Regression test:** when fixing a bug, add a test that reproduces it. The test's description (the `//` line for unit tests, the `description` JSON field for scenarios) should mention the root cause so the connection stays traceable in the generated inventory.

**Doc check:** `scripts/docs/generate_test_docs.py --check` exits non-zero if regeneration would change `docs/tests/*.md` — a CI-friendly way to catch metadata that drifted from the source.
