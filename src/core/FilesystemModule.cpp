#include "core/FilesystemModule.h"

#include "core/Control.h"
#include "core/JsonSink.h"   // fixed-buffer mode used by writeValue()
#include "core/JsonUtil.h"
#include "core/ModuleFactory.h"
#include "core/Scheduler.h"
#include "platform/platform.h"

#include <climits>  // INT16_MIN/MAX in applyValue's Int16 clamp
#include <cstdio>
#include <cstring>

namespace mm {

FilesystemModule::~FilesystemModule() {
    if (instance_ == this) instance_ = nullptr;
}

void FilesystemModule::setScheduler(Scheduler* s) {
    scheduler_ = s;
    instance_ = this;
    if (s) s->setLoadAllHook(&loadAllHookTrampoline_);
}

void FilesystemModule::setup() {
    if (!platform::fsMount()) {
        std::printf("FilesystemModule: mount failed — persistence disabled\n");
        return;
    }
    mounted_ = true;
    platform::fsMkdir(CONFIG_DIR);
    std::printf("FilesystemModule: mounted, %zu / %zu bytes used\n",
                platform::filesystemUsed(), platform::filesystemTotal());
}

void FilesystemModule::onBuildControls() {
    controls_.addReadOnly("lastSaved", lastSaveStr_, sizeof(lastSaveStr_));
    // Filesystem-partition usage bar (bytes used / total). Lives here — on the module that owns the
    // filesystem — not on SystemModule. Read the total once; loop1s refreshes the used value. Bound
    // only when the platform reports a real partition (desktop / a chip without a data partition
    // reports 0, so the bar is omitted rather than showing 0/0).
    totalFsVal_ = static_cast<uint32_t>(platform::filesystemTotal());
    fsUsedVal_ = static_cast<uint32_t>(platform::filesystemUsed());
    if (totalFsVal_ > 0) {
        controls_.addProgress("filesystem", fsUsedVal_, totalFsVal_);
    }
    MoonModule::onBuildControls();
}

void FilesystemModule::loop1s() {
    // Refresh the usage bar first — cheap, and it should track saves even before the mount/scheduler
    // guards below (the total is fixed; only the used value moves as files are written).
    if (totalFsVal_ > 0) fsUsedVal_ = static_cast<uint32_t>(platform::filesystemUsed());
    if (!mounted_ || !scheduler_) return;
    updateLastSavedStr();
    if (!dirtyPending_) return;
    if (platform::millis() - lastDirtyMs_ < DEBOUNCE_MS) return;
    flush();
}

// Refresh the "lastSaved" display string — "never" before the first save,
// otherwise how long ago the last successful write happened.
void FilesystemModule::updateLastSavedStr() {
    if (!everSaved_) {
        std::snprintf(lastSaveStr_, sizeof(lastSaveStr_), "never");
        return;
    }
    uint32_t agoSec = (platform::millis() - lastSaveMs_) / 1000;
    if (agoSec < 60) {
        std::snprintf(lastSaveStr_, sizeof(lastSaveStr_), "%us ago",
                      static_cast<unsigned>(agoSec));
    } else if (agoSec < 3600) {
        std::snprintf(lastSaveStr_, sizeof(lastSaveStr_), "%um ago",
                      static_cast<unsigned>(agoSec / 60));
    } else {
        std::snprintf(lastSaveStr_, sizeof(lastSaveStr_), "%uh ago",
                      static_cast<unsigned>(agoSec / 3600));
    }
}

void FilesystemModule::flush() {
    if (!mounted_ || !scheduler_) return;
    bool allSaved = true;
    for (uint8_t i = 0; i < scheduler_->moduleCount(); i++) {
        MoonModule* m = scheduler_->module(i);
        if (!m || m == this) continue;
        if (subtreeDirty(m)) {
            // Only clear the dirty flag when the write actually succeeded —
            // otherwise a failed write would silently drop the pending change.
            if (saveSubtree(m)) {
                clearSubtreeDirty(m);
                lastSaveMs_ = platform::millis();
                everSaved_ = true;
            } else {
                allSaved = false;
            }
        }
    }
    // Keep dirtyPending_ set if anything failed, so loop1s retries.
    dirtyPending_ = !allSaved;
}

void FilesystemModule::flushPending() {
    if (instance_) instance_->flush();
}

void FilesystemModule::noteDirty() {
    if (!instance_) return;
    instance_->lastDirtyMs_ = platform::millis();
    instance_->dirtyPending_ = true;
}

// ---- Scheduler hook trampoline (C-style for typedef compatibility) ----
void FilesystemModule::loadAllHookTrampoline_(Scheduler* s) {
    if (instance_) instance_->loadAll(s);
}

void FilesystemModule::loadAll(Scheduler* s) {
    if (!mounted_) {
        // setup() hasn't run yet (we're in phase 2, before phase 3 setup). Mount now
        // so we can read; setup() later calls fsMount again (idempotent).
        if (!platform::fsMount()) return;
        mounted_ = true;
        platform::fsMkdir(CONFIG_DIR);
    }
    migrateRenamedConfigs();
    for (uint8_t i = 0; i < s->moduleCount(); i++) {
        MoonModule* m = s->module(i);
        if (!m || m == this) continue;
        loadSubtree(m);
    }
}

// One-time cleanup of files whose owning type was renamed. Each migration is
// delete-and-warn — per-container controls today are limited to `enabled`
// (near-zero loss). A future rename can either grow this list or, if the
// settings volume gets non-trivial, become a rename-the-file step.
//
// **Domain-boundary trade-off (intentional, time-bounded).** The strings
// below are light-domain type names embedded in a core module, which
// CLAUDE.md's "domain-neutral core" rule discourages. We accept the leak
// because (a) the alternative — a `MoonModule::registerRenamedConfig()`
// API the light domain calls into — is more abstraction than two entries
// justify, and (b) this code's natural lifetime is one or two release
// cycles (after that everyone's `.config` is fresh and the entries become
// dead code). **Remove these entries** the next time the `next-iteration`
// branch is merged to `main` and a release is cut. If the list grows
// beyond ~5 entries before then, reach for option (a) instead.
void FilesystemModule::migrateRenamedConfigs() {
    struct Renamed { const char* oldFile; const char* newType; };
    static constexpr Renamed kRenamed[] = {
        {"/.config/LayoutGroup.json", "Layouts"},
        {"/.config/DriverGroup.json", "Drivers"},
    };
    for (const auto& r : kRenamed) {
        if (platform::fsExists(r.oldFile)) {
            std::printf("FilesystemModule: removing stale %s "
                        "(type was renamed to %s) — its previous values are lost\n",
                        r.oldFile, r.newType);
            platform::fsRemove(r.oldFile);
        }
    }
}

// ---- Load ----
void FilesystemModule::loadSubtree(MoonModule* m) {
    char path[MAX_PATH];
    if (!pathFor(m, path, sizeof(path))) return;
    int n = platform::fsRead(path, fileBuf_, sizeof(fileBuf_));
    if (n <= 0) return;
    // fsRead doesn't NUL-terminate; applyNode parses fileBuf_ as a C-string.
    fileBuf_[n < static_cast<int>(sizeof(fileBuf_)) ? n : static_cast<int>(sizeof(fileBuf_)) - 1] = '\0';
    applyNode(m, fileBuf_, "");
}

void FilesystemModule::applyNode(MoonModule* m, const char* json, const char* prefix) {
    char key[MAX_KEY];
    auto& cs = m->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        auto& c = cs[i];
        if (!isPersistable(c.type)) continue;
        std::snprintf(key, sizeof(key), "%s%s", prefix, c.name);
        applyValue(c, json, key);
    }
    std::snprintf(key, sizeof(key), "%senabled", prefix);
    // Note: we can't distinguish "key absent" from "key=false" with the flat parser.
    // The convention: every saved file includes "enabled", so if the file exists and
    // applyNode is reached we assume the key is present. Production callers always
    // emit enabled (see writeNode). If the user hand-edited the file and dropped it,
    // they get enabled=false (matches the default-after-bad-edit behavior).
    m->setEnabled(mm::json::parseBool(json, key));

