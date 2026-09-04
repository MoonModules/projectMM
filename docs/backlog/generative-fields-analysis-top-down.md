# Generative fields — top-down build spec

> **Forward-looking design document — exception to CLAUDE.md present-tense rule.** Stage 2 of the generative-fields work: turns the [bottom-up analysis](generative-fields-analysis-bottom-up.md) (Part 1 the specification language, Part 2 the examples, Part 3 the gap) into an implementable spec: homes, types, signatures, the MoonLive surface, budgets, tests, and a step-by-step plan. Written 2026-09-03 against the five product-owner decisions in § 0. Where this document makes a NEW decision it is marked **(proposal)** and listed in § 10. Companions: the [power-functions top-down](power-functions-analysis-top-down.md) owns the library's homes, types and standing decisions, which this document extends rather than restates; the [MoonLive roadmap](moonlive-language-roadmap.md) owns the engine.

## Status legend

| | Meaning |
|---|---|
| ✅ | **Done.** Built and in the tree. |
| 🔨 | **To do.** Open, with the reason stated. |
| ❓ | **Unsure.** A claim or number not yet verified; a question, not a fact. |

An unmarked line is context or rationale.

## TL;DR

- ✅ **Five decisions taken by the product owner (2026-09-03):** the Layer buffer goes to 16 bits per channel as the one format, subject to the memory analysis in § 2; gradient noise replaces value noise in place (one solution, the goldens move once); MoonLive gets each kernel in the same phase as the compiled function, with a shipped script; the stable-fluids solver is in scope as the last phase, a P4 and desktop showcase; every effect that does not run on the power functions is rewritten on them (algorithmic effects such as Game of Life excepted), judged against how it ran before and never made worse.
- **(proposal) The Layer width is a per-layer property fixed on the cold path, one template instantiated twice, never a per-light branch.** 16-bit is the default wherever it fits; 8-bit is the degradation step the existing adaptive-allocation cascade already has, so the classic without PSRAM keeps its proven 128² pipeline. The pipeline stays 16-bit end to end and quantizes **once, per driver, in `Correction`**, to the width that driver's wire takes: 8-bit with temporal dithering for WS2812-class LEDs and every network, Hue, video and preview output, 16-bit straight through for a 16-bit LED. Memory doubles per layer and for the output buffer (98 KB each at 128² RGB), bandwidth doubles in blend, extrude and preview, and the S3's PSRAM latency makes that measurable on cheap effects; the KPI gate reads it per batch (§ 2).
- **(proposal) Seven kernels, in the existing homes.** `core/noise.h`: gradient noise in place, plus `curl16`. `light/polar.h` (new): `PolarLut`. `core/oscillators.h` (new): `Oscillators`, the timer bank with the four LFO shapes and modulation. `light/draw.h`: `advect` (the bilinear previous-frame resampler, separable and 2D, wrap and clamp), the velocity rules, `decay` by half-life, `lineAA`, `disc`, and the 16-bit quantize with temporal dithering. `light/fluid.h` (new, last): the Stam solver. Every function names its canonical source; nothing is ported (§ 3).
- **(proposal) MoonLive operates on the Layer, not on a handle.** With the Layer at 16 bits the frame buffer IS the wide state, so advection, decay and the emitters are whole-frame builtins on the script's own layer, the shape `fill` and `fade` already have; the polar LUT and the oscillator bank are handles declared in `defineControls()`, the shape `pool()` has. Multi-argument host calls already ship (`emit` takes seven), so nothing here waits on the engine; the one engine change is the pixel-store inline ops learning the 16-bit element (§ 4).
- **Budgets are the bottom-up's, restated as targets per target.** Shaders: samples per pixel against ~750 cycles per sample on the S3, ~250 on the P4; advection: cycles per frame plus bytes of state, PSRAM-class for walls. Framerate is protected by rule: every kernel is `dt`-driven, transport stays sub-pixel per frame, and a showcase must hold its stated fps on its stated fixture or its cost knob is the default (§ 5).
- **Four showcases, three plus fluid**, specified to the control level: *Aurora* (a pure shader), *Trails* (emitters and flows), *Nebula* (a shader feeding a flow), *Fluid* (the solver, P4 and desktop) (§ 6).
- **Eight phases, desktop first, bench last; the product owner decides when enough is done to commit.** Noise swap; fields and Aurora; three dimensions everywhere; Coord3D in MoonLive; the 16-bit Layer; advection and Trails; composition, dithering and SIMD; fluid. The catalog sweep that decision 5 mandates is a follow-on plan, fed by the [effects × power functions inventory](effects-power-function-inventory.md); only the effects a phase touches directly (the noise effects in phase 0, PolarNoise in phase 1, the raw-byte writers in phase 2) are rewritten inside this plan. Each phase lands with its unit tests, its goldens, its scenario, its MoonLive builtins and script, and its catalog cards (§ 8).

## 0. Inputs and the decisions taken

From the bottom-up: two techniques over one block set; every block named from its primary source; the cost model; projectMM's three missing blocks (previous-frame resampler, color state above 8 bits, gradient noise) plus the LUT, the bank and the kernels that compose them; the per-target headroom table with the P4 and S31 as the MCU home of the family. From the power-functions top-down, carried unchanged: dimension-generic; one contract everywhere with per-target acceleration; fixed point invisible to the writer; the 16-bit contract (`angle16`, `frac16`, `pos_t` 24.8); particles as the stateful precedent; determinism (pure function of position, time, seed; stateful kernels declare a resync point); golden frames as hashes; the `Canvas` descriptor.

Product-owner decisions, 2026-09-03:

1. **Wide color state: the 16-bit Layer**, as the cleanest, least-code, fastest alternative; decided per Layer at run time from free memory with 8-bit as the fallback (option B, agreed); and **quantized once per driver in `Correction`**, so the pipeline is 16-bit end to end and a 16-bit LED is a driver, not a pipeline change (agreed). Detail in § 2.
2. **Gradient noise replaces value noise in place.** One solution; no second function kept for compatibility (CLAUDE.md minimalism).
3. **MoonLive per phase, same PR.**
4. **Stable fluids in scope**, as the last phase; if it costs CPU it is a P4 and desktop showcase.
5. **Every effect runs on the power functions for whatever they cover; an effect that does not is rewritten.** Most effects were developed quickly to demonstrate a mechanism, and keeping them rendering the same is not a goal. The rule is a mandate, not an option: an effect whose mechanism is one the library provides (motion, fields, noise, polar addressing, trails, particles, oscillators) is rewritten on the library's kernel. Bouncing balls run on `particles`; every noise-like effect runs on the noise this plan builds; every trail decays by half-life; every polar effect reads the LUT. The one exception is the **algorithmic** effect, where a rule set *is* the effect rather than a natural motion: Game of Life is the type case, and it keeps its logic and uses the library only to draw. The guard that came with "behave identically first" stays: an early Game of Life port was much worse than its MoonLight origin, so a rewrite is compared against the effect as it ran and lands only if at least as beautiful on the panel. The aim is effects at a level no other LED firmware reaches, not a 1:1 rendition of another project's catalog.

