#include "core/FileManagerModule.h"

#include "core/FilesystemModule.h"   // instance()->lastSavedStr() for the "last saved" readout
#include "platform/platform.h"       // fs* primitives

namespace mm {

namespace {
constexpr uint32_t kUsageRefreshMs = 5u * 60u * 1000u;
}

void FileManagerModule::onBuildControls() {
    // Only `show hidden` is a control: the whole File Manager surface is the tree panel (app.js
    // renderFileManager), which lists over /api/dir and reads the gauges from /api/state; the
    // mkdir/delete OPS are their own HTTP endpoints (POST/DELETE /api/dir?path=) — not persisted
    // controls — so a create/delete carries its path in the request, not in device storage. The
    // `show hidden` flag keeps it bound for the API while the generic control list skips it.
    controls_.addBool("show hidden", showHidden_);      // reveal dot-prefixed entries (e.g. .config)
    controls_.setHidden(controls_.count() - 1, true);
    // Filesystem-usage gauge (used / total bytes), shown below the tree in the panel. setup() and
    // loop1s refresh the cached values; control rebuilds only rebind descriptors.
    if (totalBytes_ > 0) {
        controls_.addProgress("filesystem", usedBytes_, totalBytes_);
        controls_.setHidden(controls_.count() - 1, true);   // renders as the usage bar in the panel, not generically
    }
    // "last saved" readout — how long ago config was persisted. The value is OWNED by the
    // FilesystemModule engine (non-UI); the File Manager just displays it here (this is where
    // filesystem state is topical). Bind the control straight to the engine's live buffer — no
    // per-instance copy — the same no-copy pattern SystemModule uses for its static strings. The
    // engine is the boot-wired singleton (alive for the device's life), and its loop1s keeps the
    // string current. Bound only when the engine exists (it's constructed before this module).
    if (FilesystemModule* fs = FilesystemModule::instance()) {
        controls_.addReadOnly("lastSaved", fs->lastSavedStr());
        controls_.setHidden(controls_.count() - 1, true);   // shown in the panel header, not generically
    }
    MoonModule::onBuildControls();
}

void FileManagerModule::loop1s() {
    if (totalBytes_ == 0) return;

    const uint32_t now = platform::millis();
    if (lastUsageRefreshMs_ != 0 && now - lastUsageRefreshMs_ < kUsageRefreshMs) return;

    refreshUsage();
}

void FileManagerModule::setup() {
    MoonModule::setup();
    // `show hidden` is a transient view preference, not device config — force it off on every boot
    // regardless of any persisted value (setup() runs after persistence overlays it). A file manager
    // opens with hidden entries hidden; the user re-toggles per session.
    showHidden_ = false;
    refreshUsage();
    rebuildControls();
}

void FileManagerModule::refreshUsage() {
    totalBytes_ = static_cast<uint32_t>(platform::filesystemTotal());
    if (totalBytes_ == 0) {
        usedBytes_ = 0;
        lastUsageRefreshMs_ = 0;
        return;
    }

    // LittleFS usage accounting can take tens of milliseconds on ESP32; keep it
    // out of onBuildControls(), which can run after ordinary control writes.
    usedBytes_ = static_cast<uint32_t>(platform::filesystemUsed());
    lastUsageRefreshMs_ = platform::millis();
}

// mkdir/delete are HTTP endpoints (POST/DELETE /api/dir?path=) in HttpServerModule: a create/delete
// carries its path in the request and touches the filesystem directly, so this module holds no op
// state and writes nothing to persisted config. The path guard (reject `..`, root at mount) lives
// once in HttpServerModule::parseFilePath, shared with /api/file + /api/dir GET.

} // namespace mm
