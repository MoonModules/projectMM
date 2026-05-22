# Preview Driver

Streams light data to the web UI via WebSocket for real-time 3D visualization.

## Controls

- `fps` (uint8_t, default 12, range 1-60) — preview frame rate (independent of render loop)
- `detail` (uint8_t, default 2, range 1-3) — downsample detail. Sets the voxel budget for the strided copy: 1 = coarse (256 voxels), 2 = medium (1024), 3 = fine (1849). Higher = a denser point cloud and a larger WebSocket payload, capped so even `detail = 3` (~5.5 KB) fits lwIP's TCP send buffer.
- `decompress` (bool, default false) — UI render hint. When on, the browser reconstructs the downsampled frame back to the original physical grid resolution by block-replicating each received voxel across its original cells, so the preview shows the same voxel count as the real layout. Purely client-side — the wire payload is the downsampled frame either way.

## Protocol

Binary WebSocket frames: `[0x02][dw16][dh16][dd16][ow16][oh16][od16][R G B ...]`

- 13-byte header: opcode, then the downsampled `dw/dh/dd` and the original-grid `ow/oh/od`, each a little-endian uint16
- RGB data: 3 bytes per voxel of the downsampled grid, row-major order
- The UI renders this as a 3D point cloud using WebGL with orbit camera controls. With `decompress` off it draws the `dw×dh×dd` cloud directly; with `decompress` on it block-replicates to the `ow×oh×od` grid.

## Downsampling

The preview frame is sent over WebSocket in a single non-blocking write, so it must fit lwIP's TCP send buffer (a backpressured browser must never stall the render task — see [HttpServerModule](../../core/HttpServerModule.md)). PreviewDriver therefore copies every Nth voxel (per axis) into a small owned buffer. The voxel budget is set by the `detail` control (256 / 1024 / 1849 for levels 1 / 2 / 3). The stride N is adaptive — the smallest stride whose downsampled voxel count fits the budget, recomputed each frame so it tracks runtime grid resizes. The frame's `width`/`height`/`depth` describe the downsampled grid; the browser derives positions from those, so no UI change is needed — it simply renders a coarser or finer point cloud. The owned buffer is sized once to the largest budget (`detail = 3`, ~5.5 KB) in `onAllocateMemory()` — PreviewDriver's only allocation.

The strided copy is fully 3D and channel-agnostic: it copies the first 3 (RGB) channels of each sampled light regardless of `channelsPerLight` (RGBW, RGBCCT, multi-channel DMX all work). The light index is derived from the bounding-box `(x, y, z)` but bounded by the real light count, so a **sparse layout** (wheel, sphere, arbitrary 3D shape — fewer lights than its bounding box) cannot read past the buffer; out-of-range cells render as black. Showing such non-grid layouts in their true shape needs the planned one-time coordinate message (below) — until then they preview as their dense bounding box.

## Non-grid layouts

For grid layouts, the browser derives 3D positions from the index (`x = i % w`). For non-grid layouts (wheel, ring, sphere, arbitrary point clouds), positions can't be derived — the browser needs actual coordinates.

Solution: **one-time coordinate message**. When the layout changes, send a separate WebSocket message with the coordinate table per light. The browser caches it. Binary pixel frames continue to stream only RGB data — coordinates sent once, then only pixel data streams.

Frame types:
- `0x02` — pixel data (every frame)
- `0x03` — coordinate table (on layout change only): `[0x03][count_lo][count_hi][x16 y16 z16 ...]`

## Tests

- [Preview Driver unit test](../../../testing.md#preview-driver) — `detail` strides, original-dimension reporting, send-buffer budget, channel-agnostic copy.
- [Scenario: preview-detail](../../../testing.md#scenario-preview-detail) — toggles `detail`/`decompress` on a live device, asserts no render-FPS regression.
- [Scenario: base-pipeline](../../../testing.md#scenario-pipeline) — full pipeline including preview driver.

## Prior art

### MoonLight — PhysicalLayer + WebSocket

Light preview sent via WebSocket binary frames directly from PhysicalLayer's display buffer. 3D WebGL renderer in frontend. No intermediate copy.

### projectMM v1 — PreviewModule ([source](https://github.com/ewowi/projectMM-v1/blob/54b50bc/src/modules/drivers/PreviewModule.h))

Streamed via WebSocket binary frames. Control: `logEveryN` (slider 1-1000) for throttling.

### projectMM v2 — PreviewModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/PreviewModule.h))

Same pattern, uses v2 DataBuffer for frame data.
