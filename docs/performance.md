# Performance & Memory

projectMM's per-step **performance contracts** live in the scenario JSONs — each `test/scenarios/*.json` step carries a per-target `contract` block (`tick_us` ceiling + `free_heap` floor) and an `observed` block (the latest reading per target). The scenarios are the source of truth and the assertion surface: every PR runs against them. See [testing.md § Performance contracts](testing.md#performance-contracts-contracttarget) for the contract semantics and renegotiation workflow. The headline numbers users care about are in [README.md § Performance](../README.md#performance).

This document holds what scenarios can't carry: structural sizes (`sizeof`), build-variant deltas, and the WiFi/Ethernet physics that explain *why* a contract comes out where it does.

---

## Desktop (64-bit)

Desktop ArtNet sends to a non-existent IP so packets complete instantly; `freeHeap` returns 0 (unlimited). Per-step tick budgets live in per-host `contract.pc-<os>` blocks across the scenarios — `pc-macos` for macOS arm64, `pc-windows` for Windows x64, `pc-linux` for Linux. The `sizeof` and dynamic-memory numbers below apply to all 64-bit desktop targets; tick numbers differ by host CPU and live in the scenario contracts.

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

Binary sizes:

| Target | Size | Build |
|--------|------|-------|
| macOS arm64 | 358 KB | debug-arm64 (release-strip is smaller) |
| Windows x64 | 432 KB | MSVC Release, static CRT |

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

### All-effects sweep (every effect, no modifier, Ethernet + ArtNet)

This Olimex sweep ran each effect alone over a Layer (no modifier) at four square grids, through the real ArtNet + Preview drivers — the per-effect cost of the same pipeline the README's single-effect headline row measures. Numbers are from a live run; apply the ~5–10% variance above. (The per-effect sweep merged into the light/heavy bracket of `scenario_perf_full`; this table is the archived Olimex run.)

**FPS** (= 1,000,000 / tick µs):

| Effect | 16² | 32² | 64² | 128² |
|--------|----:|----:|----:|-----:|
| Lines | 12658 | 7633 | 2304 | 23 |
| Rainbow | 3831 | 968 | 143 | 22 |
| Noise | 1117 | 324 | 71 | 17 |
| Plasma | 3194 | 829 | 135 | 18 |
| PlasmaPalette | 6024 | 1733 | 267 | 21 |
| Metaballs | 2016 | 521 | 102 | 18 |
| Fire | 2762 | 784 | 159 | 21 |
| Particles | 4716 | 1848 | 424 | 30 |
| GlowParticles | 1706 | 586 | 128 | 14 |
| Checkerboard | 8474 | 2617 | 397 | 21 |
| Spiral | 2403 | 571 | 87 | 15 |
| Rings | 1118 | 284 | 45 | 12 |
| LavaLamp | 3030 | 756 | 113 | 18 |
| GameOfLife | 6802 | 1519 | 226 | 13 |

At 128² nearly every effect converges to ~12–23 FPS: the board is **ArtNet-output-bound** there (the ~38 ms synchronous send dominates the tick), so effect-compute differences wash out — the same physics the README narrates for the S3 over WiFi. Effect cost is visible at 64² and below, where Rings / Noise / Spiral are the heaviest and Lines / Checkerboard the lightest.

**Free internal heap** (KB) — the scarce resource on a no-PSRAM board; drops as the grid grows because the Layer buffer + LUT and the driver output buffer live in internal RAM:

| Effect | 16² | 32² | 64² | 128² |
|--------|----:|----:|----:|-----:|
| Lines | 220 | 214 | 195 | 126 |
| Rainbow | 173 | 167 | 158 | 126 |
| Noise | 171 | 168 | 159 | 126 |
| Plasma | 173 | 171 | 162 | 126 |
| PlasmaPalette | 170 | 168 | 160 | 126 |
| Metaballs | 173 | 171 | 162 | 126 |
| Fire | 173 | 170 | 157 | 110 |
| Particles | 172 | 167 | 149 | 77 |
| GlowParticles | 173 | 171 | 162 | 126 |
| Checkerboard | 173 | 168 | 159 | 123 |
| Spiral | 170 | 169 | 160 | 123 |
| Rings | 170 | 168 | 160 | 124 |
| LavaLamp | 170 | 169 | 160 | 124 |
| GameOfLife | 171 | 165 | 150 | 90 |