    // Reconcile children with the JSON's tree shape. For each position, look up
    // "<prefix><idx>.type"; if it differs from the live child (or no live child
    // exists), factory-create the JSON type and place it at that position. The
    // newly-created child gets onBuildControls() here so the recursive applyNode
    // below can overlay its persisted values. Phases 3+4 (setup, onBuildState)
    // cascade into the new child automatically.
    // Walk JSON child positions in order; stop when "<idx>.type" is absent. No fixed cap —
    // the JSON itself terminates the loop. childCount_ is a uint8_t so the practical ceiling
    // is 255 children per parent, far above any realistic tree.
    uint8_t jsonChildCount = 0;
    for (uint8_t i = 0; ; i++) {
        char typeKey[MAX_KEY];
        std::snprintf(typeKey, sizeof(typeKey), "%s%u.type", prefix, static_cast<unsigned>(i));
        char typeName[32] = {};
        mm::json::parseString(json, typeKey, typeName, sizeof(typeName));
        if (typeName[0] == 0) break;

        MoonModule* live = m->child(i);
        if (!live || std::strcmp(live->typeName(), typeName) != 0) {
            // Position-replace can also destroy a code-wired child if the file
            // describes a different type at this slot. Bail out of further
            // reconciliation rather than killing it — the trim loop below then
            // preserves the code-wired tail, and the next save will rewrite the
            // file with the current (correct) tree shape. The rest of the JSON
            // past this position is dropped on this boot; that's better than
            // losing a code-wired child.
            if (live && live->isWiredByCode()) break;
            MoonModule* created = ModuleFactory::create(typeName);
            if (!created) {
                // Factory failed (type not registered). Stop here so subsequent JSON
                // children don't get applied to misaligned live slots; jsonChildCount
                // stays at the last successfully reconciled position, and the trim loop
                // below removes any live children past that point.
                break;
            }
            created->onBuildControls();
            if (live) {
                MoonModule* old = m->replaceChildAt(i, created);
                if (old) { old->teardown(); Scheduler::deleteTree(old); }
            } else {
                m->addChild(created);
            }
        }

        jsonChildCount = i + 1;
        char childPrefix[MAX_KEY];
        std::snprintf(childPrefix, sizeof(childPrefix), "%s%u.", prefix, static_cast<unsigned>(i));
        applyNode(m->child(i), json, childPrefix);
    }
    // Trim live children beyond what the JSON describes, EXCEPT children that
    // were wired by code at boot (main.cpp annotates those via markWiredByCode).
    // A code-wired child is preserved across persistence loads even when the
    // on-disk file predates its addition — the upgrade-day case where a new
    // release adds a code-created child (e.g. ImprovProvisioningModule under
    // NetworkModule) whose existence the device's saved file doesn't yet know
    // about. Without this exemption the child would get trimmed on every boot.
    //
    // Walks back-to-front so removeChild's left-shift of later siblings doesn't
    // skip an entry. Any code-wired child at index >= jsonChildCount stays; its
    // position relative to the JSON-described children may not match what the
    // file expects, but on the first dirty event the next save writes the
    // current (post-merge) tree shape and from then on the file matches.
    uint8_t i = m->childCount();
    while (i > jsonChildCount) {
        i--;
        MoonModule* extra = m->child(i);
        if (!extra) continue;
        if (extra->isWiredByCode()) continue;
        extra->teardown();
        m->removeChild(extra);
        Scheduler::deleteTree(extra);
    }
}

