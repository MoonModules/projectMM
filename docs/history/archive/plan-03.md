# Plan: Noise Effect + Mirror Modifier + MappingLUT

## Context

Items 3+4 from plan.md. Add a second effect (Noise) and the first modifier (Mirror kaleidoscope) with the full MappingLUT. Proves effect variety, modifiers, 1:N mapping, and LUT rebuild.

## Implementation Steps

### Step 1: NoiseEffect

File: `src/light/NoiseEffect.h` (NEW), `test/test_noise.cpp` (NEW)

Same pattern as RainbowEffect. Controls: `scale` (uint8_t, 1-32, default 4), `speed` (uint8_t, 0-255, default 50). Hash-based value noise: `(x*1619 + y*31337 + t*6271)` with bilinear interpolation and smoothstep. Output: `hsvToRgb(noiseValue, 200, 255)`. All integer math.

Tests: non-zero output, spatial variation, different from rainbow.

Promote `docs/moonmodules_draft/light/effects/NoiseEffect.md` → `docs/moonmodules/light/effects/NoiseEffect.md`.

### Step 2: MappingLUT

File: `src/light/MappingLUT.h` (NEW), `test/test_mapping_lut.cpp` (NEW)

Simplified CSR format (skip union/bitpacking for now):
- `oneToOneMapping_` flag — skip LUT when logical == physical
- `offsets_[logicalCount + 1]` + `destinations_[]` flat arrays for 1:N
- `setOneToOne(count)`, `build(logicalCount, maxDest)`, `setMapping(idx, physicals, count)`, `finalize()`
- `forEachDestination(logicalIdx, callback)` — hot-path accessor
- Allocated via `platform::alloc`

Tests: default is oneToOne, build with known 1:N mappings, verify destinations, free/rebuild.

### Step 3: ModifierBase + MirrorModifier

Files: `src/light/ModifierBase.h` (NEW), `src/light/MirrorModifier.h` (NEW), `test/test_mirror.cpp` (NEW)

ModifierBase:
```cpp
virtual void logicalDimensions(physW, physH, physD, &logW, &logH, &logD) const = 0;
virtual void mapToPhysical(lx, ly, lz, physW, physH, physD,
                           nrOfLightsType* outPhysicals, nrOfLightsType& outCount,
                           nrOfLightsType maxOut) const = 0;
```

Output array pattern (not template callback) — max 8 entries on stack for XYZ mirror.

MirrorModifier:
- Controls: `mirrorX` (bool, true), `mirrorY` (bool, true), `mirrorZ` (bool, false)
- `logicalDimensions`: halves mirrored axes with ceiling division
- `mapToPhysical`: nested iteration over mirror combinations, deduplication for centre-axis lights
- Physical index: `pz * physW * physH + py * physW + px` (matches GridLayout row-major)

Tests: logical dimensions (even/odd), corner pixel → 4 positions, centre pixel dedup, no-mirror → 1 position.

Promote draft spec → `docs/moonmodules/light/modifiers/MirrorModifier.md`.

### Step 4: Layer — modifier support + rebuildLUT

File: `src/light/Layer.h` (MODIFY)

- Add `std::array<ModifierBase*, 4> modifiers_` + `addModifier()`
- Add `MappingLUT lut_` member
- Track logical vs physical dimensions separately
- `width()`/`height()`/`depth()` return logical (effects see logical space)
- Add `physicalLightCount()` accessor
- `rebuildLUT()`: if no modifiers → `lut_.setOneToOne()`, logical == physical. If modifier → compute logical dims, allocate CSR, iterate logical coords calling `mapToPhysical`, fill LUT.
- `onAllocateMemory()`: call `rebuildLUT()`, allocate buffer to logical size
- Propagate lifecycle to modifiers (same as effects)
- Expose `const MappingLUT& lut() const`

### Step 5: BlendMap

File: `src/light/BlendMap.h` (NEW), `test/test_blend_map.cpp` (NEW)

Free function: `void blendMap(const Buffer& src, Buffer& dst, const MappingLUT& lut, uint8_t channelsPerLight)`

- If oneToOne: memcpy (fast path, but DriverGroup skips blendMap entirely in this case)
- Otherwise: clear dst, iterate logical lights, for each destination write src channels with additive clamping

Tests: oneToOne copies, 1:N mapping produces duplicated pixels, additive clamping.

### Step 6: DriverGroup — output buffer

File: `src/light/DriverGroup.h` (MODIFY)

- Add `Buffer outputBuffer_`
- `onAllocateMemory()`: if `layer_->lut().isOneToOne()`, pass layer buffer to drivers (current behavior). Otherwise allocate outputBuffer_ to physical size, pass to drivers.
- `loop()`: if LUT active, call `blendMap()` before driver loops
- Add `physicalLightCount` from `layer_->physicalLightCount()`

### Step 7: Wire + scenarios

File: `src/main.cpp` (MODIFY)

Add MirrorModifier to the pipeline. Keep both Rainbow and Noise as effects (Noise runs after Rainbow, overwriting — proves second effect works).

File: `test/scenario_runner.cpp` (MODIFY) — add NoiseEffect, MirrorModifier to registry.

File: `test/scenarios/mirror.json` (NEW) — grid with mirror, verify pipeline works with LUT.

### Step 8: Documentation

- Promote NoiseEffect and MirrorModifier specs from draft
- Update docs/testing.md with new test sections
- Add test links to promoted specs

## Verification

1. `cmake --build build` — zero warnings
2. `ctest --output-on-failure` — all tests pass (existing + new)
3. `./build/test/mm_scenarios` — all scenarios pass including mirror
4. Platform boundary check passes
5. Desktop: rainbow+noise with mirror visible on ArtNet panel (kaleidoscope pattern)
6. ESP32: rebuild and flash — same pipeline with mirror works on device
