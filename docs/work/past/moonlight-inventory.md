# MoonLight Inventory

Throwaway reference document. Harvesting proven patterns from MoonLight (3 years of hand-tuned embedded development) for v3.

## Architecture

MoonLight uses an upstream framework (ESP32-SvelteKit) with MoonBase (core) and MoonLight (light domain) layers added on top.

- **Module** — system-level module with JSON state, persistence, REST/WebSocket endpoints. Heavy (uses ArduinoJson, shared document, SvelteKit integration).
- **Node** — lightweight base class for effects, layouts, modifiers, drivers. Minimal memory. Lives inside a VirtualLayer.
- **VirtualLayer** — logical light space with mapping table. Effects write to virtualChannels.
- **PhysicalLayer** — physical output. Owns channelsD (display buffer). compositeTo() maps virtual→physical.

## Gems to harvest

### 1. PhysMap — memory-efficient mapping (2 bytes on no-PSRAM, 4 bytes on PSRAM)

The mapping table entry (`PhysMap`) is a union that packs into just 2 bytes on no-PSRAM boards:
- `m_zeroLights` (1:0): stores condensed 554 RGB in 14 bits (for unmapped lights that still need a color)
- `m_oneLight` (1:1): stores physical index in 14 bits (max 16384 lights)
- `m_moreLights` (1:N): stores index into a secondary lookup table in 14 bits
- `mapType` in 2 bits

On PSRAM boards: 4 bytes with 24-bit indices (max 16M lights) and full RGB cache.

**Key insight**: the mapping type enum (`m_zeroLights`, `m_oneLight`, `m_moreLights`) is stored IN the entry itself, not in a separate array. This saves one byte per entry vs our v3 CSR approach (which uses a separate offsets array).

### 2. nrOfLights_t — compile-time typedef for light indices

```cpp
#ifdef BOARD_HAS_PSRAM
  typedef uint32_t nrOfLights_t;
#else
  typedef uint16_t nrOfLights_t;
#endif
```

Exactly what we defined in v3 architecture-light.md as `nrOfLightsType`. MoonLight proves it works in practice.

### 3. Node — minimal memory footprint

Node class has:
- `VirtualLayer* layer` (pointer, 4 bytes)
- `JsonArray controls` (ArduinoJson reference, 8 bytes)
- `Module* moduleControl, moduleIO, moduleNodes` (3 pointers, 12 bytes)
- `const SemaphoreHandle_t* layerMutex` (pointer, 4 bytes)
- `bool on` (1 byte)
- No std::string members (uses `Char<N>` fixed-size strings)
- Virtual functions for lifecycle (setup, loop, loop20ms, destructor)
- classSize() reports actual size for memory accounting

Total base: ~29 bytes + vtable pointer. Effects add only their control variables (uint8_t each). A typical effect like NoiseEffect2D adds just 2 bytes (scale_ + speed_) on top of the base.

### 4. addControl — binds to class variable by reference

```cpp
template <class ControlType>
JsonObject addControl(ControlType& variable, const char* name, const char* type, int min, int max) {
    control["p"] = reinterpret_cast<uintptr_t>(&variable);
    // ...
}
```

The control stores a pointer to the class variable. Hot-path code reads the variable directly. UI updates write through the pointer. No getter/setter overhead. This is exactly what v3 architecture specifies.

Supports: uint8_t, int8_t, uint16_t, uint32_t, int, float, bool, Coord3D. Type is encoded as a size code for the non-template helper.

### 5. LightsHeader — fixed-size metadata supporting LED + DMX fixtures

48-byte struct covering RGB, RGBW, RGBCCT, PAR lights, and moving heads via configurable channel offsets:
- `channelsPerLight` (3 for RGB, 4 for RGBW, up to 32 for moving heads)
- `offsetRed/Green/Blue/White` — channel positions within a light
- `offsetPan/Tilt/Zoom/Rotate/Gobo` — moving head channels
- `offsetBrightness` — separate brightness channel (PAR lights)

**Key insight**: one struct handles LEDs AND DMX fixtures by varying channelsPerLight and offsets. This is the "light = LED pixel or DMX fixture" concept from v3 architecture.

### 6. VirtualLayer — layer with start/end percentages

