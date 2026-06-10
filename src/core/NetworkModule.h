#pragma once

#include "core/MoonModule.h"
#include "core/Scheduler.h"
#include "core/SystemModule.h"
#include "core/FilesystemModule.h"
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

    // Improv SET_TX_POWER path: persist + apply the TX-power cap (whole dBm,
    // 0 = lift). Must run BEFORE setWifiCredentials when both arrive from one
    // provisioning flow — brown-out-prone boards (LOLIN S3) fail WiFi auth at
    // full power, so the cap has to be in place for the association attempt.
    void setTxPowerSetting(uint8_t dBm) {
        if (dBm > 21) return;
        txPowerSetting_ = dBm;
        markDirty();
        FilesystemModule::noteDirty();   // same persist arming as setWifiCredentials
        syncTxPower();                   // applies now if the radio is up; the
                                         // STA-start path re-applies otherwise
    }

    void setWifiCredentials(const char* ssid, const char* password) {
        if (!ssid) return;
        std::strncpy(ssid_, ssid, sizeof(ssid_) - 1);
        ssid_[sizeof(ssid_) - 1] = 0;
        std::strncpy(password_, password ? password : "", sizeof(password_) - 1);
        password_[sizeof(password_) - 1] = 0;
        markDirty();
        FilesystemModule::noteDirty();   // start the debounce so the change actually flushes
                                         // (markDirty alone only sets the bit; the save scheduler
                                         // needs noteDirty to arm — Improv-pushed creds would
                                         // otherwise persist only if some other control changed)
        if constexpr (platform::hasWiFi) {
            // Tear down any prior WiFi state (AP-fallback, mid-flight STA
            // attempt, or stale init from a previous reconfigure) so the
            // platform's event-handler registration runs fresh.
            if (state_ == State::AP) {
                platform::wifiApStop();
                noteRadioStopped();
                apShutdownPending_ = false;
            }
            if (state_ == State::WaitingSta || state_ == State::ConnectedSta) {
                platform::wifiStaStop();
                noteRadioStopped();
            }
            if (platform::wifiStaInit(ssid_, password_)) {
                state_ = State::WaitingSta;
                stateChangeTime_ = platform::millis();
                // Apply the TX-power cap NOW, before the radio's first
                // probe / auth / assoc burst — that's the window the
                // LOLIN brown-out fix exists to protect. Waiting for the
                // next loop1s() tick to syncTxPower would leave up to
                // 1 s of full-power TX during association, the exact
                // failure mode the cap defends against. syncTxPower
                // itself is cheap and idempotent.
                syncTxPower();
                std::snprintf(statusBuf_, sizeof(statusBuf_), "WiFi STA: %s", ssid_);
                setStatus(statusBuf_, Severity::Status);
                // Re-evaluate control visibility — rssi was visible while
                // state_ was ConnectedSta (any prior call to wifiStaConnected)
                // and would otherwise stay rendered with a now-stale reading
                // until the cascade either reconnects (onConnected rebuilds)
                // or falls back to AP (startAP rebuilds). Match those paths.
                rebuildControls();
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
                syncTxPower();  // see setWifiCredentials's syncTxPower comment
                std::printf("NetworkModule: WiFi STA init started, SSID: %s\n", ssid_);
            } else {
                startAP();
            }
        } else {
            // Ethernet-only build: no WiFi fallback. Stay Idle until a cable
            // appears (WaitingEth is only entered on a successful ethInit()).
            state_ = State::Idle;
            std::snprintf(statusBuf_, sizeof(statusBuf_), "No network (Ethernet only)"); setStatus(statusBuf_, Severity::Error);
        }

        stateChangeTime_ = platform::millis();

        // Chain to base so children (ImprovProvisioningModule on ESP32) get setup()
        // after we've claimed the network resources we care about.
        MoonModule::setup();
    }

    void onBuildControls() override {
        // Chain to base FIRST so children (Improv on ESP32) register their
        // controls before NetworkModule appends its own — per the override-
        // and-chain convention in docs/coding-standards.md § Override-and-
        // chain ("onBuildControls — chain first, then parent work").
        // Earlier shape called this at the end, which inverted the order
        // (parent's controls landed before children's).
        MoonModule::onBuildControls();

        setStatus(statusBuf_);

        // Refresh the live-readout values (mode label + rssi + txPower) so a
        // rebuild triggered mid-state-transition shows the up-to-date numbers.
        updateMetrics();

        // `mode` reflects the state-machine state in plain language. Always
        // present (every firmware variant has a mode, even Ethernet-only).
        controls_.addReadOnly("mode", modeStr_, sizeof(modeStr_));

        // WiFi credential controls are absent in the Ethernet-only build.
        if constexpr (platform::hasWiFi) {
            controls_.addText("ssid", ssid_, sizeof(ssid_));
            controls_.addPassword("password", password_, sizeof(password_));
            // RSSI is meaningful only while associated as a STA. Hide on
            // Ethernet / AP / Idle to avoid showing a stale 0 dBm reading.
            controls_.addReadOnlyInt("rssi", rssi_, "dBm");
            controls_.setHidden(controls_.count() - 1, state_ != State::ConnectedSta);
            // TX power applies whenever the WiFi radio is active (STA or AP).
            // Hide on Ethernet / Idle where the radio is off.
            controls_.addReadOnlyInt("txPower", txPower_, "dBm");
            const bool radioOn = (state_ == State::ConnectedSta
                                  || state_ == State::WaitingSta
                                  || state_ == State::AP);
            controls_.setHidden(controls_.count() - 1, !radioOn);
            // Writable TX-power cap (LOLIN WiFi fix). Range 0..21 dBm.
            // 0 = "no override" (sentinel — syncTxPower then writes the
            // ESP-IDF ceiling, ~20 dBm, to actively lift any prior cap;
            // setting back to 0 truly restores default power). 1 is in
            // the bound but the platform layer clamps it up to 2 dBm
            // (ESP-IDF's minimum) — write 2 or higher for predictable
            // behavior. Always bound on radio-capable builds; the
            // boards.json catalog injects 8 dBm for LOLIN boards.
            controls_.addInt16("txPowerSetting", txPowerSetting_, 0, 21);
        }
        controls_.addBool("mDNS", mdnsEnabled_);

        // addressing goes immediately before the static-IP fields it conditions, so
        // the dropdown and the fields it reveals stay adjacent (mDNS, unrelated,
        // sits above rather than wedged between them).
        controls_.addSelect("addressing", addressing_, addressingOptions_, 2);

        // Static-IP fields are always bound (so persistence can load them at any time),
        // but visibility flips based on addressing mode. Toggling the Select triggers a
        // rebuildControls() in HttpServerModule which re-runs this method and re-evaluates
        // the hidden flags.
        const bool hideStatic = (addressing_ != 1);
        controls_.addIPv4("ip", staticIp_);
        controls_.setHidden(controls_.count() - 1, hideStatic);
        controls_.addIPv4("gateway", staticGateway_);
        controls_.setHidden(controls_.count() - 1, hideStatic);
        controls_.addIPv4("subnet", staticSubnet_);
        controls_.setHidden(controls_.count() - 1, hideStatic);
        controls_.addIPv4("dns", staticDns_);
        controls_.setHidden(controls_.count() - 1, hideStatic);
        // Chain to base is at the top of this method — see comment there.
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
                            syncTxPower();  // see setWifiCredentials's syncTxPower comment
                        } else {
                            startAP();
                        }
                    } else {
                        // Ethernet-only build: no fallback. Keep polling for a cable.
                        std::snprintf(statusBuf_, sizeof(statusBuf_), "No network (Ethernet only)"); setStatus(statusBuf_, Severity::Error);
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
                        noteRadioStopped();
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
                            syncTxPower();  // see setWifiCredentials's syncTxPower comment
                        } else {
                            startAP();
                        }
                    } else {
                        // Ethernet-only build: drop back to polling for the cable.
                        std::printf("NetworkModule: Ethernet dropped\n");
                        platform::mdnsStop();
                        std::snprintf(statusBuf_, sizeof(statusBuf_), "No network (Ethernet only)"); setStatus(statusBuf_, Severity::Error);
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
                        noteRadioStopped();
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
                // Recovery from a terminal-looking state. We land in Idle when
                // every bring-up path failed: Ethernet didn't appear within the
                // boot timeout, WiFi STA wasn't configured (or wasn't reachable),
                // and AP fallback failed to init. In Ethernet-only builds we
                // also land here when setup() can't ethInit(). The network
                // stack keeps running in the background though — if Ethernet
                // later acquires a DHCP lease (slow DHCP server, cable plugged
                // in after boot), ethConnected() flips true. Promote when we
                // see it; symmetric with the State::AP and State::ConnectedSta
                // upgrade checks above. Same for late WiFi STA in builds with
                // saved credentials.
                if (platform::ethConnected()) {
                    std::printf("NetworkModule: Ethernet up (recovered from Idle)\n");
                    onConnected("Ethernet");
                } else if constexpr (platform::hasWiFi) {
                    if (platform::wifiStaConnected()) {
                        std::printf("NetworkModule: WiFi STA up (recovered from Idle)\n");
                        onConnected("WiFi STA");
                    }
                }
                break;
        }

        syncMdns();
        syncTxPower();

        // Refresh the live-readout values every tick — the UI polls /api/state
        // for them, so writing the same storage addresses is enough; no
        // control rebuild needed. (Hidden-flag changes happen on state
        // transitions via rebuildControls(), not here.)
        updateMetrics();

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
            if (state_ == State::AP) { platform::wifiApStop(); noteRadioStopped(); }
            if (state_ == State::ConnectedSta || state_ == State::WaitingSta) {
                platform::wifiStaStop();
                noteRadioStopped();
            }
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
    // Module-owned backing store for the status slot inherited from MoonModule.
    // The base class only holds a const char* into this buffer (see
    // MoonModule::status_); the named "Buf" suffix makes the ownership clear
    // and distinguishes it from MoonModule's own status accessors.
    char statusBuf_[48] = {};

    // Static IP fields. uint8_t[4] octets, not strings — saves 12 bytes per
    // address vs char[16] dotted-quad, and the wire/persistence layers
    // (ControlType::IPv4) handle the string conversion at the boundary.
    // Only shown in the UI when addressing_==1 (Static); always bound for
    // persistence so toggling DHCP↔Static doesn't lose user-set values.
    uint8_t staticIp_[4]      = {0, 0, 0, 0};
    uint8_t staticGateway_[4] = {0, 0, 0, 0};
    uint8_t staticSubnet_[4]  = {255, 255, 255, 0};
    uint8_t staticDns_[4]     = {0, 0, 0, 0};

    // Read-only metrics surfaced to the UI.
    // - modeStr_ stays a buffer (state labels are short strings, no
    //   precedent for pointer-to-literal controls today).
    // - rssi_ / txPower_ are int8 — addReadOnlyInt stores them directly
    //   instead of formatting "<value> dBm" into per-control buffers
    //   (saves ~22 bytes vs the prior char[12] approach).
    char modeStr_[20] = {};   // longest label "Ethernet (waiting)" = 19+NUL
    int8_t rssi_ = 0;
    int8_t txPower_ = 0;

    // User-settable TX-power cap in whole dBm (0..21). Default 0 = "no
    // override". Persisted via the control binding. The platform setter
    // takes quarter-dBm (ESP-IDF's native unit), so syncTxPower() multiplies
    // by 4 at the call site. appliedTxPowerSetting_ tracks the last value
    // pushed to the radio so syncTxPower() in loop1s() detects changes (UI
    // write or board-injected value) and re-applies without needing a
    // per-control change callback.
    int16_t txPowerSetting_ = 0;
    int16_t appliedTxPowerSetting_ = -1;   // -1 = never applied, forces first sync

    static constexpr const char* addressingOptions_[] = {"DHCP", "Static"};

    void startAP() {
        const char* apName = (systemModule_ && systemModule_->deviceName()[0] != 0)
                             ? systemModule_->deviceName() : "MM-AP";
        if (platform::wifiApInit(apName, "4.3.2.1")) {
            state_ = State::AP;
            stateChangeTime_ = platform::millis();
            apShutdownPending_ = true;
            syncTxPower();  // see setWifiCredentials's syncTxPower comment
            std::snprintf(statusBuf_, sizeof(statusBuf_), "AP: %s @ 4.3.2.1", apName); setStatus(statusBuf_, Severity::Status);
            std::printf("NetworkModule: AP started: %s\n", apName);
        } else {
            state_ = State::Idle;
            std::snprintf(statusBuf_, sizeof(statusBuf_), "No network"); setStatus(statusBuf_, Severity::Error);
        }
        // statusBuf_ is the buffer MoonModule::status_ points at — no control
        // rebuild needed for status itself, but rssi/txPower visibility depends
        // on state_ so rebuildControls() re-evaluates their hidden flags.
        rebuildControls();
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
                noteRadioStopped();
                apShutdownPending_ = false;
            }
            if (state_ == State::ConnectedEth && platform::wifiStaConnected()) {
                std::printf("NetworkModule: Shutting down WiFi STA (Ethernet connected)\n");
                platform::wifiStaStop();
                noteRadioStopped();
            }
        }

        updateStatusIP();
        std::printf("NetworkModule: Connected via %s — %s\n", via, statusBuf_);

        syncMdns();

        // statusBuf_ is the buffer MoonModule::status_ points at — no control
        // rebuild needed for status itself, but rssi/txPower visibility depends
        // on state_ so rebuildControls() re-evaluates their hidden flags.
        rebuildControls();
        if (scheduler_) scheduler_->buildState();
    }

    void updateStatusIP() {
        char ip[16] = {};
        if (state_ == State::ConnectedEth) {
            platform::ethGetIP(ip, sizeof(ip));
            std::snprintf(statusBuf_, sizeof(statusBuf_), "Eth: %s", ip); setStatus(statusBuf_, Severity::Status);
        } else if constexpr (platform::hasWiFi) {
            if (state_ == State::ConnectedSta) {
                platform::wifiStaGetIP(ip, sizeof(ip));
                std::snprintf(statusBuf_, sizeof(statusBuf_), "WiFi: %s", ip); setStatus(statusBuf_, Severity::Status);
            }
        }
    }

    // Apply txPowerSetting_ to the radio whenever it changes (UI write,
    // board-injected value, or first time it lands after STA/AP comes up).
    // Mirrors syncMdns()'s shape: cheap idempotent check, called from
    // loop1s(). esp_wifi_set_max_tx_power requires the WiFi stack started
    // — wifiSetTxPower() guards on that and returns false otherwise, which
    // leaves appliedTxPowerSetting_ untouched so the next tick (post-STA-
    // up) retries cleanly.
    void syncTxPower() {
        if constexpr (!platform::hasWiFi) return;
        if (txPowerSetting_ == appliedTxPowerSetting_) return;
        const bool radioUp = (state_ == State::ConnectedSta
                              || state_ == State::WaitingSta
                              || state_ == State::AP);
        if (!radioUp) return;
        // Convert dBm (user-facing) → quarter-dBm (ESP-IDF native). The
        // 0 sentinel ("no override") needs to actively undo any prior cap
        // — esp_wifi_set_max_tx_power has no "reset to default" call, so
        // we push the ceiling (80 = 20 dBm) instead. Without this the
        // cap would be sticky until reboot: setting back to 0 in the UI
        // would silently leave the radio at the prior cap.
        const int8_t quarterDbm = (txPowerSetting_ == 0)
                                  ? static_cast<int8_t>(80)
                                  : static_cast<int8_t>(txPowerSetting_ * 4);
        if (platform::wifiSetTxPower(quarterDbm)) {
            appliedTxPowerSetting_ = txPowerSetting_;
        }
    }

    // Invalidate the "last applied" tracker so the next syncTxPower()
    // re-applies the cap. Must be called every time the WiFi stack stops
    // (wifiStaStop / wifiApStop / teardown): ESP-IDF resets the radio's
    // TX-power state on stop, so our cached `applied` value no longer
    // reflects what the radio thinks. Without this, the equality check
    // in syncTxPower() short-circuits and the cap never lands on the
    // restarted radio — the LOLIN board would associate at full power
    // (brown-out hazard) until the user touched the control to force a
    // resync.
    void noteRadioStopped() { appliedTxPowerSetting_ = -1; }

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

    // Map State → human label for the `mode` control. Kept here (not a static
    // table) so a new State enumerator forces a compiler error rather than
    // silently falling back to "Unknown" in the UI.
    const char* modeLabel() const {
        switch (state_) {
            case State::Idle:         return "Idle";
            case State::WaitingEth:   return "Ethernet (waiting)";
            case State::WaitingSta:   return "WiFi STA (waiting)";
            case State::ConnectedEth: return "Ethernet";
            case State::ConnectedSta: return "WiFi STA";
            case State::AP:           return "WiFi AP";
        }
        return "Unknown";
    }

    void updateMetrics() {
        std::snprintf(modeStr_, sizeof(modeStr_), "%s", modeLabel());
        if constexpr (platform::hasWiFi) {
            // rssi_ / txPower_ are hidden in non-WiFi states but we still
            // refresh them so a transition back to a WiFi state shows fresh
            // data without a one-tick stale read. Zeroing on non-WiFi states
            // avoids leaving a stale 5-minute-old reading visible if the
            // user toggles the hidden flag off via DevTools.
            rssi_ = (state_ == State::ConnectedSta)
                    ? static_cast<int8_t>(platform::wifiStaRssi()) : 0;
            const bool radioOn = (state_ == State::ConnectedSta
                                  || state_ == State::WaitingSta
                                  || state_ == State::AP);
            txPower_ = radioOn ? static_cast<int8_t>(platform::wifiTxPower()) : 0;
        }
    }

};

} // namespace mm