**Largest free internal block** (KB) — the memory-pressure signal that matters: free heap can be ample while fragmentation leaves no single block big enough for the next allocation:

| Effect | 16² | 32² | 64² | 128² |
|--------|----:|----:|----:|-----:|
| Lines | 108 | 108 | 108 | 62 |
| Rainbow | 92 | 88 | 76 | 62 |
| Noise | 92 | 88 | 76 | 62 |
| Plasma | 92 | 92 | 84 | 62 |
| PlasmaPalette | 92 | 88 | 80 | 62 |
| Metaballs | 92 | 92 | 84 | 62 |
| Fire | 96 | 92 | 76 | 62 |
| Particles | 80 | 80 | 68 | 34 |
| GlowParticles | 84 | 84 | 80 | 62 |
| Checkerboard | 96 | 88 | 72 | 62 |
| Spiral | 88 | 88 | 76 | 62 |
| Rings | 92 | 88 | 80 | 62 |
| LavaLamp | 92 | 88 | 72 | 62 |
| GameOfLife | 88 | 84 | 68 | 46 |

Most effects hold the same ~126 KB free / 62 KB block at 128² — their per-cell state is negligible next to the buffers. The exceptions carry real per-cell state: **Particles** (77 KB / 34 KB) and **GameOfLife** (90 KB / 46 KB) allocate a parallel grid-sized array, and **Fire** (110 KB) a heat map. Those three are the ones to watch for fragmentation headroom on a no-PSRAM board at large grids.

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
| Olimex Gateway, old WiFi-only `esp32` (pre-collapse) | 220 ms (4 FPS) | 155 ms |
| Olimex Gateway, default `esp32` (WiFi + Ethernet) | 85–95 ms (10–12 FPS) | 38 ms |

The default `esp32` build carries both the WiFi and Ethernet stacks, and `sdkconfig.defaults.eth` enlarges the shared lwIP/WiFi buffer pool via `CONFIG_ETH_DMA_*` — those buffers roughly quadruple ArtNet throughput versus a WiFi-only buffer pool. Generic ESP32 boards (no PCB-trace antenna, less stable 3V3) vary wildly in WiFi TX quality vs the Olimex.

### Memory at 128×128 with mirror

| Module | dynamicBytes | Breakdown |
|--------|-------------|-----------|
| Layer | 52 KB | 12 KB buffer + 40 KB LUT (uint16_t indices on ESP32) |
| Drivers | 48 KB | output buffer (128×128×3) |
| System + Network | 0 | char buffers in class, no heap |

LUT is half desktop size (uint16_t vs uint32_t per entry). The 1:1 (no-modifier) path skips the LUT entirely; see `scenario_Layer_memory_1to1` vs `scenario_MultiplyModifier_memory_lut`.

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

## ESP32-S3 — ESP32-S3 N16R8 Dev (16 MB flash, 8 MB octal PSRAM)

`esp32s3-n16r8` firmware on the ESP32-S3 N16R8 Dev at `Network.txPowerSetting=8` dBm (the brown-out cap injected by `deviceModels.json`). 128×128 grid, Mirror XY, ArtNet over WiFi STA — the grid sweep (now part of `scenario_perf_full`) against the live device. Per-step tick/heap live in `observed.esp32s3-n16r8` across the scenarios. Numbers below are the 128×128 step.

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

The brown-out cap drops TX power 12 dB below default (8 dBm vs ~20 dBm). At lower TX power, the WiFi PHY rate-adaptation algorithm picks a slower MCS rate to maintain link margin — for a frame burst this means more time on-air per packet. ~960 µs/packet × 97 packets = the ~93 ms ArtNet budget. The cap is the price of a stable association on this hardware; without it the radio brown-outs and ArtNet doesn't get sent at all.

