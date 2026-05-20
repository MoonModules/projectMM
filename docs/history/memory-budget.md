# Memory Budget — ESP32 without PSRAM

Measured on Olimex ESP32-Gateway Rev G (no PSRAM, 320KB internal RAM).

## Heap breakdown (128x128 grid, mirror, noise, ArtNet + Preview)

| Component | Bytes | Notes |
|-----------|-------|-------|
| Boot heap | 290,240 | Before any init |
| After Ethernet init | ~270,000 | lwIP + Ethernet driver |
| Pipeline buffers | ~61,000 | Logical buffer (12KB) + physical buffer (49KB) + LUT |
| Preview frame buffer | ~49,000 | 128×128×3 + 7 header, lives for entire runtime |
| HTTP server + WebSocket | ~4,000 | TcpServer listener + kernel buffers |
| WebSocket connection | ~4,000 | lwIP send/receive buffers per active connection |
| MoonModule instances | ~2,000 | All modules, controls, vtables |
| **Free heap (running)** | **~67,000** | Fluctuates ±4KB with TCP send buffer lifecycle |

## Performance impact of Preview

| Metric | Without preview | With preview |
|--------|----------------|-------------|
| FPS | 15-23 | 9 |
| Free heap | 128KB | 67KB |
| Cause | — | 49KB frame buffer + TCP send overhead |

Preview FPS (default 20) can be lowered to reduce CPU load. At preview fps=5, effect FPS improves significantly.

## Key limits

- 128x128 = 16,384 lights is feasible without PSRAM (67KB free heap)
- Larger grids would need PSRAM for the buffers
- Preview frame is the single largest allocation (49KB for 128x128)
- Without preview, 200KB+ would be available for effects/layers

## Memory savings ideas

### Eliminate PreviewDriver's frame buffer (saves ~49KB)

The PreviewDriver currently copies the entire output buffer into its own PreviewFrame. Instead, HttpServerModule could send the DriverGroup's output buffer directly via WebSocket — just prepend the 7-byte header at send time (two writes: header + buffer data). This is how MoonLight works: drivers read directly from the physical display buffer (`layerP.lights.channelsD`), no copy.

Implementation: HttpServerModule gets a pointer to the output buffer (via Scheduler tree walking or explicit setter). In `loop20ms()`, send the 7-byte header as one WebSocket frame chunk, then the raw buffer data. The PreviewDriver module and PreviewFrame struct become unnecessary. Saves 49KB heap + eliminates the memcpy per frame.

Feasibility: high. Single-threaded scheduler means no race condition — the buffer is stable between loop iterations. The only change is that the WebSocket frame is sent in two `write()` calls (header + data) instead of one.

### Reduce grid size when memory is tight

128x128 = 49KB per buffer. With mirror modifier, there are two buffers (12KB logical + 49KB physical). Dropping to 64x64 cuts each buffer to 12KB — total pipeline buffers drop from 61KB to ~16KB.

### Skip physical output buffer for 1:1 mapping

Already implemented: when `oneToOneMapping` is true (no modifiers), DriverGroup skips the output buffer and drivers read the Layer buffer directly. This saves 49KB. Only applies when no mirror/modifier is active.

### Defer Preview allocation

Only allocate the preview frame buffer when a WebSocket client actually connects. Free it when the last client disconnects. Most of the time (no browser open), the 49KB is wasted.

### Smaller preview resolution

Send a downsampled preview (e.g. every 4th pixel) to reduce frame size from 49KB to ~3KB. The 3D point cloud still looks good at lower resolution. Control via a `previewScale` parameter.
