# troyhacks/WLED — monthly activity digest

What landed on [troyhacks/WLED](https://github.com/troyhacks/WLED)'s `mdev` branch, month by month. External-context reference — a factual log of a friend repo's activity, not projectMM's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](README.md).

This is a personal fork of [MoonModules/WLED-MM](https://github.com/MoonModules/WLED-MM) (the `mdev` branch), so much of the `mdev` traffic is merges from and alignments with the MM and upstream WLED lines; the bullets below pick out what a *user* of this fork would notice. Summarised via the GitHub commits API (all commits on `mdev`, not first-parent merges), filtering out build-number bumps, merge commits, typo/comment churn, and pure refactors. No versioned release is cut from this branch (it tracks `mdev` and ships nightly `mdev` builds), so months are kept whole.

**Branch note — the experiments live off `mdev`.** troyhacks branches heavily: `mdev` is the merge/alignment stream, but the distinctive work happens in named experimental branches (HDMI output, ESP32-P4, W5500 Ethernet, hardware-panel ports, voice control, a pure-IDFv5 port, a new settings subsystem). Those are *experiments*, not necessarily destined for `mdev`, so each month below carries a separate **Experimental branches** line for what moved on them — the frontier of what this fork is probing.

## May 2026

*~18 commits on `mdev`, 2026-05-01 … 2026-05-31.*

- Quieter month on `mdev` — alignment with upstream/MM, smaller effect and build-flag fixes; the heavy lifting was in the Nov–Jan window.
- **Experimental branches:** `P4_experimental` (ESP32-P4) and `M5Stack_Core_S3_Display` both saw work — the two most-recently-touched branches in the repo.

## April 2026

*~53 commits on `mdev`, 2026-04-01 … 2026-04-30.*

- Continued PixelForge (image + scrolling-text interface) refinements and effect/UI fixes.
- Ongoing alignment with upstream WLED and WLED-MM busmanager / segment paths.
- **Experimental branches:** `Olimex_HDMI_Output` (HDMI video output on Olimex hardware) and `T-Display-P4_Experimental` (ESP32-P4 board bring-up).

## March 2026

*~46 commits on `mdev`, 2026-03-01 … 2026-03-31.*

- Effect and 2D-matrix fixes; build-target and partition adjustments.
- More upstream-compatibility alignment in the segment / bus drawing code.
- **Experimental branches:** `Pure_IDFv5_Port` (a from-scratch ESP-IDF v5 port — no Arduino) and `New-Settings-Subsystem` (settings rework with usermod auto-detection) both started.

## February 2026

*~59 commits on `mdev`, 2026-02-01 … 2026-02-28.*

- Audio-reactive receive path hardening continued (sequence checks, packet handling).
- Effect tuning and PixelForge follow-ups.
- **Experimental branches:** `mdev+W5500` (W5500 SPI-Ethernet support layered onto `mdev`).

## January 2026

*~138 commits on `mdev`, 2026-01-01 … 2026-01-31.*

**New**

- **PixelForge** image/GIF tooling gained image rotation; WLED-MM-specific adjustments.
- **RMTHI** (high-speed RMT LED output) now works on ESP32-S2 and S3; new 16 MB ESP32-with-Ethernet build target.
- Random colours via the JSON API (`"col":["r","r","r"]`); Animartrix optional gamma correction + math-optimization speedups.
- Nightly-build automation: automatic version stamping, cleaner release notes, "Nightly mdev Build" titling.

**Fixed**

- DMX output rate-limiting to prevent a watchdog reset; ESPDMX and Philips Hue robustness improvements.
- Audio-reactive UDP: automatic packet drop + improved format/sequence detection; user option to purge the audio queue.
- Always allow the serial console on S3/C3/C6; fix Hub75 removal breaking Hub75 builds.

**Experimental branches**

- `W5500_Support` (W5500 SPI-Ethernet, S3 Ethernet range moved up) and `DF2301Q_Voice_Control` (on-device voice control with hot-plug retry) both active.

## December 2025

*~97 commits on `mdev`, 2025-12-01 … 2025-12-31.*

**New**

- **WLEDPixelForge** — a new image and scrolling-text interface (`pxmagic`), with 1D GIF support, blur option, and version-14.x adaptations.
- Effect math sped up (up to ~3× faster); inlined hot-path colour/segment functions; more segment/effect data allowed on PSRAM boards.
- DDP-over-websockets / DDP-over-WS stability; E1.31 kill switch; `dnrgbw` realtime mode.

**Fixed / hardened**

- Large **preset/ledmap robustness pass**: fixed `presets.json` corruption (mutex protection on the write path), ledmap-parser robustness, reduced UI freeze when updating presets.
- Extensive **mutex / critical-section redesign** across segment and `bus.show` paths — fixes for semaphore leaks, "giving a semaphore never taken", and realtime-lock race conditions.

## November 2025

*~166 commits on `mdev`, 2025-11-01 … 2025-11-30.*

**New**

- **Full Codepage-437 / high-ASCII text support** (UTF-8→UTF-16 decoder) for scrolling text and GIFs.
- Improved 1D GIF support (blur option, bugfixes); `WLEDMM_FASTPATH` enabled for all ESP32 builds; device-ID + version-reporting features.
- WLED-MM branding for the update message box; various Help/README link updates.

**Fixed / hardened**

- HUB75 speedups and DMA-cleanup ordering; 2D drawPixel optimizations; JMap use-after-free fixes.
- USB-mode handling reworked for CDC-on-boot boards (fixes serial breakage and stale-UI-after-update issues).
- ESP8266: dropped the GIF player (too much RAM), reverted to a known-good async webserver.

**Experimental branches**

- A burst of hardware-panel and video-output branches: `ESP32-P4-86-Panel-ETH-2RO` (P4 86-size panel with Ethernet), `HDMI-Experiment` (HDMI output + "maybe faster Art-Net"), and `WaveShare_10.1_Panel` (WaveShare 10.1″ panel).

## October 2025

*~32 commits on `mdev`, 2025-10-01 … 2025-10-31.*

- **DDP-over-websockets** added; HUB75 skips colour-temperature correction for performance.
- `setPixelColor` / `getPixelColor` hot-path optimizations (cached-bus path, `colorKtoRGB` fix, IRAM placement); particle-FX framebuffer memory-calc fix.
- Bugfixes: preset-corruption prevention, IR-JSON buffer-overrun, low-brightness gradient smoothness.

## September 2025

*~15 commits on `mdev`, 2025-09-01 … 2025-09-30.*

- New **Shimmer** effect; reverse-checkmark option for Twinklecat.
- Build process learns to extract the GitHub repo/version into the firmware; 2D-matrix-generator preview fix; AutoPlaylist race-condition fix.