## 1. Homes and style ✅ *(the rule; the headers are new or grown)*

The power-functions rule stands: grow existing homes, one style, free functions over `const Canvas&`, fixed-point vocabulary, a doc block naming the canonical source. **(proposal)**

| Home | Gains | Notes |
|---|---|---|
| `core/noise.h` | gradient noise behind the existing `inoise8/16`, `fbm`, `warp`, `turbulence` names; `curl16` | Domain-neutral. The name promises the field; the algorithm changes underneath (decision 2). |
| `core/oscillators.h` **(new)** | `Oscillators`: N clocks, four LFO shapes each, modulation binding, one master speed, `dt`-driven | Domain-neutral: audio meters and services want the same LFOs. |
| `light/polar.h` **(new)** | `PolarLut`: `angle16` and radius per pixel, built at `prepare()`, 8-bit default with a 16-bit opt-in (the power-functions decision) | Was planned there and never built; `kaleido` stays in `math16.h`. |
| `light/draw.h` | `advect` (separable and 2D), the velocity rules, `decay`, `lineAA`, `disc`, `quantize` with dithering | The frame-ops family, which is where `scroll` and `blur` already live. |
| `light/layers/Buffer.h`, `BlendMap.h`, `Layer.h` | bytes per channel; the 16-bit format and its quantize-last | § 2. |
| `light/fluid.h` **(new, last)** | `Fluid`: velocity grids, diffuse, project, advect, vorticity confinement, dye transport | Stateful kernel with a resync point, like particles. |
| `light/moonlive/MoonLiveBuiltins_light.h` | one builtin per kernel, per phase | § 4. |

## 2. The 16-bit Layer

### What the codebase says

