# MappingLUT

CSR (Compressed Sparse Row) style lookup table mapping logical pixel
indices to physical pixel indices.

## Mapping Types

- **1:0** — logical pixel is unmapped (skipped). `destinationCount() == 0`.
- **1:1** — logical pixel maps to one physical position.
- **1:N** — logical pixel maps to N physical positions (mirroring).

## API

- `allocate(logicalCount, totalDestinations)` — allocate storage
- `setMapping(logicalIndex, physicalIndices, count)` — must be called
  in order: 0, 1, 2, ...
- `finalize()` — call after all setMapping calls
- `destinationCount(logicalIndex)` — how many physical destinations
- `destinations(logicalIndex)` — pointer to physical indices

## Storage

- `offsets_[]` — uint32_t, one per logical pixel + 1 sentinel
- `destinations_[]` — uint16_t, flat array of physical indices

Destination indices are uint16_t (max 65535). Sufficient for most
installations. If >65K pixels needed, introduce a `PixelIndex` typedef.

## What worked

- CSR format is cache-friendly for sequential reads on hot path.
- 1:N mapping works correctly for mirror/kaleidoscope (tested with
  4-quadrant mirror producing 1:4 mappings).
- Deduplication of overlapping mirror positions (centre axis) works.

## What needs improvement

- `setMapping` must be called in order. No random-access building.
- `totalDestinations` must be known upfront (overestimation is safe
  but wastes memory). With mirror deduplication, the actual count is
  less than `logicalCount * multiplier`.
- No validation that destinations are within physical buffer bounds.
  Out-of-bounds destinations are silently caught by blendMap's bounds
  check but could hide bugs.
