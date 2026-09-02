#pragma once

// This is the .h interface. Bodies live in FilesystemModule.cpp — splitting them
// out keeps the every-time-it-edits-recompile cost off the rest of the tree.

#include "core/MoonModule.h"

#include <atomic>

#include <cstddef>
#include <cstdint>

namespace mm {

class JsonSink;   // writeNode serializes into one (streaming/heap mode); full def in JsonSink.h (the .cpp includes it)

class Scheduler;
struct ControlDescriptor;

/// Control-list-driven JSON persistence — writes control values to flash so settings
/// survive a reboot. Always loaded, runs first in the scheduler so its load hook fires
/// before any other module's `setup()`
///
/// **Storage:** one flat JSON file per top-level MoonModule under
/// `/.config/<TypeName>.json` (the filename is `MoonModule::typeName()`). Children are
/// encoded positionally with an `<index>.` key prefix — no nested objects, no arrays,
/// loaded with the cheap first-match key helpers (parseString/Int/Bool). A control
/// whose *value* is structured (a List control's array of objects) is read back with
/// the recursive reader in `core/JsonUtil.h` via the control's own restore hook: the
/// file's top level stays flat, the structure lives inside one control's value string.
/// `ReadOnly` and `Progress` controls are never persisted — they are derived values,
/// not state.
///
/// **Structural reconciliation.** The `type` field per child drives reconciliation at
/// load time: when the JSON describes a child type at position N that differs from the
/// live tree's child at N, the loader factory-creates the JSON type, runs its
/// `defineControls()`, and swaps it into place. Children present in the live tree but
/// missing from the JSON are torn down; children in the JSON beyond the live tree's end
/// are appended. Phases 3+4 cascade into the reconciled tree, so newly-created children
/// are fully initialised like any other.
///
/// **Boot flow (Scheduler::setup, four phases):**
///   - phase 1 `defineControls()` — every module binds its full control set (incl. hidden)
///   - phase 2 `loadAllHook` — this module reads each file and overlays bound variables
///   - phase 2b `rebuildControls()` — re-runs defineControls so conditional hidden flags see the persisted values
///   - phase 3 `setup()` — modules' own init runs with persisted values in members
///   - phase 4 `prepare()` — buffers sized to final values
///
/// The Scheduler exposes `setLoadAllHook()` as a function pointer so it stays
/// independent of this module's type (no circular include); the hook is wired from
/// `setScheduler()`.
///
/// **Save flow.** HttpServerModule calls `markDirty()` + `noteDirty()` on every
/// successful mutation — control changes AND tree-shape changes (add / delete / move a
/// module marks the parent dirty so its file is rewritten with the new child set).
/// `noteDirty()` stamps `lastDirtyMs_` (and `firstDirtyMs_` when a save was not already
/// pending, which bounds the wait: see MAX_DEFER_MS); `tick1s()` waits `DEBOUNCE_MS` (2 s) after the
/// last dirty mark, then walks the tree and serialises any subtree with a dirty
/// descendant to a flat JSON blob, written atomically (write to `.tmp`, then rename). A
/// subtree's dirty flag clears only after its write succeeds; a failed write leaves it
/// set so `tick1s()` retries. `flushPending()` forces all dirty subtrees through
/// synchronously (the reboot handler calls it so an add-then-reboot doesn't lose the
/// change); losing power before the debounce expires loses the in-flight change — the
/// cost of debouncing for fewer flash writes.
///
/// **Conditional visibility.** Modules with conditional controls bind their full
/// control set unconditionally and toggle a `hidden` flag per descriptor, so the
/// persistence layer can find and overlay a value regardless of the live conditional
/// state while the UI honours the flag. A Select change at runtime triggers
/// `rebuildControls()` to re-evaluate the flags.
///
/// **Platform layer.** Filesystem access goes through `platform::fs*` (mount, mkdir,
/// read, atomic write-then-rename, used/total). ESP32 uses LittleFS on a dedicated
/// partition; desktop uses `std::filesystem` rooted at `build/` (overridable via
/// `fsSetRoot` for test isolation). Save and load are both cap-free: the save serializes into a
/// growable `JsonSink` (heap, no ceiling) and the load reads the whole file into a heap buffer
/// sized to it — a config of any size (many light presets, a wide fixture) round-trips in full.
///
/// **First boot:** no files exist → load is a no-op, modules run with default
/// member-initialised values; after the first UI change the debounce creates the file.
/// A missing key keeps the default, an unknown key is silently ignored (no schema
/// migration).
/// @card FilesystemModule.png
class FilesystemModule : public MoonModule {
public:
    static constexpr const char* CONFIG_DIR = "/.config";
    static constexpr size_t MAX_PATH = 64;
    static constexpr size_t MAX_KEY = 48;
    static constexpr uint32_t DEBOUNCE_MS = 2000;
    /// The CEILING on how long a pending save may be deferred, however often changes keep arriving.
    ///
    /// The debounce alone waits for quiet, which a continuous writer never provides: a script
    /// driving a control at 50 Hz re-stamped `lastDirtyMs_` twenty times per DEBOUNCE_MS, so the
    /// window never closed and the file was NEVER written. Measured on an ESP32-P4: an unrelated
    /// setting changed while a sweep ran was still unsaved 40 seconds later, and a power cut would
    /// have lost it. The starvation is not about the swept value itself, which nobody needs saved,
    /// but about everything ELSE in the same file being held hostage by it.
    ///
    /// Ten seconds: long enough that a burst of edits still coalesces into one write (the reason
    /// the debounce exists), short enough that a power cut loses at most that much. A save is one
    /// atomic write of a small file, so the worst case this admits is one write per ten seconds
    /// per module, which flash tolerates indefinitely.
    static constexpr uint32_t MAX_DEFER_MS = 10000;

