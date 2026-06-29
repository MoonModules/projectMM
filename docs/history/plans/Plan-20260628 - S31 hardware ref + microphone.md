# Plan — ESP32-S31 hardware reference + onboard microphone (ES8311), Ethernet pins, full feature list

## Context

While planning the S31 onboard-microphone feature, the product owner asked to also (a) collect
**all** S31 pin assignments for future use (not just audio), (b) search out **all** the board's
features (the catalog only lists Ethernet + Audio; there are more), and (c) **store all the scraped
hardware info in the docs**. So this plan has two deliverables:

1. **A durable S31 hardware-reference doc** — the home for everything scraped from the official
   Espressif ESP32-S31 Function-CoreBoard-1 schematic (pin maps for audio, Ethernet, SD, RGB,
   buttons + the board feature list). This unblocks the deferred S31 Ethernet *and* the new mic
   work, and is reference for anything S31 later.
2. **The S31 microphone feature** itself (ES8311 codec → the existing audio-reactive path), which
   the reference's audio pins make implementable.

Everything below the audio seam is reused: projectMM already has the full audio framework
(`AudioModule` → FFT → `AudioFrame`, the `hasI2sMic` platform seam). The S31's mic differs only in
that it routes through an **ES8311 I2S codec configured over I2C** (vs the existing direct-I2S
INMP441 MEMS mic) — so the new work is a platform-side codec-init step, behind the stable seam.

## Scraped hardware facts (from the official schematic — the data to store)

**Source:** `https://dl.espressif.com/schematics/esp32-s31-function-coreboard-1-schematics.pdf`
(rev C, 2026-05-13), read page-by-page. Datasheet:
`https://documentation.espressif.com/esp32-s31_datasheet_en.pdf`.

**Audio — ES8311 codec (U7, I2C addr 0x18) + NS4150B amp (U9) + electret mic (J6) + speaker:**

| Signal | GPIO | Notes |
|---|---|---|
| I2S_MCLK | GPIO52 | master clock to codec |
| I2S_SCLK (BCLK) | GPIO53 | bit clock |
| I2S_ASDOUT (mic in → ESP) | GPIO54 | ADC/mic data from codec |
| I2S_LRCK (WS) | GPIO55 | word select |
| I2S_DSDIN (→ codec DAC) | GPIO56 | playback data (speaker path) |
| ESP_I2C_SDA | GPIO50 | codec control bus |
| ESP_I2C_SCL | GPIO51 | codec control bus |
| PA_CTRL | GPIO57 | NS4150B amp enable |

The ES8311 `CE` pin sets I2C addr (default **0x18**). The codec is the standard `esp_codec_dev`
ES8311 part. The **mic path uses I2S_ASDOUT (GPIO54) for record**; the speaker path (DSDIN/PA) is
out of scope for the mic feature.

**Ethernet — YT8531 PHY (U8) → RJ45, RGMII (resolves the deferred S31 eth pins):**

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| ETH_INTN | GPIO2 | | ETH_TXD3 | GPIO10 |
| PHY_MDC | GPIO4 | | ETH_TX_CTL | GPIO11 |
| PHY_MDIO | GPIO5 | | ETH_TXCLK | GPIO13 |
| ETH_PHY_RST | GPIO6 | | ETH_RX_CLK | GPIO14 |
| ETH_TXD0 | GPIO7 | | ETH_RX_CTL | GPIO15 |
| ETH_TXD1 | GPIO8 | | ETH_RXD3 | GPIO16 |
| ETH_TXD2 | GPIO9 | | ETH_RXD2 | GPIO17 |
| | | | ETH_RXD1 | GPIO18 |
| | | | ETH_RXD0 | GPIO19 |

PHY = **YT8531** (Motorcomm), **RGMII** (1 Gbps), 25 MHz XTAL (Y2). Note: this is RGMII, not the
RMII our P4/classic eth uses — a different MAC config (the S31 EMAC does RGMII). Flag for the eth
implementation: the existing `ethInit` RMII path won't cover RGMII unmodified.

**Other onboard features (from the System Block, page 1, + pin tables):**
- **RGB LED** — WS2812 (D7) on **GPIO60** (already wired: catalog RmtLed pins="60").
- **SD card slot** — SD_D0-3 / SD_CLK / SD_CMD (the module's SDIO pins, GPIO20-25 per the user
  guide). Note: `SOC_SDMMC_SUPPORTED` is absent on the S31 soc-caps — so it's likely SPI-mode SD or
  a different controller; verify before claiming it.
- **USB-A host** (USB 2.0 HS, the high-speed host port) + **USB-C** ×2 (USB Serial/JTAG on one,
  USB-to-UART bridge / CP2102N on the other).
- **Buttons** — BOOT (GPIO61), RESET (EN).
- **40-pin GPIO header** (J2), optional 32.768 kHz XTAL footprint.

**SoC-level capabilities (S31 soc_caps):** WiFi 6, **BT** (Bluetooth, no separate BLE flag),
**IEEE 802.15.4** (Thread/Zigbee), **USB-OTG**, **GPSPI**, **TWAI** (CAN), RMT/Parlio/LCD_CAM-i80,
on-chip EMAC. (RISC-V dual-core — shares the MoonLive RISC-V backend with the P4.)

## Deliverable 1 — the hardware-reference doc

**New file `docs/reference/esp32-s31-coreboard.md`** (a new `docs/reference/` directory — the home
for board hardware references; P4/S3 reference docs can follow the same shape later). Holds the
tables above (audio/eth/RGB/SD pins + features + the schematic/
datasheet URLs), present-tense, so any future S31 work (eth, mic, SD, USB-host) reads it instead of
re-scraping the PDF. Delete the temporary `docs/backlog/s31-microphone-spec.md` draft into it.

