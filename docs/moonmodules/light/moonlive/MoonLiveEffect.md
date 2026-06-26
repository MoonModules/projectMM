# MoonLive

MoonLive is projectMM's **live-script engine** — author an effect (or layout, modifier, driver, core rule) as text and run it on a running device, compiled to native machine code so it executes at near-hand-written speed in the render hot path. The full design is the forward-looking analysis [livescripts-analysis-top-down.md](../../../backlog/livescripts-analysis-top-down.md); this page documents what currently ships.

**Status: Stage 2 (first language slice).** A scripted effect now carries its **script source** as an editable, persisted text control, and a real front-end (lexer → parser → codegen) compiles it to native code on the next tick. The grammar is one statement — `fill(r, g, b);` — the smallest real language: no expressions, variables, or control flow yet, but the compiler shape is genuine and grows rule by rule. Earlier increments proved the load-bearing path beneath it: native code MoonLive generated runs on real Xtensa, in the render tick, writing the real buffer, animating from a per-frame value, without a crash. The front-end's headline property is **golden-bytes equivalence** — parsing `fill(0,0,255)` produces byte-for-byte the same machine code the hand-written emitter does, so the parser provably adds no codegen of its own.

## Controls

- `source` — the script text (default `fill(0, 0, 255);` — solid blue). Editing it recompiles live: a valid script swaps in on the next tick; a parse error shows its diagnostic in the module status and the layer goes dark until fixed (the script-editor loop, robust + no reboot).

## Pieces

- **`MoonLive`** (`src/core/moonlive/MoonLive.h/.cpp`) — the **domain-neutral engine core**. Owns a block of executable memory; `compile(source)` runs the front-end and places the emitted code, `run(buf, nLights, cpl, t)` calls it, `free()`/`ok()`/`error()` round out the lifecycle. Includes only `<cstdint>`, the compiler/emitter seams, and the platform seam — never `EffectBase`, `Buffer`, or any projectMM type.
- **`MoonLiveCompiler`** (`src/core/moonlive/MoonLiveCompiler.h/.cpp`) — the **platform-independent front-end**: a hand-written recursive-descent lexer + parser for the `fill(r,g,b);` grammar, plus codegen that drives the per-ISA emitter. Pure (source in, bytes out, deterministic), so it unit-tests with no hardware. Knows the *language*, never an ISA.
- **`emitFill`** (`src/core/moonlive/moonlive_emit.h`) — the **per-ISA backend seam**. A neutral declaration; the implementation is the ISA's machine code, behind the platform boundary: `src/platform/esp32/moonlive_emit.cpp` (Xtensa) and `src/platform/desktop/moonlive_emit.cpp` (host arm64 / x86-64). The engine and front-end never branch on ISA — they ask for bytes and run them. This is the IR/backend seam at its first increment (no IR yet; one routine).
- **`MoonLiveEffect`** (`src/light/moonlive/MoonLiveEffect.h`) — the **thin binding**: a first-class `EffectBase` carrying the `source` control, whose `loop()` delegates to the engine over its own `buffer()`. The engine/binding split (the engine is projectMM-agnostic; the binding is the only coupled layer) is what lets projectMM be a clean library later.

## Cross-domain wiring

- **The executable-memory seam** is new platform surface (`src/platform/platform.h`): `allocExec(size)` / `freeExec(ptr,size)` allocate memory the CPU can *fetch* from (ESP32 IRAM via `MALLOC_CAP_EXEC`; an `mmap` `PROT_EXEC` page on desktop, with macOS-arm64 `MAP_JIT` + a write-protect toggle), and `writeExec(dst,src,len)` copies emitted code in safely — on ESP32 that means 32-bit-aligned IRAM stores plus an instruction-cache sync so the core fetches fresh code, not stale cache. All ISA/cache quirks live behind these three functions; the engine stays target-agnostic.
- **The producer buffer**: the emitted routine writes the same `buffer()` + `nrOfLights()*channelsPerLight()` surface a compiled effect writes — the identity-mapping fast path, no intermediate copy. The binding hands the engine `(buffer(), nrOfLights(), channelsPerLight())` each tick.
- A failed compile (no executable memory) leaves the effect `!ok()`: it renders dark and reports the error in its module status — the device keeps running (robustness, no reboot).

## Prior art

MoonLive's native-codegen approach — compile a small C-like language straight to machine code and call it as a function, so a live-authored effect runs at near hand-written speed — was pioneered by **Yves Bazin (hpwit)** in **[ESPLiveScript](https://github.com/hpwit/ESPLiveScript)**: a from-scratch tokenizer, parser, and Xtensa code generator that drives a 12,288-LED panel at ~85 fps where interpreted languages (Lua, Gravity) managed 3–10. That result is what makes "go native, not interpreted" the right call, and ESPLiveScript is the reference MoonLive is built against — studied closely, credited, and written fresh against projectMM's architecture, never copied, per [*Industry standards, our own code*](../../../../CLAUDE.md#principles). The live-scripting idea in this ecosystem also descends from **ARTI-FX / ARTI** (the interpreted-effects runtime in WLED MoonModules), which proved the load-a-script-and-run-it-live loop end to end. The host-binding surface (`setRGB`/`setRGBXY`/`setRGBXYZ`) is modelled on the **MoonLight** [effects tutorial](https://moonmodules.org/MoonLight/moonlight/effects-tutorial/).

## Tests

[unit_moonlive_fill](../../../../test/unit/core/unit_moonlive_fill.cpp) runs the engine path in-process on the desktop host backend: `emitFill`/`emitAnimatedFill` produce a non-empty routine (and reject a too-small buffer), `compile` + `run` fill a buffer with the chosen colour, the animated routine derives its colour from the per-frame `t`, zero-lights writes nothing, recompile swaps the colour, `free` returns to `!ok()`, and `allocExec`/`writeExec`/`freeExec` round-trip a callable block. [unit_moonlive_compiler](../../../../test/unit/core/unit_moonlive_compiler.cpp) pins the front-end: the **golden-bytes equivalence** (parsed `fill(r,g,b)` == hand-emitted bytes), whitespace tolerance, every parser diagnostic (wrong name, bad arity, out-of-range, missing punctuation, trailing junk — no crash on any malformed input), and the live source-recompile swap. The Xtensa backend is validated by the live S3 run (a `MoonLiveEffect` on a Layer lights the grid from its `source`), which the desktop tests can't reach.

## Source

[MoonLive.h](../../../../src/core/moonlive/MoonLive.h) · [MoonLiveCompiler.h](../../../../src/core/moonlive/MoonLiveCompiler.h) · [moonlive_emit.h](../../../../src/core/moonlive/moonlive_emit.h) · [MoonLiveEffect.h](../../../../src/light/moonlive/MoonLiveEffect.h)
