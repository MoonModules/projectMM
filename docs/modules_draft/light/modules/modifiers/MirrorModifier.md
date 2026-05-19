# Mirror Modifier

Static modifier. Mirrors the logical space around centre axes,
producing a kaleidoscope effect. Each enabled axis doubles the
physical output from the same logical data.

## Controls

- `mirrorX` (Bool, default true) — mirror around vertical centre
- `mirrorY` (Bool, default true) — mirror around horizontal centre
- `mirrorZ` (Bool, default true) — mirror around depth centre

## Effect on Pipeline

- **Logical dimensions reduced**: 128x128 with mirrorX+Y → 64x64
  logical buffer (25% of physical).
- **LUT produces 1:N mappings**: each logical light maps to 2/4/8
  physical positions depending on how many axes are mirrored.
- **Deduplication**: lights exactly on the centre axis don't get
  doubled (e.g. x=63 in 128-wide with mirrorX: 128-1-63=64 ≠ 63,
  not duplicated. But in 127-wide: 127-1-63=63 = duplicate, skipped).

## Key function: mapToPhysical

Given a logical coordinate (lx, ly, lz) and physical dimensions,
produces all physical positions this logical light maps to.
Uses nested loops over enabled axes, deduplicates, returns count.

## What worked

- Kaleidoscope-style mirror is the correct approach (not coordinate
  flip). Logical buffer is smaller, 1:N mapping fills all quadrants.
- Deduplication of centre-axis lights prevents double-brightness.
- `markDirty()` on onChange triggers LUT rebuild.
- Works with both Rainbow and Noise effects.

## What needs improvement

- `onChange` sets dirty, but the rebuild only happens when the main
  loop checks the dirty flag. If the HTTP handler triggers onChange,
  the actual rebuild is deferred to next frame. This is correct but
  not obvious.
- With Z mirror on depth=1, multiplier is 8 but actual mapping is 4
  (Z deduplicates). This overallocates the LUT destinations array.
  Harmless but wasteful.
