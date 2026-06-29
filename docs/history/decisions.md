# Decisions and Lessons Learned

## Actions for the Future

Distilled from four project iterations (MoonLight, v1, v2, v3). These
are concrete rules, not aspirations.

### Before writing any code

1. **CLAUDE.md is the constitution.** Write it first. Include: platform
   boundary rule, memory rules, build commands, code style, agent
   workflow restrictions. Vague rules produce vague code.
2. **architecture.md must be stable.** Don't start coding while the
   architecture is still being debated. Every mid-implementation
   architecture change costs more than the original design time.
3. **Write module specs before module code.** Each docs/modules/*.md
   defines: purpose, controls, behavior, edge cases, interactions.
   These are specifications, not reverse-engineered documentation.
4. **Write a UI specification.** Design the UI before implementing it.
   Define layout, controls, API, interaction rules. Don't discover
   the UI through bug reports.
5. **Define integration tests upfront.** Unit tests per component are
   necessary but not sufficient. Define full-pipeline tests that
   exercise the path from effect → buffer → LUT → blend+map → driver
   output.

### During implementation

6. **Concrete first, abstract later.** Build one working feature
   end-to-end before extracting patterns into abstractions. Don't
   design the MoonModule base class before the first effect works.
7. **Use /plan for every feature.** Review plans for: unnecessary
   files, inheritance where structs suffice, modifications outside
   the relevant directory. Reject and regenerate bad plans.
8. **Commit after every small success.** Git is the undo button. If
   the agent fails a build twice, rollback and re-prompt. Don't let
   it spiral into a fix-up loop.
9. **The agent never stages or commits.** The product owner stages,
   reviews, and commits manually.
10. **Refactor first, create second.** Prefer extending existing code
    over adding new files. Deletion is preferred over duplication.
    Every addition must pay for itself.

### Technical rules (embedded-specific)

11. **Zero allocations in the render loop.** No malloc, new, push_back,
    std::string in any code path called per-frame. Enforce with a
    test that intercepts malloc/free.
12. **PSRAM for bulk, IRAM for speed.** Large contiguous buffers
    (light buffers, LUTs) in PSRAM at boot. Never scattered small allocations
    in PSRAM during loops.
13. **Pace all network output.** Never blast UDP packets in a tight
    loop. Add inter-packet delay and FPS limiting. Missing pacing
    looks like rendering bugs but is network flooding.
14. **Use virtual interfaces, not dynamic_cast.** Modules interact
    through virtual methods. Layer should not know about MultiplyModifier
    or CheckerboardModifier by name.
15. **Rebuild propagation must be in the framework.** Don't check dirty
    flags per-module in main.cpp. Use an event/observer system or a
    centralized pipeline-changed signal.
16. **Separate build entry points per platform.** Root CMakeLists.txt
    for desktop, esp32/ wrapper for IDF. Don't pollute CMake files
    with if(ESP_PLATFORM) conditionals.

### Documentation and process

17. **Present tense only.** Docs describe the system as it is. Git
    commits are the history. No changelogs, no sprint files, no
    roadmaps in the repo.
18. **Single source of truth.** CLAUDE.md (rules), architecture.md
    (design), docs/modules/*.md (specs). Nothing else. If it's
    duplicated, delete one copy.
19. **Mandatory subtraction.** Periodically review and remove code
    and docs that no longer earn their place.
20. **Test suite is the safety net.** For both human and AI
    development. If the agent breaks a test, it broke the system.
    No exceptions, no skipping.

---

## Lessons from MoonLight (WLED fork)

### Memory and resource constraints
- On classic ESP32 (no PSRAM, 4MB flash), the framework's footprint
  left little room for effects. OTA updates and expandability were
  limited by tight resource budgets.

### Upstream dependency burden
- Diverging from the upstream framework (ESP32-SvelteKit) required
  tracking 1,271+ local changes to survive rebases. Ongoing
  maintenance friction.
- Feature implementation was delayed waiting for upstream adoption
  (Ethernet, async webserver).

### Testing was retrofitted, not foundational
- Automated tests only verified trivial cases. Complex interactions
  required code restructuring that broke encapsulation.
- Testing should be designed into the architecture from day one, not
  bolted on later.

### Sustainability
- Single maintainer carrying ~95% of workload while issue backlog
  grew with user base.
- Actively-developed system frequently broke previously working
  features — no regression safety net.

### What to carry forward
- Platform abstraction (zero `#ifdef` in module code) is essential.
- Self-describing UI (JSON-driven, no frontend code per new module).
- Test suite as safety guardrail for both human and AI development.

### Proven patterns from MoonLight (harvest for v3)

- **PhysMap — 2-byte mapping entries on no-PSRAM.** Union packing: map type (2 bits) + physical index or RGB cache (14 bits) in a single uint16_t. On PSRAM: 4 bytes with 24-bit indices. The map type is stored IN each entry, not in a separate array. Saves memory vs v3's CSR approach which uses a separate offsets array.
- **nrOfLights_t typedef.** `uint16_t` on no-PSRAM, `uint32_t` on PSRAM. Compile-time selection. Proven in production. Same concept as v3's `nrOfLightsType`.
- **addControl binds to class variable by reference.** Control stores a `uintptr_t` pointer to the variable. Hot-path code reads the variable directly — zero overhead. UI updates write through the pointer. Supports uint8_t, int8_t, uint16_t, uint32_t, int, float, bool, Coord3D.
- **Node — minimal memory.** Base class ~29 bytes + vtable. Effects add only their control variables (uint8_t each). A typical effect adds just 2 bytes on top. No std::string members.
- **LightsHeader — one struct for LEDs AND DMX fixtures.** Configurable `channelsPerLight` (3=RGB, 4=RGBW, up to 32 for moving heads) with offset fields for red/green/blue/white/pan/tilt/zoom/rotate/gobo. This IS the "light = LED pixel or DMX fixture" concept.
- **Region start/end percentages.** MoonLight's `startPct`/`endPct` as Coord3D percentages (0-100) of the total fixture. Shipped in v3 as a modifier, not as Layer controls.
- **oneToOneMapping / allOneLight fast paths.** Boolean flags that skip the mapping table entirely when mapping is 1:1 identity. Essential for the no-PSRAM 12K LED case.
- **Transition brightness.** Per-layer animated brightness overlay (current, target, step). Enables smooth fade-in/out when switching effects. Should be added to v3.
- **SharedData — zero-allocation inter-node communication.** Single struct shared by all nodes: 16-band FFT audio, volume, beat detection, gravity (IMU), status. Lightweight alternative to pub/sub.
- **Coord3D with rich operators.** Full arithmetic (+, -, *, /, %), comparison, distanceSquared, isOutofBounds. Uses `int` (not int16_t) to avoid intermediate overflow.
- **10+ layout types proven.** Panel, Ring, Rings241, HexaPanel, Cloud, Human, Cone, Globe, SpiralGlobe — all algorithmic, no stored positions.
- **60+ effects across 5 libraries.** MoonLight originals, MoonModules community, WLED ports, FastLED demos, SoulmateLights.
- **11 driver types.** FastLED, Parallel LED, Hub75, ArtNet/E1.31/DDP in/out, DMX in/out, audio (WLED + FastLED), IMU, infrared.

---

## Lessons from projectMM v1

### Module system collapse
- `StatefulModule` conflated five distinct jobs: lifecycle, JSON
  descriptors, persistence, child management, and timing windows.
  Should have been separate concerns.

### PAL boundary erosion
- Only ~50 of 1,397 PAL lines handled actual platform abstraction.
  The rest was networking, OTA, NTP, and system queries that should
  have been modules. The PAL grew into a kitchen-sink.

### Manager overloading
- `ModuleManager` became REST adapter, memory accountant, state
  persister, and dirty-flag debouncer simultaneously. Too many
  responsibilities in one class.

### Documentation bloat
- 90 markdown files across ~15,400 lines. A contributor cannot grasp
  the core concept in five minutes.
- Architecture documentation read as historical narrative rather than
  prescriptive design.
- Per-module documentation duplicated JSON schemas already rendered
  by the UI.

### Testing pyramid never materialized
- Four parallel test surfaces (unit, live, scenario, code-analysis)
  were created independently and never unified despite each solving
  real problems.

### What worked
- Line-count tagging per file enabled fast assessment of bloat.
- Frugality principle: every addition must pay for itself.
- Growth budgets with CI enforcement prevented drift.
- Architecture as constraint document (not narrative).

### Key insights for v2+
- Guardrails precede code. Framework enforced before first module
  written prevents drift accumulation.
- Mandatory subtraction: every release must remove something.
- Recurring evaluation sprints to catch bloat early.

---

## Lessons from projectMM v2

Extracted from design review session with Gemini (2026-05-18).

### The "Architecture Astronaut" trap
- v2's "maximize minimalism" philosophy led to over-engineering minimal
  abstractions. 19 sprints for release 1 is a red flag — the agent was
  caught in a loop of refactoring abstractions instead of shipping
  concrete features.
- When agents are told to be "minimal," they write highly abstract,
  overly clever code (template metaprogramming, deep trait trees) to
  keep things "elegant." On embedded systems, minimalism means flat,
  simple, predictable code — not abstract code.

### The MoonModule base class dilemma
- The generic MoonModule lifecycle (setup, loop, loop20ms, loop1s,
  teardown) tried to be an OS-level framework before the domain logic
  (LED pipeline) was working.
- Giving an agent a highly abstract concept like "a generic lifecycle
  module that handles UI, WiFi, and LED drivers simultaneously"
  generates massive boilerplate, interfaces, and registries.
- Solution: build concrete features first, extract common patterns
  later. "Concrete first, abstract later."

### Uncontrolled creation
- If you let an agent write code, tests, and build scripts without
  strict guardrails, it will solve every new problem by creating more
  code. It doesn't care about technical debt because it doesn't have
  to maintain it.
- The "Refactor First" rule: when adding a feature, prefer extending
  an existing module over creating a new file. Deletion is preferred
  over duplication.

### Sprint file accumulation
- Using release/sprint files as "AI short-term memory" backfired.
  When the agent reads its own past sprint text, it treats abandoned
  or over-engineered ideas as strict instructions. Outdated
  documentation wastes context window.
- Solution: single source of truth (CLAUDE.md + architecture.md).
  Git history is the log. Active workspace stays clean.

### Context drift and compilation loops
- When the agent hits a compilation error, it tries to rewrite
  multiple files to fix it, bloating the context window. If it fails
  twice, it's stuck in a context loop.
- Solution: rollback with Git immediately. Don't let the agent keep
  trying. Approach the bug with a different, tighter prompt.

### CLAUDE.md as "Constitution"
- CLAUDE.md must be written BEFORE any code. It's the project's
  constitution. If it's weak or vague, the agent exploits the
  loopholes and writes messy code.
- Must include: exact compiler flags, memory rules, error handling
  limits, style guide, platform boundaries.
- Anti-stalling clause: "If a build error takes more than 2 attempts
  to fix, STOP and ask the human."

### /plan mode is essential
- Must use /plan before every feature. It's the design review phase.
- Red flags to watch for in plans: adding dependencies when a small
  function would do, introducing inheritance when structs suffice,
  modifying files outside the relevant directory.
- Reject bad plan steps explicitly. Only approve when the plan is
  minimal and precise.

### Git as the undo button
- Commit after every small successful feature. Be ruthless with
  rollbacks.
- The agent has direct file system access. If it panics on a build
  error, it writes 500 lines of messy fix-up code. `git reset --hard`
  is faster than debugging the mess.

### Build system
- PlatformIO vs CMake: PlatformIO handles toolchains well for ESP32
  but its `native` desktop support is limited. CMake is ESP-IDF's
  native build system. Dual-purpose CMakeLists.txt that works for
  both desktop and idf.py is the cleanest approach, but risks
  conditional pollution. Separate entry points (root CMake for
  desktop, esp32/ wrapper for IDF) is pragmatic.

### Memory and PSRAM
- Banning PSRAM entirely is wrong for 10K+ LEDs. A single RGB frame
  is 30KB. With double-buffering and LUTs, you exceed 320KB internal
  SRAM easily.
- OPI PSRAM (80-120MHz, 16-bit bus) has sufficient bandwidth for
  sequential light data streaming.
- The rule: PSRAM allowed for large contiguous bulk allocations at
  boot. Never small scattered allocations in loops.

### ArtNet packet pacing
- Blasting 97 UDP packets per frame with zero delay causes receiver
  packet drops. Inter-packet delays (50us) and FPS limiting are
  required.
- The absence of pacing produced symptoms that looked like rendering
  bugs (missing lights, random output) when the actual issue was
  network flooding.

### Proven patterns from v2 (harvest for v3)

- **DataBuffer — lock-free single-slot SPSC.** Atomic revision counter with acquire/release semantics. Zero branches on hot path. Producer calls `acquire_write()` + `publish()` (one atomic store). Consumer calls `try_acquire_read()` (two atomic loads, nullptr if no new frame). Multiple consumers each track their own read position. Teardown-safe via invalidate() sentinel.
- **DataRegistry — type-erased buffer directory.** Producers declare buffers by id, consumers resolve by id. Hot-path cost: zero (consumers cache the pointer). Domain-neutral: stores void* + count + elem_size + dimensions.
- **onBuildControls() / onAllocateMemory() separation.** Controls registered in onBuildControls() (supports rebuild via clearControls()), memory allocated in onAllocateMemory() (sets moduleAllocBytes_ — single source of truth). Better than v1's "do everything in setup()".
- **PAL split into one file per concern.** PalHeap, PalRtos, PalUdp, PalWifi, PalFs, PalHttp, PalWs, PalSystemInfo. Each has ESP32 and PC implementations. Clean, focused, testable.
- **PixelEffectBase — shared effect spine.** Eliminates ~70 lines of boilerplate per effect. Concrete effect implements only `build_effect_controls()` + `render_(px, w, h, d)`. Base handles layout resolution, buffer management, teardown safety.
- **Multi-core scheduler with per-module core affinity.** Effects on core 0, drivers on core 1. Module declares `coreAffinity()` in constructor. Scheduler pins tasks via PalRtos.
- **Canvas view (UI).** v2 introduced a node-graph canvas view alongside the traditional tree view. Modules shown as draggable nodes with SVG connection lines. Powerful for understanding pipeline topology. Note: introduces significant UI complexity — adopt carefully in v3.
- **AutoWireSpec — declarative input wiring.** Modules declare dependencies as data (inputKey, searchType, allMatches, backKey). ModuleManager auto-wires at startup. Eliminates manual strcmp chains.
- **Footprint reporting — zero boilerplate.** classSize set once at registration via `register_type<T>()`. No CRTP, no macro. dynamicMemorySize() = moduleAllocBytes_ + framework overhead.
- **Field order optimized for padding.** MoonModule fields ordered 8B → 4B → 2B → 1B, saving 24 bytes vs naive order. Matters when many modules are loaded.
- **Separate frontend files.** index.html + app.js + style.css (not one monolithic HTML). Easier to maintain.

### Map-on-the-fly vs separate Map stage
- A separate Map stage that copies between logical and physical
  buffers doubles memory usage.
- Map-on-the-fly (applying the LUT during generation or blend) avoids
  intermediate buffers and halves memory.
- But: blend+map writes to arbitrary physical positions (not
  sequential), so the output buffer must be fully populated before
  any driver reads from it.

---

## Lessons from projectMM v3

### Product owner as critical success factor

The single biggest improvement in v3's approach: the human is an active, hands-on product owner — not a passive requester. In v1 and v2, the agent had significant autonomy: it designed, implemented, tested, and committed with light oversight. The result was bloat, architectural drift, compounding bugs, and code the human couldn't fully understand.

In v3, the product owner:
- Reviews every line of generated code before committing
- Specifies requirements in detail — the agent asks, it doesn't guess
- Controls all git operations (staging, committing, pushing)
- Tests on real hardware before approving
- Questions design choices ("why static_cast here?", "is this future-proof?", "do we need this?")
- Catches overengineering early ("that's too much code for something we might change later")
- Rejects suggestions that add complexity without clear value

This is the fundamental lesson: in agentic coding, the agent writes code but the human must think. The agent is a tool, not a decision-maker. Tight human control produces cleaner, simpler, more predictable systems than giving the agent autonomy.

### Specs-before-code works

Writing module specs before implementation prevented the architectural drift that plagued v1 and v2. Each spec documents: purpose, controls, behavior, edge cases, prior art. When the code deviates from the spec, one of them is wrong — the spec serves as the reference. One source of truth per module: the spec lives in `docs/moonmodules/`, deleted nowhere else.

### Zero-copy preview driver (memory lesson)

The initial PreviewDriver allocated a 49KB frame buffer to copy pixel data before sending via WebSocket. Following MoonLight's pattern (drivers read directly from the physical buffer), we eliminated the copy — saving 49KB on ESP32 without PSRAM. The lesson: always check if an existing buffer can be reused before allocating a new one.

### WebSocket GUID typo cost hours

A single wrong character in the RFC 6455 magic GUID (`5AB5FDF632E5` instead of `C5AB0DC85B11`) caused the WebSocket handshake to fail silently — the SHA-1 was correct, the response format was correct, but the browser rejected it. The accept key matched our computation but not the browser's because the GUID was wrong. Lesson: when implementing protocols, verify against the RFC test vectors, not just internal consistency.

### KPI tracking in every commit

Adding standardized KPIs (binary size, FPS, heap usage, test count, lizard warnings) to every commit message makes performance regressions visible in git history. The `collect_kpi.py --commit` script automates this.

## Lessons from projectMM v3 (this project)

### Single-file MoonModules are good
- Keeping each MoonModule in a single .h file (no .cpp) reduces
  file count and makes authoring easy. A developer creates one file
  and implements the interface. This pattern should be kept.

### Controls work well
- The fixed-capacity control array with typed values (uint16, bool,
  text) and auto-rendering in the UI is a good pattern. Adding a
  control to a MoonModule automatically makes it visible and
  editable. No UI code changes needed.

### The plan.md pattern works
- Having a plan.md that shrinks as steps are completed gives clear
  progress visibility. Removing completed steps keeps the document
  focused on what's next.

### Generic children eliminate boilerplate
- Moving children array + addChild/removeChild to MoonModule base eliminated ~120 lines of duplicated code across Layer, DriverGroup, LayoutGroup. The typed arrays (effects_, modifiers_, drivers_, layouts_) and typed add methods were unnecessary — role() distinguishes child types at the call site.

### Dynamic arrays over fixed-size
- Replacing fixed arrays (MAX_CHILDREN=8, ControlList<8>) with grow-on-demand eliminated arbitrary limits. Leaf modules pay zero cost (nullptr). No hot-path impact — allocation happens during setup only.

### classSize via template, not per-class override
- ModuleFactory::registerType<T>() captures sizeof(T) automatically. Eliminated 10 boilerplate `classSize() const override` lines. Same pattern as v2's register_type.

### Naming matters
- `isOneToOne()` was misleading — "1:1" includes both sequential and shuffled. Renamed to `hasLUT()` (asks the real question: is there a table?). `setIdentity()` for the no-table case. Four mapping types defined: 1:1 identical, 1:1 shuffled, 1:0 unmapped, 1:N multimap.
- Semantic variable names (`availableHeap` not `available`, `internalHeap` not `internal`) — a reader should understand without looking at the assignment.

### Per-module timing reveals bottlenecks
- tickTimeUs as primary metric (FPS derived) exposed that ArtNet takes 51% of frame time on ESP32 128x128. Without per-module timing this would be invisible.
- Memory reporting (classSize + dynamicBytes per module) revealed the LUT over-allocation: maxMultiplier() on MirrorModifier saved 64KB by using 4x instead of hardcoded 8x.

### Live scenarios must be non-destructive
- Initial implementation created modules on the running device without cleanup — the device ended up slower with extra effects. Fix: track created modules, delete them after each scenario. Show `=` for existing vs `+` for new.

### freeHeap vs freeInternalHeap
- On ESP32 with PSRAM, freeHeap() returns combined (internal + PSRAM). The HEAP_RESERVE check must use freeInternalHeap() because stack/HTTP/WiFi need internal RAM, not PSRAM. On desktop both return 0 (unlimited).

### Rule of Five catches real bugs
- MoonModule and ControlList owned raw pointers but had implicit copy/move — CodeRabbit caught the double-free risk. Delete copy/move on any class that owns raw memory.

### setName must copy, not store pointer
- HTTP module creation stored a pointer to a stack-local buffer. After the function returned, the name was garbage. Fixed by making name_ a char[24] buffer with memcpy in setName().

## Lessons from the next-iteration branch (plans 08-12)

The branch covering SystemModule/NetworkModule, persistence, the UI rewrite, the eth-only build, and the side-nav. Brief, actionable takeaways:

- **Plan-09 was right to abandon part of itself.** The first persistence attempt (~1700 LOC) didn't pay for itself; ~700 LOC of genuine foundations (partition scheme, platform fs API, MoonModule additions) were kept and the rest dropped. Plan-10 then succeeded with a far smaller control-list-driven design. Lesson: a plan that produces "keep the foundations, drop the feature, re-plan smaller" is a success, not a failure.
- **ESP-IDF v6.x removes WiFi via `EXCLUDE_COMPONENTS`, not a Kconfig flag.** `CONFIG_ESP_WIFI_ENABLED` is non-settable in v6.x — setting it `n` is silently ignored. The eth-only profile excludes `esp_wifi`/`wpa_supplicant`/`esp_coex` components (NOT `esp_phy` — the EMAC needs it) plus an `MM_NO_WIFI` define gating `if constexpr` branches in core. Verify a build profile actually changed something (image size, `nm` for symbols), don't trust the config.
- **The render tick collapses on any blocking network write on the hot path.** The FPS-swing was a blocking 49 KB preview WebSocket write spinning `vTaskDelay` until lwIP drained. Fix: non-blocking scatter-gather write + downsample the payload to fit the send buffer. Any per-tick socket write must be non-blocking and abandon-on-backpressure.
- **WiFi UDP is ~4× the Ethernet per-packet cost.** ArtNet at 16K lights is ~7 FPS on WiFi vs ~19 on Ethernet — WiFi physics (CSMA/CA, retries, rate adaptation), not a code regression. Recommend Ethernet (or the eth-only profile) for large installations.
- **No-op wrappers are an "unnecessary abstraction" the Reviewer must catch.** Two were found and removed (`HttpServerModule::parseJsonString` re-namespacing `mm::json::*`; `NetworkModule::rebuildLocalControlsAndPipeline` whose name contradicted its body). A pure pass-through that only renames is the opposite of duplication — a single thing that shouldn't exist. CLAUDE.md's Reviewer definition now names this pattern explicitly.
- **Always escape strings going into hand-built JSON.** A control value containing `"` or `\` produced malformed JSON in `/api/state` and the persisted config. Hand-rolled JSON serialization must escape on write and un-escape on read — caught by a round-trip test, not the reviewer.
- **Doc-vs-code drift hides in design changes.** When the password approach changed mid-implementation (length-only → XOR+base64), the docs and one header were updated but a second header's comment was missed — it then documented a security property the code didn't provide. When a design changes, grep for every mention of the old behaviour.

## Lessons from this branch (plans 13-16)

Plan-13 (nest child cards inside parent box), plan-14 (replace-type button), plan-15 (stream `/api/state`), plan-16 (Layouts/Layers/Drivers top-level reshape) — plus the unplanned mid-branch work the user surfaced (effect-animation freeze, Int16 zero-corruption, Layouts container disable crash, MoonModule status slot, src/light/layers/ reorganisation, FilesystemModule + Scheduler split).

- **A single `c.min`/`c.max` field can't bound multiple numeric widths.** `ControlDescriptor.min/max` are `uint8_t`; applying them to `Int16` / `Uint16` controls clamps every value into `[0..0]`. Default `addInt16` and `addUint16` leave bounds at `0,0` precisely because the slot can't represent the wider range — and on the load path that mistake silently zeros every Int16 control on every reboot. The bug shipped, was caught next session by "Layouts cannot be activated after reboot," and was reverted with explicit comments in `Control.h` documenting why bounds stay 0,0. Lesson: when a validation field's storage is narrower than what it claims to validate, the validation is wrong, not the field. A type-correct alternative (wider bounds, or per-type bound slots) is the real fix; until then, document the constraint at the field's declaration.

- **Per-tick integer division can round the animation rate to zero.** `phase += dt * bpm * 256 / 60000` truncates to 0 when `dt < 234 / bpm` ms. ESP32 at 16K LEDs (`dt ≈ 62ms`) animates fine; desktop at `dt ≈ 0..1ms` and small grids on ESP32 freeze. Four effects shipped frozen on desktop for an entire iteration before a user noticed. Fix is to keep the raw `dt * bpm` numerator in the accumulator and divide only at the read site (the pattern NoiseEffect already used). Rule, now in CLAUDE.md § Hard Rules: "effects must run at every grid size and tick rate."

- **Early-return on degenerate inputs leaks state, not safety.** `Layer::onAllocateMemory` early-returned on empty layouts (zero lights) without resetting its LUT or buffer; Drivers then reallocated its output buffer to 0 bytes while the stale LUT still pointed at 16K destinations, and the next `blendMap` dereferenced null. "Nothing to do" branches must still bring the module into a consistent zero state (dims = 0, LUT freed, buffer freed), not just skip work. Same lens as the zero-grid effect rule.

- **HTML5 `dragstart`'s `e.target` is always the draggable element, not the deepest descendant under the mouse.** Excluding regions of a draggable element via `e.target.closest(".child-class")` in `dragstart` never matches because `e.target === card`. The reliable signal is the *mousedown* target; toggle `draggable` on mousedown based on where the grab actually landed, with `touchstart` mirror for mobile. The naive `dragstart` filter pattern shipped silently because the existing exclusion list happened to include `<input>` (covered most controls); only when the controls region used a `<details>`/`<summary>` did the bug surface.

- **Severity is a real axis on the "module has something to say" channel.** A single `warning` slot conflates "this is fine, here's your IP" with "this degraded silently, look closer" with "this failed." Three levels (`Status` / `Warning` / `Error`) earn their keep when more than one module produces non-degradation messages (NetworkModule routes its old ReadOnly `status` control through the same slot). Default = `Status` (neutral info) so `setStatus("connected")` looks right; severity is only set explicitly when something is bad. Wire format mirrors the C++ enum names lowercased (`"status"` / `"warning"` / `"error"`) — collision with the field name `status` is documented at the introduction site so the bespoke choice carries its reason.

- **Lifecycle events fall into "commit" and "merge"; "push" had no work of its own.** A first cut of the lifecycle gates carved out a separate Push event for the Reviewer agent — but that produced the "address-reviewer" follow-up-commit anti-pattern (reviewer flags an issue → noise commit → trail full of "fix reviewer"). Moving the reviewer to Commit was worse: every substantial commit paid 5-7 min + tokens. Final shape: reviewer at PR-merge over the whole branch diff (where architectural drift is visible across commits, not within one); on-demand pre-commit as the safety valve when the product owner asks. Push has no gate.

- **The reviewer agent flags real findings AND wrong ones; triage matters.** Three different reviewer passes over this branch produced ~20 findings. ~30% were valid, the rest either misread line numbers (cited the wrong region), or repeated a finding already accepted in an earlier commit, or proposed a "fix" that would re-introduce a bug we just fixed (the `c.min`/`c.max` Int16 finding twice). When a reviewer finding is wrong, "skip with one-line reason in the commit body" is more honest than either fixing it to satisfy the reviewer or rejecting it silently. The reason text becomes the audit trail.

- **The docs hierarchy now has a name shape: top-level system docs are flat (`architecture.md`, `coding-standards.md`, `building.md`, `testing.md`), per-module specs live under `docs/moonmodules/<role>/<Name>.md`.** The earlier `architecture.md` + `architecture-light.md` pair was asymmetric ("the general doc + the light-domain deepening") and pulled toward growing more suffixes. Merging back into one `architecture.md` with `# Core` / `# Light domain` top-level sections matched the convention every well-known C++/web project uses (Linux, Django, Rust). Subdirectories `core/` and `light/` only kick in under `moonmodules/` because there's a plural of each kind there. Bespoke `*-light.md` / `*-coding.md` suffix patterns are now ruled out by the "common patterns first" principle.

- **`util/` is a category that classifies by "what is this thing" rather than "what does it do" — and it's the kind a new contributor learns nothing from.** When `src/core/` grew to 17 files I considered a `util/` bucket for the small headers (`Base64.h`, `Sha1.h`, `JsonSink.h`, etc.) and rejected it: each file already names what it does (it's not "util", it's WebSocket-handshake base64 / hash); grouping by file-shape rather than concern is the Frankenstein pattern. Same reason `modules/` got rejected: the four system services (Filesystem / Network / System / HttpServer) are singletons of their kind, not "modules of role X" — folder-grouping requires a plural of each kind to earn its keep. The right cleanup turned out to be header-only → `.h+.cpp` splits (done lazily as files were touched), not folders.

- **`.claude/scheduled_tasks.lock` is harness runtime state.** Per-session lock file the Claude Code harness uses to coordinate scheduled wakeups; not project content. Add `.claude/*.lock` to `.gitignore` (a broader pattern than per-file, since other harness lock files will follow the same shape). The single-file ignore on `.claude/settings.local.json` was too narrow.

## Lessons from this branch (plans 17-23)

The plan-18 branch landed plans 17 + 18 + six unplanned follow-ups (19, 19.1, 20, 21, 22, 23). What earned its keep, what surprised us, what to remember.

- **CORS-on-static-files isn't a thing GitHub Pages can fix from your side.** Plan-17's web installer assumed cross-origin fetches of GitHub release-asset `.bin` files would work; they don't (the GitHub Releases CDN returns no `Access-Control-Allow-Origin`). Plan-18 step 0 falsified this empirically and pivoted to "self-host the last N releases' binaries on Pages content" — same-origin with the install page, no CORS. The lesson sits both ways: don't assume CORS shape; AND don't fix CORS via a third-party proxy (WLED's `proxy.corsfix.com` dependency) when self-hosting works.

- **HTTPS-page → HTTP-LAN fetches are blocked by Chrome's mixed-content policy, no override available cleanly.** A Pages-hosted install page can't `fetch("http://192.168.1.X/api/state")` even if the device has `Access-Control-Allow-Origin: *` set correctly. The block happens before the request leaves the browser. Plan-20's Diagnose feature moved to the device UI (same-origin, mixed-content moot) instead. Lesson: pick the side of the security boundary that actually works; trying to "fix" mixed-content from the device side is wasted code.

- **ESP Web Tools' rich panel is in-browser-session-only.** "Visit Device" + "Configure Wi-Fi" rows appear right after provisioning; close the tab and the panel collapses to "Install + Logs" because the device URL is browser-side memory, not asked-back from the device. Plan-18 fix-pack added a `GET_CURRENT_STATE` → URL follow-up on the device (ESPHome pattern, mirrors what Improv allows) — verifiable via `improv_probe.py`, but it does NOT change the ESP Web Tools UI. Lesson: when a feature lives in a third-party tool's state machine, surfacing data from the device is necessary but not sufficient; the third-party tool's own state model also has to use it.

- **`improv_provision` rejects new credentials when WiFi STA is already connected — that's by design.** The Improv listener returns `ERROR_UNABLE_TO_CONNECT` if `wifiStaConnected()` at the time of `WIFI_SETTINGS` — protects large installs from a scan-induced ArtNet drop. The browser dialog says "Unknown error (255)" though, because Improv's error code mapping doesn't carry a human reason. Lesson: protocol-level "this is fine, expected" and browser-level "something is wrong" don't line up automatically. Document the rejection at every layer the user encounters it.

- **Per-board build directories should land as `build/<board>/`, not `<chip>/build/<board>/`.** Plan-19.1's choice of `build/esp32-<board>/` (e.g. `build/esp32-esp32-eth-wifi/`) keeps every target under one root (`build/`) and shares the namespace with desktop targets (`build/macos/`, `build/linux/`, `build/windows/`). The doubled prefix on the ESP32 side (`esp32-` for the chip family + `<board>` for the variant) is intentional — earlier draft put ESP32 builds under `esp32/build/<board>/`, which made `clean --all` more fragile because it had to walk both `esp32/build/` and `build/`. One root, one cleaner.

- **`idf.py -B <dir>` needs `-DSDKCONFIG=<dir>/sdkconfig` to keep per-build-dir sdkconfigs isolated.** Without the SDKCONFIG override, idf.py writes `<project>/sdkconfig` at the project root, which all per-board build dirs then share — switching boards trips the "project sdkconfig was generated for target X, but CMakeCache contains Y" abort. The fix is a one-liner per script that invokes idf.py, but it's not obvious until the failure happens on the second board build. Caught by the per-board build sanity-test step of plan-19.1, retrofitted to build_esp32 / flash_esp32 / collect_kpi.

- **Trying to add a child module to NetworkModule wasn't the small refactor it looked like.** Plan-21 (Improv as Network child) reverted the same session. The blocker looked like `Scheduler::tick()` only walking **top-level** modules for `loop20ms` / `loop1s`, so children never got tick callbacks. Lesson: "obviously the right shape" can hide infrastructure that isn't ready for that shape. **Resolved by the Peripheral-role branch:** the real gap was narrower than "the scheduler doesn't tick children" — `MoonModule`'s base lifecycle *does* propagate every callback (`loop`/`loop20ms`/`loop1s`/`setup`/`teardown`/`onBuild*`) to children; a child only misses a callback when the parent **overrides that method and forgets to chain to base**. The fix is therefore per-parent, not a scheduler refactor: SystemModule (which accepts user-added Peripheral children) overrides `setup()` and `loop1s()` and now chains both to `MoonModule::`, so peripheral children init and poll. `unit_SystemModule` pins this. The general rule — override-and-chain, with the parent-before/child-before convention per callback — lives in [coding-standards.md § Override-and-chain convention](../coding-standards.md#override-and-chain-convention).

- **Splitting `platform_esp32.cpp` is safe at public-API boundaries, not at section banners.** Plan-23 cut Improv + OTA + LittleFS into siblings because each owns its private state and talks back to the rest of the platform layer only through `platform.h`. Network stayed in the core file because Eth + WiFi + sockets + mDNS share eight file-scope variables (event handlers, netif pointers, init-done flags). The criterion: a split happens at a public-API boundary, not at a "this section reads coherent in the diff" boundary.

- **Desktop's platform.cpp is correctly asymmetric with ESP32's.** Plan-23 deliberately didn't split `platform_desktop.cpp` even though it has matching OTA / Improv / FS sections, because each is a 6-line stub. Per-subsystem stub files would be all overhead, no payoff. Lesson: symmetry across platforms is a heuristic, not a rule; the right axis is "does each file pay for itself."

- **Nightly builds belong in their own workflow, not the main release workflow.** Plan-22 added `.github/workflows/nightly.yml` that tags `nightly-YYYY-MM-DD` and lets the existing `release.yml` do the actual build via tag push. Zero duplication of the build matrix or Pages staging logic. The skip-on-no-change check (`should-tag=false` when `main` HEAD hasn't moved since the last nightly tag) means quiet days cost ~2s of API calls, not a full build. Lesson: a scheduled trigger that tags-and-reuses an existing trigger is cleaner than a parallel workflow that duplicates build steps.

- **`workflow_dispatch` reads the workflow YAML from the default branch, not the dispatched branch.** Cost us a CI cycle on RC2: dispatched against `plan-18` with a tag that wasn't valid against `main`'s older `release.yml`. The fix landed in the same branch (`3e2acb5` — branch allowlist + verify-version `--tag` arg), but the lesson is to read the GitHub Actions semantics for dispatch carefully: `inputs.tag` arrives correctly, but the workflow logic that consumes it is whatever main has at dispatch time.

- **The reviewer agent's job at PR-merge is architectural drift across N commits, not line-level bugs.** CodeRabbit catches line-level bugs in each PR commit; the reviewer agent at Event-2 reads the full branch diff and asks "did three commits each add a wrapper that one commit would hide?". Plan-18 ran into this productively — the in-branch CodeRabbit findings got triaged and either fixed or skipped-with-reason; the reviewer-agent run at merge is the final architectural sanity check. Two agents, two scopes.

- **A 13-commit branch is the upper end of what a single merge should carry.** Plans 18 + 19 + 19.1 + 20 + 20.1 + 21 (reverted) + 22 + 23 in one branch is a lot — the merge commit train is heavy and the reviewer-agent's job gets harder as more commits stack up. Future branches should aim for "ship the first 3-4 plans, merge, start the next branch" rather than "let the branch grow until everything's tidy." This branch worked because the plans were mostly independent (no two of them touched the same files in conflicting ways), but that won't always hold.

- **Asymmetric lifecycle propagation in MoonModule was historical, not principled.** Before this change, `setup` / `teardown` / `onBuildControls` / `onAllocateMemory` propagated to children via base defaults, but `loop` / `loop20ms` / `loop1s` defaulted to empty no-ops — every container that wanted to tick its children wrote the same 5-line per-child block (Layers, Drivers, would-be-NetworkModule-with-Improv). The fix was a one-line base-default change per callback into a shared `tickChildren` helper that gates by `!respectsEnabled() || enabled()` (children that opted out of the enabled gate keep ticking; the rest tick only when enabled) and accumulates per-child timing the same way Scheduler does for top-level modules. Leaf modules with `childCount_ == 0` pay one predicted-not-taken branch per call — sub-nanosecond. PC tick stayed in the same 55-160 µs band across scenarios; no measurable regression. Lesson: when one half of a lifecycle propagates and the other half doesn't, the asymmetry is usually historical (no one wrote the helper yet), not principled. With the helper in place, the parked Plan-21 (Improv-as-Network-child) move was a four-line follow-up: `addChild` instead of `addModule`, plus chain `NetworkModule::setup` / `loop1s` / `teardown` / `onBuildControls` to the base.

- **Override-and-chain convention: option A for loop, option B for setup.** When a container needs custom work alongside child dispatch, the convention is "parent prepares, children consume" for the loop callbacks — parent runs its work first, then chains to `MoonModule::loop()` so children read the freshly-prepared state (`Drivers::loop` runs `blendMap` before driver children read `outputBuffer_`). For `setup()` the convention is the opposite — chain to base first so children are initialised before the parent depends on them. `teardown` chains late (parent shuts down its own state, then the base reverse-iterates children). The conventions are explicit in `architecture.md § Lifecycle propagation to children`; deviations need a one-line comment at the override.

- **Control-change reactions are a three-tier split; the coarse-grained rebuild debt is closed.** The long-standing debt ("the build pass ran on all top-level modules for every control change") is resolved. `handleSetControl` now: (1) always calls the cheap `MoonModule::onUpdate(controlName)`; (2) calls `scheduler_->buildState()` only when `controlChangeTriggersBuildState(controlName)` returns true; (3) the pass reaches each module's `onBuildState()`. `controlChangeTriggersBuildState` defaults false and is overridden to true on `LayoutBase` and `ModifierBase` (every control they expose changes physical dims / LUT shape). Effect/driver value controls — including the new Drivers `brightness` — take tier 1 only, so slider drag no longer triggers a tree-wide realloc sweep. The model mirrors MoonLight's `onUpdate` / `requestMappings` (`hasOnLayout`/`hasModifier`) / `onSizeChanged` split — confirmed against MoonLight source before implementing. The verb is "build" not "rebuild" (idempotent, history-agnostic) and `onAllocateMemory` was renamed `onBuildState` for the same reason — boot and a later change are the same call. Lesson: when the spec already describes selective behaviour (architecture.md § Rebuild propagation said this for months) but the code does the coarse thing, the gap is usually a missing cheap hook, not a missing design.

- **Output correction (brightness/reorder/white) is a per-driver stage, shared via the Drivers container, not a WS2812-specific or effect-side concern.** Every physical driver needs to turn logical RGB into a physical signal; ArtNet was sending raw bytes (no brightness/order/white) — a gap, now fixed. The Drivers container owns a `Correction` (256-entry brightness LUT + channel-order table + derive-white flag) and hands each child a `const Correction*`; each physical driver applies it per-light into its own output buffer/packet. Preview is exempt (raw logical buffer). Brightness applied *before* white derivation (white = min of scaled channels). One LUT now; gamma/white-balance fold in later as a per-channel R/G/B split (the field is named `briLut` not `gammaLut` so that's a fill change, not a rename). Following MoonLight, which applies brightness/gamma/color-order at the driver edge via per-channel `redMap/greenMap/blueMap/whiteMap` LUTs + a lightPreset→offset table.

- **Per-`ControlType` behaviour belongs with the type, not with the caller.** Three call sites (`HttpServerModule::writeControls`, `FilesystemModule::writeValue`, `scenario_runner.cpp::writeJsonValAsValue` and twin apply paths) were each carrying their own `switch (c.type)` over the same enum. Each was 50-60 lines, hand-maintained in parallel, and silently drifted (the scenario runner had stopped recognising new types added for the FS path). The fix was to extract the switch into a small set of free functions in `src/core/Control.cpp` (`writeControlValue`, `writeControlMetadata`, `applyControlValue`, `isPersistable`, `hasDefault`, `controlTypeName`) and route the three call sites through them. Two cross-cutting requirements made this work: (1) `JsonSink` gained a third "fixed-buffer" mode (alongside socket and heap-grow) so the FS path could share the serializer without growing a per-value allocation, and (2) the apply path got an `ApplyPolicy` parameter (`Strict` for HTTP, `Clamp` for FS load) so the tolerant-load semantics survived the consolidation. Codified in `docs/coding-standards.md § Per-type behaviour lives with the type` — applies whenever the same `switch` appears in 2+ places. Counter-example also in that section: if only one caller needs the behaviour, keep it at the call site (a one-shot switch is cheaper than a function with one user).

- **Local Improv testing closes the dev-loop gap that made every Improv change a high-friction commit.** Before this branch, the only way to verify the Improv flow end-to-end was to tag a release, wait for CI, deploy to GitHub Pages, then flash from the live web installer — each iteration of the docs/install page or release-picker.js took minutes and burned a release tag. `scripts/run/preview_installer.py` now has a "flash-ready" mode that stages every local `build/esp32-*/projectMM.bin` under `releases/latest/` in the preview server's tree, generates matching Pages-relative manifests via the same `generate_manifest.py` production uses, and serves the install page at `localhost:8000`. The picker resolves the GitHub `latest` tag to the staged local bins via `toLocalUrl`, and Web Serial works on `localhost` without the secure-origin requirement that gates the public site — so an end-to-end flash + Improv WiFi provision is verifiable without touching any release. Paired with `scripts/build/improv_smoke_test.py` (probe + WiFi provision + LAN reachability, all three steps as a single CLI), which gives a deterministic pass/fail for the device-side Improv state machine and is wired into MoonDeck. Lesson: when a feature can only be tested in production, the iteration cost compounds into "don't touch it" — building the local test loop, even if it takes a day, pays for itself within a few commits.

## Lessons from this branch (Board injection follow-ups)

- **`src/ui/release-picker.js` is now `src/ui/install-picker.js` (exported symbol `installPicker`, embedded C array `installPickerJs`).** Renamed once the picker grew from "pick a release" to "pick release + board + firmware + install button" — the old name only described one of the three axes. The rename was a wide sweep (~20 files: source + CMake + release.yml + preview script + spec doc + every comment that mentioned the picker by name) but mechanically simple. Recorded here so a future search for `release-picker.js` lands on this entry and finds the new name; the file itself doesn't carry a "renamed from" comment because that would be the kind of past-tense history `CLAUDE.md § Principles` says belongs only in this folder.

- **Why the web installer dropped ESP Web Tools for a custom orchestrator.** ESP Web Tools 10.x's `<esp-web-install-button>` held the SerialPort exclusively across its flash + provision lifecycle and fired its `state-changed` event inside its dialog's shadow DOM (verified by reading `esp-web-tools/src/install-dialog.ts`). Two consequences: post-PROVISIONED board injection from the installer page was structurally impossible (we couldn't hook a side-call between provision and reboot), and `devices.js`'s "Your devices" auto-add silently broke because the URL was emitted behind a shadow-root event we couldn't subscribe to from outside. Owning the SerialPort end-to-end in `install-orchestrator.js` lets both fixes land in one place. The same dispatcher seeds future per-control injectables (device name override, MQTT broker URL, DMX universe) — each new field adds one vendor command ID + one dispatcher case. Lesson: a third-party install widget that owns the transport and emits lifecycle events behind a shadow root is a dead end for any flow that needs to do work after provision; if you need composability, own the transport.

## Adaptive memory allocation design (plan-07)

The core rules for how the light pipeline allocates and degrades under memory pressure. These are the invariants the code was designed around; the implementation lives in `Layer.h` and `DriverGroup.h`.

**Allocation rules:**
- **MappingLUT** is created only when ALL are true: modifiers exist on the layer; layout is not a simple non-serpentine grid (where physical == logical); enough heap available after reserving `HEAP_RESERVE` (32 KB) for stack/HTTP/WiFi.
- **Driver output buffer** is created only when: at least one layer has a LUT actually allocated (not just "has modifiers") and enough heap is available.
- Result for 1:1 unshuffled (no modifiers, or grid without serpentine): zero intermediate buffers — ArtNet reads directly from the layer buffer. Maximum LED count at minimum memory.

**Degradation cascade** — when memory is insufficient, degrade in this order:
1. Full pipeline — LUT + driver output buffer (modifier applied, clean separation)
2. Skip driver output buffer — LUT exists, DriverGroup does mapping inline (slower, sequential)
3. Skip LUT — modifier not applied, forced 1:1 mapping
4. Reduce layer dimensions — halve until buffer fits, minimum 8×8

Each degradation level is observable via flags on the module (`degraded()`, `lutSkipped()`, `outputBufferSkipped()`).

**Predict-measure-compare:** before each allocation, predict memory impact from grid dimensions + channelsPerLight + modifier presence; after allocation, compare heap delta. Variance > 5% signals a leak or accounting error. Buffer sizes: layer = W×H×D×cpl; LUT ≈ `MappingLUT::estimateBytes(logicalCount, maxDest)`; driver buffer = physicalCount×cpl.

## Plan-09 persistence failure — why JSON didn't pay for itself

Plan-09 attempted ~1700 LOC of JSON-based persistence that was fully abandoned. The five root causes are worth keeping because they recur in any persistence design:

1. **Question the format premise.** "Persistence is JSON" was assumed without justification. Neither human-readability nor manual editability were real requirements. For POD-only module state, `memcpy(file, this + sizeof(MoonModule), classSize - sizeof(MoonModule))` is one line and a complete save. Plan-10 took this path and succeeded.

2. **Suspicious helper proliferation signals over-elaborate design.** The plan spawned: `rebuildControls`, `clearControlsRecursive`, `LoadAllFn`, `setLoadAllHook`, `noteDirty`, `loadAll`, `loadTopLevel`, `applyNode`, `applyControls`, `serializeNode`, `serializeControls`, `buildTopLevelPath`, `cleanupTmpFiles_`, `cleanupTmpCb_`, `cleanupTmpLeafCb_`. That list is the system telling you the design is too elaborate for the job.

3. **Persistence forced a Scheduler reorder that bred secondary bugs.** Overlaying persisted values onto bound control variables grew the Scheduler from 3 phases to 5. This required `onBuildControls` to be idempotent, bred a duplicate-children bug, required SystemModule to guard MAC→deviceName derivation with a `deviceName_[0] == 0` check, and caused multiple "device shows nothing" hardware failures. The right approach: load BEFORE any module's setup or onBuildControls, by memcpy'ing into member memory directly.

4. **Defensive guards under memory pressure mask design bugs.** The plan added 5 null guards across BlendMap, DriverGroup, and Layer to handle failure modes that fragmentation produced. Each guard was locally correct; collectively they obscured the design problem (allocate-new-before-free fragmentation). Fix the invariant, not the call site.

5. **Test isolation reveals persistent-state contamination.** Live scenarios that mutated state (mirror toggles, grid size) contaminated each other across runs — failures appeared random until previous runs leaving state in `.config/` was identified. Any persistence layer's tests must reset state explicitly.

## Board-injection pipeline timing constraint

The web installer's `boards.json` catalog ships per-board control values that the orchestrator pushes to the device. SET_BOARD over Improv-Serial carries only the board name (one vendor RPC, one Text control on BoardModule); every other field in `controls.*` ships via HTTP after WiFi association.

This split works because every per-board control we ship today applies *post-association*: `Network.txPowerSetting` (the weak-power brown-out cap), and the future Ethernet pin maps, default-config overrides, etc. The radio briefly runs at the wrong setting for the ~1 s between association and HTTP fan-out completion — acceptable for power capping, would be unacceptable for:

- **Country code.** Governs which channels the radio scans; a wrong code at scan time picks wrong channels.
- **Antenna selector.** Wrong RF path at radio init makes the device deaf.
- **Pre-association TX-power.** Some chips need the power cap applied before the first probe request, not after association.

If we ever add such a control, **don't extend SET_BOARD's wire format to carry it** — that would couple unrelated controls to the board-name lifecycle and obscure the timing constraint. The two escape hatches are:

1. **Add a second vendor Improv RPC** (`SET_<control>` analogue of SET_BOARD) and dispatch it from the orchestrator BEFORE `SEND_WIFI_CREDENTIALS`. One RPC per pre-association control keeps the timing contract explicit.
2. **Bake the value into firmware** via a board-specific sdkconfig fragment. Works when the value is truly board-static (country code per region) and not user-configurable.

Pick (1) for user-configurable controls; (2) for truly fixed-per-board values. Avoid the implicit option C ("just push it earlier in SET_BOARD") — it tangles the lifecycle.

## Lessons from the ESP32-S3 N16R8 (DevKitC) enablement branch

Three non-obvious failures showed up while adding native-USB S3 support, all with the same diagnostic shape ("symptom looks like X, root cause is somewhere completely different"). Record them so a future S3 / native-USB addition doesn't repeat the dig.

1. **USB-Serial-JTAG ≠ UART0 on ESP32-S3.** The ESP32-S3 N16R8 Dev's USB-C port wires through the ESP32-S3's built-in USB-Serial-JTAG peripheral, NOT through an external USB-Serial bridge to UART0. ESP-IDF's secondary-console feature (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`, on by default for S3) mirrors stdio out BOTH paths, so the developer sees boot logs and assumes UART0 is wired and working. It isn't. Improv-listener-on-UART0 was deaf because the host was talking to USB-Serial-JTAG. **Fix:** install BOTH drivers and read from both transports — `#if SOC_USB_SERIAL_JTAG_SUPPORTED` keeps ESP32-classic builds free of the JTAG path. Don't make this compile-time-per-board ("the DevKit firmware") — the same binary should work on a board with either wiring.

