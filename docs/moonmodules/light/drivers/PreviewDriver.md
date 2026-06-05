# Preview Driver

![PreviewDriver controls](../../../assets/screenshots/PreviewDriver.png)

Streams a true-shape 3D preview to the web UI over WebSocket. The preview is a **point list** — only the real lights, at their real positions — not a dense grid. So a sphere, ring, or arbitrary fixture map shows in its true shape, and the per-frame data is just the lights that exist (much less than a padded bounding box).

## Controls

- `fps` (uint8_t, default 24, range 1-60) — preview stream rate (independent of the render loop)

## Protocol

PreviewDriver owns both wire formats end to end and pushes the bytes to a `BinaryBroadcaster` (the core [HttpServerModule](../../core/HttpServerModule.md) implements it via `broadcastBinary`). The HTTP server only writes the bytes to its WebSocket clients — it has no knowledge of the preview, the light domain, or the formats below. `main.cpp` wires the driver's broadcaster to the HTTP server instance. This mirrors MoonLight's model: positions sent once at mapping time, channels per frame.

Two binary message types (first byte selects):

- **`0x03` coordinate table** — sent on every LUT rebuild (layout add/replace/remove, resize, modifier change) and re-broadcast ~once per second so a newly-connected client catches up. Layout:

  `[0x03][count:u16][bx:u8][by:u8][bz:u8][stride:u16][ (x:u8, y:u8, z:u8) × count ]`

  `count` = points actually sent; `bx/by/bz` = bounding-box extent (the browser centres the cloud on it); positions are **1 byte per axis** (a layout's bounding box is ≤255/axis in practice; clamped on build). `stride` is the index-downsample factor (see Large layouts).

- **`0x02` per-frame channels** — RGB by driver-light index, in the same order as the coordinate table:

  `[0x02][count:u16][stride:u16][ (r, g, b) × count ]`

  The browser colours coordinate-table entry `i` with RGB triple `i`. It holds `0x02` frames until a `0x03` table has arrived.

## Sparse layouts & where the data comes from

The driver reads the **sparse driver buffer** — the `Layer`'s `MappingLUT` extracts the real lights from the dense render grid into a buffer of exactly `Layouts::totalLightCount()` entries (a radius-4 sphere → 210, not its 9×9×9 = 729 box). That same buffer is what ArtNet sends. PreviewDriver reads it flat by light index and builds the coordinate table from `Layouts::forEachCoord` (same driver order), so RGB index `i` and coordinate `i` always refer to the same light. See [Layer](../Layer.md) / [MappingLUT](../MappingLUT.md) for the box→driver mapping.

## Large layouts (index downsample)

A preview message is one non-blocking `writev`; it must fit lwIP's TCP send buffer (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT` = 11520 B), or the connection is dropped. Sparse layouts (sphere ≈ 634 B) send every light exactly (`stride` = 1). A large dense grid (128² = 16384 lights × 3 ≈ 48 KB) is **index-downsampled**: `stride` = smallest factor whose sent-point count (≤ 1800, ≈ 5.4 KB — well under half the send buffer, since the render task shares it and a payload near the ceiling would partial-write and drop the connection) fits the cap. Both `0x03` and `0x02` carry `stride`, and the browser plots every `stride`-th light **at its real position** — far better than the old dense-box block-replicate (which this replaces; there is no `decompress` / `detail` control anymore).

Positions are 1 byte per axis. A layout whose bounding box exceeds 255 on any axis (e.g. a 512-wide grid) is **scaled** so the largest box edge maps to 255, preserving aspect ratio (the `0x03` header carries the scaled box extents, which the browser normalises against). Boxes ≤255/axis — every sparse layout and any grid up to 255 — are sent at exact integer positions (scale factor 1). So large grids preview at their true proportions, not flattened onto the 255 plane.

## Tests

- [Unit tests: PreviewDriver](../../../tests/unit-tests.md#previewdriver) — coordinate table = real-light count (sphere → 210, not 729), per-frame RGB count matches the table, large layout strides down, small layout exact.
- [Scenario: scenario_Layer_base_pipeline](../../../tests/scenario-tests.md#scenario_layer_base_pipeline) — full pipeline including the preview driver.

## Prior art

### MoonLight — PhysicalLayer + WebSocket ([source](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Layers/PhysicalLayer.h))

The model this implements: virtual(logical grid) → physical(sparse lights) via a mapping table; light **positions sent once** at mapping time (`monitorPass`, `packCoord3DInto3Bytes` = 1 byte/axis, `isPositions` header state), **channels streamed per frame**. 3D WebGL renderer in the frontend.

### projectMM v1 — PreviewModule ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/drivers/PreviewModule.h))

Streamed via WebSocket binary frames. Control: `logEveryN` (slider 1-1000) for throttling.

### projectMM v2 — PreviewModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/PreviewModule.h))

Same pattern, uses v2 DataBuffer for frame data.
