# MoonLive

MoonLive is projectMM's **live-script engine** — author an effect as text and run it on a running device, compiled to native machine code so it executes at near-hand-written speed in the render hot path. The broader design lives in [livescripts-analysis-top-down.md](../../backlog/livescripts-analysis-top-down.md) (a backlog design study); this page documents the module.

Scripts call the same [power functions](power-functions.md) compiled effects use, reached through the builtin table — so the vocabulary is shared, in its flat scalar form.

A scripted effect names a **script file** under `/moonlive/`; the UI loads, edits and saves that file, and the module holds only the name (~32 bytes) — the text is read into a right-sized buffer to compile and freed immediately, so nothing script-sized stays resident. A front-end (lexer → parser → IR → per-ISA assembler) compiles it to native code on the next tick.

**A script is a class.** It declares one, and the host calls its functions: an effect's `tick()` runs once per frame. That is the same shape a compiled effect has, so what a contributor learns from one transfers to the other.

```
class RandomPixelEffect {
  void tick() {
    setRGB(random16(256), 0, 0, 255);   // a random pixel, blue
    setRGB(5, random16(256), 0, 0);     // pixel 5, a random red
  }
}
```

Inside a function the grammar is a sequence of **statements** — a function call, or a `for` loop over them — with **expression arguments**, so any argument may be a literal or a nested call. The class declaration is required: one top-level form rather than two means one set of rules to learn and one parse path to maintain.

**A script's role is its extension**: `.mle` an effect, `.mll` a [layout](MoonLiveLayout.md), `.mlm` a [modifier](MoonLiveModifier.md). That is what a card filters its picker on, so an effect card offers effects. The engine is role-blind and runs whichever moment the binding asks for; the extension decides what is OFFERED, not what runs.

