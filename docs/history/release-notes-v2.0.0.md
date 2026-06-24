# projectMM v2.0.0

A landmark release — the largest since 1.0 — **77 commits across 8 PRs**. Highlights: audio-reactive lighting, multi-protocol network I/O (ArtNet / E1.31 / DDP), a new ESP32-P4 firmware variant, runtime Ethernet configuration, LAN device discovery, a standardised LED-driver layer, browser-over-serial provisioning, and a ground-up rework of the 3D preview.

If you like projectMM, give it a ⭐️, fork it, or open an issue or pull request. It helps the project grow, improve, and get noticed.

### 🔬 How it's built
Features like the audio pipeline and the LED drivers are built under projectMM's [*Industry standards, our own code*](https://github.com/MoonModules/projectMM/blob/main/CLAUDE.md#principles) principle: spec the behaviour from primary sources (the ESP32 and sensor datasheets, the WS2812 timing spec, reference DSP standards), reach for the textbook algorithm and the textbook name (a DC-blocker high-pass, a Hann window, RMS, the WS2812 encoder), pin it with unit + scenario tests first, then write every line fresh against our own architecture. Each module credits its prior art by name. The result is independent by construction — a clean, recognisable implementation you can read, trust, and build on.

### ✨ Highlights

**Audio-reactive lighting (new)**
- **AudioModule** — an I2S microphone peripheral: live RMS level + a 16-band FFT spectrum (Hann window, DC-blocker, configurable sample rate / gain / noise floor). Boards with a built-in mic come pre-configured; on any board you add it from the UI.
- New **AudioSpectrum** and **AudioVolume** effects driven by it; WiFi modem power-save disabled so audio + radio coexist cleanly.

**Multi-protocol network I/O (new)**
- **NetworkSendDriver** streams frames out over **ArtNet, E1.31/sACN, and DDP**; **NetworkReceiveEffect** receives them (auto-detects the protocol per port). Per-sink `light_count` slicing lets one device drive part of a rig over LEDs and the rest over the network. Resolume-style discovery included.

**New platform — ESP32-P4**
- `esp32p4-eth` firmware variant (Waveshare P4-NANO, Ethernet): builds, publishes to the web installer + releases, runs the full pipeline.
- New **Parlio** 8-lane LED driver for the P4; per-board Ethernet pin config.

**LED drivers — standardised, per chip**
- A shared driver scaffold with **RMT** (classic/S3), **Parlio** (P4), and **LCD_CAM 8-lane** (S3) WS2812 backends, plus a parallel multi-lane path; a dedicated `Pin` control type for GPIO settings, and a hardware loopback self-test.

**Preview, reworked**
- The 3D preview streams full-resolution frames to the browser **without stalling the LED render tick** — a resumable, chunked WebSocket send drained off the hot path, with drop-new backpressure.
- **Graceful degradation** on a slow link: sheds frame rate first, then resolution (closed-form spatial downsample with a memory-derived point cap). A 128² grid previews smoothly on classic ESP32, S3, and P4.
- Responsive UI: docked split-pane preview / floating draggable picture-in-picture; the layout draws the instant its coordinate table arrives.

**More effects, modifiers & layouts**
- Effects: **Rings**, **Ripples** (MoonLight sine-wave water surface), **DistortionWaves**, **Sine**, alongside the existing Noise / Plasma / Fire / Metaballs / GlowParticles / LavaLamp / Lines / Spiral / GameOfLife / Rainbow / Checkerboard.
- Modifiers: **RandomMap**, **Rotate** (plus the existing Mirror / Multiply), with a dynamic-modifier hook; **WheelLayout** joins Grid.

**Networking & provisioning**
- **Device discovery** — DevicesModule finds other projectMM (and generic) devices on the LAN via mDNS browse + an HTTP subnet sweep, with provenance, age-out, and a persisted list.
- **Runtime Ethernet PHY config** — RMII (internal EMAC) and W5500 (SPI) pin/PHY settings are live controls with per-board defaults; W5500 reconfigures with no reboot.
- **Improv = REST over serial** — the web installer pushes device-model config to a freshly-flashed board over USB (mixed-content-proof), running the same apply-core the HTTP API uses.

**Identity & installer**
- `deviceName` is one network identity (mDNS / SoftAP / DHCP hostname); BoardModule folded into SystemModule; `board` → `deviceModel`.
- Picture-based device picker; capability chips (active / supported / planned); auto-detect device IP from serial; board-details popup.

**Docs & tooling**
- Two-chapter **Getting started** guide (install + a UI tour for new users).
- `preview_health.py` — a browser-faithful preview-stream health probe, on MoonDeck's Live tab.

### 🛠 Fixes
- mDNS browse crash on UI refresh (async-handle race → synchronous, throttled browse).
- Audio level read-out reading 0 between beats (now the per-second RMS peak).
- Windows release build (colons in plan filenames); CI double-run.

### 📦 Install
Flash from your browser — pick your board, flash the matching firmware, hand over WiFi via Improv: <https://moonmodules.org/projectMM/install/>. Step-by-step in the [Getting started guide](https://github.com/MoonModules/projectMM/blob/main/docs/gettingstarted.md).

**Supported targets this release:** ESP32 (classic / Olimex), ESP32-S3, **ESP32-P4 (new)**, plus macOS arm64 / Windows x64 desktop.
