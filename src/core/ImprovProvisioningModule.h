#pragma once

#include "core/MoonModule.h"
#include "core/NetworkModule.h"
#include "core/SystemModule.h"
#include "core/HttpServerModule.h"
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
    // For the APPLY_OP vendor RPC — the module routes a pushed REST op to the
    // HttpServerModule's apply-core (the same code /api/modules + /api/control use).
    void setHttpServerModule(HttpServerModule* h) { httpServerModule_ = h; }

    // Diagnostics keep ticking; matches FirmwareUpdateModule / SystemModule.
    bool respectsEnabled() const override { return false; }

    // Apparatus, not swappable content — provisioning is a fixed device service.
    // Not deletable (matches Board / Preview); can still be disabled.
    bool userEditable() const override { return false; }

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
                pendingDeviceModel_, sizeof(pendingDeviceModel_),
                &pendingDeviceModelReady_,
                &pendingTxPower_, &pendingTxPowerReady_,
                pendingOp_, sizeof(pendingOp_), &pendingOpReady_);
        } else {
            std::strncpy(statusStr_, "not supported on this platform", sizeof(statusStr_) - 1);
        }
    }

    void onBuildControls() override {
        controls_.addReadOnly("provision_status", statusStr_, sizeof(statusStr_));
    }

    void loop1s() override {
        // Vendor SET_TX_POWER RPC — handled BEFORE the credentials on purpose:
        // when an installer sends the cap and the credentials back-to-back,
        // both flags can land within one tick, and the cap must be persisted
        // before the STA attempt starts or a brown-out-prone board (a weak LDO /
        // marginal supply) fails auth at full power — the exact hole this RPC closes.
        if (pendingTxPowerReady_.load(std::memory_order_acquire) && networkModule_) {
            networkModule_->setTxPowerSetting(pendingTxPower_);
            pendingTxPowerReady_.store(false, std::memory_order_release);
        }
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
        // Mirror for vendor SET_DEVICE_MODEL RPC. The Improv task validated the
        // payload on the wire (length, ASCII-printable) and wrote it here;
        // SystemModule::setDeviceModel re-validates (returns false on rejection)
        // so a malformed value never reaches the persisted buffer.
        if (pendingDeviceModelReady_.load(std::memory_order_acquire) && systemModule_) {
            systemModule_->setDeviceModel(pendingDeviceModel_);
            std::memset(pendingDeviceModel_, 0, sizeof(pendingDeviceModel_));
            pendingDeviceModelReady_.store(false, std::memory_order_release);
        }
    }

    // APPLY_OP is polled per-TICK (not loop1s) because the installer pushes a burst
    // of ops during provisioning and single-buffers them: the Improv task refuses a
    // new op until this consumes the previous (clears pendingOpReady_), so a fast
    // poll keeps the busy-window to ~one tick and the install snappy. Applying on the
    // main loop here (not the Improv task) keeps the factory/tree mutation off the
    // serial task — the same discipline the credentials/deviceModel paths follow.
    void loop() override {
        if (pendingOpReady_.load(std::memory_order_acquire) && httpServerModule_) {
            httpServerModule_->applyOp(pendingOp_);   // result currently informational; ack already sent
            std::memset(pendingOp_, 0, sizeof(pendingOp_));
            pendingOpReady_.store(false, std::memory_order_release);
        }
        MoonModule::loop();   // tick children (none today, but keep the contract)
    }

private:
    SystemModule*     systemModule_     = nullptr;
    NetworkModule*    networkModule_    = nullptr;
    HttpServerModule* httpServerModule_ = nullptr;
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

    // SET_DEVICE_MODEL RPC buffer + ready flag — same producer/consumer dance as
    // pendingCredentials_, sized to SystemModule's deviceModel storage (32 bytes).
    char pendingDeviceModel_[32] = {};
    std::atomic<bool> pendingDeviceModelReady_{false};

    // Vendor SET_TX_POWER RPC — the pre-association TX-power cap (whole dBm)
    // for brown-out-prone boards; same producer/consumer shape as the above.
    uint8_t pendingTxPower_ = 0;
    std::atomic<bool> pendingTxPowerReady_{false};

    // Vendor APPLY_OP RPC — one REST op as JSON, reassembled by the Improv task and
    // applied here on the main loop via HttpServerModule::applyOp. Sized for the
    // largest op (a long pins list fits comfortably in 512 bytes).
    char pendingOp_[512] = {};
    std::atomic<bool> pendingOpReady_{false};
};

} // namespace mm