void FilesystemModule::applyValue(const ControlDescriptor& c, const char* json, const char* key) {
    // Per-type parse + validate + apply lives in Control.cpp. Use Clamp:
    // a stale on-disk value from a schema change should snap to the new
    // bounds (Uint8 200 → max 100), not silently drop to 0. The HTTP API
    // uses Strict instead so a bogus client value surfaces as a 400.
    (void)applyControlValue(c, json, key, ApplyPolicy::Clamp);
}

// ---- Save ----
// Returns true only when the file was written. On failure (path/overflow/write
// error) the caller must keep the subtree dirty so the change isn't lost.
bool FilesystemModule::saveSubtree(MoonModule* m) {
    char path[MAX_PATH];
    if (!pathFor(m, path, sizeof(path))) return false;
    int pos = std::snprintf(fileBuf_, sizeof(fileBuf_), "{");
    if (pos < 0) return false;
    if (!writeNode(m, fileBuf_, sizeof(fileBuf_), pos, "")) {
        std::printf("FilesystemModule: subtree too large for %s\n", path);
        return false;
    }
    int n = std::snprintf(fileBuf_ + pos, sizeof(fileBuf_) - pos, "}");
    if (n < 0 || static_cast<size_t>(pos + n) >= sizeof(fileBuf_)) return false;
    pos += n;
    if (platform::fsWriteAtomic(path, fileBuf_, static_cast<size_t>(pos))) {
        std::printf("FilesystemModule: saved %s (%d bytes)\n", path, pos);
        return true;
    }
    std::printf("FilesystemModule: write failed for %s\n", path);
    return false;
}

