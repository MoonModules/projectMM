# Plan: input mapping and scripted sensors

Turns [input-mapping-analysis.md](../../backlog/input-mapping-analysis.md) into steps. Two threads
run together throughout, deliberately:

- **Compiled modules** for the inputs a board ships with, declared in the device catalog.
- **MoonLive** as the flexible half, so a sensor nobody wrote a module for is a script a user writes
  with a datasheet.

They are not sequential phases. Every step that adds a platform seam exposes it as a MoonLive
builtin **in the same step**, because a builtin is one table row plus a host function
(`MoonLiveBuiltins_light.h`, 53 of them today) and because the script is how the seam gets tested
before any module depends on it. A step is done when both halves work on the bench.

## The sensors this plan is tested against

Agreed with the product owner, and all on the bench. **One per seam**: each proves a different
platform primitive, and each one that works unlocks the class of devices that share it. Nothing
redundant, because a second I2C sensor proves nothing the first did not.

| sensor | seam it proves | maps to | step |
|---|---|---|---|
| **Button** (the Dig-2-Go's, GPIO 0) | GPIO digital in | `switchN` | shipped, rebuilt in 1 |
| **PIR** (HC-SR501) | the same GPIO, as a *level* rather than a press | `switchN` | 6 |
| **IMU** (MPU6050) | I2C register read, a multi-register burst | `encoderN` / `faderN` | 4 |
| **ToF distance** (VL53L0X, on order) | I2C register read, the simple case | `faderN` / any value control | 4 |
| **ToF zone grid** (VL53L8CX) | I2C at a size the render tick cannot absorb | an effect or modifier, per zone | 4b |
| **Rotary encoder** (KY-040) | pulse timing / PCNT quadrature | `encoderN` + `switchN` for the push | 5 |

**Two on I2C is deliberate, and is not the redundancy the rule warns about**, because the two ask
opposite questions of the same bus. The MPU6050 is the sensor the unmerged `GyroDriver` commit
already drives, so it is the *regression* check that the merged seam still does what its author
built it for: a 14-byte burst, comfortably inside a render tick. The VL53L8CX is the *stress* case:
128 bytes at up to 60 Hz, which a blocking read cannot absorb on the render path at all. The first
proves the seam works; the second proves where it stops working, which is the more valuable of the
two and the reason it is not optional here.

**A VL53L0X is on order (2026-09-01), arriving in about two weeks.** It restores the gentle step:
one distance from one register read, so the I2C seam is exercised by a second, easy device before
the zone grid tests where that seam stops working. Until it arrives, step 4 proves the seam with the
IMU alone; when it lands it slots in as step 4's second sensor, which also removes the ambiguity
noted in 4b (a failure there being either the merged seam or that sensor's own demands).

**Every one is tested twice**, which is the point of running the two threads together: once as a
compiled module, once as a MoonLive script doing the same job. A sensor that works only one way
means the seam is not as domain-neutral as it claims.

Not on the list, and why: an **ultrasonic** (HC-SR04) shares the encoder's pulse-timing seam, so it
adds bench time without proving a new primitive; a **light sensor** (BH1750) is a third I2C device;
a **USB gamepad** needs hardware none of the boards here have.

## Two paths into the light domain, not one

Steps 1 and 2 build the mapping table, which is right for **events**: a press, a remote code, an
encoder detent. Steps 4 and 5 add **streams**, and those must not go through the same path. A
distance an effect samples every frame is not a control a person edits, and routing it through
`setControl` would put a name lookup and a persistence dirty-flag on the render path at frame rate.

The pattern for a stream exists and is already domain-neutral: `AudioService` publishes an
`AudioFrame` that effects pull through a static `latestFrame()`, which
[architecture.md](../../architecture.md#data-exchange-between-modules) states as the shared-struct
pull. A sensor service does the same, and may *also* carry mapping rows for thresholds a person
cares about ("closer than 50 cm" drives a switch). One module, two outputs, because that is what a
sensor is: a stream, with events derivable from it.

So every sensor step below has two acceptance criteria, and both are on the bench: **an effect reads
the value per frame through the published frame**, and **a threshold drives a control through the
table**. A sensor that only does one is half-built. The full reasoning is in
[input-mapping-analysis.md](../../backlog/input-mapping-analysis.md).

## Step 0: the decisions

**Decided (2026-09-01), so step 1 is unblocked:**

1. **A service per input kind.** `ButtonService`, `InfraredService`, later `EncoderService` and
   `AnalogService`, each with its own mapping list whose columns all apply. A board's catalog entry
   adds the ones it has. More modules than one `InputService` with a `type` column, but every column
   on a card is real for that input, and the card can validate what it shows.
2. **The infrared service is pure mappings.** No compiled-in actions. The five familiar ones
   (on/off, brightness up/down, palette next/prev) ship as **default rows in the device catalog**, so
   a remote still works out of the box while nothing is fixed in firmware and a user can delete or
   rewrite any of them.

3. **A script defines input hardware, and a script is where the LOGIC goes** (product owner,
   2026-09-02). Once a script can read a pin and call `setControl`, a script IS an input module, and
   the relationship to `ButtonService` is the one effects already have: the compiled module is the
   fast, shipped path, the script is the flexible one for hardware nobody wrote a module for.

   **Not a JSON.** A JSON can express a MAPPING (this pin drives that control) and that is exactly
   what the mapping lists in step 1 already are. It cannot express a RULE ("if the distance sensor
   reads under 50 cm, run preset 3"), and a JSON that grows conditionals, comparisons and sequencing
   becomes a programming language with none of the tools of one. So the declarative half stays the
   lists, and MoonLive is where an `if` belongs. The two compose: a script writes the surface, the
   surface is what the lists already target.

4. **One script per sensor, or one script for all of them: both, with no new mechanism.** A
   `MoonLiveService` instance holds one script, so one instance per sensor type is the tidy default
   and one instance driving several is a script with more members. This is the same choice a user
   already has with effects, and it needs no `type` column or registry to support.

**Still open, and it gates step 2 rather than step 1:**

5. **What `setControl` may reach from a script.** Recommendation: **the `Control` module only**. A
   script drives the surface, the surface drives everything, which is the same two-step model the
   tables use. Unrestricted is simpler and lets a script rewrite a driver's pin list.

## Step 1: rebuild Infrared and Button around a mapping list

**This is the first work, and it is a rebuild rather than an extension.** Both modules are wrong in
the same way, and both are new enough on this branch to change without a migration: `ButtonService`
maps one pin to one target, and `IrService` carries five compiled-in actions whose identity is the
firmware's opinion. Neither survives a board with three buttons or a remote with twenty keys.

**`IrService` is renamed `InfraredService`.** "IR" is an abbreviation the code does not need: the
project's own standard is the textbook name a new contributor reads without expanding it
([CLAUDE.md, industry standards](../../../CLAUDE.md)), and every sibling is already spelled out
(`AudioService`, `ButtonService`, `FilesystemModule`). The rename touches the class, the file, the
factory registration, the catalog entries that add it per board, and its spec anchor. Cheap now, and
it stops the abbreviation spreading to the doc, the UI card and the device catalog.

Both then grow **a list of mappings** via `Control::addList` + `addListRow`, the mechanism presets,
devices, pins and tasks already use:

- A **button row**, as built: `pin`, `activeLow`, and the shared action (`target`, `kind`, `value`).
  `kind` is toggle / set / delta, where `set` is the momentary one (writes while held, clears on
  release) and covers a foot pedal. No long-press: it needs a held-timer state machine
  `ButtonService::pollRow` does not have, so it is step 6's trigger vocabulary rather than step 1's.
  A board with three buttons is three rows, which is the whole point.
- An **infrared row**: `code` (learned), plus the same shared action. Learning binds the
  next received code to the row being learned, so a remote's twenty keys are twenty rows a user adds
  rather than five the firmware chose.
- `target` is a `Module.control` string, so `Control.switch1` (the recommended two-step path) and
  `Drivers.on` (direct) are the same mechanism with no special case.

The single-target controls that shipped (`ButtonService.target`, `IrService`'s five `code …`
controls) are **removed, not deprecated**: a one-row list is the same thing, and this branch is the
only place they have ever existed.

**MoonLive in this step:** nothing new. The GPIO builtins arrive in step 2; here a script cannot yet
read a pin, so the table is the only path. Stated so the step is not held up waiting for it.

**Test:** host tests for the row parsing and the debounce state machine,
and a test that the rename left no `IrService` behind. Bench: the Dig-2-Go's GPIO 0 button on one row
and the infrared remote on another, both driving `Control.switch1`, so two inputs reaching one target
is proven rather than assumed.

## Step 1b: an encoder that shows what it selects

Small, and it follows step 1 rather than joining it, so the rebuild is not also carrying a UI change.

Infrared's palette actions need a destination. There is no `LightsControl` module and there will not
be one ([backlog-mixed.md](../../backlog/backlog-mixed.md)): `palette` stays on `Drivers` because
that is where it is consumed, and the surface reaches into it, exactly as `fader1` already targets
`Drivers.brightness` and `switch1` targets `Drivers.on`.

- **`encoder1` targets `Drivers.palette`**, hardcoded for now. That matches the existing pair and the
  backlog's own note that per-control assignment is a later UI plus persistence job
  ([power-functions-analysis-top-down.md](../../backlog/power-functions-analysis-top-down.md)).
- **Wrap or clamp follows the bound control's type**: a palette ring wraps, a brightness clamps.

A `uint8_t` encoder covers 256 options, comfortably past the palette count. Worth stating as the
boundary, since a preset encoder would reach 64 slots and a script list could one day exceed 255.

### The display strip

Turning a knob through palettes has to read "Rainbow", not "37", and the place to show it is **one
shared display row, not a label per encoder**. That is how the hardware this surface mirrors works:
a strip of small text displays across the top, showing whatever was last touched.

- **One row, above the switches.** Declaration order is render order in `defineControls`, so the
  strip is one `addControl` before the switch loop. Above them because a channel reads
  display, then buttons, then knob, then fader on the desks this mirrors, and because it then sits
  with the numeric readouts rather than floating between banks.
- **Any control that has text can drive it.** Not an encoder feature: a select's option name, a
  preset's name, a script's status. The rule is "a surface control with a text form writes the
  strip", so a new bound control needs no display code.
- **It shows the LAST thing set.** No per-control cell to keep in sync, no question of what a stale
  cell means, and it matches what a performer wants: the thing they just touched.

**Hardware and Open Stage Control.** Some surfaces have these displays (the X-Touch's scribble
strips) and some do not, so the strip is published as a control like any other and a surface that
cannot show text simply ignores it. The shipped
[Open Stage Control session](../../reference/examples/open-stage-control.json) gains a text widget
bound to it, which is also worth doing because that session already has known gaps (its widget labels
do not render and its pad matrix draws nothing, both recorded in
[backlog-core.md](../../backlog/backlog-core.md)): adding the strip is the moment to fix the session
by building one widget of each kind in its own editor and copying the shape it produces.

**Test:** the infrared remote's palette next/prev rows driving `Control.encoder1`, with the palette
NAME appearing in the strip and the lights following. That is the whole two-step model in one
observation: a remote code reaches the surface, the surface reaches Drivers, and the strip says what
happened.

## Step 2: MoonLive at the pins

The GPIO seam shipped (`gpioInputBegin` / `gpioRead` / `gpioWrite`). This exposes it to scripts and
adds the host module that makes a scripted service addable.

- Builtins: `int gpioRead(int pin)`, `void gpioWrite(int pin, int on)`. One row each in
  `MoonLiveBuiltins_light.h` plus a host function.
- `setControl` as a builtin, scoped per step 0's decision.
- **`MoonLiveService`**: the service twin of `MoonLiveEffect`. A `script` control, the compile and
  status path, picker integration. Mostly a copy of the existing binding.

**The hello-world**, which is also the test:

```c
class ButtonScript {
  int pin = 0;
  int last = 0;
  void defineControls() { addControl("pin", pin, 0, 48); }
  void tick20ms() {
    int now = gpioRead(pin);
    if (now != last) { last = now; setControl("Control", "switch1", now); }
  }
}
```

**Test:** host tests for the builtins through the desktop seam (`setTestGpioLevel` injects a level,
the script reads it). Bench: the script above on the Dig-2-Go toggling the lights from the physical
button, doing in eight lines what `ButtonService` does in a module. That comparison is the point:
**the compiled module and the script are interchangeable**, which is the relationship effects
already have.

**And the rule, not just the mapping.** The step is only finished when a script can express what a
list cannot, which is the reason scripts are in this plan at all (step 0, decision 3). The second
hello-world is a CONDITION driving an action:

```c
class NearTrigger {
  int pin = 0;
  int threshold = 50;
  int wasNear = 0;
  void defineControls() {
    addControl("pin", pin, 0, 48);
    addControl("threshold", threshold, 0, 255);
  }
  void tick20ms() {
    int near = readDistance(pin) < threshold;
    if (near != wasNear) { wasNear = near; if (near) setControl("Control", "pad3", 1); }
  }
}
```

A mapping row cannot say "under 50 cm", cannot hold the edge state that stops it firing every tick,
and cannot pick a different pad by time of day. That is the whole argument for a script over a
richer JSON, and this test is what proves it rather than asserting it. Until step 4 lands the I2C
read, the same shape is tested with the button (`gpioRead`) standing in for the distance.

### Step 2 status (2026-09-02): built and host-verified, NOT bench-verified

Shipped: the `gpioRead` / `gpioWrite` / `setControl` builtins, a `.mls` extension with its template
and catalog entry, `MoonLiveService` registered under Services, and `unit_MoonLiveService.cpp`
pinning the whole path (a script reads an injected pin level and drives `switch1`, declares its own
controls, and survives a missing or broken script).

Still open, and the step is not done until they are:

- **The bench test.** A green host run is not verified: the script has never executed on an ESP32,
  where the JIT emits Xtensa rather than arm64 and the pin is a real button. The Dig-2-Go has one on
  GPIO 0 and `moonlive/services/button.mls` is written for it.
- **The `.mls` picker.** The extension map and syntax highlighting are wired; nobody has confirmed
  the picker offers `.mls` files or that "new script" writes `kServiceTemplate`.
- **The catalog card.** `services.md` has no MoonLiveService section, and `@card
  MoonLiveService.png` names a file that does not exist.

Two things this step found, which the text above predates:

- **The hello-world in this plan does not compile as written.** MoonLive has no local variables, so
  `int now = gpioRead(pin)` is a parse error; the value has to be a member. And `setControl` takes
  two arguments (`"switch1"`, value), not three: the module is fixed to `Control` by step 0's
  decision, so naming it at the call site would offer a choice that does not exist. The shipped
  `kServiceTemplate` is the corrected form.
- **`MoonLive::run()` refuses a call with no light buffer** (`!buf || nLights == 0 || cpl < 3`),
  which is right for an effect and wrong as the engine's only entry: a service paints nothing, and
  the guard rejected it SILENTLY, so the script compiled, reported its size and never ran a line.
  `MoonLiveService` calls `runValue` instead, which invokes the same emitted block and asks only for
  the arena. Correct but oblique: the main entry still carries a light-domain assumption, and the
  next non-light binding meets it the same way. Worth splitting the guard out of `run()`, or naming
  the two entries for what they are.

## Step 3: analog in, and the expression pedal

An ADC read is the smallest new seam and unlocks a whole input class.

- Seam: `bool adcRead(uint8_t gpio, uint16_t& raw)`, plus the calibration ESP32 needs to turn a raw
  count into millivolts.
- Compiled: `AnalogService`, a list of rows mapping an ADC pin to a `faderN` with min/max/invert,
  because a pedal's usable travel is never the full range.
- MoonLive: `int adcRead(int pin)`.

**Test:** host tests with an injected value. Bench: a potentiometer on an ADC pin driving
`Control.fader1`, first through `AnalogService` and then through a three-line script, so both halves
are proven on the same hardware.

## Step 4: I2C sensors

The prerequisite is not new work: an unmerged commit (`11f8eb76`, "Add GyroDriver (MPU6050) +
generic platform I2C layer") already carries the seam, and this branch has only `i2cScan`, which
cannot read a register.

- **Merge that layer first**, verifying it against the current tree rather than assuming it applies:
  `i2cInit`, `i2cWriteReg`, `i2cReadRegs`. The commit also carries a working **MPU6050** module,
  which is on the bench, so the merge has its own regression test built in: if the gyro still reads,
  the seam survived the merge.
- Its `GyroDriver` moves from `Drivers` to `Services` while it lands. It reads a sensor and outputs
  no lights, so `Services` is its home under the split this analysis rests on, and `DriverBase`'s
  pure `setSourceBuffer` would oblige it to consume a light buffer it has no use for.
- MoonLive: `int i2cRead(int addr, int reg)`, `void i2cWrite(int addr, int reg, int val)`.
- The second device that would prove the seam generalizes is **step 4b**, since the only ToF on the
  bench is the zone grid. So this step ends with one sensor and one script, and the generalization
  claim is not yet made.

Each publishes a frame for effects to pull, following `AudioService`: a POD struct in a plain-data
header, a static accessor, overwritten in place. A `SensorFrame` shared by both is the first question
to answer when building it, since a distance and a tilt are different shapes; the alternative is one
per sensor, which is more headers and less coupling.

**MoonLive needs to read that stream too**, or a script can drive a pin but not react to a sensor.
The builtin is a read of the published frame (`distance()`, `tilt()`, or a general
`sensor(int which)`), which is the same shape as the audio builtins scripts already have.

**Test:** host tests for the register decode against a faked bus. Bench, on the Dig-2-Go's exposed
I2C pins (21/22): the MPU6050 reporting tilt, which is the regression check that the merged seam
still does what its author built it for. Both acceptance criteria: **an effect reacting to tilt every
frame** through the published frame, and **a threshold row** driving a switch past a tilt angle.
**Then the same sensor as a script** (init registers in `setup`, the burst read in `tick20ms`, the
value published for an effect to pull), which is the proof of the scripted-sensor claim: a user with
a datasheet and no toolchain gets a working sensor an effect can use.

## Step 4b: the sensor that produces an image

**Not optional, and not a follow-up.** The VL53L8CX is the only ToF on the bench, so it is both the
second I2C device (proving the seam generalizes) and the case that proves the seam is not enough.
Those were meant to be two sensors and are now one, which makes this step harder than it reads:
a failure here is ambiguous between "the merged seam is wrong" and "this sensor needs more than a
synchronous read". Establish the MPU6050 works first (step 4), so anything that breaks here belongs
to this sensor.

It reports an **8x8 zone grid at up to 60 Hz**, so it is a depth image rather than a distance. The
constraint is not bandwidth (128 B/frame is 7.7 kB/s) but the **blocking read**: 128 bytes over I2C
at 400 kHz is ~2.9 ms, against a 289 us render tick on the Dig-2-Go. So the synchronous
`i2cReadRegs` from step 4 is not sufficient here, and this step is where that becomes visible.

Three ways out, to be chosen with the measurement in hand: **SPI at 3 MHz** (which is why the part
offers it), a **ranging rate matched to what the effect needs** rather than the sensor's maximum, or
the read **moved off the render core** as the encode split already is. Measure first: the plan should
not guess which.

It also carries an **84 KB firmware blob** uploaded to the sensor at every boot (measured from ST's
driver: 86,017 bytes, 1.94 s over I2C at 400 kHz, 0.23 s over SPI at 3 MHz). That blob cannot be
written ourselves, since it is executable firmware for the sensor's own processor with no
register-level datasheet published. Two consequences: **SPI is preferred** for the boot upload as
well as the per-frame read, and the module is **compiled in per firmware** rather than into every
image, gated by the device catalog the way `MM_HLS` already is. 84 KB is roughly 4.5% of the ~1.8 MB app PARTITION (about 2% of a 4 MB chip's flash) and
matters on a 4 MB classic; it is noise on an S3 or P4.

The consumer is an effect or a modifier that maps zones onto lights, so the published frame is an
array rather than a value. That argues a frame type per sensor family (as `AudioFrame` is specific
to audio) rather than one general struct.

**MoonLive is in scope for this sensor, through a power function.** A script cannot upload the blob
and never should, but it does not need to: the compiled module owns the blob, the bus and the frame,
and the script reads a zone through a builtin the way it already calls `beat()` or `paletteR()`
without implementing either. So this step ships a builtin alongside the module:

```c
int tofZone(int x, int y);   // distance in mm for one of the 8x8 zones, 0 = no target
```

A script that lights the column nearest a hand is then a few lines, and the heavyweight sensor is as
scriptable as the light ones. The boundary the blob draws is narrower than it first looked: a script
cannot *be* the driver for such a sensor, but it can *use* one fully.

## Step 5: pulse timing, and what it unlocks

Two wanted inputs share one missing seam: an ultrasonic sensor's echo *width*, and a rotary
encoder's quadrature. Both are edge timing, and the textbook ESP32 answer for both is a hardware
peripheral (PCNT for the encoder, RMT or a timed capture for the echo) rather than a poll.

- Seam: pulse measurement, shaped by whichever of the two is built first.
- Compiled: `EncoderService` (a row is two pins plus an optional push pin, mapping to `encoderN`),
  and an ultrasonic distance module.
- MoonLive: `int pulseIn(int pin, int level, int timeoutUs)`, the Arduino name because it is the one
  every datasheet example uses.

**Test:** an encoder on the bench driving `Control.encoder1` through the module, and an HC-SR04
reporting distance through a script.

## Step 6: PIR, and the trigger vocabulary

The cheap finish. A PIR is a digital pin, so it needs no seam at all: it is a `ButtonService` row
whose event is a *level* rather than a press. MoonLight models it exactly so (`pin_PIR`,
"HIGH = lights on"). Add `level` to the event set, and PIR is done.

If the trigger vocabulary is going further (double-click, multi-click, from ESPHome's set), this is
where it lands: the same timers, more states, and it multiplies what a three-button panel can do.

## Worth an analysis of its own: modules exchanging values over the network

**The question (product owner, 2026-09-02): AudioService syncs its frame between devices, so can any
module send and receive its values the same way?**

Worth taking seriously, and deliberately NOT folded into a step here, because it is a network
question rather than an input one. Audio sync is a single hardcoded packet (`parseWledAudioSync`, a
44-byte WLED v2 frame) carrying one struct between devices that both know what it means. Generalizing
that means a module publishing arbitrary state to a peer group and another subscribing to it, which
needs answers this plan has no reason to hold opinions about: what identifies a value across devices,
what happens when two devices publish the same one, how a subscriber behaves when the peer goes away,
and whether it is a broadcast or a subscription.

It also overlaps two things that already exist: `DevicesModule` already discovers peers and knows
what modules they run, and OSC and MQTT already carry control values off the device. So the honest
first question is whether this is a new transport at all, or a matter of pointing the existing ones
at each other.

It touches inputs at exactly one point, which is why it came up here: a sensor on one device driving
lights on another is the obvious use, and the two-step model already makes that expressible (a remote
device's surface is just another target). That is enough to note; the design belongs in its own
analysis alongside the backlog's existing network entries.

## Not in this plan

- **USB game controllers**: a HID report-descriptor parser plus USB Host bring-up, S3/P4 only.
  Its own plan, and the mapping half is free once reports arrive.
- **I2S sensors**: the platform's I2S surface is `audioMicInit`/`audioMicRead`, named for its job.
  A non-audio I2S sensor would get its own seam named for its own job rather than generalizing that
  one. There is also a hardware limit: a classic ESP32 has two I2S controllers and the mic holds one.
- **Rewriting OSC**: it already drives the surface, which is the model this plan extends to physical
  inputs. Its one gap, the missing `/mm/pad/N` handler, is worth doing alongside step 1 since every
  input wants to fire a pad.

## Why the order

Each step is shippable alone and each unlocks the next. 1 fixes a defect in what already shipped.
2 makes scripts real, and every later step then has two implementations to test against. 3 is the
smallest new seam. 4 has its prerequisite already written by someone else. 5 is the most work
(hardware peripherals) so it comes after the pattern is established. 6 is nearly free.

The MoonLive thread is not a phase at the end: a builtin lands with the seam that makes it possible,
so a script is always the second implementation proving the seam is genuinely domain-neutral. If a
seam is awkward to expose as a builtin, that is a signal the seam is wrong.
