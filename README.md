# projectMM

Drive large LED installations and DMX lighting from ESP32, Teensy, Raspberry Pi, Windows, macOS or Linux desktop. One source tree, multiple targets.

https://github.com/user-attachments/assets/b12b28ca-7e87-477a-942b-fcae601b721d

## What you get

- **Plug in, open a browser, see lights.** A live 3D preview of every effect, every modifier, every layout, controllable from the same browser tab.
- **Effects, modifiers, layouts, drivers** — all pluggable, all configurable live, all persisted across reboots.
- **One firmware, many devices.** ESP32, Teensy, Raspberry Pi, Windows / macOS / Linux desktop — the same source builds for each.
- **Native 3D** from the start. 2D and 1D are the cases where one or two dimensions are size 1; effects don't pick a mode.
- **Built-in browser UI.** The interface renders any module from its declared controls — adding a new effect needs zero UI code.
- **DMX and addressable LEDs in the same setup.** RGB strips, RGBW pixels, multi-channel par lights, moving heads — all addressed through the same pipeline.

![Web UI](docs/assets/ui.png)

## Getting started

### From a release

**ESP32 — flash from your browser.** Open <https://ewowi.github.io/projectMM/install/> in Chrome, Edge, or Opera, plug the device in over USB, pick your board, click Install. After flashing, WiFi builds boot a `projectMM-xxxx` SoftAP for first-time WiFi credentials; Ethernet builds get a DHCP address as soon as you plug the cable in. Four board variants ship: `esp32` (WiFi only), `esp32-eth` (Ethernet only, Olimex pin map), `esp32-eth-wifi` (both), `esp32s3-n16r8` (S3 DevKitC-1 with the N16R8 module).

**Desktop — download and run.** Grab the macOS arm64 tarball or Windows x64 zip from the [releases page](https://github.com/ewowi/projectMM/releases), unpack, run, open `http://localhost:8080/`. The macOS binary is unsigned, so Gatekeeper prompts on first run — right-click → Open, or clear the quarantine flag with `xattr -dr com.apple.quarantine ./projectMM`.

Once running, the UI lets you build a render pipeline visually (layouts → layers with effects + modifiers → drivers), preview the result in 3D, and save it. The source tree builds for Teensy, Raspberry Pi, and Linux too — see [building.md](docs/building.md). Release binaries for those targets are tracked in [docs/plan.md](docs/plan.md).

### From source

You need [uv](https://docs.astral.sh/uv/) (Python launcher), CMake 3.20+, and a C++20 compiler. For ESP32 you additionally need [ESP-IDF v6.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/).

```sh
uv run scripts/moondeck.py
```

Open `http://localhost:8420` and use the dev console: PC tab to build / run / test, ESP32 tab to flash, Live tab to discover devices. Full per-command reference: [scripts/MoonDeck.md](scripts/MoonDeck.md).

![MoonDeck](docs/assets/moondeck.png)

## Documentation

| Document | What's in it |
|----------|--------------|
| [architecture.md](docs/architecture.md) | How the system is put together — core runtime + light domain, pipeline, memory, parallelism |
| [coding-standards.md](docs/coding-standards.md) | How code in this repo is written — conventions, file shape, static checks |
| [building.md](docs/building.md) | How to build and flash for every supported target |
| [testing.md](docs/testing.md) | What tests exist and what they cover |
| [performance.md](docs/performance.md) | Per-module timing, memory, sizeof — per platform |
| [moonmodules/](docs/moonmodules/) | One spec page per module — [core](docs/moonmodules/core/) services and [light](docs/moonmodules/light/) effects, layouts, modifiers, drivers |
| [CLAUDE.md](CLAUDE.md) | Rules, constraints, and development process |

## How we work

projectMM is built with AI agents under tight human direction — the **product owner** decides what to build, reviews every line and every spec, and controls what gets committed. Agents write code in defined roles; they don't make decisions.

Meet the team: 🤖 Architect designs, 👽 Developer implements, 👾 Reviewer checks before merge, 🛸 Tester verifies, 💀 Runner does quick build and check passes. Full team descriptions in [CLAUDE.md](CLAUDE.md).

A few principles run through everything:

- **Common patterns first** — recognisable practice across code, docs, tests, UI. Bespoke choices need a stated reason.
- **Specs before code** — a module is documented in [`docs/moonmodules/`](docs/moonmodules/) — purpose, controls, behaviour, edge cases, prior art — well enough to implement from before it's written.
- **One capability at a time** — each change is small, tested, produces visible output.
- **Minimalism** — flat, predictable code; removing code beats adding it; every addition pays for itself.
- **The system as it is** — code and docs describe the present; git history is the changelog.

The full rules and process are in [CLAUDE.md](CLAUDE.md).

## History

This is the current iteration of years of LED / light system development. Each prior project proved ideas this one builds on:

| Project | Description | Repo |
|---------|-------------|------|
| **WLED** | Open-source LED firmware (user / contributor since 2021) | [Aircoookie/WLED](https://github.com/Aircoookie/WLED) |
| **WLED-MoonModules** | WLED fork with advanced features | [MoonModules/WLED](https://github.com/MoonModules/WLED) |
| **StarLight** | Standalone LED firmware | [ewowi/StarLight](https://github.com/ewowi/StarLight) |
| **MoonLight** | Ground-up build: 60+ effects, memory-optimised mapping, 11 driver types | [MoonModules/MoonLight](https://github.com/MoonModules/MoonLight) |
| **projectMM v1** | First agentic build: proved the MoonModule pattern, 8 releases | [ewowi/projectMM-v1](https://github.com/ewowi/projectMM-v1) |
| **projectMM v2** | Lock-free buffers, multi-core scheduling, canvas UI | [ewowi/projectMM-v2](https://github.com/ewowi/projectMM-v2) |

Their lessons and proven patterns are distilled in [`docs/history/`](docs/history/) — the codebase this project cherry-picks from, never porting wholesale.

## Contributing

projectMM is a community project — built in the open, shaped by the people who use it. We'd love to hear from you:

- **Ideas and requests** — an effect, a layout, a driver, a fixture you want supported? [Open an issue](https://github.com/ewowi/projectMM/issues) and tell us.
- **Help build it** — pick something from the [issues](https://github.com/ewowi/projectMM/issues), or propose a MoonModule. See [How we work](#how-we-work) for the process.
- **Test on hardware** — run it on your panels, boards, and fixtures, and report what works and what doesn't.
- **Talk to us** — questions, show-and-tell, and design discussion on [Discord](https://discord.gg/TC8NSUSCdV).

Find the MoonModules community on [Discord](https://discord.gg/TC8NSUSCdV), [Reddit](https://reddit.com/r/moonmodules), [YouTube](https://www.youtube.com/@MoonModulesLighting), and [GitHub](https://github.com/MoonModules).

## License

See [LICENSE](LICENSE).
