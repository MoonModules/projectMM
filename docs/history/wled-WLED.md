# WLED (upstream) — monthly activity digest

What landed on [wled/WLED](https://github.com/wled/WLED)'s `main` branch, month by month. External-context reference — a factual log of a friend repo's releases, not projectMM's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](README.md).

Months are **not** split at release dates: upstream WLED cuts releases from separate release branches (`0_15`, `16_x`), so the version tags aren't on `main` — `main` is the development trunk that feeds future releases. Each month notes which release shipped, as context.

## May 2026

*Summarised from 72 first-parent commits on `main`, 2026-05-01 … 2026-05-31. (Trunk after the **v16.0.0** release on May 3; v16 is codenamed "Kagayaki".)*

**New / boards**

- HUB75: FM6124 driver for 4-scan panels and the 64×64 limit removed on PSRAM boards; new ESP32-S3-without-PSRAM build environments; option to build against the Espressif framework instead of Tasmota.

**Fixed / hardened**

- Effects: restored palette wrap in `color_wheel()` (a regression since 0.15.x), Twinkle fixes, Dissolve "Complete" same-colour-as-background fix, gravity audio-reactive top-LED fix.
- Audio-reactive auto-suspends in realtime modes (but stays on with "use main segment only").
- DDP and all realtime protocols: relaxed-but-safer header acceptance + bounds checks; Improv/UDP parsing hardened; `/reset` auth clarified.
- Auto-migration for legacy sunrise/sunset config; animated-staircase inverted-PIR support.

## April 2026

*Summarised from 67 first-parent commits on `main`, 2026-04-01 … 2026-04-30. (v16.0.0-beta was tagged April 11.)*

**New / effects**

- Game of Life fix; FPS bump via a fast path in `blendSegment`; better packet queuing/pacing for custom-palette live preview.
- PixelForge palette/tools list moved into the repo; fxdata serialized without ArduinoJSON (smaller/faster).

**Fixed**

- Critical Candle-FX bug + Flow-FX integer issue; segment inputs no longer restrict trailing strips; iOS blending-style list filter; DDP flag-bit masking for compatibility; robustness rewinding file pointers before writes.
- (Otherwise a heavy AI-contributor-guideline / docs month.)

## March 2026

*Summarised from 65 first-parent commits on `main`, 2026-03-01 … 2026-03-31. (v0.15.4 shipped March 14 from the 0_15 release branch; trunk version bumped to 17.0.0-dev.)*

**New / effects**

- **Full FastLED replacement** merged (#4615) — WLED's own colour/math instead of the FastLED dependency.
- Many new user_fx effects: Spinning Wheel, Color Clouds, Lava Lamp, Magma, Ants, Morse Code, Comet (fire particle system), a slow >4-hour transition FX, Tetris line-clear flash.
- Scrolling-text FX gains custom fonts + international UTF-8; stencil blending mode; ESP32-C3 audio-reactive (DSP FFT + integer math); more macro/timer slots; longer max playlist duration.
- OTA update page restyled (auto-sets download URL from `info.repo`); clearer UI tool icons.

**Fixed**

- Segment-index misalignment; hostname/DNS cleanup; hue preservation in colour fade; array-bounds on short WS payloads; DDP rejects unsupported/non-display packets.

## February 2026

*Summarised from 35 first-parent commits on `main`, 2026-02-01 … 2026-02-28.*

**New**

- **Version scheme changed to Major.minor** (dropped the leading "0."), heading toward v16; bumped to 16.0.0-alpha.
- New **Pin Info** page (used/available pins overview); UI settings readability improvements.
- Improved bus handling — free choice of bus driver in any order, better memory calculations; gamma lower-limit removed (enables inverse gamma correction, applied to segment brightness too).
- Extended CCT blending (exclusive blend, colour-jump fix); full WiFi scan with BSSID apply; new ESP32-S3 8MB QSPI build; experimental ESP32-C5/C6 in the node list.

**Fixed**

- LED animations briefly pausing at bootup (ESP32); boot-up WiFi pause with extended scanning; removed dangerous mutex macros in the bus manager; Flow-FX flow at segment start/end.

## January 2026

*Summarised from 38 first-parent commits on `main`, 2026-01-01 … 2026-01-31.*

**New**

- **New custom-palettes editor** (#5010); WPA-Enterprise WiFi support; random per-LED colours via JSON API; option to save unmodified presets to autosave; PixelForge GIF image rotation.
- Removed the MAX_LEDS_PER_BUS limit for virtual buses; new ESP32 node types; JSON validation + minify on file upload in the UI.

**Fixed**

- Relay not turning on at boot; GPIO0 always grabbed by a button; gamma correction on a fresh install; Ethernet static-IP ignored; HUB75 improvements; config exceeding the LED limit.

## December 2025

*Summarised from 39 first-parent commits on `main`, 2025-12-01 … 2025-12-31. (v0.15.3 shipped December 4 from the 0_15 release branch.)*

**New / effects**

- **PacMan effect** and the new **"WLEDPixelForge"** image & scrolling-text interface (#4982); dynamic LED-type dropdown; improved 2D particle collisions; sequential UI-resource loading.
- Usermod Temperature uses full 12-bit precision; "peek" shows gaps; removed legacy EEPROM support.

**Fixed**

- FX checkmark sync; UI TypeError with a custom palette; rotary-encoder palette-count off-by-one; particle-collision binning; segment overflow.

## November 2025

*Summarised from 46 first-parent commits on `main`, 2025-11-01 … 2025-11-30. (v0.15.2-beta1 → **v0.15.2** shipped Nov 9 / Nov 29 from the 0_15 release branch.)*

**New**

- **New file editor** (#4956) with ctrl+S, toasts, efficient ledmap reading, 0-byte-file handling.
- **DDP over WebSocket**; improved 1D GIF support with blur option; variable button count up to 32; Dissolve "Complete" mode (always fades fully).
- Aurora FX speedups; better PSRAM-MB usage reporting; bootloader offsets for C3/S3 + variable bootloader sizes per MCU; Adafruit board partitions.

**Fixed**

- Stale UI after firmware updates; AP-not-showing (default channel tweaks); device-fingerprint crashes; `millis()`-rollover robustness in wait logic; OTA update for C3 from 0.15; ESP8266 low-heap and DMA fixes.

## October 2025

*Summarised from 9 first-parent commits on `main`, 2025-10-01 … 2025-10-31.*

- Quiet month on trunk. **DDP over WebSocket** groundwork (shared WS connection in common.js); Twinkle blank-area fix; low-brightness gradient "jumpyness" fix; bootloop-tracker safety check; GIF-player inactive-segment + copy-FX bugfixes.

## September 2025

*Summarised from 41 first-parent commits on `main`, 2025-09-01 … 2025-09-30.*

**New / effects**

- **Shimmer FX** added; "unrestricted" number of custom palettes; center-bin selection for 2D GEQ; Twinklecat reverse option; speed optimisations + `restoreColorLossy` fix.
- Heap-memory and PSRAM handling improvements; HUB75 AC fixes.

**Fixed**

- Tri Fade FX; custom-palette colour picker; Colortwinkles; LED buffer-size calculation; UDP name-sync rework; crash debug output added.