2. **CORS preflight is silent on the client side.** Every cross-origin POST from a browser with `Content-Type: application/json` (everything the web installer does) triggers an OPTIONS preflight. If the device's HTTP server returns 405 to OPTIONS, the browser **silently drops the subsequent POST** — no error surfaces in client code, no network tab line, no console message in the install log. The symptom is "the API write didn't happen" with no diagnostic to follow. Burned an entire session diagnosing what looked like a board-injection-fan-out bug; the root cause was an unhandled HTTP verb. **Fix:** always implement OPTIONS in any device-side HTTP server that's reachable cross-origin. Return 204 with `Access-Control-Allow-Origin: *`, `Access-Control-Allow-Methods`, `Access-Control-Allow-Headers: Content-Type`. Verify with `curl -X OPTIONS -H "Origin: ..." -H "Access-Control-Request-Method: POST" -H "Access-Control-Request-Headers: content-type" http://device/api/control` — must return 204 with the headers, not 405.

3. **Cached "last applied" state goes stale when the underlying stack restarts.** The `txPowerSetting` cap used an `appliedTxPowerSetting_` field to skip redundant `esp_wifi_set_max_tx_power` calls. When ESP-IDF stops the WiFi stack (AP→STA cascade, STA reconnect, AP shutdown), it resets the radio's TX-power state — but our cached "applied" value stayed equal to the desired value, so `syncTxPower()`'s equality check short-circuited and the cap never re-landed on the restarted radio. The user saw "the cap doesn't actually work after the first power-cycle." Classic cache-invariant bug: the cache was correct vs the *driver state we set*, but stale vs the *underlying hardware state* that an external event reset. **Fix:** every callsite that calls `wifiStaStop()` / `wifiApStop()` also invalidates the cache (`appliedTxPowerSetting_ = -1`). Generalisable: any "skip if already applied" optimisation against a hardware peripheral needs an invalidation hook tied to every event that resets the peripheral, not just the obvious explicit ones.

