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

    void setup() override {
        // Try Ethernet first (non-blocking)
        if (platform::ethInit()) {
            state_ = State::WaitingEth;
            std::printf("NetworkModule: Ethernet init started\n");
        } else if (ssid_[0] != 0) {
            // Ethernet not available, try WiFi STA
            if (platform::wifiStaInit(ssid_, password_)) {
                state_ = State::WaitingSta;
                std::printf("NetworkModule: WiFi STA init started, SSID: %s\n", ssid_);
            } else {
                startAP();
            }
        } else {
            startAP();
        }

        stateChangeTime_ = platform::millis();
    }

    void onBuildControls() override {
        controls_.addReadOnly("status", statusStr_, sizeof(statusStr_));
        controls_.addText("ssid", ssid_, sizeof(ssid_));
        controls_.addText("password", password_, sizeof(password_));
        controls_.addSelect("addressing", addressing_, addressingOptions_, 2);
        controls_.addBool("mDNS", mdnsEnabled_);

        // Dynamic: show static IP fields when addressing==1
        if (addressing_ == 1) {
            controls_.addText("ip", staticIp_, sizeof(staticIp_));
            controls_.addText("gateway", staticGateway_, sizeof(staticGateway_));
            controls_.addText("subnet", staticSubnet_, sizeof(staticSubnet_));
            controls_.addText("dns", staticDns_, sizeof(staticDns_));
        }
    }

    void loop1s() override {
        uint32_t now = platform::millis();
        uint32_t elapsed = now - stateChangeTime_;

        switch (state_) {
            case State::WaitingEth:
                if (platform::ethConnected()) {
                    onConnected("Ethernet");
                } else if ((elapsed > 3000 && !platform::ethLinkUp()) || elapsed > 15000) {
                    // No cable after 3s, or link up but no IP after 15s — cascade to WiFi
                    std::printf("NetworkModule: Ethernet %s, cascading\n",
                                platform::ethLinkUp() ? "no IP (DHCP timeout)" : "no link (no cable)");
                    if (ssid_[0] != 0) {
                        if (platform::wifiStaInit(ssid_, password_)) {
                            state_ = State::WaitingSta;
                            stateChangeTime_ = now;
                        } else {
                            startAP();
                        }
                    } else {
                        startAP();
                    }
                }
                break;

            case State::WaitingSta:
                if (platform::wifiStaConnected()) {
                    onConnected("WiFi STA");
                } else if (elapsed > 10000) {
                    // WiFi STA didn't connect in 10s, start AP
                    platform::wifiStaStop();
                    startAP();
                }
                break;

            case State::ConnectedEth:
                if (!platform::ethConnected()) {
                    std::printf("NetworkModule: Ethernet dropped, cascading\n");
                    platform::mdnsStop();
                    if (ssid_[0] != 0) {
                        if (platform::wifiStaInit(ssid_, password_)) {
                            state_ = State::WaitingSta;
                            stateChangeTime_ = now;
                        } else {
                            startAP();
                        }
                    } else {
                        startAP();
                    }
                }
                updateStatusIP();
                break;

            case State::ConnectedSta:
                if (!platform::wifiStaConnected()) {
                    std::printf("NetworkModule: WiFi STA dropped, starting AP\n");
                    platform::mdnsStop();
                    platform::wifiStaStop();
                    startAP();
                }
                updateStatusIP();
                break;

            case State::AP:
                // Check if higher-priority connection became available
                if (platform::ethConnected()) {
                    onConnected("Ethernet");
                } else if (ssid_[0] != 0 && platform::wifiStaConnected()) {
                    onConnected("WiFi STA");
                }
                break;

            case State::Idle:
                break;
        }

        syncMdns();
    }

    void teardown() override {
        platform::mdnsStop();
        if (state_ == State::AP) platform::wifiApStop();
        if (state_ == State::ConnectedSta || state_ == State::WaitingSta) platform::wifiStaStop();
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
            std::snprintf(statusStr_, sizeof(statusStr_), "AP: %s @ 4.3.2.1", apName);
            std::printf("NetworkModule: AP started: %s\n", apName);
        } else {
            state_ = State::Idle;
            std::snprintf(statusStr_, sizeof(statusStr_), "No network");
        }
        rebuildLocalControlsAndPipeline();
    }

    void onConnected(const char* via) {
        if (std::strcmp(via, "Ethernet") == 0) {
            state_ = State::ConnectedEth;
        } else {
            state_ = State::ConnectedSta;
        }
        stateChangeTime_ = platform::millis();

        // Shut down lower-priority connections
        if (apShutdownPending_ || platform::wifiApConnected()) {
            std::printf("NetworkModule: Shutting down AP (higher priority connected)\n");
            platform::wifiApStop();
            apShutdownPending_ = false;
        }
        if (state_ == State::ConnectedEth && platform::wifiStaConnected()) {
            std::printf("NetworkModule: Shutting down WiFi STA (Ethernet connected)\n");
            platform::wifiStaStop();
        }

        updateStatusIP();
        std::printf("NetworkModule: Connected via %s — %s\n", via, statusStr_);

        syncMdns();

        rebuildLocalControlsAndPipeline();
    }

    void updateStatusIP() {
        char ip[16] = {};
        if (state_ == State::ConnectedEth) {
            platform::ethGetIP(ip, sizeof(ip));
            std::snprintf(statusStr_, sizeof(statusStr_), "Eth: %s", ip);
        } else if (state_ == State::ConnectedSta) {
            platform::wifiStaGetIP(ip, sizeof(ip));
            std::snprintf(statusStr_, sizeof(statusStr_), "WiFi: %s", ip);
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

    // Rebuilds local control set AND triggers a pipeline-level allocate. Used after the
    // status changes (connected/AP/etc.) which alters statusStr_ and may toggle the addressing
    // conditional. The base rebuildControls() handles the controls clear+rebuild; we just add
    // the scheduler kick.
    void rebuildLocalControlsAndPipeline() {
        rebuildControls();
        if (scheduler_) scheduler_->rebuild();
    }
};

} // namespace mm
