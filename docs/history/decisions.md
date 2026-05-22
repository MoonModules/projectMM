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
    through virtual methods. Layer should not know about MirrorModifier
    or RotateModifier by name.
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
- **Layer start/end percentages.** `startPct`/`endPct` as Coord3D percentages (0-100) of the total fixture. Maps to v3's "start/end position within the physical layout."
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

Writing module specs before implementation prevented the architectural drift that plagued v1 and v2. Each spec documents: purpose, controls, behavior, edge cases, prior art. When the code deviates from the spec, one of them is wrong — the spec serves as the reference.

Drafts live in `moonmodules_draft/`, get reviewed and promoted to `moonmodules/` just before implementation. Drafts for implemented modules are deleted — only one source of truth.

### Zero-copy preview driver (memory lesson)

The initial PreviewDriver allocated a 49KB frame buffer to copy pixel data before sending via WebSocket. Following MoonLight's pattern (drivers read directly from the physical buffer), we eliminated the copy — saving 49KB on ESP32 without PSRAM. The lesson: always check if an existing buffer can be reused before allocating a new one.

### WebSocket GUID typo cost hours

A single wrong character in the RFC 6455 magic GUID (`5AB5FDF632E5` instead of `C5AB0DC85B11`) caused the WebSocket handshake to fail silently — the SHA-1 was correct, the response format was correct, but the browser rejected it. The accept key matched our computation but not the browser's because the GUID was wrong. Lesson: when implementing protocols, verify against the RFC test vectors, not just internal consistency.

### KPI tracking in every commit

Adding standardized KPIs (binary size, FPS, heap usage, test count, lizard warnings) to every commit message makes performance regressions visible in git history. The `collect_kpi.py --commit` script automates this.

## Lessons from projectMM v3 (this project)

Extracted from the first implementation cycle (Steps 1-9, 2026-05-18).
This cycle produced working code (effects, modifiers, ArtNet output
visible on a hub75 panel) but accumulated enough bugs and architectural
drift that we decided to restart implementation with better specs.

### Rushing from architecture to code
- We went from architecture.md directly to implementation in 9 rapid
  steps. Each step introduced bugs that required debugging in later
  steps. The debugging consumed more time than the implementation.
- Plan mode was used per step but the plans were too shallow — they
  described what to build, not how it should behave. Edge cases
  (packet pacing, 2D rotation, mirror as kaleidoscope vs flip) were
  discovered during testing, not during planning.

### Architecture evolved during implementation
- Major architectural changes happened mid-implementation:
  Fixture → LayoutGroup rename, layouts changed from LUT-owners to
  coordinate iterators, mirror changed from coordinate flip to
  kaleidoscope (1:N mapping), DriverGroup took ownership of blend+map.
- Each change required reworking already-written code and tests.
  The architecture should have been stable before coding started.

### Missing UI specification
- The web UI was built from best guesses, then iteratively fixed
  based on user feedback (dropdowns not working, checkboxes not
  responding, sliders not smooth, cache issues).
- A UI specification should exist before implementation. The UI
  should be designed, not discovered through bug reports.

### Module docs should precede code
- Module documentation was written AFTER implementation as reverse
  engineering. It should be written BEFORE as specifications.
  Each module doc should define: controls, behavior, edge cases,
  interaction with other modules.

### The "it works on my test but not in the app" problem
- Standalone tests (e.g. mirror mapping coverage test) passed
  perfectly, but the running app showed wrong output. Root cause
  was ArtNet packet flooding, not rendering bugs. Symptoms and
  causes can be in completely different subsystems.
- Need integration tests that exercise the full pipeline, not just
  unit tests per component.

### Timing-dependent bugs are the worst
- The ArtNet packet pacing issue: without a printf in the main loop,
  the receiver dropped packets and output looked wrong. With the
  printf (adding ~1ms delay), it worked. The "debug code fixes it"
  pattern is a classic timing bug indicator.
- All output drivers need explicit pacing. Never blast network
  packets in a tight loop.

