#pragma once

#include "core/MoonModule.h"
#include "core/NetworkModule.h"
#include "core/SystemModule.h"
#include "core/build_info.h"
#include "platform/platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace mm {

/// Standard Espressif BLE WiFi provisioning. The BLE transport handles the
/// local credential exchange; this module publishes accepted credentials back
/// into NetworkModule so persistence, STA retry, and AP fallback stay in one
/// place.
class BleProvisioningModule : public MoonModule {
public:
    void setSystemModule(SystemModule* s) { systemModule_ = s; }
    void setNetworkModule(NetworkModule* n) { networkModule_ = n; }

    bool respectsEnabled() const override { return false; }
    bool userEditable() const override { return false; }

    void setup() override {
        tryStart();
    }

    void onBuildControls() override {
        setStatus(statusStr_);
        controls_.addReadOnly("ble_status", statusStr_, sizeof(statusStr_));
    }

    void loop1s() override {
        if (pendingCredentials_.load(std::memory_order_acquire) && networkModule_) {
            networkModule_->setWifiCredentials(pendingSsid_, pendingPassword_);
            std::memset(pendingPassword_, 0, sizeof(pendingPassword_));
            pendingCredentials_.store(false, std::memory_order_release);
        }

        if constexpr (platform::hasBleProvisioning) {
            if (!started_) {
                const uint32_t now = platform::millis();
                if (now - lastStartAttemptMs_ >= kRetryMs) {
                    tryStart();
                }
            }
        }
    }

    void teardown() override {
        if constexpr (platform::hasBleProvisioning) {
            platform::bleProvisioningStop();
        }
    }

private:
    void tryStart() {
        if constexpr (platform::hasBleProvisioning) {
            lastStartAttemptMs_ = platform::millis();
            const char* deviceName = systemModule_ ? systemModule_->deviceName() : "projectMM";
            platform::ImprovDeviceInfo info{
                deviceName,
                platform::chipModel(),
                kVersion,
            };
            started_ = platform::bleProvisioningInit(
                info,
                pendingSsid_, sizeof(pendingSsid_),
                pendingPassword_, sizeof(pendingPassword_),
                &pendingCredentials_,
                statusStr_, sizeof(statusStr_));
        } else {
            std::strncpy(statusStr_, "not supported on this platform", sizeof(statusStr_) - 1);
            started_ = false;
        }
    }

    static constexpr uint32_t kRetryMs = 5000;

    SystemModule*  systemModule_  = nullptr;
    NetworkModule* networkModule_ = nullptr;
    char statusStr_[96] = "BLE provisioning starting";
    char pendingSsid_[33] = {};
    char pendingPassword_[64] = {};
    std::atomic<bool> pendingCredentials_{false};
    uint32_t lastStartAttemptMs_ = 0;
    bool started_ = false;
};

} // namespace mm
