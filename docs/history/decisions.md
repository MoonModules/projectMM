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
A point-in-time evaluation written during PR #3 ([feature/more-effects](https://github.com/MoonModules/projectMM/pull/3)). Captured here because the full doc didn't earn a place in `history/` (160 lines, fast-aging) but the conclusions are worth keeping.

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

This split works because every per-board control we ship today applies *post-association*: `Network.txPowerSetting` (LOLIN brown-out fix), and the future Ethernet pin maps, default-config overrides, etc. The radio briefly runs at the wrong setting for the ~1 s between association and HTTP fan-out completion — acceptable for power capping, would be unacceptable for:

- **Country code.** Governs which channels the radio scans; a wrong code at scan time picks wrong channels.
- **Antenna selector.** Wrong RF path at radio init makes the device deaf.
- **Pre-association TX-power.** Some chips need the power cap applied before the first probe request, not after association.

If we ever add such a control, **don't extend SET_BOARD's wire format to carry it** — that would couple unrelated controls to the board-name lifecycle and obscure the timing constraint. The two escape hatches are:

1. **Add a second vendor Improv RPC** (`SET_<control>` analogue of SET_BOARD) and dispatch it from the orchestrator BEFORE `SEND_WIFI_CREDENTIALS`. One RPC per pre-association control keeps the timing contract explicit.
2. **Bake the value into firmware** via a board-specific sdkconfig fragment. Works when the value is truly board-static (country code per region) and not user-configurable.

Pick (1) for user-configurable controls; (2) for truly fixed-per-board values. Avoid the implicit option C ("just push it earlier in SET_BOARD") — it tangles the lifecycle.

## Lessons from the LOLIN S3 N16R8 enablement branch

Three non-obvious failures showed up while adding native-USB S3 support, all with the same diagnostic shape ("symptom looks like X, root cause is somewhere completely different"). Record them so a future S3 / native-USB addition doesn't repeat the dig.

1. **USB-Serial-JTAG ≠ UART0 on ESP32-S3.** The LOLIN S3 N16R8's USB-C port wires through the ESP32-S3's built-in USB-Serial-JTAG peripheral, NOT through an external USB-Serial bridge to UART0. ESP-IDF's secondary-console feature (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`, on by default for S3) mirrors stdio out BOTH paths, so the developer sees boot logs and assumes UART0 is wired and working. It isn't. Improv-listener-on-UART0 was deaf because the host was talking to USB-Serial-JTAG. **Fix:** install BOTH drivers and read from both transports — `#if SOC_USB_SERIAL_JTAG_SUPPORTED` keeps ESP32-classic builds free of the JTAG path. Don't make this compile-time-per-board ("the LOLIN firmware") — the same binary should work on a board with either wiring.

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

Moving the repo `ewowi/projectMM → MoonModules/projectMM` and cutting the first stable release surfaced three CI failures that had nothing to do with the transfer's code changes — they came from the *infrastructure around* the release (GitHub's hosted runners, GitHub Pages environment rules). Same diagnostic shape as the LOLIN branch: the red X appears on a release/build, but the cause is a platform behaviour we pinned against, not our diff. Record them so the next release doesn't re-dig.

1. **Don't pin GitHub-runner toolchain specifics — the `windows-latest` image migrates underneath you.** Mid-release, GitHub began redirecting `windows-latest` from the VS 2022 image to `windows-2025-vs2026` (the run's own annotation warned: "redirected to windows-2025-vs2026 by June 15, 2026"). Two breakages followed on the *same source tree* that had been green the day before: (a) `package_desktop.py` hard-coded `-G "Visual Studio 17 2022"`, and the new image has no VS 2022 → CMake failed at configure ("could not find any instance of Visual Studio"), a 21-second fast-fail before any compile; (b) once the generator pin was dropped (let CMake auto-detect the installed VS), the build reached compilation and the *new* VS 2026 MSVC STL emitted **C5285** ("specializing `std::tuple` is forbidden") on the vendored `doctest.h`'s tuple forward-declaration, and `/WX` made it fatal. **Fixes:** drop the generator pin (auto-detect survives image migration); add `/wd5285` to the existing MSVC suppression list (it's third-party header code we don't own; GCC/Clang never warn). **Generalisable:** anything pinned to a hosted-runner's bundled toolchain version (CMake generator, compiler path, SDK version) is a time-bomb — the image rotates on GitHub's schedule, not yours. Prefer auto-detection; when a pin is unavoidable, expect to update it and don't treat a sudden Windows-only failure on an unchanged tree as your regression.

2. **Don't gate asset-publishing behind a Pages-only `environment:` — a tag fails the gate before any step runs, silently dropping all release assets.** The `release` job did two things (upload release binaries *and* deploy GitHub Pages) under one `environment: github-pages`. That environment's protection rule allowed only `main`. When publishing v1.0.0 created the `v1.0.0` *tag*, it re-triggered the workflow on `refs/tags/v1.0.0`; GitHub evaluates the environment protection rule **at job start, before the first step** — the tag failed it, so the *entire job was rejected in 2 seconds*, including the "Publish GitHub release" step. Result: a published release with **zero binaries**, and a red X whose message ("Tag v1.0.0 is not allowed to deploy to github-pages") pointed at Pages, not at the asset upload that actually got skipped. The two symptoms (missing assets + red X) had one root cause: coupling. **Fix:** split into a `release` job (no environment, `contents: write`, uploads assets — runs on tags) and a `deploy-pages` job (`needs: [release]`, `if: ref==main`, carries the `github-pages` environment). Tags publish assets; `main` deploys Pages. **Generalisable:** an `environment:` on a job gates the *whole job* via a ref-based protection rule evaluated up front — never put work that must run on refs the environment forbids (tag-triggered asset upload) in the same job as the environment-gated work (production deploy). Recovery without re-tagging: `gh workflow run release.yml -f tag=vX.Y.Z` replays the fixed job and uploads assets onto the existing release.

3. **A failure that looks like the change is often the environment — verify before assuming a regression.** Recurring across this branch: a scenario "failed" its 120µs tick contract at 536µs (machine load from concurrent builds + MoonDeck + a preview server — re-ran isolated at 118µs, passed); Improv reported `UNABLE_TO_CONNECT` while the device was *already provisioned and reachable* (async-confirmation timeout, not a join failure); MoonDeck showed `0/0 online` while the device served HTTP 200 (the active network record had an empty subnet and a duplicate record held the device). None were code or transfer defects. **The rule:** when something fails right after a change, first prove the failure is *about* the change — re-run isolated, probe the actual end state (ping/curl the device, read the real CI error line, check the env), and only then edit code. Several hours here would have been saved by checking the device was reachable *before* debugging the "WiFi failure".
