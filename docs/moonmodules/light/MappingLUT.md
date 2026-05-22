# MappingLUT

Lookup table mapping logical light indices to physical light indices.

## Mapping types

Four mapping types describe how logical lights relate to physical lights:

| Type | Meaning | LUT needed | Example |
|------|---------|-----------|---------|
| **1:1 identical** | logical index == physical index | No | Grid, no serpentine, no modifiers |
| **1:1 shuffled** | each logical → one physical, reordered | Yes | Grid with serpentine |
| **1:0 unmapped** | logical has no physical output | Yes | Sparse layouts (wheel) |
| **1:N multimap** | logical → multiple physical | Yes | Mirror/clone modifier |

## API

The code API answers one question: **does this LUT have a mapping table?**

- **`hasLUT()`** — returns true if a mapping table is allocated. Covers 1:1 shuffled, 1:0, and 1:N.
- **`setIdentity(count)`** — sets identity mode (1:1 identical). No table allocated, `hasLUT()` returns false. `forEachDestination(i, cb)` calls `cb(i)` — logical index IS the physical index.
- **`build(logicalCount, maxDest)`** — allocates CSR arrays for non-identity mapping. `hasLUT()` returns true.

Callers don't need to know which mapping type is used — they only need to know whether a table exists. DriverGroup checks `hasLUT()` to decide whether to allocate an output buffer. BlendMap checks `hasLUT()` to choose between memcpy (identity) and LUT-based mapping.

Naming: `setIdentity()` / `hasLUT()` are used rather than a "one-to-one" flag because "one-to-one" is ambiguous — it reads as covering all 1:1 mappings, but the table-free fast path applies only to the *sequential identity* case (logical index == physical index).

## Storage

Uses `nrOfLightsType` typedef (see architecture-light.md): `uint16_t` on no-PSRAM, `uint32_t` on PSRAM.

CSR (Compressed Sparse Row) format: two arrays — `offsets[logicalCount + 1]` stores where each entry's destinations start, `destinations[]` stores the flat list of physical indices. For entry `i`, destinations are `destinations[offsets[i] .. offsets[i+1])`.

Memory: `estimateBytes(logicalCount, maxDest)` returns the total allocation size. `memoryUsed()` returns actual bytes allocated (0 for identity).

## Size information

`totalDestinations` (total physical lights) is provided by the LayoutGroup. Used by both layers and driver groups to allocate their buffers. Destinations are therefore always within valid bounds.

## Prior art

### MoonLight — PhysMap ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/PhysMap.h))
Memory-optimal union. 2 bytes (no-PSRAM) or 4 bytes (PSRAM). Map type stored IN each entry. `oneToOneMapping` and `allOneLight` fast path flags. `forEachLightIndex()` for 1:N iteration. v3 renamed `oneToOneMapping` → `setIdentity()` / `!hasLUT()` because "one-to-one" reads as covering all 1:1 mappings, but the table-free fast path applies only to the sequential identity case.

### projectMM v1 — GridLayout.requestMappings ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/layouts/GridLayout.h))
Simple flat array: `mappings[logical_index] = physical_strip_index`. Only 1:1. Rebuilt on control change.