## Core/light type boundary: light_types.h split + preview decouple

`src/core/types.h` had grown into a junk-drawer of light-domain types (`nrOfLightsType`, `CoordCallback`, `defaultGridSize`, `HEAP_RESERVE`, `lengthType`) alongside `Dim`. Split it so each symbol lives with its owner. Done in two passes:

**Pass 1 — the symbols with no core consumer:**
- **`nrOfLightsType`, `CoordCallback`, `defaultGridSize`** → `src/light/light_types.h`.
- **`HEAP_RESERVE`** → `platform.h` (it's a platform memory constraint guarding stack/HTTP/WiFi headroom — not a light type and not Layer's, even though Layer was its only caller; ownership follows concept, not call count).

**Pass 2 — `lengthType`, by removing its core consumers rather than declaring them load-bearing:**
The apparent blocker was "core uses `lengthType`." On inspection the uses were incidental: `Control.h` only *mentioned* it in a comment; `HttpServerModule`'s `put16` only took it because it serialised `PreviewFrame`. The real tie was `PreviewFrame` itself — a light-produced struct sitting in `src/core/` that core's `HttpServerModule` read to build the preview WS frame. We severed it properly:
- Introduced `BinaryBroadcaster` (core interface, ~6 lines): "send these bytes to all WS clients." `HttpServerModule` implements it via `broadcastBinary` — the old `broadcastPreviewFrame` body minus all preview specifics.
- `PreviewFrame.h` → `src/light/`. `PreviewDriver` now owns the 13-byte header packing and **pushes** the bytes to the broadcaster; the HTTP server no longer reads `PreviewFrame` or knows the format. Push replaced the old `PreviewFrame::ready` poll (flag deleted).
- `lengthType` → `light/light_types.h`, zero core users left.

