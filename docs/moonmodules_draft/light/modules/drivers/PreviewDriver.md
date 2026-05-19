# Preview Driver

Streams light data to the web UI via WebSocket for real-time 3D visualization.

## Controls

- `fps` (slider, default 30, range 1-60) — preview frame rate (independent of render fps)

## Protocol

Sends binary WebSocket frames: `[0x02][w_lo][w_hi][h_lo][h_hi][d_lo][d_hi][R G B ...]`

The UI renders this as a 3D point cloud using WebGL with orbit camera controls.

## Design notes

- FPS should be independent of the render loop — preview can run at lower fps to save bandwidth
- Binary frame format is compact: 7 bytes header + 3 bytes per light
- For 128x128 = 16384 lights: ~49KB per frame. At 30fps = ~1.5MB/s. May need to downsample for large grids.

## Prior art

### MoonLight — PhysicalLayer compositeTo + WebSocket
Light preview sent via WebSocket binary frames from PhysicalLayer. 3D WebGL renderer in frontend.

### projectMM v1 — PreviewModule ([source](https://github.com/ewowi/projectMM/blob/54b50bc/src/modules/drivers/PreviewModule.h))
v1 PreviewModule (commit 54b50bc). Control: `logEveryN` (slider 1-1000) for throttling. Streamed via WebSocket binary frames.
WebSocket binary preview. Control: logEveryN for throttling.

### projectMM v2 — PreviewModule ([source](https://github.com/ewowi/projectMM-v2/blob/main/src/modules/lights/PreviewModule.h))
Same pattern, uses v2 DataBuffer for frame data.
