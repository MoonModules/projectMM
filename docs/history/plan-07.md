# Plan: Adaptive Memory Allocation & Memory Scenario Testing

## Context

The system drives 128x128 (16384 LEDs) on ESP32 without PSRAM (~320KB internal RAM). This plan defines the adaptive memory allocation strategy and the scenario testing that guards it. This is the core architectural piece — every byte matters, and the system must degrade gracefully rather than fail when memory is insufficient.

**Why now:** Per-module timing is in place, scenario infrastructure works, but there's no memory prediction, no adaptive allocation, and no scenarios that verify memory behavior. Without this, adding features (more layers, modifiers, drivers) will silently break the 128x128 baseline on ESP32.

## Invariants (non-negotiable)

1. Effects ALWAYS write to their layer's logical buffer. Never to output, never to physical coordinates.
2. DriverGroup ALWAYS owns the output path (blending, mapping, brightness correction, channel reordering).
3. Layer buffer is mandatory — if it doesn't fit, reduce dimensions until it does ("at least see something").
4. No heap allocations in the hot path (loop). All structural allocations during setup/onAllocateMemory.

## Allocation Rules

**Mapping LUT**: Created only if ALL of these are true:
- Modifiers exist on the layer
- Layout is not a simple non-serpentine grid (where physical == logical, making the modifier mapping trivially 1:1)
- Enough heap available (after reserving HEAP_RESERVE for stack/HTTP/overhead)

**Driver output buffer**: Created only if:
- At least one layer has a mapping LUT actually allocated (not just "has modifiers" — the LUT must exist)
- Enough heap available

**Result**: For 1:1 unshuffled (no modifiers, or grid-without-serpentine), zero intermediate buffers. ArtNet reads directly from layer buffer. Maximum LED count.

## Degradation Cascade

When memory is insufficient, degrade in this order:
1. **Full pipeline** — LUT + driver output buffer (modifier applied, clean separation)
2. **Skip driver output buffer** — LUT exists, but DriverGroup does mapping inline (slower, sequential)
3. **Skip LUT** — modifier not applied, forced 1:1 mapping
4. **Reduce layer dimensions** — halve until buffer fits, minimum 8x8

Each degradation is observable via flags on the module (`degraded()`, `lutSkipped()`, `outputBufferSkipped()`).

## Phases

### Phase 1: Memory Reporting

Add per-module memory tracking so we can measure before we optimize.

**MoonModule base** (`src/core/MoonModule.h`):
- Add `virtual size_t classSize() const { return sizeof(MoonModule); }`
- Add `size_t dynamicBytes_ = 0` + accessor/setter — set during onAllocateMemory

**Each MoonModule subclass** (one-liner each):
- Override `classSize()` → `return sizeof(ThisClass);`
- In `onAllocateMemory()`: set `dynamicBytes_` to actual heap used

**MappingLUT** (`src/light/MappingLUT.h`):
- Add `size_t memoryUsed() const` — returns bytes allocated (offsets + destinations), 0 for oneToOne
- Add `static size_t estimateBytes(logicalCount, maxDest)` — pre-flight estimation

**Buffer** (`src/light/Buffer.h`):
- `bytes()` already exists — sufficient

**HttpServerModule** (`src/core/HttpServerModule.h`):
- Extend `writeModuleTimingJson()` to include `classSize` and `dynamicBytes` per module
- `/api/system` response grows: `{"name":"Layer","us":65,"classSize":280,"heap":49152}`

**Console output** (`src/main.cpp`):
- Boot line: `sizeof: MoonModule=88 Layer=280 DriverGroup=120 ...`
- Per-module timing includes heap: `Layer:65us/49KB`

### Phase 2: Adaptive Allocation

The core algorithm. Layer and DriverGroup check available heap before allocating.

**Constants** (`src/core/types.h`):
- `constexpr size_t HEAP_RESERVE = 32768;` — minimum free heap to preserve for stack/HTTP/WiFi

**Layer** (`src/light/Layer.h`):
- In `rebuildLUT()`: before `lut_.build()`, estimate bytes via `MappingLUT::estimateBytes()` and check `min(freeHeap() - HEAP_RESERVE, maxAllocBlock()) >= needed`
- If insufficient: `lut_.setOneToOne(physicalCount)`, set `lutSkipped_ = true`, log warning
- For buffer: if `buffer_.allocate()` fails, halve dimensions in a loop until fit or 8x8 minimum
- Add `bool lutSkipped() const` and `bool degraded() const` accessors

**DriverGroup** (`src/light/DriverGroup.h`):
- In `onAllocateMemory()`: only allocate `outputBuffer_` if `!layer_->lut().isOneToOne()` (already done) AND enough heap
- Add `bool outputBufferSkipped() const` flag
- If skipped: still do mapping but inline (iterate LUT, write directly... or fall back to 1:1 if LUT was also skipped)

**Grid layout** (`src/light/GridLayout.h`):
- Add `bool isSerpentine() const` (currently always false — straight grid)
- Layer uses this + modifier presence to decide if LUT is truly needed

**Desktop testing**: Add `platform::setSimulatedFreeHeap(size_t)` to desktop platform for testing degradation without real memory pressure.