### dynamic_cast coupling
- Layer::render() uses dynamic_cast<MirrorModifier*> and
  dynamic_cast<RotateModifier*> to dispatch modifier behavior.
  This tightly couples Layer to specific modifier types.
- Modifiers should have a virtual interface (transformCoord,
  transformLights) that Layer calls without knowing the concrete type.

### Hot-path allocation in RotateModifier
- The RotateModifier allocated a temporary buffer every frame via
  platform::alloc. This violates the zero-alloc hot path rule and
  was never caught by tests (tests don't check for allocations).
- Need a zero-alloc render loop test that intercepts malloc/free.

### The LayoutBase adapter boilerplate
- GridLayout uses templates for forEachCoord (for type-safe lambdas)
  but LayoutGroup needs virtual dispatch (function pointer + void*).
  This requires a GridAdapter class that inherits both GridLayout
  and LayoutBase — pure boilerplate in every layout and every test.
- Layouts should implement virtual dispatch directly, or the
  template/virtual split should be resolved.

### Rebuild propagation is ad-hoc
- When a layout or modifier control changes, the Layer needs to
  rebuild its LUT and the DriverGroup needs to reallocate its
  output buffer. This is handled by explicit dirty flag checks in
  main_desktop.cpp — one check per module, manually maintained.
- This doesn't scale. Adding a new module type that affects the
  pipeline requires adding another dirty check to main.cpp.
- Need an event/observer system or a centralized "pipeline changed"
  signal.

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

### Cycle-1 assessment (May 2026)
A point-in-time evaluation written during PR #3 ([feature/more-effects](https://github.com/ewowi/projectMM-v3/pull/3)). Captured here because the full doc didn't earn a place in `history/` (160 lines, fast-aging) but the conclusions are worth keeping.

**Where v3 actually stood:** end-to-end pipeline working (layout → layer → effects/modifier → blend/LUT → drivers → HTTP/WebSocket UI), serious testing (unit + scenario + live), ESP32 hardware proven. **Not** a specs-only project. Vertical-slice completeness ~65-70%; Release 1.0 progress ~50%; full architectural vision ~25-30%.

**What was working well:**
- Engineering discipline holding: CLAUDE.md + architecture docs + `check_platform_boundary.py` + promoted specs that actually match the code. Avoiding the drift v1/v2 hit.
- MoonModule pattern earning its weight — one lifecycle, generic children, runtime CRUD via factory, no UI rewrites per effect.
- Memory-conscious by design: `memory-1to1` and `memory-lut` scenarios prove the LUT-vs-no-LUT decision; `performance.md` actively measured.
- Test ladder mature (unit → scenario → live on hardware) for the project's age.
- UI philosophy holding: no npm chain, controls render from module state, WebSocket-driven.

**Real technical debt called out:**
- `HttpServerModule.h` is a monolith — every concern in one header. Functional, but the antithesis of the rest of the codebase's per-file simplicity.
- `Scheduler::rebuild()` calls `onAllocateMemory()` on **all** top-level modules for every control change. Coarse-grained; the spec'd fine-grained rebuild propagation isn't there.
- ArtNet UDP dominates ESP32 tick time (~51% at 128×128). Not blocking, but a known scaling cliff.
- Documentation-vs-reality drift: README still said "implementation starting" when live ESP32 scenarios were passing. Internal docs were good; external were stale.

**Implementation gaps that became plan items:**
- No System MoonModule in the tree (diagnostics existed in `/api/system` but not as a queryable module). → Landed as part of plan-10's foundation work.
- No WiFi, no persistence, no UI type picker. → Persistence landed (plan-10); WiFi/AP cascade landed earlier; type picker is plan-12.
- Multi-layer support documented but not coded. → Backlog (plan.md "Multi-layer pipeline").
- WS2812/APA102/direct-DMX output absent (only ArtNet). → Out-of-scope for Release 1.0; backlog.
- Teensy and Raspberry Pi platforms documented but not implemented. → Same.

**Dimension scores given at the time** (for calibration when re-assessing later):

| Dimension | Score | Status |
|---|---|---|
| Architecture | 8/10 | Clear core/domain separation, MoonModule everywhere |
| Core implementation | 7/10 | Pipeline works, tested, hardware-proven |
| Extensibility | 7/10 | Factory + specs; UI not yet fully generic for "everything from browser" |
| Testing & quality | 8/10 | Strong for project size |
| Product/UX (end-user) | 5/10 | Works locally; no WiFi flash flow, no persistence, no type picker |
| Performance (ESP32) | 6/10 | 128×128 works; ArtNet dominates; no LED DMA path |
| Documentation | 7/10 | Excellent internally; README/plan out of sync |

**What's already aged since the assessment** (the doc was written pre-plan-10): persistence is done, the 10 effects and `GET /api/types` are imminent via PR #3, more effects in `moonmodules_draft/` are being promoted. Use this entry as the baseline; the next assessment should compare deltas.

### What to do differently next time
- Write module specifications BEFORE code (docs/modules/*.md)
- Write UI specification BEFORE implementing the web UI
- Stabilize architecture.md BEFORE starting implementation
- Use integration tests (full pipeline, not just unit tests)
- Add a zero-alloc hot path test from the start
- Resolve the template/virtual dispatch split in layouts
- Design a proper modifier interface (virtual methods, not dynamic_cast)
- Build the rebuild propagation into the framework, not main.cpp

## Lessons from the next-iteration branch (plans 08-12)

The branch covering SystemModule/NetworkModule, persistence, the UI rewrite, the eth-only build, and the side-nav. Brief, actionable takeaways:

- **Plan-09 was right to abandon part of itself.** The first persistence attempt (~1700 LOC) didn't pay for itself; ~700 LOC of genuine foundations (partition scheme, platform fs API, MoonModule additions) were kept and the rest dropped. Plan-10 then succeeded with a far smaller control-list-driven design. Lesson: a plan that produces "keep the foundations, drop the feature, re-plan smaller" is a success, not a failure.
- **ESP-IDF v6.x removes WiFi via `EXCLUDE_COMPONENTS`, not a Kconfig flag.** `CONFIG_ESP_WIFI_ENABLED` is non-settable in v6.x — setting it `n` is silently ignored. The eth-only profile excludes `esp_wifi`/`wpa_supplicant`/`esp_coex` components (NOT `esp_phy` — the EMAC needs it) plus an `MM_NO_WIFI` define gating `if constexpr` branches in core. Verify a build profile actually changed something (image size, `nm` for symbols), don't trust the config.
- **The render tick collapses on any blocking network write on the hot path.** The FPS-swing was a blocking 49 KB preview WebSocket write spinning `vTaskDelay` until lwIP drained. Fix: non-blocking scatter-gather write + downsample the payload to fit the send buffer. Any per-tick socket write must be non-blocking and abandon-on-backpressure.
- **WiFi UDP is ~4× the Ethernet per-packet cost.** ArtNet at 16K lights is ~7 FPS on WiFi vs ~19 on Ethernet — WiFi physics (CSMA/CA, retries, rate adaptation), not a code regression. Recommend Ethernet (or the eth-only profile) for large installations.
- **No-op wrappers are an "unnecessary abstraction" the Reviewer must catch.** Two were found and removed (`HttpServerModule::parseJsonString` re-namespacing `mm::json::*`; `NetworkModule::rebuildLocalControlsAndPipeline` whose name contradicted its body). A pure pass-through that only renames is the opposite of duplication — a single thing that shouldn't exist. CLAUDE.md's Reviewer definition now names this pattern explicitly.
- **Always escape strings going into hand-built JSON.** A control value containing `"` or `\` produced malformed JSON in `/api/state` and the persisted config. Hand-rolled JSON serialization must escape on write and un-escape on read — caught by a round-trip test, not the reviewer.
- **Doc-vs-code drift hides in design changes.** When the password approach changed mid-implementation (length-only → XOR+base64), the docs and one header were updated but a second header's comment was missed — it then documented a security property the code didn't provide. When a design changes, grep for every mention of the old behaviour.
