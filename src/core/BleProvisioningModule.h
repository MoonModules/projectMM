#pragma once

#include "core/MoonModule.h"
#include "core/NetworkModule.h"
#include "core/SystemModule.h"
#include "core/build_info.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

struct BleProvisioningRuntime {
    uint32_t (*millis)() = nullptr;
    const char* (*chipModel)() = nullptr;
    bool (*start)(const char* deviceName, const char* chipModel, const char* version,
                  char* ssidOut, size_t ssidOutLen,
                  char* passwordOut, size_t passwordOutLen,
                  std::atomic<bool>* ready,
                  char* statusBuf, size_t statusBufLen) = nullptr;
    void (*stop)() = nullptr;
};

/// Standard Espressif BLE WiFi provisioning. The BLE transport handles the
/// local credential exchange; this module publishes accepted credentials back
/// into NetworkModule so persistence, STA retry, and AP fallback stay in one
/// place.
class BleProvisioningModule : public MoonModule {
public:
    void setSystemModule(SystemModule* s) { systemModule_ = s; }
    void setNetworkModule(NetworkModule* n) { networkModule_ = n; }
    void setRuntime(const BleProvisioningRuntime* runtime) { runtime_ = runtime; }

    bool respectsEnabled() const override { return false; }
    bool userEditable() const override { return false; }

    void setup() override {
        if (!runtimeReady()) {
            copyStatus("BLE provisioning unavailable");
        } else if (networkModule_ && networkModule_->provisioningMode()) {
            tryStart();
        } else {
            copyStatus("BLE inactive (network configured)");
        }
        MoonModule::setup();
    }

    void defineControls() override {
        MoonModule::defineControls();
        controls_.addReadOnly("ble_status", statusStr_, sizeof(statusStr_));
    }

    void tick1s() override {
        if (pendingCredentials_.load(std::memory_order_acquire) && networkModule_) {
            networkModule_->setWifiCredentials(pendingSsid_, pendingPassword_);
            std::memset(pendingPassword_, 0, sizeof(pendingPassword_));
            pendingCredentials_.store(false, std::memory_order_release);
        }

        if (started_ && networkModule_ && !networkModule_->provisioningMode()) {
            runtime_->stop();
            started_ = false;
        } else if (!started_ && runtimeReady() && networkModule_ && networkModule_->provisioningMode()) {
            const uint32_t now = runtime_->millis();
            if (now - lastStartAttemptMs_ >= kRetryMs) {
                tryStart();
            }
        }
        MoonModule::tick1s();
    }

    void release() override {
        if (started_ && runtime_ && runtime_->stop) runtime_->stop();
        started_ = false;
        pendingCredentials_.store(false, std::memory_order_release);
        std::memset(pendingPassword_, 0, sizeof(pendingPassword_));
        MoonModule::release();
    }

private:
    bool runtimeReady() const {
        return runtime_ && runtime_->millis && runtime_->chipModel && runtime_->start && runtime_->stop;
    }

    void copyStatus(const char* status) {
        std::snprintf(statusStr_, sizeof(statusStr_), "%s", status ? status : "");
        setStatus(statusStr_);
    }

    void tryStart() {
        if (!runtimeReady()) return;
        lastStartAttemptMs_ = runtime_->millis();
        const char* deviceName = systemModule_ ? systemModule_->deviceName() : "projectMM";
        started_ = runtime_->start(
            deviceName, runtime_->chipModel(), kVersion,
            pendingSsid_, sizeof(pendingSsid_),
            pendingPassword_, sizeof(pendingPassword_),
            &pendingCredentials_,
            statusStr_, sizeof(statusStr_));
        setStatus(statusStr_);
    }

    static constexpr uint32_t kRetryMs = 5000;

    SystemModule*  systemModule_  = nullptr;
    NetworkModule* networkModule_ = nullptr;
    const BleProvisioningRuntime* runtime_ = nullptr;
    char statusStr_[96] = "BLE provisioning idle";
    char pendingSsid_[33] = {};
    char pendingPassword_[64] = {};
    std::atomic<bool> pendingCredentials_{false};
    uint32_t lastStartAttemptMs_ = 0;
    bool started_ = false;
};

} // namespace mm
