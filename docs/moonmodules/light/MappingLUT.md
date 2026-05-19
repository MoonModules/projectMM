# MappingLUT

Lookup table mapping logical light indices to physical light indices. Based on MoonLight's PhysMap design, tuned for v3.

## Mapping types

- **1:0** (`m_zeroLights`) — logical light is unmapped (skipped). In MoonLight, the mapping entry stores a cached color value for unmapped positions — a "hack" that avoids a separate buffer for unmapped lights.
- **1:1** (`m_oneLight`) — logical light maps to one physical position. Two sub-cases:
  - **1:1 unshuffled** — logical index equals physical index. Grid layout, no serpentine, X-then-Y order. This IS MoonLight's `oneToOneMapping` — the mapping table can be skipped entirely.
  - **1:1 shuffled** — logical maps to a different physical index. Grid with serpentine, or any non-identity mapping. This IS MoonLight's `allOneLight` — a direct table fast path (no per-entry type check needed).
- **1:N** (`m_moreLights`) — logical light maps to N physical positions (mirroring, cloning). Entry stores an index into a secondary flat array (CSR-style). In MoonLight, this uses `forEachLightIndex()` callback pattern.

## Storage (based on PhysMap)

Uses `nrOfLightsType` and `lengthType` typedefs (see architecture-light.md).

Each entry is a union, sized by platform:
- No-PSRAM: 2 bytes (mapType in 2 bits + 14-bit payload)
- PSRAM: 4 bytes (mapType in 8 bits + 24-bit payload)

Secondary lookup for 1:N: CSR flat array (offsets + destinations). Better cache locality than MoonLight's nested `std::vector<std::vector<>>`.

CSR (Compressed Sparse Row): two arrays — `offsets[logicalCount + 1]` stores where each entry's destinations start, `destinations[]` stores the flat list of physical indices. For entry `i`, destinations are `destinations[offsets[i] .. offsets[i+1])`.

## Size information

`totalDestinations` (total physical lights) is provided by the LayoutGroup. Used by both layers and driver groups to allocate their buffers. Destinations are therefore always within valid bounds.

## Prior art

### MoonLight — PhysMap ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/PhysMap.h))
Memory-optimal union. 2 bytes (no-PSRAM) or 4 bytes (PSRAM). Map type stored IN each entry. `oneToOneMapping` and `allOneLight` fast path flags. `forEachLightIndex()` for 1:N iteration.

### projectMM v1 — GridLayout.requestMappings ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/layouts/GridLayout.h))
Simple flat array: `mappings[logical_index] = physical_strip_index`. Only 1:1. Rebuilt on control change.
