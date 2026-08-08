# Core control

The device's control surface — the place that says "put the device into this state", whatever asked for it. A preset applied from the grid, and later a fader moved on a MIDI desk, arrive at the same code. Its first capability is presets; the surface layout exists so external controllers map onto something that already looks like them.

`ControlModule` is a top-level module, a peer of Layouts / Effects / Drivers rather than a child of Services: it reaches *across* the top-level modules, so it cannot sit inside one.

## Control modules

<a id="control"></a>

### Control

A grid of preset pads, a row of rotary encoders above them, and a bank of faders below — the layout of a Mackie-style control desk ([X-Touch](https://www.behringer.com/product.html?modelCode=0808-AAF), [QCon Pro G2](https://www.iconproaudio.com/product/qcon-pro-g2/)), so a physical surface maps onto it without a translation layer.

<img src="../../assets/core/ControlModule.png" width="300" alt="Control module surface: encoders, preset pads, faders">

- `presets` — the pad grid (8×8). One pad per preset file; click to apply, right-click (or long-press) to name it, pick which single subtree it captures, save or delete. Drag a pad to rearrange the surface.
- `enc1` … `enc8` — rotary encoders. Drag or scroll to turn; right-click shows what each drives.
- `fader1` … `fader8` — faders. `fader1` drives `Drivers.brightness`; the rest are unassigned until bound.

Detail: [technical](moxygen/ControlModule.md)

[Tests](../../tests/unit-tests.md#controlmodule)

## Presets

A preset is a file: `/.config/presets/<name>.json`. Saving writes one, applying reads one, deleting removes one. Nothing else holds preset state, so there is no second copy to keep in step: the list is rebuilt from the folder rather than persisted alongside it. That rescan runs at startup and after every save, rename and delete — a reorder only rewrites the affected files and re-sorts the rows in place, since the folder's contents have not changed. So a preset added or removed through the File Manager appears once the module next rescans (a reboot, or a save, rename or delete on the surface), not the instant the file lands.

The name becomes the file name, so it is restricted to printable ASCII without `/`, `\` or `.` — a validator on the control, which every write path runs. `slot` records which pad the preset occupies, so a surface arranged to match a physical desk survives a reboot.

### What a preset carries

A preset captures **exactly one** top-level subtree, recorded in the file:

```json
{
  "slot": 12,
  "captures": "Effects",
  "Effects.enabled": true, "Effects.0.type": "Layer", "Effects.0.0.type": "NoiseEffect"
}
```

Each captured subtree is exactly the bytes the persistence engine already writes for that module, namespaced under a `<TypeName>.` key prefix. Save and restore therefore reuse the engine that reconciles a tree against JSON ([`saveSubtreeTo` / `applySubtree`](moxygen/FilesystemModule.md)) rather than a second serializer that could drift from it.

One subtree per preset is the whole model: a preset is *a look*, or *a geometry*, or *a hardware setup*, or *a service configuration. Never a combination. A `Effects` preset is a look, and applies to a board with completely different hardware; a `Drivers` preset carries pin maps and is device-specific. Choosing the role is a single radio button when saving, and the pad's color says which role it holds.

A preset naming a subtree this build does not have is refused with a reason rather than partially applied, and a file written by an older build that names several subtrees is listed but not applied, so it can be seen and deleted rather than silently vanishing. A malformed file leaves the live tree untouched.

### One active preset per role

Each subtree is a **role**: layout, effects, driver, service. A preset holds its own role and leaves the other three alone, so a layout preset and a look can be active at the same time, and applying a new look replaces only the look.

A pad is tinted by its role: layout blue, effects violet, driver green, service amber.

### Applying is a rebuild

Applying a preset creates, replaces and destroys modules to match what the file describes — it is a restore, not a value overlay: a preset carrying more than the device has adds it, and one describing less removes what it omits.

Structural mutation quiesces the render worker, and mutations run inline on the render tick, so a large restore stalls rendering for its duration. The captured subtree is applied and `prepareTree()` runs once at the end. Presets are a cold-path feature; the tick path is untouched.

## Home Assistant

Looks reach Home Assistant two ways, and only `Effects` presets travel either of them.

**The WLED integration** (`/presets.json`) is the native path: HA renders looks in its own preset dropdown, shows which one is applied, and applies one when it is chosen. This is what HA calls a preset.

**MQTT discovery** publishes the same looks as the light entity's **effect list**. HA has no preset concept over MQTT, so they arrive as effects — the same result from the user's side, reached through a different mechanism.

HA caches the preset list and re-fetches only when the device's `info.fs.pmt` value changes, so the device reports a revision counter there that bumps on every preset save, rename and delete — a counter rather than a timestamp, so two changes inside one second still read as two. A constant there leaves HA showing the list it read at setup forever; over MQTT the same revision re-announces the effect list mid-session.

Only looks are exposed, on both paths. A `Drivers` or `Layouts` preset rewires pins or geometry, which must not be reachable from something that believes it is choosing a color scheme — the restriction is enforced at the apply entry point, not merely by omitting them from the list.

Home Assistant's WLED integration connects on **port 80 only**: its host field rejects a port, so a desktop build (which defaults to 8080) needs `--port 80`, and that needs root:

```sh
sudo uv run moondeck/run/run_desktop.py --port 80
```

The discovery buffers are sized to the looks this device actually has, and grow or shrink as presets are added and removed. There is no cap on the number: a fixed one would either reserve memory a small setup never uses, or silently publish nothing once the list outgrew it.
