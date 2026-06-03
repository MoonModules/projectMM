#pragma once

#include "core/MoonModule.h"
#include "core/NetworkModule.h"
#include "core/SystemModule.h"
#include "core/BoardModule.h"
#include "core/build_info.h"
#include "platform/platform.h"

#include <atomic>
#include <cstring>

namespace mm {

// ImprovProvisioningModule — listens for Improv WiFi frames on UART0 and
// pushes credentials into NetworkModule. Browser drives the protocol via
// ESP Web Tools / improv-wifi.com; a Python CLI mirror lives at
// scripts/build/improv_provision.py for rack / CI use over USB.
//
// The actual protocol parsing + UART task lives in the platform layer
// (`mm::platform::improvProvisioningInit` at platform.h). This module is the
// status surface: one read-only `provision_status` Control that reports
// "listening" / "received credentials" / "connecting" / "connected: <ssid>"
// / "error: …". Module's loop1s() polls a `ready` flag the platform task
// sets when credentials arrive, then calls NetworkModule::setWifiCredentials
// which writes through to the same buffers the AP-fallback UI flow uses.
//
// On desktop (platform::hasImprov == false) the module exists for UI
// uniformity; the listener-install is skipped.

class ImprovProvisioningModule : public MoonModule {
public:
    void setSystemModule(SystemModule* s) { systemModule_ = s; }
    void setNetworkModule(NetworkModule* n) { networkModule_ = n; }
    void setBoardModule(BoardModule* b) { boardModule_ = b; }

    // Diagnostics keep ticking; matches FirmwareUpdateModule / SystemModule.
    bool respectsEnabled() const override { return false; }

    void setup() override {
        if constexpr (platform::hasImprov) {
            // Strings borrowed; platform task copies them into its own storage
            // on init, so locals going out of scope here is fine.
            const char* deviceName = systemModule_ ? systemModule_->deviceName() : "projectMM";
            platform::ImprovDeviceInfo info{
                deviceName,
                platform::chipModel(),
                kVersion,
            };
            platform::improvProvisioningInit(
                info,
                pendingSsid_, sizeof(pendingSsid_),
                pendingPassword_, sizeof(pendingPassword_),
                &pendingCredentials_,
                statusStr_, sizeof(statusStr_),
                pendingBoard_, sizeof(pendingBoard_),
                &pendingBoardReady_);
        } else {
            std::strncpy(statusStr_, "not supported on this platform", sizeof(statusStr_) - 1);
        }
    }

    void onBuildControls() override {
        controls_.addReadOnly("provision_status", statusStr_, sizeof(statusStr_));
    }

    void loop1s() override {
        // The platform task writes credentials into pendingSsid_/pendingPassword_
        // then publishes via a release-store on pendingCredentials_. We do an
        // acquire-load here so the buffer writes are visible before we read
        // them. Pairs with the release-store in platform_esp32.cpp.
        if (pendingCredentials_.load(std::memory_order_acquire) && networkModule_) {
            networkModule_->setWifiCredentials(pendingSsid_, pendingPassword_);
            // Wipe the on-stack-ish password buffer; status string keeps any
            // error message the platform layer wrote. SSID is non-sensitive,
            // leave it for the next poll if a re-provision arrives.
            std::memset(pendingPassword_, 0, sizeof(pendingPassword_));
            pendingCredentials_.store(false, std::memory_order_release);
        }
        // Mirror for vendor SET_BOARD RPC. The Improv task validated the
        // payload on the wire (length, ASCII-printable) and wrote it here;
        // BoardModule::setBoard re-validates (returns false on rejection)
        // so a malformed value never reaches the persisted buffer.
        if (pendingBoardReady_.load(std::memory_order_acquire) && boardModule_) {
            boardModule_->setBoard(pendingBoard_);
            std::memset(pendingBoard_, 0, sizeof(pendingBoard_));
            pendingBoardReady_.store(false, std::memory_order_release);
        }
    }

private:
    SystemModule*  systemModule_  = nullptr;
    NetworkModule* networkModule_ = nullptr;
    BoardModule*   boardModule_   = nullptr;
    char statusStr_[64] = "listening";

    // Buffers the platform task writes; sized to NetworkModule's storage.
    // std::atomic<bool> for the ready flag — read by loop1s() on the
    // scheduler thread, written by the Improv task. Acquire/release fencing
    // ensures the buffer writes are ordered against the flag publication,
    // which matters on dual-core Xtensa where the producer and consumer can
    // run on different cores.
    char pendingSsid_[33] = {};
    char pendingPassword_[64] = {};
    std::atomic<bool> pendingCredentials_{false};

    // SET_BOARD RPC buffer + ready flag — same producer/consumer dance as
    // pendingCredentials_, sized to BoardModule's storage (24 bytes).
    char pendingBoard_[24] = {};
    std::atomic<bool> pendingBoardReady_{false};
};

} // namespace mm
