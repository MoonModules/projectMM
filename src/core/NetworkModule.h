#pragma once

#include "core/MoonModule.h"
#include "core/Scheduler.h"
#include "core/SystemModule.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

class NetworkModule : public MoonModule {
public:
    void setScheduler(Scheduler* s) { scheduler_ = s; }
    void setSystemModule(SystemModule* s) { systemModule_ = s; }

    // External entry-point for setting WiFi credentials at runtime — used by
    // ImprovProvisioningModule when the browser/CLI pushes new credentials over
    // USB-serial. Writes the same buffers the AP-fallback UI flow writes via
    // POST /api/control on `ssid` / `password`, then drives a clean transition
    // into State::WaitingSta so loop1s() takes over and either reports
    // connected (onConnected) or falls back to AP after the 10 s timeout.
    //
    // Why the explicit AP→STA tear-down (rather than just calling wifiStaInit
    // and letting esp_wifi_set_mode handle the mode change): in AP-mode the
    // platform layer's wifiInitDone_ flag is true, which makes ensureWifiInit
    // return early without registering the IP_EVENT_STA_GOT_IP handler. Without
    // that handler the wifiStaConnected_ flag never flips, the WaitingSta
    // state never sees the STA come up, and the device sits in limbo with
    // STA mode active but the state machine still thinking it's in AP.
    // wifiApStop() drops wifiInitDone_=false so the next ensureWifiInit
    // registers handlers cleanly.
    void setWifiCredentials(const char* ssid, const char* password) {
        if (!ssid) return;
        std::strncpy(ssid_, ssid, sizeof(ssid_) - 1);
        ssid_[sizeof(ssid_) - 1] = 0;
        std::strncpy(password_, password ? password : "", sizeof(password_) - 1);
        password_[sizeof(password_) - 1] = 0;
        markDirty();   // FilesystemModule picks this up on its next save
        if constexpr (platform::hasWiFi) {
            // Tear down any prior WiFi state (AP-fallback, mid-flight STA
            // attempt, or stale init from a previous reconfigure) so the
            // platform's event-handler registration runs fresh.
            if (state_ == State::AP) {
                platform::wifiApStop();
                apShutdownPending_ = false;
            }
            if (state_ == State::WaitingSta || state_ == State::ConnectedSta) {
                platform::wifiStaStop();
            }
            if (platform::wifiStaInit(ssid_, password_)) {
                state_ = State::WaitingSta;
                stateChangeTime_ = platform::millis();
                std::snprintf(statusStr_, sizeof(statusStr_), "WiFi STA: %s", ssid_);
                setStatus(statusStr_, Severity::Status);
            } else {
                // STA init failed (OOM, GPIO conflict). Try to recover via
                // AP so the user can re-enter credentials manually.
                startAP();
            }
        }
    }

    // Networking is infrastructure — keep the cascade ticking even when the user
    // toggled "enabled" off, otherwise the device would silently drop off the LAN
    // and become unreachable.
    bool respectsEnabled() const override { return false; }

    void setup() override {
        // Try Ethernet first (non-blocking)
        if (platform::ethInit()) {
            state_ = State::WaitingEth;
            std::printf("NetworkModule: Ethernet init started\n");
        } else if constexpr (platform::hasWiFi) {
            // Ethernet not available, fall back to WiFi (STA → AP).
            if (ssid_[0] != 0 && platform::wifiStaInit(ssid_, password_)) {
                state_ = State::WaitingSta;
                std::printf("NetworkModule: WiFi STA init started, SSID: %s\n", ssid_);
            } else {
                startAP();
            }
        } else {
            // Ethernet-only build: no WiFi fallback. Stay Idle until a cable
            // appears (WaitingEth is only entered on a successful ethInit()).
            state_ = State::Idle;
            std::snprintf(statusStr_, sizeof(statusStr_), "No network (Ethernet only)"); setStatus(statusStr_, Severity::Error);
        }

        stateChangeTime_ = platform::millis();

        // Chain to base so children (ImprovProvisioningModule on ESP32) get setup()
        // after we've claimed the network resources we care about.
        MoonModule::setup();
    }

