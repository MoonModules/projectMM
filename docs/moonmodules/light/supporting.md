# Light supporting modules

The light-domain machinery the catalog modules (effects, modifiers, layouts, drivers) lean on — not directly user-facing. Every row links to its generated technical page (the full API, from the `.h`) and its tests. Cross-cutting rationale that no single `.h` owns lives in the prose sections below the table.

<a id="layer"></a>

### Layer

One rendering layer — an effect writes into its buffer, modifiers transform the coordinate mapping, and the layer composites onto the shared output. The unit the render loop iterates.

<img src="../../assets/light/Layer.png" width="300" alt="Layer container with a child effect">

- `blendMode` — how this layer composites onto the ones below (overwrite / alpha / additive).

Detail: [technical](moxygen/Layer.md)

[Tests](../../tests/unit-tests.md#layer)

<a id="layers"></a>

### Effects

The container of layers — composites them (blend mode + opacity per layer) into the final light buffer.

<img src="../../assets/light/Effects.png" width="300" alt="Effects container">

Detail: [technical](moxygen/Effects.md)

[Tests](../../tests/unit-tests.md#layers)

<a id="layouts"></a>

### Layouts

The container of layout modules — walks each layout's coordinates to build the physical light set the mapping consumes.

<img src="../../assets/light/Layouts.png" width="300" alt="Layouts container">

Detail: [technical](moxygen/Layouts.md)

[Tests](../../tests/unit-tests.md#layouts)

<a id="drivers"></a>

### Drivers

The container of driver modules — owns the shared driver buffer and the per-light output correction every driver applies before sending.

<img src="../../assets/light/Drivers.png" width="300" alt="Drivers container with the on/off + brightness controls">

- `on` — master power (default on). Off scales the output to black while preserving `brightness`, so on restores the exact level. The one power control every consumer drives (the UI, IR, the WLED app / Home Assistant, MQTT).
- `brightness` — global output brightness (0–255), multiplied with each driver's own `localBrightness`.
- `palette` — the active palette effects sample from.
- `multicore` — run the output stage on the second core (default on), so a frame costs `max(render, output)` instead of `render + output`. Falls back to single-core by itself if the extra frame buffer won't fit. On is the better configuration; the switch is there to A/B it.
- `renderWait` — read-only: how long core 0 waited for core 1 at the frame boundary. Near zero means render and output overlap well; a large value means core 0 is idling on a slow output stage. Shown only while `multicore` is on.

Detail: [technical](moxygen/Drivers.md)

[Tests](../../tests/unit-tests.md#drivers)

<a id="lightpresets"></a>

### LightPresets

The named channel wirings drivers reference — which channel carries Red, Green, Blue, White, or a fixture role like Pan/Tilt. Real fixtures ship read-only (the color orders, multi-channel pars, moving heads); add your own alongside them. A driver stores a preset's stable id, not its name, so renaming or reordering never breaks a reference.

- `presets` — the editable list of preset definitions. Each row: a name, a channel count, and one role picker per channel. Built-in rows are read-only; custom rows are fully editable and persist across reboot.

Detail: [technical](moxygen/LightPresetsModule.md)

### Buffer

Contiguous light-data buffer, shared between the layers that write it (effects) and the driver groups that read it. A raw `uint8_t*` so any channel layout fits — RGB, RGBW, multi-channel DMX.

Detail: [technical](moxygen/Buffer.md)

[Tests](../../tests/unit-tests.md#buffer)

### MappingLUT

Maps the virtual grid to the physical sparse light set — a radius-4 sphere becomes its 210 real lights, not the 729-cell box. The lookup effects and the preview both consume.

Detail: [technical](moxygen/MappingLUT.md)

[Tests](../../tests/unit-tests.md#mappinglut)

### Effect base

The `EffectBase` class every effect derives from — the shared surface (buffer access, dimensions, the palette) an effect renders against.

Detail: [technical](moxygen/EffectBase.md)

### Modifier base

The `ModifierBase` class every modifier derives from — transforms the coordinate mapping (mirror, rotate, multiply, …) a layer applies before rendering.

Detail: [technical](moxygen/ModifierBase.md)

### Driver base

The `DriverBase` class every driver derives from — the shared surface (the driver window, the source buffer, the output correction) a driver reads before sending its slice.

Detail: [technical](moxygen/DriverBase.md)

### Layout base

The `LayoutBase` class every layout derives from — the shared surface a layout implements to walk its coordinates into the physical light set the mapping consumes.

Detail: [technical](moxygen/LayoutBase.md)

### Slot encoder

Turns lights into WS2812 bus words: each data bit becomes three bus slots (pulse start / data / tail), and the data slot is an 8×8 bit transpose — lanes in, bit-planes out. Shared by every parallel driver, and the render loop's measured hot spot.

Detail: [technical](moxygen/ParallelSlots.md)

### Pin list

Parses the `pins` and `ledsPerPin` controls: GPIO lists, and the broadcasting rule that spreads a window over strands (empty = even split, one number = that many each, a list = one per strand).

Detail: [technical](moxygen/PinList.md)

### Parallel LED driver base

The `ParallelLedDriver` base every parallel WS2812 driver derives from — the shared body: strand slicing, the fused correct+encode, the latch pad, and the single-shot DMA transfer. Each driver adds only its peripheral's pieces.

Detail: [technical](moxygen/ParallelLedDriver.md)