**Use Ethernet-capable boards for high-FPS ArtNet workloads.** The ESP32-S3 N16R8 Dev fits the "lots of PSRAM, accept WiFi compromise" niche — large pixel buffers or feature-heavy effects that wouldn't fit in 320 KB internal RAM.

### Memory at 128×128 with mirror

| Module | dynamicBytes | Notes |
|--------|-------------|-------|
| Layer | 92 KB | Buffer lives in PSRAM (vs 12 KB on Olimex internal) — full uint32_t LUT instead of halved uint16_t |
| Drivers | 48 KB | Output buffer (128×128×3) |
| Free internal | ~240 KB free, ~164 KB largest block | Plenty of headroom for WiFi + lwIP + Improv-on-both-transports |

The PSRAM-merged heap (`totalHeap() > totalInternalHeap()`) is auto-detected — SystemModule binds the `psram` progress control only when this comparison is true. See `docs/moonmodules/core/SystemModule.md`.

### All-effects sweep — render-only (no output driver, audio + discovery disabled)

A render-only per-effect sweep on the S3 (`observed.esp32s3-n16r8`, build `Jun 17 2026`; this curve is what `scenario_perf_full`'s light/heavy bracket now measures on-device). Unlike the Olimex sweep above (which runs through the ArtNet driver and is output-bound at 128²), this one measures **raw render cost**: audio (I2S sampling) and the Devices module (the blocking HTTP discovery sweep) are disabled and **no output driver** is attached, so the tick is Layout→Layer→effect only. On the S3 the Layer buffer lives in PSRAM, so effect-compute is visible all the way to 16K pixels (it never converges to an output-bound floor the way the no-PSRAM Olimex does).

**Tick (µs)** — render only, ~5–10% run-to-run variance applies:

| Effect | 16² (256) | 32² (1K) | 64² (4K) | 128² (16K) |
|--------|----:|----:|----:|-----:|
| Lines | 88 | 96 | 179 | 6,425 |
| Rainbow | 285 | 849 | 3,228 | 16,207 |
| Noise | 913 | 2,951 | 11,661 | 51,230 |
| Plasma | 352 | 1,020 | 3,744 | 20,020 |
| PlasmaPalette | 146 | 423 | 1,765 | 10,085 |
| Metaballs | 462 | 1,757 | 6,108 | 28,576 |
| Fire | 382 | 1,138 | 4,505 | 22,745 |
| Particles | 229 | 535 | 1,945 | 15,792 |
| GlowParticles | 580 | 1,874 | 6,959 | 31,479 |
| Checkerboard | 121 | 345 | 1,098 | 8,500 |
| Spiral | 465 | 1,379 | 6,712 | 24,666 |
| Rings | 852 | 2,455 | 9,383 | 41,403 |
| LavaLamp | 309 | 974 | 3,612 | 21,243 |
| GameOfLife | 138 | 413 | 1,870 | 16,127 |

The cheapest (Lines, Checkerboard, PlasmaPalette) clear ~100 FPS even at 16K; the heaviest is **Noise** (51 ms = ~19 FPS at 16K — simplex noise per pixel), then Rings and GlowParticles. Effect-compute differences stay visible across the whole range because nothing is output-bound here.

**Free internal heap** holds ~8.54 MB at small grids and ~8.46–8.49 MB at 16K — the ~50–100 KB delta is just the grid-sized render buffer (the `model` array), and it returns to ~8.54 MB whenever the grid shrinks: **no leak, no fragmentation creep** across the sweep. Largest free internal block stays ~90–110 KB throughout. (Internal RAM is not the constraint on this PSRAM board; the Layer buffer is in PSRAM.)

---

## Incremental cost analysis (`scenario_perf_light` / `scenario_perf_full`)

These two scenarios start from a clean canvas and add one subsystem at a time, measuring the tick/heap delta per step, so each module's cost is isolated. Measured live (2026-06-17, render-only, audio + discovery disabled) on all three boards:

- **classic** — Olimex Gateway, ESP32 @240MHz, **no PSRAM** (320KB internal), `nrOfLightsType`=uint16
- **S3** — ESP32-S3 N16R8 @240MHz, 8MB PSRAM, uint32
- **P4** — Waveshare P4-NANO, ESP32-P4 @400MHz dual-core, 32MB PSRAM, uint32

All figures tick µs at 16² unless a grid is named; ~5–10% run-to-run variance, so small (<~30µs) deltas are near the noise floor.

### Per-subsystem cost (added one at a time, 16² grid)

Absolute tick at each step (the diff vs the prior row is that subsystem's cost):

| Step | classic | S3 | P4 | Reading |
|---|--:|--:|--:|---|
| Render floor (Grid+Layer+Checkerboard) | 129 | 133 | 67 | the baseline; P4 ~2× faster |
| − AudioModule disabled | 116 | 111 | n/a | **audio ≈ +13–22µs/tick** (fixed I2S block-read; no mic on the P4) |
| − Devices discovery disabled | 116 | 112 | 56 | idle discovery is free (boot sweep is one-shot) |
| + MultiplyModifier (2×2) | 315 | 292 | 96 | **+180–200µs** — the per-frame blend+map over the LUT |
| + PreviewDriver | 115 | 118 | 56 | apparatus; free |
| + NetworkSendDriver | 139 | 141 | 67 | ArtNet/DDP build+send; cheap at this size |
| + RmtLedDriver (64 LEDs) | 152 | 120 | 56 | per-frame encode+transmit at a fixed 64-LED output |
| + LcdLedDriver (64 LEDs) | n/a¹ | 142 | 57² | S3 LCD_CAM i80 |
| + ParlioLedDriver (64 LEDs) | n/a¹ | n/a | 58 | P4 Parlio |

¹ classic has only RMT; the LCD/Parlio rows there are **not** real measurements (the driver isn't compiled/registered on classic, the optional add is skipped, so the row just re-measures the prior pipeline). Gating these drivers out per chip is tracked in the backlog. ² P4 has LCD_CAM too, but Parlio is its scale path. "n/a" = driver absent on that chip.

**Expected, and confirmed everywhere:** audio is a small fixed per-tick cost; idle discovery is free; output drivers are cheap at a capped 64-LED output (none dominates the render path). The modifier's +~190µs at 16² is the one notable per-frame add — explained below (it's the blend+map, and it *pays for itself* at large grids).

### Effect compute — light vs heavy bracket, across grid sizes (render-only)

Tick µs; FPS in parens for the 16K row:

| Grid (pixels) | classic | S3 | P4 |
|---|--:|--:|--:|
| **Checkerboard (light)** | | | |
| 16² (256) | 149 | 119 | 61 |
| 32² (1K) | 357 | 328 | 133 |
| 64² (4K) | 1,147 | 1,090 | 452 |
| 128² (16K) | 4,360 | 7,949 | 1,940 |
| **Noise (heavy)** | | | |
| 16² (256) | 1,010 | 799 | 313 |
| 32² (1K) | 3,203 | 2,831 | 1,120 |
| 64² (4K) | 13,547 | 11,235 | 4,358 |
| 128² (16K) | 62,316 (16 FPS) | 50,555 (20 FPS) | 17,433 (57 FPS) |

All curves scale **~linear in pixel count** (no superlinear blowup → no realloc/fragmentation pathology). The heavy effect is the 16K bottleneck on every board, and the board ranking is P4 ≫ S3 > classic on heavy compute (the P4's 400MHz dual-core is ~3× the S3). **Surprise worth noting:** at light-16K the *classic* (4,360µs) beats the S3 (7,949µs) — the S3's PSRAM-resident buffer has higher access latency than the classic's internal RAM for the cheap Checkerboard inner loop, and classic's uint16 LUT is half the size; on the heavy effect the compute dominates and the S3 pulls ahead again. Fixed-point / strided-sampling ideas are on the [backlog](backlog/README.md).

### MultiplyModifier — compute down, memory up (Noise effect)

Often misread (I misread it first): with the default 2×2 kaleidoscope the modifier makes the *logical* grid ¼-size, so the effect computes on fewer pixels — the modifier **reduces** tick at large grids, and its real cost is the 1:N mapping-LUT **memory**.

Tick µs, Noise alone vs Noise+Multiply:

| Grid (physical) | classic alone | classic +Mult | S3 alone | S3 +Mult | P4 alone | P4 +Mult |
|---|--:|--:|--:|--:|--:|--:|
| 16² | 1,010 | 456 | 799 | 385 | 313 | 163 |
| 32² | 3,203 | 1,808 | 2,831 | 1,573 | 1,120 | 533 |
| 64² | 13,547 | 6,958 | 11,235 | 6,552 | 4,358 | 2,058 |
| 128² (16K) | 62,316 | **28,466 (35 FPS)** | 50,555 | 29,647 | 17,433 | **9,964 (100 FPS)** |

So the modifier roughly **halves** the heavy tick at every grid (¼ logical area, but the 1:N map adds back some cost). The memory price is the LUT-destinations array — on the S3 it cost +1.7KB(16²)→+93KB(16K); on the **classic at 16K it ran with ~36KB free heap / ~26KB largest block** — tight but working, no crash, no degrade. This **confirms the no-PSRAM viability**: 16K Noise+Multiply runs on the classic at 35 FPS render-only (and has historically run at 10–20 FPS when also sending out over **ArtNet** — that send, not the render, was the limiter). Not a no-PSRAM blocker.

## ESP32 firmware size

Board: the default `esp32` (WiFi + Ethernet — the largest classic variant, measured pre-collapse as `esp32-eth-wifi`). Partition layout: app0/app1 = 1.75 MB each, LittleFS = 384 KB, coredump = 64 KB.

| | Size |
|---|---|
| Firmware image | ~1.27 MB |
| App partition | 1.75 MB (~72% used, ~28% headroom) |
| DRAM used | 38 KB |
| DRAM free | 142 KB |
| `sizeof(MoonModule)` ESP32 | 56 bytes |

### Component breakdown (default `esp32`)

Run from project root after a clean build:

```bash
uv run scripts/build/build_esp32.py --firmware esp32
idf.py -B build/esp32-esp32 \
       -DSDKCONFIG=build/esp32-esp32/sdkconfig \
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
| Embedded UI assets | ~50 KB | `index.html`, `app.js`, `style.css`, `preview3d.js`, `install-picker.js`, logo PNG — packed as `constexpr uint8_t[]`. |
| `esp_https_ota` + HTTP client | ~40 KB | OTA-from-URL machinery. |
| LittleFS | ~30 KB | `joltwallet/esp_littlefs` component. |
| Ethernet stack | ~30 KB | `esp_eth` + LAN8720 PHY. Present in every classic variant since the collapse (RMII driver is always compiled in). |
| Misc (alignment, .rodata) | ~40 KB | Format strings, error tables, version metadata. |

### Variant size deltas

| Variant | Image | Delta | Difference |
|---|---|---|---|
| `esp32` (default, WiFi + RMII Eth) | 1.27 MB | — | Everything compiled in |
| `esp32-eth` | 0.60 MB | −670 KB | WiFi stack excluded (`EXCLUDE_COMPONENTS`) |
| `esp32s3-n16r8` | ~1.27 MB | similar | Xtensa LX7, 16 MB flash, different partition table; W5500 SPI Eth instead of RMII |

The default `esp32` carries both the WiFi and Ethernet stacks (1.27 MB); `esp32-eth` is the Ethernet-only build that drops the WiFi stack for ~670 KB less image.

### Size budget for upcoming features

| Feature | Est. | Rationale |
|---|---|---|
| Mozilla cert bundle trimmed | −40 KB | `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` keeps common roots only. `_NONE` saves ~50 KB but breaks TLS. |
| Static IPv6 | +20 KB | lwIP IPv6 component (off by default). Only if a deployment needs it. |
| WebSocket TLS (`wss://`) | ~0 KB | Reuses linked mbedTLS; certificate handling adds <5 KB. |
