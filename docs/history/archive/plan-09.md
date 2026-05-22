# Plan-09 — FilesystemModule + flash partition scheme (attempted, abandoned)

## Outcome

Attempted JSON-based persistence (`FilesystemModule.h`, 436 LOC + `JsonUtil.h`, 256 LOC + 4 doctest cases for persistence + 9 doctest cases for JSON util) plus several defensive patches against memory pressure during grid resize. **Persistence and resize patches abandoned; partition layout + platform fs API + several incidental improvements kept (committed as "plan-09a foundations").** Total stripped: ~1700 LOC. Total kept: ~700 LOC of genuine improvements.

This file documents what we tried, why it didn't pay for itself, and what was kept.

## What was kept (committed)

- **Partition CSVs** (`esp32/partitions/esp32dev.csv`, `esp32s3_n16r8.csv`) copied from projectMM v1
- **Custom partition wired into sdkconfig.defaults** + `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`
- **joltwallet/esp_littlefs managed component** in `idf_component.yml` — adds ~30 KB; unused for now, will be consumed by plan-11
- **Platform fs API** in `platform.h` + desktop/ESP32 implementations: `fsMount`, `fsUnmount`, `fsMkdir`, `fsExists`, `fsRemove`, `fsRead`, `fsWriteAtomic`, `fsList`, `fsSetRoot`. Plus real `filesystemUsed/filesystemTotal` backed by `esp_littlefs_info()`. Foundation for whatever persistence story comes next.
- **MoonModule additions**:
  - `typeName_` as `const char*` (4 bytes vs the 24-byte buffer originally proposed) pointing into the factory's string literal — stable factory key distinct from per-instance `name()`
  - `dirty_` flag + `markDirty()` / `clearDirty()` / `dirty()` accessors — a clean hook for any future persistence consumer
  - `rebuildControls()` non-virtual helper + `clearControlsRecursive()` — the recursive-clear fixes a real latent bug where conditional onBuildControls would double children's controls
  - Documented onBuildControls idempotency contract
- **ModuleFactory** wires `setTypeName` alongside `setName` in `create()`
- **HttpServerModule** Select range check (rejects out-of-bounds values with 400) + `markDirty()` calls at the two mutation points in `handleSetControl`
- **Scheduler::teardown** two-pass (tear down all → delete all) so cross-module teardown logic can observe sibling state. Surfaced by attempted FilesystemModule but the bug existed regardless.
- **PreviewDriver** reads physical dimensions live from Layer each frame instead of caching startup values — fixes a pre-existing bug where grid resize broke the preview at all sizes
- **DriverBase::setLayer** + protected `Layer*` member — clean way for drivers that need geometry (Preview) to access it
- **DriverGroup** passes `Layer*` to children in `passBufferToDrivers`
- **UI**: `localStorage["mm.selectedModule"]` persists nav selection across browser refresh
- **CMakeLists DEPENDS fix** so version.h regenerates when `generate_version.py` changes
- **NetworkModule** mDNS retry-on-failure fix + local `rebuildLocalControlsAndPipeline` rename to avoid colliding with base helper
- **Test scenarios** `control-change.json` adds reset-state steps; new `grid-resize.json`
- **Two new doctest cases** in `test_moonmodule.cpp` covering `typeName` and `dirty` flag mechanics

## What was thrown away

### Persistence (the big one)

- `src/core/FilesystemModule.h` — 436 LOC
- `src/core/JsonUtil.h` — 256 LOC of custom JSON parser with nested + scoped lookups
- `test/test_filesystem_persistence.cpp` — 261 LOC
- `test/test_json_util.cpp` — 117 LOC
- `docs/moonmodules/core/FilesystemModule.md` — 174 LOC
- Scheduler 4-phase setup + 4b re-load + `LoadAllFn` hook
- SystemModule `deviceName_[0] == 0` guard (only needed because of Scheduler reorder)
- HttpServerModule `noteDirty(target)` calls + FilesystemModule include
- main.cpp FilesystemModule factory registration + scheduler injection
- Architecture.md persistence section

### Resize defensive patches

