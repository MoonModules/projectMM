# Plan — MoonLive Stage 1: Controls

## Context

MoonLive (the native-codegen live-script engine) shipped Stages 0 + 0.5: a script is a stateless
function of `(buf, nLights, cpl, t)` compiled to native machine code on Xtensa / RISC-V / host.
**Stage 1 makes a script *tunable*:** a declared variable becomes a normal MoonModule control
(slider + UI + persistence + live edit), so a user adjusts a running script without re-authoring it.

**End goal (product-owner-stated — Stage 1 is one step toward it):** scripts grow to full
Ripples-class effects with many controls, conditional controls (visibility rules), dynamic status
(`setStatus`), and a per-script *data arena* for script state (oscillator buffers, palettes). We
build toward this in steps; **temporary simplifications in Stage 1 are acceptable** as long as they
are a *clean step* toward that goal, not a hack that forces rework. Stage 1 deliberately ships the
*real* data-arena construct (heap, PSRAM-first) — the thing the end goal needs — rather than a
throwaway, while deferring conditional controls, non-uint8 types, and the compile-buffer arena.

Acceptance: `uint8_t speed = 50; // @control 0..99` then `setRGB(speed, 0, 0, 255);` renders a
`speed` slider; dragging it changes the running effect **live (no recompile)**; the value persists
across reboot. Verified on host (CI), S3 (Xtensa), P4 (RISC-V).

## Decisions locked with the product owner

1. **Control value reaches native code via a 5th function argument — a control-values arena
   pointer** (NOT a baked-in immediate, which would need a recompile per slider move). This is the
   `kArg3`/`t` pattern exactly: `t` is already a runtime arg threaded through all three ABIs and
   read each tick; a `const uint8_t* ctrls` arg is the same move, one slot over. A control read is
   one `load8 [ctrlPtr + offset]` — cheap, no alloc, no branch.
2. **Annotation syntax: trailing `// @control min..max`** — byte-for-byte how `RipplesEffect.h`
   already declares `speed`/`interval` (copy-paste-faithful) and keeps the expression grammar
   clean. (Lexer gains `//` comment handling, needed for later stages anyway.)