[architecture.md § Memory strategy](../architecture.md#memory-strategy): all buffers are raw `uint8_t*` arrays sized `channelsPerLight × nrOfLights`; "there is no fixed channel layout: `channelsPerLight` is a runtime value, so RGB, RGBW and multi-channel DMX fixtures all use the same code path; the buffer simply gets wider". Adaptive allocation checks heap before every allocation with a 32 KB reserve, and the degradation cascade "reduces layer dimensions until the buffer fits, minimum 8×8". The architecture "does not assume PSRAM"; the classic without PSRAM is "proven up to 16 K lights (128×128 measured live on Olimex)". CLAUDE.md: minimal memory, fastest hot path, "the standard, complete construct beats a hand-rolled special case", and "no `#ifdef`, no per-light virtual call, data over objects". The LED-driver analysis already planned this as its mode 3, "16-bit pipeline (incl. dither): doubles RAM; best gradient quality; required for 16-bit-native LEDs (UCS7604, HD108)", with the driver declaring its input width and the pipeline building Layer buffers accordingly ([leddriver-analysis-top-down.md § 7.3](../history/leddriver-analysis-top-down.md)).

So the width is a property the architecture already treats as runtime data (like `channelsPerLight`), the cascade already knows how to shrink a layer that does not fit, and the pipeline decision was already made in principle. What is new is doing it.

### Memory, per fixture, RGB, one layer

The last column is an **illustration** from the classic's measured free heap today (~104 KB running at 128² with mirror, Ethernet and mDNS up), not a threshold: the decision is made at run time from free memory, never from the grid size, so the same layout may run 16-bit on one device and 8-bit on another depending on what else is loaded.

| Grid | 8-bit | 16-bit | Illustration: would it fit today's measured classic heap without PSRAM? |
|---|---:|---:|---|
| 16×16 | 768 B | 1.5 KB | yes |
| 32×32 | 3 KB | 6 KB | yes |
| 64×64 | 12 KB | 24.6 KB | yes |
| 96×96 | 27 KB | 55 KB | yes, one layer |
| 128×128 | 49 KB | 98 KB | **no** as a second allocation beside the 49 KB output buffer: the reserve is 32 KB |
| 48×256 (the wall) | 37 KB | 74 KB | PSRAM already required by `nrOfLightsType` |

The driver output buffer, where one exists (two or more layers, or a mapping LUT), follows the Layer's width, so the composite case costs two 16-bit buffers; it is part of the same runtime allocation check. Advection needs a scratch copy for the separable pass (a second buffer of the same size) and, for the fluid solver, velocity, pressure and divergence grids (four more `int32` planes). Those are effect-owned `ScratchBuffer`s, allocated at `prepare()`, reported through `dynamicBytes()`, PSRAM-preferred; the memory ladder shows them exactly as it shows a particle pool.

### Can 8 or 16 be decided dynamically without hot-path cost? Yes, and it is the existing mechanism

The width is decided **per Layer at `prepare()`**, on the cold path, by the same allocation check that decides whether a LUT or an output buffer exists: the Layer attempts the 16-bit allocation against the free heap after the 32 KB reserve, keeps it when it succeeds, and otherwise takes 8-bit as the first step of the cascade (before dimension reduction). It is a measurement, not a table: no grid size is special, and the answer re-decides on every `prepare()` (a layout change, an added layer, a freed module). Every per-pixel loop that reads or writes Layer bytes is written **once as a template on the sample type** and instantiated for `uint8_t` and `uint16_t`; the choice happens **once per pass per frame** (a function pointer or a branch at the top of `blendMap`, `extrude`, `fadeToBlackBy`, `advect`), never per light. That is the "data over objects, no virtual call per light" rule satisfied, and it is one algorithm, not two implementations. The price is code size (two instantiations of the frame-op set) and a second configuration to test, which the goldens cover by running both widths.

**(proposal)** Two options, with the recommendation:

- **A. 16-bit only.** One format, the cascade shrinks a layer that does not fit. Least code and one test configuration; the classic without PSRAM loses its stated 128² full pipeline (it drops to 96² single-layer, 64² with two layers), which is a documented capability regression on a shipping board class.
- **B. 16-bit default, 8-bit as the first cascade step** (recommended). One template, two instantiations, chosen per layer on the cold path; the classic keeps 128²; the hot path pays nothing per light. Minimalism is respected in the way that matters here: one algorithm with one home, and the 8-bit path is a fallback the architecture already promises, not a second feature.

### Quantize last: once, per driver, in `Correction` (agreed)

The pipeline is 16-bit end to end: Layer → `blendMap` → output buffer, all at the Layer's width. Quantization happens **once, in `Correction::apply`, per driver**, which every driver already runs through `DriverBase` (LED, ArtNet/DDP/E1.31, PanelCard, Hue, NDI, HLS, Preview): white balance, brightness, the power limiter and the perceptual curve run at 16-bit input, and the last step narrows to the width the driver's wire declares. **8-bit with temporal dithering** for WS2812-class LEDs and every network, video and preview output; **16-bit straight through** for a 16-bit LED. This is mode 3 of the LED-driver analysis ("driver input 16-bit, wire 8-bit, the driver downsamples and dithers; required for 16-bit-native LEDs"), and its `inputBitsPerChannel()` stub becomes real: the driver declares its wire width, `Correction` quantizes to it.

Why here and not at `blendMap`: quantizing earlier throws the extra bits away before the perceptual curve, which is exactly where 8-bit input crushes the low end, and it makes a 16-bit LED impossible. One site for the whole system also means one dither state (per driver, one byte per channel of carried error, or a stateless ordered matrix) and one place to test.

Two consequences. The zero-copy path (a single layer with no LUT, the driver reading the Layer directly) needs no view: `Correction` reads the Layer's width like any other source. The `Correction` LUTs stay 256 entries with interpolation on the 16-bit input (the shape `sin16` uses), not a 64K table. In phase 2 the narrowing is `>> 8` so every existing effect renders bit-identically and no golden moves; dithering is the phase 4 switch.

**The four combinations, and the hot path.** The Layer's width (decided by memory) and the wire's width (declared by the driver) are independent facts that meet in one `Correction` template; the combinations fall out, none is a mode to maintain:

| Layer | Wire | `Correction` does | When |
|---|---|---|---|
| 16 | 16 | curve and brightness at 16, no quantization | the full pipeline: a PSRAM board with a 16-bit LED |
| 16 | 8 | curve and brightness at 16, quantize once with dithering | the common case: 16-bit fits, WS2812-class LEDs, and every network, video and preview output |
| 8 | 8 | today's path, byte for byte | a board where memory forced the 8-bit fallback |
| 8 | 16 | widen (`v · 257`), curve and brightness at 16, out at 16 | the fallback board driving a 16-bit LED; smoother dimming than 8-bit scaling, the effect's own 8-bit steps remain |

No path you have today gets slower: the 8-bit instantiations are the same code compiled for `uint8_t`, chosen once per pass per frame, so the per-light instruction count on an 8-bit Layer is unchanged. The 16-bit paths cost what they carry: twice the bytes moved (the S3's PSRAM latency shows this on cheap effects; the KPI gate reads it per batch), the same ALU ops on a 32-bit core, one interpolation per channel in `Correction` on top of the LUT lookup, and dithering as one add and one compare per channel, only on a 16-to-8 wire and only when switched on. No per-light branch, no virtual call, no allocation on the path.

**16-bit LEDs are enabled, not built.** HD108 (16-bit per channel, clocked SPI) and UCS7604 (16-bit per channel, one-wire) are the known families; APA102/SK9822 are 8-bit color with a 5-bit current gain and do not qualify. Each is its own driver increment: verify the datasheet first, then an encoder that takes 16-bit wire values from `Correction` (an SPI driver for HD108, a wider symbol table for UCS7604).

### The surface, counted

What touches Layer bytes and therefore changes in phase 2: `Buffer` (bytes per channel, 1 or 2), `Canvas` (a sample-width field), the 25 `Canvas` and 12 `Buffer` overloads in `draw.h` (one template each), `BlendMap` (identity and LUT paths, both widths), `Layer::extrude` and `fadeToBlackBy` (bytes-per-light already, width-agnostic after the template), `Correction::apply` (16-bit input, quantize to the declared wire width; the one quantization site), `DriverBase` (the declared wire width), `PreviewDriver` (quantized through the same path), `NetworkReceiveEffect` (widens), the **7 effects that write raw bytes** (AudioVolume, BouncingBalls, Fire, Lines, NetworkReceive, Rainbow, Noise), `colorFromPalette` (unchanged: returns 8-bit RGB, widened at the write), the MoonLive `StoreElem` / `FillElems` inline ops on all three backends (element = `channelsPerLight` × the Layer's bytes per channel, NOT a fixed 3 × 2: the ops already store N bytes rather than a fixed RGB, and a phase that hardcoded three channels would break the RGBW and multi-channel DMX fixtures the same buffer serves), and the zero-copy driver path. Modifiers are coordinate-only and untouched; the mapping LUT is index-only and untouched.

## 3. The kernels

Signatures are shapes, not final names; the PR is the spec. All fixed point, all `dt`-driven where time enters, all pure functions of (position, time, seed) unless stated. **(proposal)**

### 3.1 Gradient noise, in place (decision 2)

`inoise8/16(x[,y[,z]])` keep their names, their 16.0 coordinate convention and their output ranges; the implementation becomes **Perlin's improved noise** (2002): a 256-entry permutation table, gradients from the 12 edge directions of a cube (8 in 2D), the quintic fade `6t⁵ − 15t⁴ + 10t³`, integer throughout, output rescaled to the full range the header already promises. `fbm`, `warp` and `turbulence` sit on top unchanged. The seven noise effects (Noise, NoiseMeter, Noise2D, PolarNoise, Wave, Tunnel, and EffectBase's users) change look in one commit; their goldens move in that commit with the reason in the message. Under decision 5 the question on the panel is whether they look at least as good as before; any that look worse are re-tuned before the swap is accepted, since a noise effect that ran beautifully on value noise may not degrade on gradient noise. The header's note that ports from FastLED "look slightly different for a reason the author cannot see" is deleted: they no longer do.

Cost target: within 1.3× of the current value noise per sample (the gradient dot products replace the value lerps; the fade is the same), measured by the micro-bench in phase 0 before the swap is accepted.

### 3.2 `PolarLut` (`light/polar.h`)

```cpp
polar::Lut lut;                              // POD over ScratchBuffer
lut.build(dims, center, precision);          // prepare(): angle16 + radius per pixel; 8-bit each by default, 16-bit opt-in
lut.angle(i); lut.radius(i);                 // hot path: two loads per pixel, no atan, no sqrt
```

Rebuilt on geometry or center change through `affectsPrepare`; reported as `dynamicBytes`. `PolarNoiseEffect` migrates to it (pixel-identical at the 16-bit precision, golden-pinned; the 8-bit default is a deliberate, bench-judged divergence). A polar modifier consuming the same LUT is the modifier-first path the earlier decision named; it is not in these phases.

### 3.3 `Oscillators` (`core/oscillators.h`)

```cpp
Oscillators<N> osc;                          // N clocks; N is the effect's choice (4 to 20)
osc.set(i, ratio, offset);                   // per clock, at prepare() or on control change
osc.advance(dtMs, masterSpeed);              // once per frame: phase_i += dt · ratio_i · master
osc.ramp(i); osc.phase(i);                   // 32-bit ramp; angle16 wrapped phase
osc.bipolar(i); osc.unipolar(i);             // sin16 of the phase; 0..65535 form
osc.noiseAngle(i);                           // angle16 from inoise16(ramp), the wandering angle
osc.modulate(base, i, depth);                // base · (1 + depth · bipolar(i)), the multiplicative binding
```

Frame-rate independent by construction (`dt`, never a frame count, per coding-standards § Animate on elapsed time). Determinism: the phases derive from the shared time origin, so two devices agree (§ 6b of the power-functions top-down). `BeatPhase` stays for single-tempo effects.

### 3.4 Velocity rules and `curl16` (`draw.h`, `noise.h`)

A velocity rule is a function `(x, y, t, params) → (vx, vy)` in `pos_t` per frame; none is stored. The set from Part 1: `flowWind` (direction, speed, rotation rate, perpendicular wobble), `flowRadial` (in or out), `flowSpiral` (angular step, radial step), `flowRings` (three zones with smoothstep boundaries, per-zone swirl and drift), `flowNoise` (two 1D profiles, one per axis, decoupled), and `curl16(x, y, t, scale)`: the perpendicular gradient of a scalar noise potential by two central differences, divergence-free by construction (Bridson 2007). The profiles of `flowNoise` are computed once per row and column per frame, not per pixel.

### 3.5 `advect` (`draw.h`)

```cpp
draw::advect(cv, scratch, rule, dtMs, Edge::Wrap|Clamp);   // full 2D: backward sample, bilinear, per channel
draw::advectSeparable(cv, scratch, xProf, yProf, Edge);    // two 1D passes through the scratch plane
```

The semi-Lagrangian step (Stam 1999): for each destination pixel, `src = dst − v · dt`, read the previous frame at `src` with bilinear interpolation (four loads, three lerps per channel), write to `scratch`, swap. On the 16-bit Layer the color state is the Layer itself; `scratch` is one effect-owned plane of the same width. Partial transport (`blend` fraction) is a parameter. Budget: ~40 cycles per pixel per channel per pass on the S3 (the field's 80% share is what this is measured against).

### 3.6 `decay` (`draw.h`)

`draw::decay(cv, halfLifeMs, dtMs)`: multiply every sample by `k = 0.5^(dt / t½)`, `k` computed once per frame from a 16-bit `pow2` table. Framerate-independent by definition; a unit test pins `decay(2·dt) == decay(dt)²` within one LSB. The Layer's collected `fadeToBlackBy` remains the one 8-bit-per-frame decay for effects that want it; `decay` is the half-life form on wide state.

### 3.7 `lineAA`, `disc` (`draw.h`)

`lineAA` steps the segment at ~3 samples per pixel and splats each through the existing 2×2 bilinear `splat` (Wu 1991). `disc(cv, center, radius, color)` writes coverage `clamp(r + 0.5 − dist, 0, 1)` per pixel in the bounding box, using the squared-distance `coverage` path already in `draw.h`. Both additive with saturation, the particle default.

### 3.8 `quantize` with dithering (`draw.h`, phase 4)

`draw::quantize(src16, dst8, ditherState)`: `>> 8` in phase 2; in phase 4 the truncation error is carried per pixel into the next frame (one byte per channel of state, effect-owned, or a 4×4 ordered matrix with no state, selectable). Applies at every 16-to-8 boundary listed in § 2.

### 3.9 `Fluid` (`light/fluid.h`, phase 5)

The Stam 1999 solver over effect-owned `int32` Q16.16 grids: `diffuse(velocity)`, `project()` (Jacobi, iteration count a control, default 5), `advect(velocity by itself)`, `project()`, optional `vorticityConfinement()`, then dye (the Layer) advected by `draw::advect` with the stored field. Wall boundaries; `addVelocity(x, y, dvx, dvy)` and the Layer write are the source terms. Declares a resync point (re-seed to rest). Sized for panels; the P4 and desktop are its home.

## 4. MoonLive: the frame kernels, per phase (decision 3)

**The Layer is the state, so the kernels are whole-frame builtins on the script's layer**, the shape `fill(r,g,b)` and `fade(amt)` already have: no handle, no arena bytes, one host call per frame. Stateful helpers that are not the frame (the polar LUT, the oscillator bank, the fluid grids) are **handles declared in `defineControls()`**, exactly like `pool(n)`: sized once, allocated by the binding outside the arena, whole-object passes per frame. Multi-argument host calls ship (`emit` takes seven), the builtin table holds 64, so nothing waits on the engine. **(proposal)**

| Phase | Builtins | Script shipped |
|---|---|---|
| 1 fields | `polar(precision)` handle; `polarA()`/`polarR()` become inline loads from the LUT when one is declared (today they compute per pixel); `oscillators(n)`, `lfoSet(i, ratio, offset)`, `ramp(i)`, `phase(i)`, `bipolar(i)`, `unipolar(i)`, `noiseAngle(i)`, `modulate(v, i, depth)`; `gradient noise` is invisible: `noise()` just improves | `aurora.mle` |
| 3 advection | `flowWind(...)`, `flowRadial(...)`, `flowSpiral(...)`, `flowRings(...)`, `flowNoise(...)`, `flowCurl(...)` (each sets this frame's rule), `advect(blend)`, `decay(halfLifeMs)`, `disc(x, y, r, hue)`, `lineAA(x0, y0, x1, y1, hue)` | `trails.mle` |
| 4 composition | none new; `aurora.mle` as an emitter inside `nebula.mle` is composition of existing builtins | `nebula.mle` |
| 5 fluid | `fluid(iterations)` handle, `fluidJet(x, y, angle, force)`, `fluidStep(viscosity, vorticity, gravity)` | `fluid.mle` |

The per-pixel shader path improves without new builtins: the LUT turns `polarA`/`polarR` from a 3.5 µs square root into two loads, and the roadmap's inline-op work (§ 4c) is what removes the remaining call cost; that work is the roadmap's, not this document's.

The one engine change this plan requires: the `StoreElem` and `FillElems` inline ops lower a 16-bit element on all three backends when the layer is 16-bit (phase 2). The lowering already carries the element width as "N bytes"; the width becomes a per-binding constant.

## 5. Performance plan

**Currencies.** Shaders: samples per pixel, against ~750 cycles per sample on the S3 and ~250 on the P4 (bottom-up, Part 3), with the gradient-noise swap held within 1.3× of that. Advection: cycles per frame and bytes of state. The target is ~40 cycles per pixel per channel PER PASS, and separable advection is two passes (x then y), so a 128² RGB advect is 128·128·3·40·2 cycles: **~16 ms on the S3** at 240 MHz and **~10 ms on the P4** at 400 MHz. (An earlier draft said 4 ms and 1.3 ms, which matched neither one pass nor two: the arithmetic gives 8.2 ms for a single S3 pass. The numbers below follow from the corrected figures.) That is 60 fps on the P4 with room for the emitters, and on the S3 it is the whole frame at 60 fps, so a 128² wall runs advection at 30 fps or drops to 64² for 50. The phase-3 scenario sets the contract per target from measurement rather than from this estimate.

**Framerate as the rendering method, by rule.** Every kernel takes `dt`; no kernel takes a frame count. `advect` clamps the per-frame displacement to one pixel and reports when it clamps (the `frameTime`-style status), so a slow frame smears rather than tears. `decay` is a half-life. The oscillator bank is `dt`-integrated. A showcase's stated fps on its stated fixture is a scenario contract; when it fails, the default of its cost knob moves, not the contract.

**Levers, in the order the plan pulls them.** LUTs (phase 1); fixed point (already); cost knobs as controls with honest defaults (every showcase); field below output resolution with bilinear upscale (phase 4: a `fieldScale` control, the shader renders into a smaller scratch plane and `draw::upscale` bilinearly fills the layer); field below frame rate (phase 4: `fieldRate`, the shader updates every N frames and the oscillators still advance every frame); SIMD (phase 4: a PIE variant of the gradient-noise inner loop and of the bilinear lerp for the S3 and P4, behind the platform boundary, selected by `platform_config.h` capability flags, bit-identical to the scalar form and tested as such); FPU (a float variant of `curl16` and the fluid solver where `hasFpu`, the `raymarch.h` precedent, same bit-identical-within-tolerance test); a second core (the field update as a `multicore` producer, not in these phases).

**Where the desktop takes over.** The same effects, the same controls; the desktop as processing node feeds a wall over ArtNet or DDP. The per-target headroom table in the bottom-up is the guide: rich compositions above 32² (S3) or 64² (P4) are desktop work, and a showcase's card says so.

## 6. Showcases

Each is a compiled effect and a MoonLive script of the same look, so the script proves the builtin surface. Controls are the cost knobs and the look knobs, with defaults that hold the stated fps on the stated fixture. **(proposal)**

**Aurora** (phase 1; shader; 🔬 2D). Three gradient-noise layers over a `PolarLut`, each layer's transform driven by its own oscillators; the layers become R, G, B through a contrast window and a palette blend. Controls: `speed` (master), `scale` (cells across the short side), `layers` (1 to 4, the cost knob), `warp` (angle displacement by noise), `twist` (radius shears the angle), `segments` (kaleidoscope fold, 1 = off), `contrast` (low/high window width), `palette` (the shared control). Targets: S3 32² at 100 fps with 3 layers; P4 64² at 60 fps with 3 layers; desktop 128² at 200 fps.

**Trails** (phase 3; advection; 🔬 2D). Emitters drawn into the 16-bit layer, then advected and decayed. Controls: `emitter` (orbital dots, Lissajous line, both), `dots` (1 to 8), `orbit` (diameter), `flow` (wind, radial, spiral, rings, noise, curl), `strength` (pixels per second), `blend` (partial transport), `persistence` (half-life in ms), `breathe` (modulation depth on strength and orbit, from the bank), `palette`. Targets: S3 64² at 60 fps; P4 128² at 50 fps; classic 32² at 60 fps in 16-bit.

**Nebula** (phase 4; composition; 🔬 2D). The Aurora field, thresholded by its contrast window so most of it is black, is the emitter; a curl-noise flow carries it. Controls: Aurora's `layers`/`scale`/`contrast`, Trails' `strength`/`persistence`/`blend`, plus `fieldScale` (the field at half or quarter resolution) and `fieldRate` (the field every N frames). Targets: S3 64² at 50 fps with the field at half resolution; P4 128² at 50 fps; desktop unconstrained.

**Fluid** (phase 5; solver; 🔬 2D; P4 and desktop). A jet emitter into a Stam solver, dye on the 16-bit layer. Controls: `jets` (1 to 4), `force`, `angle` (oscillator-modulated), `viscosity`, `vorticity`, `gravity`, `iterations` (the cost knob), `persistence`, `palette`. Targets: P4 64² at 50 fps with 5 iterations; desktop 128² at 100 fps; on the S3 and classic it runs at whatever the panel allows and its card says so.

Each showcase gets its card in [effects.md](../moonmodules/light/effects.md) in the PolarNoise form (one line per control, the cost knob named as such), and its numbers in [performance.md](../performance.md).

## 7. Tests and bench criteria

- **Unit, per kernel, behavior-named.** Gradient noise: zero-mean over a large sample, range, continuity across cell boundaries, the same value on every ISA (a fixed-seed table of 64 samples). `PolarLut`: the LUT equals per-pixel `atan16`/`dist16` at 16-bit precision; the 8-bit default within one LSB of angle. `Oscillators`: `dt`-independence (two steps of `dt` equal one of `2·dt`), wrap, modulation bounds. `advect`: a single lit pixel moves exactly `v·dt` (sub-pixel, so two neighbors share it by the bilinear weights), wrap and clamp edges, the separable form equals the 2D form for an axis-aligned field, no write outside the buffer. `decay`: `decay(2·dt) == decay(dt)²` within one LSB, and a value never rises. `curl16`: divergence of the sampled field is zero within tolerance on a grid. `lineAA`/`disc`: total coverage of a disc equals its area within 2%; a line's coverage is independent of direction within 5%. `quantize`: `>> 8` bit-identical to the 8-bit path; dithering averages to the 16-bit value over 256 frames. 16-bit Layer: every `draw::` primitive tested at both widths.
- **Goldens.** A golden pins the plumbing, not the look (decision 5): phase 2 moves none, because a bit-identical quantize is the proof that widening the Layer is mechanical; phase 0 and phase 6 move goldens deliberately, with the reason in the commit and the product owner's comparison against the old frame on the panel, and that is the harness working. Each showcase adds its golden at both widths.
- **Scenarios, per family.** Fields: a `PolarLut` sized at prepare and rebuilt on a grid resize, with the memory ladder. Advection: Trails on a live pipeline at 16², 32², 64², 128² with the scratch plane in the ladder and the tick within its per-target contract. The 16-bit Layer: the existing `scenario_Layer_memory_1to1` and `scenario_MultiplyModifier_memory_lut` at both widths, and the cascade stepping from 16 to 8 bits on a constrained heap. Fluid: its grids in the ladder and its tick contract on the P4. Every showcase's fps target is a `contract` in its scenario, per target.
- **Perf.** The micro-bench target from the power-functions plan gains a row per kernel; `collect_kpi.py --commit` per phase on the S3 and P4; the desktop tick is the fast alarm.
- **The final gate is the product owner's eyes.** Per phase: the S3 bench board (shiffy, 80×48) for the panel class, the P4 (.139) for the MCU home, the desktop at 128×96 for the ceiling, and a wall run for Trails and Nebula. The criteria are visual: no tearing at the stated fps, no visible steps in a trail's tail, a shader that turns rather than scrolls, and the classic at 32² still smooth.

## 8. Step-by-step implementation plan

The steps below are work order, not commit order. Each step is finished on the desktop first, then the S3, then the P4, then the wall where it applies, and the product owner tests as steps land. **When to commit, and how much rides in one commit or PR, is the product owner's call** ([CLAUDE.md § Commit](../../CLAUDE.md#commit)): the pre-commit and pre-merge gates cost real time, so a commit carries several steps and a PR carries several phases. Nothing here implies one PR per phase. Order chosen so the first showcase lands before the one cross-cutting change.

### Phase 0: measure, then swap the noise (small)
1. Add the kernel rows to the micro-bench target: value noise as it is, per sample, on desktop; record.
2. Implement Perlin improved noise behind `inoise8/16` (2D and 3D; 1D as the 2D with y = 0), integer, same coordinate convention, same output ranges. Delete the "value noise" note in `noise.h`.
3. Run the micro-bench: accept only within 1.3× per sample; else optimize (the 8-gradient 2D form, a 12-entry gradient table) before continuing.
4. Desktop build, `ctest`: the seven noise goldens fail as expected; update them in the same commit with the reason.
5. On the product owner's go-ahead (CLAUDE.md § Build: a board is written to only when they say so), flash S3 and P4; `collect_kpi.py --commit`; they judge Noise, PolarNoise and Tunnel on the panel. Catalog cards: no text change unless the look note in a card mentions value noise.

### Phase 1: fields and Aurora (medium)
1. `core/oscillators.h` with its unit tests.
2. `light/polar.h`: `PolarLut` with 8-bit default and 16-bit opt-in; unit tests against `atan16`/`dist16`.
3. Migrate `PolarNoiseEffect` to the LUT (golden-pinned at 16-bit; the 8-bit default is a deliberate move judged on the panel) and to the bank for its drift.
4. `AuroraEffect`: the three-layer shader, controls per § 6, card in effects.md, golden.
5. MoonLive: `polar()` and `oscillators()` handles with their builtins; `polarA`/`polarR` read the LUT when declared; `aurora.mle`; the compile-every-script test and a script golden.
6. Scenario: fields (LUT rebuild on resize, memory ladder), Aurora's fps contract per target.
7. Desktop first; then S3 and P4 on the product owner's go-ahead; performance.md rows; their look.

### Open from phase 1

**Aurora hitches on the desktop and not on the S3 (2026-09-04).** The product owner sees a periodic
hitch in the desktop preview; the same firmware on an ESP32-S3 at 64x64 looks smooth, and the S3's
LEDs were not yet judged. Five causes were tested and ruled out with measurements:

| ruled out | the measurement |
|---|---|
| an integer overflow | every expression stays in range at every control setting, checked by hand |
| the drift sawtooth wrapping | worst single-frame field change at a wrap is 7 of 255, no spike |
| the field moving too fast | per FRAME the desktop moves 15x LESS than the S3 (0.009 against 0.136 noise cells), so it should read smoother |
| the contrast window's per-frame easing | its absolute worst frame is SMALLER at 385 fps (17.6) than at 25 fps (30.2) |
| the preview transport at a large grid | the product owner set the desktop to 64x64, matching the S3, and the hitch stayed |

What is known: brightness changes are distributed rather than discrete (median 17 per frame, 99th
percentile 101), so there is no single glitching frame; and the desktop renders at ~385 fps against
the S3's 25, which is the one large difference left standing. The next thing to try is a frame-rate
cap on the desktop: if the hitch tracks the render rate rather than the wall clock, something in the
pipeline is per-frame where it should be per-unit-time, and the effect is not the place to look.

**Aurora's layer crossover (2026-09-04).** With a winner-takes-all palette index, 12.7% of pixels
change which layer owns their color every frame, and 35% on the worst frame; each change moves the
hue a whole palette region. A weighted crossover (cubed weights, so a dominant layer still
dominates) removes the discontinuity and measured 2.7x less frame-to-frame change, for 3.4% more
time per pixel (42.8 against 41.4 ns on the host). Built, measured, then REVERTED on the product
owner's call: it did not address the hitch above, and the cost is real. Revisit when the hitch is
understood, since the two may be related.

### Phase 1b: three dimensions everywhere (medium)

Dimension-generic is a stated input of this plan (§ 0), and the library is not there yet. The gap is
uneven rather than absent, and the hot-path worry it raises turns out not to be the blocker.

**Where it already holds.** `draw::` is `Coord3D` throughout (pixel, line, blendPixel, addPixel,
glyph). `blur` runs a third pass behind `if (z > 1)`, so a 2D layout pays nothing for the capability.
`inoise8` and `inoise16` have real 1D, 2D and 3D forms, each compiling to its own straight-line body
over exactly its corners after the phase 0 swap.

**Where it does not.** `fbm16`, `turbulence8` and `warp8` stop at 2D; only `fbm8` has a 3D overload.
`PolarLut` stores angle and radius on a plane, with no z. `atan16` and `dist16` are planar by nature.

**A 3D-capable kernel costs a 2D layout nothing**, and the tree already shows both ways to get that:
the third axis is either a template parameter that compiles away (`if constexpr (Dims > 2)` in the
noise core) or a runtime guard on a value that is 1 (`blur`). Neither is a per-pixel branch. So the
missing overloads are mechanical.

The one that is not mechanical is **polar in three dimensions**, and the answer is a control rather
than a decision. Spherical (two angles and a radius), cylindrical (an angle, a radius and a height)
and distance-from-center are three different looks, and which one is right is a property of the
FIXTURE: a sphere wants the first, a tube or curtain the second, a cube may want any of them. The
choice costs nothing per pixel because it is made once when the table is built, and the table is the
same size either way. Cylindrical is the default: it reduces to exactly today's behavior when depth
is 1, so no existing 2D fixture changes.

**What "dimension-generic" does NOT claim.** The KERNELS take any arity; an EFFECT still looks good
only at the arities its design has. Aurora on a strip is one radial line through the field, which
reads as a slow flicker rather than as curtains, and no amount of library work changes that. The
Layer already handles the mismatch (a D2 effect on a 3D layer has its z=0 slice copied across z, a
D1 effect its column copied across x), so an effect that declares less than the fixture is correct
rather than broken. Which effects earn a genuine 1D or 3D form is per-effect judgment, made in the
catalog sweep, not a property this phase delivers.

1. `fbm16`, `turbulence8` and `warp8` gain their 3D overloads, matching `fbm8`'s existing shape.
2. `PolarLut` gains a z axis and a `mapping` control (cylindrical default, spherical, radial), with
   the 2D path bit-identical to today's at depth 1.
3. The effects that are `Dim::D2` only because their kernels were: Aurora, PolarNoise and Tunnel are
   the three that a volumetric fixture would show something new, and all three are blocked on step 2
   rather than on their own code. Noise2D needs no polar work at all: it already samples 3D noise
   with time on z, so a volumetric form is passing the light's own z instead.
4. ✅ **Noise2D and Noise were the same effect**, and are now one: Noise
   already uses the light's real z when depth > 1, and the two differ only in control names and
   scale defaults (`bpm`/4 against `speed`/64). Merging them is a catalog decision with a golden and
   a card, so it was done here rather than deferred: `motion` chooses drift (the old Noise) or morph
   (the old Noise2D), the drift golden is unchanged, and the second effect is deleted.
5. Unit tests per kernel that a 2D call is exactly the 3D call with z held at zero, which is what
   makes the extension safe to make everywhere else. ✅ *(shipped: `fbm8`, `fbm16`, `turbulence8` and
   `warp8` all have 3D forms, and `warp8`'s 2D form is now literally the 3D one at z = 0, so the two
   cannot drift apart.)*

### Phase 1c: Coord3D in MoonLive (medium)

A script computes a position as three loose integers today, and flattens it by hand
(`mod(bx+dx, width) + mod(by+dy, height) * width`), which is the buffer layout leaking into every
effect. The engine already passes `Coord3D` everywhere, so the script vocabulary is the odd one out.

This is **not** general struct support. The MoonLive roadmap settles that: a predefined struct is one
the compiler knows by layout, needing no user-declarable struct machinery
([roadmap § 4b](moonlive-language-roadmap.md), with user structs deferred to § 10 for readability
rather than capability). It depends on the multi-value call ABI (§ 2 there), which is what makes a
coordinate in and a color out expressible at all.

**`Coord3D` yes; the color struct waits for phase 2.** The roadmap pairs `Coord3D` with a `CRGB`,
and that pairing predates the 16-bit decision. A script-visible color fixed at three `uint8_t` would
be the one place in the pipeline where the channel width stops being runtime data, exactly as phase 2
makes `draw.h` a template over both widths and has `Correction` quantize once at the driver. So the
color type is specified AFTER phase 2 knows what it is, and it takes our own name rather than
FastLED's (CLAUDE.md: our own code, our own names). A coordinate has no such dependency, which is
why it goes first.

**How far it goes: as far as every other type.** A predefined struct that only appears in builtin
signatures would be a special case, and the language does not need another one. `Coord3D` is a
class member, a local, a function argument and a return value, the same as `int`, `byte`, `bool` and
`fixed`. Arguments and returns are roadmap § 6, which this step therefore depends on. Two
consequences worth stating: a member costs three of the 8 member records and 6 of the 64 arena bytes
unless the compiler packs it as one record, which is worth deciding when the type is added rather
than after; and a local costs one frame slot per field under today's flat allocator, so a script
holding several coordinates meets the 16-slot ceiling faster (§ 8b there).

Ordering note: this is the step that makes a 3D script possible, so it follows phase 1b rather than
leading it. A script that can hold a coordinate but calls kernels that ignore z has gained nothing.

### Phase 2: the 16-bit Layer (large, cross-cutting)
1. `Buffer`: bytes per channel; `Canvas`: sample width; `Layer::prepare()` picks the width by the allocation check (option B) or fixed 16 (option A), per the sign-off.
2. `draw.h`: every `Canvas` and `Buffer` primitive as one template instantiated for both widths; `get`/`pixel` widen 8-bit RGB on write and narrow on read for the existing callers.
3. `BlendMap`: both paths templated; the output buffer follows the Layer's width.
4. `Correction::apply` reads 16-bit input and quantizes (`>> 8` in this phase) to the wire width `DriverBase` declares (8 for every driver today); the preview goes through the same path; `NetworkReceiveEffect` widens. No quantizing view is needed on the zero-copy path.
5. The 7 raw-byte effects migrated to `draw::` (which the power-functions plan already owed).
6. MoonLive `StoreElem`/`FillElems` at 16-bit on Xtensa, RISC-V and the host backend; the existing script tests at both widths.
7. Every golden at both widths, all unchanged (bit-identical quantize is the acceptance test of this phase).
8. Scenarios: the memory scenarios at both widths; the cascade from 16 to 8 on a constrained heap (the classic at 128²).
9. KPI on S3, P4 and classic: blend, extrude and preview cost at 16-bit read against the table; the S3's PSRAM latency is the number to watch.
10. Docs: architecture.md § Buffer types, memory strategy, the leddriver mode-3 note closed, MIGRATING.md if any API a script or driver sees changed.

### Phase 3: advection and Trails (medium)
1. `advect` (2D and separable), the velocity rules, `curl16`, `decay`, `lineAA`, `disc`, with their unit tests.
2. `TrailsEffect`: emitters, flows, the bank for breathing, scratch plane in `ScratchBuffer`, card, golden.
3. MoonLive: the flow, advect, decay and emitter builtins; `trails.mle`; script golden.
4. Scenario: Trails at four sizes with the ladder and per-target contracts.
5. Desktop, S3, P4, wall; performance.md; the product owner's look (the tail of a trail is the thing to judge).

### Phase 4: composition, dithering, levers, SIMD (medium)
1. `quantize` with temporal dithering, switchable, default per the sign-off; tests.
2. `fieldScale` and `fieldRate` as shared mechanisms (`draw::upscale`, a frame-skip helper on the bank's clock).
3. `NebulaEffect` and `nebula.mle`; card, golden, scenario contract.
4. The PIE variants of the noise inner loop and the bilinear lerp for S3 and P4 behind `src/platform/`, bit-identical tests against the scalar forms; the FPU `curl16` where `hasFpu`.
5. KPI per target; the product owner's look on the wall.

### Phase 5: fluid (medium; P4 and desktop)
1. `light/fluid.h` with its unit tests (divergence after `project` within tolerance; a jet moves dye; rest state is stable).
2. `FluidEffect` and `fluid.mle`; card with its per-target honesty; golden; scenario on the P4.
3. The backlog entry closes; performance.md rows on P4 and desktop.

### After the phases: the catalog sweep (large; its own plan)
Decision 5 mandates that every natural-motion effect runs on the power functions. That sweep is not a phase of this plan: it is sized and sequenced in its own plan once the kernels exist, from the [effects × power functions inventory](effects-power-function-inventory.md), which records for each of the 58 effects what it uses today and what it could use. Two rules carry over: each rewrite is compared on the panel against the effect as it ran and lands only if at least as beautiful, and an effect a showcase supersedes outright is deleted, the way a particle-system effect replaces its non-PS twin.

## 9. Resource accounting

- **Flash:** the gradient noise is a 256-byte permutation plus a 12-entry gradient table; the LFO bank and LUT are small; the 16-bit Layer instantiates the frame-op set twice (estimate low single-digit KB per target ❓, read from the repo-health flash table per batch); the SIMD variants are per-target only.
- **RAM:** all sized buffers are `prepare()`-time, PSRAM-preferred, `dynamicBytes`-reported, zero static (`check_footprint`): the 16-bit Layer per § 2's table; `PolarLut` 2 B/pixel default; the advection scratch plane one Layer's size; the fluid grids four `int32` planes plus scratch (a 64² fluid is ~100 KB, PSRAM class).
- **Cycles:** budgets in § 5; the KPI gate per phase; the desktop tick as the fast alarm.
- **Repo:** goldens are hashes; scenarios record their observations; each phase adds ~1 effect, ~1 script, ~1 scenario.
- **Boundary:** `noise`/`oscillators` core; `polar`/`draw`/`fluid` light; SIMD and FPU variants only under `src/platform/`, selected by `platform_config.h` flags, never an `#ifdef` outside.
- **Complete construct, real consumer:** every kernel lands with its showcase and its script in the same PR.

## 10. Decisions for sign-off

1. ✅ **Layer width mechanism: B** (16-bit default, 8-bit as the first cascade step, decided per Layer at prepare from free memory, one template). Agreed 2026-09-03.
10. ✅ **Quantization lives in `Correction`, once per driver, to the driver's declared wire width**; the pipeline is 16-bit end to end and 16-bit LEDs become driver increments. Agreed 2026-09-03.
2. **Gradient noise swap accepted on the 1.3× per-sample bound**, goldens moved in one commit, judged on S3 and P4 (§ 3.1).
3. **MoonLive kernels operate on the script's Layer; LUT, bank and fluid are handles** (§ 4).
4. **Phase order**: noise, fields and Aurora, the 16-bit Layer, advection and Trails, composition, fluid (§ 8). Alternative: the 16-bit Layer first, if the cross-cutting risk should be retired before any showcase.
5. **Showcase names and targets** (§ 6): the fps-per-fixture numbers become scenario contracts on acceptance.
6. **`PolarLut` 8-bit default** (the earlier decision, restated because Aurora's angular resolution on a 128² wall may want 16-bit as its default) ❓.
7. **Temporal dithering default**: on for 16-bit layers once phase 4 lands, or off with a per-driver switch (the leddriver analysis's per-driver mode) ❓.
8. **Fluid on the S3 and classic**: runs with its card stating the panel size it holds, or hidden behind a capability flag. Recommendation: runs, honestly labeled.
9. **Two documents still state the old stance and need the PO's edit under decision 5**: [power-functions.md § Migrating an effect](../moonmodules/light/power-functions.md) ("Step 1, the port: behave identically") and the header of `test/unit/light/golden_frame.h` ("pixel-identical by default"). Proposal: "faithful first" stays, and its purpose is stated: it guards against degrading an effect into a quick-and-dirty one (the early Game of Life port), not against improving it. Two sentences follow it: every effect runs on the power functions for whatever they cover, and an effect that does not is rewritten on them, with the effect as it ran as the reference and "at least as beautiful on the panel" as the bar; algorithmic effects (a rule set is the effect) keep their logic. The golden is re-baselined in the same commit with the reason, and the harness comment reads "a golden pins the plumbing; a deliberate re-baseline records an improvement the product owner judged".

## Out of scope

16-bit-native LED drivers (HD108, UCS7604): enabled by the per-driver quantization, each its own increment starting with the datasheet; a polar modifier on the LUT (modifier-first path, its own increment); the second-core field producer; 3D variants of the velocity rules and the LUT (the dimension audit's "waits for a consumer" rule); a MoonLive per-pixel inline-op program beyond the LUT loads (the roadmap's § 4c work); GPU acceleration on desktop; supersync's protocol (each stateful kernel declares its resync point, no more).
