# JC ESP32-P4 M3 audio

## Goal

Create a dedicated firmware and device-model configuration for the JC-ESP32P4-M3-DEV so its onboard ES8311 microphone feeds AudioService and the audio-reactive effects reliably.

## Hardware contract

- ES8311 control address: `0x18`.
- I2C: SDA GPIO7, SCL GPIO8.
- I2S: BCLK GPIO12, WS GPIO10, MCLK GPIO13.
- Mic data defaults to GPIO48 from the published schematic. GPIO11 remains testable through AudioService's live `sdPin` control because the supplied ESPHome example disagrees with the schematic.
- Default sample rate: 22,050 Hz.

## Implementation

1. Add an `esp32p4-jc-m3-eth` firmware variant based on the existing P4 configuration.
2. Extend the existing Espressif `esp_codec_dev` ES8311 platform path to the JC P4 target.
3. Configure record-only ES8311 operation with a 256× MCLK ratio and a 32-bit I2S word matching the receive channel.
4. Add the JC device model with its audio pins so audio-reactive effects can consume the local source.
5. Add a hardware reference page and focused manifest/configuration tests.
6. Build the desktop target, run focused checks, and build the JC firmware.

## Validation boundary

The build and host-side checks prove the configuration and code paths. The product owner verifies real capture on the JC board, first with GPIO48 and then GPIO11 if GPIO48 produces silence.
