#pragma once

#include "core/MoonModule.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace mm {

/// Domain-neutral orchestrator that owns the top-level module list, runs the 4-phase
/// boot, drives the per-tick loop callbacks, and provides tree-walk utilities (delete,
/// name-uniquify).
///
/// **Boot phases:** see the `setup()` comment in Scheduler.cpp.
///
/// **Tick:** gates each top-level module by `enabled()` / `respectsEnabled()` and
/// dispatches `tick` / `tick20ms` / `tick1s`. A per-second window averages the tick
/// time and publishes each module's loop time.
///
/// **Loop rates:** three cadences cover every module. `tick()` is the hot path for
/// effects and drivers, called every iteration — the Scheduler handles pacing (yielding
/// to other tasks between iterations via `taskYIELD()` on ESP32, an optional sleep on
/// desktop). `tick20ms()` runs every ~20 ms for UI updates, control reads, and network
/// polling. `tick1s()` runs every ~1 second for diagnostics, reconnects, and
/// housekeeping. Not every module needs `tick()`: system modules (HTTP, WiFi) use
/// `tick20ms()` or `tick1s()` only.
///
/// **Timing:** effects animate off a synchronized clock (millis from the platform), so
/// the visual speed is frame-rate independent — the same at 30 fps and 60 fps. For
/// multi-device sync a leader synchronizes this clock across devices; no frame counter
/// is needed.
///
/// **Execution model:** `tick()` runs every top-level module inline, in one loop, in
/// declared order; each module then drives its own children. There is no per-module
/// task and no core-affinity field — a single render loop, not a task-per-module fan-out.
///
/// **Module ordering:** child modules run in their declared order within the parent,
/// and top-level modules also run in declared order. The UI supports reordering, backed
/// by the scheduler. Relationships are parent/child only — there is no arbitrary
/// dependency graph.
///
/// **Prior art:** MoonLight's `effectTask` / `svelteTask` — two FreeRTOS tasks (effects
/// on core 1, system/drivers on core 0) with a per-node `tick()` every frame and a
/// `tick20ms()` for slow updates.
///
/// This is the `.h` interface; bodies live in Scheduler.cpp.
class Scheduler {
public:
    /// Function-pointer hook invoked between phase 1 (`defineControls`) and phase 3
    /// (`setup`). Used by FilesystemModule to overlay persisted control values onto bound
    /// variables before modules' setup() runs. Scheduler stays independent of
    /// FilesystemModule's type (no circular include). Wired in via setLoadAllHook from
    /// main.cpp.
    using LoadAllFn = void(*)(Scheduler*);
    void setLoadAllHook(LoadAllFn fn) { loadAllHook_ = fn; }

    /// Hook invoked ONCE after the first prepareTree(), for a module whose control set is not final
    /// until then: a MoonLive script's declared controls exist only after the script compiles, which
    /// is prepare()'s work, so the load pass above ran before they existed and their saved values had
    /// nowhere to land. Values only; the tree shape was settled by the load pass. Same decoupling as
    /// setLoadAllHook. No-op if unset.
    void setReapplyValuesHook(LoadAllFn fn) { reapplyValuesHook_ = fn; }

    /// Hook invoked after a control mutation so the persistence layer can schedule a
    /// debounced save (FilesystemModule::noteDirty). Same decoupling as setLoadAllHook —
    /// Scheduler stays independent of FilesystemModule's type. No-op if unset.
    using NoteDirtyFn = void(*)();
    void setNoteDirtyHook(NoteDirtyFn fn) { noteDirtyHook_ = fn; }

    void addModule(MoonModule* mod);
    void setup();
    void tick() MM_NONBLOCKING;
    void release();

    uint32_t elapsed() const;

    /// Rebuild derived state across the whole tree — buffers, mappings, and any scripted module's
    /// compiled program. Runs the work IMMEDIATELY on the calling thread.
    ///
    /// Prefer requestPrepareTree() from anything but the render loop: this walk runs a scripted
    /// layout's JIT'd code, whose frame lives on the CALLING TASK's stack like any other function's.
    /// Called from an HTTP handler it therefore executes on the small web-server task rather than
    /// the render task the rest of the pipeline is budgeted against.
    void prepareTree();

