# Unit Tests

Auto-generated from `test/unit/{core,light}/unit_*.cpp` by `scripts/docs/generate_test_docs.py`. **Do not edit by hand** — update the source file's `@module` / `@also` and per-TEST_CASE `//` descriptions instead, then regenerate.

Unit tests are the fastest tier in the [test strategy](../testing.md): they run the production code in-process with doctest, no platform, no network. Each section below covers one module.

## ArtNetSendDriver

`test/unit/light/unit_ArtNetSendDriver_no_alloc_in_loop.cpp`
*Also touches: Drivers, Correction.*

- onBuildState sizes the correction-applied buffer to source-count × out-channels. The size matches what loop() needs on its first send. Calling loop() after onBuildState must not reallocate — pin the data pointer + shape.
- A preset toggle from RGB to RGBW grows outChannels from 3 to 4. The grow runs in onCorrectionChanged, off the hot path.
- A brightness-only change keeps outChannels at 3 — onCorrectionChanged is still called, but the resize short-circuits (existing buffer already fits).

`test/unit/light/unit_ArtNetSendDriver_packet.cpp`

- The built packet contains the exact header layout the Art-Net spec mandates: ID, OpCode, version, sequence, physical, universe, length, data.
- Universe 259 (0x0103) is encoded little-endian (low byte first), matching the Art-Net wire format.
- 256 RGB lights (768 bytes) split across exactly 2 universes (510 + 258), matching the 510-channel-per-universe cap.
- The data-length field is encoded big-endian (high byte first), unlike the universe field — matching the Art-Net spec.

## BlendMap

`test/unit/light/unit_BlendMap.cpp`
*Also touches: MappingLUT.*

- Identity mapping (logical N → physical N) leaves every byte unchanged.
- One logical light routed to multiple physical positions copies the colour to each (mirror-style mappings work).
- A paged LUT (forced via the maxAllocBlock test cap) must produce a byte-identical dst to a single-alloc LUT with the same mapping. Paging is an allocation detail; blendMap output must not depend on it. This is the end-to-end pin for the no-PSRAM-fragmentation fix.
- An additive (overwrites=false) LUT folding two sources onto one physical light adds and clamps at 255 (no overflow). overwrites=false is the opt-in for the rare overlap case (future multi-layer compositing); the default copy path would instead overwrite, so this pins the additive contract explicitly.
- The default (overwrites=true) path plain-copies: two sources mapped to the same physical means the LAST writer wins, no addition. Pins the fast path.
- Sparse overwrite mapping clears untouched physical cells. A sphere-style layout maps only a subset of the physical box to a source; the rest must end up black, not retain stale data from a previous frame. Pre-fills dst dirty and asserts unmapped cells are zeroed — fails if BlendMap's dst.clear() is removed (the regression target).

## BoardModule

`test/unit/core/unit_BoardModule.cpp`

