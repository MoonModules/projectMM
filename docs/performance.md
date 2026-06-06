# Performance & Memory

projectMM's per-step **performance contracts** live in the scenario JSONs — each `test/scenarios/*.json` step carries a per-target `contract` block (`tick_us` ceiling + `free_heap` floor) and an `observed` block (the latest reading per target). The scenarios are the source of truth and the assertion surface: every PR runs against them. See [testing.md § Performance contracts](testing.md#performance-contracts-contracttarget) for the contract semantics and renegotiation workflow. The headline numbers users care about are in [README.md § Performance](../README.md#performance).

This document holds what scenarios can't carry: structural sizes (`sizeof`), build-variant deltas, and the WiFi/Ethernet physics that explain *why* a contract comes out where it does.

---

## Desktop (macOS, Apple Silicon)

Desktop ArtNet sends to a non-existent IP so packets complete instantly; `freeHeap` returns 0 (unlimited). Per-step tick budgets live in `contract.pc-macos` blocks across the scenarios.

### sizeof (desktop, 64-bit)

| Class | sizeof (bytes) |
|-------|---------------|
| MoonModule | 120 |
| Layer | 208 |
| Drivers | 408 |
| GridLayout | 128 |
| SystemModule | 368 |
| NetworkModule | 336 |
| HttpServerModule | 168 |

`Drivers` grew from 120 → 408 with the per-driver `Correction` stage (256-entry brightness LUT + channel-order table). Other classes grew ~16-32 bytes each as `MoonModule` itself grew (rolling-range observed slot + wired-by-code flag + per-child `tickChildren` accounting fields).

Binary: **358 KB** (debug-arm64; release-strip yields a smaller number).

### Memory at 128×128 with mirror

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 92 KB | 12 KB buffer + 80 KB LUT (uint32_t indices on 64-bit) |
| Drivers | 48 KB | output buffer (128×128×3) |

---

## ESP32 — Olimex Gateway Rev G (no PSRAM, 320 KB internal)