    /// Ask for a rebuild at the next frame boundary, on the render thread. Cheap and safe to call
    /// from any task — it sets a flag; tick() does the work.
    void requestPrepareTree() { prepareRequested_.store(true, std::memory_order_relaxed); }

    /// Ask for a values-only reapply right after the NEXT requested prepareTree(): the runtime
    /// twin of boot's phase 5: a config-file restore can carry values for controls that exist
    /// only once prepare() has run (a MoonLive script's declared controls), so the reapply must
    /// follow that prepare, in the same tick (before any dirty save can rewrite the file).
    void requestValuesReapply() { valuesReapplyRequested_.store(true, std::memory_order_relaxed); }

    uint32_t tickTimeUs() const { return tickTimeUs_; }
    uint32_t fps() const { return tickTimeUs_ > 0 ? 1000000 / tickTimeUs_ : 0; }
    uint8_t moduleCount() const { return moduleCount_; }
    MoonModule* module(uint8_t i) const { return i < moduleCount_ ? modules_[i] : nullptr; }

    static void deleteTree(MoonModule* mod);

    /// Ensure `mod`'s name is tree-globally unique. If something else already uses
    /// the same name, suffix with " 2", " 3", … until unique. Caller must have
    /// placed `mod` in the tree already (otherwise the lookup wouldn't see it).
    /// See Scheduler.cpp for the why and the name-length cap.
    void ensureUniqueName(MoonModule* mod);

    /// Walk the whole tree and disambiguate every duplicated name. First
    /// occurrence keeps its name; later ones get " 2", " 3", … suffixes.
    /// Cold-path: called once after persistence load in setup().
    void deduplicateNamesInTree();

    /// First module in tree-walk order with this name, or nullptr if none.
    MoonModule* firstByName(const char* name);

    /// The single live Scheduler, or nullptr before setup() / after release(). Mirrors
    /// FilesystemModule::instance_ — the one Scheduler is statically reachable so a module
    /// created by the factory (InfraredService) can call setControl() without a per-module injection.
    static Scheduler* instance() { return instance_; }

    /// Outcome of setControl — the generic control-set primitive's result. Transport
    /// layers map these to their own status (HTTP → 404 / 400 / 409, …).
    enum class SetControlResult : uint8_t {
        Ok,
        ModuleNotFound,   ///< no module with that name in the tree
        ControlNotFound,  ///< module exists but has no such control
        OutOfRange,       ///< numeric value outside the control's bounds
        Malformed,        ///< value didn't parse
        ReadOnly,         ///< tried to write a display-only control
    };

    /// Set one control by (module name, control name) to a value, applying the full
    /// control-change reaction: parse+validate, rebuild the module's control list, fire
    /// onControlChanged, mark dirty for persistence, and prepareTree() when the control reshapes
    /// dims/mapping. `valueJson` is a small JSON object read for its "value" key
    /// (`{"value":128}`) — the same shape /api/control, Improv, and the WLED bridge send.
    /// This is THE domain-neutral way for any module (IR, buttons, network bridges) to
    /// drive another module's control: they compose against this one primitive instead of
    /// reaching into a target's internals. The special control name "enabled" toggles the
    /// module's enabled flag. Returns the outcome; a transport maps it to its status codes.
    SetControlResult setControl(const char* moduleName, const char* controlName,
                                const char* valueJson);

