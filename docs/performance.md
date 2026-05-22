# Performance & Memory

Measured per-module timing, memory allocation, and sizeof for each platform. Updated from live scenario runs and console output. Memory optimization ideas and analysis in [history/memory-budget.md](history/memory-budget.md).

## Desktop (macOS, Apple Silicon)

### Timing (128x128 grid, mirror XY, noise effect)

| Module | Time (us) | % of tick |
|--------|----------|----------|
| Noise effect | 50 | 100% |
| DriverGroup (blendMap + drivers) | ~0 | <1% |
| ArtNet | ~0 | <1% |
| **Total tick** | **50** | **FPS: 20,000** |

Desktop ArtNet sends to a non-existent IP so packets complete instantly.

### Memory (128x128 with mirror)

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 92KB | 12KB buffer + 80KB LUT (uint32_t indices on desktop) |
| DriverGroup | 48KB | output buffer (128x128x3) |

Desktop `freeHeap` returns 0 (unlimited). No memory constraints.

### sizeof (desktop, 64-bit)

| Class | sizeof (bytes) |
|-------|---------------|
| MoonModule | 104 |
| Layer | 176 |
| DriverGroup | 120 |
| GridLayout | 104 |
| SystemModule | ~280 |
| NetworkModule | ~320 |
| HttpServerModule | 144 |

### Binary size

Desktop: 131KB

## ESP32 — Olimex Gateway Rev G (no PSRAM, 320KB internal)

### Timing (128x128 grid, mirror XY, noise effect, Ethernet)

| Module | Time (us) | % of tick | Notes |
|--------|----------|----------|-------|
| Noise effect | 11,200 | 16% | 4096 logical pixels |
| BlendMap (LUT traversal) | 17,000 | 24% | 4096 to 16384 via CSR LUT |
| ArtNet (97 UDP packets) | 30,000 | **43%** | lwIP per-packet overhead |
| System + Network | ~2,000 | 3% | loop1s diagnostics |
| HttpServer | 90 | <1% | loop20ms only |
| **Total tick** | **69,000** | **FPS: 14** | |

With SystemModule + NetworkModule added (vs previous 58ms/17FPS without them), the 11ms overhead comes from: Ethernet event handling, mDNS, module loop1s diagnostics.

### Timing comparison (128x128, different configurations)

| Configuration | Tick | FPS | Free heap |
|--------------|------|-----|-----------|
| Ethernet, mirror XY (current) | 69ms | 14 | 124KB |
| Ethernet, mirror XY (before System/Network) | 58ms | 17 | 153KB |
| Ethernet, mirror off | 74-82ms | 12-13 | 98-103KB |
| 128x64, Ethernet, mirror XY | 30ms | 33 | 204KB |

### ESP32 tick variability (plan-11)

Measured: live capture lands anywhere from ~55ms / 18 FPS to ~100-155ms / 6-9 FPS on the same firmware with no scenario change. On slow ticks HttpServer dominates (~80-95 ms of the total) while the render path stays normal (Layer/Noise ~13 ms, DriverGroup+ArtNet ~75 ms) — the variance correlates with bursty WebSocket / HTTP work when a browser is connected. Recorded here so the swing isn't mistaken for a regression; the investigation item is tracked in [plan.md](plan.md).

### Memory (128x128 with mirror)

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 52KB | 12KB buffer + 40KB LUT (uint16_t indices on ESP32) |
| DriverGroup | 48KB | output buffer (128x128x3) |
| System + Network | 0 | No dynamic allocation (char buffers in class) |

LUT is half the size of desktop (uint16_t vs uint32_t per entry).

### Heap breakdown

| Component | Bytes | Notes |
|-----------|-------|-------|
| Boot heap | 290,240 | Before any init |
| After Ethernet + mDNS init | ~240,000 | lwIP + Ethernet + mDNS driver |
| Layer buffer | 12,288 | 64x64x3 (logical, halved by mirror) |
| Mapping LUT | 40,962 | offsets + destinations (uint16_t) |
| Driver output buffer | 49,152 | 128x128x3 (physical) |
| Preview frame | 0 | Zero-copy: pointer to output buffer |
| HTTP + WebSocket | ~8,000 | Server + kernel buffers |
| MoonModule instances | ~3,000 | All modules (System, Network, Layout, Layer, DriverGroup, HttpServer) |
| **Free heap (running)** | **~124,000** | Stable, no leaks observed |

### Memory during mirror toggle (from live scenario)

| State | Free heap | Tick | FPS |
|-------|----------|------|-----|
| Mirror XY on | 124KB | 69ms | 14 |
| Mirror X off | 103KB | 82ms | 12 |
| Mirror XY off | 98KB | 74ms | 13 |
| Mirror XY on again | 124KB | 69ms | 14 |

### mDNS impact (from mdns-toggle scenario)

| State | Tick | FPS | Heap |
|-------|------|-----|------|
| mDNS on | 69ms | 14 | 124KB |
| mDNS off | 69ms | 14 | 129KB |
| mDNS on again | 69ms | 14 | 124KB |

mDNS has zero FPS impact. The 5KB heap difference is the mDNS service memory.

### Firmware size

| Component | Size |
|-----------|------|
| Firmware image | ~880KB (partition layout updated; LittleFS available via platform fs API but no module uses it yet — plan-10) |
| App partition | 1.75MB |
| Flash chip | 4MB |
| DRAM used | 41KB |
| DRAM free | 139KB |
| `sizeof(MoonModule)` ESP32 | 56 bytes (was 80; saved 24 bytes per instance via const char* typeName_ + dirty bool) |

The partition layout matches projectMM v1: app0/app1 = 1.75 MB each, `spiffs` (LittleFS) = 384 KB, coredump = 64 KB. The joltwallet/esp_littlefs component adds ~30KB to the firmware image; plan-10 will use it for blob persistence. Partition is well under 50% used.

### Key limits

- 128x128 = 16,384 lights feasible on Ethernet (14 FPS, 124KB free heap)
- ArtNet is the bottleneck (43% of frame time), not rendering
- WiFi performance: pending testing (see plan.md)
- Partition space: 50% free in the app partition; 384KB filesystem partition holds dozens of config + preset files easily

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

Already implemented. PreviewFrame is zero-copy: `data` pointer to existing buffer, no allocation.

### Defer Preview allocation

Only allocate when a WebSocket client connects. Free on disconnect. Saves preview-related overhead when no browser is open.

### Smaller preview resolution

Downsample preview (every 4th pixel) to reduce frame data. Point cloud still looks good.

### Skip output buffer for 1:1 identical

Already implemented: `hasLUT()` returns false, DriverGroup skips output buffer, drivers read Layer buffer directly. Saves 49KB.

### Custom partition table

With firmware at 879KB / 1024KB (86%), a custom partition table is needed. See plan.md for options.
