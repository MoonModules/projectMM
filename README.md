# projectMM

Drive large LED installations and DMX lighting from ESP32, Teensy, Raspberry Pi, Windows, macOS or Linux desktop. One source tree, multiple targets.

![Web UI](docs/assets/screenshots/ui_overview.png)

👉 **Try it now:** flash an ESP32 straight from your browser → <https://moonmodules.org/projectMM/install/>

📦 **Release + downloads:** [latest release](https://github.com/MoonModules/projectMM/releases/latest)

🛠️ **Building / hacking on it?** [MoonDeck](scripts/MoonDeck.md), our browser-based dev console (build · flash · test · live device discovery), comes in the repo.

Open Chrome or Edge, plug in your board, and you'll see lights in under a minute.

If you like projectMM, give it a ⭐️, fork it, or open an issue or pull request; it helps the project grow, improve, and get noticed.

## What makes projectMM different

🔵 **16,384 LEDs on a *classic* ESP32**, not just the S3 or P4. Memory-adaptive from a 16×16 panel up to 128×128, degrading gracefully on tight boards instead of crashing.

🧊 **Native 3D from the ground up**: 2D and 1D are just the cases where a dimension is size 1. Effects never pick a mode.

🎛️ **Pluggable pipeline**: Layouts → Layers (effects + modifiers) → Drivers. Build it visually in the browser, and every change applies live (settings also persist to flash across power cycles).

🔄 **No reboot to apply a configuration change**: edit a pin map, a strand length, an output protocol, or the mic on a running device and it takes effect on the very next frame — no init-at-boot step, no restart. Where most LED-controller firmware needs a reboot for a pin or protocol change, projectMM applies it live. (Flashing new *firmware* over OTA still needs the usual power cycle — that's a binary swap, not a config change.)

💡 **DMX *and* addressable LEDs in one setup**: RGB strips, RGBW pixels, par lights, moving heads, all through the same pipeline.

🔌 **Parallel WS2812 output**: drive many strands at once over three ESP32 peripherals — RMT (every chip), the S3's LCD_CAM i80 bus (8 lanes), and the P4's Parlio engine (up to 20 simultaneous strands on the P4) — each with an on-device loopback self-test that bit-verifies the wire signal.

🌐 **Industry protocols, both directions**: send *and* receive [Art-Net](https://art-net.org.uk/), [E1.31/sACN](https://tsp.esta.org/tsp/documents/docs/ANSI_E1-31-2018.pdf), and [DDP](http://www.3waylabs.com/ddp/) over the network — interoperable with Falcon, Advatek, xLights, Resolume, LedFx and other industry gear.

🎵 **Audio-reactive**: an I²S microphone drives a 16-band FFT spectrum + sound level, consumed by audio-reactive effects — all built fresh from the mic datasheet and textbook DSP.

🛡️ **Robust to any input**: add, delete, replace, or reconfigure any module in any order, at any grid size, and the device keeps running — degraded or idle, never crashed. Every crash that's ever found becomes a regression test, so it stays fixed.

🖥️ **One source tree, many targets**: the same code runs on ESP32, Teensy, Raspberry Pi, and macOS / Windows / Linux.

🎨 **Plug in, open a browser, see lights**: a live 3D preview of every effect, modifier, and layout, controllable from the same tab. The interface renders any module from its declared controls, so adding a module needs zero UI code.

⚡ **Flash from your browser in seconds**: the web installer picks your board, flashes the matching firmware, and hands WiFi credentials to the device over USB via Improv. No serial monitor, no recompile.

## Under the hood

🛠️ **ESP-IDF directly, no Arduino**: the ESP32 build is pure ESP-IDF (v6.x): native LED drivers, `esp_http_server`, FreeRTOS, built with `idf.py`, not PlatformIO or the Arduino framework. See [building.md § Why not Arduino](docs/building.md#why-not-arduino).

📦 **No third-party libraries**: no FastLED, no ESPAsyncWebServer, no ArduinoJson. The colour math, the HTTP/WebSocket server, and the control storage are all in-tree. A library, when genuinely needed, lives behind the platform boundary in `src/platform/`, never in core. The full rationale + replacements: [building.md § Third-party libraries](docs/building.md#third-party-libraries).

🔬 **Industry standards, our own code**: we study the prior art hard — friend repos, peripheral datasheets, the Art-Net / E1.31 / WS2812 standards — carry its *ideas* forward, and credit it by name; but we write our own code rather than copying theirs or tracing their structure. Each feature is spec'd from the primary source, its behaviour pinned with unit + scenario tests, then written fresh against our own architecture, so the result is independent by construction, not a renamed fork. Textbook algorithm, textbook name, our implementation. The method: [CLAUDE.md § Principles](CLAUDE.md#principles).

🧱 **One module model**: every effect, modifier, layout, and driver is a `MoonModule`: one base class, a uniform lifecycle, declared controls. That uniformity is why the UI renders any module with zero per-module code, and why a new capability is a new file, not a new framework. See [architecture.md § MoonModules](docs/architecture.md#moonmodules).

## Performance

Measured end-to-end through a full render pipeline (effect → modifier → ArtNet output) on real hardware. FPS is derived from the per-frame tick time.

The **Desktop** column is host-CPU-bound, not OS-bound: the numbers track the machine, not macOS vs Windows vs Linux. Captured on Apple Silicon (M-series); the macOS and Windows binaries run the same code on comparable hardware.

### Frames per second

| Grid | Lights | Desktop | Olimex `esp32` | Olimex `esp32-eth` | LOLIN S3 N16R8 `esp32s3-n16r8` |
|---|---:|---:|---:|---:|---:|
| 16×16 | 256 | *(below host clock resolution)* | 1,543 | 1,628 | 1,672 |
| 32×32 | 1,024 | 166,667 | 447 | 432 | 287 |
| 64×64 | 4,096 | 40,000 | 81 | 71 | 25 |
| 128×128 | 16,384 | 9,708 | 11 | 10 | 6 |

The Olimex `esp32` figures were measured on the WiFi+Ethernet build (the pre-collapse `esp32-eth-wifi`, now the default `esp32`). The LOLIN S3 N16R8 was measured over WiFi with `Network.txPowerSetting` capped to 8 dBm (the brown-out fix, see below); at 128×128 it's bound by ArtNet over WiFi at reduced TX power (~93 ms of the ~164 ms tick), which is why it trails the Ethernet boards despite a faster core. (The S3 now also supports W5500 SPI Ethernet, which sidesteps that WiFi bottleneck on boards wired for it.) The board's niche is PSRAM headroom (8 MB) for large pixel buffers; use an Ethernet board when frame rate matters.

### Free heap

Each cell is **free internal RAM / largest contiguous internal-RAM block**. Internal RAM is the scarce, comparable resource across all boards, so for PSRAM boards (the S3) this is internal-only, NOT the PSRAM-merged total (we assume the 8 MB PSRAM pool is large enough that it isn't the constraint). The block size is the memory-pressure signal that matters: free RAM can be ample while fragmentation leaves no single block big enough for the next allocation.

| Grid | Desktop | Olimex `esp32` | Olimex `esp32-eth` | LOLIN S3 N16R8 `esp32s3-n16r8` |
|---|---:|---:|---:|---:|
| 16×16 | unlimited | 139 KB / 52 KB | 178 KB / 100 KB | 238 KB / 160 KB |
| 32×32 | unlimited | 132 KB / 50 KB | 172 KB / 92 KB | 240 KB / 152 KB |
| 64×64 | unlimited | 108 KB / 48 KB | 147 KB / 62 KB | 236 KB / 152 KB |
| 128×128 | unlimited | 129 KB / 52 KB | 132 KB / 48 KB | 240 KB / 164 KB |

The S3's internal-free stays flat across grid sizes because its Layer buffer + LUT live in PSRAM: growing the grid consumes PSRAM, not internal RAM. The Olimex boards hold those buffers in internal RAM, so their free heap drops as the grid grows.

Build variants differ structurally: the default `esp32` includes the WiFi stack (~270 KB flash, ~28 KB heap) alongside Ethernet. `esp32-eth` drops WiFi for more free heap, at the cost of slightly slower tick on large grids (lwIP buffer-pool sizing is tuned for the WiFi+Ethernet sdkconfig). The right variant depends on whether the deployment needs WiFi at all, or only Ethernet plus the extra buffers.

The numbers above are observations. The **contracts** projectMM commits to, what the device must hit on every CI run, live in [`test/scenarios/*.json`](test/scenarios/) as per-step `contract.<target>` blocks; see [docs/testing.md § Performance contracts](docs/testing.md#performance-contracts-contracttarget) for how they're set and renegotiated. The [docs/performance.md](docs/performance.md) page covers the *why* (WiFi vs Ethernet physics, sizeof tables, build-variant deltas).

## Getting started

### From a release

**ESP32: flash from your browser.** Open the [web installer](https://moonmodules.org/projectMM/install/) in Chrome or Edge; it walks you through release, board and firmware selection, flashing, and network setup. The installer lists stable releases and a `latest` build (published automatically on every merge to main) carrying the newest unreleased changes.

![Installer](docs/assets/screenshots/installer.png)

**Desktop: download and run.** Grab the build for your OS from the [releases page](https://github.com/MoonModules/projectMM/releases):

- **macOS arm64:** `projectMM-macos-arm64-vX.Y.Z.tar.gz`: unpack, run `./projectMM`. The binary is unsigned, so Gatekeeper prompts on first run; right-click → Open, or clear the quarantine flag with `xattr -dr com.apple.quarantine ./projectMM`.
- **Windows x64:** `projectMM-windows-x64-vX.Y.Z.zip`: unzip, double-click `projectMM.exe`. SmartScreen may warn on first run because the binary is unsigned (More info → Run anyway).

Then open `http://localhost:8080/`.

Once running, the UI lets you build a render pipeline visually (layouts → layers with effects + modifiers → drivers), preview the result in 3D, send it to Art-Net, and save it. The source tree also builds for Teensy, Raspberry Pi, and Linux from source (see [building.md](docs/building.md)), though currently only the macOS, Windows, and ESP32 binaries ship as releases.

### From source

You need [uv](https://docs.astral.sh/uv/) (Python launcher), CMake 3.20+, and a C++20 compiler. For ESP32, ESP-IDF v6.x is also required; see [building.md](docs/building.md) for the full setup instructions.

Once prerequisites are in place, launch MoonDeck, the browser-based dev console:

```sh
uv run scripts/moondeck.py
```

Open `http://localhost:8420`: PC tab to build / run / test, ESP32 tab to flash, Live tab to discover devices. Full per-command reference: [scripts/MoonDeck.md](scripts/MoonDeck.md).

![Moondeck Pc](docs/assets/screenshots/moondeck_pc.png)

## Documentation

| Document | What's in it |
|----------|--------------|
| [architecture.md](docs/architecture.md) | How the system is put together: core runtime + light domain, pipeline, memory, parallelism |
| [coding-standards.md](docs/coding-standards.md) | How code in this repo is written: conventions, file shape, static checks |
| [building.md](docs/building.md) | How to build and flash for every supported target |
| [testing.md](docs/testing.md) | What tests exist and what they cover |
| [performance.md](docs/performance.md) | Per-module timing, memory, sizeof, per platform |
| [moonmodules/](docs/moonmodules/) | One spec page per module: [core](docs/moonmodules/core/) services and [light](docs/moonmodules/light/) effects, layouts, modifiers, drivers |
| [CLAUDE.md](CLAUDE.md) | Rules, constraints, and development process |

## How we work

projectMM is built by AI agents under tight human direction. Everything in this repository, firmware and desktop code, the web installer, the MoonDeck dev console, all documentation, the unit and scenario tests, even the UI screenshots and effect GIFs, is authored by agents; the **product owner** writes none of it directly. What the product owner *does* author is the **process** ([CLAUDE.md](CLAUDE.md)), the **architecture** ([architecture.md](docs/architecture.md)), and the **module specifications** ([docs/moonmodules/](docs/moonmodules/)); then decides what to build next, reviews every line and every spec, runs the hardware tests, and controls every commit, merge, and release. Agents write in defined roles; they don't make decisions. The agent writes; the product owner thinks.

Meet the team: 🤖 Architect designs, 👽 Developer implements, 👾 Reviewer checks before merge, 🛸 Tester verifies, 💀 Runner does quick build and check passes. Full team descriptions in [CLAUDE.md](CLAUDE.md).

A few principles run through everything:

- **Common patterns first**: recognisable practice across code, docs, tests, UI. Bespoke choices need a stated reason.
- **Specs before code**: a module is documented in [`docs/moonmodules/`](docs/moonmodules/), purpose, controls, behaviour, edge cases, prior art, well enough to implement from before it's written.
- **Working software at every commit**: each commit builds, passes the test + scenario gates, and produces something you can see run; never a broken intermediate state.
- **Minimalism**: flat, predictable code; removing code beats adding it; every addition pays for itself.
- **The system as it is**: code and docs describe the present; git history is the changelog.

The full rules and process are in [CLAUDE.md](CLAUDE.md).

## History

This is the current iteration of years of LED / light system development. Each prior project proved ideas this one builds on:

| Project | Description | Repo |
|---------|-------------|------|
| **WLED** | Open-source LED firmware (user / contributor since 2021) | [Aircoookie/WLED](https://github.com/Aircoookie/WLED) |
| **WLED-MoonModules** | WLED fork with advanced features | [MoonModules/WLED](https://github.com/MoonModules/WLED) |
| **StarLight** | Standalone LED firmware | [ewowi/StarLight](https://github.com/ewowi/StarLight) |
| **MoonLight** | Ground-up build: 60+ effects, memory-optimised mapping, 11 driver types | [MoonModules/MoonLight](https://github.com/MoonModules/MoonLight) |

We built, maintained, and contributed to these projects, so projectMM is grounded in years of our own hands-on experience, not arms-length study. Their lessons and proven patterns are distilled in [`docs/history/`](docs/history/README.md), alongside monthly digests of friend projects (like FastLED and upstream WLED) we follow closely but don't own. From all of it we carry the ideas forward into our own implementation: we apply what we learned and write our own code rather than copying theirs; and when a specific project or person inspires something here, we credit them by name (in the history digests and each module's "Prior art" notes).

## Contributing

projectMM is a community project, built in the open, shaped by the people who use it. We'd love to hear from you:

- **Ideas and requests**: an effect, a layout, a driver, a fixture you want supported? [Open an issue](https://github.com/MoonModules/projectMM/issues) and tell us.
- **Help build it**: pick something from the [issues](https://github.com/MoonModules/projectMM/issues), or propose a MoonModule. See [How we work](#how-we-work) for the process.
- **Test on hardware**: run it on your panels, boards, and fixtures, and report what works and what doesn't.
- **Talk to us**: questions, show-and-tell, and design discussion on [Discord](https://discord.gg/TC8NSUSCdV).

Find the MoonModules community on [Discord](https://discord.gg/TC8NSUSCdV), [Reddit](https://reddit.com/r/moonmodules), [YouTube](https://www.youtube.com/@MoonModulesLighting), and [GitHub](https://github.com/MoonModules).

## License

See [LICENSE](LICENSE).
