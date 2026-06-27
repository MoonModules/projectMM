# Plan — MoonLive: expressions + host-bound functions (domain-neutral core)

> Approved plan record (CLAUDE.md *Plan before implementing* / *Refactor for simplicity*). A design correction on top of the IR rung (Steps 1-5, fill/setRGB/random16 on host + Xtensa): replace the `setRGB`-shaped special-case grammar with **general expressions**, and move the LED-domain functions (`setRGB`, `fill`, `random16`) out of the core compiler into a **host builtin table** the light-domain binding registers. Fixes three product-owner remarks at one root.

## The three remarks, one root cause

1. **`setRGB(random16(256), random16(256), 30, 0)` doesn't work** — the parser only allows `random16` in the *index* slot; colour slots are literal-only. Bespoke per-slot rules instead of "every argument is an expression."
2. **`random16(255)` caps at 255** — the index/colour validators conflate ranges; `random16` returns uint16 (0..65535).
3. **The core is light-specific** — `setRGB`/`fill`/the `Store` IR op / `buf[i*cpl]` are baked into `src/core/moonlive/`, violating *Domain-neutral core*. The engine should know *language* + *ISA*, never *LEDs*.

Root cause: the compiler was built around the *statement shape* (`setRGB(idx, r, g, b)`) rather than around **expressions + a generic call mechanism**. The fix is the architecture ESPLiveScript/ARTI-FX use and the MoonLive doc §3.4 specifies: the core knows expressions + `call(builtin, args…)`; the **host registers the functions**.

## Decisions locked with the product owner

- **Full generalization now** (not a quick per-slot patch): every argument is an expression; `setRGB`/`fill`/`random16` become host-bound functions; the core keeps only `Call` + arithmetic.
- **Host builtin table** (hpwit `arti_external_function` / ARTI / doc §3.4 model): the light-domain binding registers `{name → descriptor}`; the core parser resolves a call by name against the table and codegen dispatches generically. The core owns *dispatch*, the light domain owns *the functions*.

## The hot-path reconciliation (doc §3.4 + the product owner's choice)

Doc §3.4 says pixel writers (`setRGB`) must lower to **direct stores** (the identity-mapping fast path), NOT a per-pixel host *call* — a `call` per pixel would wreck 16K×50FPS. The product-owner choice is "all host-bound." These reconcile cleanly: **the builtin table is the binding mechanism for everything, and each descriptor carries HOW it lowers** —

- **`Kind::Call`** — a pure helper (`random16`, later `sin`/`hsvToRgb`): lower to a generic `Call` to the host C function pointer.
- **`Kind::Inline`** — a buffer writer (`setRGB`, `fill`): the descriptor names an inline lowering the backend knows by an opcode tag (a small fixed set), so it lowers to stores, no call.

Crucially, **the core does not hardcode `setRGB`** — it gets the name, the arg count, the kind, and (for inline) an opcode tag from the *table the light domain populates*. The core's inline lowering is generic over the opcode tag; the light domain decides which tags exist and registers the names. So the core stays domain-neutral (no string "setRGB", no RGB layout) while the hot path stays inline. This is the synthesis: domain-neutral core, fast path, host owns the vocabulary.

## Architecture

```
src/core/moonlive/
  MoonLiveBuiltins.h   (NEW) — the neutral descriptor + a fixed-capacity BuiltinTable:
                               { name, argc, Kind (Call|Inline), const void* fn (Call),
                                 uint8_t inlineOp (Inline) }. No LED knowledge.
  MoonLiveCompiler     — parser: a real expression grammar (primary := number | call;
                               call := ident "(" args ")"). Resolves each call name against
                               the injected BuiltinTable. Emits IR: Call for Kind::Call,
                               a generic InlineOp(tag, args…) for Kind::Inline. No setRGB/fill
                               strings, no Store-with-RGB-shape baked in.
  MoonLiveIr.h         — drop the RGB-specific Store; add a neutral `InlineOp` carrying an
                               opcode tag + operand vregs. Keep Const/Add/Mul/Bounds/Call/Loop.
                               (Bounds stays — it's a neutral guard the inline writers request.)

src/light/moonlive/
  MoonLiveBuiltins_light.{h}  (NEW) — the LIGHT-DOMAIN registration: builds a BuiltinTable with
                               setRGB (Inline op=WriteRGB), fill (Inline op=FillRGB),
                               random16 (Call → host fn). This is where "setRGB" the NAME and
                               the RGB semantics live. The binding injects this table into the
                               engine at compile time.
  MoonLiveEffect.h     — passes the light builtin table to engine_.compile(source, table).

src/platform/<isa>/
  moonlive_lower_*.cpp — the per-ISA inline-op lowering: given InlineOp(WriteRGB, addr,r,g,b)
                               emit the store sequence; InlineOp(FillRGB, …) emit the fill loop.
                               The opcode tags are a small neutral enum in core; the backends
                               implement them. (random16's Call lowering already exists.)
```

