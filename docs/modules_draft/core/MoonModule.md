# MoonModule

The base class for everything in the system. Effects, modifiers, layouts,
drivers, and system services all inherit from MoonModule.

## Interface

- `name()` — returns the display name
- `setup()` / `loop()` / `teardown()` — lifecycle, called by Scheduler
- `addControls()` — declare controls at init time
- `onChange(index)` — called when a control value changes

## Controls

Fixed-capacity array (MAX_CONTROLS = 16) of named, typed values:
- `Uint16` — slider with min/max
- `Bool` — toggle
- `Text` — text input (char[64], for IPs, names, etc.)

Controls are the bridge between the UI and the engine. The web UI
auto-renders them by type.

`markDirty()` / `dirty()` / `clearDirty()` — allows modules to signal
that their state changed and dependents need to update (e.g. layout
change triggers LUT rebuild).

## What worked

- Single base class for everything keeps the system uniform.
- Fixed-capacity controls avoid heap allocation.
- `markDirty()` pattern works for propagating changes.

## What needs improvement

- `onChange` only fires when values actually change (good), but the
  propagation to Layer/DriverGroup rebuild is handled ad-hoc in
  main_desktop.cpp with explicit dirty checks per module. This should
  be a proper observer/listener pattern.
- Control types are limited. Need: color picker (RGB), dropdown/enum.
- The `name()` return type is `const char*` — works but makes
  dynamic names (e.g. "Layer 1") impossible without static storage.