**Result:** core has zero light dependency in the preview path; light owns the preview end to end; the binary-frame *transport* stays in core (reusable by any future binary feed — the leverage that justifies the new interface).

**Pass 3 — `Dim`, and the deletion of `core/types.h`.** `Dim` (the effect/modifier dimensionality enum) was the last light symbol in core. It looked load-bearing: `ModuleFactory::registerType<T>` probes a type for `dimensions()` to capture the UI's 📏/🟦/🧊 chip, and the probe named the enum — `requires { { t.dimensions() } -> std::same_as<Dim>; }`. But the very next line did `static_cast<uint8_t>(probe.dimensions())` — the factory only ever wanted a byte. The `Dim` name in the constraint was incidental, not essential. Loosened the probe to `requires { static_cast<uint8_t>(t.dimensions()); }` — detects the method and reduces it to a byte without naming the type. `Dim` then moved to `light/light_types.h`, and with it gone **`core/types.h` was empty and was deleted.** Verified the probe still captures correctly: `/api/types` reports dim 3 for NoiseEffect, 2 for CheckerboardEffect, 0 for GridLayout/ArtNetSend — unchanged. (Safe because only EffectBase/ModifierBase declare `dimensions()`; nothing else matches the loose constraint.)

**The rule that held:** don't accept "core uses X" at face value — check whether the use is *essential* or *incidental*. Every tie here turned out incidental and severable: a comment (`Control.h`), a serializer following a misplaced struct's field type (`PreviewFrame`/`put16`), and a SFINAE constraint that named a type it immediately discarded (`Dim`). The fixes: move the real owner and give core a domain-neutral seam — `BinaryBroadcaster` for the preview bytes, a return-type-agnostic probe for `dimensions()`. End state: **no `core/types.h`; core names zero light types.**

## Time-gated effects must still paint every frame (GameOfLifeEffect)

`GameOfLifeEffect` advances one generation per `bpm`-derived "beat", so most frames don't step the simulation. The first version skipped the render on non-step frames as an optimisation — and the display went black between beats, "a flash now and then". The cause: `Layer::loop()` calls `buffer_.clear()` **before every effect frame**, so an effect that doesn't write produces a black frame, not a held one. The render hot path has no frame-to-frame persistence; the buffer is the effect's to fill, every time.

**The rule:** separate *when the simulation advances* from *when the effect paints*. Time-gate the state update (the `dt*bpm` accumulator, same shape as `CheckerboardEffect`), but **always repaint the current state**. The sim runs at `bpm`; the paint runs at frame rate. This applies to any future effect whose internal clock is slower than the tick (a slow automaton, a beat-synced pattern). A `unit_GameOfLifeEffect` case ("renders every frame between generations") pins it.

**Two adjacent traps the same effect hit:**
- **First-frame `dt`.** `lastElapsed_` starts at 0, so the first `now - lastElapsed_` is the whole device uptime — a huge `dt` that pins the step accumulator above the beat threshold *permanently* (max rate forever, `bpm` ignored). Bootstrap `lastElapsed_` on the first call and take `dt = 0` that frame.
- **Width of the change delta.** The stagnation check narrowed `alive - lastAlive_` to `uint16_t`; at the 512×512 max grid on a PSRAM board (`nrOfLightsType == uint32_t`, 262144 cells) that truncates and triggers false re-seeds. Counters derived from cell counts must be `nrOfLightsType`, not a fixed width.

## uint16 intermediate overflow blanks the display — and a status check doesn't prove the render works (MultiplyModifier)

A high fan-out modifier (`MultiplyModifier` at 8×8×4) black-screened the no-PSRAM Olimex while the desktop showed it working. Root cause: `Layer::rebuildLUT` computed `maxDest = logicalCount * mod->maxMultiplier()` in `nrOfLightsType`. On no-PSRAM that's `uint16_t`, and `256 * 256 = 65536` **wraps to 0** — the LUT was sized to ~nothing, so almost every light mapped nowhere and the frame went black. On desktop (`nrOfLightsType == uint32_t`) the product fits, so the bug was invisible there. **Fix:** compute the product in `uint64_t`, clamp to the ceiling, then narrow back. This is the same family as the GameOfLife delta-width trap above, but for an *intermediate product*, not a stored counter — **any `nrOfLightsType * nrOfLightsType` (or `× a multiplier`) can overflow uint16 even when both operands are individually small; do the arithmetic in a wider type before narrowing.**

The harder lesson is about *verification*: the agent's hardware test asserted the Layer **status string** ("16×16×1") and called it a pass — but the status reflects `logicalDimensions`, which was correct; the *render* (driven by the corrupted LUT) was black. The product owner caught it by eye. **A status/dimension assertion is not proof the pipeline renders.** A correctness test for a mapping/effect must assert the **buffer or LUT is non-empty with the expected coverage** (the regression added here counts LUT destinations == physical light count), not just that the declared dimensions look right. And: a bug that depends on `nrOfLightsType` width is **invisible on the uint32 desktop build** — width-sensitive paths need either a uint16-typed unit test or hardware confirmation, not desktop-only.

## Lessons from the repo-transfer + v1.0.0 release branch

Moving the repo `ewowi/projectMM → MoonModules/projectMM` and cutting the first stable release surfaced three CI failures that had nothing to do with the transfer's code changes — they came from the *infrastructure around* the release (GitHub's hosted runners, GitHub Pages environment rules). Same diagnostic shape as the S3-DevKit branch: the red X appears on a release/build, but the cause is a platform behaviour we pinned against, not our diff. Record them so the next release doesn't re-dig.

1. **Don't pin GitHub-runner toolchain specifics — the `windows-latest` image migrates underneath you.** Mid-release, GitHub began redirecting `windows-latest` from the VS 2022 image to `windows-2025-vs2026` (the run's own annotation warned: "redirected to windows-2025-vs2026 by June 15, 2026"). Two breakages followed on the *same source tree* that had been green the day before: (a) `package_desktop.py` hard-coded `-G "Visual Studio 17 2022"`, and the new image has no VS 2022 → CMake failed at configure ("could not find any instance of Visual Studio"), a 21-second fast-fail before any compile; (b) once the generator pin was dropped (let CMake auto-detect the installed VS), the build reached compilation and the *new* VS 2026 MSVC STL emitted **C5285** ("specializing `std::tuple` is forbidden") on the vendored `doctest.h`'s tuple forward-declaration, and `/WX` made it fatal. **Fixes:** drop the generator pin (auto-detect survives image migration); add `/wd5285` to the existing MSVC suppression list (it's third-party header code we don't own; GCC/Clang never warn). **Generalisable:** anything pinned to a hosted-runner's bundled toolchain version (CMake generator, compiler path, SDK version) is a time-bomb — the image rotates on GitHub's schedule, not yours. Prefer auto-detection; when a pin is unavoidable, expect to update it and don't treat a sudden Windows-only failure on an unchanged tree as your regression.

2. **Don't gate asset-publishing behind a Pages-only `environment:` — a tag fails the gate before any step runs, silently dropping all release assets.** The `release` job did two things (upload release binaries *and* deploy GitHub Pages) under one `environment: github-pages`. That environment's protection rule allowed only `main`. When publishing v1.0.0 created the `v1.0.0` *tag*, it re-triggered the workflow on `refs/tags/v1.0.0`; GitHub evaluates the environment protection rule **at job start, before the first step** — the tag failed it, so the *entire job was rejected in 2 seconds*, including the "Publish GitHub release" step. Result: a published release with **zero binaries**, and a red X whose message ("Tag v1.0.0 is not allowed to deploy to github-pages") pointed at Pages, not at the asset upload that actually got skipped. The two symptoms (missing assets + red X) had one root cause: coupling. **Fix:** split into a `release` job (no environment, `contents: write`, uploads assets — runs on tags) and a `deploy-pages` job (`needs: [release]`, `if: ref==main`, carries the `github-pages` environment). Tags publish assets; `main` deploys Pages. **Generalisable:** an `environment:` on a job gates the *whole job* via a ref-based protection rule evaluated up front — never put work that must run on refs the environment forbids (tag-triggered asset upload) in the same job as the environment-gated work (production deploy). Recovery without re-tagging: `gh workflow run release.yml -f tag=vX.Y.Z` replays the fixed job and uploads assets onto the existing release.

3. **A failure that looks like the change is often the environment — verify before assuming a regression.** Recurring across this branch: a scenario "failed" its 120µs tick contract at 536µs (machine load from concurrent builds + MoonDeck + a preview server — re-ran isolated at 118µs, passed); Improv reported `UNABLE_TO_CONNECT` while the device was *already provisioned and reachable* (async-confirmation timeout, not a join failure); MoonDeck showed `0/0 online` while the device served HTTP 200 (the active network record had an empty subnet and a duplicate record held the device). None were code or transfer defects. **The rule:** when something fails right after a change, first prove the failure is *about* the change — re-run isolated, probe the actual end state (ping/curl the device, read the real CI error line, check the env), and only then edit code. Several hours here would have been saved by checking the device was reachable *before* debugging the "WiFi failure".