// Returns false on overflow. `firstField` is true when this writeNode is the first
// field-emitter inside its containing `{` — the top-level call passes true, the
// recursive child call passes false because the parent already emitted its `"N.type"`
// field and the child must therefore prefix a comma before its first control.
bool FilesystemModule::writeNode(MoonModule* m, char* buf, size_t bufLen, int& pos, const char* prefix,
                                 bool firstField) {
    bool first = firstField;
    auto& cs = m->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        auto& c = cs[i];
        if (!isPersistable(c.type)) continue;
        int n = std::snprintf(buf + pos, bufLen - pos, "%s\"%s%s\":", first ? "" : ",", prefix, c.name);
        if (n < 0 || static_cast<size_t>(pos + n) >= bufLen) return false;
        pos += n;
        if (!writeValue(c, buf, bufLen, pos)) return false;
        first = false;
    }
    int n = std::snprintf(buf + pos, bufLen - pos, "%s\"%senabled\":%s",
                          first ? "" : ",", prefix, m->enabled() ? "true" : "false");
    if (n < 0 || static_cast<size_t>(pos + n) >= bufLen) return false;
    pos += n;
    for (uint8_t i = 0; i < m->childCount(); i++) {
        MoonModule* child = m->child(i);
        if (!child) continue;  // addChild rejects nullptr today; defend against future invariants
        char childPrefix[MAX_KEY];
        std::snprintf(childPrefix, sizeof(childPrefix), "%s%u.", prefix, static_cast<unsigned>(i));
        // Emit "0.type":"NoiseEffect" so the reader can detect tree-shape mismatches.
        n = std::snprintf(buf + pos, bufLen - pos, ",\"%stype\":\"%s\"",
                          childPrefix, child->typeName());
        if (n < 0 || static_cast<size_t>(pos + n) >= bufLen) return false;
        pos += n;
        if (!writeNode(child, buf, bufLen, pos, childPrefix, /*firstField=*/false)) return false;
    }
    return true;
}

bool FilesystemModule::writeValue(const ControlDescriptor& c, char* buf, size_t bufLen, int& pos) {
    // Bridge into the shared serializer via JsonSink's fixed-buffer mode:
    // writeControlValue (in Control.cpp) writes through the JsonSink API,
    // which writes into our slice and flips overflowed_ if we run out of
    // capacity. Matches the prior overflow-returns-false contract.
    if (pos < 0 || static_cast<size_t>(pos) >= bufLen) return false;
    JsonSink local(buf + pos, bufLen - static_cast<size_t>(pos));
    writeControlValue(local, c);
    if (local.overflowed()) return false;
    pos += static_cast<int>(local.size());
    return true;
}

// ---- Dirty walking ----
bool FilesystemModule::subtreeDirty(MoonModule* m) {
    if (!m) return false;
    if (m->dirty()) return true;
    for (uint8_t i = 0; i < m->childCount(); i++) {
        if (subtreeDirty(m->child(i))) return true;
    }
    return false;
}

void FilesystemModule::clearSubtreeDirty(MoonModule* m) {
    if (!m) return;
    m->clearDirty();
    for (uint8_t i = 0; i < m->childCount(); i++) clearSubtreeDirty(m->child(i));
}

// ---- Paths ----
// Filename = "/.config/<TypeName>.json". Single instance assumed; multi-instance gets a
// .N suffix when that becomes a requirement (item 12 — module switching).
bool FilesystemModule::pathFor(MoonModule* m, char* out, size_t n) {
    if (!m || m->typeName()[0] == 0) return false;
    int w = std::snprintf(out, n, "%s/%s.json", CONFIG_DIR, m->typeName());
    return w > 0 && static_cast<size_t>(w) < n;
}

} // namespace mm