    void onBuildControls() override {
        setStatus(statusStr_);
        // WiFi credential controls are absent in the Ethernet-only build.
        if constexpr (platform::hasWiFi) {
            controls_.addText("ssid", ssid_, sizeof(ssid_));
            controls_.addPassword("password", password_, sizeof(password_));
        }
        controls_.addSelect("addressing", addressing_, addressingOptions_, 2);
        controls_.addBool("mDNS", mdnsEnabled_);

        // Static-IP fields are always bound (so persistence can load them at any time),
        // but visibility flips based on addressing mode. Toggling the Select triggers a
        // rebuildControls() in HttpServerModule which re-runs this method and re-evaluates
        // the hidden flags.
        const bool hideStatic = (addressing_ != 1);
        controls_.addText("ip", staticIp_, sizeof(staticIp_));
        controls_.setHidden(controls_.count() - 1, hideStatic);
        controls_.addText("gateway", staticGateway_, sizeof(staticGateway_));
        controls_.setHidden(controls_.count() - 1, hideStatic);
        controls_.addText("subnet", staticSubnet_, sizeof(staticSubnet_));
        controls_.setHidden(controls_.count() - 1, hideStatic);
        controls_.addText("dns", staticDns_, sizeof(staticDns_));
        controls_.setHidden(controls_.count() - 1, hideStatic);

        // Chain to base so children (Improv on ESP32) get their controls built too.
        MoonModule::onBuildControls();
    }

    void loop1s() override {
        uint32_t now = platform::millis();
        uint32_t elapsed = now - stateChangeTime_;

        switch (state_) {
            case State::WaitingEth:
                if (platform::ethConnected()) {
                    onConnected("Ethernet");
                } else if ((elapsed > 3000 && !platform::ethLinkUp()) || elapsed > 15000) {
                    if constexpr (platform::hasWiFi) {
                        // No cable after 3s, or link up but no IP after 15s — cascade to WiFi
                        std::printf("NetworkModule: Ethernet %s, cascading\n",
                                    platform::ethLinkUp() ? "no IP (DHCP timeout)" : "no link (no cable)");
                        if (ssid_[0] != 0 && platform::wifiStaInit(ssid_, password_)) {
                            state_ = State::WaitingSta;
                            stateChangeTime_ = now;
                        } else {
                            startAP();
                        }
                    } else {
                        // Ethernet-only build: no fallback. Keep polling for a cable.
                        std::snprintf(statusStr_, sizeof(statusStr_), "No network (Ethernet only)"); setStatus(statusStr_, Severity::Error);
                        stateChangeTime_ = now;
                    }
                }
                break;

            case State::WaitingSta:
                if constexpr (platform::hasWiFi) {
                    if (platform::wifiStaConnected()) {
                        onConnected("WiFi STA");
                    } else if (elapsed > 10000) {
                        // WiFi STA didn't connect in 10s, start AP
                        platform::wifiStaStop();
                        startAP();
                    }
                }
                break;

            case State::ConnectedEth:
                if (!platform::ethConnected()) {
                    if constexpr (platform::hasWiFi) {
                        std::printf("NetworkModule: Ethernet dropped, cascading\n");
                        platform::mdnsStop();
                        if (ssid_[0] != 0 && platform::wifiStaInit(ssid_, password_)) {
                            state_ = State::WaitingSta;
                            stateChangeTime_ = now;
                        } else {
                            startAP();
                        }
                    } else {
                        // Ethernet-only build: drop back to polling for the cable.
                        std::printf("NetworkModule: Ethernet dropped\n");
                        platform::mdnsStop();
                        std::snprintf(statusStr_, sizeof(statusStr_), "No network (Ethernet only)"); setStatus(statusStr_, Severity::Error);
                        state_ = State::WaitingEth;
                        stateChangeTime_ = now;
                    }
                }
                updateStatusIP();
                break;

            case State::ConnectedSta:
                if constexpr (platform::hasWiFi) {
                    // Ethernet outranks WiFi: if a cable comes up while we are on
                    // WiFi STA, promote to Ethernet. onConnected() then shuts the
                    // WiFi STA down. Gated on ethConnected() (link + DHCP IP), not
                    // bare link-up, so WiFi is never dropped for a not-yet-working
                    // Ethernet — matches the State::AP upgrade check.
                    if (platform::ethConnected()) {
                        std::printf("NetworkModule: Ethernet up, switching from WiFi STA\n");
                        platform::mdnsStop();
                        onConnected("Ethernet");
                    } else if (!platform::wifiStaConnected()) {
                        std::printf("NetworkModule: WiFi STA dropped, starting AP\n");
                        platform::mdnsStop();
                        platform::wifiStaStop();
                        startAP();
                    } else {
                        updateStatusIP();
                    }
                }
                break;

            case State::AP:
                if constexpr (platform::hasWiFi) {
                    // Check if higher-priority connection became available
                    if (platform::ethConnected()) {
                        onConnected("Ethernet");
                    } else if (ssid_[0] != 0 && platform::wifiStaConnected()) {
                        onConnected("WiFi STA");
                    }
                }
                break;

            case State::Idle:
                break;
        }

        syncMdns();

        // Tick children after our own state machine — option A: parent prepares,
        // children consume. ImprovProvisioningModule (when present) polls a
        // ready-flag here and may call back into setWifiCredentials().
        MoonModule::loop1s();
    }

