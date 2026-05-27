# Performance & Memory

Measured per-module timing, memory allocation, and sizeof for each platform. Updated from live scenario runs and console output.

---

## Desktop (macOS, Apple Silicon)

| Module | Time (µs) | % of tick |
|--------|----------|----------|
| Noise effect | 50 | 100% |
| Drivers (blendMap + ArtNet) | ~0 | <1% |
| **Total tick** | **50** | **FPS: 20,000** |

Desktop ArtNet sends to a non-existent IP so packets complete instantly. `freeHeap` returns 0 (unlimited).

### Memory (128×128 with mirror)

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 92 KB | 12 KB buffer + 80 KB LUT (uint32_t indices on 64-bit) |
| Drivers | 48 KB | output buffer (128×128×3) |

### sizeof (desktop, 64-bit)

| Class | sizeof (bytes) |
|-------|---------------|
| MoonModule | 104 |
| Layer | 176 |
| Drivers | 120 |
| GridLayout | 104 |
| SystemModule | ~280 |
| NetworkModule | ~320 |
| HttpServerModule | 144 |

Binary: **131 KB**

---

## ESP32 — Olimex Gateway Rev G (no PSRAM, 320 KB internal)

### Timing (128×128, mirror XY, RainbowEffect, Ethernet, browser connected)

Per-module breakdown from `esp32/monitor.log`, `esp32-eth-wifi` firmware, 16,384 lights:

| Module | Time (µs) | % of tick | Notes |
|--------|----------|----------|-------|
| Drivers (BlendMap + ArtNet) | 45,800 | **89%** | 4096 logical → 16384 physical via LUT; ArtNet is 27,700 µs of this |
| &nbsp;&nbsp;↳ ArtNet (97 UDP packets) | 27,700 | 54% | connected socket + lwIP core locking |
| RainbowEffect | 3,400 | 7% | 4096 logical pixels |
| Layer | 3,500 | 7% | buffer clear + effect dispatch |
| System + Network | ~900 | 2% | loop1s diagnostics |
| HttpServer | ~850 | 2% | preview broadcast + state push |
| Preview | ~340 | <1% | downsample strided copy |
| **Total tick** | **~51,000** | **FPS: 19** | |

### Timing comparison (128×128, various configurations)

| Configuration | Tick | FPS | Free heap |
|--------------|------|-----|-----------|
| Ethernet, mirror XY, PlasmaEffect, browser connected | ~44 ms | 22 | 132 KB |
| Ethernet, mirror XY, RainbowEffect, browser connected | ~51 ms | 19 | 128 KB |
| 128×64, Ethernet, mirror XY | 26–30 ms | 33–37 | 182–204 KB |

### Run-to-run variance

Individual measurements vary ~50,000–66,000 µs on the Olimex board with no configuration change — inherent ESP32/Ethernet timing jitter (lwIP `tcpip_thread` scheduling, EMAC DMA, Ethernet ACK pacing). When triaging a live-scenario failure, re-run before treating a 1-FPS miss as real; a genuine regression shows up consistently. The `collect_kpi.py --commit` gate parses a single `tick:` line from `esp32/monitor.log` and can flag an unlucky sample — same rule applies.

### ArtNet over WiFi

| | Ethernet | WiFi STA |
|--|----------|----------|
| ArtNet (97 UDP packets) | ~27,000 µs | ~110,000 µs |
| Total tick | ~50,000 µs / 20 FPS | ~130,000 µs / 7 FPS |

WiFi `sendto()` is ~1,140 µs/packet vs Ethernet's ~280 µs — CSMA/CA backoff, rate adaptation, link-layer retries. Not a code regression; WiFi physics. For ArtNet at 16K lights, use Ethernet. See [decisions.md](history/decisions.md) "next-iteration branch" for the root-cause analysis of the preview FPS-swing and ArtNet optimizations.

### Build-variant WiFi comparison (128×128, 2026-05-25)

Same source tree, same MCU (ESP32 classic, 160 MHz):

| Board | Build | Tick / FPS | ArtNet send | `/api/state` | `/app.js` (77 KB) |
|---|---|---|---|---|---|
| Olimex Gateway | `esp32` (WiFi-only) | 220 ms / 4 FPS | 155 ms | 0.22 s | 0.29 s |
| Olimex Gateway | `esp32-eth-wifi` | 85–95 ms / 10–12 FPS | 38 ms | 4.34 s | 1.47 s |
| Generic ESP32 board | `esp32` | 100 ms / 10 FPS | 45 ms | timeout | 32 s stall |
| Generic ESP32 board | `esp32-eth-wifi` | 82–97 ms / 10–12 FPS | 28–45 ms | timeout | partial (28 KB in 10 s) |

Finding: the Olimex `esp32` build is 4× slower at ArtNet than `esp32-eth-wifi` on the same board — `sdkconfig.defaults.eth` likely enlarges a shared lwIP/WiFi buffer pool via `CONFIG_ETH_DMA_*`. Fix tracked in [plan.md](plan.md). Generic boards vary wildly in WiFi TX quality vs the Olimex (PCB-trace antenna, regulated 3V3).

**Use `esp32-eth-wifi` for any ArtNet workload on classic ESP32**, even without Ethernet connected.

### Preview `detail` cost (128×128, live scenario)

| Preview setting | Tick | FPS |
|-----------------|------|-----|
| baseline | 51,257 µs | 19 |
| `detail` 1 (16×16) | 53,769 µs | 18 |
| `detail` 2 (32×32) | 55,677 µs | 17 |
| `detail` 3 (43×43) | 65,434 µs | 15 |
| `decompress` on/off | 54,313 / 54,788 µs | 18 |

