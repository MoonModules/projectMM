# projectMM

Drive large LED installations and DMX lighting from ESP32, Teensy, Raspberry Pi, Windows, macOS or Linux desktop. One source tree, multiple targets.

![Web UI](docs/assets/screenshots/ui_overview.png)

## What you get

- **Plug in, open a browser, see lights.** A live 3D preview of every effect, every modifier, every layout, controllable from the same browser tab.
- **Effects, modifiers, layouts, drivers** — all pluggable, all configurable live, all persisted across reboots.
- **One firmware, many devices.** ESP32, Teensy, Raspberry Pi, Windows / macOS / Linux desktop — the same source builds for each.
- **Native 3D** from the start. 2D and 1D are the cases where one or two dimensions are size 1; effects don't pick a mode.
- **Built-in browser UI.** The interface renders any module from its declared controls — adding a new effect needs zero UI code.
- **DMX and addressable LEDs in the same setup.** RGB strips, RGBW pixels, multi-channel par lights, moving heads — all addressed through the same pipeline.

## Performance

What projectMM delivers — pipeline: NoiseEffect → MirrorModifier XY → ArtNet over Ethernet, captured per-grid-size on each supported platform. **FPS shown is computed from the underlying tick measurement (FPS = 1,000,000 / tick_us)** — tick is the unit the contracts and assertions actually use; FPS is the headline number.

Every measurement below comes from a real scenario run on the listed board — `test/scenarios/light/scenario_GridLayout_grid_sizes.json` is the canonical sweep. Per-step `contract.<target>` blocks carry the promises the device must hit; per-step `observed.<target>` blocks carry the latest reading.

### Frames per second

| Grid | Lights | Apple Silicon (M-series) | Olimex `esp32-eth-wifi` | Olimex `esp32-eth` |
|---|---:|---:|---:|---:|
| 16×16 | 256 | — *(below host clock resolution)* | 1,543 | 1,628 |
| 32×32 | 1,024 | 166,667 | 447 | 432 |
| 64×64 | 4,096 | 40,000 | 81 | 71 |
| 128×128 | 16,384 | 9,708 | 11 | 10 |

### Free heap

| Grid | Apple Silicon (M-series) | Olimex `esp32-eth-wifi` | Olimex `esp32-eth` |
|---|---:|---:|---:|
| 16×16 | unlimited | 150 KB | 178 KB |
| 32×32 | unlimited | 144 KB | 171 KB |
| 64×64 | unlimited | 119 KB | 146 KB |
| 128×128 | unlimited | 104 KB | 132 KB |

Build variants differ structurally: `esp32-eth-wifi` includes the WiFi stack (~270 KB flash, ~28 KB heap). `esp32-eth` drops WiFi for ~28 KB more free heap, at the cost of slightly slower tick on large grids (lwIP buffer-pool sizing is tuned for the eth-wifi sdkconfig). The right variant depends on whether the deployment needs WiFi.

The numbers above are observations. The **contracts** projectMM commits to — what the device must hit on every CI run — live in [`test/scenarios/*.json`](test/scenarios/) as per-step `contract.<target>` blocks; see [docs/testing.md § Performance contracts](docs/testing.md#performance-contracts-contracttarget) for how they're set and renegotiated. The [docs/performance.md](docs/performance.md) page covers the *why* (WiFi vs Ethernet physics, sizeof tables, build-variant deltas).

## Getting started

### From a release

**ESP32 — flash from your browser.** Open the [web installer](https://ewowi.github.io/projectMM/install/) in Chrome or Edge — it walks you through firmware selection, flashing, and network setup. The installer lists stable releases and a `latest` build (published automatically on every merge to main) carrying the newest unreleased changes, labelled *(beta)*.


![Installer](docs/assets/screenshots/installer.png)

**Desktop — download and run.** Grab the macOS arm64 tarball from the [releases page](https://github.com/ewowi/projectMM/releases), unpack, run, open `http://localhost:8080/`. The binary is unsigned, so Gatekeeper prompts on first run — right-click → Open, or clear the quarantine flag with `xattr -dr com.apple.quarantine ./projectMM`. Windows desktop binaries are blocked on the Windows platform-layer port (see [docs/plan.md](docs/plan.md)); macOS arm64 is the only desktop binary that ships from 1.0 today.

Once running, the UI lets you build a render pipeline visually (layouts → layers with effects + modifiers → drivers), preview the result in 3D, and save it. The source tree builds for Teensy, Raspberry Pi, and Linux too — see [building.md](docs/building.md). Release binaries for those targets are tracked in [docs/plan.md](docs/plan.md).

### From source

You need [uv](https://docs.astral.sh/uv/) (Python launcher), CMake 3.20+, and a C++20 compiler. For ESP32, ESP-IDF v6.x is also required — see [building.md](docs/building.md) for the full setup instructions.

Once prerequisites are in place, launch MoonDeck — the browser-based dev console:

```sh
uv run scripts/moondeck.py
```

Open `http://localhost:8420`: PC tab to build / run / test, ESP32 tab to flash, Live tab to discover devices. Full per-command reference: [scripts/MoonDeck.md](scripts/MoonDeck.md).

![MoonDeck](docs/assets/moondeck.png)


![Moondeck Pc](docs/assets/screenshots/moondeck_pc.png)

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
