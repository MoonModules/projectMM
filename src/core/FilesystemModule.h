#pragma once

// FilesystemModule — control-list-driven JSON persistence.
//
// Storage: one flat JSON file per top-level MoonModule under /.config/<TypeName>.json.
// Children are encoded with "<index>." key prefix (positional). Parser is the existing
// flat parseJsonString/Int/Bool from core/JsonUtil.h — no nested objects, no arrays.
//
// Boot flow:
//   Scheduler phase 1: onBuildControls (every module binds full control set incl. hidden ones)
//   Scheduler phase 2: this module's loadAllHook reads each file and overlays bound variables
//   Scheduler phase 3: modules' own setup() runs with persisted values in member vars
//   Scheduler phase 4: onAllocateMemory
//
// Save flow:
//   HttpServerModule::handleSetControl calls target->markDirty() on every mutation
//   This module's loop1s() debounces 2s, walks the tree, writes any dirty subtree atomically

#include "core/MoonModule.h"
#include "core/ModuleFactory.h"
#include "core/Scheduler.h"
#include "core/JsonUtil.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

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
    ~FilesystemModule() override { if (instance_ == this) instance_ = nullptr; }

    // Persistence must keep flushing dirty subtrees regardless of the `enabled` toggle —
    // otherwise the user could lose changes by accidentally disabling this module via
    // the UI before the 2s debounce expires.
    bool respectsEnabled() const override { return false; }

    void setScheduler(Scheduler* s) {
        scheduler_ = s;
        instance_ = this;
        if (s) s->setLoadAllHook(&loadAllHookTrampoline_);
    }

    void setup() override {
        if (!platform::fsMount()) {
            std::printf("FilesystemModule: mount failed — persistence disabled\n");
            return;
        }
        mounted_ = true;
        platform::fsMkdir(CONFIG_DIR);
        std::printf("FilesystemModule: mounted, %zu / %zu bytes used\n",
                    platform::filesystemUsed(), platform::filesystemTotal());
    }

    void loop1s() override {
        if (!mounted_ || !scheduler_) return;
        if (!dirtyPending_) return;
        if (platform::millis() - lastDirtyMs_ < DEBOUNCE_MS) return;
        flush();
    }

    // Synchronous save of every dirty subtree, bypassing the debounce. Same work loop1s
    // does once the debounce expires. Exposed for tests so they can assert the file appears
    // without wall-clock waits; production callers shouldn't need this.
    void flush() {
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
                } else {
                    allSaved = false;
                }
            }
        }
        // Keep dirtyPending_ set if anything failed, so loop1s retries.
        dirtyPending_ = !allSaved;
    }

    // FilesystemModule polls dirty flags in loop1s; modules don't call us directly.
    // markDirty() on MoonModule (set by HttpServerModule) is the only producer.
    // We just need to know "something is dirty" — checked by walking the tree in loop1s.
    // To avoid walking the tree every loop1s when nothing is dirty, the noteDirty() static
    // is called by the same HttpServerModule path (cheap timestamp record).
    // Static convenience for callers (e.g. reboot handler) that need to force any
    // debounced saves through before a teardown — mirrors noteDirty's call style.
    static void flushPending() {
        if (instance_) instance_->flush();
    }

    static void noteDirty() {
        if (!instance_) return;
        instance_->lastDirtyMs_ = platform::millis();
        instance_->dirtyPending_ = true;
    }

private:
    static inline FilesystemModule* instance_ = nullptr;
    Scheduler* scheduler_ = nullptr;
    bool mounted_ = false;
    bool dirtyPending_ = false;
    uint32_t lastDirtyMs_ = 0;
    uint32_t lastSaveMs_ = 0;
    // Shared load/save buffer — load runs once at boot (phase 2), save runs in loop1s after
    // the 2s debounce. Mutually exclusive, so one buffer is enough. Kept off the task stack
    // since 2KB plus recursive applyNode/writeNode frames is uncomfortably close to the ESP32
    // default task stack ceiling (4–8KB).
    char fileBuf_[MAX_FILE_BYTES] = {};

    // ---- Scheduler hook trampoline (C-style for typedef compatibility) ----
    static void loadAllHookTrampoline_(Scheduler* s) {
        if (instance_) instance_->loadAll(s);
    }

    void loadAll(Scheduler* s) {
        if (!mounted_) {
            // setup() hasn't run yet (we're in phase 2, before phase 3 setup). Mount now
            // so we can read; setup() later calls fsMount again (idempotent).
            if (!platform::fsMount()) return;
            mounted_ = true;
            platform::fsMkdir(CONFIG_DIR);
        }
        for (uint8_t i = 0; i < s->moduleCount(); i++) {
            MoonModule* m = s->module(i);
            if (!m || m == this) continue;
            loadSubtree(m);
        }
    }

    // ---- Load ----
    void loadSubtree(MoonModule* m) {
        char path[MAX_PATH];
        if (!pathFor(m, path, sizeof(path))) return;
        int n = platform::fsRead(path, fileBuf_, sizeof(fileBuf_));
        if (n <= 0) return;
        // fsRead doesn't NUL-terminate; applyNode parses fileBuf_ as a C-string.
        fileBuf_[n < static_cast<int>(sizeof(fileBuf_)) ? n : static_cast<int>(sizeof(fileBuf_)) - 1] = '\0';
        applyNode(m, fileBuf_, "");
    }

    void applyNode(MoonModule* m, const char* json, const char* prefix) {
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

    void applyValue(const ControlDescriptor& c, const char* json, const char* key) {
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
    bool saveSubtree(MoonModule* m) {
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
    bool writeNode(MoonModule* m, char* buf, size_t bufLen, int& pos, const char* prefix,
                   bool firstField = true) {
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
    static bool writeJsonString(const char* s, char* buf, size_t bufLen, int& pos) {
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

    bool writeValue(const ControlDescriptor& c, char* buf, size_t bufLen, int& pos) {
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
    static bool subtreeDirty(MoonModule* m) {
        if (!m) return false;
        if (m->dirty()) return true;
        for (uint8_t i = 0; i < m->childCount(); i++) {
            if (subtreeDirty(m->child(i))) return true;
        }
        return false;
    }
    static void clearSubtreeDirty(MoonModule* m) {
        if (!m) return;
        m->clearDirty();
        for (uint8_t i = 0; i < m->childCount(); i++) clearSubtreeDirty(m->child(i));
    }

    // ---- Paths ----
    // Filename = "/.config/<TypeName>.json". Single instance assumed; multi-instance gets a
    // .N suffix when that becomes a requirement (item 12 — module switching).
    static bool pathFor(MoonModule* m, char* out, size_t n) {
        if (!m || m->typeName()[0] == 0) return false;
        int w = std::snprintf(out, n, "%s/%s.json", CONFIG_DIR, m->typeName());
        return w > 0 && static_cast<size_t>(w) < n;
    }
};

} // namespace mm