- BlendMap.h null guards on src.data() / dst.data()
- DriverGroup.h null guard in `loop()` (`outputBuffer_.data()` check)
- DriverGroup.h "allocate failed → fall back to Layer's buffer" logic
- Layer.h `allocateBuffer` redesign (identity-at-physical fallback, "buffer empty" tier)

## Why it didn't pay for itself

### 1. JSON was the wrong primitive for module persistence

The spec started with "human-readable, editable JSON" as an unexamined premise. **Neither human-readability nor manual editability are real requirements.** Once that premise was challenged, the code cost (custom nested JSON parser, recursive serializer, scoped lookup helpers, ~800 LOC) becomes hard to defend.

The honest job description is "save and restore module state at the right time". For POD-only module state (which is what MoonModule subclasses are), `memcpy(file, this + sizeof(MoonModule), classSize - sizeof(MoonModule))` is one line and produces a complete save. Plan-11 will pursue this.

### 2. Persistence forced a Scheduler reorder that bred secondary bugs

To overlay persisted values onto bound control variables, the Scheduler grew from 3 phases (setup → onBuildControls → onAllocateMemory) to 5 phases (onBuildControls → load → setup → rebuildControls → load again → onAllocateMemory). This:
- Required `onBuildControls` to be idempotent (good contract, but enforced by a foot-gun rather than a type)
- Bred a duplicate-children bug because `onBuildControls` recurses into children and `controls_.clear()` was top-level only
- Required SystemModule to guard `MAC → deviceName` derivation behind `if (deviceName_[0] == 0)` so the second `setup()` wouldn't overwrite persisted values
- Was the trigger for several "device shows nothing" hardware behaviors during testing

The right approach (for blob persistence): load happens BEFORE any module's setup or onBuildControls, by directly memcpy'ing into member memory. No 5-phase dance, no idempotency contract, no guards.

### 3. Resize defensive guards were fighting the symptom

The underlying issue is that `Layer::onAllocateMemory` + `DriverGroup::onAllocateMemory` rebuild in-place (allocate new before freeing old), which fragments the heap. Free heap stayed ~60 KB but max contiguous block shrunk to ~15 KB — too small for new lwIP TCBs, so HTTP refused connections. We added 5 patches across BlendMap, DriverGroup, Layer to handle each failure mode the fragmentation produced. The patches accumulated; each one was correct in isolation; collectively they obscured the design problem.

Plan-11.5 will pursue free-then-allocate: a two-phase rebuild that frees all light-pipeline buffers BEFORE attempting to allocate the new sizes. `canAllocate` sees true post-free heap, degrade decisions become deterministic, and the various stride-mismatch / zombie-state failure modes disappear by construction.

## Lessons

1. **Question the format premise.** "Persistence is JSON" was assumed in the spec without justification. Whenever a spec specifies a serialization format up front, ask: what would the minimum-bits form look like? For POD-only data the answer is usually "memcpy".

2. **Be suspicious of helper proliferation.** When we found ourselves writing `rebuildControls`, `clearControlsRecursive`, `LoadAllFn`, `setLoadAllHook`, `noteDirty`, `loadAll`, `loadTopLevel`, `applyNode`, `applyControls`, `serializeNode`, `serializeControls`, `buildTopLevelPath`, `cleanupTmpFiles_`, `cleanupTmpCb_`, `cleanupTmpLeafCb_` — that was the system telling us the design was too elaborate for the job.

3. **Defensive guards under memory pressure mask design bugs.** Each guard says "I don't trust this invariant" — which is a signal to fix the invariant, not the deref. If `outputBuffer_.data() == null` is reachable when `hasLUT() == true`, the design has a hole. Patch the design, not the call site.

4. **Test isolation reveals real test-design issues.** Live scenarios that mutated persistent state (mirror toggles, grid size) contaminated each other across runs — the test failures appeared random until we realized previous runs were leaving state in `.config/`. Useful diagnostic for any future persistence layer: tests that need state must reset it explicitly.

5. **Ship the foundation, redo the load.** When the build fails this badly, the right move was what we did: identify what's genuinely useful (partition table, platform fs API, MoonModule improvements, PreviewDriver fix, scheduler teardown fix) and commit that subset, then start fresh on the actual persistence design.