## Lessons from the LCD_CAM WS2812 driver bench debug (LcdLedDriver, S3)

Bringing the 8-lane LCD_CAM driver from "compiles and ticks" to "strip actually animates" took three stacked root causes, each masked by the one before it. The through-line: **every layer of indirection between "the code ran" and "the LED lit" hid a failure the layer above couldn't see.** Record the chain so the next parallel-output driver (16-lane LCD, P4 PARLIO, Teensy FlexIO) skips the dig.

1. **The i80 peripheral requires ALL `bus_width` data GPIOs — a partial bus never exists.** `esp_lcd_new_i80_bus` validates every `data_gpio_nums[0..bus_width)` and rejects `GPIO_NUM_NC` entries (`esp_lcd_panel_io_i80.c`, "configure GPIO failed"). A 1-pin config therefore never initialized: no bus, no transmit, dark strip — while the UI showed a configured, enabled driver. **Fixes:** the driver demands exactly 8 pins and reports `LCD bus needs exactly 8 pins` in the status slot (unused lanes take `0` in `ledsPerPin` and idle LOW); the loopback builds its private bus full-width from the driver's real pin set. **Generalisable:** when a peripheral claims a *group* of pins, surface the group contract in the control's validation — don't let a config that the hardware layer will reject look valid in the UI.

2. **"Capacity still fits" is not "config unchanged" — a resize-optimisation early-return swallowed pin changes.** `reinit()` skipped the bus rebuild when the existing DMA buffer was big enough — correct for grid resizes, wrong for pin edits: moving lane 0 from GPIO 13 to 18 keeps the frame size identical, so the bus kept clocking out on the OLD pins and the strip pin carried nothing. **Fix:** record the pin set (data + WR + DC) the live bus was built with and compare on every reinit; any difference forces the rebuild. **Generalisable:** an "is the existing resource still good?" fast path must compare *identity* (what the resource is bound to), not just *capacity* (how big it is). Same family as the S3-DevKit branch's stale-cache lesson: the cached check was true about the wrong invariant.

3. **Gate the LCD driver on `CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED`, NOT `CONFIG_SOC_LCD_I80_SUPPORTED` — the names look interchangeable and are not.** `lcdLanes` (and the whole driver wiring) first gated on `SOC_LCD_I80_SUPPORTED`, on the assumption it meant "has the S3-style LCD_CAM i80 bus." It doesn't: **the classic ESP32 also defines `SOC_LCD_I80_SUPPORTED=1`** for its unrelated *I2S-LCD* peripheral. So on the classic chip `lcdLanes` became 8, the `LcdLedDriver` got wired at boot, and `esp_lcd_new_i80_bus()` hung the watchdog trying to init an LCD_CAM bus the chip lacks → **boot-loop on every classic ESP32** shipped with that gate. The correct macro is **`SOC_LCDCAM_I80_LCD_SUPPORTED`**, defined only on chips with the actual LCD_CAM peripheral (S3 / P4) — which is what `esp_lcd`'s i80 driver requires. Bisect-proven (the prior commit booted, the LCD-driver commit looped) and hardware-verified after the fix (classic boots with LcdLed absent, S3/P4 keep it). The gate lives in `src/platform/esp32/platform_config.h` (`lcdLanes`) and the `#if` in `platform_esp32_lcd.cpp`. **Generalisable:** SOC capability macros with near-identical names can describe *different peripherals* on different chips — verify the macro is actually defined where you expect (and only there) before gating on it; a "supported" flag that's true on a chip you didn't mean is worse than a missing one, because it silently activates code on the wrong hardware.

4. **A self-test that passes while the device fails is telling you what the test doesn't cover — close the gap before theorising.** The first loopback transmitted a 3-byte synthetic pattern through a 136-byte transfer and PASSED while the real strip washed out max-white. The differences between test and reality were the suspect list: frame size (5.4 KB, multi-descriptor GDMA chain), sustained back-to-back cadence, and pulse timing as seen by a real WS2812 rather than an RMT RX capture. Upgrading the test to transmit the driver's REAL frame (same size, same chain, repeated like the render loop) and bit-verify the WHOLE capture (RMT RX with the DMA backend, ~1536 symbols) eliminated the first two suspects in one run — leaving timing as the only remaining difference, which was the answer. **Generalisable:** when test and reality disagree, enumerate what the test abstracts away and make the test transmit the genuine article; each closed gap either finds the bug or eliminates a theory with proof instead of speculation.

