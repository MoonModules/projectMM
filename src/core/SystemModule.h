#pragma once

#include "core/MoonModule.h"
#include "core/Scheduler.h"
#include "core/FilesystemModule.h"   // setDeviceModel() arms the debounced save (noteDirty)
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// System-level diagnostics and device identity — always loaded, always visible in the
/// UI. Surfaces the live tick metrics (uptime, fps, tick time, free heap, PSRAM, largest
/// allocatable block), the static hardware facts (chip, SDK/IDF version, flash size,
/// boot/reset reason, WiFi co-processor state), and owns the device's identity: its
/// network name and its physical-hardware model.
///
/// **Controls (ordered by change frequency):**
/// - *Dynamic (every second):* `uptime` (progress), `fps` (derived from the Scheduler's
///   tick time), `tickTimeUs` (average tick, microseconds), `heap` (progress: used /
///   total internal), `psram` (progress: used / total, only when present), `maxBlock`
///   (largest contiguous allocatable block).
/// - *Configurable:* `deviceName` (default `MM-XXXX`, XXXX = last 4 hex of the MAC) and
///   `deviceModel` (display-only in the UI, pushed by tooling).
/// - *Static (set at boot):* `chip`, `sdk`, `flash`, `bootReason`, and `wifiCoproc`.
///   On desktop the hardware-specific fields read "desktop" / "N/A".
///
/// **`wifiCoproc`:** shown only on boards whose radio is a separate chip (the ESP32-P4
/// with its on-board ESP32-C6 over esp_hosted); absent on native-radio targets (the
/// platform returns an empty string and the control is not added). Reports the detected
/// slave firmware version (`C6 fw 2.12.9`) when the link is up, or `not detected` when
/// the C6 never completes its handshake / reports 0.0.0 — the signature of absent or
/// incompatible C6 slave firmware. `loop1s()` re-queries it, so the state stays current
/// if the link comes up after boot or the C6 is reflashed without a host reboot.
///
/// **Device name:** `deviceName` is the single network identity across the system —
/// NetworkModule uses it as the mDNS hostname (`<name>.local`), the SoftAP SSID, and the
/// DHCP hostname, and MoonDeck shows it in the device list. It is coerced to a valid,
/// non-empty hostname every tick (sanitize + MAC fallback) so whatever the user typed or
/// persistence restored, a live rename propagates everywhere within one tick — see
/// `loop1s()`. The default `MM-XXXX` derives from the last 4 hex of the MAC.
///
/// **Device model:** `deviceModel` is the physical-hardware identity (which product this
/// is, for example `Olimex ESP32-Gateway Rev G`) — the entry name from the device-model
/// catalog. The device cannot self-identify its hardware, so this is pushed by tooling:
/// the web installer sends it as an `APPLY_OP` `set` op during provisioning, or MoonDeck
/// over HTTP `/api/control`. The printable-ASCII rule (1..31 chars, 0x20–0x7E, no NUL)
/// is a per-control validator on the descriptor, so every write path (HTTP, serial
/// APPLY_OP, persistence load) runs it in the backend. See `validateDeviceModel`.
///
/// **`bootReason`:** the human-readable reset reason from `platform::resetReason()`
/// (`POWERON`, `SW`, `PANIC`, `INT_WDT`, `TASK_WDT`, `BROWNOUT`, `DEEPSLEEP`; desktop
/// always reports `OK`). The UI flags the reboot button with a red border when the value
/// is one of PANIC / INT_WDT / TASK_WDT / BROWNOUT, indicating the prior boot ended
/// unexpectedly.
///
/// **PSRAM detection is derived, not flagged:** ESP-IDF auto-detects the PSRAM chip at
/// boot and merges its pool into the heap allocator, after which `totalHeap()` reports
/// internal + PSRAM combined while `totalInternalHeap()` reports internal only — so
/// `totalHeap > totalInternal` is the "PSRAM present" signal. Boards without PSRAM skip
/// the `psram` control naturally, with no per-platform code path.
///
/// **Children:** accepts user-added Peripheral children (sensors, actuators) through the
/// generic MoonModule add/replace/delete + persistence path — the same firmware runs
/// with or without them.
///
/// **Prior art:** MoonLight — system diagnostics via REST API; device name used for
/// mDNS.
/// @card SystemModule.png
class SystemModule : public MoonModule {
public:
    void setScheduler(Scheduler* s) { scheduler_ = s; }

    /// Diagnostics keep ticking regardless — disabling System hides uptime/heap/fps
    /// from the UI for no good reason, and the user can't easily re-enable.
    bool respectsEnabled() const override { return false; }

