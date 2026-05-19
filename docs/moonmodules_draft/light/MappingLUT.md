# MappingLUT

Lookup table mapping logical light indices to physical light indices.

## Mapping Types

- **1:0** — logical light is unmapped (skipped). `destinationCount() == 0`.
- **1:1** — logical light maps to one physical position.
- **1:N** — logical light maps to N physical positions (mirroring).

## v3 prototype approach (CSR)

- `offsets_[]` — uint32_t, one per logical light + 1 sentinel
- `destinations_[]` — uint16_t, flat array of physical indices
- CSR format is cache-friendly for sequential reads on hot path.
- 1:N mapping works for mirror/kaleidoscope.

## Design decision for v3

Consider MoonLight's PhysMap approach (type-in-entry union) for minimal memory. But use a flat CSR-style secondary array instead of nested vectors for 1:N entries. Best of both: minimal per-entry size + cache-friendly 1:N lookup.

## What needs improvement

- `setMapping` must be called in order. No random-access building.
- `totalDestinations` must be known upfront (overestimation wastes memory).
- No validation that destinations are within physical buffer bounds.
- Should support MoonLight's `oneToOneMapping` fast path flag.

## Prior art

### MoonLight — PhysMap ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/PhysMap.h))
**Memory-optimal approach.** Each entry is a union packed into 2 bytes (no-PSRAM) or 4 bytes (PSRAM):
- No-PSRAM (2 bytes): mapType in 2 bits + 14-bit payload (physical index OR condensed 554 RGB OR secondary index)
- PSRAM (4 bytes): mapType in 8 bits + 24-bit payload (max 16M lights) + 3-byte RGB cache

The map type is stored IN each entry, not in a separate array. This is more memory-efficient than v3's CSR approach which uses a separate offsets array (4 bytes per entry).

Fast paths: `oneToOneMapping` flag skips the table entirely (identity mapping). `allOneLight` flag enables a direct-table path bypassing the switch/case when no 1:N entries exist.

Secondary lookup for 1:N: `std::vector<std::vector<nrOfLights_t>>` (per-entry variable length). v3's CSR flat array is better for cache locality.

### projectMM v1 — GridLayout.requestMappings ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/layouts/GridLayout.h))
Simple flat array: `mappings[logical_index] = physical_strip_index`. Only 1:1 mapping. Rebuilt on control change.