    /// Test-only: age the pending save's ceiling clock by `ms`, as though that long had passed
    /// since the first dirty mark.
    ///
    /// The starvation contract is "a pending save lands within MAX_DEFER_MS however often marks
    /// arrive", and proving it by SLEEPING costs ten seconds of wall clock in a unit suite that
    /// otherwise runs in nine. The same seam shape platform::setTestGpioLevel uses: the behavior
    /// under test is the comparison in tick1s, not the host's ability to wait.
    void ageDirtyForTest(uint32_t ms) { firstDirtyMs_ -= ms; }

    /// Singleton is registered in setScheduler() (called by main.cpp on the real
    /// FilesystemModule), NOT in the constructor. The factory creates short-lived
    /// probe instances for /api/types defaults capture; the probe's destructor would
    /// otherwise clear instance_ and break noteDirty()/flushPending() for the rest
    /// of the device's life.
    FilesystemModule() = default;
    ~FilesystemModule() override;

    /// Persistence must keep flushing dirty subtrees regardless of the `enabled` toggle —
    /// otherwise the user could lose changes by accidentally disabling this module via
    /// the UI before the 2s debounce expires.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// Non-UI: a pure persistence engine with no controls — not shown as a card (its "last saved"
    /// status is displayed by FileManagerModule). See MoonModule::appearsInUi.
    bool appearsInUi() const override { return false; }

    void setScheduler(Scheduler* s);
    void setup() override;
    void tick1s() MM_NONBLOCKING override;

    /// The engine's live "last saved" buffer — "never" before the first save, else "5m ago". This
    /// is the persistence engine's status; FileManagerModule binds its read-only "lastSaved" control
    /// straight to this buffer (no copy — same no-per-instance-copy pattern SystemModule uses for
    /// its statics), and this module's tick1s keeps it current. Returns the mutable buffer because
    /// addReadOnly takes a char* it points the control at. FilesystemModule itself has NO controls —
    /// it's a non-UI engine, not a card in the module tree.
    char* lastSavedStr() { return lastSaveStr_; }

    /// The live singleton (the main-wired instance registered in setScheduler), or null before
    /// boot wiring. FileManagerModule reads lastSavedStr() through this.
    static FilesystemModule* instance() { return instance_; }

    /// Synchronous save of every dirty subtree, bypassing the debounce. Same work
    /// tick1s does once the debounce expires. Exposed for tests so they can assert
    /// the file appears without wall-clock waits; production callers shouldn't need this.
    void flush();

    /// Static convenience for callers (such as the reboot handler) that need to force any
    /// debounced saves through before a release — mirrors noteDirty's call style.
    static void flushPending();

    /// Called by HttpServerModule on every successful control mutation so the
    /// 2s debounce starts. Cheap timestamp record; the actual walk happens in tick1s().
    static void noteDirty();

    /// Serialize a subtree into a caller's sink instead of to `/.config/<TypeName>.json`.
    ///
    /// Same bytes `saveSubtree` writes — the flat dotted-key form `applySubtree` reads back — so a
    /// caller that stores a subtree elsewhere (ControlModule's presets, one file per named preset)
    /// gets the persistence format for free rather than growing a second serializer that could
    /// drift from this one. Emits the enclosing `{}`; returns false only on allocation failure.
    /// `prefix` namespaces every key ("Effects.0.type"), so several subtrees can share one flat
    /// object and `applySubtree` reads each back with the same prefix. Empty for a bare subtree.
    bool saveSubtreeTo(MoonModule* m, JsonSink& sink, const char* prefix = "");

