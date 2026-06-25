# Plan — Multi-layer composition (blend N layers into the consumer buffer)

> Approved feature plan (PO reference, per CLAUDE.md *Plan before implementing*). The filename gets a `(shipped)` / `(attempted, abandoned)` marker when its outcome is known.

## Context

The product owner wants two things: **multiple effects per layer** and **multiple layers blended into the consumer buffer**. Investigation shows:

- **Multiple effects per layer is ALREADY DONE.** `Layer::loop()` ([src/light/layers/Layer.h:137-142](../../src/light/layers/Layer.h)) iterates *all* enabled effect children in order and calls each `eff->loop()`; they write the same layer buffer sequentially, each overwriting/adding where it writes — exactly the requested "next effect overwrites the previous where it writes." No work needed; verify with a scenario and move on.
- **Multi-layer composition is the real work** — the 🚧 designed-not-implemented item (architecture.md §345) + backlog item (backlog-core.md:208). `Layers` holds N layers but `Layers::activeLayer()` hands `Drivers` only the **first enabled** layer; the rest render their buffers but never reach output.

The groundwork is already in place, which makes this small:
- **`Drivers` already owns `outputBuffer_`** and already calls `blendMap(layer->buffer(), outputBuffer_, lut, cpl)` in `loop()` ([Drivers.h:186-193](../../src/light/drivers/Drivers.h)) — for one layer today.
- **`blendMap` already has the additive-with-clamp path** for overlapping sources, built *for* this ([BlendMap.h:16](../../src/light/layers/BlendMap.h), documented in [BlendMap.md](../../docs/moonmodules/light/BlendMap.md) §5).
- **Each Layer already owns its own buffer + LUT** and renders independently.

So composition = loop over enabled layers instead of one, blending each into the same `outputBuffer_` per its mode/opacity.

## Decisions (with PO)

