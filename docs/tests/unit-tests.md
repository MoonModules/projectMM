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
- Two logical lights writing into the same physical light add and clamp at 255 (no overflow).

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
*Also touches: SpiralEffect, PlasmaPaletteEffect, RipplesEffect, GlowParticlesEffect, LavaLampEffect.*

- Checkerboard paints at least one non-zero byte on a 16×16 grid (effect actually renders).
- With cell_size=4, adjacent cells render different colours (the checker pattern is real, not uniform).
- LavaLampEffect has localised blob features that can land on identical corner palette indices at some t values (corner-pair check is too strict). Scan the whole buffer for any two distinct pixels instead — same approach as RipplesEffect below. LavaLamp paints at least one non-zero byte (effect actually renders).
- Across 10 frames at bpm=60, at least one frame shows two distinct colours somewhere in the buffer (blobs move and the field varies).
- RipplesEffect has localised features (thin rings); corner-pair check is too strict, so we scan for any two distinct pixels instead. Ripples paints at least one non-zero byte (effect actually renders).
- At least two distinct pixels exist somewhere in the buffer (ripples are localised, so corner-pair would be too strict).

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

`test/unit/light/unit_Layer_zero_grid.cpp`
*Also touches: RainbowEffect, NoiseEffect, PlasmaEffect, CheckerboardEffect, SpiralEffect, MetaballsEffect, PlasmaPaletteEffect, RipplesEffect, GlowParticlesEffect, LavaLampEffect, FireEffect, ParticlesEffect.*

- Rainbow on 0,0,0 grid: no crash.
- Noise on 0,0,0 grid: no crash.
- Plasma on 0,0,0 grid: no crash.
- Checkerboard on 0,0,0 grid: no crash.
- Spiral on 0,0,0 grid: no crash.
- Metaballs on 0,0,0 grid: no crash.
- PlasmaPalette on 0,0,0 grid: no crash.
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

`test/unit/light/unit_Layouts_toggle_cycle.cpp`
*Also touches: Layer, Drivers.*

- Disabling the only layout child and re-enabling it must not crash Drivers, and rendering resumes cleanly.

## MappingLUT

`test/unit/core/unit_MappingLUT.cpp`

- A fresh LUT carries no mapping (hasLUT==false, logicalCount==0); BlendMap takes the fast identity copy path.
- setIdentity(N) declares a 1:1 mapping for N lights without allocating a LUT; forEachDestination still iterates correctly.
- Each logical light can map to a different count of physical lights; forEachDestination yields every mapped index in order.
- free() releases memory and resets counts; build() can be called again to install a fresh mapping.

## MetaballsEffect

`test/unit/light/unit_MetaballsEffect.cpp`

- One tick on a 16×16 grid leaves at least one non-zero byte in the layer buffer (proves the effect rendered).
- Pixels at opposite corners of a 32×32 grid differ in colour (the effect is not flat-filling the buffer).

## MirrorModifier

`test/unit/light/unit_MirrorModifier.cpp`

- MirrorModifier reports D3 dimensions (handles all three axes via mirrorX/Y/Z toggles).
- A 128×128 physical grid with mirrorXY has 64×64 logical lights (effect only renders one quadrant).
- An odd-axis physical grid (127×127) rounds up: 64×64 logical lights with one centre row/column shared.
- mirrorZ on a 128×128×4 grid yields 64×64×2 logical lights (mirroring also halves the Z axis).
- With mirrorXY enabled, the corner pixel (0,0) maps to all four corners of the physical grid.
- The centre pixel on an odd-axis grid deduplicates: all four mirror reflections land on the same position so count=1.
- With no mirror axis enabled, mapToPhysical returns one position (identity pass-through).
- mirrorX-only yields two positions per logical pixel (original + horizontal reflection).

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
*Also touches: GridLayout, MirrorModifier, NoiseEffect, Drivers.*

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

- The three `detail` levels (1/2/3) downsample a 128-axis grid into 16/32/43 axes — distinct strides so the levels are visibly different.
- Even when downsampled, the frame carries the original grid dimensions so the UI's `decompress` hint can block-replicate back.
- detail=3 (largest payload) stays under lwIP's ~5.7 KB TCP send buffer so writeChunks completes in one whole pass.
- A small grid (≤ budget) is copied 1:1 with no downsampling — preview matches the original exactly.
- On RGBW (4-channel) sources the preview keeps only the first 3 channels — the wire frame is always 3 bytes per voxel.
- Default controls: fps=24 (preview stream rate), detail=3 (finest), decompress=true.

## RainbowEffect

`test/unit/light/unit_RainbowEffect.cpp`

- A single frame on a 4×4 grid leaves the buffer non-zero (rainbow always paints somewhere).
- Pixel (0,0) is at full saturation and value (one channel exactly 255) — confirms hsvToRgb wiring.
- Distant pixels carry different hues (the rainbow gradient is spatial, not uniform).

## Scheduler

`test/unit/core/unit_Scheduler_unique_names.cpp`

- A name with no collision is returned unchanged.
- The second module with a duplicate name gets " 2" suffixed; the first keeps its original name.
- Suffix counting increments past existing " 2" / " 3" suffixes ("Layer", "Layer 2", "Layer" → "Layer 3").
- deduplicateNamesInTree() walks the entire module tree in one pass and disambiguates every duplicate (used after persistence load).
- firstByName(name) returns the first match in DFS order, or nullptr if no module carries that name.
- If the disambiguating suffix would overflow the 16-byte name buffer, ensureUniqueName refuses to truncate and keeps the colliding name (sharp edge, documented).

## SystemModule

`test/unit/core/unit_SystemModule.cpp`

- On the desktop platform (MAC DE:AD:BE:EF:CA:FE), the auto-generated device name is "MM-CAFE" (last two MAC bytes).
- After setup, SystemModule exposes exactly 12 controls on desktop, including a deviceName Text control bound to the MAC-derived name.
- The `firmware` control is always present and non-empty (either a real firmware key from build_info.h or the fallback "unknown").
- The `bootReason` control is populated from platform::resetReason; on desktop it reports "OK".

## platform

`test/unit/core/unit_platform_clock.cpp`

- setTestNowMs freezes platform::millis() to the given value; passing 0 restores the real clock so subsequent test cases see fresh time.
