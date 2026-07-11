#pragma once

#include "core/MoonModule.h"
#include "core/OtaUpdateState.h"
#include "core/build_info.h"   // kVersion / kRelease / kBuildDate / kFirmwareName
#include "platform/platform.h" // firmwareSize / firmwarePartition

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

// Shared OTA status storage lives in OtaUpdateState.h so trigger modules can use
// the lifecycle gate without depending on this UI module.
/// A thin status surface for OTA flashing — surfaces flash progress as live
/// read-only controls plus the per-module status banner
///
/// Not user-configurable: `ensureInfraModules()` recreates it on every boot if
/// absent (same safety net as NetworkModule). The actual flash is driven by
/// `POST /api/firmware/url` in HttpServerModule, which hands the URL to
/// `platform::http_fetch_to_ota()` — a task that downloads via `esp_https_ota` and
/// writes the next OTA partition, communicating through the file-scope globals above
/// (`g_otaStatus`, `g_otaBytesRead`, `g_otaBytesTotal`). This module polls them in
/// `tick1s()` and copies into its bound control buffers so the WebSocket state push
/// picks up the change at 1 Hz. The shared-buffer + 1 Hz poll pattern is the simplest
/// way to bridge a FreeRTOS task and a MoonModule on the scheduler thread without
/// locks; no synchronisation, since torn reads of display-only fields are acceptable.
///
/// **Controls.** `version` — pure semver (`MM_VERSION`): a stable release is a clean
/// `X.Y.Z`, a moving `latest` build is a monotonic prerelease `` `<core>-dev.<N>` `` (semver.org
/// §9/§11), so the release channel is derivable from the version rather than mixed into
/// it, keeping the string a clean machine-comparable semver the UI's update check compares
/// against the newest GitHub release. `build` — build date/time. `firmware` — the
/// build-time variant key (`esp32`, `esp32-eth`, `esp32s3-n16r8`, … / `desktop-*`) that
/// identifies which release asset matches the device (a legacy `esp32-eth-wifi` key
/// OTA-maps to `esp32`); the physical hardware is SystemModule's `deviceModel`.
/// `firmwarePartition` — running app image size / app-partition size (named distinctly
/// from `firmware` so a `find(name === "firmware")` caller resolves the string).
/// `update_pct` — live byte counters rendered "X KB / Y KB" (`total` is 0 until
/// `esp_https_ota_get_image_size` reports it just after the TLS handshake; the name is
/// historical, the wire shape is bytes).
///
/// **Flash phase is not a control** — it surfaces through the module's shared status slot
/// (`setStatus()`): `idle` clears the banner, an `error:` prefix maps to `Severity::Error`,
/// everything else (`starting`/`downloading`/`flashing`/`rebooting`) is neutral. On
/// desktop (`platform::hasOta == false`) the controls still exist for UI uniformity but the
/// route returns 501 and status stays "idle" forever.
///
/// **Flash lifecycle.** A `POST /api/firmware/url` (HttpServerModule, body `{"url": …}`) writes
/// `starting` and spawns the platform OTA task, which walks: `downloading` → `esp_https_ota_begin`
/// opens the TLS connection and follows redirects (a GitHub release URL 302s to
/// `release-assets.githubusercontent.com`) → `flashing`, `esp_https_ota_perform` advancing
/// `update_pct` 0→100 → `esp_https_ota_finish` commits the image to the next OTA partition and flips
/// the boot pointer → `rebooting`, a ~600 ms delay so the HTTP response reaches the browser first,
/// then `esp_restart()`. The device boots the new image and the UI re-reads `version` / `firmware`.
///
/// **Error taxonomy** (all via the status slot, `error: ` prefix, staying until the next POST):
/// `error: ota begin <IDF name>` (DNS / TLS / no OTA partition), `error: ota perform <IDF name>`
/// (network drop mid-download), `error: incomplete download` (size mismatch), `error: ota finish
/// <IDF name>` (commit / boot-flip), `error: task create failed` (`xTaskCreate` OOM — no retry). A
/// wrong-firmware binary fails at `esp_https_ota_begin` (chip-family mismatch) or boot (partition
/// mismatch) — recoverable by a USB re-flash, not a brick.
///
/// **Compatibility** is the *caller's* responsibility (the install-picker's `isCompatible()`): strip
/// `-eth*` from both firmware keys, equal identities are compatible — so `esp32` and `esp32-eth` are
/// mutually OTA-compatible (same chip, different feature flags), the legacy `esp32-eth-wifi` key
/// strips to `esp32`, and `esp32s3-n16r8` is only itself.
///
/// **Prior art:** `esp_https_ota` is the standard ESP-IDF OTA-from-HTTP component used by every ESP32
/// OTA flow since IDF v4.x; the install-picker UI is the new layer on top.
/// @card FirmwareUpdateModule.png
class FirmwareUpdateModule : public MoonModule {
public:
    /// Diagnostics keep ticking regardless of the user toggle; matches
    /// SystemModule + NetworkModule. The user can't easily re-enable a
    /// disabled diagnostic module without it being visible.
    bool respectsEnabled() const override { return false; }