- After onBuildControls, BoardModule exposes exactly one `board` control, bound as Text to a 32-byte buffer.
- Default state is the empty string — MoonDeck pushes a value on first reach.
- respectsEnabled() returns false so the `board` value stays visible even when the module is disabled — identity-class data shouldn't vanish.
- setBoard happy path: valid value lands in the buffer + marks dirty so FilesystemModule's debounced save picks it up. Mirrors the shape of NetworkModule::setWifiCredentials' unit-test pattern.
- Empty string is rejected — no buffer write, no dirty flag.
- 32+ char value is rejected (buffer is 32 bytes including NUL, so 31 max).
- Non-printable bytes are rejected. Catches accidental binary smuggling (would also break the persistence JSON encoder).
- nullptr is rejected (defensive — a bogus caller shouldn't crash the device).
- BoardModule is a code-wired System child and must NOT be user-deletable — now that SystemModule accepts user add/remove of (peripheral) children, the board identity opts out via userEditable() == false so the user can't delete it.

## Buffer

`test/unit/core/unit_Buffer.cpp`

- allocate(N,3) reserves count×channels bytes; count/channelsPerLight/bytes/data/span all reflect that.
- clear() zeroes every byte in the allocated range.
- Move-constructing transfers the data pointer and resets the source (no double free, no copy).
- Move-assigning transfers ownership the same way the move constructor does.
- Calling free() twice is harmless; pointer and count remain zeroed.
- allocate() refuses zero-count or zero-channels (returns false, no allocation, buffer left empty so a caller that ignores the bool doesn't get a partial state).

## CheckerboardEffect

`test/unit/light/unit_CheckerboardEffect.cpp`
*Also touches: SpiralEffect, PlasmaPaletteEffect, RingsEffect, RipplesEffect, GlowParticlesEffect, LavaLampEffect.*

- Checkerboard paints at least one non-zero byte on a 16×16 grid (effect actually renders).
- With cell_size=4, adjacent cells render different colours (the checker pattern is real, not uniform).
- LavaLampEffect has localised blob features that can land on identical corner palette indices at some t values (corner-pair check is too strict). Scan the whole buffer for any two distinct pixels instead — same approach as RipplesEffect below. LavaLamp paints at least one non-zero byte (effect actually renders).
- Across 10 frames at bpm=60, at least one frame shows two distinct colours somewhere in the buffer (blobs move and the field varies).
- RingsEffect has localised features (thin rings); corner-pair check is too strict, so we scan for any two distinct pixels instead. Rings paints at least one non-zero byte (effect actually renders).
- At least two distinct pixels exist somewhere in the buffer (rings are localised, so corner-pair would be too strict).
- RipplesEffect (MoonLight sine-wave water surface) lights one pixel per column at a sine-driven height. On a flat 2D layer it still paints a visible wavefront — assert it renders something and varies across the surface.
- _RipplesEffect spatial variation_

## CheckerboardModifier

`test/unit/light/unit_CheckerboardModifier.cpp`

- Identity dimensions — a mask doesn't resize the logical box.
- size=1: every cell is its own square; parity = (x+y+z)&1. Default (invert false) keeps even-parity cells, drops odd-parity.
- invert flips which parity passes — the cell that was dropped now passes and vice versa.
- size>1 groups cells into squares: with size=2, the 2×2 block at the origin is all one square (parity of 0/2=0), so all four pass; the next block over drops.
- Never fans out — at most one destination.

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

## Drivers

`test/unit/light/unit_Drivers_container.cpp`

- Disabled child drivers don't tick: toggling `enabled` flips whether that driver's loop() runs.

## FilesystemModule

`test/unit/core/unit_FilesystemModule_persistence.cpp`
*Also touches: Scheduler, Layer.*

- Persistence round-trip: set deviceName → save → recreate Scheduler+modules → load → assert. Uses fsSetRoot to isolate the test from any real /.config/ on disk. A control change (deviceName) saved with flush() reappears on the next boot once a fresh Scheduler loads the same path.
- Structural persistence: hand-write a Layer.json describing a different tree shape than the one main.cpp builds, then load and verify the live tree reconciles to match the JSON — type swap at position 0, trim of position 1. On load, a Layer's children are reconciled against the saved JSON: position 0 swaps to the saved type, extras at later positions are trimmed.
- Pins the wiredByCode-preserves-child contract that lets a new firmware revision add a code-created child (e.g. ImprovProvisioning under NetworkModule) without the child getting trimmed on every boot for users whose saved Network.json predates the addition.  Setup: an on-disk file describes Layer with zero children. Live tree has Layer with a RainbowEffect child that main.cpp would have wired and marked. After scheduler.setup() runs the persistence load, the wired child must survive. A code-wired child (markWiredByCode) survives a load from older JSON that doesn't mention it — new firmware additions aren't trimmed for existing users.
- Companion to the wiredByCode case above: when the JSON describes a different type at the position where a code-wired child lives, the position-replacement must NOT kill the code-wired child. Stop reconciliation at that index instead and let the next save re-write the file with the actual tree shape. When the saved JSON wants a different type at the position where a code-wired child lives, reconciliation stops at that index instead of destroying the wired child.
- Round-trip persistence with children: write a Layer subtree that contains both controls and child modules with controls of their own, then read the file back as text and verify it parses as valid JSON. Regresses the missing-comma bug between each child's "N.type" field and that child's first control (e.g. "0.type":"X""0.foo":1 instead of "0.type":"X","0.foo":1). Saving a Layer with multiple children produces valid JSON — comma separators between child `N.type` and the child's first control field are present.
- Singleton survives probe lifecycle: /api/types factory-creates a probe of every registered type (including FilesystemModule) to capture defaults, then deletes it. The probe's destructor must NOT clear the singleton — otherwise every save path (noteDirty, debounced loop1s, flushPending on reboot) silently no-ops for the rest of the device's life. The fix is to register the singleton in setScheduler(), not in the constructor. This test catches that singleton-clear regression. /api/types factory-creates a temporary FilesystemModule probe; its destruction must NOT clear the static singleton (otherwise every later save silently no-ops).
- Regression: Int16 controls (GridLayout's width/height/depth, Layer's start/end) round-tripped through the filesystem load path were clamped to c.min/c.max, which default to 0,0 because ControlDescriptor.min/max are uint8_t and can't represent an int16 range. Every Int16 control loaded as 0 — so a 128×128 grid became 0×0×0 after restart and the whole pipeline allocated no buffers. Int16 controls (GridLayout width/height, Layer start/end) preserve their saved value across load — no zero-clamping from uint8 min/max bounds.

## FireEffect

`test/unit/light/unit_FireEffect.cpp`

- On a 16×16 grid the heat buffer sizes to width × height bytes (one byte of heat per cell).
- With sparking at max, the buffer contains non-zero pixels within 50 frames (sparks emerge and propagate).
- Disabling the effect releases its heat buffer back (dynamicBytes drops to 0).

## GameOfLifeEffect

`test/unit/light/unit_GameOfLifeEffect.cpp`

- Two cell grids of width × height bytes each.
- Disabling releases both grids (dynamicBytes drops to 0) via the parent lifecycle.
- A blinker (horizontal 3-in-a-row) oscillates with period 2 under B3/S23: it becomes a vertical 3-in-a-row, then back. Pins both birth (B3) and survival (S23) on a known pattern.
- A 2×2 block is a still-life: every live cell has 3 neighbours (S3), no dead cell has exactly 3 (no B3), so stepOnce leaves it unchanged.
- A lone cell dies (underpopulation: 0 neighbours, not S2/S3) → extinction.
- Wraparound: a blinker on the right edge stays a valid 3-cell pattern because neighbours wrap, rather than losing cells to a hard edge.
- Reallocation on dimension change: grids resize, byte count tracks new w×h.
- Must not crash on a zero-size grid (no allocation, loop is a no-op).
- bpm time-gates the generation rate: a low bpm advances fewer generations per unit time than a high bpm over the same elapsed window. Drives time via the desktop millis() test seam (Layer reads platform::millis in loop()).
- Regression: the Layer clears the buffer before every effect frame, so the grid must be re-painted on EVERY frame, not just on the (rarer) beats where a generation advances. A bpm gate that skipped the paint left non-step frames black — visible as "a flash now and then" at low bpm. Drive several frames at a slow bpm (most are non-step) and require the buffer stays lit on all of them.

## GridLayout

`test/unit/light/unit_GridLayout.cpp`
*Also touches: Layouts.*

- A 4×4×1 grid yields 16 lights iterated row-major: x sweeps fastest, then y, then z.
- A 3D 2×2×2 grid yields 8 lights with z-plane separation (indices 0-3 at z=0, 4-7 at z=1).
- A single-light grid (1×1×1) is a valid layout: one coordinate at (0,0,0).
- Layouts with a single child delegates totalLightCount and forEachCoord to that child directly.
- Two child layouts produce contiguous physical indices: the second layout's coords are offset by the first's lightCount.

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

## Layer

`test/unit/light/unit_Layer_extrude.cpp`
*Also touches: RainbowEffect, NoiseEffect, PlasmaEffect, CheckerboardEffect, FireEffect, ParticlesEffect.*

- A D2 effect (Rainbow) on a 3D layer writes z=0 once; Layer::extrude copies that slice across every z>0 — slices are byte-identical.
- A D1 effect writes row y=0,z=0; extrude duplicates that row across every y and every z-slice.
- NoiseEffect declared D3 still produces a valid image on a depth=1 layer (it honours the runtime depth instead of hardcoding z).
- PlasmaEffect (D3) on a 2D layer same contract: valid 2D image, no buffer overrun.
- NoiseEffect (D3) on a 1D layer (height=depth=1) writes a valid strip and never overflows.
- PlasmaEffect (D3) on a 1D layer same contract: valid 1D strip, no overflow.
- CheckerboardEffect (D2) on a 3D layer: extrude copies z=0 to every z>0 (stateless D2 contract).
- FireEffect (D2, stateful — heat buffer sized to w×h) extrudes cleanly across z on a 3D layer.
- ParticlesEffect (D2, stateful — trail sized to w×h×cpl) extrudes cleanly across z on a 3D layer.

`test/unit/light/unit_Layer_phase_animation.cpp`
*Also touches: MetaballsEffect, CheckerboardEffect, LavaLampEffect, SpiralEffect.*

- Metaballs visibly changes over 100ms even when per-tick dt is sub-millisecond (no phase-accumulator truncation).
- Checkerboard advances at desktop speed (cells flip across 100ms).
- LavaLamp animates across 100ms (blobs move).
- Spiral animates across 100ms (rotation visible).
- Replace path: swap one effect for another mid-flight (same shape as HttpServerModule::handleReplaceModule) and confirm the new effect animates. Replacing one effect with another mid-tick (HttpServerModule's swap path) leaves the new effect animating, not frozen.

`test/unit/light/unit_Layer_sparse_mapping.cpp`

- Dense grid: every box cell is a light, so no LUT — the identity/memcpy fast path is preserved exactly (the grid short-circuit).
- Sparse sphere: a LUT is built; its destinations are driver indices in [0, lightCount), and the render buffer stays the dense bounding box.
- Sphere + Mirror: the modifier's box-coordinate destinations are translated into driver-index space; no destination escapes [0, lightCount).
- REGRESSION: a high fan-out Multiply (8×8×4 = 256) on a 128×128 grid must build a NON-EMPTY LUT that covers every physical light. The maxDest estimate (logicalCount × maxMultiplier) is computed in 64-bit; before that fix it overflowed uint16 on no-PSRAM boards (256 × 256 = 65536 wraps to 0), sized the LUT to ~nothing, and blanked the display. Here we assert the LUT actually maps the full light set, in range — the symptom that black-screened the device.

`test/unit/light/unit_Layer_zero_grid.cpp`
*Also touches: RainbowEffect, NoiseEffect, PlasmaEffect, CheckerboardEffect, SpiralEffect, MetaballsEffect, PlasmaPaletteEffect, RingsEffect, RipplesEffect, GlowParticlesEffect, LavaLampEffect, FireEffect, ParticlesEffect.*

- Rainbow on 0,0,0 grid: no crash.
- Noise on 0,0,0 grid: no crash.
- Plasma on 0,0,0 grid: no crash.
- Checkerboard on 0,0,0 grid: no crash.
- Spiral on 0,0,0 grid: no crash.
- Metaballs on 0,0,0 grid: no crash.
- PlasmaPalette on 0,0,0 grid: no crash.
- Rings on 0,0,0 grid: no crash.
- Ripples on 0,0,0 grid: no crash.
- GlowParticles on 0,0,0 grid: no crash.
- LavaLamp on 0,0,0 grid: no crash.
- Fire on 0,0,0 grid: no heat buffer allocated, no crash.
- Particles on 0,0,0 grid: no trail buffer allocated, no crash.

## Layers

`test/unit/light/unit_Layers_container.cpp`
*Also touches: Layer.*

- A Layers container with one child Layer must produce the same output as that Layer used directly (no-op container).
- With two child Layers, each one's loop() runs and writes its own buffer (the container iterates all enabled children).
- activeLayer() returns the first enabled child, or the only child if all are disabled (so dimensions stay queryable during boot/toggle-off).
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

- Reports D3 — handles all three axes. Pins the ModifierBase default too.
- Defaults (multiply 2/2/1, mirror true/true/false) reproduce the canonical mirror-XY pipeline: a 128×128 physical grid → 64×64 logical (each axis folds).
- multiplyZ tiles the Z axis too: 128×128×4 with multiply 2/2/2 → 64×64×2.
- PURE-FOLD EQUIVALENCE: with the defaults (mult 2, mirror XY), the corner logical pixel (0,0) fans out to all four physical corners — byte-identical to the old MirrorModifier corner test. This is the canonical-pipeline guarantee.
- PURE-FOLD EQUIVALENCE: an interior pixel folds to the same two columns the old mirrorX-only produced — original + horizontal reflection.
- No multiplication on any axis (all multipliers 1) → identity pass-through.
- Tiling WITHOUT mirror repeats (does not reflect) — multiply 2 on X, mirror off: logical x=0 lands at physical x=0 (tile 0) and x=64 (tile 1, identity offset), NOT x=127. This is the difference from a fold.
- multiplyZ on a 2D (depth-1) layout is a no-op: the effective multiplier clamps to the axis extent (1), so logD stays 1 and the layer isn't blanked. Before the clamp, multiplyZ=4 made logD = 1/4 = 0 → empty layer.
- A multiplier larger than the axis extent clamps to the extent (can't tile more times than there are pixels).
- maxMultiplier is the product of the raw controls (the fan-out upper bound).
- REGRESSION: maxMultiplier() must NOT wrap when all axes are maxed. The product 64×64×16 = 65536 overflows nrOfLightsType (uint16 on no-PSRAM) and would wrap to 0 — feeding the uint64 maxDest math in Layer::rebuildLUT an already-wrapped (possibly 0) multiplier → empty LUT → black display. It must saturate to the type max instead. (Single-axis tests above stay under the wrap; this one crosses it.) On uint32 (PSRAM) the product fits and isn't saturated — assert only the non-wrap, non-zero invariant that holds on both widths.
- REGRESSION: an 8×8 multiply must emit all 64 tile positions, not be truncated to 8. The Layer's scratch buffer is sized to ModifierBase::kMaxFanout (64); a smaller buffer (the original physicals[8]) silently dropped 56 of the 64 tiles, so a 128×128 grid showed only 8 tiles instead of the full 8×8 = 64.
- Fan-out never exceeds maxOut even if asked for more than the buffer holds.

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

## NoiseEffect

`test/unit/light/unit_NoiseEffect.cpp`
*Also touches: PlasmaEffect, RainbowEffect.*

- One tick on an 8×8 grid leaves at least one non-zero byte (noise paints somewhere).
- Opposite corners of a 16×16 grid carry different colours (noise is not flat).
- Noise and Rainbow produce visibly different frames on the same grid (sanity check that they're distinct algorithms).
- With depth > 1, adjacent and distant z-slices each render differently (3D noise, not a stack of identical 2D slices).
- Same z-slice variation requirement holds for Plasma — each depth plane renders differently.

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
- A large layout is index-downsampled (stride > 1) so the payload fits the send-buffer cap — but at REAL positions, not a padded box.
- Default fps is the rate-limited preview stream rate.
- Regression: deleting the active Layer must not leave a driver holding a dangling layer_ pointer. Previously Drivers::passBufferToDrivers early-returned when the active Layer was null, leaving PreviewDriver's layer_ pointing at the freed Layer; the next onBuildState read layer_->layouts() on freed memory and crashed the device (LoadProhibited → boot loop, since the broken tree persists). Now passBufferToDrivers clears the drivers' layer_/sourceBuffer_ to null, a safe idle state. This drives the real path: Drivers bound to a Layers CONTAINER (self-healing), the Layer removed, then buildState re-resolves activeLayer()=null.

## RainbowEffect

`test/unit/light/unit_RainbowEffect.cpp`

- A single frame on a 4×4 grid leaves the buffer non-zero (rainbow always paints somewhere).
- Pixel (0,0) is at full saturation and value (one channel exactly 255) — confirms hsvToRgb wiring.
- Distant pixels carry different hues (the rainbow gradient is spatial, not uniform).

## RmtLedDriver

`test/unit/light/unit_RmtLedDriver_lifecycle.cpp`
*Also touches: Drivers, Correction.*

- _RmtLedDriver sizes the symbol buffer in onBuildState_
- _RmtLedDriver keeps the symbol buffer across a rebuild (reinit must not free it)_
- _RmtLedDriver grows the symbol buffer when the grid grows_
- _RmtLedDriver releases the symbol buffer on teardown_
- MoonModule contract: teardown reverses setup, so setup→teardown→setup→teardown cycles leave no residue — no leaked heap (ASAN in the test runner catches that), no stuck state. After each teardown the driver must look untouched: no symbol buffer, no status. Run several cycles to surface any accumulation.
- Conditional control: loopbackRxPin is visible only while loopbackTest is on, hidden otherwise — but always bound (so a saved rxPin loads regardless). Same add-then-setHidden pattern as NetworkModule (architecture.md § Conditional controls). This pins the exact behavior that, with the old UI, showed the pin at the wrong times; a regression in the C++ flag now fails here.

`test/unit/light/unit_RmtLedEncoder.cpp`
*Also touches: Correction.*

- _encoder: one byte, MSB-first, 0 and 1 bits get the right pulse widths_
- _encoder: one light's channels emit channels*8 symbols in byte order_
- _encoder: GRB ordering comes from Correction, encoder is order-agnostic_
- _encoder: RGBW preset yields 32 symbols per light_

## Scheduler

`test/unit/core/unit_Scheduler_unique_names.cpp`

- A name with no collision is returned unchanged.
- The second module with a duplicate name gets " 2" suffixed; the first keeps its original name.
- Suffix counting increments past existing "-2" / "-3" suffixes ("Layer", "Layer-2", "Layer" → "Layer-3").
- deduplicateNamesInTree() walks the entire module tree in one pass and disambiguates every duplicate (used after persistence load).
- firstByName(name) returns the first match in DFS order, or nullptr if no module carries that name.
- If the disambiguating suffix would overflow the 16-byte name buffer, ensureUniqueName refuses to truncate and keeps the colliding name (sharp edge, documented).

## SphereLayout

`test/unit/light/unit_SphereLayout.cpp`

- lightCount() must equal the number of points forEachCoord emits — they share one shell predicate, so allocation and fill can never disagree.
- The sphere is HOLLOW: the centre lattice point (r,r,r) is never emitted, and neither is any interior point (distance < radius-0.5 from centre).
- radius = 1 is the smallest hollow sphere: the 6 axis neighbours (d^2=1) plus the 12 edge points (d^2=2) of the centre — 18 lights, no centre.
- The shell is symmetric about the centre: for every emitted point its mirror through the centre is also emitted (a sphere has no preferred direction).
- Physical indices are sequential 0..N-1 over the emitted shell points (no gaps from the unindexed lattice voids), so the buffer maps 1:1 to emitted lights.
- Default radius is a sensible small sphere (not 0, not huge).

## SystemModule

`test/unit/core/unit_SystemModule.cpp`

- On the desktop platform (MAC DE:AD:BE:EF:CA:FE), the auto-generated device name is "MM-CAFE" (last two MAC bytes).
- deviceName is bound as a Text control to the MAC-derived default ("MM-CAFE" on the desktop platform).
- The `firmware` control is always present and non-empty (either a real firmware key from build_info.h or the fallback "unknown").
- The `bootReason` control is populated from platform::resetReason; on desktop it reports "OK".
- SystemModule accepts user-added Peripheral children (sensors/actuators the user solders on); the role string drives the type-picker filter + add policy.
- Regression: SystemModule overrides setup() and loop1s(); both must chain to MoonModule's base so a Peripheral child's setup()/loop1s() actually fire. Without the chain a sensor would never init or poll (the "children miss callbacks" trap from history/decisions.md). loop20ms() isn't overridden, so the base default already propagates it.
- roleName maps the new Peripheral enum to its lowercase API string.

## platform

`test/unit/core/unit_platform_clock.cpp`

- setTestNowMs freezes platform::millis() to the given value; passing 0 restores the real clock so subsequent test cases see fresh time.