    /// Accepts user-added Peripheral children (sensors, actuators — bridges to
    /// hardware/network the user solders on or off). The same firmware runs with
    /// or without them, so the user adds/deletes them at runtime; the add/replace/
    /// delete + persistence machinery is the generic MoonModule path. (The deviceModel
    /// identity is a SystemModule control above, not a child module — SystemModule owns
    /// the device's identity, name + model, directly.)
    const char* acceptsChildRoles() const override { return "peripheral"; }

    void setup() override {
        // Compute default deviceName from MAC: MM-XXXX. Skip if a persisted value was
        // already overlaid by Scheduler phase 2 (deviceName_ non-empty). Sanitize first
        // in case the persisted value is an invalid hostname (older firmware let any
        // text through); coerce + MAC-fallback so deviceName_ is a valid, non-empty
        // hostname from the very first read — every network name (mDNS/AP/DHCP) derives
        // from it directly, so it must be valid before NetworkModule::setup() reads it.
        sanitizeHostname(deviceName_);
        if (deviceName_[0] == 0) {
            uint8_t mac[6];
            platform::getMacAddress(mac);
            std::snprintf(deviceName_, sizeof(deviceName_), "MM-%02X%02X",
                          mac[4], mac[5]);
        }

        // chip / sdk / mac / bootReason controls bind straight to the platform's static strings (no
        // per-module copy — see onBuildControls). version / build / firmware (firmware identity, incl.
        // the build date) moved to FirmwareUpdateModule — SystemModule keeps only the IDF `sdk`.
        if constexpr (platform::hasWifiCoprocessor) {
            (void)platform::coprocessorWifi();   // prime the static (the control points at it)
        }

        if (scheduler_) scheduler_->setTargetFps(fpsCap_);

        if (chipFlashVal_ > 0) {
            std::snprintf(flashStr_, sizeof(flashStr_), "%uMB",
                          static_cast<unsigned>(chipFlashVal_ / (1024 * 1024)));
        }

        // Chain to base so children (user-added Peripherals) get
        // their setup() — a peripheral initialises its hardware here. Overriding
        // setup() shadows the base default that would otherwise propagate.
        MoonModule::setup();
    }