    void teardown() override {
        // Tear down children first (Improv on ESP32) so the platform-side
        // Improv task stops touching UART0 before we drop the network state.
        MoonModule::teardown();
        platform::mdnsStop();
        if constexpr (platform::hasWiFi) {
            if (state_ == State::AP) platform::wifiApStop();
            if (state_ == State::ConnectedSta || state_ == State::WaitingSta) platform::wifiStaStop();
        }
    }

private:
    Scheduler* scheduler_ = nullptr;
    SystemModule* systemModule_ = nullptr;

    enum class State : uint8_t {
        Idle,
        WaitingEth,
        WaitingSta,
        ConnectedEth,
        ConnectedSta,
        AP
    };

    State state_ = State::Idle;
    uint32_t stateChangeTime_ = 0;
    bool apShutdownPending_ = false;
    bool mdnsRunning_ = false;

    // Controls
    char ssid_[33] = {};
    char password_[64] = {};
    uint8_t addressing_ = 0; // 0=DHCP, 1=Static
    bool mdnsEnabled_ = true;
    char statusStr_[48] = {};

    // Static IP fields (only shown when addressing_==1)
    char staticIp_[16] = {};
    char staticGateway_[16] = {};
    char staticSubnet_[16] = "255.255.255.0";
    char staticDns_[16] = {};

    static constexpr const char* addressingOptions_[] = {"DHCP", "Static"};

    void startAP() {
        const char* apName = (systemModule_ && systemModule_->deviceName()[0] != 0)
                             ? systemModule_->deviceName() : "MM-AP";
        if (platform::wifiApInit(apName, "4.3.2.1")) {
            state_ = State::AP;
            stateChangeTime_ = platform::millis();
            apShutdownPending_ = true;
            std::snprintf(statusStr_, sizeof(statusStr_), "AP: %s @ 4.3.2.1", apName); setStatus(statusStr_, Severity::Status);
            std::printf("NetworkModule: AP started: %s\n", apName);
        } else {
            state_ = State::Idle;
            std::snprintf(statusStr_, sizeof(statusStr_), "No network"); setStatus(statusStr_, Severity::Error);
        }
        // statusStr_ is the buffer MoonModule::status_ points at — no control
        // rebuild needed (it isn't a control any more); just kick the scheduler
        // for any dependent reallocations.
        if (scheduler_) scheduler_->buildState();
    }

    void onConnected(const char* via) {
        if (std::strcmp(via, "Ethernet") == 0) {
            state_ = State::ConnectedEth;
        } else {
            state_ = State::ConnectedSta;
        }
        stateChangeTime_ = platform::millis();

        // Shut down lower-priority WiFi connections (no-op in the Ethernet-only build).
        if constexpr (platform::hasWiFi) {
            if (apShutdownPending_ || platform::wifiApConnected()) {
                std::printf("NetworkModule: Shutting down AP (higher priority connected)\n");
                platform::wifiApStop();
                apShutdownPending_ = false;
            }
            if (state_ == State::ConnectedEth && platform::wifiStaConnected()) {
                std::printf("NetworkModule: Shutting down WiFi STA (Ethernet connected)\n");
                platform::wifiStaStop();
            }
        }

        updateStatusIP();
        std::printf("NetworkModule: Connected via %s — %s\n", via, statusStr_);

        syncMdns();

        // statusStr_ is the buffer MoonModule::status_ points at — no control
        // rebuild needed (it isn't a control any more); just kick the scheduler
        // for any dependent reallocations.
        if (scheduler_) scheduler_->buildState();
    }

    void updateStatusIP() {
        char ip[16] = {};
        if (state_ == State::ConnectedEth) {
            platform::ethGetIP(ip, sizeof(ip));
            std::snprintf(statusStr_, sizeof(statusStr_), "Eth: %s", ip); setStatus(statusStr_, Severity::Status);
        } else if constexpr (platform::hasWiFi) {
            if (state_ == State::ConnectedSta) {
                platform::wifiStaGetIP(ip, sizeof(ip));
                std::snprintf(statusStr_, sizeof(statusStr_), "WiFi: %s", ip); setStatus(statusStr_, Severity::Status);
            }
        }
    }

    void syncMdns() {
        bool shouldRun = mdnsEnabled_ && (state_ == State::ConnectedEth || state_ == State::ConnectedSta);
        if (shouldRun && !mdnsRunning_) {
            const char* devName = (systemModule_ && systemModule_->deviceName()[0] != 0)
                                  ? systemModule_->deviceName() : "mm";
            // Only mark running on success — leave false so loop1s retries next tick
            if (platform::mdnsInit(devName)) {
                mdnsRunning_ = true;
            }
        } else if (!shouldRun && mdnsRunning_) {
            platform::mdnsStop();
            mdnsRunning_ = false;
        }
    }

};

} // namespace mm