**Expand the S31 catalog `planned` list.** `check_devices.py` whitelists only
`{LEDs, WiFi, Ethernet, Audio}` for **`supported`**, but **`planned` accepts any string** (its
whitelist is `None`). So the S31 `planned` can carry the fuller, honest feature list — add the ones
the board has that we don't drive yet: e.g. `Ethernet`, `Audio` (already), plus `Bluetooth`,
`Thread/Zigbee (802.15.4)`, `SD card`, `USB host`, `Speaker`, `CAN (TWAI)`. (Confirm the exact
labels with the PO; these describe the *board*, not yet projectMM modules.)

## Deliverable 2 — the microphone feature (ES8311)

Now implementable with the real pins. Design (unchanged from the prior research, pins filled in):

- **Seam stays stable:** `AudioModule` keeps calling `audioMicInit(ws, sd, sck, rate)` +
  `audioMicRead`; the codec init slots in *below* it. A board has either an INMP441 or an ES8311,
  not both — so codec choice is a per-target property, not an `AudioModule` control.
- **New platform seam:** `audioCodecInit(CodecType, AudioCodecPins)` in `platform.h` (neutral
  `CodecType{None, Es8311}`), called by `AudioModule::reinit()` before `audioMicInit()`; inert stub
  on desktop / non-codec targets.
- **New `src/platform/esp32/platform_esp32_es8311.cpp`** — ES8311 init via the **`esp_codec_dev`**
  managed component (record mode, mic gain, MCLK), incl. the platform's **first I2C master bus**
  (I2C is new to `src/platform/esp32/`). Behind `#if SOC_I2S_SUPPORTED` + a codec gate.
- **`esp32/main/idf_component.yml`** — add `espressif/esp_codec_dev`, `rules: target == esp32s31`
  (the established chip-gated managed-component pattern, like `ip101`/`w5500`).
- **`platform_esp32_i2s.cpp`** — parameterise the I2S slot if the ES8311 format differs from the
  INMP441 Philips-LEFT default (the codec presents standard I2S; confirm master/slave from the
  schematic — the ESP drives MCLK on GPIO52, so ESP is I2S master).
- **`deviceModels.json`** — S31 `Audio` → `supported`; add an `AudioModule` with the I2S pins
  (ws=GPIO55, sd=GPIO54, sck=GPIO53) + the I2C pins (sda=GPIO50, scl=GPIO51) + MCLK=GPIO52.

## Files

- **New:** `docs/history/esp32-s31-coreboard.md` (the hardware reference),
  `src/platform/esp32/platform_esp32_es8311.cpp` (codec init).
- **Edit:** `src/platform/platform.h` (codec seam), `src/platform/desktop/platform_desktop.cpp`
  (stub), `src/platform/esp32/platform_esp32_i2s.cpp` (slot param) + `platform_config.h` (per-target
  audio default), `src/core/AudioModule.h` (call codec init first; I2C-pin controls if board-var),
  `esp32/main/idf_component.yml` (esp_codec_dev), `docs/install/deviceModels.json` (S31 audio +
  expanded planned), `docs/moonmodules/core/AudioModule.md` (ES8311 path). Delete
  `docs/backlog/s31-microphone-spec.md` (folded into the reference + AudioModule.md).

## Riskiest parts

1. **The mic feature can be bench-verified now — the pins are known.** The earlier blocker is gone.
2. **ES8311 master/slave + MCLK** — ESP drives MCLK (GPIO52) ⇒ ESP is I2S master; confirm the
   `esp_codec_dev` config matches.
3. **`esp_codec_dev` on `release/v6.1`** — a managed component must resolve + build on the pinned
   IDF; same v6.0-floor / managed-component decision class as the P4 esp-hosted exception, record it.
4. **First I2C in the platform layer** — keep the I2C master owned by the codec file, behind the
   boundary.
5. **(For the later eth work, not this plan)** the S31 eth is **RGMII**, not RMII — the existing
   `ethInit` RMII path needs an RGMII branch. Captured in the reference; out of scope here.

## Verification

- Reference doc renders, present-tense, with the pin tables + URLs; `check_devices.py` green with
  the expanded S31 `planned`.
- Desktop build green (codec stub). ESP32-S31 build green with `esp_codec_dev`. Other targets
  unaffected (stub). `ctest` + scenarios green (additive seam).
- **Bench (the real test):** flash the S31, add an `AudioModule` with the audio pins above, make
  sound → the level + 16-band FFT respond in the UI, an audio-reactive effect lights up. Inert
  audio behaviour preserved on a non-codec board (S3/P4 INMP441 path still works).
- Save the approved plan to `docs/history/plans/Plan-YYYYMMDD - S31 hardware ref + microphone.md`.

## Decisions locked

- **Doc home:** new `docs/reference/esp32-s31-coreboard.md` (a new `docs/reference/` dir for board
  references).
- **Scope:** the reference doc + the mic feature land **together** in one feature/branch (the doc's
  audio pins feed straight into the implementation).

## Open question (minor, settle during implementation)

- **`planned` labels:** the exact capability strings for the S31 board's not-yet-driven features —
  proposed: `Bluetooth`, `Thread/Zigbee`, `SD card`, `USB host`, `Speaker`, `CAN`. Since `planned`
  takes any string, I'll use clear short labels and the PO can adjust in review.

## Out of scope

- **Speaker / DAC output** (NS4150B, I2S_DSDIN/PA_CTRL) — separate capability.
- **S31 Ethernet implementation** — the *pins* are now captured (RGMII), but wiring RGMII into
  `ethInit` is its own feature (the existing path is RMII-only).
- **SD card / USB-host / BT / Thread / CAN drivers** — board has them; projectMM doesn't, listed in
  `planned` as board capabilities, not built here.