**The shipped scripts are the reference**: [`moonlive/`](https://github.com/MoonModules/projectMM/tree/main/moonlive) in the repository holds every script the library ships, one file per effect, layout and modifier. Read them to see what the language looks like in practice: they are the same text the card edits.

**The library, and how it reaches a device.** A device carries the NAMES of every library script and the text of none, so the picker offers the whole library while flash holds a few KB rather than a few hundred. A name the device does not have yet is marked with a cloud; picking it downloads that one script and it becomes an ordinary local file. A device therefore holds the handful it actually uses, which is the normal case: one layout describes the rig it is wired to and the rest are meaningless on it.

The browser does the downloading, not the device: it reads the script from GitHub and posts it to the device's own file endpoint. So the device needs no internet at any point, and a rig on an isolated network is served by whatever machine is looking at its UI. The script comes from the firmware's own release tag, so it always matches the engine that will run it.

**Two directories, and why.** A downloaded library script lands in `/.moonlive`, hidden the way `/.config` is; your own scripts live in `/moonlive`. The editor only ever saves to `/moonlive`, so **editing a library script forks it**: your copy is a second file of the same name, and it wins. Deleting the fork restores the original, which is why the delete button reads **revert** (`↺`) there. That is a local operation, so getting a shipped script back never needs a network.

**Sending one back.** A script you wrote or changed carries a **`↗`** button beside the editor. It opens GitHub with the script already filled in: a new script as a new file under `moonlive/`, a changed library script as an edit of the one that is there. GitHub forks the repository on your behalf when you propose it, so contributing needs a GitHub account and nothing else. The button appears only for a file in your own directory, since an untouched library copy is byte-identical to what is already upstream.

**`GET /api/scripts`** is what the picker reads: the library's names per role (`effects`, `layouts`, `modifiers`), the tag they are fetched from, and the directory a download lands in. The catalog is compiled into the firmware, generated from `moonlive/` at build time by `catalog_scripts.cmake`, so a script added to the repository reaches devices with no other change.

The **class name is not the file name**. `plasma.mle` may declare `class PlasmaEffect`; the file is what the engine loads, the class is what diagnostics and the module status report. Renaming either leaves the other alone, the same way a C translation unit and the functions inside it are independent.

The functions are **not built into the compiler** — `setRGB`, `fill`, `random16` are registered by the *host* (the light domain) in a builtin table; the core compiler owns only the grammar and a generic call/inline mechanism (the ESPLiveScript / ARTI bound-function model). The compiler emits machine code for whichever ISA the device runs (Xtensa on the classic/S3) or the host ISA on desktop, places it in executable memory, and the engine calls it each render tick.

## Controls

- `script`: the script this module runs, picked from the library or your own files and **edited on the card itself**. A fresh module has none: it reports `no script — set the script name` and renders nothing, rather than every new module compiling the same default.

    Type in the box and the script compiles when you click away, press Ctrl/Cmd+S, or press Save; a dot on the Save button marks unsaved work. A valid script swaps in on the next tick. A failed compile frees the old code, shows the diagnostic in the module status, and renders dark until it is fixed, so a typo costs a message rather than a reboot. Fixing it in place is enough: nothing has to be renamed.

    The card also creates and deletes scripts (delete asks twice), and the same editor is what the File Manager opens from a file row. The control is [`filepath`](../core/ui.md#control-types), which is generic: the module says only where its files are and which extension they carry.
- **Scripted controls**: a script declares members, then says which of them the UI shows by calling `addControl` inside a `defineControls()`, the same call a compiled module makes. Each becomes a real MoonModule control (slider + UI + persistence), bound to a live value the running native code reads each tick:

  ```c
  class SpeedyEffect {
    byte speed = 50;
    byte hue   = 128;
    int  dwell = 900;           // a value a byte cannot hold
    byte phase = 0;             // a member, not a control: the UI never shows it

    void defineControls() {
      addControl("speed", speed, 0, 99);
      addControl("hue", hue, 0, 255);
      addControl("dwell", dwell, 0, 1000);
    }

    void tick() { setRGB(speed, hue, phase, 255); }
  }
  ```

  **Where** a declaration sits is what it means. In the class body it is a **member**: visible in
  every function, surviving every call, and persisted, which is what a setting is. Inside a
  function it is a **local**: `int now = gpioRead(pin);` lives in a frame slot, dies at the closing
  brace of its block, and is never written to config, which is what a working value is. Whether the
  UI shows a member is the separate question `defineControls()` answers; a member no control names
  is simply the script's own state.

  A local takes **every value type a member does**, and each means the same thing in both
  positions: `int`, `byte`, `bool` and `fixed`. A `byte` local wraps at 255 exactly as a `byte`
  member does, and its initializer is range-checked the same way, so `byte b = 300;` is refused
  rather than silently becoming 44. Only `string` is a member-only type, since there is no runtime
  string to put in a frame slot. A local must be initialized where it is declared (`int x;` would
  hold whatever the last block left there). The two scalings do not mix, exactly as they do not for
  a member: `fixed d = 0;` starts a Q16.16 value at zero because the literal adopts the fixed side,
  while anything computed names its own conversion. A local may not shadow a member or a system
  variable, so `x = 1` can never write somewhere the author did not mean. The two budgets are separate: sixteen frame slots
  against eight member records, and a block hands its slots back at its `}`, so a script is not
  limited to sixteen locals in total.

  The member is named by identifier rather than by repeating the string, so a typo is a compile error here as it is there, and the quoted name is the UI label, free to differ from the member's name. The **default** comes from the member's initializer, so there is one home for the starting value. The range arguments are ordinary expressions, like every other argument in the language: `addControl("speed", speed, base, base * 4 + 5)` is valid.

  `defineControls()` runs once after a successful compile, the way the Scheduler runs a compiled module's. Editing a control's slider does **not** recompile: the value lands in the engine's control-values arena and the next render tick reads it (the live-edit guarantee, the *no-reboot* principle). Saving the script and re-naming it recompiles and re-derives the control set; a control kept across the edit keeps its slider value, a removed control's saved value drops.

  **One call for every type**: which widget appears follows from how the member was declared, so a call and a declaration can no longer disagree. A `byte` becomes a 0..255 slider, a `bool` a toggle, an `int` a full-range number. A range the member's type cannot hold is refused rather than truncated (`addControl("n", n, 0, 900)` on a `byte`), because a slider whose top silently wraps is worse than one that never appears. A control binds a single member, never an array, and a `fixed` or `string` member is refused: neither has a widget yet.

### The five types

A type says what a value **means**; the storage is the compiler's business. Every **scalar** occupies the same 4-byte slot whatever its type, and only **arrays** pack by element — which is where the width still earns its keep, since a `byte[]` heat map costs a quarter of an `int[]` one and the classic ESP32 has no PSRAM to absorb the difference.

| Type | Range | For |
|---|---|---|
| `int` | −2,147,483,648 … 2,147,483,647 | counts, indices, milliseconds, anything whole |
| `byte` | 0 … 255 | a channel, a palette index, a heat cell — the LED's own range |
| `bool` | `true` / `false` | a flag |
| `fixed` | −32,768.0 … 32,767.99998, in steps of 1/65,536 | coordinates and anything fractional |
| `string` | one of the script's own literals | a name passed to a builtin |

An initializer is range-checked against its type, so `byte n = 300;` is a compile error naming the member rather than a silent 44. Arrays are declared `byte heat[16];` and start at zero; a `string` array is refused, since there is no runtime string to fill one with.

### `fixed`: fractional arithmetic without a float

`fixed` is Q16.16 — the number is stored scaled by 65,536, which is how every coordinate in the engine has always worked, now spelled the way a script reads it. There is no float anywhere: fixed-point is bit-identical on all four backends, which is also what makes an effect reproducible.

```c
fixed ux = 0.0;
ux = uvX(x, width, height);     // uvX and uvY hand back a fixed coordinate
ux = ux * 2 + 0.5;              // ordinary arithmetic, decimals written as decimals
setRGB(0, toInt(ux * 100), 0, 255);
```

**Mixing a whole number and a fixed value is a compile error** naming the conversion to write, because at run time the two are the same 32 bits and a silent mix is a number 65,536 times off with nothing reporting it. `toFixed(v)` and `toInt(v)` convert explicitly, each one instruction.

The exception is an integer **literal**, which adopts the fixed side at a meet point and converts at compile time: `ux * 2`, `if (ux < 0)`, and `ux = 5;` all read naturally and cost nothing at run time. A *variable* never adopts — its scaling is not visible where it is used, so it keeps the explicit rule.

### System variables — what the engine hands a script

Some names are **reserved**: the engine defines them, the script only reads them, and a declaration that reuses one is a compile error (`name is a system variable`). **One vocabulary serves every role** — a name means the same thing in a layout, an effect and a modifier — so what you learn from one script transfers to the next.

| name | what it is |
|---|---|
| `t` | elapsed milliseconds — the clock an animation is written against |
| `width`, `height`, `depth` | the **logical grid**, `0..255` |
| `xPos`, `yPos`, `zPos` | the light being transformed, `0..255` (a [modifier](MoonLiveModifier.md) is the one handed these; elsewhere they read 0) |

Every one but `t` is a byte, because it lives in the controls arena. A grid extent past 255 reports 255 rather than wrapping to a small number, and a modifier handed a coordinate outside `0..255` passes it through untransformed instead of folding a wrong position — so a script never silently sees a value that means something else.

The coordinate is `xPos`/`yPos`/`zPos` rather than `x`/`y`/`z` so that **`x` and `y` stay free as loop counters in every script**, which is what an author reaches for and what the shipped `grid.mll` uses. Reserving them globally would break the most ordinary code there is; a per-role reservation was the alternative and was worse, because a name then meant one thing in one role and was refused in another — which is how `disasm.py`, compiling against the widest vocabulary, came to refuse the shipped default layout.

`width`/`height`/`depth` are the Layer's own dimensions, derived from the layouts and the modifier chain. An effect is *told* its canvas rather than declaring it: a size restated as a control is a second answer that can disagree with the first, and a script that sets `width` to 16 on an 8×8 panel draws off the edge. A [layout](MoonLiveLayout.md) is upstream of that grid — it is what the dimensions are derived *from* — so it names its own controls instead (`cols`, `rows`) and reads the grid only if it has a use for it.

Reserving is what makes the guarantee hold: without it a declaration would silently shadow the value the engine handed in, and the script would disagree with its layer with no error anywhere.

The grid is TOLD to a script; its own dimensionality is DECLARED. `int dimensions() { return 1; }`
says the script paints a line, `2` an x/y picture, `3` the whole volume, and the Layer extrudes
whatever it writes across the axes it did not iterate: a D1 script's x=0 column is fanned across the
width, a D2 script's z=0 slice copied through the depth. That is what lets one script fill a 16x16
panel and a 1x60x10 tube rig without knowing either shape. A script that declares nothing is treated
as 2, which is what every script rendered as before it could say. `string tags()` alongside it gives
the emoji the card and the picker show. Both are read once per compile; the full rules are in
[the language reference](https://github.com/MoonModules/projectMM/blob/main/moonlive/README.md).

### The vocabulary — what a script can call

Registered by the light domain, not built into the compiler (the core owns only the grammar and a generic call/inline mechanism), so the list is one edit in `MoonLiveBuiltins_light.h`.

| call | does |
|---|---|
| `setRGB(index, r, g, b)` | write one light |
| `setXYZ(x, y, z)` | write one position (a [modifier](MoonLiveModifier.md)) |
| `fill(r, g, b)` | write every light |
| `addLight(x, y, z)` | place the next light (a [layout](MoonLiveLayout.md)) |
| `line(x1, y1, x2, y2, r, g, b)` | a straight segment on the grid, via the shared `draw::line` |
| `random16(n)` | a value in `[0, n)` |
| `mod(a, b)` | `a % b` — the wrap a cyclic animation needs |
| `beat(bpm, t)` | a `0..65535` sawtooth at `bpm` |
| `beatsin(bpm, t, high)` | a sine `0..high` at `bpm` |
| `noise(x, y, z)` | `0..255` gradient noise at that point, the field behind fire, clouds and plasma |
| `scale(value, n)` | a `0..65535` value onto `0..n-1` — lands a wave on an axis |
| `sin(angle)`, `cos(angle)` | the circle; one turn is `0..65535`, result biased to `1..65535` centered at 32768 |
| `turn(n)` | one revolution split `n` ways — the angle step for placing `n` points on a circle |
| `print(v)` | log a value and return it ([what it costs](writing-scripts.md#debugging-print)) |
| `a / b`, `a % b` | divide and remainder. Both are host calls: cheap on a cold path, deliberate per light. Dividing by zero **saturates** toward the numerator's sign rather than faulting, so no script needs a zero-check of its own; the remainder is 0 |
| `toFixed(v)`, `toInt(v)` | convert between a whole number and a `fixed` one, each a single instruction |
| `smoothstep(e0, e1, v)` | a soft `0..65535` ramp between two edges, the anti-aliasing primitive |
| `uvX(x, w, h)`, `uvY(y, w, h)` | shader space, as a `fixed` value: centered on 0.0, normalized on the short side so a circle stays round on a wide panel |
| `smin(a, b, k)` | the smooth minimum of two distances, so shapes melt into one surface rather than overlapping |
| `fade(amt)` | dim every light toward black, FastLED's `fadeToBlackBy`. The trail primitive |
| `polarA(dx, dy)`, `polarR(dx, dy)` | angle and distance from a center, for a radial effect |
| `fbm(x, y, octaves)` | octaves of noise summed at doubling frequency and halving amplitude, `0..255`: the cloud, smoke and terrain field. `octaves` is the cost knob, one noise sample each |
| `warp(x, y, strength)` | the field sampled where the field itself displaced it, `0..255`: the flowing, marbled look. Three noise samples |
| `fbm3(x, y, z, octaves)`, `warp3(x, y, z, strength)` | the same two fields with a third axis, so a volumetric fixture samples through the field rather than repeating one slice. On a panel, pass `0` for `z` and the result is the 2D form exactly |
| `osc(rate, ms, shape)` | a low-frequency oscillator, `0..65535`, at `rate` cycles per minute. Shapes: 0 sine, 1 triangle, 2 sawtooth, 3 square. Stateless, so two oscillators sharing a rate hold their phase relationship |
| `escape(cx, cy, jx, jy, iters)` | the Mandelbrot/Julia escape count, `0..255`, `0` inside the set. Zero seed = Mandelbrot; the four coordinates are `fixed`, so uv output flows straight in. The one loop a script cannot write: it squares signed values in 64 bits |
| `setPaletteColor(x, y, index, bri)` | one light from the ACTIVE palette, in one call |
| `setPaletteColorZ(x, y, z, index, bri)` | the same, addressing a light in a volume |
| `paletteR(i, bri)`, `paletteG`, `paletteB` | one palette channel, when a script needs the value rather than a pixel |
| `pool(n)` | size this script's particle pool, from `defineControls()`. Returns what it got |
| `emit(x, y, angle, speed, n, life, hue)` | throw `n` particles from a point |
| `gravity(g)`, `drag(k)` | the two forces |
| `step()` | move every particle, and drop what left the grid |
| `age(rate)` | count down life; a dead particle frees its slot |
| `bounce(e)` | reflect off the grid walls, keeping `e`/256 of the speed |
| `collide(radius)` | particles notice each other and pile up. NOT linear in pool size |
| `render(maxLife)` | draw the pool from the active palette |

The particle calls are each ONE PASS OVER THE WHOLE POOL, once per frame rather than once per
light, so a 300-spark script costs far less than a shader touching every pixel (`fountain.mle`
measures 1.1 ms on an 80x48 against `metal.mle`'s 59.6 ms). Size the pool from `defineControls()`:
`pool()` anywhere else reports the live count and allocates nothing, which is what keeps a malloc
off the render path. `collide()` is the exception to the cost model, being an N-body check: a few
dozen particles pile convincingly, a few hundred cost more than the rest of the frame. The
vocabulary follows the [WLED Particle System](https://github.com/wled/WLED) by Damian Schneider
([@DedeHai](https://github.com/DedeHai)); the fixed-point kernel and this binding are ours.

`sin`/`cos` return an **unsigned** wave centered on 32768, so a coordinate comes from scaling by the full span and not by half of it: `scale(cos(a), radius * 2 + 1)` sweeps a whole axis, where scaling by `radius` alone would only ever reach one side of center. Subtract 32768 for a signed wave when you want one.

`uvX`/`uvY` are the other way round, and the difference is deliberate: they return a **signed** coordinate with the center of the grid at 0 and the left half negative. A coordinate has an origin, so a script uses the number it is given rather than re-centering it; a wave does not, which is why the two conventions differ. They return a **`fixed`** value (Q16.16), so a script holds one in a `fixed` member and does ordinary arithmetic on it; `escape()` takes four of them, which is what lets uv output flow straight into a fractal.

`noise(x, y, z)` takes **16.8 fixed-point** coordinates: the high byte selects the noise cell and the low byte interpolates within it. So `x * zoom` sets how much of the field the fixture spans, and the time axis must be **monotonic** — feeding it a `beat()` sawtooth walks one cell and then snaps back to its start, which reads as a hiccup once per beat. Scaling `t` keeps walking into new cells. 2D is the same call with `z` held constant.

`turn(n)` exists because a full revolution is 65536 — one past the largest number a script can write — and the grammar has no division. Without it, placing `n` points evenly on a circle is not expressible.

### The script's own functions

A class may define functions beside its entry point and call them, including calling itself. `effects/crosshair.mle` is the worked example: a `column()` and a `row()`, both called from `tick()`.

These are real calls, not text pasted in by the compiler: the callee allocates its own frame when it runs, which is what lets one helper call another and what makes recursion work. A function takes no arguments yet, but it may RETURN a value: declare it `int` and the helper computes where a number is needed (`setRGB(0, level(), 0, 0)`); declare it `void` and it does a whole job instead.

Two rules a script author meets:

- **Declare a helper above the function that calls it.** Only functions already parsed are visible, so a call to one declared further down reports `unknown function`. A function can always call itself.
- **Recursion is bounded.** About 30 calls deep a further call does nothing and returns, because a render task has a fixed stack and the alternative to a limit is a device that resets mid-frame. It is not reported: what you see is the picture being wrong where the recursion stopped, on a device that keeps running.

### Wire contract — control declaration

The controls are **declared by the script** (one per `addControl` call in its `defineControls()`), then **surfaced in `/api/state`**, the device JSON view the integrator consumes, as regular controls alongside `script` (a `byte` member as uint8, a `bool` as bool, an `int` as int32). So an integrator sees and writes them exactly like any other control — e.g. `POST /api/control` with `{"module": "ML", "control": "speed", "value": 80}`; they're fully present in the device JSON, just authored in the script rather than fixed in the module. The script's `\n` line breaks are standard JSON string escapes the device decodes, so a multi-line script round-trips through `/api/file`.

## What the card tells you: size, memory, and how close to a wall

Three numbers, and they are not the same thing.

**`status` is the size of the compiled program**: how many bytes of machine code the script became.
That is what a script author asks and what nothing else answers.

**The memory figure (`696B + 1.4KB`) is what the module costs the device.** The first part is the
module's own `sizeof`, fixed whether or not a script is loaded. The second is its dynamic bytes: the
exec block holding the JIT'd code, plus the 17-byte control arena. So the status and the dynamic
figure describe the same bytes from two angles, one as the program and one as the allocation, which
is word-rounded and includes the arena.

**`tickTimeUs` is the real per-tick cost** of running the compiled function, measured the way every
module's is. `defineControls()` is not in it: that runs once after a compile.

**A third allocation exists and appears nowhere**, deliberately. Compiling needs a staging buffer,
sized from the script's token count before a byte is emitted, and it is freed the moment the compile
returns. It never reaches a card because by the time the UI reads anything it is gone. It also does
not accumulate: three scripted modules compiling in sequence each borrow and return it, so what
persists per module is only the exec block, sized to what was actually emitted rather than to the
estimate.

### The walls, and which one the card warns about

A script can exhaust ten limits, but only five are ones an author can act on:

| limit | ceiling | what to do |
|---|---|---|
| code size | 16 KB | split or simplify the script |
| controls | 8 | remove an `addControl` |
| members | 8 | shares the budget with controls |
| functions | 8 | merge two helpers |
| string bytes | 128 | shorter control labels |

The other five (IR ops, virtual registers, frame slots, assembler labels and fixups) are derived
from code size or loop nesting, so a number for them is noise: nothing an author writes addresses
them directly.

The card shows the **tightest** of the five, and only past half full: `1568 B, controls 8/8`. The
others by definition have more room, so showing all five would bury the one that matters. An
ordinary script reads its size and nothing else.

## Pieces

- **`MoonLive`** (`src/core/moonlive/MoonLive.h/.cpp`) — the **domain-neutral engine core**. Owns a block of executable memory; `compile(source, table)` runs the front-end against a host builtin table and places the emitted code, `run(buf, nLights, cpl, t)` calls it. Includes only `<cstdint>`, the compiler/emitter seams, and the platform seam — never `EffectBase`, `Buffer`, or any LED type.
- **`MoonLiveBuiltins`** (`src/core/moonlive/MoonLiveBuiltins.h`) — the **neutral host-binding seam**: a `BuiltinTable` of `{name → descriptor}`, where a descriptor is either `Call` (a host C function pointer — a pure helper like `random16`) or `Inline` (a neutral opcode tag the backend emits inline — the hot-path buffer writers, no per-pixel call). The core owns no function names; it resolves a call against whatever the host registered.
- **`MoonLiveCompiler`** (`src/core/moonlive/MoonLiveCompiler.h/.cpp`) — the **platform-independent front-end**: a recursive-descent lexer + expression parser that lowers each statement to the typed IR (`MoonLiveIr.h`). Pure (source + table in, IR out, deterministic). Knows the *language*, never an ISA and never a domain.
- **`MoonLiveBuiltins_light`** (`src/light/moonlive/MoonLiveBuiltins_light.h`) — the **light-domain registration**: the only place the LED vocabulary lives. Registers the whole vocabulary above — Inline ops lowering to stores, and Calls into host helpers — plus the system variables each binding supplies. A different host (display, sensor) writes its own table; the core is unchanged.
- **per-ISA assembler + lowering** (`src/platform/<target>/moonlive_asm_*` + `moonlive_lower_*`): a tiny named-instruction MacroAssembler with label back-patching, and the IR→bytes lowering that drives it. Xtensa for the classic/S3 (`__XTENSA__`), the host ISA on desktop (arm64/x86-64). Adding an ISA is a new assembler + lowering; the front-end and IR are unchanged. (`emitFill`/`emitAnimatedFill` remain as the hand-encoded `fill` references the assembler's output is checked against.) An ISA also brings its own **frame contract**, which the emitter honors before a single instruction matters: on Xtensa the top 32 bytes of every frame belong to the register-window spill hardware, enforced by a `static_assert` tied to the widest call emitted plus the structural codegen test ([why, and how it was found](../../history/lessons.md#lessons-from-the-moonlive-on-xtensa-branch-the-register-window-frame-bug)).
- **`MoonLiveEffect`** (`src/light/moonlive/MoonLiveEffect.h`) — the **thin binding**: a first-class `EffectBase` carrying the `script` control, whose `tick()` delegates to the engine over its own `buffer()`. `compile(source, table, sysvars)` takes both host tables: the shared `lightBuiltins()`, and the system variables THIS binding supplies — `effectSysVars()` here, `modifierSysVars()` for a modifier, `layoutSysVars()` for a layout, which is what decides the names each kind of script can read and cannot declare. The engine is projectMM-agnostic; the binding is the only coupled layer.

## Cross-domain wiring

- **The executable-memory seam** is new platform surface (`src/platform/platform.h`): `allocExec(size)` / `freeExec(ptr,size)` allocate memory the CPU can *fetch* from (ESP32 IRAM via `MALLOC_CAP_EXEC`; an `mmap` `PROT_EXEC` page on desktop, with macOS-arm64 `MAP_JIT` + a write-protect toggle), and `writeExec(dst,src,len)` copies emitted code in safely — on ESP32 that means 32-bit-aligned IRAM stores plus an instruction-cache sync so the core fetches fresh code, not stale cache. All ISA/cache quirks live behind these three functions; the engine stays target-agnostic.
- **The producer buffer**: the emitted routine writes the same `buffer()` + `nrOfLights()*channelsPerLight()` surface a compiled effect writes — the identity-mapping fast path, no intermediate copy. The binding hands the engine `(buffer(), nrOfLights(), channelsPerLight())` each tick.
- A failed compile (no executable memory) leaves the effect `!ok()`: it renders dark and reports the error in its module status — the device keeps running (robustness, no reboot).

## Prior art

MoonLive's native-codegen approach — compile a small C-like language straight to machine code and call it as a function, so a live-authored effect runs at near hand-written speed — was pioneered by **Yves Bazin (hpwit)** in **[ESPLiveScript](https://github.com/hpwit/ESPLiveScript)**: a from-scratch tokenizer, parser, and Xtensa code generator that drives a 12,288-LED panel at ~85 fps where interpreted languages (Lua, Gravity) managed 3–10. That result is what makes "go native, not interpreted" the right call, and ESPLiveScript is the reference MoonLive is built against — studied closely, credited, and written fresh against projectMM's architecture, never copied, per [*Industry standards, our own code*](../../../CLAUDE.md#principles). The live-scripting idea in this ecosystem also descends from **ARTI-FX / ARTI** (the interpreted-effects runtime in WLED MoonModules), which proved the load-a-script-and-run-it-live loop end to end. The host-binding surface (`setRGB`/`setRGBXY`/`setRGBXYZ`) is modelled on the **MoonLight** [effects tutorial](https://moonmodules.org/MoonLight/moonlight/effects-tutorial/).

## Tests

[unit_moonlive_fill](../../../test/unit/core/unit_moonlive_fill.cpp) runs the engine path in-process on the desktop host backend (`compile`/`run`, the animated routine, zero-lights, recompile, `free`, the `allocExec`/`writeExec`/`freeExec` round-trip, the buffer-shape guards). [unit_moonlive_ir](../../../test/unit/core/unit_moonlive_ir.cpp) pins the **behavioral golden** — a compiled `fill` and the hand-encoded reference render an identical buffer — plus setRGB's single-pixel write and the runtime bounds guard. [unit_moonlive_compiler](../../../test/unit/core/unit_moonlive_compiler.cpp) pins the expression grammar (`random16` in any/every argument slot, uint16 bounds), the parser diagnostics (no crash on malformed input), live recompile, and the **domain-neutral** property: with an empty builtin table the core knows *no* functions, and a host can register an arbitrary name against the same machinery.

The grammar + bounds guard are verified live on the S3/Olimex (Xtensa) by saving a script file and naming it — the device compiles the expression on-chip and renders it.

[scenario_MoonLiveEffect_livescript](../../../test/scenarios/light/scenario_MoonLiveEffect_livescript.json) exercises the effect **as a wired MoonModule** — what the unit tests can't reach: add it, live-edit the script file to recolor (recompile), push a broken script (`MoonLive::compile` fails, frees the previous code, `MoonLiveEffect` reports the parse error in the status and renders dark — no crash), recover, resize the grid to 1×1 and back while rendering (the every-grid-size hard rule), then remove and re-add (exec memory re-acquired clean). It runs in-process on the desktop backend each commit, and the same JSON runs live over REST against the device backends. The Xtensa/RISC-V backends are validated by the live S3/P4 runs (a `MoonLiveEffect` on a Layer lights the grid from its script file), which the desktop tests can't reach.

## Source

[MoonLive.md](../core/moxygen/MoonLive.md) · [MoonLiveBuiltins.md](../core/moxygen/MoonLiveBuiltins.md) · [MoonLiveCompiler.md](../core/moxygen/MoonLiveCompiler.md) · [MoonLiveIr.md](../core/moxygen/MoonLiveIr.md) · [MoonLiveEffect.md](moxygen/MoonLiveEffect.md)
