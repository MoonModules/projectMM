#include "core/Scheduler.h"

#include "core/Control.h"    // applyControlValue + ApplyResult in setControl
#include "core/JsonUtil.h"   // mm::json::parseBool for the "enabled" pseudo-control
#include "platform/platform.h"

#include <cstdio>   // std::snprintf in ensureUniqueName
#include <cstring>  // std::strcmp in firstInTree

namespace mm {

void Scheduler::addModule(MoonModule* mod) {
    if (!mod || moduleCount_ >= modules_.size()) return;
    modules_[moduleCount_++] = mod;
}

void Scheduler::setup() {
    instance_ = this;   // the one live Scheduler, reachable via Scheduler::instance()
    startTime_ = platform::millis();

    // Phase 1: bind each module's controls. After this, ControlList descriptors hold
    // (name → variable pointer) so the persistence hook can apply file values.
    for (uint8_t i = 0; i < moduleCount_; i++) {
        modules_[i]->onBuildControls();
    }

    // Phase 2: persistence load. No-op if no hook is set.
    if (loadAllHook_) loadAllHook_(this);

    // Phase 2a: disambiguate any same-name modules introduced by persistence
    // (positional load gives each freshly-created module the factory's display
    // name; two Layer instances both get "Layer"). The /api/state UI sends
    // names back as parent_id, so duplicates break "add child to the second
    // one". Walks the tree once; first occurrence keeps the name, later ones
    // get " 2", " 3", … suffixes.
    deduplicateNamesInTree();

    // Phase 2b: re-run onBuildControls with persisted values in place so any conditional
    // hidden flags (e.g. NetworkModule's static-IP fields depending on addressing_) are
    // evaluated against the loaded state, not the default. rebuildControls clears the
    // descriptor list before re-binding, so this is idempotent.
    if (loadAllHook_) {
        for (uint8_t i = 0; i < moduleCount_; i++) {
            modules_[i]->rebuildControls();
        }
    }

    // Phase 3: each module's own init. Persisted values are already in member variables,
    // so e.g. NetworkModule sees the persisted ssid_, SystemModule sees an overlaid
    // deviceName_ (or guards if empty to derive the MAC-based default).
    for (uint8_t i = 0; i < moduleCount_; i++) {
        modules_[i]->setup();
    }

    // Phase 4: allocate buffers sized to final control values.
    for (uint8_t i = 0; i < moduleCount_; i++) {
        modules_[i]->onBuildState();
    }

    lastLoop20ms_ = platform::millis();
    lastLoop1s_ = platform::millis();
    lastTimingUpdate_ = platform::millis();
}

void Scheduler::tick() {
    uint32_t tickStart = platform::micros();
    bool renderedFrame = false;

    uint32_t now = platform::millis();
    const uint32_t targetFrameUs = 1000000u / targetFps_;
    if (lastFrameStartUs_ != 0) {
        const uint32_t nowUs = platform::micros();
        if (static_cast<int32_t>(nowUs - (lastFrameStartUs_ + targetFrameUs)) >= 0) {
            renderedFrame = true;
            lastFrameStartUs_ = nowUs;
        }
    } else {
        renderedFrame = true;
        lastFrameStartUs_ = platform::micros();
    }
    const uint32_t workStartUs = platform::micros();

    // Scheduler gates loop callbacks by `enabled()` — disabled modules don't tick.
    // System modules that need to keep running regardless (HttpServer, Network,
    // Filesystem — so users can re-enable other modules through them) override
    // `respectsEnabled()` to return false. `onEnabled()` fires once per transition
    // for custom start/stop semantics; see MoonModule::setEnabled().
    auto shouldRun = [](MoonModule* m) {
        return !m->respectsEnabled() || m->enabled();
    };
    if (renderedFrame) {
        for (uint8_t i = 0; i < moduleCount_; i++) {
            if (!shouldRun(modules_[i])) continue;
            uint32_t modStart = platform::micros();
            modules_[i]->loop();
            modules_[i]->addAccumUs(platform::micros() - modStart);
        }
    }

    if (now - lastLoop20ms_ >= 20) {
        lastLoop20ms_ = now;
        for (uint8_t i = 0; i < moduleCount_; i++) {
            if (!shouldRun(modules_[i])) continue;
            uint32_t modStart = platform::micros();
            modules_[i]->loop20ms();
            modules_[i]->addAccumUs(platform::micros() - modStart);
        }
    }

    if (!loop1sSweepActive_ && now - lastLoop1s_ >= 1000) {
        lastLoop1s_ = now;
        loop1sSweepActive_ = true;
        loop1sCursor_ = 0;
    }
    if (loop1sSweepActive_) {
        while (loop1sCursor_ < moduleCount_) {
            MoonModule* mod = modules_[loop1sCursor_++];
            if (!shouldRun(mod)) continue;
            uint32_t modStart = platform::micros();
            mod->loop1s();
            mod->addAccumUs(platform::micros() - modStart);
            break;
        }
        if (loop1sCursor_ >= moduleCount_) loop1sSweepActive_ = false;
    }

    if (renderedFrame) frameCount_++;

    // Every 1 second: publish per-module timing. Do this before closing the
    // scheduler timing window so the publish walk itself is counted in maxWorkUs.
    const uint32_t timingNow = platform::millis();
    const bool publishWindow = timingNow - lastTimingUpdate_ >= 1000;
    if (publishWindow) {
        for (uint8_t i = 0; i < moduleCount_; i++) {
            modules_[i]->publishTiming(frameCount_);
        }
    }

    const uint32_t tickEndUs = platform::micros();
    const uint32_t frameElapsedUs = tickEndUs - tickStart;
    const uint32_t workElapsedUs = tickEndUs - workStartUs;
    tickAccumUs_ += frameElapsedUs;
    workAccumUs_ += workElapsedUs;
    tickSampleCount_++;
    if (workElapsedUs > maxWorkAccumUs_) maxWorkAccumUs_ = workElapsedUs;
    if (frameElapsedUs > maxFrameAccumUs_) maxFrameAccumUs_ = frameElapsedUs;

    // Every 1 second: compute scheduler averages/maxima.
    if (publishWindow) {
        tickTimeUs_ = tickSampleCount_ > 0 ? tickAccumUs_ / tickSampleCount_ : 0;
        workTimeUs_ = tickSampleCount_ > 0 ? workAccumUs_ / tickSampleCount_ : 0;
        maxWorkTimeUs_ = maxWorkAccumUs_;
        maxFrameTimeUs_ = maxFrameAccumUs_;
        fps_ = frameCount_;

        tickAccumUs_ = 0;
        workAccumUs_ = 0;
        maxWorkAccumUs_ = 0;
        maxFrameAccumUs_ = 0;
        frameCount_ = 0;
        tickSampleCount_ = 0;
        lastTimingUpdate_ = platform::millis();
    }
}

void Scheduler::teardown() {
    // Two passes: tear down all modules first (so a module's teardown can still safely
    // observe sibling modules' state), then delete the trees. Otherwise the reverse-order
    // teardown-then-delete pattern would leave a module's teardown looking at already-freed
    // siblings — relevant for any cross-module cleanup work.
    for (uint8_t i = moduleCount_; i > 0; i--) {
        modules_[i - 1]->teardown();
    }
    for (uint8_t i = moduleCount_; i > 0; i--) {
        deleteTree(modules_[i - 1]);
    }
    moduleCount_ = 0;
    instance_ = nullptr;
}

uint32_t Scheduler::elapsed() const {
    return platform::millis() - startTime_;
}

void Scheduler::buildState() {
    for (uint8_t i = 0; i < moduleCount_; i++) {
        modules_[i]->onBuildState();
    }
}

void Scheduler::deleteTree(MoonModule* mod) {
    if (!mod) return;
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        deleteTree(mod->child(i));
    }
    delete mod;
}

