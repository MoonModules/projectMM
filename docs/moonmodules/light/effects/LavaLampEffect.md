# Lava Lamp 2D Effect

Three slow blobs whose summed field is mapped through a black → red → orange → yellow → white palette. Atmospheric, fluid look — like a real lava lamp rather than the bright HSV of `MetaballsEffect`.

## Controls

- `enabled` (bool) — from `EffectBase`
- `bpm` (uint8_t, default 8, range 1-64) — orbit speed in beats per minute
- `radius` (uint8_t, default 36, range 8-80) — blob influence radius
- `intensity` (uint8_t, default 200, range 64-255) — how strongly the field maps into the palette

## Rendering

Three integer-orbited blobs (`sin8` for x/y at independent phases and speeds). Per pixel: `field += (radius² × 64) / (dx² + dy² + 1)` summed over the blobs, then `palette_[(field × intensity) >> 8]`. Palette is a 256-entry constexpr table in flash (768 bytes). No heap allocations.

## Tests

[Module test](../../../testing.md#stateless-effects) — non-zero output, spatial variation.
