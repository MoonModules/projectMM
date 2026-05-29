#pragma once

// Scheduler — owns the top-level module list, runs the 4-phase boot, drives the
// per-tick loop callbacks, and provides tree-walk utilities (delete, name-uniquify).
//
// Boot phases: see setup() comment in Scheduler.cpp.
// Tick: gates each top-level module by enabled() / respectsEnabled() and dispatches
//   loop / loop20ms / loop1s. Per-second window averages the tick time and publishes
//   each module's loop time.
//
// This is the .h interface. Bodies live in Scheduler.cpp.

#include "core/MoonModule.h"

#include <array>
#include <cstdint>

namespace mm {

class Scheduler {
public:
    // Function-pointer hook invoked between phase 1 (onBuildControls) and phase 3 (setup).
    // Used by FilesystemModule to overlay persisted control values onto bound variables
    // before modules' setup() runs. Scheduler stays independent of FilesystemModule's type
    // (no circular include). Wired in via setLoadAllHook from main.cpp.
    using LoadAllFn = void(*)(Scheduler*);
    void setLoadAllHook(LoadAllFn fn) { loadAllHook_ = fn; }

    void addModule(MoonModule* mod);
    void setup();
    void tick();
    void teardown();

    uint32_t elapsed() const;
    void buildState();

    uint32_t tickTimeUs() const { return tickTimeUs_; }
    uint32_t fps() const { return tickTimeUs_ > 0 ? 1000000 / tickTimeUs_ : 0; }
    uint8_t moduleCount() const { return moduleCount_; }
    MoonModule* module(uint8_t i) const { return i < moduleCount_ ? modules_[i] : nullptr; }

    static void deleteTree(MoonModule* mod);

    // Ensure `mod`'s name is tree-globally unique. If something else already uses
    // the same name, suffix with " 2", " 3", … until unique. Caller must have
    // placed `mod` in the tree already (otherwise the lookup wouldn't see it).
    // See Scheduler.cpp for the why and the name-length cap.
    void ensureUniqueName(MoonModule* mod);

    // Walk the whole tree and disambiguate every duplicated name. First
    // occurrence keeps its name; later ones get " 2", " 3", … suffixes.
    // Cold-path: called once after persistence load in setup().
    void deduplicateNamesInTree();

    // First module in tree-walk order with this name, or nullptr if none.
    MoonModule* firstByName(const char* name);

private:
    void walkAndEnsureUnique(MoonModule* mod);
    static MoonModule* firstInTree(MoonModule* mod, const char* name);

    std::array<MoonModule*, 32> modules_{};
    uint8_t moduleCount_ = 0;
    LoadAllFn loadAllHook_ = nullptr;
    uint32_t startTime_ = 0;
    uint32_t lastLoop20ms_ = 0;
    uint32_t lastLoop1s_ = 0;
    uint32_t tickTimeUs_ = 0;
    uint32_t tickAccumUs_ = 0;
    uint32_t frameCount_ = 0;        // frames in current 1-second window (for averaging)
    uint32_t lastTimingUpdate_ = 0;   // 1-second window start
};

} // namespace mm
