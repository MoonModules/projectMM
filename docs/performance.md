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

### Timing (128x128 grid, mirror XY, rainbow effect, Ethernet, browser connected)

Per-module breakdown from `esp32/monitor.log`, eth-only firmware, 16,384 lights:

| Module | Time (us) | % of tick | Notes |
|--------|----------|----------|-------|
| DriverGroup (BlendMap LUT traversal) | 45,800 | **89%** | 4096 logical → 16384 physical via CSR LUT, **includes** the ArtNet child |
| &nbsp;&nbsp;↳ ArtNet (97 UDP packets) | 27,700 | 54% | lwIP per-packet overhead (connected socket + core locking) |
| RainbowEffect | 3,400 | 7% | 4096 logical pixels |
| Layer | 3,500 | 7% | buffer clear + effect dispatch |
| System + Network | ~900 | 2% | loop1s diagnostics |
| HttpServer | ~850 | 2% | preview broadcast + state push (was ~44,000 pre-fix) |
| Preview | ~340 | <1% | downsample strided copy |
| **Total tick** | **~51,000** | **FPS: 19** | |

The FPS-swing fix (HttpServer ~44,000 → ~850 µs) and the ArtNet send-cost optimization (~50,000 → ~27,700 µs) together brought the steady tick from ~69 ms / 14 FPS to ~51 ms / 19 FPS with a browser connected.

### Timing comparison (128x128, different configurations)

| Configuration | Tick | FPS | Free heap |
|--------------|------|-----|-----------|
| Ethernet, mirror XY, browser connected (current) | ~51ms | 19 | 128KB |
| Ethernet, mirror XY (before FPS-swing + ArtNet fixes) | 69ms | 14 | 124KB |
| Ethernet, mirror XY (before System/Network) | 58ms | 17 | 153KB |
| 128x64, Ethernet, mirror XY | 26-30ms | 33-37 | 182-204KB |

### Run-to-run tick variance

The steady tick is ~51 ms / 19 FPS, but individual live-scenario measurements vary roughly 50,000–66,000 µs run-to-run on the Olimex board, even with no configuration change. The baseline reading itself swings ~50,300–53,600 µs across consecutive scenarios. This is inherent ESP32/Ethernet timing jitter (lwIP `tcpip_thread` scheduling, EMAC DMA timing, Ethernet ACK pacing) — not a regression.

Consequence for the live-scenario suite: relative bounds (`min_pct`) with a tight margin (1 FPS, ~1000 µs) can fail on an unlucky sample even when nothing changed. When triaging a live-scenario failure, re-run before treating a 1-FPS miss as real; a genuine regression shows up consistently across runs. The absolute `min_fps_led_product` floor is set at the 128×128 reference (55,556 µs budget) which sits inside the variance band, so it too can flag a slow sample — it is enforced as a hard gate only in `collect_kpi.py --commit`, which captures a settled median rather than a single scenario step.

### ESP32 tick variability — root cause found and fixed

**Symptom:** the render tick collapsed from ~38 ms (26 FPS) with no browser to ~100-155 ms (6-9 FPS) when a browser connected, varying with the browser's ACK timing.

**Root cause:** `HttpServerModule::broadcastPreviewFrame()` pushed a ~49 KB WebSocket binary frame to each browser. The non-blocking socket's lwIP send buffer (~5.7 KB) filled, and `TcpConnection::write()` spun `vTaskDelay(1ms)` retries until all 49 KB drained — 40+ ms per frame, blocking the render task. PreviewDriver capped the preview at 20 fps, but 20 × ~40 ms ≈ 800 ms/s overwhelmed the tick.

**Fix:** the preview broadcast uses a single non-blocking scatter-gather write (`TcpConnection::writeChunks`, one `writev`/`sendmsg`). For the write to be atomic the frame must fit lwIP's TCP send buffer, which is enlarged to 11520 B (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT`), so PreviewDriver downsamples the preview to ≤1849 voxels (~5.5 KB payload, adaptive stride) — the render task never blocks and the frame always sends whole. The PreviewDriver `fps` default dropped 20 → 12.

**Measured (Olimex, eth-only firmware, 128×128 grid):** before the fix the `HttpServer` step cost ~44,000 µs/tick with a browser connected and the tick collapsed to 9 FPS. After the fix `HttpServer` is ~500-900 µs/tick steady (brief ~8 ms spikes once per second for the JSON state push), and FPS holds at 13 with a browser connected — the same as with no browser. The preview still animates in the browser.

### ArtNet UDP send cost

With HttpServer no longer a factor, the tick was dominated by `ArtNet` — 97 UDP universe packets per frame for 16,384 lights. Measured at ~505 µs per `sendto` (uniform, not burst stalls): ~120 µs route + ARP lookup, ~225 µs cross-thread round-trip to lwIP's `tcpip_thread`, ~160 µs pbuf/framing/EMAC.

Two fixes, measured on hardware (Olimex, eth-only, 128×128):

- **Connected UDP socket** — `UdpSocket::connect()` binds the destination once, so each `sendTo()` skips the per-packet address parse + route lookup. ~49,000 → ~37,000 µs/tick.
- **lwIP core locking** (`CONFIG_LWIP_TCPIP_CORE_LOCKING=y`) — socket calls take the TCP/IP core mutex and run inline instead of context-switching to `tcpip_thread` per call. ~37,000 → ~27,000 µs/tick.

Combined: ArtNet ~50,000 → ~26,600 µs/tick, and the overall tick ~73 → ~49 ms — **20 FPS at 128×128 with ArtNet output and a browser connected**. The remaining ~26 ms is the genuine floor (97 × ~280 µs: pbuf alloc + IP/Ethernet framing + EMAC DMA + ~4 ms wire time). Reducing it further would need packet batching or moving ArtNet output off the render task.

### Preview `detail` cost on the render tick

The PreviewDriver downsample (strided RGB copy into the owned buffer) runs on the render task. Measured live on the Olimex board, 128×128 grid / 16,384 lights, via the `preview-detail` scenario:

| Preview setting | Tick | Render FPS |
|-----------------|------|-----------|
| baseline (no preview change) | 51,257 µs | 19 |
| `detail` 1 (coarse, 16×16) | 53,769 µs | 18 |
| `detail` 2 (medium, 32×32) | 55,677 µs | 17 |
| `detail` 3 (fine, 43×43) | 65,434 µs | 15 |
| `decompress` on | 54,313 µs | 18 |
| `decompress` off | 54,788 µs | 18 |

`decompress` is purely client-side and has no render-tick cost, as designed. `detail` does have a cost: the strided copy across all 16,384 lights scales with the voxel budget, and `detail = 3` adds ~14 ms/tick (19 → 15 FPS). The send is non-blocking, but the downsample work itself is on the hot path.

Known, accepted for now: `detail = 3` drops render FPS below the 18 FPS / 16384-light throughput floor (`min_fps_led_product`), and `detail = 2` is marginal (55,677 µs vs the 55,556 µs budget). The preview is a dev visualization, so a lower render FPS while a browser is open at high detail is tolerated. If this needs fixing later, the downsample copy could move off the render task or `detail` be capped at 2.

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