    /// Read one control's value as a BYTE, in SURFACE units: the mirror of setControl, and the
    /// other half of what a control surface needs.
    ///
    /// DERIVED from getControlWide below rather than reading the control itself: the two answer the
    /// same question in different units, so the conversion is a rule (clamp to 8 bits, a Bool at
    /// full scale) rather than a second per-type switch to keep in step. A surface that only writes drifts the moment anything else moves the
    /// target (the web UI, a preset recall, an audio-reactive effect), and starts out of step at
    /// boot, where the surface's own default has never met the target's persisted value.
    ///
    /// A byte because that is the unit every surface control speaks (a fader's travel, a switch's
    /// on/off, an encoder's position), and the scaling to a wire lives in the transport. A Bool
    /// reads back 0 or 255 so a switch and a fader answer in the same units.
    ///
    /// Deliberately generic rather than a per-target accessor: the bindings are hard-wired today
    /// (fader1 to brightness, switch1 to on), and the point of routing through the same primitive
    /// setControl uses is that a soft-wired binding needs no new code here.
    ///
    /// Returns false when the module or control does not exist, or its type has no byte reading
    /// (a text or file-path control); `out` is untouched then.
    bool getControl(const char* moduleName, const char* controlName, uint8_t& out) const;

    /// Read one control's value at its OWN width, signed. THE reader: the byte form above is this
    /// one converted, so a new control type is added here and both callers follow.
    ///
    /// The byte reader is what a SURFACE speaks, and clamping is right there: a fader has 8 bits of
    /// travel. It is wrong for arithmetic on the control itself. A Uint16 holding 300 reads
    /// back 255, so a `+10` delta writes 265 rather than 310, and a negative Int16 clamps to 0, so a
    /// delta can never move it down at all. An input mapping nudges the control, not the surface, so
    /// it reads through this one.
    ///
    /// Returns false when the module or control does not exist, or its type has no numeric reading;
    /// `out` is untouched then.
    bool getControlWide(const char* moduleName, const char* controlName, int32_t& out) const;

private:
    void walkAndEnsureUnique(MoonModule* mod);
    static MoonModule* firstInTree(MoonModule* mod, const char* name);

    static inline Scheduler* instance_ = nullptr;
    std::array<MoonModule*, 32> modules_{};
    uint8_t moduleCount_ = 0;
    // ATOMIC: written from HTTP handlers (any task) and consumed on the render thread, so a
    // plain bool is a data race — and a lost request means a script edit silently never applies.
    std::atomic<bool> prepareRequested_{false};   // asked for off-thread; tick() honours it
    LoadAllFn loadAllHook_ = nullptr;
    LoadAllFn reapplyValuesHook_ = nullptr;
    bool valuesReapplied_ = false;   // the hook fires once, after the first prepareTree()
    std::atomic<bool> valuesReapplyRequested_{false};   // one-shot, consumed with the next requested prepare
    NoteDirtyFn noteDirtyHook_ = nullptr;
    uint32_t startTime_ = 0;
    uint32_t lastLoop20ms_ = 0;
    uint32_t lastLoop1s_ = 0;
    uint32_t tickTimeUs_ = 0;
    uint32_t tickAccumUs_ = 0;
    uint32_t frameCount_ = 0;        // frames in current 1-second window (for averaging)
    uint32_t lastTimingUpdate_ = 0;   // 1-second window start
};

// --- Drivers master-state reads, shared by every consumer that mirrors the light state (the WLED
// /json shim, MQTT). Domain-neutral: they name the `Drivers` module + its controls and read via the
// generic MoonModule::read* helpers, so the absent-control defaults are defined ONCE here instead of
// per consumer (a device with no `on` control still reads as on; brightness/palette default to 0). ---
inline bool driversOn(Scheduler* s) {
    MoonModule* d = s ? s->firstByName("Drivers") : nullptr;
    return d ? d->readBool("on", true) : true;   // absent → on
}
inline uint8_t driversBrightness(Scheduler* s) {
    MoonModule* d = s ? s->firstByName("Drivers") : nullptr;
    return d ? d->readUint8("brightness", 0) : 0;
}
inline uint8_t driversPalette(Scheduler* s) {
    MoonModule* d = s ? s->firstByName("Drivers") : nullptr;
    return d ? d->readUint8("palette", 0) : 0;
}

} // namespace mm
