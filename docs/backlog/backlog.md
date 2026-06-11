# What to build next

Completed items are removed. This file is deleted when empty.

---

## Distribution

### Release 2.0 — distribution catches up to the source tree

1.0 ships ESP32 firmware (4 variants) + macOS arm64 + Windows x64. Still to add:

- **ESP32-P4** firmware variant — new chip target, new sdkconfig fragment, fits the existing `FIRMWARES` table in `build_esp32.py`.
- **Linux desktop binary** — third desktop job in `release.yml`, static-linked libstdc++.
- **Teensy 4.1** — toolchain-file build, `.hex` for Teensy Loader.
- **Raspberry Pi** — ARM64, cross-built or native.
- **macOS code-signing** — drops the Gatekeeper "downloaded from internet" prompt.
- **Windows code-signing** — drops the SmartScreen warning on first run of `projectMM.exe`. Same shape as macOS signing; needs an EV / OV code-signing certificate (Microsoft Trusted Signing is the cheapest current option). Until then, the README notes the SmartScreen prompt.
- **Runtime PHY / pin config for Ethernet** — replaces build-time Olimex-pin baking in `sdkconfig.defaults.eth` with a runtime picker via `platform::ethPresent()` / `platform::wifiPresent()`. Once landed, `esp32-eth*` variants stop being Olimex-specific; NetworkModule's `onBuildControls()` flips the `hidden` flag on absent-interface controls. **This unblocks the firmware-variant collapse below — it is the prerequisite.**

  **Target end-state once runtime PHY config lands — collapse the three classic variants to two.** Today there are three (`esp32` = WiFi-only, `esp32-eth` = Eth-only-Olimex, `esp32-eth-wifi` = both-Olimex), because the `.eth` fragment *bakes in Olimex's RMII pins* (clock GPIO17, reset GPIO5, LAN8720, addr 0 — see `sdkconfig.defaults.eth` + `ethInit()` in `platform_esp32.cpp`). So Ethernet can't be in the default build: a LOLIN/QuinLED/Generic board flashing it would drive Olimex's pins as RMII lines and stall at boot waiting for a PHY that isn't there. Once `ethInit()` selects pins/PHY at runtime (and no-ops when no PHY is present), the variants become:
  - **`esp32`** — the default: WiFi **and** Ethernet, Eth brought up only when a PHY is detected/configured. Replaces both `esp32` and `esp32-eth-wifi`.
  - **`esp32-eth-only`** (today's `esp32-eth`) — Eth only, WiFi compiled out via `EXCLUDE_COMPONENTS`. Kept for two real reasons: it saves flash, and it guarantees no WiFi stack is present to interfere (relevant to the [WiFi ArtNet performance](#wifi-artnet-performance-pending-investigation) and the eth-flapping investigation). Only the Eth-only case earns a separate firmware — so this is variant *reduction*, not explosion.
  - **PSRAM boards still get the default `esp32`/`esp32s3-*` build, Ethernet-capable, no extra firmware.** The **ESP32-S3 has no built-in EMAC**, but that does *not* mean a separate firmware: an external **SPI PHY (W5500) wired to GPIO pins** gives the S3 Ethernet, and which PHY/pins (RMII vs SPI, the W5500 MISO/MOSI/SCK/CS/IRQ above) is **runtime config**, exactly the runtime-PHY mechanism this item adds — the same way MoonLight handles it (`ethernetType: BoardDefault / LAN8720-RMII / W5500-SPI` per board). So the default firmware ships **both** the RMII and the SPI-PHY driver; whether Ethernet comes up, and over which PHY, is selected at runtime from the board/pin config. Classic ESP32 uses its internal RMII MAC; S3 uses an SPI PHY; neither needs its own firmware variant. (Product-owner proposal, recorded on PR #16.)
- **Installer UX polish** — clear "Pre-release (beta)" warning on RC/latest picks, yank-by-asset-tag instead of yank-by-release-deletion.

---

## ESP32 performance and memory

### Intermittent ~0.5 s LED pauses with the RMT driver (pending investigation)

Observed on the bench (2026-06): LED output running on the RMT driver occasionally freezes for about half a second. Postponed by the product owner until more observations exist. Ranked suspects from the initial analysis, each with a cheap experiment:

1. **WiFi modem power-save never disabled** — nothing in `src/` calls `esp_wifi_set_ps(WIFI_PS_NONE)`, so the IDF default `WIFI_PS_MIN_MODEM` is active; the radio's DTIM sleep causes exactly this class of intermittent multi-hundred-ms stall. WLED and the v1/v2 lineage disable sleep. Experiment: one line in the ESP32 platform code after association.
2. **NetworkSendDriver sending synchronously every tick to an absent destination** (default `192.168.1.70`) — lwIP keeps re-ARPing a dead address while the send sits in the render tick. Data point (2026-06-10): the bench esp32-16mb had NetworkSend *disabled* in its persisted config, consistent with the pauses being annoying enough to switch the sender off. Experiment: point the ArtNet IP at a live host (or disable the driver) and see if the pauses stop.
3. **`rmt_tx_wait_all_done` 1 s timeout** — a wedged transmission blocks the tick up to a full second (multi-pin: up to N×1 s). Least likely (~1 s, not ~0.5 s) but it's the only hard block in the driver itself.

If pauses correlate with UI control changes, also consider the 2 s-debounced SPIFFS save stalling flash-resident code. The per-tick KPI log around a pause discriminates between these immediately.

### E1.31 multicast receive (IGMP join)

NetworkReceiveEffect accepts E1.31 via unicast only — the same scope MoonLight ships. Multicast senders address the per-universe group `239.255.{universe_hi}.{universe_lo}`, which a receiver must join via IGMP; the platform `UdpSocket` has no `IP_ADD_MEMBERSHIP` support yet (lwIP `setsockopt` on ESP32, plain `setsockopt` on desktop, plus a join-per-accepted-universe bookkeeping question). Add when a multicast-only sender actually shows up on a bench; until then the spec documents "point sACN senders at the device's IP".

### WiFi ArtNet performance (pending investigation)

128×128 WiFi ArtNet measurements exist (see [performance.md](../performance.md) "ArtNet over WiFi" and "Build-variant WiFi comparison"). Remaining matrix:

- WiFi STA 64×64 (4K LEDs, 24 universes)
- WiFi STA 32×32 (1K LEDs, 6 universes)

This determines the practical LED limit for WiFi-only boards. Until the `sdkconfig.defaults` TX-buffer fix lands (identified in the build-variant table), **use `esp32-eth-wifi` for any ArtNet workload on classic ESP32** even if Ethernet isn't physically connected.

### Network round-trip test — drop/reorder measurement (deferred)

`scripts/scenario/run_network_roundtrip.py` measures PC→device→PC **latency and jitter** per protocol (ArtNet/E1.31/DDP) by timing how long a sent colour takes to appear in the device's preview stream. It deliberately does **not** measure per-frame **drops or reorder**, because the path can't track individual frames cleanly: `NetworkSendDriver` re-clocks at its own fps (decoupled from receive) and `NetworkReceiveEffect` holds-last-frame, so frames don't pass through 1:1 — a sequence number embedded in frame N may be re-sent 0, 1, or several times downstream. The min/median/max spread the test already reports *is* the jitter signal (it surfaced multi-second outliers on the classic ESP32). To measure true drop/reorder, the firmware would need a sequence-faithful echo path (e.g. a `NetworkReceiveEffect` echo mode that re-emits each received frame 1:1 back to the sender, bypassing the fps re-clock), then the PC could match sent↔received sequence numbers. The test's docstring lists this under "extend later" alongside per-frame sequence matching and the device→device chain.

### Async ArtNet send — decouple the wire from the render tick (PSRAM-only)

The ArtNet send is synchronous: `NetworkSendDriver::loop()` blasts ~97 universes (a 48 KB frame at 128×128) inline, and the per-universe `send()` blocks on lwIP TX backpressure — the netif/EMAC (or WiFi) drivers throttle to wire throughput. Measured on hardware: **~35 ms over Ethernet, ~90 ms over WiFi**, charged straight to the render tick, so ArtNet alone caps the Olimex at ~15 FPS and the S3 (WiFi) at ~7 FPS at 128×128. This is a transport throughput limit, **not** something a non-blocking socket can shed — verified that neither `O_NONBLOCK` nor `MSG_DONTWAIT` makes lwIP return early for UDP (the block is below the socket API; both flags drop zero packets and cost the same ~35 ms). The earlier "non-blocking recovered it to ~2 ms" reading was a transient external condition (the receiver/switch draining the burst freely in one window), unreproducible under steady load with the exact firmware.

The real fix is a **dedicated send task**: `loop()` snapshots the corrected frame into a handoff buffer and signals the task; the send task drains it to the wire at its own pace while the render task continues. The tick stops paying the ~35–90 ms — render runs at its own rate (~30 FPS on the Olimex), ArtNet streams independently at whatever the link sustains.

**Gate this on `platform::hasPsram`. It does not fit non-PSRAM boards at the grid sizes where it would help** — the math is hard:
- The handoff buffer must hold one full frame: **48 KB at 128×128** (16384 × 3), plus a ~4 KB task stack.
- The Olimex (no-PSRAM) at 128×128 runs at **~46 KB free heap, ~18 KB largest contiguous block** (measured). The 48 KB buffer exceeds *both* the total free heap and (by far) the largest block — it can't allocate at all, the same fragmentation cliff the paged MappingLUT just had to work around.
- A double-buffer (so the task reads frame N while render writes N+1) doubles it to ~96 KB — even more out of reach.
- At 64×64 the frame is only 12 KB and *might* fit, but at 64×64 the synchronous send is already fast enough that ArtNet isn't the bottleneck — so the task buys nothing where it's affordable on no-PSRAM.

So the PSRAM gate isn't conservative; it's a hard requirement. PSRAM boards (S3/S2, Olimex-with-PSRAM variants) have megabytes for the handoff buffer via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`; non-PSRAM boards keep the synchronous send and the documented "use Ethernet / smaller grid for high FPS at large grids" guidance ([NetworkSendDriver.md](../moonmodules/light/drivers/NetworkSendDriver.md)).

When implemented: `if constexpr (platform::hasPsram)` (or a runtime `hasPsram()` check) selects the async path; the buffer lives in PSRAM; the send task pins to the core opposite the render task (see [Task core-pinning](#task-core-pinning-backlog)). Non-PSRAM keeps `loop()`'s inline send unchanged. One handoff buffer + a binary semaphore/notification is the minimal shape — don't build a ring of frames until a second consumer needs it.

### `esp32-eth` slow Ethernet bring-up vs `esp32-eth-wifi` (investigation)

On Olimex ESP32-Gateway flashed with `esp32-eth`, Ethernet sometimes takes **a minute or more** to acquire a DHCP lease at boot. The same hardware flashed with `esp32-eth-wifi` brings Ethernet up in seconds. The B1 Idle-recovery fix in `src/core/NetworkModule.h` masks the symptom (status correctly transitions to "Eth: <ip>" once the lease arrives), but the underlying slow bring-up is a real performance regression on the eth-only build.

What we know:
- `build/esp32-esp32-eth/sdkconfig` and `build/esp32-esp32-eth-wifi/sdkconfig` are **byte-identical** (3,617 lines each, `cmp -s` confirms). So lwIP buffer pools, DHCP timeouts, and Ethernet driver settings are the same.
- Same hardware (Olimex ESP32-Gateway Rev G), same RMII pin/clock config (`EMAC_CLK_OUT` on GPIO17), same `ethInit()` code in `src/platform/esp32/platform_esp32.cpp`.
- The only difference at link time: `esp32-eth` passes `EXCLUDE_COMPONENTS=esp_wifi;wpa_supplicant;esp_coex` to ESP-IDF (see `scripts/build/build_esp32.py:31`).
- `esp_coex` (WiFi/Bluetooth coexistence) was an early hypothesis: even though Ethernet doesn't share the radio, `esp_coex`'s init might warm a shared clock path that helps Ethernet auto-negotiation, and the eth-only build excludes it. **Disproven — see below.**

**Firmware is ruled out (the evidence is contradictory across reboots with the *same* build, which by itself proves build-independence).** Over one session, on the same Olimex + cable:
- `esp32-eth-wifi` (keeps `esp_coex`) → flapped: `Ethernet link up` → `link down` repeating, never reached DHCP.
- `esp32-eth` (excludes it) → on one flash **came up immediately and worked**; on a later flash **flapped the same way**.

So the *same* eth-only build both works and flaps at different times, and the eth-wifi build flaps too. The instability does **not** track the firmware build — it's intermittent. That kills the `esp_coex` theory and any WiFi-interference theory. It also confirms our code isn't the cause: `mm_net: Ethernet link up/down` is logged straight from the ESP-IDF `ETHERNET_EVENT_CONNECTED/DISCONNECTED` events (`platform_esp32.cpp:238-243`) — the **PHY hardware reports the drops**; `NetworkModule` only reacts, never stops/restarts the link. Memory is ruled out (boot heap 286 KB, steady 133 KB free / 110 KB block — abundant).

**Signature = physical layer.** When flapping, the link holds for ~2 s, drops, comes back ~10 s later — a repeating cycle, not random. That fingerprints the PHY auto-negotiating, holding briefly, then losing sync: marginal **PHY power, clock, cable, or connector**, not firmware.

**Correlates with board reset.** The flapping tends to *start* right after a flash/soft-reset (this session: a reset preceded each flapping window; a clean power-up tended to link cleanly). Fits the documented slow/flaky PHY re-link on the Olimex after a reset — on some resets the PHY settles, on others it cycles for a long time before (or instead of) holding.

What we still don't know (all **physical** tests — no code change is warranted):
- Does a **clean power-cycle** (vs soft reset) reliably link? (Tests the reset-relink correlation.)
- Does **barrel-jack / stronger 5V** power stop it? (Tests PHY brown-out under USB-only supply, a known Olimex-Gateway weakness.)
- Does **swapping cable / switch port** stop it? (Rules out cable/connector.)

Bottom line: intermittent, build-independent, reset-correlated → a hardware/PHY issue, not a firmware bug. The earlier "slow DHCP at boot" is likely the same root cause (the PHY cycling many times before one window holds long enough to complete DHCP). Pick this up with the physical tests above before touching any code.

### NoiseEffect simplex cost on ESP32 (investigation)

With mirror XY at 128×128, NoiseEffect renders the 64×64 logical quadrant in **~11 ms/tick** on the Olimex (measured) — the simplex math dominates, since the Xtensa LX6 has no FPU and float math is software-emulated. (RainbowEffect on the same pipeline is much cheaper.) This is correct, non-degraded behaviour; it's only worth revisiting if a deployment needs Noise faster than ~11 ms at this grid.

Worth investigating if so:

- **Q16 fixed-point simplex** instead of float (kills the software-float emulation cost).
- **Lower-precision hash** — current simplex uses a 256-entry permutation lookup; a smaller / SIMD-friendly hash may be faster on Xtensa.
- **Strided sampling + interpolation** — render at 32×32, bilinear up to 64×64. Visual quality cost; needs A/B comparison.
- **Inline / unroll the inner per-pixel loop** to keep the simplex state in registers.

None of these are obviously free, and a fixed-point port may shift the visual signature. Defer until there's a real use case — at large grids the tick is dominated by the synchronous ArtNet send (~35 ms), not Noise, so the effect is rarely the bottleneck.

### MoonDeck doc-asset endpoint hardening (backlog)

`scripts/moondeck.py::_serve_doc_asset` accepts any ROOT-relative path and serves the file. Path traversal *is* blocked (`asset_path.relative_to(ROOT.resolve())`), but inside the repo any file is served — including local-only artefacts like `scripts/build/wifi_credentials.json` if present. MoonDeck binds to all interfaces by design (the existing comment in `main()` explicitly enables LAN reach), so anyone on the LAN can hit the endpoint.

Two improvements when this matters:
- **Subdirectory whitelist** — only serve under `docs/` (and image asset paths the markdown renderer needs). Reject `scripts/build/wifi_credentials.json` etc. with 403.
- **Extension whitelist** — only image / CSS / JS mime types via a small allowlist.
- **Optional bind-to-localhost flag** — `--bind 127.0.0.1` for users who don't want LAN reachability. Default stays "" (all interfaces) since the LAN-reach is the documented design.

Not blocking — MoonDeck is a developer tool, not a production server. Pick this up when MoonDeck is in scope for hardening.

### mDNS toggle (evaluate)

Added as a diagnostic tool during performance investigation; testing showed mDNS has zero FPS impact. Evaluate whether to keep (useful for debugging on other boards) or remove (unnecessary complexity). Decide after WiFi performance testing above.

### Static IP on WiFi STA — wire the existing fields to the network (backlog)

NetworkModule exposes `addressing` (DHCP / Static) plus `ip` / `gateway` / `subnet` / `dns` fields, and they persist — but they are **not applied to the WiFi STA interface**. `wifiStaInit(ssid, password)` takes only credentials; the STA always runs DHCP (there is no `esp_netif_dhcpc_stop` + `esp_netif_set_ip_info` on `staNetif_` — that pattern exists only for the AP). So selecting Static and entering an IP currently does nothing: the device keeps its DHCP lease. The fields are display-only scaffolding ahead of the functionality.

Implementing it needs to answer three UX/safety questions (these *are* the spec):

- **When is it applied?** NOT per-keystroke — editing the fields must only update the stored values, never reconfigure the live interface mid-entry (a valid `ip` with a still-zero `gateway` would otherwise be applied and break routing). Apply on an explicit commit — safest is **on next connect / reboot**, not a live switch, because changing the STA IP drops the very connection the browser UI is talking to.
- **Validation before apply.** Require all of ip/gateway/subnet present and self-consistent; reject `0.0.0.0` gateway/ip. If invalid, stay on DHCP rather than half-apply.
- **Warn before a live change.** If applied live (not reboot-deferred), the UI must confirm ("about to change this device's IP to X — you'll need to reconnect at the new address") and surface the new URL, since the current socket dies the instant the IP changes.

Platform work: extend `wifiStaInit` (or add `wifiStaSetStatic`) to take optional ip/gateway/subnet/dns and call `esp_netif_dhcpc_stop` + `esp_netif_set_ip_info` on `staNetif_` when addressing is Static and the config validates. Needs careful hardware testing — a wrong static config locks the device off-network (recovery is the AP-fallback path or a flash erase). Until landed, consider hiding the Static option so it doesn't read as functional.

### Memory ceiling on non-PSRAM ESP32 with eth-wifi (backlog)

On `esp32-eth-wifi`, default 128×128 grid, free heap at boot is ~28 KB — not enough for `esp_wifi_init` (needs ~16 KB RX buffers) after the light pipeline allocates ~210 KB. The device stays running but WiFi init fails silently.

Fix options in increasing scope:
- **Cap the default grid** — drop to 64×64 on `esp32-eth-wifi` (Layer ~32 KB + LUT ~16 KB = 48 KB, comfortably under). Simplest.
- **PSRAM for Layer buffer + LUT** — ESP32-Gateway has 4 MB PSRAM unused on non-S3 builds. Moving the 49 KB pixel buffer + 64 KB LUT out of DRAM frees ~110 KB for radios. Cost: ~25% FPS hit (PSRAM bandwidth ~12 MB/s vs DRAM ~80 MB/s); needs measurement. See [decisions.md](../history/decisions.md) "Adaptive memory allocation design" for the allocation rules.
- **Lazy WiFi init** — skip `esp_wifi_init` when `ssid_` is empty and no AP-fallback is pending. Helps only when credentials exist but the network is unreachable — niche.

### Boot-time buffer degradation on non-PSRAM at 128×128 (investigation)

On the Olimex (no-PSRAM) at 128×128 with a modifier, the Layer sometimes comes up **degraded** at boot — status `"buffer reduced — not enough memory"`, with a visibly wrong render (the reduced render buffer overflows what the LUT/extrude expects). **Toggling any layout control (forcing a fresh `onBuildState`) fixes it** — the rebuild allocates the full buffer and the display is correct. So this is a boot-time allocation *race*, not a code bug: the same rebuild path that fails at boot succeeds moments later.

Measured: the full pipeline needs two ~49 KB contiguous buffers (Layer render buffer + Drivers output buffer, both 128×128×3), plus the LUT. At boot the largest contiguous block is only ~14–20 KB while the network stack / mDNS / HTTP-server buffers are still settling into the heap — so the Layer can't claim a contiguous 49 KB and degrades (halves its dimensions). A rebuild after the heap settles wins the contiguous block and allocates full. The device is at the fragmentation edge either way (~42 KB free / ~14 KB largest block at 128×128 with the full pipeline up).

The annoyance is purely that the device boots degraded and needs a poke to recover — it should come up working. Fix options in increasing scope:
- **Allocation order** — claim the big Layer/Drivers buffers *before* the network/mDNS/HTTP buffers fragment DRAM (i.e. wire the light pipeline's `onBuildState` ahead of network bring-up in `main.cpp`). Cheapest if the ordering is safe.
- **Boot retry** — if `onBuildState` degrades, schedule one more `buildState()` after boot settles (a one-shot, e.g. after the first `loop1s` or once the network reports up). Self-healing without reordering init.
- **Cap the default grid** on no-PSRAM to a size whose two buffers fit the post-boot largest block (same lever as the eth-wifi memory ceiling above).
- **PSRAM for the buffers** on PSRAM-equipped variants — sidesteps DRAM fragmentation entirely (related: the [Async ArtNet](#async-artnet-send--decouple-the-wire-from-the-render-tick-psram-only) and [Memory ceiling](#memory-ceiling-on-non-psram-esp32-with-eth-wifi-backlog) PSRAM notes).

Related: this is the render/output-buffer face of the same non-PSRAM fragmentation cliff the paged `MappingLUT` already addressed for the *LUT*. The buffers themselves still allocate as single contiguous blocks.

### Preview memory optimizations (backlog)

- **Defer Preview allocation** — allocate the preview buffer on first WebSocket client connect; free on last disconnect. Saves ~5 KB when no browser is open.
- **Cap `detail` at 2 by default** — `detail = 3` adds ~14 ms/tick (see [performance.md](../performance.md) "Preview detail cost"). Straightforward guard in PreviewDriver.

### Task core-pinning (backlog)

No FreeRTOS tasks are pinned today. At 16K LEDs the render task takes ~52 ms/tick; if OTA download or Improv scan causes tick-variance spikes, pin render → core 1, OTA/Improv → core 0 (where WiFi already lives via `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`). Defer until contention is observed — neither OTA nor Improv runs during normal operation.

---

## Architecture

### Runtime board presets (multi-commit, partially landed)

The firmware-vs-board separation is now in place across the codebase (see [architecture.md § Firmware vs board](../architecture.md#firmware-vs-board)). `build_esp32.py --firmware <variant>` picks the compiled binary; MoonDeck deduces the physical board where the firmware uniquely identifies hardware (`esp32-eth*` ⇒ `olimex-esp32-gateway-rev-g`) and lets the user pick from a short hardcoded list otherwise. Firmware variants stay separate — `esp32-eth` saves ~670 KB flash + ~30 KB DRAM vs `esp32-eth-wifi` (measured); merging would erase that win.

What still needs separation: the eth variants hardcode Olimex Gateway RMII pins in `src/platform/esp32/platform_esp32.cpp::ethInit()`, so they only work on that one PCB. As we add boards with different pins (LOLIN D32 tested 2026-06-02, QuinLED variants planned), runtime pin configuration becomes the next step.

Pin config moves to runtime (next, separate commit):
- Drop hardcoded `GPIO_NUM_17` from `ethInit()`. NetworkModule reads `Network.eth_rmii_clock_gpio` (new control) and similar pin values, defaulting to current Olimex hardcodes so behaviour is unchanged.
- Same for any other hardware-pin literal in the firmware.

Board preset catalog + upload (later, when the runtime config has real consumers):
- Add structured per-board files (location TBD — not `docs/` since they're config not docs; `boards/` at repo root is the strong candidate, matches the PlatformIO convention contributors will recognise).
- Each file declares chip, flash, PSRAM, Ethernet PHY + pins, default module config.
- New `/api/board-preset` endpoint accepts the JSON; device persists to LittleFS; bootstrap applies pins + defaults on next boot.
- MoonDeck "Set board" picker reads the catalog to populate the dropdown.
- Pin reassignment requires reboot (ESP-IDF can't hot-reconfigure EMAC pins after `esp_eth_driver_install`); document the constraint.
- A first attempt at this catalog landed and was rolled back during the firmware-vs-board separation work — the catalog only earns its keep once the device reads it, otherwise it's a docs-shaped file in the wrong place.

**Prior art — MoonLight's per-board pin database** ([ModuleIO.h](https://github.com/MoonModules/MoonLight/blob/main/src/MoonBase/Modules/ModuleIO.h)). MoonLight (our own project) already models exactly this for ~25 boards across ESP32-D0 / S3 / P4: a `pins[]` array of `{GPIO, usage, index}` plus board-level `maxPower`, `ethernetType`, `ethPhyAddr`, `ethClkMode`. Don't copy the file or paste its tables here — read it when building the catalog and write our own. Its `usage` enum enumerates the hardware functionalities a projectMM board preset *could* drive once the device-side consumers exist (each needs its own module/control before the corresponding `boards.json` / catalog field earns its keep — none exist today beyond `Board.board` + `Network.txPowerSetting`):

- **LED output pins** — per-strip data GPIOs (1–16 outputs/board); the first real consumer (a Driver pin control) unblocks multi-output boards (QuinLED Dig-Quad/Octa, SE16, LightCrafter).
- **Ethernet PHY config** — LAN8720/RMII (MDC/MDIO/CLK/power-pin/PHY-addr/clock-mode) vs W5500/SPI (MISO/MOSI/SCK/CS/IRQ); the consumer is the runtime `Network.eth_*` controls listed above, replacing the hardcoded Olimex pins.
- **Power budget** — `maxPower` (Watts) per board, for a future current-limit / brightness-cap control.
- **Audio / I2S** — SD/WS/SCK/MCLK pins, the input side of audio-reactive effects ([Pi-5 sensor note](#sensor-input-on-raspberry-pi-5--microphone-imu-line-in-post-10-multi-commit) is the desktop counterpart).
- **Buttons & inputs** — push/toggle/lights-on, PIR, digital-input; needs an input-event concept the firmware doesn't have yet.
- **Relays & power control** — relay / lights-on / high-low pins.
- **Infrared** — IR receive pin (remote control).
- **RS485 / DMX** — TX/RX/DE pins (DMX output beyond the current ArtNet path).
- **Sensing** — voltage / current / battery / temperature ADC pins.
- **Onboard LED / key, exposed / reserved pins** — board-housekeeping and conflict-avoidance metadata.

Sequencing rule (unchanged): each functionality lands a device-side control first, then its preset field; the catalog grows one earned consumer at a time, never as a speculative pin dump.

**Module variant + PSRAM within the classic-ESP32 family.** `getChipDescription()` and MoonLight's `ModuleIO.h` both report only the *core* family ("ESP32"), not the *module* (WROOM / WROVER / PICO) — so neither distinguishes whether a classic-ESP32 board has PSRAM. This matters for projectMM (whose large-LED story leans on PSRAM) in a way it doesn't for MoonLight: e.g. the **QuinLED Dig-Next-2 is built on an ESP32-PICO with 2 MB PSRAM**, but projectMM's `esp32` build has no `CONFIG_SPIRAM` (see the `#ifdef CONFIG_SPIRAM` gate in `platform_esp32.cpp::psramAlloc`), so it flashes and runs as a no-PSRAM device and hits the non-PSRAM fragmentation ceiling at large grids that the 2 MB would otherwise relieve. A PSRAM-enabled classic-ESP32 firmware variant (e.g. `esp32-psram`) would unlock it; `boards.json` could then carry a `psram` hint per board to steer the picker — but only once that variant exists (no consumer today). `boards.json` currently maps every classic board to the WiFi-only `esp32` variant, which is correct-but-unoptimised for PSRAM-bearing PICO boards.

### Multi-layer composition (backlog)

`Layers` holds N layers; `Drivers` reads from a single active layer today. Composition is the missing piece — additional layers render their buffers but only the first enabled layer reaches output.

When picked up:
- `Drivers::loop()` blends each enabled Layer's buffer into the shared output using per-Layer blend mode + opacity (controls to add on Layer).
- `Layer::startX/Y/Z` / `endX/Y/Z` (already persisted, currently no-op) become active in `rebuildLUT` — each Layer carves a percentage region of the physical extent.
- Memory-aware allocator at `onBuildState` time decides how many Layers fit and degrades gracefully.
- Persistence already encodes Layers children positionally — adding siblings just works on the file-format side.

### Per-layout coordinate offset for independent placement (backlog)

`Layouts` stitches multiple child layouts into one physical light space, but only their *indices* are stitched (offset sequentially in `forEachCoord`) — their *coordinates* are not translated. Two layouts therefore overlap in the same coordinate box: two 64×64 grids both occupy x,y ∈ 0..63, so the Layer's dense bounding-box buffer is 64×64 (4096 voxels) even though the container reports 8192 lights, and the second layout's lights land on the first's positions. `scenario_Layouts_mutation` documents this (its steps assert pipeline liveness, not buffer-size arithmetic).

When picked up: add `offsetX/Y/Z` (lengthType) controls to `LayoutBase`; `Layouts::forEachCoord` translates each child's emitted coords by its offset so layouts occupy disjoint regions of the physical extent (a 64-wide grid at offsetX=64 sits beside another at offsetX=0 → a 128×64 combined extent). `Layer::onBuildState` already derives physical dims from the max emitted coordinate, so it would pick up the wider extent automatically. Until then, "multiple layouts" means "multiple layouts sharing a coordinate box", which is only useful when they genuinely overlap (e.g. a sphere inscribed in a grid).

### Improv as a child of NetworkModule (deferred — needs scheduler work first)

Architecturally the right shape; attempted in plan-21, reverted. Blocker: `Scheduler::tick()` only walks top-level modules for `loop20ms`/`loop1s` — children silently miss those callbacks. See [decisions.md](../history/decisions.md) "Trying to add a child module to NetworkModule".

Minimum-scope fix before the move:
1. `MoonModule::loop20ms`/`loop1s` propagate to children (or Scheduler walks them) — pick whichever costs less at runtime.
2. Audit every existing override of `setup`/`loop1s`/`loop20ms`/`onBuildControls`/`teardown` in NetworkModule, SystemModule, FilesystemModule, HttpServerModule to confirm base-chaining.
3. Then the actual move is a one-line `networkModule->addChild(improvModule)` swap. Estimate ~2 h total.

### Platform API: `std::span` migration (backlog)

Several `platform.h` APIs still use `(buf, len)` pairs where `std::span` would catch length/pointer mismatches at compile time. Concrete sites: `http_fetch_to_ota`, `improvProvisioningInit`, and friends. ~2 h including ripple updates to callers. Do alongside the next platform-API expansion (Windows socket port or POST /api/firmware streaming).

### Board injection + Improv as a general data injector (multi-commit, partially landed)

Today the **firmware** the device runs is baked in at compile time (`MM_FIRMWARE_NAME`) and self-reported via SystemModule. The **board** the firmware runs on (Olimex Gateway, LOLIN D32, generic ESP32, …) the device cannot self-identify — no readable PCB ID on classic ESP32. MoonDeck deduces it from the firmware where the firmware uniquely identifies hardware (`esp32-eth*` ⇒ Olimex) and otherwise asks the user via a picker; the value lives in `scripts/moondeck.json` on the laptop only. The device's own UI and API have no concept of board.

Goal: get the board key onto the device (persisted, reported via `/api/state`) so it survives between MoonDeck sessions and other clients (HomeAssistant, future MQTT, the device's own OTA-picker compatibility filter) can read it. Then make injection a first-class part of the install flow (web installer + Improv) so end users get the right board key without needing MoonDeck at all.

Builds on existing plan items: see [Runtime board presets](#runtime-board-presets-multi-commit-partially-landed) for the longer-term goal of pin maps / module-config defaults living per-board on disk; this section is the prerequisite — getting the *key* onto the device — that unlocks that work.

**Step 1 — Catalog + device module + MoonDeck push (DONE, commit `8a76be2`):**
- `docs/install/boards.json` is the single source of truth for valid board names. Schema landed as `[{ name, firmwares[], default_firmware }]` — single `name` field (no key/label split; `name` is both identifier and display label).
- New `BoardModule` (code-wired child of SystemModule) carries one `board` Text control with the new `readonly` UI flag (display-only on the device's own web UI; HTTP writes still apply, that's how the injectors push). Persisted to `/.config/BoardModule.json` via the standard FilesystemModule path — no bespoke setter, no bespoke route. Injection is a regular `POST /api/control { "module":"Board", "control":"board", "value":"<name>" }`.
- MoonDeck loads `boards.json` at startup; `_deduce_board` is a catalog reverse-lookup (firmware → unique board, else ""); pushes the picked / deduced value to the device on every discover / refresh / dropdown change (`POST /api/push-board` MoonDeck endpoint → `_push_board_to_device` → device's `/api/control`).

**Step 2 — Web installer board picker (DONE, UX-only):**
- Installer page's `install-picker.js` fetches `boards.json` same-origin at init and renders a board `<select>` above the existing release + firmware selects (opt-in via `enableBoardPicker:true`, default for the web installer; the on-device OTA picker passes `false` because the device already knows its board).
- Picking a board narrows the firmware dropdown to that board's `firmwares[]`, pre-selects `default_firmware` (precedence: board default > localStorage saved > first compatible), and disables the firmware select when only one option remains.
- **No automatic device push.** Original plan called for a post-`PROVISIONED` HTTP fetch to inject the board, but ESP Web Tools 10.x emits `state-changed` on the internal `ImprovSerial` client (inside the dialog's shadow DOM), not as a bubbling DOM event on `<esp-web-install-button>`. Reading the EWT source (`src/install-dialog.ts`) confirmed there's no public event surface for "Improv just succeeded with URL X". (The pre-existing `devices.js` "Your devices" auto-add silently broke for the same reason — kept as best-effort for compatibility with future EWT releases that may re-expose the event.) Step 3 picks up the push on the Improv Web Serial channel — no DOM events, no mixed-content concern.
- Net Step 2 win: end users at the public installer pick "LOLIN D32" first and can't accidentally flash `esp32s3-n16r8` on it. MoonDeck remains the working board-injection path until Step 3 lands.

**Step 3 — Improv RPC injection + full EWT replacement (DONE):**
- Device: `platform_esp32_improv.cpp::improvHandleSetBoard` dispatches vendor RPC `0xFE` (high end of the 0x80–0xFE vendor range). Payload is a length-prefixed UTF-8 board name (1..23 ASCII-printable bytes). Validates inline; on accept publishes via the same producer/consumer pattern as `SEND_WIFI_CREDENTIALS` (atomic ready flag + buffer); `ImprovProvisioningModule::loop1s()` picks it up on the scheduler thread and calls `BoardModule::setBoard()`. Same dirty-flag + debounced-save chain MoonDeck's HTTP write triggers.
- Browser: ESP Web Tools' install button was the blocker (OS-level SerialPort exclusivity + shadow-DOM event isolation). Dropped EWT entirely. New `docs/install/install-orchestrator.js` owns the SerialPort across flash (esptool-js) → WiFi provision (improv-wifi-serial-sdk) → SET_BOARD (raw frame bytes written via `port.writable.getWriter()` — the SDK's `writePacketToStream` is private as of 2.5.0). Custom install modal replaces EWT's dialog.
- Same root cause behind the `devices.js` "Your devices" auto-add — fixed in this commit. `myDevices.addProvisionedDevice(url, board)` now fires from the orchestrator's `onSuccess` callback, populating the bookmark list as designed.
- Vendor RPC dispatcher is the seed for the "Improv as a general data injector" forward-look. Step 4+ additions reuse the same pattern: new command ID + dispatcher case + orchestrator helper.
- The future contributor note: don't naively re-add ESP Web Tools — the orchestrator works because it owns the port. Putting EWT back means giving up Improv RPC injection.

**Step 4 — Catalog grows (only when there's a consumer):**
- Once the device-side runtime board presets work ([Runtime board presets](#runtime-board-presets-multi-commit-partially-landed)) actually lands, `boards.json` entries gain optional `presets` fields (`ethernet.{phy, rmii_clock_gpio, mdio_gpio, …}`, `default_module_config.{Network, Layouts, …}`). MoonDeck pushes the relevant subset alongside the board key via a new `POST /api/system/board-preset` route. Until then, **don't add `presets` fields** — JSON shape grows when a consumer earns its keep, not before.

**Improv as a general data injector (deferred until a second use case lands):**

Step 3's custom RPC infrastructure is the seed. Plausible follow-on injectables: device name override (skip the `MM-CAFE` default), MQTT broker URL (when MQTT module ever lands), static IP, DMX universe assignments, pre-shared API token. **Don't generalise yet** — building a generic key-value Improv injector before there's a second use case is premature abstraction. If two or three more inject-at-install fields land with the same shape, *then* refactor ImprovProvisioningModule into a generic handler that dispatches by RPC command ID to registered callbacks.

**Resolved risks (Steps 1-3 done):**
- ~~HTTP injection only works in dev~~ — Step 3's Improv RPC path works on HTTPS Pages, the dev/prod gap is closed.
- ~~ESP Web Tools' custom-Improv-RPC sending API~~ — EWT doesn't expose ImprovSerial; Step 3 replaced the install button with our own esptool-js + improv-wifi-serial-sdk orchestrator (`docs/install/install-orchestrator.js`). SDK's `writePacketToStream` was also private; raw frame bytes via `port.writable.getWriter()` solved that.
- ~~SET_BOARD command ID collision~~ — picked `0xFE` (high end of 0x80-0xFE vendor range), documented at the definition site in `platform_esp32_improv.cpp` and in `BoardModule.md`. Renegotiable if the spec ever expands into the high vendor range.

**Open follow-up: per-control validator hook on `ControlDescriptor`.** `BoardModule::setBoard()` validates ASCII-printable (rejecting control bytes, embedded NUL); the HTTP `POST /api/control` write path uses the generic `applyControlValue()` in `Control.cpp` which has no per-control validator and writes the raw bytes through. Acceptable today (HTTP-write callers source values from `boards.json` which the project controls), but the right fix is a per-control validator hook on `ControlDescriptor` so any control can declare an inline validation function pointer. Worth doing when the next per-board control with non-trivial input constraints lands, or when the threat model grows (an integration accepts arbitrary external input and POSTs it through). Sketch: `ControlDescriptor` grows a `bool (*validate)(const void*, size_t)` slot defaulting to nullptr; `applyControlValue` calls it before writing and returns `ApplyResult::Malformed` on false; `addText` / `addPassword` get an optional validator argument. Touches ~5 sites; no protocol change.

**Open follow-up: shared JS helpers across device-UI and web-installer.** `safeLocalGet` / `safeLocalSet` (3-line hostile-storage guards) are duplicated in `src/ui/install-picker.js` (device firmware, embedded as a C string via `embed_ui.cmake`) and `docs/install/devices.js` (web installer page, served from Pages). The two live in different build contexts so the shared extract isn't trivial — it'd need a new `src/ui/safe-storage.js` plus updates to: `embed_ui.cmake` (embed the new file), `ui_embedded.h` generator (new C array), HTTP server file routing (new path served), `release.yml` workflow staging, `preview_installer.py` staging. Five files for one 3-line helper is too much pre-merge. Worth doing when the next shared helper arrives — `relativeTime`, `formatBytes`, and the catalog-parse helper (`tryHttpInjectBoard` + `consumePendingBoardParam` share a fetch+find+iterate shape) are candidates. Two helpers earn the build-glue cost; one doesn't.

---

## Sensors and audio-reactive input

### Audio-reactive input on ESP32 — DONE (level + FFT 16-band), with follow-ups deferred

The first audio-reactive capability **shipped**: [MicModule](../moonmodules/core/MicModule.md) reads an INMP441 I²S mic and publishes an `AudioFrame` (sound level + 16-band spectrum + dominant peak) consumed by [AudioVolumeEffect](../moonmodules/light/effects/AudioVolumeEffect.md) and [AudioSpectrumEffect](../moonmodules/light/effects/AudioSpectrumEffect.md). It is a **plain, manual** spectrum: a log/dB display scale with two knobs, `floor` (noise floor) and `gain` (sensitivity). Deferred follow-ups, each its own increment on that base:

- **Adaptive conditioning** — auto noise-floor / auto-gain / smoothing so the display self-calibrates to a room ("sound off → dark, sound on → vivid") instead of being tuned by hand. A self-calibrating version was prototyped and removed; the manual `floor`/`gain` is the shipped baseline. Reinvent from scratch when wanted, and **tune it in a quiet room** — a noisy environment (a strong, varying low-frequency ambient) is the adversarial case that made the prototype hard to settle.
- **Pin auto-scan** — detect the mic's `sdPin` with `wsPin`/`sckPin` fixed (a noise-prompt + confirm convenience); ships today with explicit pin controls.
- **Beat / onset detection** beyond the raw peak; more audio effects (2D / palette-driven frequency-reactive).

### Sensor input on Raspberry Pi 5 — microphone, IMU, line-in (post-1.0, multi-commit)

Audio-reactive lighting (and motion-reactive) is core to what WLED-MM / MoonLight are known for. The Pi 5 is the right host for it: it has the CPU and RAM for real FFT-based audio analysis that the Xtensa ESP32 struggles with, and a full Linux audio + I²C stack. None of this exists today — the codebase has no sensor, audio, or IMU concept, and the Pi currently runs the **desktop** platform backend (there is no `src/platform/rpi/`), which has no hardware access. So this is a domain expansion built on a real platform-backend prerequisite, not a small add.

**Target sensors and their Pi 5 interfaces:**

- **Microphone** — I²S MEMS mic, or a USB audio device read via ALSA. The high-value one: FFT → frequency bands + beat detection drive audio-reactive effects.
- **Line-in** — the Pi 5 has no native analog input, so this is a USB audio interface / DAC HAT feeding the same audio pipeline as the mic; only the source differs.
- **IMU / gyro** — an I²C device (MPU-6050 / 9250-class) on the Pi's I²C bus; tilt / motion → effect parameters.

**How it fits the architecture (the load-bearing part):**

1. **The module category exists — `ModuleRole::Peripheral`.** Peripherals are user-add/deletable children of SystemModule (a gyro `Peripheral` already lands there via the GyroDriver→core move). What's missing for audio-reactive is the *consumption* side: a sensor reads hardware and *produces* values (audio bands, IMU axes) that effects consume — the producer side of the [producer/consumer data-exchange model](../architecture.md#data-exchange-between-modules) (a sensor produces an `AudioFrame` / `ImuState` the way effects produce a buffer that drivers consume). Define the producer struct domain-neutrally so it isn't audio-specific. Today's peripherals are display-only; wiring them into effects is the new work.
2. **All hardware access stays behind the platform boundary.** New `platform::` APIs (e.g. `readAudio()` returning PCM/FFT, `readImu()` returning axes) with the ALSA / I²S / I²C implementation in a real `src/platform/rpi/` backend — which is itself the prerequisite that doesn't exist yet (the Pi uses the desktop backend today). No ALSA/I²C include or call outside `src/platform/`.
3. **Effects consume sensor data the same way they read the layer.** An audio-reactive effect reads the current `AudioFrame` (bands/level/beat) the way `PreviewDriver` reads what `Layer` produces — through a plain data structure wired in `main.cpp`, not a direct hardware call.

**Increments (each a normal domain addition, picked up one at a time):**

1. A real `src/platform/rpi/` hardware backend (GPIO/I²C/I²S/ALSA) — the prerequisite; until it lands, the Pi runs the desktop backend with no sensors.
2. The producer struct(s) (`AudioFrame` / `ImuState`) + the `platform::read*` APIs. (The `Peripheral` role + SystemModule add/delete already exist.)
3. The first audio peripheral — **MicrophoneModule** (canonical, highest value: FFT bands + beat).
4. The first audio-reactive effect(s) consuming it.
5. IMU and line-in slot into the same source-module + platform-API shape afterwards.

Study the proven audio pipeline in MoonLight / WLED-MM (FFT band layout, AGC, beat detection) to inform our own — reference the approach, don't port their code, per [history](../history/README.md) practice. Specs before code: a `MicrophoneModule.md` (and the source-category contract) get written and reviewed before implementation.

---

## HTTP and OTA

### POST /api/firmware — direct binary upload OTA (backlog)

Today `POST /api/firmware/url` covers end-user OTA (device fetches a GitHub release asset). Missing: developer workflow — build HEAD locally, flash over LAN without USB or a public URL.

What to build (~4 h):
- **Platform layer** — implement `ota_begin`/`ota_write`/`ota_end` in [platform_esp32.cpp](../src/platform/esp32/platform_esp32_ota.cpp) wrapping `esp_ota_*` directly (already declared in [platform.h](../src/platform/platform.h), currently bypassed by `esp_https_ota_perform`).
- **HTTP route** — `POST /api/firmware` in [HttpServerModule.cpp](../src/core/HttpServerModule.cpp); streams `conn.read(chunkBuf, 4096)` → `ota_write`; shares `g_otaStatus`/`g_otaPct` with the URL path.
- **`device_ip` control on SystemModule** — lets a script discover the device without mDNS.
- **`scripts/build/flash_over_network.py`** — finds the latest `.bin` in `build/esp32-<board>/`, POSTs it, polls for `rebooting`.
- **UI file-upload affordance** — `<input type="file" accept=".bin">` in a `<details>` expander on the Firmware card.

### HTTP file serving blocks the render tick (backlog)

`HttpServerModule::handleConnection()` serves large embedded files (`app.js`, `style.css`) with the blocking `TcpConnection::write` — a page load can briefly stall `loop20ms`. One-shot per load (lower priority than the per-tick preview issue, which is fixed). Fix: serve large HTTP responses with `writeChunks` (the same non-blocking path used for preview frames).

---

## Effects and preview

### Add real z-axis variation to 2D effects (pending)

Only **NoiseEffect**, **PlasmaEffect** and **RipplesEffect** have z-aware math. The other honest-D2 effects use `Layer::extrude` to duplicate the z=0 plane, so every z-slice is identical on 3D layers. Candidates for genuine D3 promotion: Metaballs/GlowParticles (add z to blob coordinates), Plasma palette/Spiral (add z-driven phase term), Fire (z-drift heat grid), Rings/LavaLamp/Checkerboard/Particles (add z to each element). Prioritise after seeing real 3D installations; each promoted effect also needs its `dynamicBytes` budget for the full 3D buffer.

### Full-density interpolated preview for large layouts (backlog)

The preview index-downsamples a large layout to fit the WS send budget (e.g. 128×128 = 16384 lights → ~1639 sent at stride 10), so the UI shows a sparse sample, not every light. To show **all** lights at their real positions with **interpolated** colours for the unsent ones:

- Decouple the `0x03` coordinate-table density from the per-frame `0x02` stride. Positions are static and sent once, so the table can carry **all** light coordinates (16384 × 3 = ~48 KB one-time — acceptable off the per-frame path, possibly chunked) while the per-frame RGB stays strided to protect ArtNet/the link.
- The browser holds the full position set and, per frame, interpolates each unsent light's colour from its nearest sent neighbours (the sent indices are known from the stride). True positions, guessed colours — better than the removed dense-box block-replicate because positions are exact.
- Open questions: 48 KB one-time table vs `MAX_WRITE_CHUNKS` / send-buffer (needs chunked send or a raised cap, with the same partial-write care as `writeChunks`' drain); interpolation cost on a 16384-point cloud each frame in JS; whether nearest-neighbour or weighted is worth it.

Not simple — own planning pass. Until then the preview is a faithful strided *sample* (correct shape/colour/motion, not per-pixel). A cheap interim (point-size scaled by stride to fatten samples into their cells) was tried and reverted as not what's wanted — it filled the volume but didn't add real points.

---

## Testing

### Additional test coverage (pending)

- **UI page load time** — scenario step measuring HTTP response time for `/`, `/api/state`, `/api/system` via the live runner. Verifies acceptable load time on ESP32.
- **Module teardown memory** — scenario that tears down all modules and verifies heap returns to pre-setup baseline. Confirms no lifecycle leaks.
- **JavaScript test harness** — `vitest` or `node --test` with `jsdom` for pure helpers in `install-picker.js` (`isCompatible`, `parseFirmwaresFromAssets`, `relativeTime`) **and `app.js`'s conditional-control DOM logic** (`syncVisibleControls` — reconciles which control rows are rendered when a `hidden` flag flips). The C++/backend half of conditional controls IS unit-tested (`conditional_controls.h` + per-module tests pin the binding + `hidden` flag), but the **UI re-render half is not** — `syncVisibleControls` was the source of a real re-render-loop freeze (Network static-IP toggle) caught only on hardware. A `jsdom` test that builds a card, flips a control's `hidden`, runs `updateValues`, and asserts the right rows appear/disappear (and that it converges — doesn't re-render every tick) would have caught it. This is now the **second non-trivial JS module** the deferral was waiting for, so the toolchain is more justified than before.
- **Browser-level Improv automation** (deferred) — `scripts/build/improv_smoke_test.py` (added 2026-06-03) exercises the device-side Improv listener over plain serial; what's missing is the browser-side equivalent — Playwright driving Chrome's Web Serial, clicking through ESP Web Tools' install modal, filling the WiFi creds form, asserting `PROVISIONED`. Catches "ESP Web Tools changed its Improv handling in a way that broke our manifest format" failures the serial-only smoke test can't see. Hard to set up reliably (headless Chrome with Web Serial is finicky, needs a wired ESP32 in CI). Pick this up if a regression in the browser flow ever escapes the manual dev-environment test (preview_installer flash-ready mode at <http://localhost:8000/>).

### Live full-suite run leaks state between scenarios (test infra)

`run_live_scenario.py --module all` runs scenarios in sequence against one device, and they share the live tree. Two scenario styles don't compose:
- **Canvas-preparing scenarios** (`scenario_modifier_swap`, `scenario_AllEffects_grid_sizes`) `clear_children` the containers and rebuild, then their cleanup leaves the tree **bare**.
- **Canonical-tree-assuming scenarios** (the older `scenario_GridLayout_*`, `scenario_MoonModule_control_change`, `scenario_NetworkModule_mdns_toggle`) are `mutate` scenarios that expect the boot tree (Grid / Noise / Multiply) to already exist and only tweak it.

Run a bare-leaving scenario before a tree-assuming one and the latter fails pre-flight ("references ids neither on the device nor added by an earlier step"). Each passes **in-process** (fresh tree per scenario — the authoritative gate) and **live individually** (after a clean boot); only the chained live run trips. Not a product bug — a consequence of the "scenarios own their state, no restore" model the canvas-preparing scenarios follow, which the older ones predate.

Fix options: (a) make every live mutate scenario clear+rebuild its own canvas (consistent with the newer ones) so order never matters; or (b) have the live runner reboot / restore the canonical tree between scenarios. (a) is the cleaner long-term shape. Until then, the in-process suite is the gate; live full-suite runs need a clean boot per scenario, or run scenarios individually.

---

## Housekeeping

### ESP-IDF version pinning (pending)

The build IDF is `v6.1-dev-399-gd1b91b79b5` (a dev-branch snapshot, 2025-11-05), which is *ahead* of the latest stable (`v6.0`, released 2026-03) but *behind* the eventual `v6.1` (beta1 2026-06-11, stable 2026-07-31). Being on an unreleased dev branch already cost us once — the missing `ESP_ROM_ELF_DIR` in the post-build gdbinit step (fixed in `build_esp32.py`). **Partly landed:** `setup_esp_idf.py` now carries `PINNED_IDF_COMMIT`/`PINNED_IDF_VERSION` and **warns on drift** (installed HEAD vs pinned) — it can't `checkout` for you (it doesn't own the clone), but a silent `git pull` or a fresh shallow clone landing elsewhere is now visible. **Still to do:** (a) a MoonDeck UI banner / status dot surfacing the same drift (the CLI warning only shows during Setup), and (b) the decision to migrate off the dev snapshot — stay on the pinned commit (chosen for now: it's what all targets incl. P4 were validated against), or migrate to `v6.1` stable (skipping v6.0 since v6.1 is close); migration is a full re-validation pass across classic/S3/P4, a deliberate task, not a pull. Until then: don't `git pull` the IDF. **Schedule note:** the v6.1-stable target of 2026-07-31 is unlikely to hold — v6.0 slipped ~1 month (planned 2026-02-27, shipped late March), and Espressif minor releases historically slip 2-6 weeks on the *final* even when betas land on time. So migrate **to the event** (v6.1 stable actually tagging on the GitHub releases page), not to the calendar date. `v6.0` stable is the lower-risk fallback if the dev-branch warts (already two: `ESP_ROM_ELF_DIR`, API-churn risk) get worse before v6.1 lands.

### ESP32-P4 support — rounds 3-4 (in progress)

Rounds 1 (board + Ethernet-only) and 2 (Parlio LED driver) have landed. Remaining rounds, each its own plan + commit:
- **Round 3 — WiFi via the C6 co-processor.** The P4 has no native radio (`SOC_WIFI_SUPPORTED` absent); WiFi comes from the on-board ESP32-C6 over SDIO via `esp_wifi_remote` / esp-hosted (a managed component, **not in mainline IDF v6.1-dev** — must be pulled). Needs a WiFi abstraction seam so the P4 routes to the remote stack while classic/S3 stay on native `esp_wifi`, plus Improv over the remote path. The C6 SDIO pins on the NANO: CLK 18, CMD 19, D0-D3 14-17, C6 reset 54. Highest-risk round.
- **Round 4 — Parlio loopback + real strip.** A Parlio rx/tx (or RMT-RX-captured) loopback self-test like the RMT/LCD ones, then a real WS2812 strip proven on hardware.

### WiFi runtime disable (backlog)

Compile-time answer already ships: `--firmware esp32-eth` excludes the WiFi stack. This item is the runtime variant — a single `esp32-eth-wifi` binary that skips WiFi init when Ethernet hardware is present. Prerequisite: `platform::ethPresent()` / `platform::wifiPresent()` (listed under Release 2.0 above). Defer until that API lands.