- **Compositing site: Drivers.** Each Layer keeps its own buffer; `Drivers` composites them in order into `outputBuffer_`, then map+correct per child driver as today. (Matches architecture.md §345.)
- **Blend value on Layer, blend logic in Drivers.** `blendMode` + `opacity` are per-Layer **controls** (inert parameters that travel with the layer through add/delete/reorder — no separate sync'd list). Drivers reads each layer's settings + the **Layers container's child order** and blends predecessors→successors. Precedent: `Correction` (per-X state, Drivers applies it). The PO's insight — a Layer can't know its stack position — is honoured: order and orchestration live in Drivers; only the *parameter* lives on the Layer.
- **Blend modes first: Alpha (opacity) + Additive.** The two architecture.md §345 names. Additive = sum-with-clamp (the existing `blendMap` path); Alpha = opacity-weighted over. More modes (multiply/screen) later.
- **Order = Layers container child order** (already drag-reorderable, like effects/modifiers). The bottom (first-composited) layer's blendMode is moot — nothing under it; it just fills the buffer.

## Approach

### 1. Per-Layer blend controls (`src/light/layers/Layer.h`)
- Add `blendMode` (Select: `alpha` / `additive`) + `opacity` (uint8 0–255, default 255) controls in `onBuildControls()`. Inert — Layer doesn't act on them; Drivers reads them.
- Expose accessors `blendMode()` / `opacity()` for Drivers to read.

### 2. `Layers` exposes all enabled layers in order (`src/light/layers/Layers.h`)
- Keep `activeLayer()` (first enabled) for the degenerate/back-compat path, but add an ordered walk — e.g. `forEachEnabledLayer(cb)` or `enabledLayers()` — so Drivers can iterate the stack in child order. Don't build a parallel list; iterate the container's children, role-filtered to `Layer`, `enabled()` only (same filter `activeLayer()` already uses).

### 3. Drivers composites all layers (`src/light/drivers/Drivers.h`)
- `onBuildState()`: size `outputBuffer_` from the composition extent (the max physical extent across enabled layers — today it's the single layer's `physicalLightCount()`; with N layers it's the max, since they composite into one physical space). Keep the degrade-on-alloc-fail path.
- `loop()`: replace the single `blendMap(layer_->buffer(), outputBuffer_, …)` with an ordered pass:
  - **First enabled layer**: clears + writes `outputBuffer_` (the existing overwrite/clear behaviour — `blendMap` already clears dst first).
  - **Each subsequent enabled layer**: blends into `outputBuffer_` per its `blendMode` + `opacity` — `additive` uses the existing clamp path; `alpha` is `out = src*α + out*(1-α)`.
  - This needs `blendMap` to take a **blend mode + opacity** (today it picks overwrite-vs-additive purely from the LUT's `overwrites_`). Extend its signature: `blendMap(src, dst, lut, cpl, BlendOp op, uint8_t opacity, bool first)` — `first` selects clear-then-write; `op`/`opacity` select the per-pixel combine. Keep the fast overwrite path for `first && opacity==255`.
- `passBufferToDrivers()`: unchanged in spirit — children still read `outputBuffer_` (the composed result). The single-layer identity fast path (no LUT → read `layer_->buffer()` directly) only applies when there's exactly one enabled layer with no LUT; with ≥2 layers there's always a composite, so `outputBuffer_` is the source. Preserve the 1-layer-no-LUT zero-copy fast path as a special case.

### 4. `blendMap` gains a mode + opacity (`src/light/layers/BlendMap.h`)
- Add a small `BlendOp { Overwrite, Alpha, Additive }` enum + opacity param. Keep the existing fast overwrite-copy path (first layer, full opacity, single-write LUT). Bounds-checks stay. The additive-clamp path already exists; add the alpha path (integer math: `(src*α + dst*(255-α) + 127) / 255` per channel, clamped — textbook 8-bit alpha-over).

## Files
- `src/light/layers/Layer.h` — blendMode + opacity controls + accessors
- `src/light/layers/Layers.h` — ordered enabled-layer walk
- `src/light/drivers/Drivers.h` — composite loop over enabled layers into outputBuffer_; size from max extent
- `src/light/layers/BlendMap.h` — BlendOp + opacity param; alpha path
- `docs/moonmodules/light/Layer.md` / `Layers.md` / `Drivers.md` / `BlendMap.md` — document the controls + composition (move the 🚧 in architecture.md §345 to present-tense; remove/trim the backlog item)
- `docs/architecture.md` — §345 multi-layer composition: 🚧 → present tense once it ships
- `test/scenarios/light/scenario_*` — new composition scenario(s); a unit test for the alpha/additive blend math

## Hot-path notes
- Integer alpha math only (no float per-light) — `(src*α + dst*(255-α) + 127)/255`, clamped; the project's per-light-integer rule.
- N layers = N `blendMap` passes over the physical buffer per tick — cost scales with enabled-layer count × physical lights. Single-layer path keeps today's cost (one pass, or zero-copy when no LUT). Capture the multi-layer tick in `performance.md`.
- No per-tick allocation: `outputBuffer_` is allocated in `onBuildState`, reused each frame. PSRAM-first via the existing alloc.
- Robustness: add/delete/reorder/disable any layer in any order, 0×0×0, all enabled / none enabled — the compositor degrades (none enabled → cleared/black output, never a crash; the existing null-tolerance + degrade-on-alloc-fail patterns extend to the loop).

## Verification
- Unit: blend math (alpha + additive, clamp, opacity endpoints 0/255).
- Scenario: two layers, one additive one alpha, assert composited buffer; reorder layers → output changes; disable top layer → only bottom shows; multi-effect-per-layer (verify the already-working behaviour while here).
- Build (`-Werror`), ctest, scenarios, ESP32 build, KPI (record the multi-layer tick).
- Bench: two layers on the S3, blend modes + opacity live-adjusted, no reboot.

## Staging (start small, grow)
1. **Additive-only, opacity 255**, 2 layers → proves the composite loop end-to-end (smallest beautiful increment; uses the existing blendMap additive path almost unchanged).
2. **Opacity** (alpha-over) — the per-pixel alpha path + the opacity control.
3. **blendMode control + UI** — select between alpha/additive per layer.
4. **Docs present-tense + backlog removal**; performance.md multi-layer numbers.
5. (later) more blend modes (multiply/screen) when wanted.

## Out of scope
- Per-layer coordinate offset (separate backlog item — layers still share the coordinate box today).
- More blend modes beyond alpha/additive (later).
- Multi-layer UI beyond the per-layer controls (the "tab/accordion to switch layers" UI note in backlog-core.md:347 stays backlog).