`decompress` is client-side only, zero render cost. `detail = 3` adds ~14 ms/tick; the downsample copy runs on the hot path. Accepted for now — the preview is a dev tool. Cap tracked in [plan.md](plan.md).

### Memory (128×128 with mirror)

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 52 KB | 12 KB buffer + 40 KB LUT (uint16_t indices on ESP32) |
| Drivers | 48 KB | output buffer (128×128×3) |
| System + Network | 0 | char buffers in class, no heap |

LUT is half desktop size (uint16_t vs uint32_t per entry).

### Heap breakdown (128×128, mirror, RainbowEffect, Ethernet + mDNS)

| Component | Bytes | Notes |
|-----------|-------|-------|
| Boot heap | 290,240 | Before any init |
| After Ethernet + mDNS init | ~240,000 | lwIP + Ethernet + mDNS driver |
| Layer buffer | 12,288 | 64×64×3 (logical, halved by mirror) |
| Mapping LUT | 40,962 | offsets + destinations (uint16_t) |
| Driver output buffer | 49,152 | 128×128×3 (physical) |
| Preview frame | 0 | Zero-copy: pointer to output buffer |
| HTTP + WebSocket | ~8,000 | server + kernel buffers |
| MoonModule instances | ~3,000 | all modules combined |
| **Free heap (running)** | **~124,000** | stable, no leaks |

### Memory during mirror toggle

| State | Free heap | FPS |
|-------|----------|-----|
| Mirror XY on | 124 KB | 14 |
| Mirror X off | 103 KB | 12 |
| Mirror XY off | 98 KB | 13 |
| Mirror XY on again | 124 KB | 14 |

Note: tick/FPS here are from a pre-optimization snapshot (before the FPS-swing + ArtNet fixes); current steady-state with mirror XY on is 19 FPS.

### mDNS impact

Zero FPS impact. 5 KB heap difference is the mDNS service memory.

### 1:1 identical vs LUT pipeline

| | 1:1 identical (no modifier) | With mirror (LUT) |
|--|---------------------------|-------------------|
| Layer dynamicBytes | 49 KB (buffer only) | 52 KB (buffer + LUT) |
| Drivers dynamicBytes | 0 | 48 KB (output buffer) |
| Total pipeline | 49 KB | 100 KB |

1:1 path skips blendMap entirely and saves ~51 KB.

---

## ESP32 firmware size

Board: `esp32-eth-wifi` (largest variant). Partition layout: app0/app1 = 1.75 MB each, LittleFS = 384 KB, coredump = 64 KB.

| | Size |
|---|---|
| Firmware image | ~1.27 MB |
| App partition | 1.75 MB (~72% used, ~28% headroom) |
| DRAM used | 38 KB |
| DRAM free | 142 KB |
| `sizeof(MoonModule)` ESP32 | 56 bytes |

### Component breakdown (`esp32-eth-wifi`)

Run from project root after a clean build:

```bash
uv run scripts/build/build_esp32.py --board esp32-eth-wifi
idf.py -B build/esp32-esp32-eth-wifi \
       -DSDKCONFIG=build/esp32-esp32-eth-wifi/sdkconfig \
       size-components | head -40
```

These numbers shift with IDF version + sdkconfig — treat as rough proportions.

| Category | Approx | What |
|---|---|---|
| WiFi stack | ~400 KB | `esp_wifi` + `wpa_supplicant` + `esp_phy`. ~1/3 of the binary; `esp32-eth` drops it entirely (image → ~602 KB). |
| lwIP networking | ~180 KB | TCP/IP stack, DHCP, DNS, ARP, mDNS, SNTP. |
| TLS + cert bundle | ~170 KB | `mbedtls` + Mozilla root bundle (~50 KB). Used by `esp_https_ota`; reused by any future HTTPS client. |
| FreeRTOS + IDF core | ~150 KB | Kernel, esp_event, esp_timer, heap, logging, partition ops. Always present. |
| projectMM code | ~120 KB | `src/core/` + `src/light/` + `src/platform/esp32/` + `src/main.cpp`. ~10% of the binary. |
| HTTP server + WS | ~60 KB | `esp_http_server` + `HttpServerModule` routing. |
| Embedded UI assets | ~50 KB | `index.html`, `app.js`, `style.css`, `release-picker.js`, logo PNG — packed as `constexpr uint8_t[]`. |
| `esp_https_ota` + HTTP client | ~40 KB | OTA-from-URL machinery. |
| LittleFS | ~30 KB | `joltwallet/esp_littlefs` component. |
| Ethernet stack | ~30 KB | `esp_eth` + LAN8720 PHY. `esp32` variant drops this. |
| Misc (alignment, .rodata) | ~40 KB | Format strings, error tables, version metadata. |

### Variant size deltas

| Variant | Image | Delta | Difference |
|---|---|---|---|
| `esp32-eth-wifi` | 1.27 MB | — | Everything compiled in |
| `esp32` | 1.00 MB | −270 KB | No Eth driver + RMII config |
| `esp32-eth` | 0.60 MB | −670 KB | WiFi stack excluded (`EXCLUDE_COMPONENTS`) |
| `esp32s3-n16r8` | ~1.27 MB | similar | Xtensa LX7, 16 MB flash, different partition table |

### Size budget for upcoming features

| Feature | Est. | Rationale |
|---|---|---|
| Mozilla cert bundle trimmed | −40 KB | `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` keeps common roots only. `_NONE` saves ~50 KB but breaks TLS. |
| Static IPv6 | +20 KB | lwIP IPv6 component (off by default). Only if a deployment needs it. |
| WebSocket TLS (`wss://`) | ~0 KB | Reuses linked mbedTLS; certificate handling adds <5 KB. |