Per-step tick/heap live in `contract.esp32-eth-wifi` and `contract.esp32-eth` across the scenarios; see the [README perf table](../README.md#performance) for the headline grid×board matrix. The notes below cover what those rows don't.

### Run-to-run variance

Individual measurements vary ~5–10% on the Olimex board with no configuration change — inherent ESP32/Ethernet timing jitter (lwIP `tcpip_thread` scheduling, EMAC DMA, Ethernet ACK pacing). Scenarios use 10% default ESP32 tolerance to absorb this; when a step trips, re-run before treating it as a real regression. The `collect_kpi.py --commit` gate parses a single `tick:` line from `esp32/monitor.log` and can flag an unlucky sample — same rule applies.

### ArtNet over WiFi vs Ethernet

| | Ethernet | WiFi STA |
|--|----------|----------|
| ArtNet (97 UDP packets) | ~27,000 µs | ~110,000 µs |
| Total tick | ~50,000 µs / 20 FPS | ~130,000 µs / 7 FPS |

WiFi `sendto()` is ~1,140 µs/packet vs Ethernet's ~280 µs — CSMA/CA backoff, rate adaptation, link-layer retries. Not a code regression; WiFi physics. For ArtNet at 16K lights, use Ethernet.

### Build-variant note: WiFi-only `esp32` is slow on Olimex

Same source tree, same MCU (ESP32 classic, 160 MHz):

| Board / firmware | 128×128 tick | ArtNet send |
|---|---|---|
| Olimex Gateway, `esp32` (WiFi-only) | 220 ms (4 FPS) | 155 ms |
| Olimex Gateway, `esp32-eth-wifi` | 85–95 ms (10–12 FPS) | 38 ms |

The Olimex `esp32` build is 4× slower at ArtNet than `esp32-eth-wifi` on the same board — `sdkconfig.defaults.eth` enlarges a shared lwIP/WiFi buffer pool via `CONFIG_ETH_DMA_*` that the WiFi-only build doesn't get. **Use `esp32-eth-wifi` for any ArtNet workload on classic ESP32**, even without Ethernet connected. Generic ESP32 boards (no PCB-trace antenna, less stable 3V3) vary wildly in WiFi TX quality vs the Olimex.

### Memory at 128×128 with mirror

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 52 KB | 12 KB buffer + 40 KB LUT (uint16_t indices on ESP32) |
| Drivers | 48 KB | output buffer (128×128×3) |
| System + Network | 0 | char buffers in class, no heap |

LUT is half desktop size (uint16_t vs uint32_t per entry). The 1:1 (no-modifier) path skips the LUT entirely; see `scenario_Layer_memory_1to1` vs `scenario_MirrorModifier_memory_lut`.

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
| **Free heap (running)** | **~104,000** | stable, no leaks |

---

## ESP32-S3 — LOLIN S3 N16R8 (16 MB flash, 8 MB octal PSRAM)

`esp32s3-n16r8` firmware on LOLIN S3 N16R8 at `Network.txPowerSetting=8` dBm (the brown-out cap injected by `boards.json`). 128×128 grid, Mirror XY, ArtNet over WiFi STA — the `scenario_GridLayout_grid_sizes.json` sweep against the live device. Per-step tick/heap live in `observed.esp32s3-n16r8` across the scenarios. Numbers below are the 128×128 step.

| Metric | Value | Notes |
|---|---|---|
| Total tick | ~164 ms / 6 FPS | Dominated by ArtNet at the 8 dBm cap |
| ArtNetSend | ~93 ms (97 UDP packets) | ~960 µs/packet — slower than full-power WiFi (cf. Olimex `esp32-eth-wifi` at 38 ms) because the cap cuts radio TX margin, association-rate adaptation lands at a lower MCS rate, and packets retry more |
| Free internal RAM | ~240 KB | The comparable, scarce resource. Stays flat (~238–240 KB) across all grid sizes — the Layer buffer + LUT live in PSRAM, so growing the grid doesn't touch internal RAM. This is the number the README perf table shows for the S3, so devices compare on the same axis. |
| Free heap (incl. PSRAM) | ~8,163 KB | The PSRAM-merged total (`totalHeap` reports 8 MB combined). Looks huge but isn't the constraint — assume PSRAM is ample for now. |
| maxBlock (internal) | ~164 KB | Internal-RAM largest contiguous block — the scarce-resource KPI. `maxAllocBlock` (any-memory) reports ~8 MB on PSRAM boards and is meaningless as a pressure signal; SystemModule + scenario_runner use `maxInternalAllocBlock` instead. |
| Layer buffer | 92 KB | In PSRAM (auto by heap_caps preference) |
| Image | 1,307 KB | ~30% larger than `esp32-eth-wifi` due to USB-Serial-JTAG driver + Improv-dual-transport listener |

Per-grid-size FPS from the same sweep: 16×16 → 1672, 32×32 → 287, 64×64 → 25, 128×128 → 6. The steep drop is ArtNet-bound: packet count scales with the pixel count, and at 8 dBm each packet is ~3× slower on-air than the Olimex Ethernet path.

### Why ArtNet is slower at 8 dBm

The LOLIN brown-out fix caps TX power 12 dB below default (8 dBm vs ~20 dBm). At lower TX power, the WiFi PHY rate-adaptation algorithm picks a slower MCS rate to maintain link margin — for a frame burst this means more time on-air per packet. ~960 µs/packet × 97 packets = the ~93 ms ArtNet budget. The cap is the price of a stable association on this hardware; without it the radio brown-outs and ArtNet doesn't get sent at all.

**Use Ethernet-capable boards for high-FPS ArtNet workloads.** The LOLIN S3 N16R8 fits the "lots of PSRAM, accept WiFi compromise" niche — large pixel buffers or feature-heavy effects that wouldn't fit in 320 KB internal RAM.

### Memory at 128×128 with mirror

| Module | dynamicBytes | Notes |
|--------|-------------|-------|
| Layer | 92 KB | Buffer lives in PSRAM (vs 12 KB on Olimex internal) — full uint32_t LUT instead of halved uint16_t |
| Drivers | 48 KB | Output buffer (128×128×3) |
| Free internal | ~240 KB free, ~164 KB largest block | Plenty of headroom for WiFi + lwIP + Improv-on-both-transports |

The PSRAM-merged heap (`totalHeap() > totalInternalHeap()`) is auto-detected — SystemModule binds the `psram` progress control only when this comparison is true. See `docs/moonmodules/core/SystemModule.md`.

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
uv run scripts/build/build_esp32.py --firmware esp32-eth-wifi
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
| Embedded UI assets | ~50 KB | `index.html`, `app.js`, `style.css`, `install-picker.js`, logo PNG — packed as `constexpr uint8_t[]`. |
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
