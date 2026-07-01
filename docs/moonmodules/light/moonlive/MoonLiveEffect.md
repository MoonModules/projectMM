# MoonLive

MoonLive is projectMM's **live-script engine** — author an effect as text and run it on a running device, compiled to native machine code so it executes at near-hand-written speed in the render hot path. The broader design lives in [livescripts-analysis-top-down.md](../../../backlog/livescripts-analysis-top-down.md) (a backlog design study); this page documents the module.

A scripted effect carries its **script source** as an editable, persisted multi-line text control (a resizable `textarea` in the UI), and a front-end (lexer → parser → IR → per-ISA assembler) compiles it to native code on the next tick. The grammar is a function-call statement with **expression arguments** — any argument may be a literal or a nested call:

```
setRGB(random16(256), 0, 0, 255);   // a random pixel, blue
setRGB(5, random16(256), 0, 0);     // pixel 5, a random red
fill(0, 0, 255);                    // every light blue
```

The functions are **not built into the compiler** — `setRGB`, `fill`, `random16` are registered by the *host* (the light domain) in a builtin table; the core compiler owns only the grammar and a generic call/inline mechanism (the ESPLiveScript / ARTI bound-function model). The compiler emits machine code for whichever ISA the device runs (Xtensa on the classic/S3) or the host ISA on desktop, places it in executable memory, and the engine calls it each render tick.

## Controls

- `source` — the script text (default: random pixels — `setRGB(random16(256), random16(256), random16(256), random16(256));`, one random light in a random colour each tick). Editing it recompiles live: a valid script swaps in on the next tick; a failed compile frees the old code, shows the diagnostic in the module status, and renders dark until fixed (the script-editor loop, robust + no reboot).
- **Scripted controls** — a script declares a tunable variable with a range annotation, and the engine surfaces it as a real `uint8` MoonModule control (slider + UI + persistence), bound to a live value the running native code reads each tick:

  ```c
  uint8_t speed = 50;   // @control 0..99      → a "speed" slider, default 50, range 0..99
  uint8_t hue   = 128;  // @control 0..255
  setRGB(speed, hue, 0, 255);
  ```

  Declaring the variable is what **creates** the control: `uint8_t <name> = <default>;` becomes a `<name>` slider (default `<default>`, range `0..255`). The trailing `// @control <min>..<max>` only **adjusts that control's range**; it's optional. A declared name used in a statement reads the control's **current** value. Editing a control's slider does **not** recompile — the value lands in the engine's control-values arena and the next render tick reads it (the live-edit guarantee, the *no-reboot* principle). Editing the `source` recompiles and re-derives the control set; a control kept across the edit keeps its slider value, a removed control's saved value drops. Stage 1 is `uint8` only.

### Wire contract — control declaration

