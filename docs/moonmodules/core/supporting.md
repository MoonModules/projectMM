# Core supporting modules

The core machinery the UI modules lean on, internal rather than user-facing, so no controls of their own. Each row links to its generated technical page (the full API, from the `.h`) and its tests. Cross-file design rationale that no single `.h` owns lives in the prose sections below the table.

### Control

A named, typed value a MoonModule exposes to the UI: the binding between a class variable and its web-UI widget, DMX channel, and persisted value. Every module holds a list of these.

Detail: [technical](moxygen/Control.md)

[Tests](../../tests/unit-tests.md#moonmodule)

### Scheduler

Orders module `setup()` by declared init-order dependencies (WiFi before HTTP, HTTP before WebSocket) and drives the per-tick `tick()` sweep. The one place init-order lives, so modules declare a dependency instead of hard-coding boot sequence.

Detail: [technical](moxygen/Scheduler.md)

[Tests](../../tests/unit-tests.md#scheduler)

### MoonModule

The base class every module derives from, carrying the shared lifecycle (`setup` / `tick` / `release`), the controls list, child propagation, and the self-reporting footprint (`classSize` / `dynamicBytes` / `tickTimeUs`). Learn the pattern once, apply it everywhere.

Detail: [technical](moxygen/MoonModule.md)

[Tests](../../tests/unit-tests.md#moonmodule)

<a id="filesystem"></a>

### Filesystem

The persistence **engine**: writes control values to `/.config/*.json` and restores them on boot, overlaying loaded values through each control's pointer during `defineControls()`. A non-UI module (renders no card); its "last saved" status is surfaced by the File Manager. The home of the no-reboot live-reconfiguration behavior (see below).

Detail: [technical](moxygen/FilesystemModule.md)

[Tests](../../tests/unit-tests.md#filesystemmodule)

## Persistence and dynamic rebuild

Control values persist via [FilesystemModule](moxygen/FilesystemModule.md), which overlays loaded values through each control's variable pointer during `defineControls()`. Calling `defineControls()` again at runtime (e.g. when a Select changes mode) clears and rebuilds the set, so only the controls relevant to the current mode show. This is how a control's conditional `hidden` flag re-evaluates. The rebuild sweep is also how a config change applies live, with no reboot.