    void setup() override {
        // Copy the file-scope globals into the bound buffers on boot so the
        // first WS state push surfaces a coherent "idle" / 0 pair.
        std::strncpy(statusStr_, g_otaStatus, sizeof(statusStr_) - 1);
        statusStr_[sizeof(statusStr_) - 1] = '\0';
        publishStatus();
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
        std::snprintf(productStr_, sizeof(productStr_), "%s", kProductName);
        std::snprintf(brandStr_, sizeof(brandStr_), "%s", kBrandName);
        std::snprintf(updateManifestStr_, sizeof(updateManifestStr_), "%s", kAutoUpdateManifestUrl);
    }

    void defineControls() override {
        // Firmware identity (static), then OTA progress. firmwarePartition is the running app
        // partition's usage; queried here (idempotent, no I/O) so the gate sees a real total.
        controls_.addReadOnly("version", versionStr_, sizeof(versionStr_));
        controls_.addReadOnly("build", buildStr_, sizeof(buildStr_));
        controls_.addReadOnly("firmware", firmwareStr_, sizeof(firmwareStr_));
        controls_.addReadOnly("product", productStr_, sizeof(productStr_));
        controls_.addReadOnly("brand", brandStr_, sizeof(brandStr_));
        if (updateManifestStr_[0]) controls_.addReadOnly("updateManifest", updateManifestStr_, sizeof(updateManifestStr_));
        firmwareSizeVal_ = static_cast<uint32_t>(platform::firmwareSize());
        totalFlashVal_ = static_cast<uint32_t>(platform::firmwarePartition());
        if (totalFlashVal_ > 0) {
            controls_.addProgress("firmwarePartition", firmwareSizeVal_, totalFlashVal_);
        }

        // OTA status goes through MoonModule::setStatus() (the per-module status
        // slot every module shares), not a bespoke read-only control — same
        // choice DevicesModule made. The error-prefixed states map to
        // Severity::Error; everything else is neutral. See publishStatus().
        //
        // Total is captured by value into the descriptor's `aux`; we re-bind
        // (via markDirty → HttpServerModule rebuildControls) when totalSnap_
        // changes. Initially 0; the UI shows "0KB / 0KB" until esp_https_ota
        // reports the image size, then "X KB / 1297KB" for the rest.
        controls_.addProgress("update_pct", bytesRead_, totalSnap_);
    }

    void tick1s() override {
        // Poll the OTA task's progress + status. No locks: the writer is
        // a single task, reads are atomic at this granularity, and a torn
        // read shows as a brief mid-update glimpse — visually harmless.
        std::strncpy(statusStr_, g_otaStatus, sizeof(statusStr_) - 1);
        statusStr_[sizeof(statusStr_) - 1] = '\0';
        publishStatus();
        bytesRead_ = g_otaBytesRead;
        // Re-bind on total transition. Only fires once per OTA (and once on
        // any later OTA the user starts — we deliberately don't reset the
        // total to 0 between updates; the previous value is a fine starting
        // estimate until the new task reports the new size). rebuildControls
        // re-runs defineControls() so the addProgress' captured `aux` (total)
        // is refreshed to the new totalSnap_ value.
        if (g_otaBytesTotal != totalSnap_) {
            totalSnap_ = g_otaBytesTotal;
            rebuildControls();
        }
    }

    /// Point the shared status slot at our owned buffer, choosing the severity
    /// from the status text: the platform OTA task prefixes every failure with
    /// "error: " (see platform_esp32_ota.cpp), so that prefix is the Error gate.
    /// "idle" is the quiescent state and reads better as no banner than as an
    /// info banner, so it clears the slot. setStatus doesn't copy — statusStr_
    /// outlives every call, so the pointer stays valid.
    void publishStatus() {
        if (std::strcmp(statusStr_, "idle") == 0) {
            clearStatus();
        } else {
            setStatus(statusStr_,
                      std::strncmp(statusStr_, "error:", 6) == 0 ? Severity::Error
                                                                 : Severity::Status);
        }
    }

private:
    char     statusStr_[64] = "idle";
    uint32_t bytesRead_     = 0;
    uint32_t totalSnap_     = 0;
    // Firmware identity (static for this build) + the running app-partition usage.
    char     versionStr_[32] = {};   ///< pure semver — such as "2.0.0" or "2.1.0-dev.7"
    char     buildStr_[24]   = {};
    char     firmwareStr_[24] = {};  ///< build variant name, such as "esp32s3-n16r8"
    char     productStr_[24] = {};
    char     brandStr_[24] = {};
    char     updateManifestStr_[96] = {};
    uint32_t firmwareSizeVal_ = 0;   ///< bytes used in the app partition
    uint32_t totalFlashVal_   = 0;   ///< app partition size
};

} // namespace mm
