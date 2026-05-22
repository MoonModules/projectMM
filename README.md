# projectMM v3

A high-performance multi-platform system that drives large LED installations and DMX lighting fixtures.

## What it does

- Drives 10,000+ addressable LEDs and DMX fixtures (pars, moving heads, dimmers)
- Supports LED protocols (WS2812, APA102) and DMX/ArtNet/E1.31/DDP
- Multiple synchronized devices for large installations
- Web UI for real-time control with 3D preview
- 3D from the core — all coordinates, effects, and layouts operate in 3D space

## Platforms

- **ESP32** (primary) — ESP-IDF, no Arduino. See [rationale](docs/architecture.md#esp-idf-no-arduino).
- **Teensy 4.x** — DMA-based LED output (OctoWS2811)
- **macOS / Windows / Linux** — development, fast live testing/scenarios, high-speed processing via ArtNet/DDP, and simulation
- **Raspberry Pi** — GPIO + network output

## Architecture

The system is two layers:

- **Core** — a domain-neutral modular runtime. Everything is a **MoonModule**: effects, modifiers, layouts, drivers, and system services all share the same class structure, lifecycle, and controls. The core knows nothing about lights — it provides modules, controls, scheduling, persistence, and platform abstraction.
- **Light domain** — built on the core. The render pipeline: effects write into layer buffers → mapping LUT translates logical to physical positions → drivers output to hardware/network.

| Document | Description |
|----------|-------------|
| [CLAUDE.md](CLAUDE.md) | Rules, constraints, and development process |
| [architecture.md](docs/architecture.md) | Core architecture: the domain-neutral runtime — MoonModule, controls, scheduling, persistence, platform abstraction, build system, testing |
| [architecture-light.md](docs/architecture-light.md) | Light domain: pipeline, layouts, layers, effects, modifiers, mapping, drivers, parallelism, memory strategy |
| [plan.md](docs/plan.md) | What to build next |

## MoonModule specifications

Draft specifications for every MoonModule live in [`docs/moonmodules_draft/`](docs/moonmodules_draft/). Each spec documents the module's purpose, controls, behavior, edge cases, and **prior art** with links to source files from previous projects where the same concept was proven.

Structure mirrors the source tree:

```
docs/moonmodules_draft/
  core/                    MoonModule base, Control, Scheduler, HttpServer, UI spec
  light/                   Buffer, MappingLUT, Layer, LayoutGroup, DriverGroup, EffectBase, LightConfig
    effects/               Rainbow, Noise, DistortionWaves, GameOfLife, Sine, Ripples, Lines, ArtNetReceive
    layouts/               Grid, Wheel (sparse)
    modifiers/             Mirror (kaleidoscope 1:N), Rotate (2D)
    drivers/               ArtNet send, Preview (WebSocket 3D)
```

Specs are reviewed and promoted to [`docs/moonmodules/`](docs/moonmodules/) one at a time, just before implementation. This is how we cherry-pick — we never implement wholesale.

Currently promoted (ready for implementation):

```
docs/moonmodules/
  core/                    MoonModule, Control, Scheduler
  light/                   Buffer, MappingLUT, Layer, LayoutGroup, DriverGroup, EffectBase, BlendMap, LightConfig
    effects/               Rainbow 2D
    layouts/               Grid
    drivers/               ArtNet send
```

## History

Built on years of LED/light system development:

| Project | Description | Repo |
|---------|-------------|------|
| **WLED** | Open source LED firmware (user/contributor since 2021) | [Aircoookie/WLED](https://github.com/Aircoookie/WLED) |
| **WLED-MoonModules** | WLED fork with advanced features | [MoonModules/WLED](https://github.com/MoonModules/WLED) |
| **StarLight** | Standalone LED firmware | [ewowi/StarLight](https://github.com/ewowi/StarLight) |
| **MoonLight** | Ground-up build: 60+ effects, memory-optimized mapping, 11 driver types | [MoonModules/MoonLight](https://github.com/MoonModules/MoonLight) |
| **projectMM v1** | First agentic build: proved MoonModule pattern, 8 releases | [ewowi/projectMM](https://github.com/ewowi/projectMM) |
| **projectMM v2** | Lock-free buffers, multi-core scheduling, canvas UI | [ewowi/projectMM-v2](https://github.com/ewowi/projectMM-v2) |

Lessons learned and proven patterns from all projects are distilled in [`docs/history/`](docs/history/):
- [decisions.md](docs/history/decisions.md) — concrete actions, lessons, proven patterns to carry forward
- [moonlight-inventory.md](docs/history/moonlight-inventory.md) — PhysMap (2-byte mapping), nrOfLights_t typedef, addControl by reference, LightsHeader (LED+DMX), transition brightness
- [v1-inventory.md](docs/history/v1-inventory.md) — WebSocket UI, 3D WebGL preview, type picker, module hierarchy
- [v2-inventory.md](docs/history/v2-inventory.md) — DataBuffer (lock-free SPSC), DataRegistry, PixelEffectBase, canvas view, multi-core scheduler

## Status

Architecture and specifications phase. Implementation starting with [plan.md](docs/plan.md).

A prototype branch (`prototype-v3-cycle1`) exists with working code (effects, modifiers, ArtNet output visible on hub75 panel) from the first implementation cycle. It is kept as reference, not as a starting point.

## Developer tools

**MoonDeck** — browser-based dev console at `http://localhost:8420`. Run with `uv run scripts/moondeck.py`. Build, flash, test, monitor — all from the browser.

## Community

- [MoonModules on GitHub](https://github.com/MoonModules)
- [Discord](https://discord.gg/TC8NSUSCdV)
- [Reddit](https://reddit.com/r/moonmodules)
- [YouTube](https://www.youtube.com/@MoonModulesLighting)

## License

See [LICENSE](LICENSE).
