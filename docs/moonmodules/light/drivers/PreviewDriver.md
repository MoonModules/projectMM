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

The driver reads the **sparse driver buffer** — the `Layer`'s `MappingLUT` extracts the real lights from the dense render grid into a buffer of exactly `Layouts::totalLightCount()` entries (a radius-4 sphere → 210, not its 9×9×9 = 729 box). That same buffer is what ArtNet sends. PreviewDriver reads it flat by light index, and the coordinate table is built in the same driver order (closed-form for a dense grid, via `Layouts::forEachCoord` for a sparse one — see *How the kept lights are chosen*), so RGB index `i` and coordinate `i` always refer to the same light. See [Layer](../Layer.md) / [MappingLUT](../MappingLUT.md) for the box→driver mapping.

**No preview-side buffers.** Both messages stream straight from the driver buffer — neither holds a copy of a frame:

- **Colour frame (`0x02`)** at full resolution (`stride`=1, `cpl`=3) is the **driver buffer streamed 1:1** through the resumable `sendBufferedFrame` (header copied, body = the buffer pointer). For a dense identity grid that buffer is the Layer's box buffer; for a sparse/mapped layout (a sphere, a serpentine grid) it's the LUT-mapped output buffer — only the real lights, in driver order, the **same buffer the LED drivers consume**, not the dense box. So the colour count always equals the coordinate-table count. A downsampled (`stride`>1) or non-RGB (`cpl`≠3) frame packs only the kept lights, in the same subset + order the coordinate table used (colour `k` ↔ coord `k`, no stored index map), through the synchronous `begin`/`push`/`end`. Either way: no `rgb_`/gather buffer.
- **Coordinate table (`0x03`)** streams the kept lights' scaled positions — no `coords_` buffer. Sent only on a geometry change / new client / downscale change (rare).

**How the kept lights are chosen (closed-form vs walk).** A **dense grid in natural box order** (no mapping LUT) is a regular box, so the kept set, the count, each position, and each light's buffer index are all **closed-form from the box dimensions and the stride** — the driver strides the box directly (`for z in 0,s,2s…; y; x`, light `(x,y,z)` at buffer index `z·H·W + y·W + x`), touching only the kept lights. No per-frame `forEachCoord` walk over skipped cells. A **sparse / serpentine / modified** layout has a LUT (arbitrary index↔position map), so those three paths walk `Layouts::forEachCoord` applying the lattice predicate. In practice the sparse case stays under the point cap and sends at `stride`=1 (the 1:1 buffered path, no walk at all), so the per-frame colour walk is effectively never on the hot path.

**Resumable send + adaptive frame rate (no stall, no buffer).** The full-res colour frame rides [HttpServerModule](../../core/HttpServerModule.md)'s `sendBufferedFrame`, drained a chunk per `loop20ms` from the stable driver buffer — so a large frame (128² = ~49 KB, 196² = ~115 KB) never spins the preview loop. The driver starts a new frame only when `bufferedSendIdle()` (the previous one fully drained), so the **effective frame rate self-limits to what the link sustains**: a fast link hits the `fps` ceiling, a slow link drops to a few fps — and the browser's status line shows the measured rate. A geometry rebuild frees+reallocs the driver buffer, so `onBuildState()` calls `cancelBufferedSend()` first (the browser discards the half-sent message and gets the fresh table + frame next tick) — a use-after-free guard, pinned by a test.

## Large layouts (spatial downsample + adaptive)

The preview never freezes and never tears at any grid size: it always delivers a **complete** frame — full-res, at a reduced frame rate, or spatially downsampled — and sheds in that order (rate first, then resolution). The point count is bounded two ways:

- **Point cap = min(display, memory).** `maxPreviewPoints()` takes the smaller of two bounds. (1) A **display cap** (~4096) — a preview is a browser canvas a few hundred px wide, so beyond a few thousand points the lights are sub-pixel and *more points only cost link bandwidth* (a 16K-point full-res frame streams at <1 fps even on Ethernet). Capping to a display-sensible count makes a big-RAM board downsample to a frame the **link** can push fast — the bottleneck at large grids is throughput, not memory. (2) A **memory cap** derived at runtime from `maxAllocBlock()` with a reserve margin (architecture.md *"sizes determined at runtime based on available memory"*) — it only bites on a board too tight to stream even the display cap, downscaling sooner. Above the resulting cap the driver downsamples on a **spatial lattice** — keep a light only where its grid position lands on a per-axis step `s` (`x,y,z ≡ 0 (mod s)`), which generalises to 2D/3D with no diagonal moiré because it samples *positions*, not flat indices. For a dense grid this is the closed-form `[::s]` stride above; for a sparse layout, the same predicate over `forEachCoord`. Any layout under the cap sends every light (`stride` = 1, exact).
- **Adaptive downscale** — the *deeper* fallback, after frame rate. The struggle signal is **latency**: a buffered frame still draining after a few `fps` slots (the link can't sustain even one frame at this resolution), or a frame/coord table that didn't reach a client. This fires even when frames *eventually* send (the slow-but-complete case a pure all-sent signal misses — e.g. a full-res 196² frame on ethernet that delivers at 2 fps). On sustained struggle the driver coarsens the lattice (`stride`++); a sustained run of prompt, fully-sent frames refines back (hysteresis stops oscillation). The factor rides the `0x03` `stride` field to the browser's status line.

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
