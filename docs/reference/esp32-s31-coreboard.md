# ESP32-S31 Function-CoreBoard-1 — hardware reference

Pin maps and onboard features for the Espressif **ESP32-S31 Function-CoreBoard-1**, read from the
official schematic so projectMM work (Ethernet, audio, SD, USB-host) reads this instead of
re-scraping the PDF. The board is the bench S31 (`esp32s31` firmware).

**Sources**
- Schematic (rev C, 2026-05-13): <https://dl.espressif.com/schematics/esp32-s31-function-coreboard-1-schematics.pdf>
- PCB layout: <https://dl.espressif.com/schematics/esp32-s31-function-coreboard-1-pcb-layout.pdf>
- Datasheet (chip): <https://documentation.espressif.com/esp32-s31_datasheet_en.pdf>
- Module (WROOM-3): <https://documentation.espressif.com/esp32-s31-wroom-3_datasheet_en.pdf>
- User guide: <https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/user_guide.html>

The module is **ESP32-S31-WROOM-3**: RISC-V dual-core (≤300 MHz), Wi-Fi 6, BT, IEEE 802.15.4,
on-chip 1 Gbps EMAC. It shares the MoonLive RISC-V backend with the ESP32-P4.

## Audio (ES8311 codec)

The onboard electret mic (J6) and speaker connect through an **ES8311 mono codec** (U7) + an
**NS4150B 3 W class-D amplifier** (U9). The ESP is the I2S master (it drives MCLK).

| Signal | GPIO | Direction / role |
|---|---|---|
| I2S_MCLK | 52 | master clock → codec (ESP is I2S master; MCLK = 256 × sample_rate) |
| I2S_SCLK (BCLK) | 53 | bit clock |
| I2S_LRCK (WS) | 55 | word select |
| I2S_ASDOUT | 54 | **mic / ADC data: codec → ESP** (the record path) |
| I2S_DSDIN | 56 | playback / DAC data: ESP → codec (speaker path) |
| **ESP_I2C_SDA** | **51** | codec control bus — **SDA is GPIO51, SCL is GPIO50** |
| **ESP_I2C_SCL** | **50** | codec control bus |
| PA_CTRL | 57 | NS4150B amplifier enable |

> **SDA/SCL are GPIO51/GPIO50** — the *opposite* of what the schematic's `ESP_I2C_SDA` /
> `ESP_I2C_SCL` net labels suggest. Bench-confirmed: the [I2cScanModule](../moonmodules/core/I2cScanModule.md)
> (sda=51, scl=50 in the S31 catalog entry) finds the ES8311 ACK at 0x18; with 50/51 nothing
> ACKs. The other audio pins match the schematic + the chip's GPIO table (all of GPIO50–57 are
> plain I/O GPIOs routed through the matrix — no special-function conflict).

- **ES8311 I2C address: `0x18`** (the default; set by the `CE` pin tie).
- Driven by Espressif's **`esp_codec_dev`** managed component (the ES8311 driver). The codec
  needs **MCLK running before it answers I2C**, and `es8311_codec_cfg.mclk_div` must be set
  (256, the standard ratio) or `open` fails "unable to configure sample rate". So AudioModule
  brings up the I2S channel (which drives MCLK on GPIO52) **before** the codec I2C config.
- **Mic-only path** (audio-reactive input) needs MCLK/SCLK/LRCK + ASDOUT (record) + the I2C bus.
  The speaker path (DSDIN + PA_CTRL) is output, a separate capability.

## Ethernet (YT8531 PHY, RGMII, 1 Gbps)

On-chip EMAC → **YT8531** (Motorcomm) PHY (U8) → RJ45, **RGMII** with a 25 MHz crystal (Y2).

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| ETH_INTN | 2 | | ETH_TXD3 | 10 |
| PHY_MDC | 4 | | ETH_TX_CTL | 11 |
| PHY_MDIO | 5 | | ETH_TXCLK | 13 |
| ETH_PHY_RST | 6 | | ETH_RX_CLK | 14 |
| ETH_TXD0 | 7 | | ETH_RX_CTL | 15 |
| ETH_TXD1 | 8 | | ETH_RXD3 | 16 |
| ETH_TXD2 | 9 | | ETH_RXD2 | 17 |
| | | | ETH_RXD1 | 18 |
| | | | ETH_RXD0 | 19 |

> **RGMII, not RMII.** projectMM's classic/P4 Ethernet path (`ethInit` in
> `src/platform/esp32/platform_esp32.cpp`) is RMII (fewer data lines, 50 MHz ref clock). The S31's
> 1 Gbps EMAC is RGMII (4-bit data each way + TX/RX clocks). Wiring the S31 eth needs an RGMII MAC
> config branch — it is not a drop-in of the RMII pin struct.

## Other onboard features

- **Addressable RGB LED** — WS2812 (D7) on **GPIO60**. *(Driven today: the catalog's S31
  `RmtLedDriver` uses `pins: "60"`.)*
- **SD card slot** — SD_D0–D3 / SD_CLK / SD_CMD on the module's SDIO pins (GPIO20–25 per the user
  guide). Note: `SOC_SDMMC_SUPPORTED` is **absent** in the S31 soc-caps, so the slot is likely
  SPI-mode (GPSPI) rather than the SDMMC peripheral — confirm before relying on it.
- **USB-A host** — USB 2.0 high-speed host port (5 V, 0.5 A limit).
- **USB-C ×2** — one is USB Serial/JTAG (native), one is a USB-to-UART bridge (CP2102N default, or
  CH9102X with the alternate BOM). Auto-download via DTR/RTS → EN/BOOT.
- **Buttons** — BOOT (GPIO61), RESET (EN).
- **40-pin GPIO header** (J2). Optional 32.768 kHz crystal footprint (Y1, NC by default).

## SoC capabilities (from `components/soc/esp32s31/include/soc/soc_caps.h`)

Wi-Fi 6 · Bluetooth (no separate BLE soc-flag) · IEEE 802.15.4 (Thread/Zigbee) · USB-OTG · GPSPI ·
TWAI (CAN) · RMT · Parlio · LCD_CAM i80 · on-chip EMAC · PSRAM. RISC-V dual-core.

What projectMM drives on the S31 today: **LEDs** (RMT, GPIO60) + **Wi-Fi 6**. Ethernet, audio, and
the rest are board capabilities not yet wired — see the S31 entry's `planned` list in
`docs/install/deviceModels.json`.