    void onBuildControls() override {
        // Platform-derived totals queried here (idempotent, no I/O) so the conditionals that
        // gate the Progress controls see real values rather than waiting on setup().
        totalInternalVal_ = static_cast<uint32_t>(platform::totalInternalHeap());
        totalHeapVal_ = static_cast<uint32_t>(platform::totalHeap());
        chipFlashVal_ = static_cast<uint32_t>(platform::flashChipSize());

        // Device name on top
        controls_.addText("deviceName", deviceName_, sizeof(deviceName_));

        // deviceModel — the physical-hardware identity (the catalog entry name, e.g.
        // "Olimex ESP32-Gateway Rev G"). The device can't self-identify its hardware, so
        // this is INJECTED by tooling: MoonDeck / the device UI via HTTP /api/control, or
        // the web installer via an APPLY_OP `set System.deviceModel` over serial. It's a
        // normal Text control like any other default — the printable-ASCII rule below is a
        // per-control validator (see ControlDescriptor::validate) so EVERY write path
        // checks it in the backend, wherever the write comes from. Display-only in
        // the UI (pushed, never user-typed); bound as Text — not ReadOnly — because Text is
        // auto-persisted and the readonly flag is only a UI-render hint.
        controls_.addText("deviceModel", deviceModel_, sizeof(deviceModel_), validateDeviceModel);
        controls_.setReadOnly(controls_.count() - 1, true);

        // Dynamic (updated every second)
        controls_.addReadOnly("uptime", uptimeStr_, sizeof(uptimeStr_));
        controls_.addReadOnly("fps", fpsStr_, sizeof(fpsStr_));
        controls_.addUint8("fpsCap", fpsCap_, 10, 120);
        controls_.addReadOnly("tickTimeUs", tickStr_, sizeof(tickStr_));
        if (totalInternalVal_ > 0) {
            controls_.addProgress("heap", heapUsedVal_, totalInternalVal_);
        }
        // PSRAM detection — derived, not flagged. ESP-IDF auto-detects the
        // PSRAM chip at boot (`I (...) esp_psram: Found NMB PSRAM device`)
        // and merges its pool into the heap allocator. After that
        // `totalHeap()` reports internal + PSRAM combined while
        // `totalInternalHeap()` reports internal only — so `totalHeap >
        // totalInternal` IS the "PSRAM present" signal. No explicit flag,
        // no per-platform code path; boards without PSRAM (or with PSRAM
        // disabled in sdkconfig) skip this control naturally.
        if (totalHeapVal_ > totalInternalVal_) {
            controls_.addProgress("psram", psramUsedVal_, totalHeapVal_ - totalInternalVal_);
        }
        controls_.addReadOnly("maxBlock", maxBlockStr_, sizeof(maxBlockStr_));

        // Flash. (version / build / firmware / firmwarePartition moved to FirmwareUpdateModule — the
        // firmware card owns firmware identity + partition usage; the filesystem-usage bar moved to
        // FilesystemModule — the module that owns the filesystem.)
        if (chipFlashVal_ > 0) {
            controls_.addReadOnly("flash", flashStr_, sizeof(flashStr_));
        }

        // Static info — the platform returns these as permanent static strings (a chip's MAC / model
        // / SDK never change at runtime), so the ReadOnly control binds straight to the platform
        // buffer. Nothing stored per-module: don't copy a static string into a member (minimise
        // memory; don't store what's derivable). const_cast is safe — ReadOnly never writes.
        // bufSize is unused for a ReadOnly (never written), so the default is fine.
        controls_.addReadOnly("mac", const_cast<char*>(platform::macString()));   // stable per-chip identity (tooling keys on it)
        controls_.addReadOnly("chip", const_cast<char*>(platform::chipModel()));
        controls_.addReadOnly("sdk", const_cast<char*>(platform::sdkVersion()));
        controls_.addReadOnly("bootReason", const_cast<char*>(platform::resetReason()));
        // WiFi co-processor (P4 + on-board C6) firmware read-out. Gated at compile
        // time on hasWifiCoprocessor, so the whole control — and the snprintf/query
        // cost — vanishes on native-radio builds (classic/S3/desktop) and the
        // eth-only P4. Its value proves the C6 slave-firmware state ("C6 fw 2.12.9"
        // vs "not detected"). loop1s() refreshes it.
        if constexpr (platform::hasWifiCoprocessor) {
            controls_.addReadOnly("wifiCoproc", const_cast<char*>(platform::coprocessorWifi()));
        }

        // Chain into children (user-added Peripherals). Per the override-and-chain
        // convention in architecture.md § Lifecycle propagation to children:
        // `onBuildControls` cascades to children via MoonModule's base default;
        // overriding the method shadows that default, so we must call it
        // explicitly. Order doesn't matter here — SystemModule's own controls
        // don't depend on children's controls.
        MoonModule::onBuildControls();
    }

    void onUpdate(const char* name) override {
        if (std::strcmp(name, "fpsCap") == 0 && scheduler_) {
            scheduler_->setTargetFps(fpsCap_);
        }
    }

    void loop1s() override {
        // deviceName is the single network identity (mDNS <name>.local, SoftAP SSID,
        // DHCP hostname all derive from it), so it must stay a valid hostname whatever
        // the user typed or persistence restored. Coerce it here each tick — idempotent
        // on an already-valid name, and it runs before NetworkModule::loop1s() reads it,
        // so a live rename ("My Room" → "My-Room") propagates everywhere within a tick.
        sanitizeHostname(deviceName_);
        if (deviceName_[0] == 0) {        // user cleared it / all-invalid → MAC fallback
            uint8_t mac[6];
            platform::getMacAddress(mac);
            std::snprintf(deviceName_, sizeof(deviceName_), "MM-%02X%02X", mac[4], mac[5]);
        }

        // Update dynamic values
        uint32_t uptimeSec = scheduler_ ? scheduler_->elapsed() / 1000 : 0;
        uint32_t hours = uptimeSec / 3600;
        uint32_t mins = (uptimeSec % 3600) / 60;
        uint32_t secs = uptimeSec % 60;
        std::snprintf(uptimeStr_, sizeof(uptimeStr_), "%u:%02u:%02u",
                      static_cast<unsigned>(hours),
                      static_cast<unsigned>(mins),
                      static_cast<unsigned>(secs));

        uint32_t fps = scheduler_ ? scheduler_->fps() : 0;
        std::snprintf(fpsStr_, sizeof(fpsStr_), "%u", static_cast<unsigned>(fps));

        if (scheduler_) scheduler_->setTargetFps(fpsCap_);

        uint32_t tickUs = scheduler_ ? scheduler_->tickTimeUs() : 0;
        std::snprintf(tickStr_, sizeof(tickStr_), "%u", static_cast<unsigned>(tickUs));

        uint32_t freeTotal = static_cast<uint32_t>(platform::freeHeap());
        uint32_t freeInternal = static_cast<uint32_t>(platform::freeInternalHeap());
        heapUsedVal_ = totalInternalVal_ > freeInternal ? totalInternalVal_ - freeInternal : 0;
        uint32_t freePsram = freeTotal > freeInternal ? freeTotal - freeInternal : 0;
        uint32_t totalPsram = totalHeapVal_ > totalInternalVal_ ? totalHeapVal_ - totalInternalVal_ : 0;
        psramUsedVal_ = totalPsram > freePsram ? totalPsram - freePsram : 0;

        // maxInternalAllocBlock — NOT maxAllocBlock. The internal-RAM block
        // is the scarce-resource KPI; the all-memory variant reports ~8 MB
        // on PSRAM-equipped boards (S3/S2) and tells the user nothing.
        std::snprintf(maxBlockStr_, sizeof(maxBlockStr_), "%uKB",
                      static_cast<unsigned>(platform::maxInternalAllocBlock() / 1024));

        // Refresh the WiFi co-processor status, so the displayed C6 firmware state
        // stays current if the link comes up after boot or the C6 is reflashed
        // without a host reboot. coprocessorWifi() re-queries the C6 and updates its own
        // static buffer in place — the wifiCoproc control points straight at it, so we
        // just call it to refresh (no per-module copy). Compiled out where there's no co-proc.
        if constexpr (platform::hasWifiCoprocessor) {
            (void)platform::coprocessorWifi();
        }

        // Chain to base so children get their loop1s() — a Peripheral formats
        // its read-only display values here. Overriding loop1s() shadows the
        // base default that would otherwise propagate. (setup/loop20ms/loop/
        // teardown propagate too: setup is chained above, loop20ms/loop/teardown
        // aren't overridden so the base default carries them.)
        MoonModule::loop1s();
    }