5. **Modern WS2812B reads a 412 ns "0" as "1" — T0H max is ~380 ns on newer revisions, and 3.3 V direct drive eats the remaining margin.** The classic 3-slots-at-2.4 MHz encoding (hpwit / FastLED lineage, slot ≈ 416 ns) produced a waveform the RMT RX decoded perfectly — and the strip rendered as max white with flicker: most "0" pulses sampled as "1" (mostly-ones ≈ white; the animation's actual 1-bits flicker through). The same strip on the same pin ran clean from the RMT driver's 350 ns zeros, which isolated timing as the only variable. **Fix:** pclk 2.67 MHz → 375 ns slots ("0" = 375 ns, "1" = 750 ns, bit 1125 ns), inside every WS2812B revision's window; latch pad resized to keep ≥300 µs. The lineage gets away with 416 ns because those rigs typically sit behind a 74HCT level shifter that restores threshold margin. **Generalisable:** datasheet timing windows shrank across WS2812B revisions — design new encoders against the NEWEST revision's T0H max (≤380 ns), and treat "RMT-captured waveform is correct but the strip disagrees" as a threshold/margin problem, not a logic problem.

Bench-procedure notes worth keeping: a board that drops STA mid-session falls back to softAP silently — poll the device's reachability before interpreting an unanswered API call as a wedge (one STA beacon-timeout drop was observed seconds after the LCD bus first went live; not reproduced since, watch for recurrence). And the differential test that cracked the case twice: drive the same pin/strip with the already-proven RMT driver — it exonerates wiring, power, and the strip in one move.

## Diagnosing LED flicker: eliminate firmware with hardware tests before blaming (or fixing) the wire

A classic ESP32 driving a WS2812 strip on RMT showed random wrong colours on LEDs the effect left black ("blue flicker", later "random flicker"). The temptation is to guess — WiFi interference, a buffering bug, timing — and start changing code. The bench session that resolved it instead ran a four-step elimination, each step a *measurement*, and the answer fell out:

1. **Capture the source/preview buffer** — it held zero stray colour. The effect output is clean, so the corruption is downstream of the logical buffer (not an effect or correction bug).
2. **Run the whole-frame loopback self-test** through a short jumper — bit-exact `PASS`, repeatedly, even under WiFi load. The RMT encode + transmit emit a correct WS2812 waveform on real silicon, so the firmware/peripheral is innocent.
3. **Sweep `txPowerSetting` 20 → 1 dBm** while watching the strip — the flicker was *constant*. A ~50× drop in radiated RF changed nothing, so it is **not** WiFi coupling into the data line (the standing hypothesis, disproven by the experiment).
4. **Check the pulse timing** — 350/700/1250 ns, spec-exact, and the loopback confirms the wire carries them. Not a timing-margin bug like the LCD T0H case.

With every firmware cause eliminated *by test*, the remaining cause is the physical data path — and "constant regardless of TX power" specifically fingers electrical signal integrity over radio. On a 3.3 V part driving WS2812 directly, the dominant cause is the missing 3.3 → 5 V level shift (the LED's logic-high threshold sits above what the GPIO drives, so marginal bits flip under any noise). Fix is hardware: level shifter, series resistor, shorter/grounded wire — documented in [RmtLedDriver.md § Troubleshooting](../moonmodules/light/drivers/RmtLedDriver.md).

**The transferable lesson** is the order: when hardware output looks wrong, prove the firmware is *not* the cause with a measurement at each layer (buffer → encode/transmit loopback → environment sweep) before either editing code or buying parts. Two strong hypotheses here (a buffering regression, WiFi interference) were both wrong, and only the measurements said so — guessing would have burned a level shifter's worth of time on the wrong layer, or "fixed" code that was never broken. The whole-frame loopback self-test exists precisely so step 2 is a one-click answer instead of a scope session.

**Red-herring note:** the flicker's *appearance* shifted (blue-only → random) after an unrelated change (the `lightPreset` default went RGB→GRB, remapping which channel carries which colour) plus a pin swap. The underlying electrical fault was identical; only the colour mapping over it changed. A changed symptom is not proof a code change caused it — confirm the mechanism, not the surface.

## ESP32-P4 support, round 1 — per-board Ethernet pin config, and the P4's WiFi reality

Adding the Waveshare ESP32-P4-NANO (round 1 of 4: board + Ethernet-only; later rounds add the Parlio LED driver, C6-co-processor WiFi, and the Parlio loopback). Two findings worth keeping:

**The P4 has no native WiFi.** `SOC_WIFI_SUPPORTED` is absent on esp32p4 (it has EMAC, RMT, LCD_CAM i80, and Parlio, but no radio). WiFi on these boards comes from an on-board **ESP32-C6 co-processor over SDIO** via the `esp_wifi_remote` / esp-hosted stack — which is a managed component, **not in mainline IDF v6.1-dev**. So round 1 ships Ethernet-only (`MM_ETH_ONLY`, WiFi components excluded), and round 3 will introduce a WiFi abstraction seam so the P4 routes to the remote stack while classic/S3 stay on native `esp_wifi`. The C6 SDIO pins on the P4-NANO (recorded for round 3, and so round-2 Parlio avoids them): CLK 18, CMD 19, D0-D3 14-17, C6 reset 54.

**Ethernet pins became a per-target compile-time config, not scattered #ifdefs.** `ethInit()` had the Olimex RMII/PHY pins baked in as literals. The P4-NANO needs different ones (IP101 PHY addr 1, MDC 31, MDIO 52, reset 51, and crucially an *external* 50 MHz RMII clock fed IN on GPIO50 — `EMAC_CLK_EXT_IN`, the opposite of Olimex's `EMAC_CLK_OUT`). Rather than `#ifdef` the pins inside `ethInit`, an `EthPinConfig` struct + a `constexpr ethPins = isEsp32P4 ? {…} : {…}` lives in `platform_config.h`, and `ethInit` reads it. This keeps the platform-boundary rule (compile-time branching is `if constexpr` on config flags, not `#ifdef` in domain code), turns the Olimex magic numbers into a named config, and is the seam future eth boards extend. Full *runtime* PHY/pin selection stays a 2.0 backlog item — this is compile-time-per-target, which is all the board variants need.

**The IP101 PHY driver is a managed component in IDF v6.** IDF v6 moved every per-PHY driver out of `esp_eth` core into the component registry (`espressif/ip101`). The generic PHY (Olimex LAN8720) stays in core, so only the P4 build pulls `espressif/ip101` (added to `idf_component.yml`); the IP101 ctor is behind `if constexpr (ethPins.isIp101)` so non-P4 builds never reference the symbol. A subtle build-script trap fixed alongside: the eth-fragment detector matched only `.eth`, so the new `sdkconfig.defaults.esp32p4-eth` (ending `-eth`) would have silently set `MM_NO_ETH` and stubbed Ethernet out — the matcher now accepts both `.eth` and `-eth`.

## ESP32-P4 round 2 — Parlio LED driver: a simpler peripheral, the same encoder

Adding `ParlioLedDriver` (the P4's parallel WS2812 path, round 2 of 4) turned out to be mostly *subtraction* from the LCD driver, not addition — worth recording why, so the next parallel-output backend (16-lane, Teensy FlexIO) starts from the right base.

**Parlio is a simpler peripheral than the LCD_CAM i80 bus, so the driver is simpler too:**
- **No sacrificial WR/DC pins.** The i80 bus mandates two real GPIOs (pixel-clock + data/command) that WS2812 ignores — `LcdLedDriver` carries `clockPin`/`dcPin` controls just to feed them. Parlio generates the clock internally (`output_clk_freq_hz`), so `ParlioLedDriver` has neither control.
- **No exactly-8-pins rule.** The i80 layer rejects a partial bus (`esp_lcd_new_i80_bus` requires a real GPIO on every data line), which is why `LcdLedDriver` demands exactly 8 pins. Parlio takes `data_gpio_nums[]` with unused = `-1`, so the driver runs on any 1–8 lanes. The "exactly 8" validation simply isn't there.

**The encoder is shared, not duplicated.** `LcdSlots.h::encodeWs2812LcdSlots` outputs "one bus byte per slot, bit L = data line L" — and a Parlio bus byte is byte-identical to an i80 bus byte. So `ParlioLedDriver` reuses the same encoder and the same `unit_LcdLedEncoder.cpp` test; no new encode code, no new encode test. (This is the "reuse a recognisable shape" rule paying off: a second parallel peripheral cost ~0 encoder lines.)

**One Parlio API constraint to remember:** `data_width` must be a *power of two* (`(w & (w-1)) == 0`), ≤ `SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH`. We always create the unit at `data_width = 8` (matching the encoder's 8-bit bus byte) and set unused lanes' GPIOs to `-1` — rather than narrowing the bus to the lane count, which would mismatch the encoder's byte layout. So 1–8 driver lanes all map onto an 8-wide Parlio bus; the unused high bits are simply not wired.

**Clock math is identical to the LCD driver:** Parlio's default `PLL_F160M` (160 MHz) ÷ 60 = 2.67 MHz = the same WS2812 slot rate (375 ns) the LCD driver settled on for the T0H-margin reason — so the timing decision carries straight across with no rework.

**Default pins must dodge the P4's strapping GPIOs.** The first cut of `ParlioLedDriver` defaulted `pins` to `36,37,38,...` — but **GPIO 34-38 are the ESP32-P4's strapping pins** (boot-mode control). It inited fine on the bench (nothing drove them during the boot window), but defaulting LED *output* onto strapping pins is a latent footgun: a driven level at the wrong moment can change boot mode. Fixed the default to 8 strapping-safe pins (`pins="20,21,22,23,24,25,26,27"`) with `ledsPerPin="64"` putting all 64 lights on lane 0 — a serpentine 8×8 panel is one 64-LED strand, and the other 7 lanes idle LOW at zero cost (the parallel DMA transfer time is set by the *longest* lane, not the lane count, so 1-of-8 is the same speed as 8-of-8; reassigning `ledsPerPin` adds strips later with no pin change). The clear GPIOs on the P4-NANO, after Ethernet (28-31, 49-52), C6 SDIO (14-19, 54), I2C (7-8) and **strapping (34-38)**, are 20-27, 32-33, 39-48. **Generalisable:** when picking default output pins for a new chip, pull the strapping-pin list from the datasheet first — "it booted on the bench" doesn't prove a strapping pin is safe to drive, only that nothing drove it at the wrong instant.

## Audio input (INMP441 mic) — ship the manual core, design fresh from the datasheet

The first audio-reactive capability: `AudioModule` (a SystemModule Peripheral) reads an INMP441 I²S mic and publishes an `AudioFrame` (level + 16-band spectrum + dominant peak) that `AudioVolumeEffect` and `AudioSpectrumEffect` consume. Decisions worth keeping:

**Two seams, everything else host-tested.** Only the I²S read and the FFT *kernel* sit behind the platform boundary (`platform_esp32_i2s.cpp`: IDF `i2s_std` + esp-dsp's float `dsps_fft2r_fc32`). All the signal math — DC strip, RMS, the Hann window, the magnitude→16-band log mapping — is pure header-only domain code (`AudioLevel.h`, `AudioBands.h`), the `RmtSymbol.h`/`LcdSlots.h` shape. The desktop `audioFft` stub is a real (naive O(n²)) DFT, so the *whole* pipeline (window → FFT → bands) runs end-to-end in CI on synthesized sines with no hardware. esp-dsp **float** (not fixed-point) is the right call on an FPU chip — Espressif's own benchmark fact.

**Effects reach the producer via a static accessor, not a boot setter.** The normal producer/consumer wiring passes a `const Foo*` to the consumer once in `main.cpp` (PreviewDriver→HttpServer). That works when both ends are boot-wired singletons — but an audio effect can be *added through the UI after boot*, and a boot setter only ever wired the boot instance. So `AudioModule::latestFrame()` is a static accessor: the active mic registers itself in `setup()`, clears the pointer in `teardown()`, and returns a static silent frame when there's no mic. Any add/remove order yields the live frame or valid silence, never null. Reach for this whenever a *user-addable* consumer needs a *singleton* producer's data.

**Found on hardware, pinned by tests.** Two bugs the desktop build couldn't surface, both now regression-tested: (1) a missing `registerType<AudioModule>` made `create("AudioModule")->markWiredByCode()` deref null and **boot-loop** — the fix is the registration plus a null-guard, pinned by a "registered + createable" test; (2) the I²S read **blocked the render tick** ~7.7 ms at a 20 ms timeout — fixed to a non-blocking read plus a cross-tick sample accumulator (a full 512-sample block takes ~23 ms at 22 kHz, longer than one tick), dropping AudioModule to ~400 µs. The INMP441 is also on the **left** I²S slot here (the right reads empty) — one config line, the first bench suspect when level floors with sound present.

**Shipped the manual core; the adaptive conditioner was prototyped and removed.** The mic, FFT, log/dB scale, two effects, and a `floor`/`gain` manual control surface are the solid, host-tested increment that landed. A self-calibrating conditioner (auto noise-floor + AGC + smoothing, goal "sound off → dark, sound on → vivid") was built and then *deleted* — it needs bench tuning in a **quiet** room, and the development environment (a campground van with strong, *varying* low-frequency engine/inverter rumble) was the adversarial worst case that kept it from settling: a per-band auto-floor removes *constant* tones but can't track *varying* broadband ambient; global AGC pumps the residual to full in silence; a relative per-band floor fixes treble over-cut but flattens the spectrum. Lesson — **land the manual core, treat adaptive auto-tuning as its own increment, and tune it where the noise floor is real-quiet, not in the field.** Also: `level` is overall RMS loudness (independent of the FFT) — don't derive it from the bands, or it stops fluctuating with volume.

**Designed fresh from the datasheet + textbook DSP, not from a prior project.** Per the product owner: don't trace WLED-MM (or any existing controller) for naming, structure, or functionality — build something independent. The concrete DSP choices and *why* (Hann/RMS/geometric-bands/argmax, and why a flat ±3 dB mic needs no per-frequency correction table) are documented in the module spec, [AudioModule.md](../moonmodules/core/AudioModule.md), where an integrator looks for them. The lesson worth keeping here is consistent with the repo's *Industry standards, our own code* principle (study with respect, don't copy): **reference proven behaviour, don't trace structure.** Here the datasheet made even the behaviour-reference unnecessary — a flat ±3 dB mic has no per-frequency error to correct, so the hand-tuned band-correction table years of prior-project work produced was the wrong tool, and the textbook defaults were enough. Read prior art to understand *what* works and *why*; let the hardware datasheet and standard DSP decide *how*, so the result is independent by construction rather than a renamed trace.

## ESP32-P4 round 3 — WiFi via the C6: the abstraction the earlier round feared wasn't needed

Round 1 recorded that round 3 "will introduce a WiFi abstraction seam so the P4 routes to the remote stack while classic/S3 stay on native `esp_wifi`." The actual implementation was far smaller, and *why* is the lesson.

**`esp_wifi_remote` is API-compatible, so there was no seam to build.** From the P4's side you still call `esp_wifi_init()`, `esp_wifi_connect()`, `esp_wifi_scan_start()`, `esp_netif_create_default_wifi_sta()` — identical signatures; the component forwards them to the C6 over SDIO. So the entire existing WiFi platform layer (`wifiStaInit`/`wifiApInit`/scan/tx-power, ~230 lines) compiles and runs **unchanged** on the P4. The only genuinely new code is a **two-call prelude** — `esp_hosted_init()` + `esp_hosted_connect_to_slave()` — that must run *before* `esp_wifi_init()`, added to `ensureWifiInit()` behind `if constexpr (platform::usesRemoteWifi)` (= `isEsp32P4 && hasWiFi`). Lesson: before designing an abstraction for "two backends," check whether the vendor already made them API-identical — here the "seam" was a 15-line prelude, not a routing layer. *Concrete first* would have caught this even without the foresight; the round-1 note over-scoped from a position of less information.

**Init ordering is the whole risk.** Espressif's docs and the community starters are emphatic: esp_hosted must be fully up before anything touches the WiFi stack, and wrong ordering surfaces as **NVS errors / asserts / a silent hang**, not a clean error — so it reads like an unrelated bug. The prelude-before-`esp_wifi_init` placement is the mitigation; it's the first thing to check if a P4-wifi build misbehaves at boot.

**Pulled P4-only via a `rules` gate.** `esp_wifi_remote` + `esp_hosted` are added to `idf_component.yml` with `rules: - if: "target == esp32p4"`, so classic/S3 and the eth-only P4 build never fetch or compile them. This is the clean answer to "managed components are per-project" — the gate makes the pull per-target. (The pre-existing `ip101` entry could use the same gate but doesn't; not worth churning.)

**A deliberate v6.0-floor exception.** These components live outside mainline v6.0, so the build steps below the [v6.0 floor](../building.md#esp-idf-version). That floor has an explicit-exception clause exactly for cases like this: the product owner accepted it consciously, it's documented at every introduction site (the yml, the sdkconfig fragment, the platform flag, building.md), and it's scoped to the P4 — the other targets keep the v6.0 fallback intact. Lesson: a floor rule with a *documented-exception* path beats a rigid one; the exception stays honest because it's recorded, not silent.

## RMT timeout: don't cancel a stuck transfer — the cancel crashes classic ESP32

A deferred "fuller error handling" item for `rmtWs2812Show`/`rmtWs2812Wait` (🐇 CodeRabbit PR#17) turned out to be mostly *already done* and partly *actively harmful* — a good case of *default to subtraction*.

**Most of it had already landed** in the multi-pin (2a) work: `rmtWs2812Transmit` already returns the `rmt_transmit` result, and `RmtLedDriver::loop()` already tracks `started[]` so it only waits on channels whose transmit succeeded (a failed one gives no done-callback, so waiting would burn the full 1 s timeout). And there's no mid-transmit corruption risk because the symbol buffer is re-encoded from scratch *before* any transmit each tick. So two of the three "to-do" items were no-ops.

**The remaining item — cancel the in-flight transfer on timeout via `rmt_disable()` — is a trap.** `rmt_disable()` while a transmission is still active triggers an **interrupt-WDT panic on classic ESP32** (espressif/esp-idf#17692; classic-only, S3/C6/P4 unaffected). That trades a self-healing dropped frame for a crash on a shipping target — strictly worse, and a direct *"crashed is not acceptable"* violation. So the right change was to **not** add the cancel, and instead replace the vague "deferred" comment with a sourced explanation of why we deliberately leave a timed-out transfer alone (it self-heals: next tick re-encodes and re-transmits; a still-busy channel just fails its `rmt_transmit` cleanly, and `started[]` skips the wait). Lesson: a deferred-improvement note is a hypothesis, not a spec — verify the improvement is real *and* safe on every target before implementing it; sometimes the finished work is "document why the current code is already right."

## Pin defaults: assign one only when it cannot do harm

A mic-less **classic ESP32** boot-looped (TG1WDT_SYS_RESET at ~736 ms, no panic backtrace — a silent hang). Bisect: clean-built the known-good commit (still looped → recent driver work innocent), then disabled the AudioModule wiring in `main.cpp` → booted clean → **AudioModule was the cause.** Root cause: AudioModule was **auto-wired** (`addChild` + `markWiredByCode()` in `main.cpp`, gated on `platform::hasI2sMic`), so on every boot it ran `setup()` → `reinit()` → `platform::audioMicInit()` → the IDF `i2s_channel_enable()`, which on the classic's older I²S driver **blocks forever** when no mic is clocking the pins. The watchdog fired on the stuck init. (The P4 was never affected: its newer I²S either returns a silent frame or fails cleanly without blocking — same mic-less condition, different driver behaviour. Two independent code paths, one symptom only on classic.)

**The fix, per the product owner, was a design fix not a band-aid:** (1) **don't auto-wire AudioModule** — register it in the factory (`registerType<AudioModule>`) so it's user-addable like an effect, but only when the user with a mic adds it; (2) **default the mic pins to unset (0)**, not to bench values; (3) `reinit()` **no-ops on any unset pin** (`if (wsPin==0||sdPin==0||sckPin==0) { setStatus("set …"); return; }`) so even an added-but-unconfigured module never touches I²S. Classic then boots 191 FPS, 0 WDT resets, and an added mic works once its real GPIOs are entered.

**The generalisable rule the product owner drew from it: _assign a pin default only when it cannot do harm._** The test is *who fixes the pin*:
- **Chip-/board-fixed pins → default them** (and you *must*): the RMII **Ethernet** pin map is silicon-/PCB-wired, so a default cannot do harm — and *omitting* it does, because a no-WiFi board with un-defaulted Ethernet pins can never connect to be configured (a chicken-and-egg lockout). This is why `platform::ethPins` is a compile-time-per-target constant, never a user-blank control — and it stays that way.
- **User-soldered pins → leave them empty**: a MEMS mic or an LED strand goes wherever the user ran the wire, so any default is a guess that can drive a pin the user committed to something else. Empty until set; idle with a "set pins" status meanwhile (the robustness rule: degraded is fine, crashed is not).

The LED drivers (Rmt/Lcd/Parlio) are the same Device-level case — user-soldered, so their pin defaults follow the same rule. This rule is the runtime face of the three-level **MCU → Board → Device** config-provenance model ([architecture.md § Config provenance](../architecture.md#config-provenance-mcu--board--device)): a pin may be defaulted only at the level that actually fixes it, and the empty Device-level defaults are the correct baseline a saved device profile later *fills* rather than *overrides*. Lesson: a hard-coded pin default is a claim about the user's hardware — make that claim only when the hardware, not the user's soldering iron, decides the pin; and never auto-run a peripheral whose init can block on absent hardware.

## Live reconfiguration falls out of the prepare-pass for free — MoonLight's "initless" goal, a different mechanism

projectMM has a property most LED-controller firmware lacks: **every module reconfigures live the instant a control changes — pins, leds-per-pin, output protocol, mic pin/rate — with no reboot, immediately reactive on the next render tick.** The design note for *why* this exists lives in [architecture.md § Live reconfiguration](../architecture.md#live-reconfiguration-every-change-applies-without-a-reboot); the lineage and the *how-it-differs* are the lesson worth keeping here.

**The lineage is MoonLight's "initless drivers."** The product owner's earlier project ([MoonLight nodes.md § Initless drivers](https://github.com/ewowi/MoonLight/blob/main/docs/develop/nodes.md)) set the same no-reboot goal at the LED-driver level, named *initless*: a driver with **no `addLeds` (FastLED) / `initLed` (Parallel LED Driver) step** — it reads a mutable Context at `show()` time, so pin allocation, leds-per-pin, RGB/RGBW and light type all change live without a restart or recompile.

**projectMM reaches the same outcome by a different mechanism, so the word doesn't transfer.** Our drivers *do* have an explicit rebuild — `RmtLedDriver::reinit()` re-creates the RMT channels, the i80/Parlio drivers rebuild the DMA bus — so they are not "initless" in MoonLight's no-`initLed` sense. What makes the behaviour universal here is that the rebuild is driven by the **generic tier-3 `onBuildState()` sweep** ([§ Event triggering](../architecture.md#event-triggering-between-modules)), not hand-built per driver: any module that returns `true` from `controlChangeTriggersBuildState` inherits live-reconfig for free, which is why it spans drivers, the audio peripheral, effects, layouts, modifiers and network I/O alike. Lesson: credit the lineage for the *idea* (MoonLight's initless drivers), but name the property by what the user sees (*live, no-reboot reconfiguration*) when the mechanism differs — overloading a prior project's term onto a different implementation misleads. And: a generic prepare-pass buys breadth a per-driver technique can't — the same three tiers that rebuild a mapping LUT also re-target a GPIO, so the property generalised itself.

## Lessons from the catalog-driven installer branch (3-layer device model)

The installer was reworked so a board catalog ([`boards.json`](../install/boards.json)) sets a device up for its hardware at install time. The mechanics and schema live in the [installer README](../install/README.md); these are the hard-won principles worth keeping.

**Inject from data, don't bury in code (vs MoonLight's `ModuleIO.h`).** MoonLight hardcoded ~20 boards' pin presets in firmware C++ (`setBoardPresetDefaults()`, behind `#ifdef CONFIG_IDF_TARGET_*`); adding a board is a recompile, and every binary ships every board's table. projectMM instead **injects** the same information from the catalog after flash — the firmware is a generic engine that knows nothing about QuinLED/Serg/Olimex, and the *data* specialises it. Adding a board is a JSON edit, binaries carry no board tables, and board definitions become community-contributable data. This is the *domain-neutral core* principle: the specialisation is data, not code.

**Investigate before building — the device side was already done.** The original plan was a new `POST /api/preset` batch endpoint with a device-side consumer. Investigation overturned it: three install clients already fan a catalog's controls out as `POST /api/control` calls. The *real* gap was that the fan-out can't configure a module that doesn't exist on a fresh flash (a control write 404s) — solved by the **already-existing, idempotent `POST /api/modules`** the clients simply weren't driving. Lesson: the reflex to add an endpoint hid that the mechanism existed; a day of mapping the existing code replaced a new core endpoint (and a forbidden JSON-array parser) with a small client change. *Default to subtraction.*

**`loopbackTxPin` — verify the claim against the code before deciding twice.** A driver loopback self-test transmits on `pins[0]`, and the bench's loopback TX jumper is a *different* pin from the operational LED pins — so the test forced retyping `pins`. A `loopbackTxPin` override control was proposed, then **dropped** on the reasoning "it only fits RMT's single pin, not Parlio/Lcd's lane array," then **re-added** after reading the code: the Parlio/Lcd loopback only drives **lane 0** with the test pattern, so a single TX override substituting for lane 0 works uniformly on all three drivers. Lesson: the "doesn't fit lane arrays" objection was an assumption about the loopback, not a fact — checking `ParallelLedDriver::runLoopbackSelfTest` (lane-0-only) settled it. Verify the mechanism before a design call that rests on how it behaves.

**Board vs Device is a completeness spectrum, not two schemas.** The carrier/shield pattern (a PCB an ESP32 *module* plugs into) is a **Board** — it fixes the LED/relay pins, the MCU and strips are chosen separately. A vendor-finished all-in-one with peripherals soldered (QuinLED Dig-2-Go, Dig-Next-2 with built-in mics) is a **Device**. But they are the *same* catalog entry shape — a "Device" is just a Board entry with more of its optional `modules`/`controls` filled in (verified: every `boards.json` entry shares one schema). So **no *separate-schema* `devices.json`**: one entry type, not two. (This rejects a *second schema*, not a future *same-schema* grouping — architecture.md leaves the door open to splitting the flat list into a `devices.json` / `kind:` tag purely for organisation once it's large enough to be unwieldy; that's a file-layout choice under the sequencing rule, not a second entry type.) A board's pins fall into three categories, not two: *always-fixed* (LED outputs, status LED — default freely), *board-optional* (a populated-or-not W5500/IR/power-monitor — an opt-in peripheral block, can't default blind because the same board name ships with and without), and *user-soldered* (always unset). The optional-peripheral case (e.g. SE 16 with a W5500) is the runtime SPI-PHY mechanism, not a new one.

**Per-board capability spec'n'test loop.** Each board's pin-layout `image` and product-page `url` are the *inputs* to a repeatable build loop, not just decoration: (1) read the board's capabilities off the image + link (LEDs, mic, IR, power monitor, …); (2) for a capability we already offer (an I²S mic → `AudioModule`), wire it into the entry's `modules`/`controls` with real pins; (3) for one we don't offer yet (IR receive), write a proposal against the architecture, spec it, **create test scripts** (host unit + scenario, hardware loopback where applicable), iterate until it works, *then* add it; (4) the entry grows only as far as each capability is spec'n'tested — an un-implemented capability is a recorded proposal, not a half-wired control. A fully-implemented board entry is the *output* of running this loop; the image/link tell you what to aim for. This is *Specs before code* at board granularity.

**Drivers became catalog-added, with an OTA nuance.** LED/network drivers stopped being boot-wired (only `Preview` stays, since it needs the HTTP broadcaster the catalog can't supply); each board declares its driver(s) in `modules`. A fresh-erased board boots with `Drivers = [Preview]` only — the deliberate explicit-add model. **The nuance hardware surfaced:** an OTA update *without* erase keeps a device's previously-persisted drivers (they're saved config the new firmware reloads as user-added children), so the clean Preview-only state applies only to a fresh/erased flash. That's correct — an update shouldn't wipe a user's configured drivers — but it means "out-of-box" and "after-update" differ, which isn't obvious until you flash a non-erased board.

**A persistence overlay must distinguish "key absent" from "value 0".** The runtime-Ethernet-PHY work moved pin/PHY config from a compile-time `constexpr ethPins` into persisted NetworkModule controls (`ethType`, pin GPIOs, …) with **non-zero per-chip defaults** (P4 IP101 = `ethType` 2). That exposed a latent bug in `applyControlValue` (the persistence load path): it used `json::parseInt(json,key)`, which returns 0 for an *absent* key — indistinguishable from a real 0 — and then wrote that 0 into the control under the Clamp policy. So loading an older/partial `<Module>.json` that omitted a key **clobbered the control's default with 0**. On the ESP32-P4 this zeroed `ethType` (2 → 0 = none), so `ethInit()` dispatched to "no Ethernet": link LEDs on, but no DHCP. It was invisible on classic/Olimex (their eth defaults are mostly 0 anyway) and on `main` (which still read the `constexpr ethPins` directly), so it only bit once eth config became persisted controls with meaningful non-zero defaults. **Fix:** a `json::hasKey()` guard in `applyControlValue` — an absent key leaves the control untouched (preserves its default); a present key (even value 0) still applies. Lesson: any "control resets to its default/0 after reboot" symptom is a persistence-overlay smell, not a control-init bug; a flat JSON parser that returns a zero sentinel for missing keys MUST be paired with a presence check before the value is applied as authoritative. The decisive debugging move was a `std::printf` of the runtime struct over the P4's *secondary* USB-Serial-JTAG console (stdout reaches USB even when ESP_LOG/UART is on GPIO 37/38), after a `git worktree` bisect (round-1 ✓, main ✓, uncommitted ✗) proved it was our code, not hardware or IDF.

**A GPIO pin is its own control type (`ControlType::Pin`), not an overloaded int16.** Pins were first added as `addInt16` with a `-1..48` range, which the UI rendered as a *slider* — meaningless for a GPIO, and the cap wrongly excluded the P4's high pins (MDIO 52, clk 50). Dropping the range didn't help: the UI's `int16` case *always* draws a slider (an unbounded int16 falls back to a −100..200 percentage slider that position/region controls rely on), so int16 couldn't be made to mean both "position slider" and "pin number." The fix is a dedicated `Pin` type: `int8_t` storage (one byte — a GPIO never exceeds ~54, and on a DRAM-scarce ESP32 the per-pin byte matters across many pin controls), −1 = unused, the UI always renders a plain number input keyed off the `"pin"` type string, and min/max are a server-side write-clamp guard only. Serializes/parses as a plain integer (same as int16). This also serves every future pin control (LED-driver clockPin/dcPin, GyroDriver SDA/SCL, board pins) — they migrate to `addPin` for free. Lesson: when one control type is doing two jobs with different UX (slider vs number), that's the smell for a new type, not a range hack; and pick the smallest storage that fits the domain (int8 for a pin).

**`deviceName` (identity) vs `deviceModel` (product) vs board (bare PCB) — one term was doing three jobs.** "Board" had been overloaded to mean the per-unit network identity, the hardware product/catalog key, AND the bare PCB. Untangling it: `deviceName` is the **per-unit identity** — one string that drives mDNS (`<deviceName>.local`), the SoftAP name, and the DHCP hostname, so the device shows up under one name everywhere; it's RFC-1123-coerced (`sanitizeHostname`) because it becomes a hostname. `deviceModel` is the **hardware product** (the `deviceModels.json` catalog key, e.g. "projectMM testbench S3") — display-form, spaces allowed, never a hostname. "Device" is the umbrella noun; "board" now means **only the bare PCB**. This drove the BoardModule→SystemModule fold (the identity is core unit state, not a separate module), the `board`→`deviceModel` rename across catalog/installer/Improv (SET_BOARD→SET_DEVICE_MODEL, byte 0xFE unchanged), and the eth pin-map clarification (driver = firmware, pin map = firmware-seeded but **deviceModel-authoritative** so an Olimex entry can override). Lesson: when one noun answers three different questions ("what do I call this unit on the network?", "what product is it?", "what's the bare board?"), that's a naming smell — split it into the qualified terms, pick one umbrella word, and make the split visible in every layer (control names, RPC symbols, catalog keys, docs) so the three concepts can't re-merge.

**"Improv = REST over serial" — one apply-core, two transports, and the testability that follows from extracting the hard part.** The deployed HTTPS installer couldn't configure a flashed device: a browser blocks an HTTPS page from POSTing to an `http://` device (mixed-content), and the `?deviceModel=` pull/handoff that replaced it only ran if the user opened that exact link. The fix reframed the problem — the installer already owns the USB serial port during provisioning, so push the config over it as the *same REST operations the HTTP API runs*: a new `APPLY_OP` (0xFC) Improv vendor RPC whose payload is `{"op":"add|set|clearChildren",…}`, the same JSON a `POST /api/modules`/`/api/control` body carries. On the device the op routes to **one transport-free apply-core** (`HttpServerModule::applyAddModule/applySetControl/applyClearChildren/applyOp`) the HTTP handlers also call, so a network REST call and a serial APPLY_OP execute identical code; the handlers became thin `switch(applyX())` → status-code mappers. This **deleted** the whole browser handoff (device-side catalog fetch, `?deviceModel=` decoration, the inject button) — a net subtraction — and works on Ethernet-only firmware once the Improv listener is decoupled from WiFi (the vendor RPCs compile in unconditionally; only `WIFI_SETTINGS`/`GET_WIFI_NETWORKS` stay `#ifndef MM_NO_WIFI`). Lesson 1: when a push is blocked by the *medium* (mixed-content on HTTPS), look for a medium you already control (the serial port mid-flash) instead of bolting on a fragile pull. Lesson 2 (the one with legs): the way to make it *provable* was to **extract the hard part into a pure core primitive** — the chunk reassembly + out-of-order/duplicate sequence guard moved from the ESP32-only handler into `src/core/ImprovOpReassembler.h` (header-only state machine, returns `Continue/Ready/Error`), and the JS frame builders into `docs/install/improv-frame.js` so `node:test` imports them without the orchestrator's browser deps. Both are *Complexity lives in core; domain modules stay simple* applied for testability: the device handler keeps only its serial I/O, the algorithm gets unit-tested on the desktop, and a format implemented three times (device C++, Python, JS) is pinned by **one shared golden vector** asserted in `test/python` + `test/js` — a contract test is the right answer to *forced* duplication no shared compilation target can remove. The reflex worth keeping: a hard mechanism buried in a platform `.cpp` that "can only be tested on hardware" is a smell — extract its pure core, and "rock solid proven" becomes a unit test instead of a bench session.

**A periodic re-broadcast to let late joiners "catch up" is a hack wearing a keepalive costume.** The 3D preview sends a coordinate table (positions) once, then per-frame colour. The original implementation re-sent the *whole table every ~1 second* "so a client that connected after the last rebuild catches it." It looked fine — a fresh page recovered within a second — so it shipped and sat there. But it's a workaround, not the mechanism: it rebuilt the full table from the layout **every tick-second forever**, on the hot path, whether or not anyone had connected and whether or not anything changed — and it papered over a missing request/response with polling. The correct construct is event-driven: send the table **when it actually changes** (`onBuildState` — grid/layout/LUT rebuild) and **when a client asks** (a new WS connection bumps `BinaryBroadcaster::clientGeneration()`, which `PreviewDriver::loop()` watches and re-sends on change). That's strictly *less* code than the timer and zero idle cost. How it sneaked past review: the workaround *worked* in casual testing and its cost was invisible until a later change made each rebuild heavier and the per-module tick was profiled. Lessons: (1) "re-send periodically so it eventually syncs" is the polling-instead-of-events smell — ask "what's the event that should trigger this?" and trigger on *that*; (2) a recurring rebuild on the hot path must justify itself every tick, so "every second, just in case" fails *Data over objects / fastest hot path* on sight; (3) this is *Continuous refactor, no hacks* — the fix isn't a scheduled cleanup, it's "the moment you see a keepalive timer standing in for a request, replace it." The guard is a test that advances the clock several seconds with no client change and asserts the table is **not** re-sent (the old timer would have).

**When a working seam regresses after your "fix," suspect the fix — and measure with a tool faithful to what the user sees.** The resumable preview send (`sendBufferedFrame`/`drainPreviewSend` — stream the producer buffer a memory-adaptive chunk per `loop20ms`, drop-new backpressure, downsample + display cap) shipped working on all three boards. A later attempt to *also* route the coordinate table + downsampled colour frame through the resumable path (removing the synchronous `sendAllOrClose` spin-and-close) looked principled but **regressed every board into an intermittent stall**, through several variants. Three lessons compounded: (1) **Stop at the first failed fix on a working path.** Each "improvement" to a seam that already worked added a new failure; the discipline (CLAUDE.md *Anti-stalling*) is to revert to the known-good state at attempt two, not re-engineer. The committed synchronous coord/downsampled path *closes a wedged client past a spin budget and lets it reconnect* — not elegant, but proven; the elegant unification wasn't worth a regression nobody could pin. (2) **A measurement tool must be faithful to the real client or it invents and hides bugs.** A plain one-shot WebSocket probe *gave up on close* where a browser *reconnects*, so it reported stalls users never saw AND missed blips they did — it sent the debugging in circles for a whole session. The fix was a browser-faithful probe (`scripts/diag/preview_health.py`: reads binary frames, sends the 25 s keepalive ping, auto-reconnects with backoff — exactly `app.js`'s `connectWs`); only then did probe and eyes agree. (3) **A stale process masquerades as a code bug.** "No preview on the PC build" with a corrupt-looking on-the-wire coordinate count survived every fresh rebuild — because a **38-hour-old desktop binary** still held port 8080; the freshly-built one couldn't bind and the browser/probe hit the stale one. The tell was the *uptime*, spotted by the product owner, not the diff. Lesson: before bisecting a "rebuild didn't fix it" bug, confirm the artifact under test is the one you built (check the process uptime / `build` timestamp / what's actually bound to the port). The faithful probe is now the standing way to measure preview health on any target (PC + the three boards), wired into MoonDeck's Live tab.

**Don't hold a vendor library's async handle across your own event loop — it races the library's internal timers.** A UI refresh intermittently crashed the device (`assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))` — a null FreeRTOS queue — inside the espressif mDNS component's `mdns_query_async_get_results`, plus an `Interrupt wdt timeout`). The mDNS *browse* (discovering peers for the "Your devices" list — distinct from mDNS *advertise*, which serves `<deviceName>.local` and was never the problem) used the async API: `mdns_query_async_new` returns a handle that `DevicesModule` held across ticks, polling it each `loop1s` with a 0 ms timeout. The trap: the mDNS component's **own task** owns that handle's queue and **frees it when the query's window (3 s) expires** — so a poll landing in the gap after expiry asserts on a freed queue. It was intermittent and grid-size-sensitive (a bigger grid lengthens the tick, widening the gap) and looked like "refresh crashes it" only because a refresh's activity coincided with the poll. **First fix attempt was wrong:** I assumed a *service-table mutation* (live rename re-registering `_http._tcp`) tore the handle down and added a cancel-before-mutate guard — it didn't fix it, because the freeing party is the component's expiry timer, not our code. **Real fix:** stop using the async-handle API entirely — replace the start/poll/stop trio with one synchronous `mdnsBrowse()` (`mdns_query_ptr`) that queries, delivers results, and frees everything in a single call, holding **no handle across ticks**, so the race window can't exist. The catch that synchronous introduced: `mdns_query_ptr` blocks the *full* timeout (it waits the whole window for late responders, no early return) and `loop1s` is charged to the tick — an 80 ms query tanked the tick. So **throttle**: browse one service type every ~8th tick with a ~60 ms timeout — one brief hiccup every ~8 s, invisible for discovery, FPS untouched in between. Lessons: (1) a library's async/iterator handle is only valid between *its* lifecycle events — if you can't see/where those fire (here, an internal expiry timer on another task), don't hold the handle across your loop; prefer a self-contained synchronous call that owns the whole lifecycle. (2) An *intermittent, load-dependent* crash whose backtrace sits in a vendor component is a **lifecycle race**, not a component bug — but find the *actual* concurrent actor before "fixing" (my first guess at the actor was wrong and the fix did nothing). (3) Trading async for synchronous trades a race for a blocking cost — budget it (throttle + bound the timeout) so the cure isn't a tick-killer. (4) Desktop stubs these mDNS calls to no-ops, so it's a hardware-only fix the unit suite can't reach; the reproduction (concurrent WS churn at a large grid → crash before, stable after, uptime climbing) is the proof, in the commit, not a desktop test.

**A dead control that was always meant to be functional belongs in the mechanism that already expresses it, not where it happens to sit.** Six persisted-but-ignored Layer controls (percent region carving) were due to be wired into `rebuildLUT`; the product owner's question — *can a modifier already do this?* — was the better path. `ModifierBase`'s two virtuals (`logicalDimensions`, `mapToPhysical`) express carving exactly, so it shipped as a modifier and the Layer controls were deleted. Lessons: (1) before wiring an inert control where it lives, check whether an existing mechanism already covers it. (2) "make the default fastest" is best met by making the default the *absence* of the feature — full coverage = no modifier = the existing fast path, zero added cost — not a guarded branch through it.

One sub-decision the implementation forced: the boundary rounding. The original spec said inclusive-ceil ("start 33/end 66 on a 4-wide axis → pixels 1..3"), which on a 128-wide axis makes `end=50` land on pixel 64 *inclusive* — so two abutting layers (0..50, 50..100) **overlap by one pixel** at the seam. The product owner chose **half-open `[start, end)`** instead: `end=50` → pixels 0..63, and 0..50 + 50..100 tile a 128 axis into 0..63 / 64..127 exactly, no overlap, no gap (with a min-1-pixel floor so tiny panels still get a non-zero region). Lesson: when a region/range feature will be used to *tile* a space, half-open intervals are the textbook choice (same reason `[begin, end)` is the C++ iterator convention) — inclusive bounds double-count the seam.

Lessons: (1) a persisted-but-inert control is a feature with no home yet — before wiring it where it sits, ask whether an existing mechanism already expresses it (the modifier interface did, completely). (2) "make it the fastest at the default" is often best met by making the default the *absence* of the feature, not a fast branch inside it. (3) a feature framed as "a Layer property" may really be "a composable transform" — the modifier framing also unlocked stacking for free. (4) reach for half-open intervals whenever regions abut.

## Composable modifiers — invert the map build (physical→logical), don't bolt fan-out onto the old interface

Modifiers needed to chain (Region then Multiply then Rotate), but the old interface — `mapToPhysical(logicalCoord) → [physical indices]`, a virtual→physical **fan-out** — didn't compose: stages emitted flat indices, not coordinates, and chaining would need a product-of-`maxMultiplier` fan-out ceiling (the exact overflow class that caused the multiplyZ black-screen). The fix was to **invert the build to physical→logical**, adopting MoonLight's proven model (the product owner's prior engine) in projectMM's own code: each modifier becomes an in-place coordinate fold, and the Layer walks the *physical* lights, folding each through the enabled chain to its logical cell. Three hooks — `modifyLogicalSize` (fold the box), `modifyLogical` (fold a coord, return false to reject), `modifyLive` (per-frame remap for Rotate).

Why the inversion was the right call, not just the chaining bolt-on:
- **Fan-out becomes free.** N physical lights folding onto one logical cell *is* the fan-out — no fan-out list, no product ceiling, no overflow. `destinationCount ≤ driverCount` is now a hard invariant. The whole `maxMultiplier`/scratch-buffer/`buildBoxToDriver`/`buildSparseIdentityLUT` machinery deleted.
- **The hot path is untouched.** Our `MappingLUT` is a CSR keyed by logical index; the inverted build is a scatter onto arbitrary logical keys, which doesn't fit `setMapping`'s in-order contract — so the build is a textbook **counting-sort CSR construction** (count, prefix-sum, scatter, replay) entirely on the cold path. The per-frame `forEachDestination` read is byte-identical.
- **Static vs dynamic split correctly.** Mask/tile/crop fold forward at build time (`modifyLogical`); rotation gathers backward per frame (`modifyLive`) — each in its natural direction, so rotation keeps its clean inverse-sample (no gaps), and a static-only chain pays *nothing* per frame (the live pass is gated on `hasModifyLive()`).

Lessons: (1) when a feature "doesn't compose," check whether the *interface direction* is wrong before adding machinery to force it — inverting the build deleted more code than it added. (2) A proven external model (MoonLight) is worth adopting wholesale when it's the textbook approach (backward mapping + LUT bake), but write it fresh against your own structures (our CSR, our names) rather than porting. (3) Matrices compose the *affine* subset cleanly (Rotate is written as an explicit 2×2 matrix, the codebase's matrix reference) but can't express masks/tiles — so the coordinate fold is the general composition model, with a matrix-backed modifier as a special case the same interface hosts.

## MoonLive: build around expressions + host-bound functions, not statement shapes

The MoonLive live-script compiler (IR rung) was first built around the *statement shape* `setRGB(idx, r, g, b)` — the parser had per-slot rules (index could be `random16`, colours were literal-only) and the IR had an RGB-specific `Store` op baked into the **core**. Three product-owner remarks exposed the same root flaw: (1) `random16` only worked in the index slot, not any argument; (2) `random16(255)` capped at a byte because the index/colour validators conflated ranges; (3) the core compiler was light-domain-specific (`setRGB`/`fill`/`Store` hardcoded), violating *Domain-neutral core*.

The fix was the ESPLiveScript / ARTI / doc-§3.4 model: **the core knows only expressions + a generic call mechanism; the host registers its functions in a builtin table.** Every argument parses as an expression (a literal or a nested call), so `setRGB(random16(256), random16(256), 30, 0)` works and a number is a uint16. `setRGB`/`fill`/`random16` — the LED *names* and the RGB meaning — live only in the light-domain registration (`MoonLiveBuiltins_light.h`); the core sees a neutral `BuiltinTable` of `{name → Call(fn ptr) | Inline(opcode tag)}`. A buffer writer is `Kind::Inline` (lowers to stores — the hot-path fast path, no per-pixel call); a pure helper is `Kind::Call`. A mechanised test pins the neutrality: with an empty table the core knows *no* functions, and a host can register an arbitrary name (`paint`) against the same machinery.

Two codegen lessons surfaced fixing it: (1) **the live-vreg-across-call contract must hold for ANY expression, not a hand-ordered one** — once arguments can be calls, a value computed before one call can be live across a *second* call; the assembler's `call()` must save/restore the whole caller-saved register set (host: a full stp/ldp frame; Xtensa: s32i/l32i of the rotate-out registers a8/a9/a11, with the result stashed in a non-saved reg across the restore). (2) **register budget is real on the MCU** — fold the address into a dead vreg (WriteRGB writes into the index register after `index *= cpl`) rather than reserving fresh scratch, so a multi-call statement fits the small windowed register file.

Lessons: (1) when a language "can't express X in slot Y," the fix is almost always *real expressions*, not a per-slot special case — build the general grammar once. (2) Domain-neutrality is testable: assert the core, given an empty host table, knows nothing — if it compiles a domain function, the domain leaked in. (3) The bound-function table is the same seam for *speed* and *neutrality*: the descriptor carries how it lowers (inline store vs call), so the core stays LED-free while the hot path stays inline.

### MoonLive RISC-V backend + vreg reuse (the third ISA, and what it exposed)

Bringing up the ESP32-P4 (RISC-V) backend — a third per-ISA assembler + lowering behind the same neutral IR — was mechanical *and* revealing. Mechanical: RV32 is uniform 4-byte instructions and a standard (non-windowed) call ABI, so the assembler is simpler than Xtensa's; every encoding was verified by disassembling the assembler's own output with `riscv32-esp-elf-as`/`objdump` before flashing (the same discipline that caught every Xtensa encoding bug). Revealing: the P4 was the first target where multi-call statements *failed to compile*, exposing two limits the host (14 regs) and Xtensa (12 regs, but the heaviest test was 2 calls) had masked.

First, the **register file**. The front-end allocated a fresh virtual register per sub-expression and never reused it, so `setRGB(random16(..), random16(..), random16(..), 0)` needed more live vregs than the 12-register device pool — "codegen failed." The fix is the textbook tree-walk register stack: a free-list allocator where each argument temp is returned to the pool the moment its call consumes it, so a chain of N calls peaks at a handful of registers instead of growing 2N. `vregsUsed` (the lowering's reservation) is the high-water mark, which now *shrinks* because freed vregs are reused at low indices. This is the "concrete-first, the allocator arrives when a real script exhausts registers" point the design anticipated — and a 4-call statement is exactly that script.

Second, the **code arena**. RISC-V's `call()` saves the full caller-saved set around each host call (~140 bytes), so four calls in one statement is ~600 bytes — past the original 256-byte staging cap. Rather than grow per-script (a moving target), the arena is sized once for the heaviest *realistic* single statement (four-arg-all-calls on the bulkiest ISA). Exec memory is cheap; a fixed worst-case cap is simpler and more predictable than dynamic growth.

Lessons: (1) a third backend is the cheap insurance that the IR seam is real — if adding RISC-V had needed front-end changes, the seam was a lie; it needed none (only the new register/byte *limits* surfaced, which are target properties, not design leaks). (2) Register allocation is where "it works on my 14-register host" quietly diverges from "it works on the 12-register device" — bring up the smallest register file early. (3) Verify every emitted instruction against the real toolchain's disassembler before trusting it on hardware; it is faster than debugging a `StoreProhibited` on-device, every time.

### MoonLive: size the exec block to the program, and bound every fixed table the codegen fills

Two follow-on lessons after the engine landed, both about *fixed-capacity buffers in a code generator*. (1) **Allocate the live exec block at the emitted length, not the worst-case cap.** The first cut allocated `allocExec(kCodeCap)` (the 768-byte ceiling sized for a four-call statement) for *every* script, so a `fill(0,0,255)` — ~60 bytes of native code — reserved 768. The fix: `place()` allocates `allocExec(len)` (word-rounded for the IRAM 32-bit-store rule), so per-effect heap scales with the script; the staging *buffer* stays worst-case (it's transient stack), only the *retained* block is right-sized. Paired with it: the effect must **report that block** (`setDynamicBytes(codeLen())`) or the JIT'd code is invisible to the memory accounting — the UI card showed only `sizeof(MoonLiveEffect)` and none of the native code it allocated. (2) **A code generator's label/fixup tables need the same bounds check as its code buffer.** The assemblers guarded `emit()` against `kCap` from the start but let `newLabel()`/the branch-fixup enqueue write `labelPos_[]`/`fixups_[]` unbounded — a script with enough branches would corrupt memory past the arrays. The fix routes both through the same `overflow_` signal `emit()` already uses (a guarded `addFixup()` helper, a bounds check in `newLabel()`), so *every* fixed table the codegen fills fails cleanly, not just the byte buffer.

Lessons: (1) when a JIT retains a fixed buffer per unit, size the *retained* allocation to the actual output and keep only the *scratch* at worst-case — and report the retained bytes, or the memory accounting lies. (2) a bounds check on the code buffer is not enough: audit *every* fixed-capacity array the generator appends to (labels, fixups, relocation tables) and route them all through one overflow signal — robustness is per-table, not per-generator. (3) these surfaced from a deliberate memory review at the small-language stage, before loops/expressions multiply the branch count — cheaper to pin the discipline now than after a real script first overruns a table.

### MoonLive Stage 1: live controls via a 5th-argument data arena (not baked immediates)

A scripted effect became *tunable*: `uint8_t speed = 50; // @control 0..99` surfaces a real uint8 MoonModule control. The load-bearing decision was **how a control value reaches the native code each tick without recompiling on every slider move**. A baked-in `Const` immediate is wrong (a slider change would need a re-emit); the answer is the **`kArg3`/`t` pattern, one slot over** — pass a pointer to a control-values *arena* as a 5th function argument (`CtrlFn = void(*)(buf, nLights, cpl, t, const uint8_t* ctrls)`), add an IR `LoadCtrl{offset}` that lowers to one byte load (`ldrb [x4]` / `l8ui a6` / `lbu (a4)`), and let the binding update an arena byte when a slider moves. The next `run()` reads it — live, no recompile. `t` already proved this exact mechanism works across all three ABIs, so a control pointer was a mechanical addition, not a new design.

Three sub-decisions held against the principles: (1) **the arena lives in the engine as a stable-address, grow-capacity-only heap block** (`platform::alloc`, PSRAM-first), NOT a fixed member array (won't scale to big effects) and NOT a core sweep change (re-ordering `rebuildControls`→`onBuildState` to allow realloc-and-rebind would touch *every* module to serve one — violating *Core grows slower than the domain*). Because the sweep runs `onBuildControls` on every change, re-binding the control pointers there is automatic; because the arena only grows (never moves on a normal recompile), `&arena[i]` stays valid, and a kept control keeps its live value across a source edit. (2) **The binding owns the projectMM-facing details** — the engine exposes a *neutral* `DeclaredControl{name, type, min, max, def, offset}` list and the binding turns each into `controls_.addUint8` bound to the arena slot, keeping the engine domain-neutral; the binding copies the (non-NUL-terminated) name spans into a stable pool because the control descriptor borrows the name pointer. (3) A scripted control's value change returns **false** from `controlChangeTriggersBuildState` (no recompile); only `source` returns true.

Two things surfaced building it. **The device JSON string reader (`parseString`) decoded only `\"` and `\\`, not `\n`** — yet the writer emits `\n` escapes — so a multi-line script arrived with a literal backslash-n and the `// @control` comment swallowed the statement. The fix is symmetry: the reader now decodes the standard escapes its own writer produces (`\n \r \t \b \f`), which benefits any multi-line text control, not just scripts. And **the `ok()` predicate**: adding the `ctrl_` (5-arg) compiled-source path meant `ok()` had to include it — the classic "added a code path, forgot the predicate that gates it" (compile returned true while ok() returned false, so every source compile looked failed). Pinned by the live-read unit test thereafter.

Lessons: (1) when a value must change live without re-emitting code, pass it through *data* (an arena read each tick), never *code* (a baked immediate or self-modifying patch) — the read is one instruction and the arena is the seam the next capability (script state, palettes) reuses. (2) Reach for the mechanism the system already proves: `t` was the template, so the 5th-arg control pointer was low-risk by construction. (3) A reader that decodes a subset of the escapes its writer emits is a latent asymmetry bug — make them symmetric. (4) Adding a parallel code path means revisiting every predicate that gates the old paths: the `ok()` bug — a host-side compile-validation gap where the new `ctrl_` path compiled fine but `ok()` still tested only `fn_`/`anim_`, so every source compile read as failed — was caught by the host live-read unit test (the runtime test path's job), not by disassembly. (5) Separately, on the device codegen: register-map shifts (stealing the next arg register) are the riskiest part; disassembling every emitted instruction against the real toolchain before flashing is what confirmed `l8ui a6`/`lbu (a4)` land correctly. The reader-symmetry and runtime-predicate fixes are host test-path lessons; the register-encoding evidence is device-ISA — distinct, not the same fix.

### Adding a new chip (ESP32-S31) + bumping the IDF: where the cost actually was

Adding the S31 (a RISC-V preview chip) cost almost nothing in *our* code — it inherited the RISC-V MoonLive backend and all four LED drivers for free, because both gate on SOC capabilities (`__riscv`, `SOC_RMT_SUPPORTED`, `SOC_PARLIO_SUPPORTED`, …), not a chip-family flag. The real work, and the lessons, were in the **toolchain and the platform/board layer**:

- **An IDF pin bump is a re-test pass, not a `git pull`.** Moving from an early v6.1-dev snapshot to the `release/v6.1` tip (the line carrying the S31 preview target) jumped ~4800 commits, which surfaced HAL *graduation* drift: the RMT HAL moved into its own `esp_hal_rmt` component, deleting the public `SOC_RMT_TX_CANDIDATES_PER_GROUP` soc-cap (now `RMT_LL_TX_CANDIDATES_PER_INST`), and Parlio renamed `sample_edge`→`shift_edge`. None of this is caught by building one target — a **re-verify gate that rebuilds every existing target on the new IDF** is what found them before they reached CI. The pin also needs the toolchain re-installed (`install.sh`) and the env's stale Python venvs refreshed; the build picks the most-recently-modified venv, which can be the wrong one.
- **A chip's *minimum-revision* default can silently brick field silicon.** v6.1 defaults the P4 minimum to rev 3.1; the bench (and field) P4 is v1.3, so the stock build refused to boot ("requires chip revision [v3.1-v3.99]"). The fix (`SELECTS_REV_LESS_V3` + `REV_MIN_0`) was only *found by flashing real hardware* — it builds clean either way. Hardware re-test, not CI, is what catches a min-rev trap.
- **Newest esptool-js ≠ best for flashing.** esptool-js 0.6.0 (the latest) deterministically corrupts a compressed flash — a P4 web-flash aborts at the *same* block (seq 38) every time, where 0.4.7/0.5.7 and the CLI all succeed (a fixed-seq failure, not random, rules out a transient). We pinned **0.5.7 — the version ESP Web Tools (ESPHome/WLED) ship** — over the newest, because *Common patterns first* applies to dependency versions too: match the battle-tested one the ecosystem runs, not the highest number. (0.5.x also moved `hardReset` off `ESPLoader`; driving DTR/RTS through the transport directly is version-agnostic.)
- **A new chip can be CLI-flashable but not browser-flashable**, and that's an *upstream* gap, not ours: esptool.py knows the S31 (since v5.2.0), esptool-js has no S31 chip class in any version. Worse, the S31's ROM magic collides with the classic ESP32's, so esptool-js would *mis-identify* it — a version bump alone won't fix browser flashing, it needs the secondary detection esptool.py already has. The catalog ships the firmware (`ships:True`) anyway: even with no web-flash, the published release asset is the device's **OTA** update channel, which is the more-used path (flash once via CLI, OTA forever).