3. **Arena lives in the light-domain binding as a stable-address, grow-capacity-only heap block**
   (`platform::alloc`, PSRAM-first). NOT a fixed member array (doesn't scale to big effects), NOT a
   core sweep change (rejected — changing the `rebuildControls→onBuildState` order is a *core* change
   touching every module to serve one module's edge case; violates *Core grows slower than the
   domain*). The sweep runs `rebuildControls()` (→ `onBuildControls`) on every control change, so
   re-binding the control pointers there is automatic; the arena only *grows* its capacity (never
   shrinks/moves on a normal recompile), so `&arena[i]` stays valid. This is the §3.7 per-script data
   arena — the real construct the end goal needs, shipped now.
4. **Conditional controls: DEFERRED** to a later stage (needs the language to express a visibility
   condition — a grammar feature past Stage 1). Stage 1 = flat "declare the var, get the control".
5. **uint8 controls only** in Stage 1 (the RipplesEffect pattern). The neutral `DeclaredControl.type`
   enum is the seam for uint16/int16/bool/select later.
6. **Persistence: orphaned keys drop on load** — a control renamed/removed in an edited script has
   no binding, so its saved value is dropped (matches every other dynamic-control module; falls out
   of the existing persist-by-name path). A renamed control starts at its declared default.

## Central mechanism: the 5th argument + `LoadCtrl`, across three backends

New runtime signature (sits beside the existing FillFn/AnimFn):
```cpp
using CtrlFn = void (*)(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t, const uint8_t* ctrls);
```
New IR vreg `kArg4 = 4` (shift `kFirstTemp` 4→5). New IR op `LoadCtrl{ dst, imm=byteOffset }` →
`dst = ((const uint8_t*)kArg4)[imm]`. One instruction per backend; the new arg steals the lowest
scratch register on each ISA (shifting the scratch map up by one):

| Backend | kArg4 reg | LoadCtrl | call() impact |
|---|---|---|---|
| Host arm64 | x4 | `ldrb wDst,[x4,#imm]` | add x4 to the saved set (live arg across a host call) |
| Xtensa | a6 | `l8ui aDst,a6,imm` | window preserves a2..a7 free; reflect the scratch shift in call()'s save set |
| RISC-V | a4 | `lbu rDst,imm(a4)` | a4 is caller-saved → add to call()'s saved pool |

## Grammar (single-statement → declarations-then-statements)
```
program := { decl } { stmt }
decl    := "uint8_t" ident "=" number ";"  [ "// @control" min ".." max ]
stmt    := call ";"
expr    := number | ident | call        // ident now resolves to a declared control
```
- `parseProgram()` parses zero-or-more decls first, each → a `DeclaredControl{name,type=Uint8,
  min,max,def,offset}` (offset = declaration index, one byte per uint8). Then loops `call ";"` to
  `End` (multi-statement now, since there's a decl line + a statement line).
- `parseExpr()` ident path: look up the name in the declared-controls table first; if found, emit
  `LoadCtrl{ dst=alloc(), imm=offset }`. Else fall through to `parseCall` (builtin). So `speed` in
  `setRGB(speed, …)` lowers to a load of arena byte 0.

## Neutral engine surface (domain-neutral core)
```cpp
struct DeclaredControl { const char* name; uint8_t type; int32_t min, max, def; uint8_t offset; };
```
- `CompileResult` gains `DeclaredControl controls[kMaxCtrls]; uint8_t controlCount;` (fixed array,
  no heap — mirrors `BuiltinTable`). `type` is a neutral enum (`Uint8` only now), NOT a projectMM
  `ControlType`.
- `MoonLive` exposes `const DeclaredControl* declaredControls(uint8_t& n)` and owns the **arena**:
  `uint8_t* ctrlArena_` + `uint8_t ctrlCap_`, allocated via `platform::alloc`, grown-only on
  recompile, freed in `free()`. `run()` passes `ctrlArena_` as the 5th arg. `uint8_t*
  controlSlot(uint8_t offset)` lets the binding point a control reference at `ctrlArena_[offset]`.

## Binding (`MoonLiveEffect.h`) — dynamic per-script controls, no core change
- `onBuildState()`: compile, then ensure the arena holds `controlCount` bytes (grow capacity if
  needed — never shrink/move on a normal edit), seed each slot with its `def` (only newly-appeared
  controls; preserve an existing slider's value when the script is edited but the control persists).
- `onBuildControls()`: after `addTextArea("source", …)`, loop `declaredControls()` →
  `controls_.addUint8(c.name, *engine_.controlSlot(c.offset), c.min, c.max)`. The arena slot **is**
  the backing variable (reference-based controls bind by `&var`). This runs on every
  `rebuildControls()`, so pointers are always freshly bound to the current arena.
- `controlChangeTriggersBuildState()`: returns true ONLY for `"source"` (unchanged). A scripted
  control's value change → false → **no recompile**; the slider write lands in the arena slot, the
  next `run()` reads it. This is the live-edit guarantee and the *no-reboot* principle applied.

## Files
- **`src/core/moonlive/MoonLiveIr.h`** — `kArg4`, `IrOp::LoadCtrl`, `DeclaredControl`, capacity consts.
- **`src/core/moonlive/MoonLiveCompiler.h` / `.cpp`** — lexer (`//` comments + `@control` capture +
  `=`), the decl grammar, declared-controls table, ident→LoadCtrl, multi-statement loop; surface the
  list on `CompileResult`.
- **`src/core/moonlive/moonlive_emit.h`** — `CtrlFn` typedef.
- **`src/core/moonlive/MoonLive.h` / `.cpp`** — arena (`ctrlArena_`/`ctrlCap_`/`controlSlot`),
  `declaredControls()`, `run()` passes the 5th arg, `free()` releases the arena.
- **`src/platform/desktop/moonlive_asm_host.{h,cpp}` + `moonlive_lower_host.cpp`** — `LoadCtrl`
  lowering + x4 reservation + call() saves x4. (The CI gate — proves the seam on arm64.)
- **`src/platform/esp32/moonlive_asm_xtensa.{h,cpp}` + `moonlive_lower_xtensa.cpp`** — kArg4→a6 +
  scratch shift + `l8ui`. **`…_riscv` pair** — kArg4→a4 + scratch shift + `lbu` + call() saves a4.
- **`src/light/moonlive/MoonLiveEffect.h`** — dynamic `onBuildControls` + arena-bound `addUint8` +
  arena alloc/seed in `onBuildState`.
- **Docs**: `docs/moonmodules/light/moonlive/MoonLiveEffect.md` (the `@control` contract),
  `docs/backlog/livescripts-analysis-top-down.md` (mark Stage 1 done in the §9 ladder),
  `docs/history/decisions.md` (the live-control-via-arena lesson).

## Implementation steps (~7, host before device — the §0.5 invariant in miniature)

These are **build-and-test steps the product owner verifies one at a time**, NOT separate commits.
The whole feature lands as a **single combined commit** at the end (the product owner branches and
commits). Each step is independently buildable/green so it can be tested in isolation before moving on.

1. **IR + neutral surface** (`MoonLiveIr.h`, compiler header): `kArg4`, `LoadCtrl`, `DeclaredControl`,
   capacity. Pure types — compiles, no behaviour. Unit: struct round-trip.
2. **Lexer**: `//` comments + `@control min..max` + `=`. Unit: token/annotation extraction.
3. **Parser**: decl grammar, declared-controls table, ident→LoadCtrl, multi-statement. Unit:
   `declaredControls()` output + malformed-decl diagnostics.
4. **Host backend codegen**: `LoadCtrl` + x4 reservation + call() saves x4. Unit: the live-read IR
   test (the macOS-arm64 CI path proves the seam here first).
5. **Engine arena + signature**: `CtrlFn`, `ctrlArena_`/`controlSlot()`/`declaredControls()`,
   `run()` 5th arg, `free()` releases arena.
6. **Binding**: dynamic `onBuildControls` + arena-bound `addUint8` + alloc/seed. Scenario:
   controls scenario + live-edit (value change renders differently, tick stays cheap = no recompile).
7. **Device backends**: `LoadCtrl` + kArg4→a6(Xtensa)/a4(RISC-V) + scratch shift + call() save-set.
   Validated on-device; behaviourally golden against the host backend.

Step 4 (host) precedes step 7 (device) so the host backend proves the seam before the device ISAs
replicate it. After all 7 steps pass + the full gate set is green, the product owner makes the one
combined commit.

## Tests
- **Unit (`unit_moonlive_compiler.cpp`)**: parse `uint8_t speed = 50; // @control 0..99\n
  setRGB(speed,0,0,255);` → assert `declaredControls()` = `[{speed,Uint8,0,99,50,off=0}]`; malformed
  decl (missing `=`, bad range) → clean diagnostic, no crash.
- **Unit (`unit_moonlive_ir.cpp`)**: compile a control script on host, place it, call with a 1-byte
  arena `arena[0]=5` → pixel 5 written; mutate `arena[0]=9`, call again (NO recompile) → pixel 9
  written. Pins LoadCtrl codegen + the live-read contract. Add a `control + random16` case so the
  `call()` save-set interaction (kArg4 live across a call) is pinned.
- **Scenario (`scenario_MoonLiveEffect_controls.json`)**: add a MoonLiveEffect with a speed-control
  source; assert the `speed` control appears; `set_control speed=…` + `measure` → render changes,
  tick stays cheap (a recompile would spike tick / churn free-heap); persistence rides the standard
  uint8-control path.

## Riskiest part
**The device register-map shift (commit 7).** kArg4 steals a6 (Xtensa) / a4 (RISC-V), shifting every
scratch index in the lowering AND the hand-written `call()` save-sets — the codebase has a documented
Xtensa register-slip → StoreProhibited crash. Mitigation: host codegen lands first (commit 4,
CI-proven on arm64); device backends stay behaviourally golden against it; the `control + random16`
unit case pins the save-set interaction; every emitted instruction is disassembled against the real
toolchain before flashing (the discipline that caught every prior encoding bug). Second risk:
`kMaxVRegs`/scratch pressure — each LoadCtrl consumes a temp; a many-control statement must stay
under each ISA's scratch count (2-3 controls fine; flag if a statement needs many at once).

## Deferred (out of scope for Stage 1, on the path to the end goal)
- **Conditional controls** (visibility rules) — needs a language condition; a later stage.
- **Non-uint8 control types** (uint16/int16/float/bool/select) — wider arena slots + a neutral
  type→width map; the `DeclaredControl.type` enum is the seam.
- **Dynamic `setStatus` from a script** — supported by the same arena-owning pattern later.
- **The shared heap *compile-buffer* arena** (§3.7) — only needed at Ripples op-count scale (Stage 3+);
  distinct from this Stage-1 *control* arena.
- **`@effect dim=3D`** role/dimension annotation — Stage 4/5.

## Verification
- `ctest` (unit) + `uv run scripts/scenario/run_scenario.py` (scenarios) green at each **step**
  boundary (each step is independently testable); host codegen proven (step 4) before device
  backends (step 7). The full commit-gate set runs once at the end, before the single combined commit.
- Disassemble the host + Xtensa + RISC-V output for a control script against the real toolchains;
  confirm `LoadCtrl` is the expected `ldrb`/`l8ui`/`lbu` and `kArg4` lands in x4/a6/a4.
- Live on the bench: author `uint8_t speed = 50; // @control 0..99  setRGB(speed,0,0,255);` on the
  S3 and P4, confirm the `speed` slider appears, drag it and watch the lit pixel move **without** a
  recompile (status stays clear, tick unchanged), reboot and confirm the value persisted.
- Save the approved plan to `docs/history/plans/Plan-YYYYMMDD - MoonLive Stage 1 controls.md` as the
  first implementation step (per CLAUDE.md).