    /// The device's network identity (mDNS hostname, SoftAP SSID, DHCP hostname all
    /// derive from it). Guaranteed a valid, non-empty hostname — coerced every tick.
    const char* deviceName() const { return deviceName_; }

    /// The physical-hardware identity (device-model catalog entry name), pushed by tooling.
    const char* deviceModel() const { return deviceModel_; }

    /// Per-control validator for `deviceModel`, applied on EVERY write path (HTTP
    /// /api/control, APPLY_OP over serial, persistence load) via ControlDescriptor::validate.
    /// Accepts 1..31 chars, ASCII-printable (0x20–0x7E), no embedded NUL. The printable floor
    /// rejects control bytes / NULs that would corrupt downstream consumers — JSON
    /// serialization (control bytes need \u escaping at best, break naive emitters at worst),
    /// the device UI (rendered verbatim; a BEL/ESC would mangle the page), and C-string
    /// handling (no embedded NUL → strlen/strcpy round-trip cleanly). Printable ASCII still
    /// contains `"` and `\`, which serializers must escape normally — the floor isn't a
    /// license to skip escaping. (Length: the 31-char cap matches deviceModel_'s 32-byte
    /// buffer; over-long is rejected, not truncated.) Declaring the rule on the control
    /// keeps it with the data, so it holds for every transport that writes deviceModel.
    static bool validateDeviceModel(const char* value) {
        if (!value) return false;
        size_t n = std::strlen(value);
        if (n == 0 || n >= 32) return false;   // 1..31 (32-byte buffer, NUL-terminated)
        for (size_t i = 0; i < n; i++) {
            unsigned char b = static_cast<unsigned char>(value[i]);
            if (b < 0x20 || b > 0x7E) return false;
        }
        return true;
    }

private:
    Scheduler* scheduler_ = nullptr;

    // Configurable
    char deviceName_[24] = {};
    // Physical-hardware identity (catalog entry name). 32-byte buffer fits the longest
    // entry ("Olimex ESP32-Gateway Rev G" = 26) with headroom; the Improv RPC handler
    // caps str_len against this size dynamically.
    char deviceModel_[32] = {};
    uint8_t fpsCap_ = 60;

    // Dynamic (updated in loop1s)
    char uptimeStr_[16] = {};
    char fpsStr_[8] = {};
    char tickStr_[8] = {};
    char maxBlockStr_[12] = {};
    uint32_t heapUsedVal_ = 0;
    uint32_t psramUsedVal_ = 0;

    // Static (set in setup)
    uint32_t totalInternalVal_ = 0;
    uint32_t totalHeapVal_ = 0;
    char flashStr_[12] = {};
    uint32_t chipFlashVal_ = 0;     // total chip flash
};

} // namespace mm