The opcode-tag enum (e.g. `InlineKind::WriteRGB`, `FillRGB`) lives in core as a neutral list — it's "the inline operations a backend knows how to emit," not "LED operations." The light domain maps its function *names* to these tags; a different host (a display, a sensor) would register different names against whatever inline ops its backend supports, or use only `Call`. The core never says "RGB" in a domain sense — `WriteRGB` is just "store 3 consecutive bytes at a computed address," a neutral primitive.

## Expression grammar (the real fix for #1, #2)

```
program  := stmt ";" End
stmt     := call                       // a statement is a (void) call: setRGB(...) / fill(...)
call     := ident "(" [expr {"," expr}] ")"
expr     := number                     // 0..65535 (uint16) — range checked at USE, not parse
          | call                       // nested: random16(256) as an argument
```

- Every argument slot parses an `expr`, so `setRGB(random16(256), random16(256), 30, 0)` works (#1).
- A number literal is a uint16 (0..65535); `random16(N)` accepts N up to 65535 (#2). A value used as a colour is masked to a byte at the store (the inline writer does `& 0xFF`), so out-of-byte colours wrap rather than erroring — consistent, no bespoke per-slot range rule.
- Each `expr` lowers to a vreg (a `Const`, or a `Call` result). `setRGB`/`fill` then consume those vregs via their InlineOp. The bounds guard wraps the inline write as before.

## Steps (desktop-first, each green)

1. **Core: BuiltinTable + neutral IR.** Add `MoonLiveBuiltins.h` (descriptor + table) and the neutral `InlineOp` + `InlineKind` enum; remove the RGB-specific `Store` and the `buildSetRgbIr`/`buildFillIr`/`buildSetRgbRandomIr` helpers from core (they encode LED shape). The IR builders move to the light domain.
2. **Light: register setRGB/fill/random16.** `MoonLiveBuiltins_light.h` builds the table; the IR-construction for WriteRGB/FillRGB lives here (it knows RGB). `random16` registered as a `Call`.
3. **Compiler: expression parser + table resolution.** Parse expr-per-arg; resolve call names against the injected table; build IR (Call / InlineOp). Delete the `setRGB`/`fill` keyword special-cases.
4. **Host backend: inline-op lowering.** `moonlive_lower_host.cpp` lowers `InlineOp(WriteRGB/FillRGB)` to the store sequences (the bytes the old `Store`/fill produced). Behavioral golden test still passes (output unchanged).
5. **Xtensa backend: inline-op lowering.** Same for `moonlive_lower_xtensa.cpp`. Build + flash Olimex; verify `setRGB(random16(256), random16(256), 30, 0)` and `random16(65535)` live.
6. **Tests + docs.** Update unit tests (expression cases, every-arg-random, uint16 range, the domain-neutral-core assertion: grep core for "RGB"/"setRGB" → none in the LED sense). Extend the scenario. decisions.md: the "built around the statement shape, not expressions" lesson. Update MoonLiveEffect.md (the builtin-table model, prior art: ESPLiveScript/ARTI bound functions).

## Validation

- Desktop: `setRGB(random16(256), random16(256), 30, 0)` writes a random pixel with a random red+green; `random16(65535)` accepted; behavioral golden (fill output unchanged) holds. All unit tests green.
- **Domain-neutral check** (the #3 fix, mechanised): a test/grep asserts `src/core/moonlive/` contains no LED vocabulary ("setRGB", "fill", "RGB" in the colour sense, "cpl"/"buffer" semantics) — only `Call`, `InlineOp`, arithmetic, the neutral opcode enum.
- Xtensa: the failing cases from the remarks work live on the Olimex; no crash.
- P4/RISC-V still builds (stub).

## Risks / watch-items

- **Don't let `WriteRGB` smuggle LED-ness into core.** The neutral framing must hold: the core enum entry is "store N bytes at a computed address," documented as such; the *name* `setRGB` and the 3-channel meaning live only in the light registration. If that line blurs, the refactor failed its own #3 goal.
- **Inline-op set stays small + neutral.** Resist adding `setRGBXY`-specific ops; XY/XYZ are index arithmetic feeding the same WriteRGB (expressions compute the index). Only genuinely-distinct store shapes get a tag.
- **No per-pixel call regression.** `setRGB` must stay inline (Kind::Inline), not become a `Call` — the doc §3.4 hot-path rule. The table's Kind enforces this.
- **Scope creep.** No `sin`/`hsvToRgb`/variables/`for`-in-source this plan — just the generalization + the 3 fixes. Those built-ins are later (they slot into the same table trivially, which is the point).

## Out of scope (later)
Float built-ins (sin/cos/sqrt/hsvToRgb), source-level variables and loops, setRGBXY/XYZ sugar, the RISC-V inline lowering (P4), a real register allocator. This plan is: expressions, the host builtin table, and domain-neutral core — fixing the three remarks.
