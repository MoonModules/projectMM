# What to build next

Completed items are removed. This file is deleted when empty.

---

## Distribution

### Release 2.0 — distribution catches up to the source tree

1.0 ships ESP32 firmware (4 variants) + macOS arm64 + Windows x64. Still to add:

- **ESP32-P4** firmware variant — **`esp32p4-eth` (Ethernet-only) shipped**: in `build_esp32.py`'s `FIRMWARES`, the `boards.json` catalog (Waveshare P4-NANO), and CI builds + publishes it to the web installer + releases. **Still to ship: `esp32p4-eth-wifi`** (the C6-WiFi variant) — it doesn't build reproducibly in CI yet (the `CONFIG_WIFI_RMT_*` Kconfig defaults don't survive a plain build without a fresh `set-target`), so it's held out of the release matrix until that's fixed; see [§ ESP32-P4 round 3](#esp32-p4-support--rounds-3-4-in-progress).
- **Linux desktop binary** — third desktop job in `release.yml`, static-linked libstdc++.
- **Teensy 4.1** — toolchain-file build, `.hex` for Teensy Loader.
- **Raspberry Pi** — ARM64, cross-built or native.
- **macOS code-signing** — drops the Gatekeeper "downloaded from internet" prompt.
- **Windows code-signing** — drops the SmartScreen warning on first run of `projectMM.exe`. Same shape as macOS signing; needs an EV / OV code-signing certificate (Microsoft Trusted Signing is the cheapest current option). Until then, the README notes the SmartScreen prompt.
- **Live RMII Ethernet reconfigure** — runtime PHY/pin config shipped (`ethType` + pin controls in NetworkModule, per-board defaults in `boards.json`, `platform::setEthConfig`/`ethInit` dispatch). W5500 (SPI) on S3 applies **live** — `ethStop()` tears down the SPI bus and `ethInit()` re-runs on the next `loop1s()` with no reboot. RMII (classic/P4 internal EMAC) still saves config and asks for a restart to apply, because the EMAC bring-up is fiddlier to hot-cycle cleanly. Make RMII live too: a hot `esp_eth_stop` + EMAC/netif teardown + re-init on config change, matching the W5500 path, so every interface honours the no-reboot principle.
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

This determines the practical LED limit for WiFi-only boards. Until the `sdkconfig.defaults` TX-buffer fix lands (identified in the build-variant table), **prefer wired Ethernet for any ArtNet workload on classic ESP32** — the default `esp32` build carries both stacks, so Ethernet is available even when the original measurements were taken on the old `esp32-eth-wifi` variant.

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

### CI: pin GitHub Actions to commit SHAs (supply-chain hardening)

`.github/workflows/release.yml` references all 9 action types by mutable `@vN` tag (`actions/checkout@v4`, `astral-sh/setup-uv@v3`, `softprops/action-gh-release@v2`, `espressif/esp-idf-ci-action@v1`, …). A mutable tag can be force-moved to malicious code by a compromised publisher; pinning each `uses:` to a full commit SHA (with a `# vN` trailing comment) removes that vector. **Done already (cheaper half):** `persist-credentials: false` on every checkout that doesn't push, so the `GITHUB_TOKEN` isn't left in `.git/config` for later steps to read (the `release` job keeps it — it force-pushes the `latest` tag). **Not done (this item):** SHA-pinning, because it carries an ongoing cost — pinned SHAs go stale and miss security patches, so it only pays for itself **alongside Dependabot** (or a Renovate config) to auto-bump them. Pick this up as a deliberate "CI hardening + Dependabot" pass, not piecemeal. Low risk today: every action pinned is a first-party `actions/*` or a well-known publisher (astral, espressif, softprops), not an obscure third-party action.

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

### Extract shared lane-driver scaffolding when the 3rd parallel backend lands (deferred)

The LcdLedDriver (S3 LCD_CAM i80) and ParlioLedDriver (P4 Parlio) share ~245 of 362 lines, and their platform-side loopback capture+verify is ~100 lines byte-for-byte identical (`platform_esp32_parlio.cpp` even notes "The RX capture half is byte-for-byte identical" to the LCD one). The status-string lifecycle (`failBuf_` / `configErr_` / `clearFailBuf` / `clearConfigErr`) is triplicated across all three LED drivers (RMT/LCD/Parlio), ~60 lines. The branch deliberately extracted the *encoders* (`LcdSlots.h` shared by i80+Parlio, `RmtSymbol.h`, `PinList.h`) on the "extract when the second user lands" rule, but stopped at the lifecycle/loopback scaffolding. **Accepted for this merge** (the reviewer agreed driver-level extraction can wait): the duplication is in mechanical lifecycle/test scaffolding, not domain logic, and a DriverBase-level refactor touching three drivers is riskier than the duplication it removes. **Do it when the third parallel backend arrives** (16-lane widening, or Teensy FlexIO), at which point the pattern is proven three ways: (a) a `detail::` platform helper for capture+verify (the only per-peripheral difference is the transmit call, pass a callback, beside the already-shared `loopbackJumperOk`), and (b) a small owned-status helper or DriverBase members for the fail/config strings. Until then the cost is line count, not correctness.

### 1..8-pin LCD output (future) — would let S3 default to LCD

`LcdLedDriver` requires **all 8** i80 data lanes (`kExactLaneCount = true`, `LcdLedDriver.h`): the ESP-IDF `esp_lcd` i80 bus configures every data line of the bus width and rejects a partial set, so even a few WS2812 strands claim 8 GPIOs. That's why **S3 boards default to `RmtLedDriver`** in `boards.json` (RMT runs one channel per pin, 1..N) rather than LCD — a board with fewer than 8 strips can't sensibly use the LCD driver, and the 8-lane LCD bench wiring (`1,2,4,5,6,7,8,9`) collides with common peripheral pins (e.g. the mic on 4/5/6). A **1..8-pin LCD mode** (drive only the lanes named in `pins`, leave the rest unclaimed — matching Parlio's flexibility) would let the parallel S3 path run any lane count, at which point an S3 board entry could choose LCD vs RMT by intent. Parlio already does this (`kExactLaneCount = false`, 1..8 lanes), so the P4 default *is* the parallel driver. Until LCD gains the same flexibility, S3 stays on RMT by default. Low priority — RMT covers the few-strip S3 case today.

### Classic ESP32 I2S 16-lane parallel LED driver (future) — beyond RMT's 8 channels

The **classic ESP32 has 8 RMT TX channels** (`platform_config.h`: "8 on classic ESP32, 4 on the S3 and P4"), so RMT covers up to 8 parallel outputs on classic ESP32 — e.g. the 8-output QuinLED Dig-Octa runs fine on `RmtLedDriver`. For **more than 8 lanes on classic ESP32**, the established trick drives the **I2S peripheral in LCD/parallel mode** (the hpwit [I2SClocklessLedDriver](https://github.com/hpwit/I2SClocklessLedDriver) / FastLED I2S lineage), clocking out up to **16 lanes** from one autonomous DMA transfer. This is the classic ESP32's high-lane-count path, distinct from the S3 (LCD_CAM → `LcdLedDriver`, plus the [1..8-pin LCD item](#18-pin-lcd-output-future--would-let-s3-default-to-lcd) above) and the P4 (Parlio). No catalog board needs it today (none exceeds 8 outputs), so no board's `planned` list points at it yet; it's the marker for a future ≥9-output classic-ESP32 board. Studied under *Industry standards, our own code* — carry the idea, write our own against the project architecture (host-testable encoder in `src/light/`, peripheral seam in `src/platform/esp32/`). Prior art: hpwit's I2SClockless lineage and FastLED's I2S driver; the same parallel-DMA lineage is already credited in [LcdLedDriver.md § Prior art](../moonmodules/light/drivers/LcdLedDriver.md#prior-art).

### Runtime board presets (multi-commit, partially landed)

The firmware-vs-board separation is now in place across the codebase (see [architecture.md § Firmware vs board](../architecture.md#firmware-vs-board)). `build_esp32.py --firmware <variant>` picks the compiled binary; MoonDeck deduces the physical board where the firmware uniquely identifies hardware (`esp32-eth*` ⇒ `olimex-esp32-gateway-rev-g`) and lets the user pick from a short hardcoded list otherwise. Firmware variants stay separate — `esp32-eth` saves ~670 KB flash + ~30 KB DRAM vs the default `esp32` (WiFi+Ethernet, measured); merging would erase that win.

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

### Audio-reactive follow-ups

The manual level + 16-band FFT spectrum has shipped ([AudioModule](../moonmodules/core/AudioModule.md); what landed and why is in [decisions.md](../history/decisions.md)). These are the deferred follow-ups, each its own increment:

- **Per-band noise-floor (kill a steady single-frequency hum)** — the bench mic picks up a constant ~258 Hz tone (a mains harmonic via the mic/supply) that lights one band even in silence. A high-pass can't remove it (it's well above the ~40 Hz DC-blocker cutoff) without also killing real bass; the clean fix is a per-band adaptive floor that learns each band's idle baseline and subtracts it, so a constant tone in one band gates to dark while the others stay sensitive. Minimal version ≈ 16 floats of state + ~16 ops/frame. This is the next concrete audio step.
- **Adaptive conditioning** — auto noise-floor / auto-gain / smoothing so the display self-calibrates to a room ("sound off → dark, sound on → vivid") instead of being tuned by hand. A self-calibrating version was prototyped and removed; the manual `floor`/`gain` is the shipped baseline. Reinvent from scratch when wanted, and **tune it in a quiet room** — a noisy environment (a strong, varying low-frequency ambient) is the adversarial case that made the prototype hard to settle. (The per-band floor above is the first piece of this.)
- **Adaptive noise gate** — replace the borrowed `squelch`/`floor`-as-gate with a real noise gate: asymmetric bang-bang timing (open fast, close slow), a relative "detect silence" test (thresholds as factors of a learned floor, not absolute sample counts), keying off the RMS envelope we already compute, GEQ/FFT bands left untouched. A softhack007 concept; analysed and judged in full (good idea, industry-standard, but tight on the <30ms budget; decompose into steps rather than overhaul) in [AudioModule.md § Adaptive noise gate](../moonmodules/core/AudioModule.md#adaptive-noise-gate-forward-looking). The recommended sequencing: the per-band floor above is step 1 (its complementary frequency-domain half), the relative-threshold-over-RMS is the cheap high-value cherry-pick as step 2, hysteresis/timing step 3, log-domain + soft-gate optional. Eventually retires the manual squelch.
- **Pin auto-scan** — detect the mic's `sdPin` with `wsPin`/`sckPin` fixed (a noise-prompt + confirm convenience); ships today with explicit pin controls.
- **Beat / onset detection** beyond the raw peak; more audio effects (2D / palette-driven frequency-reactive).

### GyroDriver → core Peripheral move + AudioModule-consistency pass (branched, not merged)

A working **GyroDriver** (MPU6050 IMU over I²C) exists on an unmerged branch (commit `11f8eb7`, "Add GyroDriver (MPU6050) + generic platform I2C layer"); it is not in this branch's tree. This entry reverse-engineers that commit so the move is tracked now. **Verify against the real implementation when the branch merges, then delete this entry.**

What the commit contains (reverse-engineered):

- `src/light/drivers/GyroDriver.h` — reads an MPU6050 over I²C and surfaces five read-only telemetry controls (`gyroX`/`gyroY`/`gyroZ` rates in °/s, `pitch`/`roll` tilt angles). Polls the sensor in `loop20ms()` (50 Hz), formats the display strings in `loop1s()`. WHO_AM_I probe + wake on `setup()`, big-endian 14-byte burst parse, `atan2`-based tilt (no fusion filter).
- A **generic, domain-neutral platform I²C master** (`platform::i2cInit`/`i2cWriteReg`/`i2cReadRegs`, 7-bit addressing) so future sensors reuse it; ESP32 impl on the IDF v6 `i2c_master` driver in a new `platform_esp32_i2c.cpp`, plus an MPU6050-shaped desktop simulation so the UI and host tests see live values without hardware.
- `unit_GyroDriver.cpp` — WHO_AM_I probe, simulated burst parse, control formatting, time-ramp tracking.

The move: it currently masquerades as an input-only **driver** under the Drivers container (a no-op `setSourceBuffer(Buffer*) override {}` is the tell). It belongs as a **SystemModule Peripheral** child, exactly like [AudioModule](../moonmodules/core/AudioModule.md) — both are sensor peripherals that poll hardware and publish read-only telemetry. On the move, make it consistent with AudioModule (the established sibling pattern):

- **Relocate** `src/light/drivers/GyroDriver.h` → `src/core/` and its spec `docs/moonmodules/light/drivers/GyroDriver.md` → `docs/moonmodules/core/`; change `role()` to `Peripheral`; delete the `setSourceBuffer` no-op; rewrite the doc's "input-only driver under the Drivers container" framing.
- **Pin controls + rebuild path.** GyroDriver hardcodes SDA/SCL (`static constexpr` 21/22, with its own "Hardcoded until BoardModule exposes I2C pin mapping" comment). AudioModule already shows the pattern: editable `uint16` pin controls + `controlChangeTriggersBuildState` + a `reinit()` on `onBuildState`. Adopting it retires the hardcoded-pins TODO and satisfies the robustness rule (reconfigure in any order).
- **Lifecycle.** GyroDriver has `setup()` only — no `teardown()`. Add teardown for symmetry with AudioModule's setup/teardown/reinit (the shared I²C bus has little per-instance state to free, so this is consistency, not a leak fix).
- **Document the cadence difference.** GyroDriver polls in `loop20ms()` (50 Hz is plenty for tilt); AudioModule reads in `loop()` every tick because I²S DMA must be drained promptly or it overflows. Both are correct; add a one-line "why this cadence" comment at each so the two siblings aren't "harmonised" into a bug.
- **Wire it** in `main.cpp` as a Peripheral child of System under `markWiredByCode`, the same shape as AudioModule.

Already done on this branch (the reverse direction): AudioModule's two live read-outs were switched from `addText`+`setReadOnly` to `addReadOnly` (the display-only type, matching SystemModule and the way GyroDriver already does it correctly) — so the telemetry idiom is consistent before the gyro branch even lands.

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

- **RipplesEffect in the AllEffects sweep** — `scenario_AllEffects_grid_sizes.json` measures 14 effects but not `RipplesEffect` (added after the sweep was written; its description now says so explicitly). Add a 4-grid-size measurement block mirroring its sibling `RingsEffect` (~300 lines of scenario JSON), then a live run to populate the per-target `observed` blocks. Cheap but mechanical; do it with a device attached so the observations are real, not zero-filled.
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

The build IDF is `v6.1-dev-399-gd1b91b79b5`, a dev-branch snapshot (2025-11-05) ahead of the v6.0 stable but on the unreleased v6.1 line. The version facts (what v6.0 vs v6.1 changed, the release schedule, the 30-month support policy, how to check for a newer tag) live in [building.md § ESP-IDF version](../building.md#esp-idf-version); this entry tracks only the **open decisions** the doc doesn't make. Being on a dev branch already cost us once — the missing `ESP_ROM_ELF_DIR` in the post-build gdbinit step (fixed in `build_esp32.py`). **Partly landed:** `setup_esp_idf.py` carries `PINNED_IDF_COMMIT`/`PINNED_IDF_VERSION` and **warns on drift** (installed HEAD vs pinned) — it can't `checkout` for you (it doesn't own the clone), but a silent `git pull` or a stray shallow clone is now visible. **Still to do:** (a) a MoonDeck UI banner / status dot surfacing the same drift (the CLI warning only shows during Setup), and (b) the migrate-or-stay call — stay on the pinned commit (chosen for now: it's what all targets incl. P4 were validated against), or move to `v6.1` stable (skipping v6.0, since v6.1 is close); migration is a full re-validation pass across classic/S3/P4, a deliberate task, not a pull. Until then: don't `git pull` the IDF. **Schedule note:** the v6.1-stable target of 2026-07-31 is unlikely to hold — v6.0 slipped ~1 month (planned 2026-02-27, shipped late March), and Espressif minors historically slip 2-6 weeks on the *final* even when betas land on time. So migrate **to the event** (v6.1 stable actually tagging on the releases page), not to the calendar date. `v6.0` stable is the lower-risk fallback if the dev-branch warts (`ESP_ROM_ELF_DIR`, API-churn risk) get worse before v6.1 lands.

### Three-level device model: MCU → Board → Device (config provenance)

The model itself is now a shipped design — see [architecture.md § Config provenance](../architecture.md#config-provenance-mcu--board--device) (the three levels + the `txPowerSetting` example + "default only at the level that fixes it"). The catalog that carries it is [`install/boards.json`](../install/boards.json) ([schema](../install/README.md)). **MoonDeck device-profile save/restore is shipped** — capture a device's pin/peripheral config (`/api/save-profile`) and re-apply it after a reflash or to a clone (`/api/apply-profile`), stored per-device in `moondeck.json`. The remaining forward-looking pieces — a `devices.json`/MCU-layer split and annotated-pin images — stay gated by the sequencing rule (no catalog field ahead of a consumer); see the closed [installer-3layer-plan.md](installer-3layer-plan.md) for their status.

### Persistence overlay: partial-save / schema-change audit (backlog)

The absent-key fix (`json::hasKey` guard in `applyControlValue`, so a saved file omitting a key no longer zeroes the control's default — the P4 `ethType` no-DHCP root cause) closed the acute hole. A broader audit would harden the overlay against the rest of the schema-drift surface, now that controls carry meaningful non-zero defaults:
- **Type-change migration.** `ethType` changed `int16` → `uint8` (Select) this branch. A persisted file written under the old type still loads (flat parser reads the number either way), but confirm every type transition is safe — especially Text/Password/IPv4 buffer-size changes and a numeric control narrowing its range (Clamp handles range, but a renamed control silently drops its old value with no warning, unlike `migrateRenamedConfigs` for whole files).
- **Per-control rename path.** `FilesystemModule::migrateRenamedConfigs` only renames whole *files* (by type). A *control* rename within a module has no migration — the old key is silently ignored (now preserved-as-default rather than zeroed, which is better, but the user's saved value is still lost). Decide whether control-level renames need a migration map or are rare enough to accept the loss with a logged warning.
- **Test coverage.** `unit_Control_apply_absent_key` pins the absent-key contract; extend with a type-change round-trip (save int16, load into uint8 Select) and a narrowed-range clamp case so a future schema change can't regress silently.
This is hardening, not a known bug — the shipped fix is correct for the cases that occur today.

### ESP32-P4 support — rounds 3-4 (in progress)

Rounds 1 (board + Ethernet-only) and 2 (Parlio LED driver) have landed. Remaining rounds, each its own plan + commit:
- **Round 3 — WiFi via the C6 co-processor. PARTIALLY PROVEN — C6 link up, STA failover not yet working.** The P4 has no native radio (`SOC_WIFI_SUPPORTED` absent); WiFi comes from the on-board ESP32-C6 over SDIO via `esp_wifi_remote` / esp_hosted. Landed as the `esp32p4-eth-wifi` firmware variant: components pulled P4-only (`rules:` gate in `idf_component.yml`), and `ensureWifiInit()` adds an `esp_hosted_init` + `connect_to_slave` prelude before `esp_wifi_init` (gated on `platform::usesRemoteWifi`). The rest of the WiFi seam is unchanged because `esp_wifi_remote` is API-compatible. A deliberate, documented [v6.0-floor exception](../building.md#esp-idf-version); C6 config via `CONFIG_SLAVE_IDF_TARGET_ESP32C6` + `CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6` + the `CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD` SDIO-pin preset.

  **Hardware results (bench, P4-NANO, 2026-06-12):**
  - ✅ **esp_hosted / C6 SDIO comes up at boot.** `host_init: ESP Hosted`, `H_API: ESP-Hosted starting`, `add_esp_wifi_remote_channels`, `H_SDIO_DRV: sdio_data_to_rx_buf_task started`. No NVS error / assert / panic / hang. Device boots fully (~57-60 FPS), `hasWiFi` true, WiFi controls present. esp_hosted **self-initialises at boot via a constructor** (`ESP_SYSTEM_INIT_FN` → `esp_hosted_init`), so no bring-up code is needed in our platform layer — an earlier explicit `esp_hosted_init` + `esp_hosted_connect_to_slave` prelude was *removed*: init was a redundant no-op and `connect_to_slave` is actually a transport *reconfigure* (slave GPIO-54 reset + SDIO re-init). SDIO config confirmed correct on the wire: `CLK[18] CMD[19] D0[14] D1[15] D2[16] D3[17] Slave_Reset[54]`, 4-bit 40 MHz.
  - ❌ **WiFi STA connect fails on the SDIO re-init.** The cascade DOES fire correctly (`Ethernet no link, cascading` → STA path), but `esp_wifi_init()` (forwarded to esp_wifi_remote) internally triggers `esp_hosted_reconfigure` → `Reset slave using GPIO[54]` → **`sdmmc_card_init failed` (×15) → `card init failed` → `esp_wifi_init failed: ESP_FAIL`**. So: the boot-time SDIO init succeeds, but a **runtime slave reset can't re-establish the SDIO link**. The C6 doesn't come back after the GPIO-54 reset during operation. This is an esp_hosted/SDIO/C6-slave-firmware level issue (reset timing or slave image), below our application code — the pins and Kconfig are correct.

  **Open issues before this is done:**
  1. **Runtime SDIO re-init of the C6 fails — CONFIRMED a C6 slave-firmware problem (not a guess).** SystemModule now exposes a `wifiCoproc` read-only control (via `platform::coprocessorWifi()` → `esp_hosted_get_coprocessor_fwversion()`), and on the bench it reads **`not detected`** — the C6 returns no valid firmware version (0.0.0 / handshake never completes), which is exactly the documented signature of absent / incompatible C6 slave firmware. So this is proven off the device, not inferred. Likely a version mismatch on top of that: The host pulled esp_hosted **2.12.9**; Espressif's P4-Function-EV-Board ships its C6 pre-flashed with esp_hosted slave **v0.0.6**, and the **Waveshare NANO is a different board that may carry a different / absent C6 slave image**. The symptom fits: boot inits the host SDIO master fine, but resetting the C6 (GPIO 54) and re-enumerating it as a slave fails (`sdmmc_card_init failed`) because the C6 has no compatible slave firmware responding. **Primary next step: build + flash the version-matched esp_hosted slave firmware onto the NANO's C6.** The slave project is already vendored at `esp32/managed_components/espressif__esp_hosted/slave/` (`sdkconfig.defaults.esp32c6`, `partitions.esp32c6.csv`); `idf.py create-project-from-example "espressif/esp_hosted:slave"` → `set-target esp32c6` → flash. **Caveat / needs PO + bench hardware:** flashing the C6 on the EV board uses an **ESP-Prog wired to the `PROG_C6` header** with the P4 held in bootloader mode (esp_hosted `docs/esp32_p4_function_ev_board.md` §5.2) — the NANO's C6-flash path must be confirmed (separate USB? equivalent header? ESP-Prog?), and an ESP-Prog may be needed. An OTA slave-update path exists but needs a *working* link first (chicken-and-egg here). This is a hardware-provisioning task, not application code. Secondary fallbacks if firmware-match doesn't fix it: an esp_hosted option to skip the reconfigure/slave-reset when the transport is already up at boot; a slower SDIO freq or 1-bit mode; verify GPIO 54 reset polarity/timing for the NANO. **(Note: EIM — the building.md v6.0-adoption item — does NOT help here; it's a host-machine installer, unrelated to device-side C6 firmware.)**
  2. **Co-processor components no longer compile into `esp32p4-eth` — FIXED.** The gate is now `rules: if CONFIG_MM_P4_WIFI == True` (a Kconfig option declared in `esp32/main/Kconfig.projbuild`, set only by `sdkconfig.defaults.esp32p4-eth-wifi`), so `esp_hosted` / `esp_wifi_remote` are pulled **only** by the WiFi build, never by eth-only. The old `target == esp32p4` gate pulled them into `esp32p4-eth` too; that wasn't merely build-time waste — esp_hosted self-inits its SDIO master at boot, which on the eth-only build interfered with the EMAC bring-up (a red herring chased during the P4 no-DHCP hunt). The eth-only image dropped 1.36→1.12 MB once gated out. The `wifiCoproc` read-out stays compile-gated on `platform::hasWifiCoprocessor` (`isEsp32P4 && hasWiFi`).
  3. **Build reproducibility.** `build_esp32.py` does not yet build this variant reliably: the C6 slave-target Kconfig `default ... if IDF_TARGET_ESP32P4` only fires on `set-target`, and the reconfigure a plain `build` triggers drops it back to ESP32-H2 (no WiFi) → fails on missing `CONFIG_WIFI_RMT_*`. A clean manual sequence works (`rm -rf <build dir>` → `set-target esp32p4` → `build`, all with the same `-DSDKCONFIG`/`-DSDKCONFIG_DEFAULTS`); the wrapper needs a fix so the auto-default sticks across reconfigures (see the KNOWN ISSUE comment in `build_esp32.py`).
- **Round 4 — Parlio loopback + real strip.** A Parlio rx/tx (or RMT-RX-captured) loopback self-test like the RMT/LCD ones, then a real WS2812 strip proven on hardware.

  **Dev-loop note — reading the P4's runtime log over USB.** The P4-NANO's primary console is **UART on GPIO 37/38** (`CONFIG_ESP_CONSOLE_UART_DEFAULT`), not the USB port, so `ESP_LOGI` / `mm_net` lines are *not* visible over `/dev/cu.usbmodem*` by default — only the ROM boot banner and `std::printf`-to-stdout (which routes to the **secondary** USB-Serial-JTAG console) come through. Two workarounds when you need the runtime log over USB: (a) temporarily set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (note the JTAG endpoint re-enumerates when the app starts, so a reader must reconnect across the drop — `idf.py monitor` handles it; a plain fixed `pyserial` handle dies); or (b) hang a USB-UART adapter on GPIO 37/38. This cost real time during the P4 no-DHCP hunt; the fastest signal there turned out to be a `printf` of the runtime struct (stdout → secondary JTAG console) plus a `git worktree` bisect (build an old commit, flash, check LAN reachability) to prove code-vs-hardware without needing the log at all.

### Drop the i80 WR/DC sacrificial pins (S3 LcdLedDriver) via direct LCD_CAM

The S3 i80 LED path costs **two GPIOs the LEDs never use**: the IDF `esp_lcd` i80 bus hard-requires a WR (pixel clock) and a DC pin on real GPIOs (`esp_lcd_panel_io_i80.c`: `wr_gpio_num >= 0 && dc_gpio_num >= 0`), even though WS2812 strands ignore both. Today `LcdLedDriver` keeps overridable defaults (clockPin=10, dcPin=11) — peripheral-required, not user-strand wiring, so a default cannot do harm. **Two ways to reclaim the pins, neither trivial:**
- **Cannot reuse a data pin for WR/DC.** A GPIO carries exactly one peripheral signal (`esp_rom_gpio_connect_out_signal` binds data_sig[i] / wr_sig / dc_sig each to its own pin); routing WR onto a data lane would clock the *clock* waveform onto that strand instead of its colour bytes. WR/DC must be distinct *physical* pins from the 8 data pins. (You CAN already point them at any otherwise-free or unstrapped GPIO via the controls — that's the "reuse a pin you're not using" answer; it's the *spare* pin you avoid, not a data pin.)
- **Zero WR/DC pins needs bypassing esp_lcd** and driving the LCD_CAM peripheral's registers directly (hpwit's I2SClockless approach — legacy parallel mode has no DC concept and emits WR without a dedicated config pin). That's the only path to 8-pins-total on the S3. Cost: leaving the recognisable IDF `esp_lcd` API for register-banging (a *Common patterns first* hit), re-proving the driver bit-perfect on hardware (the loopback self-test is the proof). Benefit: 2 GPIOs back on a tight S3 board. Its own increment, not a pin-default tweak. Parlio (P4) already needs no extra pins (`clk_out_gpio_num = GPIO_NUM_NC`), so this is S3-i80-only.

### LCD/Parlio DMA frame buffer → PSRAM (free internal SRAM for big frames)

For driving **lots of LEDs**, internal SRAM is the scarce resource and the parallel-driver DMA frame buffer is the biggest consumer (8 lanes × lights × outCh × 24 slot-bytes + latch pad). Today both parallel drivers allocate it as `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` (`platform_esp32_lcd.cpp`, `platform_esp32_parlio.cpp`) — **internal SRAM only**, so a large frame can exhaust DRAM while PSRAM sits unused. The IDF confirms both peripherals' GDMA **can burst straight from PSRAM** on the S3/P4: `esp_lcd_panel_io_i80.c` sets `access_ext_mem = true` and itself allocates the buffer with `MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA` when asked; `esp_driver_parlio/src/parlio_tx.c:158` sets `access_ext_mem = true  // support transmit PSRAM buffer`. (RMT already does the right thing — its symbol buffer goes through `platform::alloc`, which is PSRAM-first with an internal fallback.)

**The change:** allocate the LCD/Parlio buffer `MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM` first, falling back to internal when PSRAM is absent/full, using the **external-memory alignment** the IDF requires (`gdma_get_alignment_constraints` → `ext_mem_align`, typically the cache line — larger than the current 64-byte internal alignment) and keeping the buffer cache-aligned + its size a multiple of that alignment. **Why it's its own increment, not this commit:** it changes the proven hot DMA path, PSRAM DMA has real caveats (cache-line alignment, write-back/coherence on the encode→DMA handoff, and lower PSRAM bandwidth that the IDF guards with a CPU-MAX DFS lock during transmit), and it **must be re-proven on S3 + P4 hardware** (the loopback self-test bit-verifies it, then a real strip). Measure the bandwidth headroom too: a very wide, long frame at speed may want internal SRAM regardless. Scope: the two `heap_caps_aligned_alloc` sites + their `bufferBytes` alignment rounding + the capacity check; no domain-code change (the encode loop already writes through `dmaBuf_`).

### WiFi runtime disable (backlog)

Compile-time answer already ships: `--firmware esp32-eth` excludes the WiFi stack. The default `esp32` already *cascades* — `ethInit()` runs first, WiFi only comes up if no PHY responds — so a wired board never associates over WiFi. What's still missing is reclaiming WiFi's **heap**: even when Ethernet wins the cascade, `esp_wifi_init`'s RX buffers stay allocated. This item skips that init entirely once Ethernet is up, freeing ~16 KB. Defer until the heap saving is worth the teardown-ordering risk.