Layer boundaries as percentages of the total fixture:
```cpp
Coord3D startPct = {0, 0, 0};
Coord3D endPct = {100, 100, 100};
```

This maps to v3's "start/end position within the physical layout" concept. Proven in MoonLight.

### 7. oneToOneMapping / allOneLight fast paths

Boolean flags that enable fast paths:
- `oneToOneMapping`: skip the mapping table entirely (identity mapping)
- `allOneLight`: all entries are 1:1, use a direct table instead of the switch/case

These are the optimizations v3 architecture describes for no-PSRAM large layouts.

### 8. Transition brightness

Per-layer animated brightness overlay:
```cpp
uint8_t transitionBrightness = 255;
uint8_t transitionTarget = 255;
int16_t transitionStep = 0;
```

Enables smooth fade-in/out when switching effects. Not in v3 yet — should be.

### 9. SharedData — inter-node communication

A single `SharedData` struct shared by all nodes for audio sync, gravity, status:
- 16-band FFT results
- Volume, peak frequency, beat detection
- FPS, connection status
- Gravity (IMU data)

Lightweight alternative to a pub/sub system. Zero allocation.

### 10. Coord3D — rich operator overloads

Full arithmetic operators (+, -, *, /, %, +=, /=), comparison (==, !=, <), utility (maximum, distanceSquared, isOutofBounds). Uses `int` (not int16_t) — larger but avoids overflow issues in intermediate calculations.

## What NOT to harvest

- **ArduinoJson dependency** — MoonLight uses ArduinoJson everywhere (controls, state, persistence). v3 should use fixed-size types and avoid JSON in the core.
- **ESP32-SvelteKit integration** — the upstream dependency that caused 1,271 local changes. v3 has no upstream dependency.
- **Module class** — heavy, tightly coupled to SvelteKit. v3's MoonModule is simpler.
- **std::vector in mapping tables** — MoonLight uses `std::vector<std::vector<nrOfLights_t>>` for m_moreLights secondary lookup. v3's CSR flat array is better for cache and avoids nested heap allocations.

## Driver inventory

| Driver | Description |
|--------|-------------|
| D_FastLEDDriver | WS2812/APA102 via FastLED |
| D_ParallelLEDDriver | Parallel output via I2S/parlio |
| D_Hub75 | HUB75 LED panels |
| D_NetworkOut | ArtNet/E1.31/DDP output |
| D_NetworkIn | ArtNet/E1.31/DDP input |
| D_DMXOut | DMX512 output |
| D_DMXIn | DMX512 input |
| D_WLEDAudio | WLED audio sync |
| D_FastLEDAudio | FastLED audio analysis |
| D_IMU | Gyroscope/accelerometer |
| D_Infrared | IR remote control |

## Effect libraries

| File | Effects count | Source |
|------|--------------|--------|
| E_MoonLight.h | ~15 effects | Original MoonLight effects |
| E_MoonModules.h | ~10 effects | MoonModules community effects |
| E_WLED.h | ~20+ effects | Ported from WLED |
| E_FastLED.h | FastLED demo effects | From FastLED examples |
| E_SoulmateLights.h | Soulmate effects | Ported |
| E_MovingHeads.h | Moving head fixtures | DMX fixture control |

## Layout types

| Layout | Description |
|--------|-------------|
| Panel | Standard LED matrix |
| Ring | Circular ring |
| Rings241 | Concentric rings (241 LEDs) |
| HexaPanel | Hexagonal panel |
| Cloud | Irregular point cloud |
| Human | Body-shaped layout |
| Cone | Conical layout |
| Globe | Spherical layout |
| SpiralGlobe | Spiral on a sphere |
| SE16 | Specific hardware (Sound Experience 16) |

## Modifier types

| Modifier | Description |
|----------|-------------|
| Mirror | Kaleidoscope-style mirroring |
| ReverseX/Y/Z | Axis reversal |
| Transpose | Swap X and Y |
| Rotate | Pixel rotation |
| PinWheel | Pinwheel distortion |
| Kaleidoscope | Multi-axis kaleidoscope |
| ScrollingText | Text overlay |
| Brightness | Per-layer brightness with pulsing |
