#include "core/FilesystemModule.h"

#include "core/Control.h"
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
    MoonModule::onBuildControls();
}

void FilesystemModule::loop1s() {
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
        if (c.type == ControlType::ReadOnly || c.type == ControlType::Progress) continue;
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
    // below can overlay its persisted values. Phases 3+4 (setup, onAllocateMemory)
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
    // Trim any live children beyond what the JSON describes.
    while (m->childCount() > jsonChildCount) {
        MoonModule* extra = m->child(m->childCount() - 1);
        if (!extra) break;
        extra->teardown();
        m->removeChild(extra);
        Scheduler::deleteTree(extra);
    }
}

void FilesystemModule::applyValue(const ControlDescriptor& c, const char* json, const char* key) {
    switch (c.type) {
        case ControlType::Uint8: {
            int v = mm::json::parseInt(json, key);
            if (v < c.min) v = c.min;
            if (v > c.max) v = c.max;
            *static_cast<uint8_t*>(c.ptr) = static_cast<uint8_t>(v);
            break;
        }
        case ControlType::Uint16: {
            int v = mm::json::parseInt(json, key);
            *static_cast<uint16_t*>(c.ptr) = static_cast<uint16_t>(v);
            break;
        }
        case ControlType::Int16: {
            int v = mm::json::parseInt(json, key);
            // Clamp before narrowing — parseInt returns int (up to ±2^31).
            // A persisted-or-corrupted JSON value outside int16 range
            // would otherwise wrap (e.g. 40000 → -25536). No c.min/c.max
            // clamp here: those fields are uint8_t and can't bound an int16
            // range, so applying them zeros every Int16 control on load.
            if (v < INT16_MIN) v = INT16_MIN;
            if (v > INT16_MAX) v = INT16_MAX;
            *static_cast<int16_t*>(c.ptr) = static_cast<int16_t>(v);
            break;
        }
        case ControlType::Bool:
            *static_cast<bool*>(c.ptr) = mm::json::parseBool(json, key);
            break;
        case ControlType::Text:
        case ControlType::Password: {
            // Password persists to disk like Text — the leak that mattered
            // was the network API, not local flash.
            uint8_t maxLen = c.max > 0 ? c.max : 16;
            mm::json::parseString(json, key, static_cast<char*>(c.ptr), maxLen);
            break;
        }
        case ControlType::Select: {
            int v = mm::json::parseInt(json, key);
            if (v < 0) v = 0;
            if (c.max > 0 && v >= c.max) v = c.max - 1;
            *static_cast<uint8_t*>(c.ptr) = static_cast<uint8_t>(v);
            break;
        }
        default: break;
    }
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
        if (c.type == ControlType::ReadOnly || c.type == ControlType::Progress) continue;
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

// Emit a JSON string literal (with surrounding quotes) for `s`, escaping the
// two characters that would otherwise break JSON: " and \. Returns false if
// the value (plus quotes/escapes) does not fit the remaining buffer.
bool FilesystemModule::writeJsonString(const char* s, char* buf, size_t bufLen, int& pos) {
    if (static_cast<size_t>(pos) >= bufLen) return false;
    buf[pos++] = '"';
    for (; *s; s++) {
        char c = *s;
        bool escape = (c == '"' || c == '\\');
        if (static_cast<size_t>(pos) + (escape ? 2 : 1) >= bufLen) return false;
        if (escape) buf[pos++] = '\\';
        buf[pos++] = c;
    }
    if (static_cast<size_t>(pos) + 1 >= bufLen) return false;
    buf[pos++] = '"';
    return true;
}

bool FilesystemModule::writeValue(const ControlDescriptor& c, char* buf, size_t bufLen, int& pos) {
    int n = 0;
    switch (c.type) {
        case ControlType::Uint8:
            n = std::snprintf(buf + pos, bufLen - pos, "%u",
                              *static_cast<uint8_t*>(c.ptr));
            break;
        case ControlType::Uint16:
            n = std::snprintf(buf + pos, bufLen - pos, "%u",
                              *static_cast<uint16_t*>(c.ptr));
            break;
        case ControlType::Int16:
            n = std::snprintf(buf + pos, bufLen - pos, "%d",
                              *static_cast<int16_t*>(c.ptr));
            break;
        case ControlType::Bool:
            n = std::snprintf(buf + pos, bufLen - pos, "%s",
                              *static_cast<bool*>(c.ptr) ? "true" : "false");
            break;
        case ControlType::Text:
        case ControlType::Password:
            // Escape " and \ so a value like My"SSID can't produce malformed
            // JSON. Returns false on buffer overflow.
            return writeJsonString(static_cast<const char*>(c.ptr), buf, bufLen, pos);
        case ControlType::Select:
            n = std::snprintf(buf + pos, bufLen - pos, "%u",
                              *static_cast<uint8_t*>(c.ptr));
            break;
        default:
            n = std::snprintf(buf + pos, bufLen - pos, "null");
            break;
    }
    if (n < 0 || static_cast<size_t>(pos + n) >= bufLen) return false;
    pos += n;
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
