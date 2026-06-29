# Backlog — index

The forward-looking half of the docs (the backward-looking half is [`../history/`](../history/)). This folder is **not** present-tense and agents don't read it automatically — only when planning new work. See [CLAUDE.md § Documentation](../../CLAUDE.md) for how `backlog/` and `history/` relate.

This README is the **landing page**: the rest of the system links here, not into individual items, so the present-tense docs stay present-tense.

## The to-build list

Split along the codebase's own boundary (`src/core/` vs `src/light/`), with a third file for items that genuinely span both:

- **[backlog-core.md](backlog-core.md)** — core / infrastructure: distribution + platforms, ESP32 performance & memory, network & persistence, HTTP/OTA, architecture, testing, housekeeping, and UI.
- **[backlog-light.md](backlog-light.md)** — the light domain: LED drivers (architecture + deferred increments), LCD/DMA driver work, effects & preview, and sensors / audio-reactive input.
- **[backlog-mixed.md](backlog-mixed.md)** — cross-domain items where a core mechanism interacts with a light driver/effect/modifier.

Completed items are removed; a file is deleted when empty (per [*Mandatory subtraction*](../../CLAUDE.md#process-rules)). Tags in item titles: *(investigation)* = needs measurement before a fix · *(backlog)* = scoped but not started · *(deferred)* = waiting on a prerequisite · *(future / long term)* = directional.

## At a glance

A map of everything in the three files, by theme.

### Core ([backlog-core.md](backlog-core.md))

- **Distribution** — remaining platforms (Linux, Teensy, RPi), code-signing (macOS/Windows), live RMII Ethernet reconfigure, installer UX polish, P4 DHCP-hostname recheck, S31 web-flash (waiting on esptool-js); DevicesModule discovery growth (HTTP probe off the render task, more mDNS types + UDP, deterministic scan scenario).
- **ESP32 performance & memory** — E1.31 multicast (IGMP), WiFi ArtNet perf matrix, async ArtNet send (PSRAM-only), network round-trip drop/reorder test, slow eth bring-up, non-PSRAM memory ceiling + boot-time buffer degradation, task core-pinning; ops: static IP on STA, mDNS toggle, MoonDeck doc-asset hardening, CI SHA-pinning.
- **Architecture** — disable-releases-resources, cross-module pin-uniqueness check, Improv-child-of-NetworkModule, `std::span` platform API, Improv-as-REST follow-ups, **live scripting** (on-device authored effects/layouts/modifiers/drivers/sensor logic — design phase, see the bottom-up survey); composition/config: runtime board presets, per-layout coordinate offset.
- **HTTP & OTA** — direct binary-upload OTA, HTTP file serving off the render tick.
- **Testing** — additional coverage (UI load time, teardown memory, JS harness), live full-suite state leak.
- **Housekeeping** — WS-send socket-pair fixture, ESP-IDF version pinning, three-level device model, persistence-overlay audit, **ESP32-P4 rounds 3-4 (in progress)**, WiFi runtime disable.
- **UI** — deferred-to-1.x items, open design questions (multi-layer UI, modifier-chain viz, presets, node-graph), and the v1 gap analysis.

### Light ([backlog-light.md](backlog-light.md))

- **Drivers** — extract shared lane-driver scaffolding (on the 3rd backend), 1..8-pin LCD output, classic ESP32 I2S 16-lane driver.
- **LED drivers — deferred** — sigrok flicker cross-check, core-1 driver task, fuller RMT error handling, per-driver buffer window, 16-bit/dither, moving-head preview interpreter.
- **LCD / DMA driver work** — drop the i80 WR/DC sacrificial pins, LCD/Parlio DMA buffer → PSRAM.
- **Effects & preview** — real z-axis in 2D effects, full-density interpolated preview, self-describing frame header, RGBW preview, fixture model (moving heads/beams), extract the resumable transport.
- **Sensors & audio-reactive input** — audio follow-ups (per-band noise floor, adaptive gate), GyroDriver → core Peripheral move, Raspberry Pi 5 sensor input (mic/IMU/line-in).

### Mixed ([backlog-mixed.md](backlog-mixed.md))

- MultiplyModifier mapping-LUT memory at large grids; intermittent ~0.5 s RMT LED pauses; NoiseEffect simplex cost on ESP32.

## In-flight draft specs

A spec for a not-yet-built module can live here as a plain draft `.md` (alongside the design studies below) until the module ships — at which point its final spec is written in [`../moonmodules/`](../moonmodules/) and the draft is deleted. None are in flight right now.

## Design studies

One-off research documents that informed a future direction, kept for the reasoning rather than as living specs.

- [leddriver-analysis-top-down.md](leddriver-analysis-top-down.md) — reasons from the end goal (driving WS2812-class LEDs from a GPIO pin) toward a generic driver architecture, per-platform implementation, and a testing strategy.
- [leddriver-analysis-bottom-up.md](leddriver-analysis-bottom-up.md) — the companion landscape survey: catalogues the existing LED-driver libraries across ESP32, Teensy, Raspberry Pi, and PC, and recommends a path.
- [livescripts-analysis-bottom-up.md](livescripts-analysis-bottom-up.md) — live scripting (run user-authored effects/layouts/modifiers/drivers/sensor logic on-device without a reflash), Stage-1 survey. Deep-reads the ESPLiveScript fork (hpwit's native-Xtensa JIT), surveys the field (ARTI-FX interpreter by ewowi, embedded VMs, WASM/WAMR), and records the product-owner direction.
- [livescripts-analysis-top-down.md](livescripts-analysis-top-down.md) — the Stage-2 redesign: a native-codegen engine, Xtensa-first behind an IR seam (WASM/WAMR the per-target fallback), a C-subset language that ports an effect near-verbatim, the MoonModule binding, and a staged spike plan along the MoonLight effects-tutorial ladder.

## Project transition

- [rename-to-moonlight.md](rename-to-moonlight.md) — the phased plan to rename **projectMM → MoonLight** (and move the predecessor MoonLight to a personal repo). Now / coming-time / during-the-switch sequencing around the repo-name collision, the externally-visible references that gate the cutover (binary name, OTA URLs, mDNS identity), and a MoSCoW of the feature gaps that must close before the new name isn't a downgrade.
