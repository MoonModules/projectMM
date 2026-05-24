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
        std::snprintf(versionStr_, sizeof(versionStr_), "%s", kVersion);
        std::snprintf(buildStr_, sizeof(buildStr_), "%s", kBuildDate);
        std::snprintf(boardStr_, sizeof(boardStr_), "%s", kBoardName);
        std::snprintf(bootReasonStr_, sizeof(bootReasonStr_), "%s", platform::resetReason());

        if (chipFlashVal_ > 0) {
            std::snprintf(flashStr_, sizeof(flashStr_), "%uMB",
                          static_cast<unsigned>(chipFlashVal_ / (1024 * 1024)));
        }
    }

    void onBuildControls() override {
        // Platform-derived totals queried here (idempotent, no I/O) so the conditionals that
        // gate the Progress controls see real values rather than waiting on setup().
        totalInternalVal_ = platform::totalInternalHeap();
        totalHeapVal_ = platform::totalHeap();
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
        if (totalHeapVal_ > totalInternalVal_) {
            controls_.addProgress("psram", psramUsedVal_, totalHeapVal_ - totalInternalVal_);
        }
        controls_.addReadOnly("maxBlock", maxBlockStr_, sizeof(maxBlockStr_));

        // Flash/firmware/filesystem
        if (totalFlashVal_ > 0) {
            controls_.addProgress("firmware", firmwareSizeVal_, totalFlashVal_);
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
        controls_.addReadOnly("board", boardStr_, sizeof(boardStr_));
        controls_.addReadOnly("chip", chipInfo_, sizeof(chipInfo_));
        controls_.addReadOnly("sdk", sdkInfo_, sizeof(sdkInfo_));
        controls_.addReadOnly("bootReason", bootReasonStr_, sizeof(bootReasonStr_));
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

        std::snprintf(maxBlockStr_, sizeof(maxBlockStr_), "%uKB",
                      static_cast<unsigned>(platform::maxAllocBlock() / 1024));
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
    char versionStr_[16] = {};
    char buildStr_[24] = {};
    // 24 fits the longest current key ("desktop-macos-arm64" = 19) with headroom.
    char boardStr_[24] = {};
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
