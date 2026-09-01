# Plan: input services and the GPIO seam

## Context

Wiring a QuinLED Dig-2-Go (button on GPIO 0, IR on 5, I2S mic on 18/4/19) exposed that projectMM
has no way to read a GPIO as **input**. It also raised a definition question worth settling before
any code lands, because the answer decides where four upcoming features live: a hardware button, a
stage foot pedal, a USB game controller, and MoonLive scripts that talk to pins.

## What a driver is: the contradiction, and the correction

Two definitions are in the repo today and they do not agree.

- [`docs/moonmodules/light/drivers.md:3`](../../moonmodules/light/drivers.md) — "A driver sends
  lights somewhere." Output-only, light-specific.
- [`docs/architecture.md:143`](../../architecture.md) — "producers vs consumers: producers generate
  data, consumers process and output it. Effects are producers, drivers are consumers." A role in a
  dataflow, said of the light domain.

The product owner's definition is broader than both: **a driver communicates with hardware or the
network**, which explicitly includes talking to GPIOs. That is the definition this plan adopts, and
it is the better one: it describes what the code *is* (the boundary between the device and the
physical world) rather than what today's instances happen to do.

**The concrete contradiction in code.** `DriverBase` declares `virtual void setSourceBuffer(Buffer*)
= 0` — every driver is *structurally required* to consume a light buffer. So the light domain's
`DriverBase` is not "anything that talks to hardware"; it is specifically "a consumer of the light
buffer that outputs it". Every one of the sixteen drivers under `src/light/drivers/` does exactly
that. The narrow wording in `drivers.md` is therefore an accurate description of `DriverBase`, and
the disagreement is that it claims the word "driver" while describing only the light-domain
specialization of it.

**The resolution, and it is a rename of concepts rather than of code.** There are two distinct
things, and the repo needs both words:

| | what it is | where it lives | contract |
|---|---|---|---|
| **Driver** (light domain) | consumes the light buffer and outputs it | `Drivers` container, `DriverBase` | `setSourceBuffer` |
| **Service** (core domain) | a capability bridge the device provides or consumes | `Services` container, `ModuleRole::Service` | none imposed |

`services.md` already states this precisely: Services are "capability bridges the device provides or
consumes ... the core-domain twin of the light domain's Effects/Drivers". Audio and IR already live
there. Both talk to hardware; neither touches the light buffer.

So under the PO's broader definition, a light Driver and a core Service are **both** drivers in the
general sense — two families of the same idea, split by whether the light buffer is involved. The
documentation should say that, and `drivers.md` should stop implying its own definition is the only
one.

**Action:** amend `drivers.md:3` to scope its claim ("A *light* driver sends lights somewhere"), and
add a line to `architecture.md` naming the general sense and the two families. No code changes.

## Where the new inputs go: Services

A button, a foot pedal and a game controller are capability bridges the device **consumes**. They
produce no lights and never touch the light buffer, so they are Services, beside Audio and IR. This
is not a new container or a new role: `Services::acceptsChildRoles()` already returns `"service"`,
and `ModuleRole::Service` already exists.

## What they drive: the existing surface, not a new one

The [OSC plan](Plan-20260829%20-%20OSC%20control%20ingest.md) settled this and it applies unchanged:

> It does NOT own a second control surface: the pads, encoders and faders already exist, and
> duplicating them would be the split-brain the architecture forbids.

`ControlModule` is the surface (8x8 pads, 8 encoders, 8 faders), and every input reaches it through
`Scheduler::setControl` — the same primitive `/api/control`, Improv, the WLED bridge and OSC use.
So the shape is one row per transport:

```
transport                     ->  ControlModule surface  ->  setControl
  OSC   /mm/pad/3             ->  pad 3
  GPIO  pin 0 (button)        ->  pad N
  GPIO  pin N (foot pedal)    ->  pad N
  USB   gamepad button/axis   ->  pads + encoders
  IR    learned remote code   ->  (decoder first, then the same surface)
```

