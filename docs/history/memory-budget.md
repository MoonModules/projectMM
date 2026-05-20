# Memory Budget — ESP32 without PSRAM

Measured on Olimex ESP32-Gateway Rev G (no PSRAM, 320KB internal RAM). Current measurements in [performance.md](../performance.md).

## Heap breakdown (128x128 grid, mirror, noise, ArtNet + Preview)

| Component | Bytes | Notes |
|-----------|-------|-------|
| Boot heap | 290,240 | Before any init |
| After Ethernet init | ~270,000 | lwIP + Ethernet driver |
| Layer buffer | 12,288 | 64x64x3 (logical, halved by mirror) |
| Mapping LUT | 40,962 | offsets + destinations (uint16_t on no-PSRAM) |
| Driver output buffer | 49,152 | 128x128x3 (physical) |
| Preview frame | 0 | Zero-copy: pointer to output buffer, no allocation |
| HTTP server + WebSocket | ~8,000 | TcpServer + kernel buffers |
| MoonModule instances | ~2,000 | All modules, controls, vtables |
| **Free heap (running)** | **~153,000** | Measured via live scenarios, stable |

## Performance impact of Preview

| Metric | Without preview | With preview |
|--------|----------------|-------------|
| FPS | 15-23 | 9 |
| Free heap | 128KB | 67KB |
| Cause | — | 49KB frame buffer + TCP send overhead |

Preview FPS (default 20) can be lowered to reduce CPU load. At preview fps=5, effect FPS improves significantly.

## Key limits

- 128x128 = 16,384 lights is feasible without PSRAM (153KB free heap)
- ArtNet is the bottleneck (51% of frame time at 128x128), not rendering
- Without preview, ~49KB more heap available
- 128x64 doubles FPS (33 vs 17)

## Memory savings ideas

### ~~Eliminate PreviewDriver's frame buffer (saves ~49KB)~~ DONE

Already implemented. PreviewFrame is zero-copy: `data` is a pointer to the existing output buffer, no separate allocation. The 49KB saving is realized.

### Reduce grid size when memory is tight

Already implemented: adaptive allocation halves layer dimensions when buffer doesn't fit (minimum 8x8). See degradation cascade in [architecture-light.md](../architecture-light.md).

### Skip physical output buffer for 1:1 identical mapping

Already implemented: when `hasLUT()` returns false (no modifiers, grid without serpentine), DriverGroup skips the output buffer and drivers read the Layer buffer directly. Saves 49KB.

### Defer Preview allocation

Only allocate the preview frame buffer when a WebSocket client actually connects. Free it when the last client disconnects. Most of the time (no browser open), the 49KB is wasted.

### Smaller preview resolution

Send a downsampled preview (e.g. every 4th pixel) to reduce frame size from 49KB to ~3KB. The 3D point cloud still looks good at lower resolution. Control via a `previewScale` parameter.
