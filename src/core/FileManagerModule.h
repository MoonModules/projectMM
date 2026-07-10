#pragma once
// Core service module — `.h` interface, bodies in FileManagerModule.cpp (the core `.h`+`.cpp`
// convention: it bridges the platform fs layer + has real logic, so implementation edits recompile
// only the .cpp, not every TU that includes this header).

#include "core/MoonModule.h"

#include <cstddef>
#include <cstdint>

namespace mm {

/// Browse and manage the device filesystem from the UI — the counterpart to FilesystemModule,
/// which is the *persistence engine* (writes `/.config/*.json`, loads at boot). This module is the
/// user-facing **file manager**: an expand/collapse folder tree, each node's name + size, and
/// create / delete / edit of files and folders.
///
/// **Browsing lives in the UI.** The tree is a client-side lazy tree (the standard VS Code /
/// Explorer / web-file-tree shape): each folder loads *its own* children on first expand from the
/// `/api/dir?path=` endpoint (a single-level listing — the same `platform::fsList` the seam offers),
/// and expand-state is UI state. The module owns none of that; it only exposes the *operations*,
/// keeping the domain module simple and the recursion/paging out of core state and off `/api/state`.
///
/// **Hidden entries.** A leading `.` (e.g. the `.config` persistence dir) is hidden unless the
/// `show hidden` toggle is on — the standard dotfile convention. The toggle is a transient
/// per-session view preference (a bool the tree forwards to `/api/dir` as the `hidden` filter):
/// `setup()` forces it off on every boot, so a file manager always opens with hidden entries hidden.
///
/// **Operations are HTTP endpoints, not controls.** `POST /api/dir?path=` creates a folder,
/// `DELETE /api/dir?path=` removes a file or empty folder, and a file's contents are read/written
/// over `/api/file` — the path rides the request query, so an op carries its target in the request
/// rather than a stored control (nothing to persist to flash per op). All share the one path guard
/// (`HttpServerModule::parseFilePath`: reject `..`, root at the mount), and each fails cleanly on a
/// bad path / non-empty-dir delete — never crashes (the Robustness rule). This module itself owns
/// only the `show hidden` toggle + the read-only usage gauges.
///
/// **Not shown yet:** last-modified dates need a time source (NTP) + LittleFS mtime storage, both
/// backlogged — the tree is name + size for now.
///
/// **Prior art:** the lazy-loaded folder tree is the standard file-explorer shape (a node loads its
/// children when expanded); the `/api/dir` + `/api/file` split mirrors the listing-vs-contents split
/// every file API draws (WebDAV PROPFIND vs GET).
/// @card FileManagerModule.png
class FileManagerModule : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Peripheral; }

    void onBuildControls() override;
    void setup() override;
    void loop1s() override;

private:
    bool showHidden_ = false;      // reveal dot-prefixed entries (forwarded to /api/dir by the UI)
    uint32_t usedBytes_ = 0;       // "filesystem" progress: bytes used, refreshed sparingly
    uint32_t totalBytes_ = 0;      // "filesystem" progress: partition total, read once at build
    uint32_t lastUsageRefreshMs_ = 0;
};

} // namespace mm
