# NightDriverStrip — monthly activity digest

What landed on [NightDriverStrip](https://github.com/PlummersSoftwareLLC/NightDriverStrip)'s `main` branch, month by month. External-context reference — a factual log of a friend repo's releases, not projectMM's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](README.md).

Summarised via the GitHub commits API (no local clone), so counts are all commits on `main`, not first-parent merges — the bullets filter out dependency bumps, whitespace, and pure refactors. The one release in the window, **v1.3.0**, was published 2026-01-10 but tagged from a late-November commit; it isn't a clean month boundary, so months are kept whole with the release noted as context.

## June 2026 (up to v2.0.0)

The big one: **NightDriverStrip 2.0.0** shipped on June 14 — a major release cut from `main`.

**New**
- Brand-new Web UI replacing the old one, plus a new browser-based web installer for flashing devices.
- Runtime-selectable output drivers: settings that used to be compile-time (like strip type) can now be changed on the device without rebuilding.
- APA102 / SK9822 strips are now a first-class output type alongside WS281x and HUB75.
- Multi-layered, categorized device settings structure.
- New optional WiFi-activity output pin that goes HIGH while WiFi is drawing; projects can now define zero active effects and stay idle until then.
- Effect-timeout reset on a remote-control effect switch is now a user setting (previous always-reset behavior stays the default).
- Effects reworked (weather, stocks, subscribers, etc.) to display usefully on short 48x16 matrices.
- New and revised effects, plus smarter memory handling (targeted mix of internal RAM and PSRAM).

**Fixed**
- Render hiccup/stutter when settings auto-saved: SPIFFS/JSON writes no longer block the render thread.
- Effect-manager crash shortly after load when the effect set changed.
- Serpentine-matrix visualization corrected.
- Lower COLORDATA server framerate optimized for smoother remote color streaming.
- PSRAM-related instability during flash/cache writes reduced by keeping save-time JSON in internal RAM.

## June 2026 (post-v2.0.0)

- **v2.0.1** (also June 14): patch that fixes the web installer failing to build for the 2.0.0 release (removed a stale project entry).
- Stock-ticker effect now shows correct live data, fetched through the new V2 API.
- Fixed 64x32 (wide-and-short) displays that were rotating and doubling their content instead of scaling — effect previews in the Web UI and CLI now match the active output driver's pixel mapping (issue #878).

**Watching**
- Issue #877 ("networking seems broken") drew heavy discussion (38 comments) around the WebUI being unreachable after a recent merge on some setups; closed in June.

Auditability: ~40 first-parent merges/commits on `main` with author-date in 2026-06-01..2026-06-30 (`commits?sha=main&since=…until=…`); two versioned releases published June 14 — v2.0.0 (commit ce00eaa) and v2.0.1 (commit 835015b), both ancestors of `main`, so the month is split at v2.0.0. Issues checked via `search/issues` for `created:2026-06-01..2026-06-30` (0 opened in range) and `closed:2026-06-01..2026-06-30` (#877, #878, #825 closed; #878 and #877 user-facing).

## May 2026

*~75 commits on `main`, 2026-05-01 … 2026-05-31.*

**New**

- **RGBWW / SK6812 white-channel support** — W/WW helpers, a WarmGlow test effect, configurable SK6812 white extraction; colour-temperature naming cleanup.
- **New Setup Wizard / guided-installer WebUI** — and a push to make the UI a "non-special consumer": everything the official UI needs now comes from the firmware over the wire (spec/schema), not baked into `app.js`.
- M5 Stick S3 support (IR + WS2812B); optimised WS2812B draw path; `ACTIVITY_PIN` support; allow zero effects in the table.

**Fixed / hardened**

- Major **PSRAM strategy reversal** — switched to PSRAM-default routing (threshold 96, Mesmerizer's proven value) and removed the bespoke `psram_allocator` family; JSON save-path now uses internal RAM to avoid touching PSRAM during flash/cache-disabled windows.
- Several concurrency and memory-safety fixes across buffer / drawing / network / task paths; PolarMap and noise-generation bugs; weather-data state protection.

## April 2026

*~60 commits on `main`, 2026-04-01 … 2026-04-30.*

**New**

- **New local WebUI** with a custom LED RMT output driver, dynamic settings, and a WS2812B output driver; first beat-detection / audio-architecture modernization pass.
- OTA: successfully cancels rollback on boot; unified hardware identity via eFuse.

**Fixed**

- LED-buffer crash (#842); socket race condition + NTP init issues; nullptr deref polling MAC before the C6 companion is ready; Cube/Noise matrix-effect refactors; PSRAM cache-issue partition correctness; hexagon build.

## March 2026

*~32 commits on `main`, 2026-03-01 … 2026-03-31.*

- **Replaced the RemoteDebug dependency** with a custom Logger + Telnet server — plus WiFi-stability and hardware-safety improvements; fixed a DebugCLI use-after-free for active telnet sessions and a stack-corruption (FD_SETSIZE) in the telnet sink.
- New Arduino-V3 partition layouts (standard, NOOTA, 8MB).
- Otherwise dependency bumps and include hygiene.

## February 2026

*~13 commits on `main`, 2026-02-01 … 2026-02-28.*

- **Replaced the IRremoteESP8266 library with a native RMT IR decoder** (Key44 remote support, string IR-code mapping).
- FFT implementation optimised (moved to member, weights enabled); SoundAnalyzer implementation moved to its own source file.

## January 2026

*~60 commits on `main`, 2026-01-01 … 2026-01-31. (**v1.3.0** was published Jan 10.)*

**New**

- **Fuzzy effect selection** (CLI + firmware) with tab completion and brightness nudges; text-scrolling improvements.
- Smoke effect + noise-calculation precision improvements.
- Python client (`nightdriver_client`): layout mapping + auto-detection, PNG-sequence and contact-sheet output, fuzzy effect resolution.

**Fixed / build**

- Worked around an ESPAsyncWebServer macro/symbol conflict (renamed `STR` macros, adapted pattern subscribers); replaced the UrlEncode library with a local implementation; many `ENABLE_WIFI` / `ENABLE_NTP` guard fixes so non-networked builds compile.

## December 2025

*~50 commits on `main`, 2025-12-01 … 2025-12-31.*

**New**

- HUB75: horizontal scrolling for long effect titles; QRCode + UrlEncode support.
- Decoupled `EFFECTS_MATRIX` from the SmartMatrix dependency; single `graphics_lib` build expansion; `nightdriver_client` added with auto-brightness and gain for dim displays.
- Better serial-port interactivity + enhanced CLI debugger; MeteorEffect refactored to a modern struct-of-arrays.

**Fixed**

- StarEffect works with silent/no-audio; `FillGetNoise` scaling/centering on non-square matrices; GCC 14 map-allocator constness; Apple5x7 font linker error; merged ESP-IDF4/5 audio layers.

## November 2025

*2 commits on `main`, 2025-11-01 … 2025-11-30.*

- Quiet month — a dependency bump (js-yaml). (The v1.3.0 tag was cut from a late-November commit but published in January.)

## October 2025

*2 commits on `main`, 2025-10-01 … 2025-10-31.*

- Quiet month — a dependency bump (vite).

## September 2025

*~35 commits on `main`, 2025-09-01 … 2025-09-30.*

**New**

- Effect **visualizer synced to effect output** with improved FPS calculation; unified effects between Mesmerizer and M5StackDemo; Spectrum2 build uses the full matrix effects.
- One bitmap shared across all snowflakes; GFX classes renamed; HUB75 now implies MATRIX.

**Fixed**

- Dimension-overflow prevention; redundant-clear fix; toggle-button double-definition warnings; timezone file updates; release preparation.
