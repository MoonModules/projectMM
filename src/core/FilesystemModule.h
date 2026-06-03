#pragma once

// FilesystemModule — control-list-driven JSON persistence.
//
// Storage: one flat JSON file per top-level MoonModule under /.config/<TypeName>.json.
// Children are encoded with "<index>." key prefix (positional). Parser is the flat
// parseJsonString/Int/Bool from core/JsonUtil.h — no nested objects, no arrays.
//
// Boot flow:
//   Scheduler phase 1: onBuildControls (every module binds full control set incl. hidden ones)
//   Scheduler phase 2: this module's loadAllHook reads each file and overlays bound variables
//   Scheduler phase 3: modules' own setup() runs with persisted values in member vars
//   Scheduler phase 4: onBuildState
//
// Save flow:
//   HttpServerModule::handleSetControl calls target->markDirty() on every mutation
//   This module's loop1s() debounces 2s, walks the tree, writes any dirty subtree atomically
//
// This is the .h interface. Bodies live in FilesystemModule.cpp — splitting them
// out keeps the every-time-it-edits-recompile cost off the rest of the tree.

#include "core/MoonModule.h"

#include <cstddef>
#include <cstdint>

namespace mm {

class Scheduler;
struct ControlDescriptor;

class FilesystemModule : public MoonModule {
public:
    static constexpr const char* CONFIG_DIR = "/.config";
    static constexpr size_t MAX_FILE_BYTES = 2048;
    static constexpr size_t MAX_PATH = 64;
    static constexpr size_t MAX_KEY = 48;
    static constexpr uint32_t DEBOUNCE_MS = 2000;

    // Singleton is registered in setScheduler() (called by main.cpp on the real
    // FilesystemModule), NOT in the constructor. The factory creates short-lived
    // probe instances for /api/types defaults capture; the probe's destructor would
    // otherwise clear instance_ and break noteDirty()/flushPending() for the rest
    // of the device's life.
    FilesystemModule() = default;
    ~FilesystemModule() override;

    // Persistence must keep flushing dirty subtrees regardless of the `enabled` toggle —
    // otherwise the user could lose changes by accidentally disabling this module via
    // the UI before the 2s debounce expires.
    bool respectsEnabled() const override { return false; }

    void setScheduler(Scheduler* s);
    void setup() override;

    // Read-only "last saved" display — surfaces in the UI so the user can see
    // how long ago the config was last written (or "never" before any save).
    void onBuildControls() override;
    void loop1s() override;

    // Synchronous save of every dirty subtree, bypassing the debounce. Same work
    // loop1s does once the debounce expires. Exposed for tests so they can assert
    // the file appears without wall-clock waits; production callers shouldn't need this.
    void flush();

    // Static convenience for callers (e.g. reboot handler) that need to force any
    // debounced saves through before a teardown — mirrors noteDirty's call style.
    static void flushPending();

    // Called by HttpServerModule on every successful control mutation so the
    // 2s debounce starts. Cheap timestamp record; the actual walk happens in loop1s().
    static void noteDirty();

private:
    static inline FilesystemModule* instance_ = nullptr;
    Scheduler* scheduler_ = nullptr;
    bool mounted_ = false;
    bool dirtyPending_ = false;
    bool everSaved_ = false;       // false until the first successful save
    uint32_t lastDirtyMs_ = 0;
    uint32_t lastSaveMs_ = 0;
    char lastSaveStr_[24] = "never";  // "lastSaved" read-only control value
    // Shared load/save buffer — load runs once at boot (phase 2), save runs in loop1s after
    // the 2s debounce. Mutually exclusive, so one buffer is enough. Kept off the task stack
    // since 2KB plus recursive applyNode/writeNode frames is uncomfortably close to the ESP32
    // default task stack ceiling (4–8KB).
    char fileBuf_[MAX_FILE_BYTES] = {};

    // ---- Internals ----
    void updateLastSavedStr();
    static void loadAllHookTrampoline_(Scheduler* s);
    void loadAll(Scheduler* s);
    void migrateRenamedConfigs();
    void loadSubtree(MoonModule* m);
    void applyNode(MoonModule* m, const char* json, const char* prefix);
    void applyValue(const ControlDescriptor& c, const char* json, const char* key);
    bool saveSubtree(MoonModule* m);
    bool writeNode(MoonModule* m, char* buf, size_t bufLen, int& pos, const char* prefix,
                   bool firstField = true);
    bool writeValue(const ControlDescriptor& c, char* buf, size_t bufLen, int& pos);
    static bool subtreeDirty(MoonModule* m);
    static void clearSubtreeDirty(MoonModule* m);
    static bool pathFor(MoonModule* m, char* out, size_t n);
};

} // namespace mm
