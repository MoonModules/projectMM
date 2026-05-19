# Scheduler

Domain-neutral. Manages MoonModule lifecycles and frame counting.

## Interface

- `add(MoonModule*)` — register a module (max 32)
- `setup()` — calls `addControls()` then `setup()` on all modules
- `loop()` — calls `loop()` on all modules, increments frame counter
- `teardown()` — calls `teardown()` on all modules
- `frame()` — monotonic frame counter
- `elapsed()` — milliseconds since setup

## What worked

- Simple, domain-neutral. Doesn't know about lights or lights.
- Frame counter is useful for deterministic effects.

## What needs improvement

- No priority or ordering control. Modules are called in registration
  order, but some modules need to run before others (e.g. HTTP server
  should run after render, not before).
- No concept of different loop rates (loop20ms, loop1s). All modules
  run every frame. System modules that only need 1Hz updates waste
  cycles being called at 60Hz.
- `MAX_MODULES = 32` is a compile-time limit. Fine for now but may
  need to grow.
- No dependency tracking between modules. The dirty flag propagation
  is handled externally in main.cpp.
