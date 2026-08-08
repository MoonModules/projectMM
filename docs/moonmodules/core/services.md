# Core services

The user-added **Service** modules — capability bridges the device provides or consumes, added and removed at runtime in the `Services` container (the core-domain twin of the light domain's `Effects`/`Drivers`). Fixed device infrastructure (identity, network, inspection tools) lives under **System** — see [core/system.md](system.md). Every row links to its generated technical page (the full API, from the `.h`) and its tests.

<a id="services"></a>

## Services

The top-level container the Service modules hang under — a grouping node with no controls of its own, the same shape as `Effects`/`Drivers` in the light domain. Adds/removes its children (Audio, IR) at runtime via the generic module machinery.

Detail: [technical](moxygen/Services.md)

<a id="audio"></a>

### Audio

A Service (added by the user, not auto-wired): the audio source that feeds the FFT audio-reactive effects consume via `AudioService::latestFrame()`. `mode` is the first choice, the module's identity, and each mode shows only its own detail controls: Local audio runs its own peripheral (an I²S microphone or line-in ADC) and analyzes it locally, Receive network is a pure network sink a peer's WLED-compatible audio drives, and Simulate is a synthesized source for demos and tests. Idle until real GPIOs are entered in Local mode. The Receive network mode and every network-sync control (`send audio`, `syncPort`, `sync status`) exist only on network-capable targets (`platform::hasNetwork`); a no-network build offers just Local audio and Simulate, without a mode picker if those are the only two.

<img src="../../assets/core/AudioService.png" width="300" alt="Audio module controls">

- `mode` — Local audio / Receive network / Simulate: analyze the on-board mic/line-in, consume a peer's audio off the network (WLED-compatible), or feed a synthesized signal. Receive network appears only on a network build; the controls below are its detail, shown per mode.
- `sckPin` / `wsPin` / `sdPin` — (Local) the I²S GPIOs (bit clock / word-select / data; unset until entered).
- `mclkPin` — (Local) master-clock GPIO for a line-in ADC that needs one (e.g. the PCM1808); leave unset for a plain mic.
- `sampleRate` — (Local) mic/ADC sample rate.
- `floor` / `gain` — (Local) noise floor and input gain for the analysis.
- `send audio` — (Local, network build) broadcast the local analysis as WLED audio-sync packets for the WLED ecosystem.
- `simulate` — (Simulate) the synthetic pattern: `music` (a plausible song) or `sweep` (a deterministic band-marching test pattern).
- `syncPort` — (network build) the UDP port (default 11988, the WLED standard), shown when sending or receiving; set it the same on both ends. `sync status` reports the live send/receive state.
- read-only — `level` (RMS), `peakHz` (the audio driving effects, from any source).

Detail: [technical](moxygen/AudioService.md)

[Tests](../../tests/unit-tests.md#audioservice)

<a id="ir"></a>

### IR

A Service (added per board): an IR remote receiver that drives other modules' controls through the shared `Scheduler::setControl` primitive. It **learns** any remote (NEC-over-RMT): pick an action in `learn`, press a button to bind its code. What each action does + the status-line messages: ⌄ details.

<img src="../../assets/core/IrService.png" width="300" alt="IR module controls">

- `pin` — the IR receiver GPIO (unset until entered; on the SE16 it shares GPIO 5 with the Ethernet MISO via the board switch, on the LightCrafter it is its own GPIO 4 alongside Ethernet).
- `learn` — pick an action to bind (`on/off` / brightness up / brightness down / palette next / palette prev); the next received code binds to it, then learning disarms. The first option, `off`, is the disarmed state (bind nothing), not a light action.
- `code on/off` / `code brightness up` / `code brightness down` / `code palette next` / `code palette prev` — read-only, the learned code for each action (persisted).

Detail: [technical](moxygen/IrService.md)

## IR — details

The learned actions drive the `Drivers` module: `on/off` toggles `Drivers.on` (master power), brightness up/down nudge `Drivers.brightness` (±16, clamped 0–255), and palette next/prev step `Drivers.palette`.

The status line reports setup state ("set pin to receive" / "ready"), the learn prompt, a binding ("learned … = 0x…"), a fired action ("Drivers.brightness → N", "Drivers.on → off"), and an unbound code ("received 0x… (unassigned)").
