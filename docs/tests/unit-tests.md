# Unit Tests

Auto-generated from `test/unit/{core,light}/unit_*.cpp` by `scripts/docs/generate_test_docs.py`. **Do not edit by hand** — update the source file's `@module` / `@also` and per-TEST_CASE `//` descriptions instead, then regenerate.

Unit tests are the fastest tier in the [test strategy](../testing.md): they run the production code in-process with doctest, no platform, no network. Each section below covers one module.

## AudioModule

`test/unit/light/unit_AudioBands.cpp`
*Also touches: AudioSpectrumEffect.*

- _AudioBands: silence yields all-zero bands and no peak_
- _AudioBands: a low tone lands in a low band, a high tone in a high band_
- _AudioBands: the reported peak frequency tracks the played tone_
- _AudioBands: a single tone concentrates energy, not smears it everywhere_
- _AudioBands: noiseFloor gates a low idle spectrum to zero, gain scales it back_
- _AudioBands: zero / degenerate input never crashes_

`test/unit/light/unit_AudioLevel.cpp`
*Also touches: AudioVolumeEffect.*

- _DcBlocker: a constant DC offset is filtered out_
- _DcBlocker: an audio tone passes through (DC removed, AC kept)_
- _DcBlocker: reset clears state, null-safe_
- _AudioLevel: silence reads zero_
- _AudioLevel: pure DC reads zero (DC offset stripped)_
- _AudioLevel: a loud sine reads a higher level than a quiet one_
- _AudioLevel: DC bias does not change the level of a sine_
- _AudioLevel: a high noiseFloor (dB floor) gates a modest signal to zero_
- _AudioLevel: higher gain (narrower dB window) reads a higher level_
- _AudioLevel: empty / null input is silence, never a crash_
- _AudioLevel: isqrt64 matches floor(sqrt) on a spread of values_
- Regression: the boot wiring in main.cpp does create("AudioModule")->markWiredByCode() and create() returns nullptr for an UNREGISTERED type — so a missing registerType<AudioModule> made the deref crash and the device boot-looped (found on the S3 bench). These pin that AudioModule and the two audio effects are all registered + createable through the factory, and that latestFrame() is never null even with no mic (so a consumer added before the mic can't deref null).
- _AudioModule::latestFrame is never null (silent frame with no active mic)_

`test/unit/light/unit_AudioModule.cpp`

- _AudioModule: a fresh, unconfigured module is idle (pins default unset)_
- _AudioModule: setup/teardown is repeatable with no residual state_
- _AudioModule: teardown clears the active mic (latestFrame falls back to silence)_
- _AudioModule: last setup() wins, any add/remove order stays coherent_

## BlendMap

`test/unit/light/unit_BlendMap.cpp`
*Also touches: MappingLUT.*

- Identity mapping (logical N → physical N) leaves every byte unchanged.
- One logical light routed to multiple physical positions copies the colour to each (mirror-style mappings work).
- A paged LUT (forced via the maxAllocBlock test cap) must produce a byte-identical dst to a single-alloc LUT with the same mapping. Paging is an allocation detail; blendMap output must not depend on it. This is the end-to-end pin for the no-PSRAM-fragmentation fix.
- An additive (overwrites=false) LUT folding two sources onto one physical light adds and clamps at 255 (no overflow). overwrites=false is the opt-in for the within-layer overlap case; the default copy path would instead overwrite, and a full-opacity Overwrite op still routes through this additive accumulate, so this pins the contract explicitly (the regression after the multi-layer rewrite).
- The default (overwrites=true) path plain-copies: two sources mapped to the same physical means the LAST writer wins, no addition. Pins the fast path.
- Sparse overwrite mapping clears untouched physical cells. A sphere-style layout maps only a subset of the physical box to a source; the rest must end up black, not retain stale data from a previous frame. Pre-fills dst dirty and asserts unmapped cells are zeroed — fails if BlendMap's dst.clear() is removed (the regression target).
- Alpha-over at half opacity: dst = src*α + dst*(255-α). With dst=200, src=100, α=128 → 100*128 + 200*127 = 12800 + 25400 = 38200; /255 ≈ 150.
- Alpha at full opacity collapses to overwrite (src replaces dst exactly).
- Alpha at opacity 0 is a no-op (dst unchanged) — the invisible-layer case.
- Additive with opacity scales the source before adding, then clamps. dst=100, src=200, opacity=128 → add 200*128/255 ≈ 100 → 200.
- clearFirst=false preserves dst cells the source doesn't touch — the mechanic that lets a top layer blend onto the bottom layer's already-composited frame.
- No-LUT alpha-over at half opacity: dst = div255(src*α + dst*(255-α)). dst=200, src=100, α=128 → div255(100*128 + 200*127) = div255(38200) = 149.
- No-LUT alpha at full opacity collapses to a plain copy (overwrite).
- No-LUT alpha at opacity 0 is a no-op (the invisible top layer).
- No-LUT additive with opacity scales the source then clamps at 255. dst=100, src=200, opacity=128 → 100 + div255(200*128)=100 → 200.
- No-LUT additive at full opacity saturates: 200 + 100 = 300 → clamp 255.

## Buffer

`test/unit/core/unit_Buffer.cpp`

- allocate(N,3) reserves count×channels bytes; count/channelsPerLight/bytes/data/span all reflect that.
- clear() zeroes every byte in the allocated range.
- Move-constructing transfers the data pointer and resets the source (no double free, no copy).
- Move-assigning transfers ownership the same way the move constructor does.
- Calling free() twice is harmless; pointer and count remain zeroed.
- allocate() refuses zero-count or zero-channels (returns false, no allocation, buffer left empty so a caller that ignores the bool doesn't get a partial state).

## CheckerboardModifier

`test/unit/light/unit_CheckerboardModifier.cpp`

- A mask leaves the logical box unchanged.
- size=1: every cell is its own square; parity = (x+y+z)&1. Default (invert false) keeps even-parity cells, drops odd-parity. Passing cells keep their coord.
- invert flips which parity passes.
- size>1 groups cells into squares: with size=2, the 2×2 block at the origin is all one square (parity 0), so all four pass; the next block over drops.

## Color

`test/unit/core/unit_Color.cpp`

- Hue 0 is pure red.
- Hue 85 (one third round the wheel) is pure green; a sliver of red is tolerated since 85 is approximate, not exact.
- Hue 170 (two thirds round) is pure blue.
- Zero saturation produces a grey of the given value, regardless of hue.
- Zero value is black, regardless of hue or saturation.
- A hue between the cardinal points blends two channels (here: orange = red + green).
- `hsvToRgb` is `constexpr` — evaluable at compile time.
- `scale8(v, f)` multiplies two 8-bit values and returns 8 bits. Factor 255 is identity, factor 0 zeroes, factor 128 halves (within integer rounding).
- `scale8` is also `constexpr`.

## Control

`test/unit/core/unit_Control_apply_absent_key.cpp`
*Also touches: FilesystemModule.*

- hasKey distinguishes an absent key from one whose value is 0 — the capability the fix relies on. parseInt alone can't (returns 0 for both).
- The core regression: a control bound with a non-zero value, overlaid with a JSON that does NOT contain its key, must keep its value — not snap to 0.
- A present key still applies (the fix must not break the normal load path).
- A present key whose value IS 0 must apply the 0 (don't confuse "present 0" with "absent"). Guards against an over-eager fix that skipped on value rather than key.
- _a per-control validator accepts a valid value and rejects bad input_
- Length boundary of the deviceModel validator (accepts 1..31). Uses a buffer wider than the validator's limit so the 32-char value reaches the validator intact (parseString truncates to bufSize-1, so the buffer must exceed 32 for the validator's own length check — not parse truncation — to be what rejects it). The scratch buffer in applyControlValue is sized to bufSize, so a long value isn't truncated before validation.
- _a Text control with no validator accepts anything that fits_

`test/unit/core/unit_Control_list.cpp`

- _ControlType::List value serializes as an array of row summaries_
- _ControlType::List metadata carries a parallel detail array_
- _ControlType::List with an empty source emits []_
- _ControlType::List type identity + persistable + restore round-trip_

## Correction

`test/unit/light/unit_Correction.cpp`

- At brightness=255, the LUT maps every input value to itself (no scaling).
- At brightness=128, every entry is roughly halved using scale8 (255→128, 128→64, 2→1).
- RGB preset at full brightness passes the source RGB through unchanged (3 output channels, no white).
- GRB preset swaps R and G in the output (G first, then R, then B) — for WS2812-like drivers.
- BGR preset reverses the channel order entirely (B, G, R).
- RGBW preset adds a fourth white channel derived as min(R, G, B) per pixel.
- GRBW preset combines the GRB reorder with the W derivation (G, R, B, W=min).
- Brightness scaling runs before white derivation so W = min of the *scaled* RGB values.
- rebuild() can switch the output channel count between RGB (3) and RGBW (4) on the fly.

## DemoReelEffect

`test/unit/light/unit_DemoReelEffect.cpp`

- The reel enumerates the effect registry, hosts one effect at a time, renders it, and advances through the whole list without crashing — the create/teardown/delete churn every tick is the robustness path this pins. Registering two real effects + the reel gives it something to cycle.

## DevicePlugin

`test/unit/core/unit_DeviceIdentify.cpp`
*Also touches: DevicesModule.*

- _MmPlugin claims a presence packet carrying the projectMM marker_
- _MmPlugin declines a plain WLED packet (no projectMM marker)_
- _WledPlugin claims a plain WLED packet as WLED_
- _WledPlugin declines a projectMM-marked packet (that's a peer, not a WLED)_
- _Plugins decline a short / garbage datagram, never read out of bounds_
- _WledPlugin tolerates an empty name (the module supplies the IP fallback)_

## DevicesModule

`test/unit/core/unit_DevicesModule_ageout.cpp`

- A cached (restored-but-never-re-heard) device is on a short probation, NOT the full 24 h — else a long-gone persisted device would survive forever across reboots (its clock resets to "boot" each restore). It drops once past kCachedGraceMs.
- _DevicesModule: a cached device drops once past the probation window_
- A live-confirmed device (a presence packet cleared its `cached` flag) gets the full 24 h.
- _DevicesModule: a live-confirmed device drops once past kStaleMs (24h)_
- A projectMM peer also answers as a plain WLED (its presence packet without our marker), so a later WLED-classified sighting must NOT relabel a restored projectMM row. This drives the downgrade-prevention in upsertDevice through the public path: restore the row as projectMM, inject a plain WLED packet from the same IP, confirm it stays projectMM.
- _DevicesModule: restore tolerates an empty / malformed cache_

`test/unit/core/unit_DevicesModule_discovery.cpp`
*Also touches: DevicePlugin.*

- _DevicesModule: a plain WLED packet lists a WLED device with its name_
- _DevicesModule: a projectMM-marked packet lists a projectMM device_
- _DevicesModule: a short / garbage datagram is ignored, never listed_
- The P4-bench bug: two DIFFERENT devices (a WLED and a projectMM peer) must each keep their OWN name + type — no cross-contamination between packets.
- A peer RENAME must propagate: a later packet from the same IP with a new name updates the row in place — the live-update requirement (the name rides the presence packet).
- A projectMM device stays projectMM even when a later plain-WLED packet arrives from the same address — the type only RAISES toward projectMM, never downgrades. (A projectMM peer could be seen via an unmarked packet too; that must not relabel it WLED.)

`test/unit/core/unit_DevicesModule_hue.cpp`

- _DevicesModule: a Hue bridge is listed with its colour count_
- _DevicesModule: upsertHueBridge is idempotent, updates count in place_
- _DevicesModule: a persisted Hue bridge restores as a Hue row with its count_
- _DevicesModule: a corrupt persisted colour clamps to the valid range, row still restores_

## DistortionWavesEffect

`test/unit/light/unit_DistortionWavesEffect.cpp`

- _DistortionWavesEffect writes non-zero RGB data_
- _DistortionWavesEffect produces spatial variation_
- _DistortionWavesEffect speed 0 is frozen (stable across ticks)_
- _DistortionWavesEffect survives a 0x0x0 grid_

## Drivers

`test/unit/light/unit_Drivers_container.cpp`

- Disabled child drivers don't tick: toggling `enabled` flips whether that driver's loop() runs.

`test/unit/light/unit_Drivers_firstOutputRgb.cpp`

- _Drivers::firstOutputRgb reads pixel 0 of the driven buffer_
- _Drivers::firstOutputRgb reports black pixel 0 as-is (caller substitutes the default)_
- _Drivers::firstOutputRgb returns false when there is no driven buffer_
- _MoonModule::firstOutputRgb defaults to false (no output module)_

## FilesystemModule

`test/unit/core/unit_FilesystemModule_persistence.cpp`
*Also touches: Scheduler, Layer.*

- Persistence round-trip: set deviceName → save → recreate Scheduler+modules → load → assert. Uses fsSetRoot to isolate the test from any real /.config/ on disk. A control change (deviceName) saved with flush() reappears on the next boot once a fresh Scheduler loads the same path.
- Structural persistence: hand-write a Layer.json describing a different tree shape than the one main.cpp builds, then load and verify the live tree reconciles to match the JSON — type swap at position 0, trim of position 1. On load, a Layer's children are reconciled against the saved JSON: position 0 swaps to the saved type, extras at later positions are trimmed.
- Pins the wiredByCode-preserves-child contract that lets a new firmware revision add a code-created child (e.g. ImprovProvisioning under NetworkModule) without the child getting trimmed on every boot for users whose saved Network.json predates the addition.  Setup: an on-disk file describes Layer with zero children. Live tree has Layer with a RainbowEffect child that main.cpp would have wired and marked. After scheduler.setup() runs the persistence load, the wired child must survive. A code-wired child (markWiredByCode) survives a load from older JSON that doesn't mention it — new firmware additions aren't trimmed for existing users.
- Companion to the wiredByCode case above: when the JSON describes a different type at the position where a code-wired child lives, the position-replacement must NOT kill the code-wired child. Stop reconciliation at that index instead and let the next save re-write the file with the actual tree shape. When the saved JSON wants a different type at the position where a code-wired child lives, reconciliation stops at that index instead of destroying the wired child.
- Round-trip persistence with children: write a Layer subtree that contains both controls and child modules with controls of their own, then read the file back as text and verify it parses as valid JSON. Regresses the missing-comma bug between each child's "N.type" field and that child's first control (e.g. "0.type":"X""0.foo":1 instead of "0.type":"X","0.foo":1). Saving a Layer with multiple children produces valid JSON — comma separators between child `N.type` and the child's first control field are present.
- Singleton survives probe lifecycle: /api/types factory-creates a probe of every registered type (including FilesystemModule) to capture defaults, then deletes it. The probe's destructor must NOT clear the singleton — otherwise every save path (noteDirty, debounced loop1s, flushPending on reboot) silently no-ops for the rest of the device's life. The fix is to register the singleton in setScheduler(), not in the constructor. This test catches that singleton-clear regression. /api/types factory-creates a temporary FilesystemModule probe; its destruction must NOT clear the static singleton (otherwise every later save silently no-ops).
- Regression: Int16 controls (GridLayout's width/height/depth, Layer's start/end) round-tripped through the filesystem load path were clamped to c.min/c.max, which default to 0,0 because ControlDescriptor.min/max are uint8_t and can't represent an int16 range. Every Int16 control loaded as 0 — so a 128×128 grid became 0×0×0 after restart and the whole pipeline allocated no buffers. Int16 controls (GridLayout width/height, RegionModifier start/end) preserve their saved value across load — no zero-clamping from uint8 min/max bounds.

## FireEffect

`test/unit/light/unit_FireEffect.cpp`

- On a 16×16 grid the heat buffer sizes to width × height bytes (one byte of heat per cell).
- With sparking at max, the buffer contains non-zero pixels within 50 frames (sparks emerge and propagate).
- Disabling the effect releases its heat buffer back (dynamicBytes drops to 0).

## FirmwareUpdateModule

`test/unit/core/unit_FirmwareUpdateModule.cpp`

- The `firmware` control is always present and non-empty (either a real firmware key from build_info.h or the fallback "unknown"). The firmware card owns firmware identity (version/build/firmware) + the partition usage.
- OTA phase is surfaced through the shared status slot (MoonModule::setStatus()), not a control. publishStatus() runs in setup()/loop1s() and maps the platform OTA status string to a severity: "idle" clears the banner, an "error: " prefix is Severity::Error, anything else is neutral Severity::Status.

## GameOfLifeEffect

`test/unit/light/unit_GameOfLifeEffect.cpp`

- The B#/S# parser turns a rule string into birth/survive neighbour sets. Conway = B3/S23.
- A 2×2 block is a Conway still life: every live cell has 3 neighbours (survives), and the surrounding dead cells never have exactly 3 (no births). It must be identical after a step.
- Regression: a 3D grid gives a cell up to 26 neighbours (3×3×3 minus self), but the B/S rule tables are sized 9 (single-digit Conway notation, 0..8). A dense 3D neighbourhood must not read those tables out of bounds — a count ≥9 is in no single-digit ruleset, so the cell dies / stays dead. This fills a 3×3×3 cube (the centre has all 26 neighbours alive) and just steps: the test passing under ASan/bounds-checking is the OOB-read pin; behaviourally the over-crowded centre dies (26 ∉ S) and the dense interior doesn't survive.
- A horizontal 3-cell blinker oscillates to vertical after one step (period-2 oscillator). This is the canonical "the rules actually run" check: birth on 3, death of the ends (1 neighbour each).
- A lone cell (0 neighbours) dies — the dead-by-isolation rule, and a sanity check that an empty grid stays empty (no spontaneous births at count 0 under Conway).

## GridLayout

`test/unit/light/unit_GridLayout.cpp`
*Also touches: Layouts.*

- A 4×4×1 grid yields 16 lights iterated row-major: x sweeps fastest, then y, then z.
- Serpentine reverses x on odd rows (boustrophedon), so the strip snakes back and forth: driver index advances linearly while the emitted x zigzags. Even rows L→R, odd rows R→L. The COORDINATE is always the true (x,y) — only the index→position order changes, which is what makes the mapping non-identity.
- A 3D 2×2×2 grid yields 8 lights with z-plane separation (indices 0-3 at z=0, 4-7 at z=1).
- A single-light grid (1×1×1) is a valid layout: one coordinate at (0,0,0).
- Layouts with a single child delegates totalLightCount and forEachCoord to that child directly.
- Two child layouts produce contiguous physical indices: the second layout's coords are offset by the first's lightCount.

## HttpServerModule

`test/unit/core/unit_HttpServerModule_apply.cpp`

- _apply-core: applyAddModule adds a child, idempotent on the id_
- _apply-core: applySetControl writes a value, rejects out-of-range / unknown_
- _apply-core: applyClearChildren empties a container (replaceChildren)_
- _apply-core: applyOp dispatches each op type and tolerates bad input_
- A per-control validator (like SystemModule.deviceModel's printable-ASCII rule) is enforced THROUGH the apply-core — so the APPLY_OP `set` the installer pushes over serial is guarded exactly like an HTTP write, with no per-transport special-casing. This is the point of moving validation onto the control: one backend check, every path.

## HueDriver

`test/unit/light/unit_HueDriver.cpp`

- _HueDriver: a coloured pixel becomes an on/bri/hue/sat state body_
- _HueDriver: a black pixel becomes on:false_
- _HueDriver: RGB→HSV maps the primaries to the right Hue wheel positions_
- _HueDriver: unchanged colour is not resent, a changed one is_
- _HueDriver: parseLights keeps only colour-capable, reachable lights_
- Room + light selection filters which colour lights the driver actually drives. Both dropdowns default to "All" (index 0): then every colour light is driven (unchanged behaviour). Selecting a room narrows the driven set to that room's colour lights; selecting a light drives just that one.
- The single status line (folding what were the separate hueStatus / colourLights controls) shows the light count as driven-of-total: "N-M lights" while filtered, the plain "M lights" when not.
- fetchLights sizes its read buffer by growing while the body looks truncated. The signal is "does the body end in '}'": a too-small buffer cuts the JSON mid-content. (Regression: an earlier check tested strlen==cap-1, which never fires because httpRequest strips headers first, so a >2 KB bridge response was parsed truncated and lights silently disappeared.)

## ImprovFrame

`test/unit/core/unit_ImprovFrame.cpp`

- improvChecksum returns the sum of all input bytes modulo 256 (zero-length input is 0).
- buildImprovFrame writes the full wire shape: "IMPROV" magic + version + type + length + payload + 1-byte checksum.
- A payload larger than kImprovMaxPayload (128) is refused: builder returns 0 bytes written.
- If the caller's output buffer can't hold the framed bytes, the builder refuses (returns 0).
- A zero-length payload is valid: length byte is 0, checksum covers magic+version+type+length only.
- Feeding a well-formed frame byte by byte ends in FrameReady; the parser exposes the type, length, and payload.
- A zero-length payload frame parses to FrameReady with lastPayloadLen() == 0.
- A corrupted checksum byte yields BadChecksum at the end of the frame.
- A length byte greater than kImprovMaxPayload trips OversizePayload at that byte (before any payload data arrives).
- Garbage bytes before the magic 'I' are silently skipped; a fresh well-formed frame after them parses normally.
- "I" followed by another "I" treats the second byte as a fresh magic-start (not discarded) — the parser doesn't lose a real frame that begins mid-aborted-magic.
- When the byte after MagicV isn't the version but happens to be 'I', the parser re-enters magic search at Magic1 — recovers a new frame that arrives right after a corrupted header.
- Every defined ImprovFrameType (CurrentState, ErrorState, Rpc, RpcResponse) round-trips through builder + parser cleanly.
- After FrameReady the parser returns to Magic0 and parses the next frame on the same instance without reset.

## ImprovOpReassembler

`test/unit/core/unit_ImprovOpReassembler.cpp`

- _a single-frame op (seq 0, last 1) is Ready with the exact bytes_
- _a multi-chunk op reassembles in order and NUL-terminates_
- _a duplicate chunk is rejected and resets the buffer_
- _an out-of-order chunk (skipped seq) is rejected_
- _a non-zero opening seq (no fresh start) is rejected_
- _overflow past the buffer (minus the NUL) is rejected, not truncated_
- _exactly buffer-minus-one bytes fits (boundary)_
- _seq 0 mid-stream abandons a partial op and starts fresh_
- _an empty final chunk still completes (last with zero bytes)_
- _reset() drops a partial op_

## JsonUtil

`test/unit/core/unit_JsonUtil_parse.cpp`

- _parse a flat object reads each typed field_
- _parse an array of objects (the persisted device list use case)_
- _parse a nested object_
- _escaped quotes and backslashes round-trip inside a string value_
- _negative and fractional numbers_
- _malformed inputs fail cleanly without crashing_
- _overflow safety: too many nodes fails cleanly_
- _overflow safety: nesting deeper than kMaxDepth fails cleanly_
- _input longer than the text buffer fails cleanly_
- parseString must DECODE the JSON string escapes our own writer emits (JsonSink/writeJsonString) — \" \\ \n \r \t \b \f — so reader and writer are symmetric. A multi-line value (a script with `\n`) must arrive as a real newline, not a literal backslash-n.

## Layer

`test/unit/light/unit_Layer_extrude.cpp`
*Also touches: RainbowEffect, NoiseEffect, PlasmaEffect, SpiralEffect, FireEffect, ParticlesEffect.*

- A D2 effect (Rainbow) on a 3D layer writes z=0 once; Layer::extrude copies that slice across every z>0 — slices are byte-identical.
- A D1 effect writes the x=0 column; extrude duplicates it across every x and every z-slice.
- NoiseEffect declared D3 still produces a valid image on a depth=1 layer (it honours the runtime depth instead of hardcoding z).
- PlasmaEffect (D3) on a 2D layer same contract: valid 2D image, no buffer overrun.
- NoiseEffect (D3) on a 1D layer (height=depth=1) writes a valid strip and never overflows.
- PlasmaEffect (D3) on a 1D layer same contract: valid 1D strip, no overflow.
- SpiralEffect (D2) on a 3D layer: extrude copies z=0 to every z>0 (stateless D2 contract).
- FireEffect (D2, stateful — heat buffer sized to w×h) extrudes cleanly across z on a 3D layer.
- ParticlesEffect (D2, stateful — trail sized to w×h×cpl) extrudes cleanly across z on a 3D layer.

`test/unit/light/unit_Layer_live_modifier.cpp`
*Also touches: RotateModifier, ModifierBase.*

- With a Rotate present, the live pass rotates the gradient each frame as the angle advances — so two frames at different times differ. A static GradientEffect alone would produce identical frames, so any difference is the live remap.
- PAY-FOR-WHAT-YOU-USE: a Layer with no live modifier must NOT run the live pass — the static gradient is byte-identical across frames regardless of the clock.
- A DISABLED Rotate must not run the live pass either (the gate keys off ENABLED live modifiers). Same static gradient → identical frames.
- COALESCED REBUILD: two beat-driven modifiers (RandomMap) on one Layer both ask for a rebuild on a beat; Layer::loop() must rebuild ONCE (not re-enter onBuildState per modifier) and the Layer must stay valid — the composed mapping changes, no crash.

`test/unit/light/unit_Layer_modifier_chain.cpp`
*Also touches: ModifierBase.*

- Region (left half) THEN Multiply (2× mirror): the logical box folds twice. On a 16-wide axis: Region 0..50 → 8, then Multiply 2 → 4. Both modifiers apply — the second is no longer dead weight.
- Order matters: Region-then-Multiply differs from Multiply-then-Region. Region's percentage applies to whatever box it sees, so the composed logical size differs.
- A DISABLED middle modifier is skipped — the chain folds only the enabled ones.

`test/unit/light/unit_Layer_phase_animation.cpp`
*Also touches: MetaballsEffect, SpiralEffect, LavaLampEffect, SpiralEffect.*

- Metaballs visibly changes over 100ms even when per-tick dt is sub-millisecond (no phase-accumulator truncation).
- SpiralEffect advances at desktop speed (the spiral rotates across 100ms).
- LavaLamp animates across 100ms (blobs move).
- Spiral animates across 100ms (rotation visible).
- Replace path: swap one effect for another mid-flight (same shape as HttpServerModule::handleReplaceModule) and confirm the new effect animates. Replacing one effect with another mid-tick (HttpServerModule's swap path) leaves the new effect animating, not frozen.

`test/unit/light/unit_Layer_sparse_mapping.cpp`

- Dense grid: every box cell is a light, so no LUT — the identity/memcpy fast path is preserved exactly (the grid short-circuit).
- Serpentine grid: dense (every box cell is a light, so the count check alone would pick the identity fast path) but SHUFFLED (driver index i != box cell i). isNaturalOrder() measures that from the coords and routes it through the box->driver LUT instead. This is the lever for exercising the non-identity mapping path without a sparse layout or a modifier.
- Sparse sphere: a LUT is built; its destinations are driver indices in [0, lightCount), and the render buffer stays the dense bounding box.
- Sphere + Mirror: the modifier's box-coordinate destinations are translated into driver-index space; no destination escapes [0, lightCount).
- REGRESSION: a high fan-out Multiply (8×8×4 = 256) on a 128×128 grid must build a NON-EMPTY LUT that covers every physical light. The maxDest estimate (logicalCount × maxMultiplier) is computed in 64-bit; before that fix it overflowed uint16 on no-PSRAM boards (256 × 256 = 65536 wraps to 0), sized the LUT to ~nothing, and blanked the display. Here we assert the LUT actually maps the full light set, in range — the symptom that black-screened the device.
- Region carving: a RegionModifier shrinks the Layer's LOGICAL box to the region (so the effect renders only there), and the LUT maps each region cell to its box cell at the start offset — every destination in range, none outside the region. The driver buffer still holds all physical lights; cells outside the region simply get no logical source (dark). Default 0/100 = full box (the no-carve fast path) is covered by unit_RegionModifier; here we carve a quarter.

`test/unit/light/unit_Layer_zero_grid.cpp`
*Also touches: RainbowEffect, NoiseEffect, PlasmaEffect, SpiralEffect, MetaballsEffect, RingsEffect, RipplesEffect, LavaLampEffect, FireEffect, ParticlesEffect, GameOfLifeEffect, GEQ3DEffect, PaintBrushEffect.*

- Rainbow on 0,0,0 grid: no crash.
- Noise on 0,0,0 grid: no crash.
- Plasma on 0,0,0 grid: no crash.
- Spiral on 0,0,0 grid: no crash.
- Metaballs on 0,0,0 grid: no crash.
- Rings on 0,0,0 grid: no crash.
- Ripples on 0,0,0 grid: no crash.
- LavaLamp on 0,0,0 grid: no crash.
- Fire on 0,0,0 grid: no heat buffer allocated, no crash.
- Particles on 0,0,0 grid: no trail buffer allocated, no crash.
- GameOfLife on 0,0,0 grid: no heap alloc for 0 cells, no crash.
- GEQ3D / PaintBrush on 0,0,0 grid: audio effects, no crash with no buffer.
- _PaintBrushEffect on 0,0,0 grid_

## Layers

`test/unit/light/unit_Layers_container.cpp`
*Also touches: Layer.*

- A Layers container with one child Layer must produce the same output as that Layer used directly (no-op container).
- With two child Layers, each one's loop() runs and writes its own buffer (the container iterates all enabled children).
- Multi-layer composition: Drivers blends ≥2 enabled Layers into its own output buffer and hands THAT to drivers (not a single Layer's buffer). Bottom layer overwrites; top layer blends per its blendMode/opacity. This is the end-to-end pin for the composite loop in Drivers::loop.
- Disabling the top layer drops cleanly to the single (bottom) layer — no crash, the driver now sees the bottom layer's content. Pins the robustness path.
- Drivers' composition/output-buffer allocation contract (architecture.md § Adaptive allocation). The driver output buffer exists ONLY when the pipeline must blend into physical space; otherwise the lone layer's buffer is handed to drivers directly (zero-copy). dynamicBytes() reflects outputBuffer_.bytes(), so it's 0 ⇔ no buffer. Pins all three cases in one place: 1. one identity (no-LUT) layer  → NO output buffer (zero-copy) 2. two enabled layers           → output buffer (must composite) 3. one layer WITH a LUT         → output buffer (must map logical→physical)
- activeLayer() returns the first enabled child, or the only child if all are disabled (so dimensions stay queryable during boot/toggle-off).
- firstEnabledLayer() is the output-selection counterpart to activeLayer(): it never falls back to a disabled layer, so it returns nullptr exactly when nothing renders.
- If the container holds only non-Layer children, activeLayer() returns nullptr (the role-guard skips, never miscasts).

## Layouts

`test/unit/light/unit_Layouts_container.cpp`

- Disabled layouts contribute nothing; enabled siblings shift down to close the gap (no index holes).
- Disabling the Layouts container itself zeroes totalLightCount and yields no coordinates.

`test/unit/light/unit_Layouts_mutation.cpp`

- Add a single layout: the container reports its light count and iterates it.
- Add more than one layout (mixed types): counts sum, indices stitch end-to-end.
- Replace a layout with a different type at the same slot: the other layouts and their order are preserved; only the replaced slot's contribution changes.
- Remove a layout: it leaves the tree, the remaining layouts shift to close the gap, and the total drops by exactly the removed layout's light count.

`test/unit/light/unit_Layouts_toggle_cycle.cpp`
*Also touches: Layer, Drivers.*

- Disabling the only layout child and re-enabling it must not crash Drivers, and rendering resumes cleanly.

## LcdLedDriver

`test/unit/light/unit_LcdLedDriver.cpp`
*Also touches: Drivers, Correction.*

- Explicit counts slice the buffer consecutively; the frame is sized by the LONGEST lane. The bus always has all 8 lanes — unused strands take the 0-light remainder and idle LOW.
- Empty ledsPerPin splits evenly — same PinList semantics the RMT driver uses.
- An RGB→RGBW preset toggle grows the frame (32 vs 24 slot bytes per light).
- A bad pin list idles the driver with the parse literal in the status; fixing it recovers.
- Pins now default UNSET (the "default only when it cannot do harm" rule — the strand is user-soldered). A fresh, unconfigured driver idles, never grabbing the 8 data GPIOs on its own. (wire() back-fills empty pins for the slicing cases, so this one wires the buffer directly to keep pins empty.)
- IDF's i80 bus rejects partial pin sets, so the driver does too — fewer than 8 pins is a config error, not a narrower bus.
- A 0×0×0 grid is a clean idle: zero counts, zero frame (no pad for an empty frame), no crash.
- setup/teardown cycles leave no residue (status clean, ASAN-checked heap).
- loopbackRxPin is bound always, visible only while loopbackTest is on.
- loopbackTxPin (optional lane-0 TX override) is bound always, hidden until the test is on — same conditional-control contract as loopbackRxPin. The override's lane-0 substitution is hardware-only (lcdLanes==0 on desktop); the visibility contract is host-testable here via the shared helper (toggles loopbackTest both ways and asserts the control stays bound while flipping visibility).

`test/unit/light/unit_LcdLedEncoder.cpp`
*Also touches: Correction.*

- One lane, one byte 0xA5: slot0 always the mask, slot1 follows the bits MSB-first, slot2 always zero.
- Two lanes 0xFF/0x00 in one row: the data slot carries lane 0's bit only — the transpose itself.
- A lane excluded from the mask contributes to NEITHER slot 0 nor slot 1, even with garbage wire bytes — short strands idle LOW (no white flashes).
- Mask 0 (a row past every lane's strand) is a fully idle row.
- Channel order comes from Correction (logical red → GRB wire {0,255,0}); the encoder is order-agnostic.
- RGBW rows emit 4 channels × 8 bits × 3 slots = 96 bytes.

## MappingLUT

`test/unit/core/unit_MappingLUT.cpp`

- A fresh LUT carries no mapping (hasLUT==false, logicalCount==0); BlendMap takes the fast identity copy path.
- setIdentity(N) declares a 1:1 mapping for N lights without allocating a LUT; forEachDestination still iterates correctly.
- Each logical light can map to a different count of physical lights; forEachDestination yields every mapped index in order.
- When no single contiguous block fits (forced via the test cap) but total heap allows it, build() pages the destinations array. The mapping must read back identically to a single-alloc build — paging is an allocation detail, not a behaviour change. isPaged() confirms the fallback actually engaged.
- build() returns false on genuine exhaustion — total free heap (minus the reserve) can't hold the destinations — so the caller degrades to 1:1. Forced here via a non-zero freeHeap is desktop-only-unavailable, so this case pins the paged path's success and the boundary; the tier-3 false path is covered by the Layer sparse-mapping degrade test on real heap limits.
- free() releases memory and resets counts; build() can be called again to install a fresh mapping.

## MetaballsEffect

`test/unit/light/unit_MetaballsEffect.cpp`

- One tick on a 16×16 grid leaves at least one non-zero byte in the layer buffer (proves the effect rendered).
- Pixels at opposite corners of a 32×32 grid differ in colour (the effect is not flat-filling the buffer).

## ModuleFactory

`test/unit/core/unit_ModuleFactory.cpp`

- registerType<T>(name) instantiates a probe of T to read its role(), then stores name+role+constructor for later create() calls.
- create(name) returns a heap-allocated instance whose role and typeName match what was registered.
- create() returns nullptr for an unknown name or a nullptr name (no crash on bogus input).
- typeName/typeRole with an out-of-range index returns nullptr / Generic safely (never UB).
- The factory grows its registry capacity dynamically — registering 10+ extra types past the initial size still works and every name stays discoverable.

## MoonLive

`test/unit/core/unit_moonlive_compiler.cpp`

- _compileSource: fill(r,g,b) fills every light_
- _compileSource: setRGB(index, r,g,b) writes one pixel_
- REMARK #1: every argument is an expression — random16 in ANY slot.
- REMARK #2: a literal / random16 bound may be a uint16 (0..65535), not capped at 255.
- _compileSource: out-of-range index is bounds-rejected at runtime_
- _compileSource rejects malformed programs with a diagnostic, never crashes_
- _MoonLive.compile(source) on a bad script leaves the engine !ok with an error_
- VREG REUSE: a chain of calls must fit the small device register file. Each argument temp dies once its call consumes it and is recycled, so peak register pressure stays low no matter how many calls a statement nests — setRGB with all four arguments a random16 still compiles.
- DOMAIN-NEUTRAL: the core compiler owns no function names. With an EMPTY table it knows nothing — `setRGB`/`fill`/`random16` are all "unknown function". The LED vocabulary lives only in the host's table; a different host registers different names. (Remark #3.)
- _MoonLive recompiling swaps the program live (fill <-> setRGB)_
- STAGE 1 CONTROLS — parse layer: a `uint8_t name = def; // @control min..max` declaration surfaces a DeclaredControl, and a declared name used in a statement resolves to it.
- _compileSource: malformed control declarations fail with a diagnostic, never crash_

`test/unit/core/unit_moonlive_fill.cpp`

- _MoonLive emitFill produces a non-empty routine_
- _MoonLive emitFill rejects a too-small buffer (degrades, no overrun)_
- _MoonLive emitFill/emitAnimatedFill reject a null output buffer (no crash)_
- _MoonLive compiles and fills a buffer with the chosen colour_
- _MoonLive run on zero lights writes nothing (robust to empty)_
- The native routines write channels +0/+1/+2 per light, so a layer with fewer than 3 channels per light can't hold RGB — run() must leave it untouched, not overrun it.
- _MoonLive recompile swaps the colour; free returns to !ok_
- _MoonLive animated fill derives colour from the per-frame t_
- _platform allocExec returns usable executable memory, freeExec releases it_
- _MoonLive controls: declaredControls + controlSlot seeded from the default_
- _MoonLive controls: arena address is STABLE across a recompile and the slot value survives_
- _MoonLive controls: free() releases the arena (no stale slot after teardown)_

`test/unit/core/unit_moonlive_ir.cpp`

- _MoonLive compiled fill is BEHAVIORALLY identical to the hand-encoded emitFill (golden)_
- _MoonLive compiled fill is robust: zero lights writes nothing_
- _MoonLive compileSource degrades on a too-small code buffer_
- _MoonLive compiled setRGB writes one pixel; out-of-range is bounds-rejected_
- _MoonLive control: a declared control reads the arena live (no recompile on value change)_
- _MoonLive control survives a host call (kArg4 live across random16)_

## MoonModule

`test/unit/core/unit_MoonModule.cpp`

- setup() and teardown() each fire exactly when called and update their respective state flags.
- name() starts empty; setName() copies the string into the internal 16-byte buffer.
- typeName (set by the factory) is independent of name; setName doesn't touch typeName so a human-renamed module still serializes under its real type.
- dirty()/markDirty()/clearDirty() round-trip cleanly (the bit FilesystemModule polls for save scheduling).
- parent() starts null; setParent() records the upstream container for tree walks.
- Adding Uint8/Bool controls stores live pointers to the module's fields, so changes propagate either direction (field ↔ control->ptr).
- controls().clear() empties the list; calling onBuildControls() again repopulates it (the standard rebuild path).
- addReadOnly binds a char buffer the UI can render; updating the buffer is visible through control.ptr.
- addSelect binds a uint8 + an options array (stored in aux) — control.max carries the option count.
- addProgress binds a uint32 plus a "total" value (in aux) — the UI renders value/total as a progress bar.
- enabled defaults to true; setEnabled flips the universal enable gate (Scheduler and parent containers respect it).
- addBool binds a bool field — toggling the field updates control.ptr's view.

`test/unit/core/unit_MoonModule_control_change_gate.cpp`
*Also touches: GridLayout, MultiplyModifier, NoiseEffect, Drivers.*

- Layout and Modifier modules opt in to rebuild on a control change (their controls reshape the pipeline).
- Effects and Drivers opt out — their controls are values read directly in the hot path, no rebuild needed (prevents slider stutter).

`test/unit/core/unit_MoonModule_lifecycle.cpp`

- A parent's default loop() fans out to every enabled child — no per-container boilerplate needed.
- Disabled children are skipped during propagation (the universal enable-gate).
- Modules that override respectsEnabled() to false (NetworkModule, SystemModule, …) tick regardless of their enable bit.
- loop20ms / loop1s use the same gate-and-propagate rule as loop().
- A leaf module (no children) ticks safely as a no-op with no accumulated timing.
- Each child's loopTimeUs() reflects its own accumulated cost (Scheduler reads per-child timing, not the parent's sum).

`test/unit/core/unit_MoonModule_movechild.cpp`

- Moving a child to its current index returns false and changes nothing.
- Moving a child forward shifts intervening children leftward to close the gap.
- Moving a child backward shifts intervening children rightward to make room.
- Single-position moves work in either direction (UI's up/down arrow buttons).
- A target index beyond childCount() is refused (returns false, tree untouched).
- A module that isn't actually a child of the parent is refused.
- Middle-to-middle moves preserve the integrity of every sibling's index.

`test/unit/core/unit_MoonModule_replacechild.cpp`

- Replacing position 1 swaps that child while leaving siblings and child count untouched.
- The returned old child has its parent cleared; the fresh child has its parent set to the container.
- An out-of-range index returns nullptr and the tree (plus the rejected replacement's parent pointer) stays untouched.
- A nullptr replacement returns nullptr and leaves the tree intact.
- After replace, the caller follows the lifecycle order: onBuildControls → setup → onBuildState on the fresh module, then teardown on the old.

## MultiplyModifier

`test/unit/light/unit_MultiplyModifier.cpp`

- _MultiplyModifier advertises D3 dimensions_
- Defaults (multiply 2/2/1) → a 128×128 physical grid folds to a 64×64 logical box.
- _MultiplyModifier logical size on Z_
- FAN-OUT (fold direction): with the defaults (mult 2, mirror XY), all four physical CORNERS fold onto the single logical pixel (0,0) — the inverse of the old "logical (0,0) → 4 physical corners". This is the kaleidoscope fold made concrete.
- mirrorX only: two physical columns fold to the same logical column (original + its horizontal reflection). The logical box is 64 wide.
- All multipliers 1 → identity: the box is unchanged and every coord folds to itself.
- Tiling WITHOUT mirror repeats (does not reflect): physical x=64 (tile 1) folds to logical x=0, same as physical x=0 — both tiles map identically, no reflection.
- multiplyZ on a 2D (depth-1) layout is a no-op: the effective multiplier clamps to the axis extent (1), so depth stays 1 and the layer isn't blanked.
- A multiplier larger than the axis extent clamps to the extent.
- REGRESSION (🐇): a non-divisible extent leaves a leftover edge strip that the tiles don't cover — those pixels must be DROPPED, not wrapped back into a tile (which would duplicate the edge). 5-wide, multiply 2 → tile width 2, covers pixels 0..3; pixel 4 is the leftover and has no tile.

## NetworkModule

`test/unit/core/unit_NetworkModule.cpp`

- setWifiCredentials copies SSID + password into internal buffers and raises the dirty flag so the next loop1s() applies them.
- A nullptr SSID is silently ignored (no copy, no dirty flag) — guards against a bogus caller.
- A nullptr password is treated as empty (open networks), still copies SSID and marks dirty.
- An over-length SSID (100 chars) is truncated cleanly into the 33-byte buffer; ASAN catches any overflow.
- After setup(), NetworkModule exposes a `mode` read-only control whose value reflects the current state-machine state. On the desktop platform every network init stub returns false, so the cascade lands on Idle.
- parseDottedQuad (in Control.h) is the validator on every IPv4 write, over both the HTTP API and persistence. Pin the contract.
- The static-IP fields (ip / gateway / subnet / dns) are bound as IPv4 controls — 4 bytes of storage each, not 16-char dotted-quad strings. They start hidden because addressing defaults to DHCP.
- In WiFi-capable builds (anything other than --firmware esp32-eth), the rssi and txPower controls are present and start hidden — Idle/Ethernet don't expose live WiFi metrics. The Ethernet-only build compiles them out entirely so the iteration finds nothing, which is still a valid pass shape.
- Conditional controls: the static-IP fields (ip/gateway/subnet/dns) are visible only when addressing == Static (1), hidden under DHCP (0) — but ALWAYS bound so persistence can load a saved static config regardless of the live mode. This is the documented add-then-setHidden pattern (architecture.md § Conditional controls); the test pins it both ways so a regression (e.g. dropping setHidden, or conditionally NOT adding the field) fails here, not on hardware.

`test/unit/core/unit_NetworkModule_ethernet.cpp`

- The enum values are a wire contract: the Select index, the ethInit() switch, and every deviceModels.json `ethType` all agree on these. Pin them so a reorder fails here.
- Desktop has no Ethernet: the default PHY type is ethNone, so a board that never pushes an eth config still reports "no Ethernet" and the cascade falls through.
- The platform seam must accept any runtime config and never bring Ethernet up on desktop — ethInit() returns false so NetworkModule cascades to WiFi/AP. Pushing a fully-populated W5500 config and an RMII config both leave ethInit() false and ethConnected() false; ethStop() is safe to call when nothing is running.

## NetworkReceiveEffect

`test/unit/light/unit_NetworkReceiveEffect.cpp`
*Also touches: NetworkSendDriver.*

- A packet built by the sender's builder parses back to the same universe and payload — the two sides can't drift.
- Bad magic, non-OpDmx opcodes, truncated headers, and lying length fields are all rejected — the receiver drops them.
- Universe universe_start lands at byte 0; the next universe lands at byte 510 — the same split the sender uses.
- The layer clears its buffer every tick; staging holds the last frame, so the lights don't strobe black between packets.
- Universes below universe_start are ignored; universes relative to a non-zero start land at offset 0.
- A payload overrunning the buffer end is clamped; a universe entirely beyond the buffer is ignored.
- A 0×0×0 grid accepts packets as a clean no-op — degraded, not crashed.
- Staging is sized in onBuildState (off the hot path), loop() never reallocates it, teardown frees it.
- A real packet sent over localhost UDP lands in the layer buffer — the end-to-end proof of the platform receive path.

`test/unit/light/unit_NetworkReceiveEffect_protocols.cpp`
*Also touches: NetworkSendDriver.*

- A packet built by the sender's builder parses back to the same universe and payload — the two sides can't drift.
- Truncated headers, a bad ACN identifier, wrong layer vectors, a non-zero start code, and a lying property count are all rejected.
- A packet built by the sender's builder parses back to the same byte offset and payload.
- Truncated headers, wrong version bits, and a lying length field are rejected.
- Each universe-protocol parser refuses the other protocols' datagrams — port mix-ups degrade to silence, not garbage.
- An ArtPoll datagram is recognised (the discovery hook Resolume/Madrix use); OpDmx and non-ArtNet packets are not polls.
- The ArtPollReply carries the fields controllers read: opcode, IP, port, names, universe switches, MAC.
- DDP's byte addressing lands payloads at the exact offset; out-of-range and overflowing offsets are clamped or dropped.
- channels_per_universe = 512 maps universes at 512-byte strides and clamps a 512-channel payload to its slot.
- Three senders — one per protocol — hit the same effect on its three ports; each payload lands. The autodetect proof.

## NetworkSendDriver

`test/unit/light/unit_NetworkSendDriver_no_alloc_in_loop.cpp`
*Also touches: Drivers, Correction.*

- onBuildState sizes the correction-applied buffer to source-count × out-channels. The size matches what loop() needs on its first send. Calling loop() after onBuildState must not reallocate — pin the data pointer + shape.
- A preset toggle from RGB to RGBW grows outChannels from 3 to 4. The grow runs in onCorrectionChanged, off the hot path.
- A brightness-only change keeps outChannels at 3 — onCorrectionChanged is still called, but the resize short-circuits (existing buffer already fits).

`test/unit/light/unit_NetworkSendDriver_packet.cpp`

- The built packet contains the exact header layout the Art-Net spec mandates: ID, OpCode, version, sequence, physical, universe, length, data.
- Universe 259 (0x0103) is encoded little-endian (low byte first), matching the Art-Net wire format.
- 256 RGB lights (768 bytes) split across exactly 2 universes (510 + 258), matching the 510-channel-per-universe cap.
- The data-length field is encoded big-endian (high byte first), unlike the universe field — matching the Art-Net spec.
- The built E1.31 packet carries the exact ACN layout strict sACN receivers (and tools like xLights) validate: identifier, the three flags+length fields, CID, source name, priority, universe, property count, start code.
- The built DDP packet carries version+push bits, RGB data type, default destination, and big-endian offset/length.

## NoiseEffect

`test/unit/light/unit_NoiseEffect.cpp`
*Also touches: PlasmaEffect, RainbowEffect.*

- One tick on an 8×8 grid leaves at least one non-zero byte (noise paints somewhere).
- Opposite corners of a 16×16 grid carry different colours (noise is not flat).
- Noise and Rainbow produce visibly different frames on the same grid (sanity check that they're distinct algorithms).
- With depth > 1, adjacent and distant z-slices each render differently (3D noise, not a stack of identical 2D slices).
- Same z-slice variation requirement holds for Plasma — each depth plane renders differently.

## Palette

`test/unit/light/unit_Palette.cpp`

- _Palette: gradient endpoints land on the first/last stop colours_
- _Palette: a mid-gradient sample interpolates between stops_
- _Palette: colorFromPalette index 0 reads entry 0; brightness scales_
- _Palette: the index wraps at 255→0 (no out-of-range read)_
- _Palette: a degenerate (empty) gradient is all black, never out-of-bounds_
- _Palettes::active swaps the global palette on setActive_

## ParlioLedDriver

`test/unit/light/unit_ParlioLedDriver.cpp`
*Also touches: Drivers, Correction.*

- Three lanes (Parlio accepts any 1..8 count) slice the buffer consecutively; the frame is sized by the LONGEST lane.
- Empty ledsPerPin (the default) splits evenly over the 8 lanes — shared PinList semantics, same as the RMT/LCD drivers.
- The Parlio-vs-LCD difference: 1..8 pins are ALL valid (no exactly-8 rule).
- More than 8 pins is rejected (the chip's lane cap), like the other drivers.
- An RGB→RGBW preset toggle grows the frame (32 vs 24 slot bytes per light).
- A bad pin list idles the driver with the parse literal in the status; fixing it recovers.
- Pins now default UNSET (the "default only when it cannot do harm" rule — the strand is user-soldered). A fresh, unconfigured driver idles, never grabbing a GPIO. (wire() back-fills empty pins for the slicing cases, so this one wires the buffer directly to keep pins empty.)
- A 0×0×0 grid is a clean idle: zero counts, zero frame, no crash.
- loop() is crash-safe across single-pin / multi-pin / pre-init configs (the transmit path is gated out on the host; this pins the reachable contract).
- setup/teardown cycles leave no residue (status clean, ASAN-checked heap).
- loopbackRxPin is bound always, visible only while loopbackTest is on.
- loopbackTxPin (optional lane-0 TX override) is bound always, hidden until the test is on — same conditional-control contract as loopbackRxPin. The override's lane-0 substitution is hardware-only (parlioLanes==0 on desktop); the visibility contract is host-testable here via the shared helper (toggles loopbackTest both ways and asserts the control stays bound while flipping visibility).

## ParticlesEffect

`test/unit/light/unit_ParticlesEffect.cpp`

- The trail buffer sizes to width × height × 3 bytes (one RGB per cell, used to fade existing pixels).
- A single tick is enough to paint particles into the buffer.
- Disabling the effect releases the trail buffer (dynamicBytes returns to 0).

## PlasmaEffect

`test/unit/light/unit_PlasmaEffect.cpp`
*Also touches: NoiseEffect.*

- One tick on an 8×8 grid produces at least one non-zero byte.
- Opposite corners of a 16×16 grid differ in colour (the plasma is not flat-filling).
- Plasma and Noise produce visibly different frames on the same grid (sanity check that they're distinct algorithms).

## PreviewDriver

`test/unit/light/unit_PreviewDriver.cpp`

- A sphere sends its SHELL lights (210), not the dense 9x9x9 box (729).
- Per-frame 0x02 RGB count matches the coordinate-table count.
- A small grid sends every light at its grid position (stride 1, exact).
- A large layout is SPATIALLY downsampled (a regular per-axis lattice, not every-Nth-flat- index) so the payload fits the send-buffer cap without the diagonal moiré that linear stride produced on a grid whose width didn't divide the stride. The wire "stride" field carries the per-axis lattice/downscale factor (colour k still maps 1:1 to coord k).
- A SPARSE layout under the cap must NOT be downsampled for its big BOUNDING BOX alone: the lattice bound is the layout's LIGHT count, not its box cell count, so a sphere whose shell fits the cap sends every light at stride 1 (a radius-8 sphere → ~812 shell lights, well under the 4096 display cap, in a 17³≈4913-cell box). (A genuinely huge sparse layout above the cap downsamples like any other — the cap is about points streamed, not box size.)
- Default fps is the rate-limited preview stream rate.
- Regression: a coordinate table dropped under backpressure must be RETRIED, and colour frames withheld until it lands — otherwise the device sends 0x02 frames the browser skips (count mismatch) and the preview freezes for the whole session. Drives loop() (where the coord-pending logic lives) with a broadcaster that drops every 0x03, then lets it through.
- Regression: deleting the active Layer must not leave a driver holding a dangling layer_ pointer. Previously Drivers::passBufferToDrivers early-returned when the active Layer was null, leaving PreviewDriver's layer_ pointing at the freed Layer; the next onBuildState read layer_->layouts() on freed memory and crashed the device (LoadProhibited → boot loop, since the broken tree persists). Now passBufferToDrivers clears the drivers' layer_/sourceBuffer_ to null, a safe idle state. This drives the real path: Drivers bound to a Layers CONTAINER (self-healing), the Layer removed, then buildState re-resolves activeLayer()=null.
- Coordinates are sent ONLY when the geometry changes or a new client connects — never per-frame and never on a timer (a periodic full-table rebuild would starve the tick). A new client (clientGeneration bump) re-sends immediately so a page refresh shows the preview at once. Driven through loop() with a frozen clock for determinism.
- A full-res RGB frame is sent through the RESUMABLE buffered path (sendBufferedFrame), whose body is the DRIVER (consumer) buffer itself — no copy. For a dense identity grid that's the Layer's dense box buffer; for a sparse/mapped layout it's the LUT-mapped output buffer (the real lights), the same buffer the LED drivers consume — NOT the dense box.
- Sparse layout: the buffered send streams the LUT-mapped DRIVER buffer (only the real lights, in driver order), exactly like the LED drivers — NOT the dense bounding box. So coordCount == the shell count and the frame is sent whole at full res through the resumable path.
- Dense-grid CLOSED-FORM downsample, exact colour placement: a 200×1 strip pinned over a small cap strides in x only, so the kept lights are columns 0,s,2s,… The colour pass must read each from its dense buffer index (closed-form x for a 1-row grid) and pack them in the SAME order as the coord table — no forEachCoord. Painting a known colour at a kept column and finding it at the matching frame position pins the index math + the lattice order.
- ADAPTIVE FRAME RATE: while a buffered send is still draining (a slow link), loop() must NOT start a new frame — it waits for bufferedSendIdle(). So the effective rate self-limits to the link.
- USE-AFTER-FREE GUARD: a geometry rebuild (resize) frees+reallocs the producer buffer, so any in-flight buffered send (which holds a pointer into it) MUST be cancelled in onBuildState before the buffer goes away — else drainPreviewSend would read freed memory.

## RainbowEffect

`test/unit/light/unit_RainbowEffect.cpp`

- A single frame on a 4×4 grid leaves the buffer non-zero (rainbow always paints somewhere).
- Pixel (0,0) is at full saturation and value (one channel exactly 255) — confirms hsvToRgb wiring.
- Distant pixels carry different hues (the rainbow gradient is spatial, not uniform).

## RandomMapModifier

`test/unit/light/unit_RandomMapModifier.cpp`
*Also touches: Layer.*

- A remap leaves the logical box unchanged.
- The core property: a true bijection over [0, w*h*d) — every destination index appears exactly once (no gaps, no duplicates).
- Deterministic seed → reproducible permutation (what makes it testable).
- Reshuffling (a beat) changes the mapping, still a bijection.
- Robustness: an empty (0×0×0) box must not crash — it folds to a no-op.
- A resize (different box count) rebuilds the permutation to the new size.
- _RandomMapModifier loop() reshuffles on a beat (bpm 60 ≈ 1/s)_
- _RandomMapModifier loop() with bpm 0 never reshuffles (frozen)_

## RegionModifier

`test/unit/light/unit_RegionModifier.cpp`

- Default region (0/100 on every axis) is the full box: identity size, no rejection.
- Half of an axis, half-open: end=50 on 128 → region width 64, not 65.
- Two abutting regions tile a 128-wide axis with no overlap and no gap.
- A physical coord inside the region folds to region-local (subtract the start pixel); a coord outside is rejected.
- Rounding rule on a small panel: start floors, end ceils to an exclusive pixel. start 33 / end 66 on a 4-wide axis → floor(1.32)=1 .. ceil(2.64)=3 → pixels 1,2.
- A region that rounds to nothing still gets a 1-pixel floor.
- OFF-SCREEN: a window slid half off the left edge keeps its FULL size (the effect renders at a fixed scale); only the visible half maps to physical lights. startX=-50 on 64 → window [−32, 32), span 64. Physical x 0..31 land at window-local 32..63 (the right half of the window — the visible part); the left half of the window (0..31) has no physical light, so it's dark. The effect isn't rescaled.
- A window entirely off the box maps NO lights — the layer goes dark on that axis, which is how an effect is moved completely out of view. The box still has a valid size (the effect renders), nothing just reaches the screen.
- A window stretched WIDER than the box (start<0 and end>100) renders the full span; the box shows the middle slice. startX=-50,endX=150 on 64 → window [−32, 96), span 128.
- Degenerate axes don't crash: a 1-wide axis stays 1, a 0-extent axis yields 0.

## RmtLedDriver

`test/unit/light/unit_RmtLedDriver_lifecycle.cpp`
*Also touches: Drivers, Correction.*

- _RmtLedDriver sizes the symbol buffer in onBuildState_
- _RmtLedDriver keeps the symbol buffer across a rebuild (reinit must not free it)_
- _RmtLedDriver keeps the symbol buffer across a pins change_
- _RmtLedDriver grows the symbol buffer when the grid grows_
- _RmtLedDriver releases the symbol buffer on teardown_
- MoonModule contract: teardown reverses setup, so setup→teardown→setup→teardown cycles leave no residue — no leaked heap (ASAN in the test runner catches that), no stuck state. After each teardown the driver must look untouched: no symbol buffer, no status. Run several cycles to surface any accumulation.
- Conditional control: loopbackRxPin is visible only while loopbackTest is on, hidden otherwise — but always bound (so a saved rxPin loads regardless). Same add-then-setHidden pattern as NetworkModule (architecture.md § Conditional controls). This pins the exact behavior that, with the old UI, showed the pin at the wrong times; a regression in the C++ flag now fails here.
- loopbackTxPin is the optional TX override (transmit on it instead of pins[0] during the self-test). Like loopbackRxPin it's a conditional control: always bound (so a saved override loads), shown only while loopbackTest is on. The override's effect on the transmitted pin is hardware-only (rmtTxChannels==0 on desktop), but the conditional-visibility contract is host-testable here.
- Editing `pins` while the loopback test is ON must refresh the parsed config before the self-test runs — onUpdate fires before the buildState sweep re-parses, so without the in-branch parseConfig() the test would transmit on the OLD pin and show a verdict for it. Mirrors the fix in ParallelLedDriver; this pins the RMT sibling that the dedup left behind. Host-observable via pinCount(): the refresh re-parses to the new pin set even though the platform loopback itself is inert.

`test/unit/light/unit_RmtLedDriver_pins.cpp`
*Also touches: Drivers, Correction.*

- "18,17,16" parses to three pins in list order — the order defines the buffer slices.
- A single pin (the default "18") and spaces around tokens are both fine.
- _parsePinList rejects bad input with a static error message_
- maxPins is the chip's RMT TX-channel cap: 5 pins fail an S3-sized 4, fit a classic 8.
- The same GPIO twice would double-drive one strand — rejected at parse time.
- Explicit "100,100,50" maps one count to each pin by position.
- A short list assigns what it names; unlisted pins share the remaining lights evenly.
- _assignCounts with an empty list splits evenly, last pin takes the rounding remainder_
- _assignCounts clamps so the sum never exceeds the buffer_
- _assignCounts handles a zero-light buffer (0×0×0 grid) as all-zero_
- _assignCounts rejects a bad token_
- _assignCounts ignores extra counts beyond the pin list_
- _RmtLedDriver slices the buffer across pins (even split)_
- _RmtLedDriver slices the buffer per ledsPerPin_
- _RmtLedDriver idles with a status error on a bad pin list_
- _RmtLedDriver with the empty default pins idles cleanly (no pin assumed)_
- _RmtLedDriver re-slices when the source buffer changes_
- _RmtLedDriver window: ledsPerPin distributes over the window, not the whole buffer_
- _RmtLedDriver window: count 0 means the rest of the buffer from start_
- _RmtLedDriver window: a size-1 window at 0 is the onboard-LED case_
- _RmtLedDriver window: a start past the buffer end yields an empty slice_
- loop() is a safe no-op across single-pin, multi-pin and zero-grid configs.

`test/unit/light/unit_RmtLedEncoder.cpp`
*Also touches: Correction.*

- _encoder: one byte, MSB-first, 0 and 1 bits get the right pulse widths_
- _encoder: one light's channels emit channels*8 symbols in byte order_
- _encoder: GRB ordering comes from Correction, encoder is order-agnostic_
- _encoder: RGBW preset yields 32 symbols per light_

## RotateModifier

`test/unit/light/unit_RotateModifier.cpp`

- _RotateModifier advertises a live (per-frame) modifier_
- At the initial angle (0) the rotation matrix is the identity — every cell samples itself.
- z passes through (2D rotation) — a 3D coord's z is untouched.
- An empty box doesn't divide-by-zero or wrap: the remap is a no-op-ish transform that the Layer's live pass then treats as out-of-box (dark), never a crash.

## Scheduler

`test/unit/core/unit_Scheduler_unique_names.cpp`

- A name with no collision is returned unchanged.
- The second module with a duplicate name gets " 2" suffixed; the first keeps its original name.
- Suffix counting increments past existing "-2" / "-3" suffixes ("Layer", "Layer-2", "Layer" → "Layer-3").
- deduplicateNamesInTree() walks the entire module tree in one pass and disambiguates every duplicate (used after persistence load).
- firstByName(name) returns the first match in DFS order, or nullptr if no module carries that name.
- If the disambiguating suffix would overflow the 16-byte name buffer, ensureUniqueName refuses to truncate and keeps the colliding name (sharp edge, documented).

## SineEffect

`test/unit/light/unit_SineEffect.cpp`

- _SineEffect writes non-zero RGB data_
- _SineEffect amplitude 0 yields a black buffer_
- _SineEffect varies across the x axis (R channel follows x)_
- _SineEffect survives a 0x0x0 grid_

## Sort

`test/unit/core/unit_Sort.cpp`

- _insertionSort orders ints ascending_
- _insertionSort with a custom (descending) comparator_
- _insertionSort orders C-strings (the device-name use case)_
- _insertionSort is stable — equal keys keep input order_
- _insertionSort handles empty and single-element arrays_

## SphereLayout

`test/unit/light/unit_SphereLayout.cpp`

- lightCount() must equal the number of points forEachCoord emits — they share one shell predicate, so allocation and fill can never disagree.
- The sphere is HOLLOW: the centre lattice point (r,r,r) is never emitted, and neither is any interior point (distance < radius-0.5 from centre).
- radius = 1 is the smallest hollow sphere: the 6 axis neighbours (d^2=1) plus the 12 edge points (d^2=2) of the centre — 18 lights, no centre.
- The shell is symmetric about the centre: for every emitted point its mirror through the centre is also emitted (a sphere has no preferred direction).
- Physical indices are sequential 0..N-1 over the emitted shell points (no gaps from the unindexed lattice voids), so the buffer maps 1:1 to emitted lights.
- Default radius is a sensible small sphere (not 0, not huge).

## SpiralEffect

`test/unit/light/unit_effects_render.cpp`
*Also touches: RingsEffect, RipplesEffect, LavaLampEffect.*

- LavaLampEffect has localised blob features that can land on identical corner palette indices at some t values (corner-pair check is too strict). Scan the whole buffer for any two distinct pixels instead — same approach as RingsEffect below. LavaLamp paints at least one non-zero byte (effect actually renders).
- Across 10 frames at bpm=60, at least one frame shows two distinct colours somewhere in the buffer (blobs move and the field varies).
- RingsEffect has localised features (thin rings); corner-pair check is too strict, so we scan for any two distinct pixels instead. Rings paints at least one non-zero byte (effect actually renders).
- At least two distinct pixels exist somewhere in the buffer (rings are localised, so corner-pair would be too strict).
- RipplesEffect (MoonLight sine-wave water surface) lights one pixel per column at a sine-driven height. On a flat 2D layer it still paints a visible wavefront — assert it renders something and varies across the surface.
- Ripples lights one pixel per column at a sine-driven height, so the surface holds at least two distinct colours (wavefront vs background) — scan the whole buffer, corner-pair would be too strict.

## SystemModule

`test/unit/core/unit_SystemModule.cpp`

- On the desktop platform (MAC DE:AD:BE:EF:CA:FE), the auto-generated device name is "MM-CAFE" (last two MAC bytes).
- deviceName is bound as a Text control to the MAC-derived default ("MM-CAFE" on the desktop platform).
- deviceName is the single network identity, so SystemModule keeps it a valid hostname. A live edit to an invalid value ("My Room!") is coerced on the next loop1s tick (mm::sanitizeHostname), the same path mDNS/AP/DHCP read — so they never see spaces.
- An all-invalid name collapses to empty after sanitising; the MAC fallback then fills it, so deviceName is never empty (mDNS/AP/DHCP always have a name to register).
- An already-valid name is left untouched (idempotent) — a normal user name survives.
- The `bootReason` control is populated from platform::resetReason; on desktop it reports "OK".
- SystemModule accepts user-added Peripheral children (sensors/actuators the user solders on); the role string drives the type-picker filter + add policy.
- Regression: SystemModule overrides setup() and loop1s(); both must chain to MoonModule's base so a Peripheral child's setup()/loop1s() actually fire. Without the chain a sensor would never init or poll (the "children miss callbacks" trap from history/decisions.md). loop20ms() isn't overridden, so the base default already propagates it.
- roleName maps the new Peripheral enum to its lowercase API string.

`test/unit/core/unit_sanitizeHostname.cpp`
*Also touches: NetworkModule.*

- _sanitizeHostname leaves a valid hostname unchanged (idempotent)_
- _sanitizeHostname replaces spaces with a single dash_
- _sanitizeHostname strips punctuation and other invalid chars_
- _sanitizeHostname trims leading and trailing dashes / invalid runs_
- _sanitizeHostname yields empty for all-invalid input (caller falls back)_

## TextEffect

`test/unit/light/unit_TextEffect.cpp`

- Static text renders glyph pixels top-left. On a grid tall/wide enough for one line of the 6x8 font, a non-empty string lights some pixels; an empty string lights none.
- A multi-line string wraps: the second line renders on a lower row (font-height down), so a two-line string lights pixels below the first font's height. Uses the 4x6 font (height 6).
- Scroll mode advances the text over time and never crashes; on a degenerate grid it's a safe no-op.

## WaveEffect

`test/unit/light/unit_WaveEffect.cpp`

- _WaveEffect: sawtooth ramps 0→top across the phase_
- _WaveEffect: triangle peaks in the middle and returns_
- _WaveEffect: sine sits mid at the zero crossings_
- _WaveEffect: square is low then high_
- _WaveEffect: every type stays within the grid bounds_
- _WaveEffect: a zero-height grid never reads out of bounds_

## WheelLayout

`test/unit/light/unit_WheelLayout.cpp`

- _WheelLayout lightCount = spokes * ledsPerSpoke and matches the iterator_
- _WheelLayout indices are dense [0, count)_
- _WheelLayout coordinates are non-negative (centre-shifted into address space)_
- _WheelLayout different spoke counts give different layouts_

## WledPacket

`test/unit/core/unit_WledPacket.cpp`

- _WledPacket::build produces a valid WLED header (token/id/size)_
- _WledPacket::readName round-trips the device name_
- _WledPacket marker is set only when stamped, and stays WLED-valid_
- _WledPacket::isValid rejects short / wrong-magic / null input_
- _WledPacket::readName truncates a long name to the buffer, never overruns_

## crc

`test/unit/core/unit_crc.cpp`

- CRC-16/CCITT-FALSE has a well-known check value: "123456789" → 0x29B1. Pinning it proves the polynomial/init/reflection match the standard variant (so a fingerprint computed here matches any other CCITT-FALSE implementation).
- A change-detector: different content → (almost always) different CRC; identical content → same.
- Empty span returns the init value (no bytes processed).

## draw

`test/unit/light/unit_draw.cpp`

- mm::draw::pixel() writes inside the grid and silently clips outside it (no out-of-bounds write).
- A 1D line (a row): every pixel from a.x to b.x inclusive is lit.
- A 2D diagonal: endpoints are lit and the line is contiguous (one pixel per step on the main diagonal of a square).
- A 3D line: drives all three axes, endpoints lit, no out-of-bounds on a small cube.
- A line running off the grid clips: it draws the on-grid part and stops, no crash.
- The `shorten` parameter pulls the far endpoint back toward `a` by shorten/255 (with WLEDMM *2 rounding), so an effect can sweep a partial segment. For a→b = (0,0)→(8,0): shorten 255 draws the whole line (tip at 8), 128 ≈ half (tip at (16*128/255+1)/2 = 4), 1 = just the start pixel (tip 0), 0 = nothing. This pins the rounding of the shorten branch, previously untested.
- draw::blur on a 1D row matches the canonical carryover-seep reference byte-for-byte (same behaviour as FastLED blur1d / MoonLight blurRows), and is symmetric around a centred bright pixel.
- blur runs separably on every axis with extent>1: a 2D blur spreads a centre pixel to all four orthogonal neighbours; a 3D blur reaches the z neighbours too. And it never writes out of bounds.
- A glyph blits in the correct orientation — neither X-mirrored (a 'b' as a 'd') nor Y-flipped. 'L' is the ideal probe: its vertical bar must be on the LEFT and its foot on the BOTTOM row. A regression here (reading the wrong column bit or the wrong row direction) is what made the DemoReel name overlay render each letter mirrored on the display.

## light_types

`test/unit/light/unit_Coord3D.cpp`

- _Coord3D arithmetic is per-axis_
- _Coord3D modulo and divide fold per axis_
- _Coord3D % and / guard a zero or degenerate axis_
- _Coord3D equality_

## math8

`test/unit/core/unit_math8.cpp`

- sin8: a 256-entry sine LUT centred on 128, peaking near 255 and 0 a quarter and three-quarters of the way round. cos8 is sin8 shifted a quarter turn.
- triwave8: linear up 0→255 then down 255→0, peaking at the midpoint.
- qadd8/qsub8 clamp at the 0..255 ends instead of wrapping.
- nscale8 is the recognisable spelling of scale8 (n/256 channel scale), so nscale8(x,255)==x.
- beat8: a sawtooth completing `bpm` cycles per minute. At t=0 it's 0; halfway through a beat ~128.
- beatsin8: a sine oscillating in [low,high] at bpm. Stays in range across the cycle and actually moves (not stuck at one value).
- Random8: a seeded PRNG — same seed gives the same sequence (determinism), and below(n) stays under n. Two different seeds diverge.
- atan2_8 / dist8: the geometry helpers moved here from color.h still behave.
- map8 rescales 0..255 onto [lo,hi] inclusively — the top of the input must REACH hi (FastLED's map8 == map(in,0,255,lo,hi)). Regression: an earlier scale8-based form left hi unreachable, so a one-step span (a bar height of 1) collapsed to 0 — the bug GEQ3D's height mapping hit.

## noise

`test/unit/core/unit_noise.cpp`

- Determinism: the same coordinate always gives the same value (a pure function of position), so a field is reproducible frame to frame and across the 1D/2D/3D entry points at z/y = 0.
- Smoothness: neighbouring positions WITHIN a cell (sub-256 steps) differ only a little — that's what makes it value noise rather than a raw hash (which would jump randomly every step).
- Range: output is a full byte; over a swept field it uses a wide span (not stuck near one value).

## platform

`test/unit/core/unit_platform_clock.cpp`

- setTestNowMs freezes platform::millis() to the given value; passing 0 restores the real clock so subsequent test cases see fresh time.