A button press and an OSC pad press become indistinguishable downstream, which is the point: adding
a transport must not add a control model.

**This also closes an open gap.** The backlog records that `/mm/pad/N` has no handler, so a surface
"cannot fire a preset, which is the one thing a pad grid exists for". The pad route is a
prerequisite for the button, the pedal and the gamepad alike: all three want to fire a pad. Build it
once, here.

## The GPIO seam

`platform.h` already carries a well-built GPIO vocabulary, and it is the industry-standard shape:
`gpioCapability(gpio)` (static truth: valid / output-capable / RTC / strap / reserved, from the
IDF's own `GPIO_IS_VALID_GPIO`, `GPIO_IS_VALID_OUTPUT_GPIO`, `rtc_gpio_is_valid_gpio`) and
`gpioLiveState(gpio)` (what a pin is doing now). `PinsModule` is the ownership map.

What is missing is an **input role**: `gpioLiveState` reads the pad, but it is a diagnostic sampled
on tick1s, not an input path. Two functions close it:

```cpp
// Configure one GPIO as an input with an optional internal pull. Idempotent; re-configuring a pin
// the caller already owns is not an error. Returns false on a pin gpioCapability rejects.
bool gpioInputBegin(uint8_t gpio, GpioPull pull);
// Read one configured input. Cheap enough for a 20 ms poll; no allocation, no blocking.
bool gpioRead(uint8_t gpio);
```

Plus `gpioWrite(uint8_t, bool)` for the output half, which the Dig-2-Go's relay needs and which
MoonLive's hello-world requires (below). Desktop implements all three against the existing test-seam
pattern (`setTestGpioLiveState`), so button logic is host-testable without hardware.

**Debouncing belongs in the module, not the seam.** The seam reports the pad; a bouncing contact is
a property of the switch, and the module owns the time constant as a control. This keeps the
platform layer a thin, faithful boundary, which is what the platform rule asks for.

## The modules

**`ButtonService`** (core, Service). Declares its pin with `controls.addPin("pin", pin_)` — the
established convention every LED driver already uses, which gives live reconfiguration and the
PinsModule ownership entry for free. Controls: `pin`, `pull` (up/down/none), `activeLow`,
`debounceMs`, and the action (which pad it fires, or a direct module/control target). A press routes
through `setControl` like everything else.

**Foot pedals** are `ButtonService` instances. A stage pedal is electrically a momentary switch on a
jack: nothing new is needed beyond the module above, and the plan should say so rather than invent a
`PedalService`. What a pedal *does* want, and a wall button does not, is **momentary vs latching**
as a control: hold-to-activate versus press-to-toggle. That is one control on `ButtonService`.

**`GamepadService`** (core, Service) is genuinely new work and is scoped separately below: USB Host
is a different transport with real constraints.

## MoonLive: scripts that talk to pins

The product owner's direction is that MoonLive gains **driver scripts**, whose hello-world is "read
from GPIO, write to GPIO", and that a precompiled equivalent always exists alongside.

That makes the GPIO seam load-bearing in a second way: `gpioRead` / `gpioWrite` become **MoonLive
builtins**, and `ButtonService` becomes the precompiled sibling of a script anyone could write. This
is exactly the relationship effects already have — `ballpit.mle` the script beside `BallpitEffect`
the compiled module — and as of the picker work the two are indistinguishable when adding one.

Two builtins, matching the seam:

```c
int  gpioRead(int pin);           // 0 or 1
void gpioWrite(int pin, int on);
```

A script's hello-world then reads:

```c
class ButtonScript {
  int pin = 0;
  int last = 0;
  void defineControls() { addControl("pin", pin, 0, 48); }
  void tick() {
    int now = gpioRead(pin);
    if (now != last) { last = now; setControl("Control", "pad1", now); }
    gpioWrite(2, now);            // mirror it on the status LED
  }
}
```

**`setControl` as a MoonLive builtin is the piece that makes scripted services possible at all**, and
it needs its own decision: it is the same primitive every transport uses, so exposing it to scripts
is consistent, but it also lets a script write any control on any module. Worth deciding
deliberately rather than as a side effect of this plan.

**A scripted service needs a host module.** Effects have `MoonLiveEffect`; a service needs
`MoonLiveService` with the same shape (a `script` control, the compile/status path, the same picker
integration). That is a small module and mostly a copy of the existing binding, but it is the thing
that turns "MoonLive can read a pin" into "a user can add a scripted service from the picker".

## Steps

1. **Docs first (no code):** correct the driver definition in `drivers.md` and `architecture.md` per
   the table above. This is the cheapest step and the one everything else refers to.
2. **The GPIO seam:** `gpioInputBegin` / `gpioRead` / `gpioWrite` in `platform.h`, ESP32 + desktop
   implementations, host tests through the existing test seam.
3. **`/mm/pad/N` handler** in `OscModule` (the backlog gap), since every input below fires a pad.
   Decide what a press means on an empty slot and whether nonzero is press-vs-hold.
4. **`ButtonService`**, with momentary/latching so it covers foot pedals. Unit tests for debounce and
   latching on the host; bench test on the Dig-2-Go's GPIO 0.
5. **Dig-2-Go catalog entry** gains its button pin (0), IR pin (5), mic pins (18/4/19) and relay (12)
   per [architecture.md:307](../../architecture.md), which puts button pins in the deviceModel. Also
   add `flashBaud: 460800` — measured: the board's bridge fails at the CLI's fast default.
6. **MoonLive GPIO builtins** + the `setControl` decision + `MoonLiveService`. Its own plan if it
   grows; noted here so the seam in step 2 is designed with it in mind.
7. **`GamepadService`** — separate plan (below).

## USB game controllers: why this is its own plan

A USB gamepad is not a GPIO. It needs **USB Host**, which on our targets means:

- **Classic ESP32 cannot do it at all** (no USB Host peripheral; it has no USB OTG). So this feature
  is S3/P4-only, and the Dig-2-Go in front of us could never use it.
- **S3/P4** have USB-OTG, and the IDF ships a USB Host stack plus a HID class driver. A gamepad is a
  HID device with a report descriptor that must be parsed to know which byte is which button/axis:
  that parsing is the real work, and it is why "support gamepads" is not a small item.
- **Desktop** would use the OS gamepad API, a completely different implementation behind the same
  seam.

The mapping half is easy once reports arrive (buttons -> pads, axes -> encoders/faders, through the
same `setControl`); the transport half is a genuine project. Scope it separately, after the GPIO
inputs land, and expect it to be S3/P4-only from the start.

## Tests

- Host: `gpioRead`/`gpioWrite` through the test seam; `ButtonService` debounce, active-low,
  momentary vs latching; the `/mm/pad/N` route with an empty and a filled slot.
- Bench (Dig-2-Go, the board in front of us): the physical button on GPIO 0 fires a pad and toggles
  the lights; IR on pin 5 learns a remote code; the I2S mic on 18/4/19 tracks real sound.
- The mic is worth calling out: it is a **digital I2S** mic and `AudioService` already supports
  exactly that with configurable SCK/WS/SD. It very likely never worked in MoonLight because it
  needs pins set, not because it needs code.

## Risks

- **`setControl` from a script** is powerful and unscoped. Decide before shipping it.
- **The definition change is documentation**, so nothing enforces it. If the two families drift
  apart again, the check is a reader noticing, not a test.
- **GPIO 0 is a boot strap pin** on the classic ESP32 (`gpioCapability` reports `strap`). A button
  there is what QuinLED shipped, and it is safe to *read*, but PinsModule will flag it; the flag is
  correct and should stay.