    /// Apply a serialized subtree to a LIVE module tree, creating, replacing and destroying children
    /// to match, then driving the lifecycle a runtime rebuild needs.
    ///
    /// The reconciliation itself is the boot loader's (`applyNode`): type-at-position decides
    /// whether a child is kept and overlaid, replaced, or appended; an unknown type is skipped so
    /// the rest of the tree still applies; code-wired children are never replaced. What this adds is
    /// the part boot gets from the Scheduler's later phases and a runtime caller does not:
    /// `applyNode` calls only `defineControls()` on a child it creates, so `setup()` and
    /// `applyState()` must follow or the module comes back half-initialized. Mirrors the tail of
    /// HttpServerModule's runtime add path.
    ///
    /// Does NOT call `prepareTree()` or persist: a caller applying several subtrees should batch one
    /// `prepareTree()` after the last, since each is a full walk and this runs inline on the render
    /// tick. Structural mutation is safe here because `addChild`/`replaceChildAt` quiesce the render
    /// worker themselves (MoonModule::quiesceForMutation).
    ///
    /// `prefix` is "" for a whole subtree; the dotted form addresses a node within one.
    /// Returns whether the subtree was applied. False means the body was not credibly one of
    /// ours (malformed, or missing this prefix entirely) and the live tree was left alone — a
    /// caller reporting success to a user needs to tell that apart from a real apply.
    bool applySubtree(MoonModule* m, const char* json, const char* prefix = "");

    /// Live reconfiguration for a WRITTEN config file: a `/.config/<Type>.json` upload applies
    /// onto the running tree through the same `applySubtree` a preset uses, the boot loader's
    /// rule extended to the file-upload path, so a restored backup needs no reboot. One level
    /// only: `/.config/presets/*` are apply-on-demand captures, not module files; any other path
    /// is not config. Returns whether a module matched AND its subtree applied.
    bool applyConfigFile(const char* path);

    /// The DEFERRED entry the file-upload path uses: queue the module named by a written
    /// `/.config/<Type>.json`; tick20ms() applies it on the RENDER task. Applying inline on the
    /// small web-server task crashed the ESP32 (NetworkModule::setup() re-run mid-request), and
    /// deferring also sends the HTTP response before a network-reconfiguring apply can cut the
    /// socket. Returns whether the path names a live top-level module (same rules as
    /// applyConfigFile). Multi-file uploads coalesce to one apply per module.
    bool requestConfigApply(const char* path);

    void tick20ms() MM_NONBLOCKING override;

private:
    static inline FilesystemModule* instance_ = nullptr;
    Scheduler* scheduler_ = nullptr;
    bool mounted_ = false;
    bool dirtyPending_ = false;
    int moduleIndexForConfigPath(const char* path);
    std::atomic<uint32_t> pendingApplyMask_{0};   // bit = scheduler module index; web task sets, render tick consumes
    bool everSaved_ = false;       ///< false until the first successful save
    uint32_t lastDirtyMs_ = 0;
    /// When the CURRENT pending save first became dirty, so a continuous writer cannot defer it
    /// forever. Stamped by the first noteDirty after a flush, not by every one. See MAX_DEFER_MS.
    uint32_t firstDirtyMs_ = 0;
    uint32_t lastSaveMs_ = 0;
    char lastSaveStr_[24] = "never";  ///< "last saved" status string; FileManagerModule reads it via lastSavedStr()
    // No persistent load/save buffer: save serializes into a transient growable JsonSink and load
    // into a transient file-sized heap buffer, each allocated for the operation and freed after — so
    // the module holds NO large standing buffer (the old fixed 2 KB member is gone), and the heap is
    // used only briefly during a save (debounced, ~every 2 s) or the one boot load.

    // ---- Internals ----
    void updateLastSavedStr();
    static void loadAllHookTrampoline_(Scheduler* s);
    void loadAll(Scheduler* s);
    static void reapplyValuesHookTrampoline_(Scheduler* s);
    void reapplyValues(Scheduler* s);
    void loadSubtree(MoonModule* m);
    void applyNode(MoonModule* m, const char* json, const char* prefix);
    /// Second load pass, values only, run after the tree is prepared: see the definition for why a
    /// schema that only settles in prepare() (a MoonLive script's declared controls) needs it.
    void reapplySubtree(MoonModule* m);
    void reapplyNode(MoonModule* m, const char* json, const char* prefix);
    void applyWiredChildFromJson(MoonModule* wired, const char* json, const char* prefix);
    static bool hasWiredChildOfType(const MoonModule* parent, const char* typeName);
    void overlayControls(MoonModule* m, const char* json, const char* prefix);
    void applyValue(const ControlDescriptor& c, const char* json, const char* key);
    bool saveSubtree(MoonModule* m);
    void writeNode(MoonModule* m, JsonSink& sink, const char* prefix, bool firstField = true);
    static bool subtreeDirty(MoonModule* m);
    static void clearSubtreeDirty(MoonModule* m);
    static bool pathFor(MoonModule* m, char* out, size_t n);
};

} // namespace mm