// Why this exists: ModuleFactory::create gives every freshly-created module
// a display name derived from its type ("NoiseEffect" → "Noise", "Layer"
// stays "Layer"). When the user adds two Layers, both factory-default to
// "Layer"; the HTTP API uses names as parent_id, and findModuleByName does
// a first-match DFS, so the second Layer becomes unreachable. Same problem
// happens when persistence rebuilds the tree positionally on boot.
//
// Called from HttpServerModule after addChild (single-module add) and from
// deduplicateNamesInTree after persistence load (whole-tree pass).
void Scheduler::ensureUniqueName(MoonModule* mod) {
    if (!mod) return;
    const char* base = mod->name();
    if (!base || base[0] == 0) return;
    if (firstByName(base) == mod) return;  // we're the first occurrence — keep the name

    // `candidate` is sized to match MoonModule::name_[16] — there's no point
    // computing a longer name than setName can store. The snprintf check
    // below refuses to truncate, which means the practical cap depends on
    // the base length: 99 for ≤ 5-char bases, 9 for 12–13-char bases like
    // "GlowParticles" or "PlasmaPalette" (where "GlowParticles-10" = 16
    // chars + NUL doesn't fit). When the cap is hit we keep the duplicate
    // name rather than truncate; first-match DFS lookups become ambiguous
    // for that name but the engine doesn't crash. This is unlikely in
    // practice (10+ same-typed siblings on one tree) — bump name_/candidate
    // together if it ever bites.
    //
    // Separator is '-', not a space: the name becomes a URL path segment in the
    // module API (DELETE / replace / move `/api/modules/<name>`); a space there
    // needs URL-encoding and breaks the device's raw-path name lookup, so a
    // device-created "Grid 2" couldn't be deleted. '-' is URL-safe and readable.
    char candidate[16];
    for (int suffix = 2; suffix < 100; suffix++) {
        int n = std::snprintf(candidate, sizeof(candidate), "%s-%d", base, suffix);
        if (n < 0 || n >= static_cast<int>(sizeof(candidate))) return;  // doesn't fit name_
        if (firstByName(candidate) == nullptr) {
            mod->setName(candidate);
            return;
        }
    }
    // Loop exhausted (would mean 99 same-named siblings) — degrade silently.
}

