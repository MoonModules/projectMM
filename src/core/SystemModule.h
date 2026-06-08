#pragma once

#include "core/MoonModule.h"
#include "core/Scheduler.h"
#include "core/build_info.h"
#include "platform/platform.h"

#include <cstdio>

namespace mm {

class SystemModule : public MoonModule {
public:
    void setScheduler(Scheduler* s) { scheduler_ = s; }

    // Diagnostics keep ticking regardless — disabling System hides uptime/heap/fps
    // from the UI for no good reason, and the user can't easily re-enable.
    bool respectsEnabled() const override { return false; }

    // Accepts user-added Peripheral children (sensors, actuators — bridges to
    // hardware/network the user solders on or off). The same firmware runs with
    // or without them, so the user adds/deletes them at runtime; the add/replace/
    // delete + persistence machinery is the generic MoonModule path. BoardModule
    // (also a System child) is code-wired and opts out of deletion via its own
    // userEditable() == false.
    const char* acceptsChildRoles() const override { return "peripheral"; }

    void setup() override {
        // Compute default deviceName from MAC: MM-XXXX. Skip if a persisted value was
        // already overlaid by Scheduler phase 2 (deviceName_ non-empty).
        if (deviceName_[0] == 0) {
            uint8_t mac[6];
            platform::getMacAddress(mac);
            std::snprintf(deviceName_, sizeof(deviceName_), "MM-%02X%02X",
                          mac[4], mac[5]);
        }

        // Snprintf static display strings into the bound buffers. onBuildControls already
        // bound these buffers by pointer; we fill them now and the UI picks up the content
        // on the next WebSocket push.
        std::snprintf(chipInfo_, sizeof(chipInfo_), "%s", platform::chipModel());
        std::snprintf(sdkInfo_, sizeof(sdkInfo_), "%s", platform::sdkVersion());
        // version = semver (what code) + release channel (which channel) when
        // the build pipeline burned one in: "1.0.0-rc2 (latest)". kRelease is
        // "" on local / dev builds, where we show the bare semver. See
        // build_info.h for the MM_VERSION vs MM_RELEASE split.
        if (kRelease[0] != 0) {
            std::snprintf(versionStr_, sizeof(versionStr_), "%s (%s)", kVersion, kRelease);
        } else {
            std::snprintf(versionStr_, sizeof(versionStr_), "%s", kVersion);
        }
        std::snprintf(buildStr_, sizeof(buildStr_), "%s", kBuildDate);
        std::snprintf(firmwareStr_, sizeof(firmwareStr_), "%s", kFirmwareName);
        std::snprintf(bootReasonStr_, sizeof(bootReasonStr_), "%s", platform::resetReason());

        if (chipFlashVal_ > 0) {
            std::snprintf(flashStr_, sizeof(flashStr_), "%uMB",
                          static_cast<unsigned>(chipFlashVal_ / (1024 * 1024)));
        }

        // Chain to base so children (BoardModule, user-added Peripherals) get
        // their setup() — a peripheral initialises its hardware here. Overriding
        // setup() shadows the base default that would otherwise propagate.
        MoonModule::setup();
    }

