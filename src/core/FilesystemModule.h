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

    FilesystemModule() { instance_ = this; }
    ~FilesystemModule() override { if (instance_ == this) instance_ = nullptr; }

    void setScheduler(Scheduler* s) {
        scheduler_ = s;
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

        for (uint8_t i = 0; i < scheduler_->moduleCount(); i++) {
            MoonModule* m = scheduler_->module(i);
            if (!m || m == this) continue;
            if (subtreeDirty(m)) {
                saveSubtree(m);
                clearSubtreeDirty(m);
                lastSaveMs_ = platform::millis();
            }
        }
        dirtyPending_ = false;
    }

    // FilesystemModule polls dirty flags in loop1s; modules don't call us directly.
    // markDirty() on MoonModule (set by HttpServerModule) is the only producer.
    // We just need to know "something is dirty" — checked by walking the tree in loop1s.
    // To avoid walking the tree every loop1s when nothing is dirty, the noteDirty() static
    // is called by the same HttpServerModule path (cheap timestamp record).
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
        char buf[MAX_FILE_BYTES];
        int n = platform::fsRead(path, buf, sizeof(buf));
        if (n <= 0) return;
        applyNode(m, buf, "");
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
            jsonChildCount = i + 1;

            MoonModule* live = m->child(i);
            if (!live || std::strcmp(live->typeName(), typeName) != 0) {
                MoonModule* created = ModuleFactory::create(typeName);
                if (!created) continue;
                created->onBuildControls();
                if (live) {
                    MoonModule* old = m->replaceChildAt(i, created);
                    if (old) { old->teardown(); Scheduler::deleteTree(old); }
                } else {
                    m->addChild(created);
                }
            }

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
            case ControlType::Text: {
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
    void saveSubtree(MoonModule* m) {
        char path[MAX_PATH];
        if (!pathFor(m, path, sizeof(path))) return;
        char buf[MAX_FILE_BYTES];
        int pos = std::snprintf(buf, sizeof(buf), "{");
        if (pos < 0) return;
        if (!writeNode(m, buf, sizeof(buf), pos, "")) {
            std::printf("FilesystemModule: subtree too large for %s\n", path);
            return;
        }
        int n = std::snprintf(buf + pos, sizeof(buf) - pos, "}");
        if (n < 0 || static_cast<size_t>(pos + n) >= sizeof(buf)) return;
        pos += n;
        if (platform::fsWriteAtomic(path, buf, static_cast<size_t>(pos))) {
            std::printf("FilesystemModule: saved %s (%d bytes)\n", path, pos);
        } else {
            std::printf("FilesystemModule: write failed for %s\n", path);
        }
    }

    // Returns false on overflow.
    bool writeNode(MoonModule* m, char* buf, size_t bufLen, int& pos, const char* prefix) {
        bool first = true;
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
            char childPrefix[MAX_KEY];
            std::snprintf(childPrefix, sizeof(childPrefix), "%s%u.", prefix, static_cast<unsigned>(i));
            // Emit "0.type":"NoiseEffect" so the reader can detect tree-shape mismatches.
            n = std::snprintf(buf + pos, bufLen - pos, ",\"%stype\":\"%s\"",
                              childPrefix, m->child(i)->typeName());
            if (n < 0 || static_cast<size_t>(pos + n) >= bufLen) return false;
            pos += n;
            if (!writeNode(m->child(i), buf, bufLen, pos, childPrefix)) return false;
        }
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
                n = std::snprintf(buf + pos, bufLen - pos, "\"%s\"",
                                  static_cast<char*>(c.ptr));
                break;
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
