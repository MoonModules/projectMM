# Preview Driver

Streams light data to the web UI via WebSocket for real-time 3D visualization. Zero-copy: reads directly from the physical output buffer, no allocation.

## Controls

- `fps` (uint8_t, default 20, range 1-60) — preview frame rate (independent of render loop)

## Protocol

Binary WebSocket frames: `[0x02][w_lo][w_hi][h_lo][h_hi][d_lo][d_hi][R G B ...]`

- 7-byte header: opcode + width/height/depth as little-endian uint16
- RGB data: 3 bytes per light, physical buffer order
- The UI renders this as a 3D point cloud using WebGL with orbit camera controls

Zero-copy design: PreviewDriver points at the existing physical output buffer (owned by DriverGroup or Layer). HttpServerModule sends the 7-byte header + buffer data directly via two WebSocket writes. No frame buffer allocation, no memcpy.

## Non-grid layouts

For grid layouts, the browser derives 3D positions from the index (`x = i % w`). For non-grid layouts (wheel, ring, sphere, arbitrary point clouds), positions can't be derived — the browser needs actual coordinates.

Solution: **one-time coordinate message**. When the layout changes, send a separate WebSocket message with the coordinate table per light. The browser caches it. Binary pixel frames continue to stream only RGB data — coordinates sent once, then only pixel data streams.

Frame types:
- `0x02` — pixel data (every frame)
- `0x03` — coordinate table (on layout change only): `[0x03][count_lo][count_hi][x16 y16 z16 ...]`

## Tests

[Scenario: base-pipeline](../../../testing.md#scenario-pipeline) — full pipeline including preview driver.

## Prior art

### MoonLight — PhysicalLayer + WebSocket
Light preview sent via WebSocket binary frames directly from PhysicalLayer's display buffer. 3D WebGL renderer in frontend. No intermediate copy.

### projectMM v1 — PreviewModule ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/drivers/PreviewModule.h))
Streamed via WebSocket binary frames. Control: `logEveryN` (slider 1-1000) for throttling.

### projectMM v2 — PreviewModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/PreviewModule.h))
Same pattern, uses v2 DataBuffer for frame data.