    void onBuildControls() override {
        // Platform-derived totals queried here (idempotent, no I/O) so the conditionals that
        // gate the Progress controls see real values rather than waiting on setup().
        totalInternalVal_ = static_cast<uint32_t>(platform::totalInternalHeap());
        totalHeapVal_ = static_cast<uint32_t>(platform::totalHeap());
        firmwareSizeVal_ = static_cast<uint32_t>(platform::firmwareSize());
        totalFlashVal_ = static_cast<uint32_t>(platform::firmwarePartition());
        chipFlashVal_ = static_cast<uint32_t>(platform::flashChipSize());
        totalFsVal_ = static_cast<uint32_t>(platform::filesystemTotal());

        // Device name on top
        controls_.addText("deviceName", deviceName_, sizeof(deviceName_));

        // Dynamic (updated every second)
        controls_.addReadOnly("uptime", uptimeStr_, sizeof(uptimeStr_));
        controls_.addReadOnly("fps", fpsStr_, sizeof(fpsStr_));
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

        // Flash/firmware/filesystem. The progress bar is named
        // `firmwarePartition` (not `firmware`) to avoid colliding with the
        // string `firmware` control bound a few lines below — both shared the
        // name pre-board-injection, which made any consumer that did
        // `controls.find(c => c.name === "firmware")` get whichever was bound
        // first (the progress bar's integer value) and break on string-only
        // operations like install-picker's isCompatible.
        if (totalFlashVal_ > 0) {
            controls_.addProgress("firmwarePartition", firmwareSizeVal_, totalFlashVal_);
        }
        if (chipFlashVal_ > 0) {
            controls_.addReadOnly("flash", flashStr_, sizeof(flashStr_));
        }
        if (totalFsVal_ > 0) {
            controls_.addProgress("filesystem", fsUsedVal_, totalFsVal_);
        }

        // Static info
        controls_.addReadOnly("version", versionStr_, sizeof(versionStr_));
        controls_.addReadOnly("build", buildStr_, sizeof(buildStr_));
        controls_.addReadOnly("firmware", firmwareStr_, sizeof(firmwareStr_));
        controls_.addReadOnly("chip", chipInfo_, sizeof(chipInfo_));
        controls_.addReadOnly("sdk", sdkInfo_, sizeof(sdkInfo_));
        controls_.addReadOnly("bootReason", bootReasonStr_, sizeof(bootReasonStr_));

        // Chain into children (BoardModule today). Per the override-and-chain
        // convention in architecture.md § Lifecycle propagation to children:
        // `onBuildControls` cascades to children via MoonModule's base default;
        // overriding the method shadows that default, so we must call it
        // explicitly. Order doesn't matter here — SystemModule's own controls
        // don't depend on children's controls.
        MoonModule::onBuildControls();
    }

    void loop1s() override {
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

        uint32_t tickUs = scheduler_ ? scheduler_->tickTimeUs() : 0;
        std::snprintf(tickStr_, sizeof(tickStr_), "%u", static_cast<unsigned>(tickUs));

        uint32_t freeTotal = static_cast<uint32_t>(platform::freeHeap());
        uint32_t freeInternal = static_cast<uint32_t>(platform::freeInternalHeap());
        heapUsedVal_ = totalInternalVal_ > freeInternal ? totalInternalVal_ - freeInternal : 0;
        uint32_t freePsram = freeTotal > freeInternal ? freeTotal - freeInternal : 0;
        uint32_t totalPsram = totalHeapVal_ > totalInternalVal_ ? totalHeapVal_ - totalInternalVal_ : 0;
        psramUsedVal_ = totalPsram > freePsram ? totalPsram - freePsram : 0;

        fsUsedVal_ = static_cast<uint32_t>(platform::filesystemUsed());

        // maxInternalAllocBlock — NOT maxAllocBlock. The internal-RAM block
        // is the scarce-resource KPI; the all-memory variant reports ~8 MB
        // on PSRAM-equipped boards (S3/S2) and tells the user nothing.
        std::snprintf(maxBlockStr_, sizeof(maxBlockStr_), "%uKB",
                      static_cast<unsigned>(platform::maxInternalAllocBlock() / 1024));

        // Chain to base so children get their loop1s() — a Peripheral formats
        // its read-only display values here. Overriding loop1s() shadows the
        // base default that would otherwise propagate. (setup/loop20ms/loop/
        // teardown propagate too: setup is chained above, loop20ms/loop/teardown
        // aren't overridden so the base default carries them.)
        MoonModule::loop1s();
    }

    const char* deviceName() const { return deviceName_; }

private:
    Scheduler* scheduler_ = nullptr;

    // Configurable
    char deviceName_[24] = {};

    // Dynamic (updated in loop1s)
    char uptimeStr_[16] = {};
    char fpsStr_[8] = {};
    char tickStr_[8] = {};
    char maxBlockStr_[12] = {};
    uint32_t heapUsedVal_ = 0;
    uint32_t psramUsedVal_ = 0;
    uint32_t fsUsedVal_ = 0;

    // Static (set in setup)
    char chipInfo_[16] = {};
    char sdkInfo_[24] = {};
    char versionStr_[32] = {};  // semver + " (channel)" — e.g. "1.0.0-rc2 (latest)"
    char buildStr_[24] = {};
    // 24 fits the longest current key ("desktop-macos-arm64" = 19) with headroom.
    char firmwareStr_[24] = {};
    char bootReasonStr_[16] = {};
    uint32_t totalInternalVal_ = 0;
    uint32_t totalHeapVal_ = 0;
    char flashStr_[12] = {};
    uint32_t totalFlashVal_ = 0;    // app partition size
    uint32_t firmwareSizeVal_ = 0;
    uint32_t chipFlashVal_ = 0;     // total chip flash
    uint32_t totalFsVal_ = 0;
};

} // namespace mm
