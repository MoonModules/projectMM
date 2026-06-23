# Preview Driver

![PreviewDriver controls](../../../assets/screenshots/PreviewDriver.png)

Streams a true-shape 3D preview to the web UI over WebSocket. The preview is a **point list** — only the real lights, at their real positions — not a dense grid. So a sphere, ring, or arbitrary fixture map shows in its true shape, and the per-frame data is just the lights that exist (much less than a padded bounding box).

## Controls

- `fps` (uint8_t, default 24, range 1-60) — preview stream rate (independent of the render loop)

## Protocol

PreviewDriver owns both wire formats end to end and **streams** the bytes to a `BinaryBroadcaster` (the core [HttpServerModule](../../core/HttpServerModule.md)) via `beginBinaryFrame`/`pushBinaryFrame`/`endBinaryFrame` — it never builds a copy of a frame, pushing straight from the producer buffer and the layout's coordinate iterator. The HTTP server only writes the bytes to its WebSocket clients — no knowledge of the preview, the light domain, or the formats below. `main.cpp` wires the driver's broadcaster to the HTTP server instance. This mirrors MoonLight's model: positions sent once at mapping time, channels per frame.

Two binary message types (first byte selects):

- **`0x03` coordinate table** — sent on every LUT rebuild (layout add/replace/remove, resize, modifier change), when a new client connects (a generation bump), and when the adaptive downscale factor changes; re-sent on the next tick if a send is dropped under backpressure. Layout:

  `[0x03][count:u32][bx:u8][by:u8][bz:u8][stride:u16][ (x:u8, y:u8, z:u8) × count ]`  (10-byte header)

  `count` = points actually sent (**u32** — a HUB75 wall can exceed 65535 lights; matches `nrOfLightsType`); `bx/by/bz` = bounding-box extent (the browser centres the cloud on it); positions are **1 byte per axis** (a layout's bounding box is ≤255/axis in practice; scaled on build if larger). `stride` carries the **downscale factor** (1 = full resolution; >1 = the per-axis lattice step — see Large layouts), which the browser shows as `preview 1/N · link limited`.

- **`0x02` per-frame channels** — RGB, one triple per sent point, in coordinate-table order:

  `[0x02][count:u32][stride:u16][ (r, g, b) × count ]`  (7-byte header)

  The browser colours coordinate-table entry `i` with RGB triple `i`. It **skips a `0x02` frame whose `count` ≠ the current `0x03` count** (a rebuild is mid-flight — the colours would map to the wrong positions); they realign within ~1 frame. The device likewise withholds colour frames until the matching `0x03` has been accepted by the transport, so the two never desync.

## Sparse layouts & where the data comes from

The driver reads the **sparse driver buffer** — the `Layer`'s `MappingLUT` extracts the real lights from the dense render grid into a buffer of exactly `Layouts::totalLightCount()` entries (a radius-4 sphere → 210, not its 9×9×9 = 729 box). That same buffer is what ArtNet sends. PreviewDriver reads it flat by light index and builds the coordinate table from `Layouts::forEachCoord` (same driver order), so RGB index `i` and coordinate `i` always refer to the same light. See [Layer](../Layer.md) / [MappingLUT](../MappingLUT.md) for the box→driver mapping.

**No preview-side buffers.** Both messages STREAM — neither holds a copy of a frame:

- **Colour frame (`0x02`)** at full resolution (`stride`=1) is the **producer buffer streamed 1:1**: if it's 3-channel RGB (`cpl`=3, the logical buffer's native layout) the buffer bytes ARE the payload, pushed straight through `beginBinaryFrame`/`pushBinaryFrame`. A downsampled frame walks `forEachCoord` applying the same lattice skip the coordinate table used (same subset, same order — so colour `k` lines up with coord `k` with no stored index map), pushing 3 bytes per kept light from the buffer. A non-RGB source (`cpl`≠3) pushes its 3 colour bytes per light. Either way: no `rgb_`/gather buffer.
- **Coordinate table (`0x03`)** streams the kept lights' scaled positions from `forEachCoord` — no `coords_` buffer. Sent only on a geometry change / new client / downscale change (rare).

The send is synchronous on the preview's rate-limited loop (not the LED render tick): a large frame on a slow link briefly occupies that loop — a resumable cross-tick send (push what fits, resume next loop) is the follow-up.

## Large layouts (spatial downsample + adaptive)

The point count is bounded two ways:

- **Static cap** — `MAX_PREVIEW_POINTS` is RAM-derived: `131072` on PSRAM boards, `16384` on no-PSRAM. Above the cap the driver downsamples on a **spatial lattice** — keep a light only when its grid position lands on a per-axis step (`x%s==0 && y%s==0 && z%s==0`), a regular sub-grid that generalises to 2D and 3D, with no diagonal moiré (the lattice samples *positions*, not flat indices). Sparse layouts (a sphere shell) and any grid under the cap send every light (`stride` = 1, exact).
- **Adaptive downscale** — when a streamed frame doesn't reach every client (`endBinaryFrame()` false — the link couldn't take it), the driver coarsens the lattice (`stride`++) after a short run so frames shrink; a sustained run of fully-sent frames refines back toward full resolution (hysteresis stops oscillation). The factor rides the `0x03` `stride` field to the browser's status line.

Positions are 1 byte per axis. A layout whose bounding box exceeds 255 on any axis (e.g. a 512-wide grid) is **scaled** so the largest box edge maps to 255, preserving aspect ratio (the `0x03` header carries the scaled box extents, which the browser normalises against). Boxes ≤255/axis are sent at exact integer positions (scale factor 1), so large grids preview at their true proportions, not flattened onto the 255 plane.

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

## Source

[PreviewDriver.h](../../../../src/light/drivers/PreviewDriver.h)
