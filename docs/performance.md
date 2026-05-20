# Performance & Memory

Measured per-module timing, memory allocation, and sizeof for each platform. Updated from live scenario runs and console output. Memory optimization ideas and analysis in [history/memory-budget.md](history/memory-budget.md).

## Desktop (macOS, Apple Silicon)

### Timing (128x128 grid, mirror XY, noise effect)

| Module | Time (us) | % of tick |
|--------|----------|----------|
| Noise effect | 65 | 63% |
| DriverGroup (blendMap + drivers) | 41 | 39% |
| ArtNet | 4 | 4% |
| **Total tick** | **106** | **FPS: 9,400** |

### Memory (128x128 with mirror)

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 92KB | 12KB buffer + 80KB LUT (uint32_t indices on desktop) |
| DriverGroup | 48KB | output buffer (128x128x3) |

Desktop `freeHeap` returns 0 (unlimited). No memory constraints.

### sizeof (desktop, 64-bit)

| Class | sizeof (bytes) |
|-------|---------------|
| MoonModule | 96 |
| Layer | 176 |
| DriverGroup | 120 |
| GridLayout | 104 |
| HttpServerModule | 144 |

## ESP32 — Olimex Gateway Rev G (no PSRAM, 320KB internal)

### Timing (128x128 grid, mirror XY, noise effect)

| Module | Time (us) | % of tick | Notes |
|--------|----------|----------|-------|
| Noise effect | 11,200 | 19% | 4096 logical pixels |
| BlendMap (LUT traversal) | 17,000 | 29% | 4096 to 16384 via CSR LUT |
| ArtNet (97 UDP packets) | 30,000 | **51%** | lwIP per-packet overhead |
| HttpServer | 90 | <1% | loop20ms only |
| **Total tick** | **58,000** | **FPS: 17** | |

ArtNet dominates. Each of 97 UDP packets takes ~309us through lwIP.

### Timing (128x64 grid, mirror XY)

| Metric | 128x128 | 128x64 |
|--------|---------|--------|
| Tick | 58ms | 30ms |
| FPS | 17 | 33 |
| Free heap | 153KB | 204KB |
| ArtNet universes | 97 | 48 |

### Memory (128x128 with mirror)

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 52KB | 12KB buffer + 40KB LUT (uint16_t indices on ESP32) |
| DriverGroup | 48KB | output buffer (128x128x3) |

LUT is half the size of desktop (uint16_t vs uint32_t per entry).

### Heap breakdown

| Component | Bytes | Notes |
|-----------|-------|-------|
| Boot heap | 290,240 | Before any init |
| After Ethernet init | ~270,000 | lwIP + Ethernet driver |
| Layer buffer | 12,288 | 64x64x3 (logical, halved by mirror) |
| Mapping LUT | 40,962 | offsets + destinations (uint16_t) |
| Driver output buffer | 49,152 | 128x128x3 (physical) |
| Preview frame | 0 | Zero-copy: pointer to output buffer, no allocation |
| HTTP + WebSocket | ~8,000 | Server + kernel buffers |
| MoonModule instances | ~2,000 | All modules, controls, vtables |
| **Free heap (running)** | **~153,000** | Stable, no leaks observed |

### Memory during mirror toggle

| State | Free heap | Tick | FPS |
|-------|----------|------|-----|
| Mirror XY on | 153KB | 58ms | 17 |
| Mirror X off | 131KB | 71ms | 14 |
| Mirror XY off | 89KB | 97ms | 10 |
| Mirror XY on again | 152KB | 58ms | 17 |

Disabling mirror INCREASES memory usage: layer buffer grows from 12KB (64x64) to 49KB (128x128), even though LUT (40KB) and driver buffer are freed. Net: 89KB free vs 153KB free.

### Key limits

- 128x128 = 16,384 lights is feasible without PSRAM (153KB free heap)
- ArtNet is the bottleneck (51% of frame time), not rendering
- 128x64 doubles FPS (33 vs 17) — halving pixels halves work

## 1:1 identical vs LUT pipeline

| Metric | 1:1 identical (no modifier) | With mirror (LUT) |
|--------|---------------------------|-------------------|
| Layer dynamicBytes | 49KB (buffer only) | 52KB (buffer + LUT) |
| DriverGroup dynamicBytes | 0 | 48KB (output buffer) |
| Total pipeline | 49KB | 100KB |
| Extra overhead | none | LUT traversal + blendMap |

The 1:1 identical path saves ~51KB and skips blendMap entirely.

## Memory optimization ideas

### Eliminate PreviewDriver frame buffer (saves ~49KB)

HttpServerModule sends DriverGroup's output buffer directly via WebSocket — just prepend the 7-byte header. No copy needed. This is how MoonLight works.

### Defer Preview allocation

Only allocate when a WebSocket client connects. Free on disconnect. Saves 49KB when no browser is open.

### Smaller preview resolution

Downsample preview (every 4th pixel) to reduce from 49KB to ~3KB. Point cloud still looks good.

### Skip output buffer for 1:1 identical

Already implemented: `hasLUT()` returns false, DriverGroup skips output buffer, drivers read Layer buffer directly. Saves 49KB.