The controls are **derived from `source`** (one per declared `uint8` control; the optional `@control` annotation only refines a control's range), then **surfaced in `/api/state`** — the device JSON view the integrator consumes — as regular `uint8` controls alongside `source`. So an integrator sees and writes them exactly like any other control — e.g. `POST /api/control` with `{"module": "ML", "control": "speed", "value": 80}`; they're fully present in the device JSON, just authored in the script rather than fixed in the module. The script's `\n` line breaks are standard JSON string escapes the device decodes, so a multi-line `source` round-trips.

## Pieces

- **`MoonLive`** (`src/core/moonlive/MoonLive.h/.cpp`) — the **domain-neutral engine core**. Owns a block of executable memory; `compile(source, table)` runs the front-end against a host builtin table and places the emitted code, `run(buf, nLights, cpl, t)` calls it. Includes only `<cstdint>`, the compiler/emitter seams, and the platform seam — never `EffectBase`, `Buffer`, or any LED type.
- **`MoonLiveBuiltins`** (`src/core/moonlive/MoonLiveBuiltins.h`) — the **neutral host-binding seam**: a `BuiltinTable` of `{name → descriptor}`, where a descriptor is either `Call` (a host C function pointer — a pure helper like `random16`) or `Inline` (a neutral opcode tag the backend emits inline — the hot-path buffer writers, no per-pixel call). The core owns no function names; it resolves a call against whatever the host registered.
- **`MoonLiveCompiler`** (`src/core/moonlive/MoonLiveCompiler.h/.cpp`) — the **platform-independent front-end**: a recursive-descent lexer + expression parser that lowers each statement to the typed IR (`MoonLiveIr.h`). Pure (source + table in, IR out, deterministic). Knows the *language*, never an ISA and never a domain.
- **`MoonLiveBuiltins_light`** (`src/light/moonlive/MoonLiveBuiltins_light.h`) — the **light-domain registration**: the only place the LED vocabulary lives. Registers `setRGB`/`fill` (Inline, lowering to RGB stores) and `random16` (Call). A different host (display, sensor) writes its own table; the core is unchanged.
- **per-ISA assembler + lowering** (`src/platform/<target>/moonlive_asm_*` + `moonlive_lower_*`) — a tiny named-instruction MacroAssembler with label back-patching, and the IR→bytes lowering that drives it. Xtensa for the classic/S3 (`__XTENSA__`), the host ISA on desktop (arm64/x86-64). Adding an ISA is a new assembler + lowering; the front-end and IR are unchanged. (`emitFill`/`emitAnimatedFill` remain as the hand-encoded `fill` references the assembler's output is checked against.)
- **`MoonLiveEffect`** (`src/light/moonlive/MoonLiveEffect.h`) — the **thin binding**: a first-class `EffectBase` carrying the `source` control, whose `loop()` delegates to the engine over its own `buffer()` and passes the light builtin table to `compile`. The engine is projectMM-agnostic; the binding is the only coupled layer.

## Cross-domain wiring

- **The executable-memory seam** is new platform surface (`src/platform/platform.h`): `allocExec(size)` / `freeExec(ptr,size)` allocate memory the CPU can *fetch* from (ESP32 IRAM via `MALLOC_CAP_EXEC`; an `mmap` `PROT_EXEC` page on desktop, with macOS-arm64 `MAP_JIT` + a write-protect toggle), and `writeExec(dst,src,len)` copies emitted code in safely — on ESP32 that means 32-bit-aligned IRAM stores plus an instruction-cache sync so the core fetches fresh code, not stale cache. All ISA/cache quirks live behind these three functions; the engine stays target-agnostic.
- **The producer buffer**: the emitted routine writes the same `buffer()` + `nrOfLights()*channelsPerLight()` surface a compiled effect writes — the identity-mapping fast path, no intermediate copy. The binding hands the engine `(buffer(), nrOfLights(), channelsPerLight())` each tick.
- A failed compile (no executable memory) leaves the effect `!ok()`: it renders dark and reports the error in its module status — the device keeps running (robustness, no reboot).

## Prior art

MoonLive's native-codegen approach — compile a small C-like language straight to machine code and call it as a function, so a live-authored effect runs at near hand-written speed — was pioneered by **Yves Bazin (hpwit)** in **[ESPLiveScript](https://github.com/hpwit/ESPLiveScript)**: a from-scratch tokenizer, parser, and Xtensa code generator that drives a 12,288-LED panel at ~85 fps where interpreted languages (Lua, Gravity) managed 3–10. That result is what makes "go native, not interpreted" the right call, and ESPLiveScript is the reference MoonLive is built against — studied closely, credited, and written fresh against projectMM's architecture, never copied, per [*Industry standards, our own code*](../../../../CLAUDE.md#principles). The live-scripting idea in this ecosystem also descends from **ARTI-FX / ARTI** (the interpreted-effects runtime in WLED MoonModules), which proved the load-a-script-and-run-it-live loop end to end. The host-binding surface (`setRGB`/`setRGBXY`/`setRGBXYZ`) is modelled on the **MoonLight** [effects tutorial](https://moonmodules.org/MoonLight/moonlight/effects-tutorial/).

## Tests

[unit_moonlive_fill](../../../../test/unit/core/unit_moonlive_fill.cpp) runs the engine path in-process on the desktop host backend (`compile`/`run`, the animated routine, zero-lights, recompile, `free`, the `allocExec`/`writeExec`/`freeExec` round-trip, the buffer-shape guards). [unit_moonlive_ir](../../../../test/unit/core/unit_moonlive_ir.cpp) pins the **behavioral golden** — a compiled `fill` and the hand-encoded reference render an identical buffer — plus setRGB's single-pixel write and the runtime bounds guard. [unit_moonlive_compiler](../../../../test/unit/core/unit_moonlive_compiler.cpp) pins the expression grammar (`random16` in any/every argument slot, uint16 bounds), the parser diagnostics (no crash on malformed input), live recompile, and the **domain-neutral** property: with an empty builtin table the core knows *no* functions, and a host can register an arbitrary name against the same machinery.

The grammar + bounds guard are verified live on the S3/Olimex (Xtensa) by editing the `source` control — the device compiles the expression on-chip and renders it.

[scenario_MoonLiveEffect_livescript](../../../../test/scenarios/light/scenario_MoonLiveEffect_livescript.json) exercises the effect **as a wired MoonModule** — what the unit tests can't reach: add it, live-edit the `source` to recolour (recompile), push a broken script (`MoonLive::compile` fails, frees the previous code, `MoonLiveEffect` reports the parse error in the status and renders dark — no crash), recover, resize the grid to 1×1 and back while rendering (the every-grid-size hard rule), then remove and re-add (exec memory re-acquired clean). It runs in-process on the desktop backend each commit, and the same JSON runs live over REST against the device backends. The Xtensa/RISC-V backends are validated by the live S3/P4 runs (a `MoonLiveEffect` on a Layer lights the grid from its `source`), which the desktop tests can't reach.

## Source

[MoonLive.h](../../../../src/core/moonlive/MoonLive.h) · [MoonLiveBuiltins.h](../../../../src/core/moonlive/MoonLiveBuiltins.h) · [MoonLiveCompiler.h](../../../../src/core/moonlive/MoonLiveCompiler.h) · [MoonLiveIr.h](../../../../src/core/moonlive/MoonLiveIr.h) · [MoonLiveBuiltins_light.h](../../../../src/light/moonlive/MoonLiveBuiltins_light.h) · [MoonLiveEffect.h](../../../../src/light/moonlive/MoonLiveEffect.h)