### Phase 3: Memory Scenarios

Scenarios that verify memory behavior. Both in-process and live.

**New scenario step types** in `test/scenario_runner.cpp`:
- `"measure": true` already captures heap — extend with memory-specific bounds
- Add `"bounds": { "heap": { "min": N }, "maxBlock": { "min": N } }` support
- Add `"bounds": { "dynamicBytes": { "module": "Layer", "equals": 768 } }` for precise checks
- Report per-step: heapBefore → heapAfter → delta

**New scenarios:**

`test/scenarios/memory-boot.json` — Boot overhead:
- Add all modules (no grid yet)
- Measure: sizeof() values, dynamicBytes = 0, heap baseline

`test/scenarios/memory-1to1.json` — 1:1 unshuffled:
- Grid 16x16 + Layer + Effect + DriverGroup + ArtNet, no modifier
- Assert: LUT is oneToOne, no driver output buffer, Layer dynamicBytes = 768

`test/scenarios/memory-shuffled.json` — With modifier:
- Same + MirrorModifier
- Assert: LUT allocated, driver buffer allocated, report sizes

`test/scenarios/memory-scaling.json` — Find boundaries:
- Start 8x8, increase to 16x16, 32x32, 64x64, 128x128, 256x256
- Each step: measure heap, check bounds
- On ESP32: observe degradation cascade kicking in at some grid size

**Live runner** (`scripts/scenario/run_live_scenario.py`):
- Parse heap/maxBlock bounds from scenario JSON
- Report memory deltas per step

### Phase 4: Predict-Measure-Compare

Before each step, predict memory impact. After, compare.

**Prediction function** in `scenario_runner.cpp`:
- Given grid dimensions + channelsPerLight + modifiers → compute expected buffer sizes
- Layer buffer: `W × H × D × cpl`
- LUT: `MappingLUT::estimateBytes(logicalCount, maxDest)`
- Driver buffer: `physicalCount × cpl` (if LUT exists)
- Total predicted delta = sum of new allocations

**Scenario output**:
```
  PREDICT  Layer buffer: 49152, LUT: 0, driver buffer: 0 → total: 49152
  MEASURE  heap delta: 49168 (variance: +16 bytes, 0.03%)
  PASS     variance < 5%
```

**Variance threshold**: configurable, default 5%. Catches leaks (consistent positive variance) and accounting errors.

### Phase 5: Direct-to-Packet (deferred)

For 1:1 sequential with multiple layers: DriverGroup blends directly into ArtNet packets / LED DMA. Requires multi-layer support (DriverGroup knowing about multiple layers). Also includes brightness correction and channel reordering in the output chain.

**Defer until**: multi-layer support is implemented. Document the design now, implement later.

### Phase 6: Architecture & Spec Updates

Updated alongside each phase:

- `docs/architecture-light.md` — memory tiers, degradation cascade, invariants, allocation rules
- `docs/moonmodules/core/MoonModule.md` — classSize, dynamicBytes reporting
- `docs/moonmodules/light/Layer.md` — adaptive LUT allocation, degradation behavior
- `docs/moonmodules/light/MappingLUT.md` — estimateBytes, memory formulas
- `docs/moonmodules/light/drivers/` — direct-to-packet design (for Phase 5)
- `docs/testing.md` — memory scenario descriptions
- `docs/history/memory-budget.md` — updated with actual measured values

## Files Summary

```
src/core/MoonModule.h           # classSize(), dynamicBytes_
src/core/types.h                # HEAP_RESERVE constant
src/light/Layer.h               # adaptive LUT allocation, degradation
src/light/DriverGroup.h         # adaptive output buffer, degradation flags
src/light/MappingLUT.h          # memoryUsed(), estimateBytes()
src/light/GridLayout.h          # isSerpentine()
src/core/HttpServerModule.h     # memory fields in /api/system
src/main.cpp                    # sizeof boot log
src/platform/desktop/platform_desktop.cpp  # setSimulatedFreeHeap
test/scenario_runner.cpp        # memory bounds, predict-measure, per-step heap
test/scenarios/memory-boot.json
test/scenarios/memory-1to1.json
test/scenarios/memory-shuffled.json
test/scenarios/memory-scaling.json
scripts/scenario/run_live_scenario.py  # memory bounds support
docs/architecture-light.md      # memory tiers, invariants
docs/moonmodules/core/MoonModule.md
docs/moonmodules/light/Layer.md
docs/testing.md
```

## Implementation Order

**Do now**: Phases 1 + 2 + 3 + 6 (reporting → adaptive allocation → scenarios → docs)
**Next step**: Phase 4 (predict-measure-compare)
**Deferred**: Phase 5 (direct-to-packet, needs multi-layer)

## Verification

1. Desktop build + all existing tests pass (no regression)
2. `sizeof` values logged at boot
3. `/api/system` returns classSize + dynamicBytes per module
4. Memory scenarios pass: 1:1 has zero LUT/driver buffer, shuffled has both
5. On desktop with simulated low heap: degradation cascade triggers correctly
6. On ESP32: 128x128 still runs, memory-scaling scenario finds actual boundary
7. Platform boundary check passes
8. Architecture docs accurately describe the implemented behavior
