#pragma once

#include "core/MoonModule.h"
#include "core/build_info.h"   // kVersion / kRelease / kBuildDate / kFirmwareName
#include "platform/platform.h" // firmwareSize / firmwarePartition

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

// FirmwareUpdateModule — surfaces OTA flash progress as live read-only controls.
//
// Not user-configurable: ensureInfraModules() recreates it on every boot if
// absent (same safety net as NetworkModule). The actual flash is driven by
// POST /api/firmware/url in HttpServerModule — that handler spawns the OTA
// task via platform::http_fetch_to_ota(), which writes to two file-scope
// globals (g_otaStatus, g_otaBytesRead, g_otaBytesTotal). This module polls
// them in loop1s() and
// copies into its bound control buffers so the WebSocket state push picks up
// the change at 1 Hz. The shared-buffer + 1 Hz poll pattern is the simplest
// way to bridge a FreeRTOS task and a MoonModule on the scheduler thread
// without locks. No synchronisation: torn reads of
// the status string are acceptable for display-only fields.
//
// On desktop (platform::hasOta == false) the controls still exist for UI
// uniformity but the route returns 501; status stays "idle" forever.

// File-scope globals shared with the OTA route + the platform-layer task.
// Declared `inline` (C++17) so multiple translation units that include the
// header still share one storage instance (the header is included from
// HttpServerModule.cpp via the route, and from the module instantiation
// site in main.cpp — both must see the same g_otaStatus). An anonymous
// namespace would do the opposite — per-TU storage — which is why we
// use `inline` here.
//
// g_otaBytesRead / g_otaBytesTotal are the live byte counters the task writes.
// The UI renders them as "X KB / Y KB" via the existing progress control. The
// total starts at 0 (unknown) and flips to the real image size as soon as
// esp_https_ota_get_image_size returns it; the module's loop1s() re-binds
// the progress control when that transition happens so the static total
// captured by addProgress reflects reality (addProgress takes total by value,
// not pointer — re-bind is the cheaper alternative to widening that contract).
inline char     g_otaStatus[64]     = "idle";
inline uint32_t g_otaBytesRead      = 0;
inline uint32_t g_otaBytesTotal     = 0;

class FirmwareUpdateModule : public MoonModule {
public:
    // Diagnostics keep ticking regardless of the user toggle; matches
    // SystemModule + NetworkModule. The user can't easily re-enable a
    // disabled diagnostic module without it being visible.
    bool respectsEnabled() const override { return false; }

    void setup() override {
        // Copy the file-scope globals into the bound buffers on boot so the
        // first WS state push surfaces a coherent "idle" / 0 pair.
        std::strncpy(statusStr_, g_otaStatus, sizeof(statusStr_) - 1);
        statusStr_[sizeof(statusStr_) - 1] = '\0';
        bytesRead_ = g_otaBytesRead;
        totalSnap_ = g_otaBytesTotal;

        // Firmware identity (static for this build). version is PURE SEMVER (kVersion from
        // library.json): a clean "2.0.0" on a stable release, or a prerelease like "2.1.0-dev" on a
        // moving/dev build (semver.org §9 — the prerelease suffix is how a not-yet-released build is
        // expressed). The release channel is derivable from the version itself (a prerelease suffix
        // means "not a stable release"), so it is NOT mixed into the string; kRelease stays the
        // separate build-channel tag (which git tag this binary shipped under) without polluting the
        // machine-comparable version. This keeps `version` a clean semver the UI's update check can
        // compare against the newest GitHub release.
        std::snprintf(versionStr_, sizeof(versionStr_), "%s", kVersion);
        std::snprintf(buildStr_, sizeof(buildStr_), "%s", kBuildDate);
        std::snprintf(firmwareStr_, sizeof(firmwareStr_), "%s", kFirmwareName);
    }

    void onBuildControls() override {
        // Firmware identity (static), then OTA progress. firmwarePartition is the running app
        // partition's usage; queried here (idempotent, no I/O) so the gate sees a real total.
        controls_.addReadOnly("version", versionStr_, sizeof(versionStr_));
        controls_.addReadOnly("build", buildStr_, sizeof(buildStr_));
        controls_.addReadOnly("firmware", firmwareStr_, sizeof(firmwareStr_));
        firmwareSizeVal_ = static_cast<uint32_t>(platform::firmwareSize());
        totalFlashVal_ = static_cast<uint32_t>(platform::firmwarePartition());
        if (totalFlashVal_ > 0) {
            controls_.addProgress("firmwarePartition", firmwareSizeVal_, totalFlashVal_);
        }

        controls_.addReadOnly("update_status", statusStr_, sizeof(statusStr_));
        // Total is captured by value into the descriptor's `aux`; we re-bind
        // (via markDirty → HttpServerModule rebuildControls) when totalSnap_
        // changes. Initially 0; the UI shows "0KB / 0KB" until esp_https_ota
        // reports the image size, then "X KB / 1297KB" for the rest.
        controls_.addProgress("update_pct", bytesRead_, totalSnap_);
    }

    void loop1s() override {
        // Poll the OTA task's progress + status. No locks: the writer is
        // a single task, reads are atomic at this granularity, and a torn
        // read shows as a brief mid-update glimpse — visually harmless.
        std::strncpy(statusStr_, g_otaStatus, sizeof(statusStr_) - 1);
        statusStr_[sizeof(statusStr_) - 1] = '\0';
        bytesRead_ = g_otaBytesRead;
        // Re-bind on total transition. Only fires once per OTA (and once on
        // any later OTA the user starts — we deliberately don't reset the
        // total to 0 between updates; the previous value is a fine starting
        // estimate until the new task reports the new size). rebuildControls
        // re-runs onBuildControls() so the addProgress' captured `aux` (total)
        // is refreshed to the new totalSnap_ value.
        if (g_otaBytesTotal != totalSnap_) {
            totalSnap_ = g_otaBytesTotal;
            rebuildControls();
        }
    }

private:
    char     statusStr_[64] = "idle";
    uint32_t bytesRead_     = 0;
    uint32_t totalSnap_     = 0;
    // Firmware identity (static for this build) + the running app-partition usage.
    char     versionStr_[32] = {};   // semver + " (channel)" — e.g. "1.0.0-rc2 (latest)"
    char     buildStr_[24]   = {};
    char     firmwareStr_[24] = {};  // build variant name, e.g. "esp32s3-n16r8"
    uint32_t firmwareSizeVal_ = 0;   // bytes used in the app partition
    uint32_t totalFlashVal_   = 0;   // app partition size
};

} // namespace mm