void Scheduler::deduplicateNamesInTree() {
    for (uint8_t i = 0; i < moduleCount_; i++) {
        walkAndEnsureUnique(modules_[i]);
    }
}

MoonModule* Scheduler::firstByName(const char* name) {
    if (!name || name[0] == 0) return nullptr;   // firstInTree strcmps name; a null would be UB
    for (uint8_t i = 0; i < moduleCount_; i++) {
        if (auto* m = firstInTree(modules_[i], name)) return m;
    }
    return nullptr;
}

Scheduler::SetControlResult Scheduler::setControl(const char* moduleName,
                                                 const char* controlName,
                                                 const char* valueJson) {
    MoonModule* target = firstByName(moduleName);
    if (!target) return SetControlResult::ModuleNotFound;

    // Module-level "enabled" pseudo-control — toggles the flag, then a full rebuild so the
    // disabled subtree stops/starts ticking.
    if (std::strcmp(controlName, "enabled") == 0) {
        target->setEnabled(mm::json::parseBool(valueJson, "value"));
        target->markDirty();
        if (noteDirtyHook_) noteDirtyHook_();
        buildState();
        return SetControlResult::Ok;
    }

    auto& ctrls = target->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        auto& c = ctrls[i];
        if (std::strcmp(c.name, controlName) != 0) continue;

        // Per-type parse + validate + apply lives in Control.cpp. A non-Ok result leaves
        // the storage untouched, so there is no rollback to do.
        switch (applyControlValue(c, valueJson, "value")) {
            case ApplyResult::Ok:         break;
            case ApplyResult::OutOfRange: return SetControlResult::OutOfRange;
            case ApplyResult::Malformed:  return SetControlResult::Malformed;
            case ApplyResult::ReadOnly:   return SetControlResult::ReadOnly;
        }
        // Rebuild the control list so onBuildControls() re-evaluates conditional visibility
        // for the new value; fire the three-tier change reaction (onUpdate always, a
        // tree-wide buildState only when the control reshapes dims/mapping); persist.
        target->rebuildControls();
        target->onUpdate(controlName);
        target->markDirty();
        if (noteDirtyHook_) noteDirtyHook_();
        if (target->controlChangeTriggersBuildState(controlName)) buildState();
        return SetControlResult::Ok;
    }
    return SetControlResult::ControlNotFound;
}

void Scheduler::walkAndEnsureUnique(MoonModule* mod) {
    if (!mod) return;
    ensureUniqueName(mod);
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        walkAndEnsureUnique(mod->child(i));
    }
}

MoonModule* Scheduler::firstInTree(MoonModule* mod, const char* name) {
    if (!mod) return nullptr;
    if (mod->name() && std::strcmp(mod->name(), name) == 0) return mod;
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        if (auto* m = firstInTree(mod->child(i), name)) return m;
    }
    return nullptr;
}

} // namespace mm
