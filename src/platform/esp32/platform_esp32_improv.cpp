// Improv WiFi listener — UART0 RPC dispatch + credential pump.
//
// Cut out of platform_esp32.cpp (plan-23) for size + readability. Self-
// contained: the file owns the g_improv state in an anonymous namespace
// and only reads back into the rest of the platform layer through the
// public wifiStaConnected() / wifiStaGetIP() symbols declared in
// platform.h. Move was a code-organisation change with no API delta.
//
// Whole file is compiled out on Ethernet-only builds (MM_NO_WIFI). A
// link-parity stub at the bottom satisfies the platform.h declaration
// on those profiles (ImprovProvisioningModule guards the call with
// `if constexpr (hasImprov)`, so it's never invoked at runtime).

#include "platform/platform.h"

#ifndef MM_NO_WIFI

#include "core/ImprovFrame.h"

#include "driver/uart.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "improv.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mm::platform {
namespace {

// Improv-serial framing — see src/core/ImprovFrame.h. That header carries
// the parser, builder, and checksum, all unit-tested at test/test_improv_frame.cpp.
// This task only does the IO + RPC dispatch.

// Shared with the Improv task; const after init.
struct ImprovTaskState {
    char name[33] = {};               // copied from ImprovDeviceInfo at init
    char chipFamily[16] = {};
    char firmwareVersion[16] = {};
    char* ssidOut = nullptr;          // module-owned buffers (NetworkModule's
    char* passwordOut = nullptr;      // ssid_ / password_ via the module)
    size_t ssidOutLen = 0;
    size_t passwordOutLen = 0;
    std::atomic<bool>* ready = nullptr;   // module polls and clears
    char* statusBuf = nullptr;        // module shows as `provision_status`
    size_t statusBufLen = 0;
};
static ImprovTaskState g_improv;  // single global — only one Improv task per device

static void improvSetStatus(const char* fmt, ...) {
    if (!g_improv.statusBuf || g_improv.statusBufLen == 0) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(g_improv.statusBuf, g_improv.statusBufLen, fmt, args);
    va_end(args);
}

// Send a framed Improv message. ImprovFrameType values match the upstream
// improv::ImprovSerialType numerically (we just don't include improv.h in
// the host-side test path, so the host-only header has its own enum).
static void improvSend(ImprovFrameType type, const std::vector<uint8_t>& payload) {
    uint8_t frame[6 + 1 + 1 + 1 + kImprovMaxPayload + 1];
    size_t n = buildImprovFrame(type, payload.data(), payload.size(),
                                frame, sizeof(frame));
    if (n == 0) return;  // oversize payload — caller bug, silently drop
    uart_write_bytes(UART_NUM_0, reinterpret_cast<const char*>(frame), n);
}

static void improvSendCurrentState(improv::State state) {
    improvSend(ImprovFrameType::CurrentState, {static_cast<uint8_t>(state)});
}

static void improvSendError(improv::Error err) {
    improvSend(ImprovFrameType::ErrorState, {static_cast<uint8_t>(err)});
}

static void improvSendDeviceInfo() {
    // RPC response: [type=GET_DEVICE_INFO][len][n strings].
    std::vector<std::string> data = {
        "projectMM",                            // firmware name
        g_improv.firmwareVersion,
        g_improv.chipFamily,
        g_improv.name,
    };
    auto rpc = improv::build_rpc_response(improv::GET_DEVICE_INFO, data, false);
    improvSend(ImprovFrameType::RpcResponse, rpc);
}

static void improvSendWifiNetworks() {
    // Synchronous-ish scan. Replies one network per RPC frame per the Improv
    // spec, then a final empty payload to mark end-of-list. Limit to 10
    // entries to keep the response set bounded.
    wifi_scan_config_t scan_cfg = {};
    if (esp_wifi_scan_start(&scan_cfg, true /*block*/) != ESP_OK) {
        improvSendError(improv::ERROR_UNKNOWN);
        return;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 10) n = 10;
    wifi_ap_record_t records[10] = {};
    esp_wifi_scan_get_ap_records(&n, records);
    for (uint16_t i = 0; i < n; i++) {
        char rssi[8];
        std::snprintf(rssi, sizeof(rssi), "%d", static_cast<int>(records[i].rssi));
        std::vector<std::string> data = {
            reinterpret_cast<const char*>(records[i].ssid),
            rssi,
            records[i].authmode == WIFI_AUTH_OPEN ? "NO" : "YES",
        };
        auto rpc = improv::build_rpc_response(improv::GET_WIFI_NETWORKS, data, false);
        improvSend(ImprovFrameType::RpcResponse, rpc);
    }
    // End-of-list sentinel: empty payload.
    improvSend(ImprovFrameType::RpcResponse,
               improv::build_rpc_response(improv::GET_WIFI_NETWORKS, {}, false));
}

// On WIFI_SETTINGS command: stash credentials for the module to consume.
// The module's loop1s() polls `g_improv.ready` and calls
// NetworkModule::setWifiCredentials, which writes through to the existing
// wifiStaInit path. We don't call wifiStaInit from this task because we
// don't want WiFi-driver work on the Improv parser task's stack.
static void improvHandleProvision(const improv::ImprovCommand& cmd) {
    if (wifiStaConnected()) {
        improvSetStatus("error: already connected");
        improvSendError(improv::ERROR_UNABLE_TO_CONNECT);
        return;
    }
    improvSetStatus("received credentials");
    std::strncpy(g_improv.ssidOut, cmd.ssid.c_str(), g_improv.ssidOutLen - 1);
    g_improv.ssidOut[g_improv.ssidOutLen - 1] = 0;
    std::strncpy(g_improv.passwordOut, cmd.password.c_str(), g_improv.passwordOutLen - 1);
    g_improv.passwordOut[g_improv.passwordOutLen - 1] = 0;
    // release-store: pairs with the module's acquire-load in loop1s() so the
    // SSID/password buffer writes above are visible before the consumer sees
    // ready=true (matters on the dual-core ESP32-S3; single-core ESP32 is a
    // no-op but the explicit ordering documents intent).
    g_improv.ready->store(true, std::memory_order_release);
    improvSendCurrentState(improv::STATE_PROVISIONING);

    // Wait up to 30 s for IP. Polls existing platform state — no extra wiring.
    char ip[32] = {};
    for (int i = 0; i < 300; i++) {  // 30 s @ 100 ms
        vTaskDelay(pdMS_TO_TICKS(100));
        if (wifiStaConnected()) {
            wifiStaGetIP(ip, sizeof(ip));
            break;
        }
    }
    if (!wifiStaConnected()) {
        improvSetStatus("error: no IP after 30s");
        improvSendError(improv::ERROR_UNABLE_TO_CONNECT);
        return;
    }
    improvSetStatus("connected: %s", cmd.ssid.c_str());
    // Success frame: RPC response carrying the device URL.
    char url[64];
    std::snprintf(url, sizeof(url), "http://%s/", ip);
    std::vector<std::string> urls = { url };
    auto rpc = improv::build_rpc_response(improv::WIFI_SETTINGS, urls, false);
    improvSend(ImprovFrameType::RpcResponse, rpc);
    improvSendCurrentState(improv::STATE_PROVISIONED);
}

// Dispatch a completed frame from the parser. Only RPC frames carry commands
// we care about; the spec lets the other types through silently.
static void improvDispatchFrame(const ImprovFrameParser& parser) {
    if (parser.lastType() != improv::TYPE_RPC) return;
    improv::ImprovCommand cmd = improv::parse_improv_data(
        parser.lastPayload(), parser.lastPayloadLen(), false);
    switch (cmd.command) {
        case improv::GET_CURRENT_STATE:
            if (wifiStaConnected()) {
                improvSendCurrentState(improv::STATE_PROVISIONED);
                // Follow up the state frame with the device URL on the
                // WIFI_SETTINGS RPC, the way ESPHome does. The state frame
                // alone tells a tool the device is on WiFi but doesn't say
                // *where*; the URL follow-up makes the protocol self-
                // describing on every reconnect (any future Improv client —
                // a browser tab post-refresh, a re-run of improv_probe.py,
                // another tool — can find the device without re-provisioning).
                // ESP Web Tools' current rich-panel "Visit Device" affordance
                // is in-session-only, so this doesn't visibly change its UI;
                // the value is protocol completeness, observable via
                // improv_probe.py.
                char ip[32] = {};
                wifiStaGetIP(ip, sizeof(ip));
                if (ip[0]) {
                    char url[64];
                    std::snprintf(url, sizeof(url), "http://%s/", ip);
                    std::vector<std::string> urls = { url };
                    auto rpc = improv::build_rpc_response(improv::WIFI_SETTINGS, urls, false);
                    improvSend(ImprovFrameType::RpcResponse, rpc);
                }
            } else {
                improvSendCurrentState(improv::STATE_AUTHORIZED);
            }
            break;
        case improv::GET_DEVICE_INFO: improvSendDeviceInfo(); break;
        case improv::GET_WIFI_NETWORKS:
            // Refuse scans while WiFi STA is connected — esp_wifi_scan_start
            // puts the radio into scan mode for 2-5 s, dropping inbound ArtNet.
            // On large installs (16K+ LEDs) that's a visible glitch. The state
            // returned by GET_CURRENT_STATE already tells the browser the device
            // is online; a scan adds no new diagnostic value once provisioned.
            if (wifiStaConnected()) improvSendError(improv::ERROR_UNABLE_TO_CONNECT);
            else                    improvSendWifiNetworks();
            break;
        case improv::WIFI_SETTINGS: improvHandleProvision(cmd); break;
        default:                    improvSendError(improv::ERROR_UNKNOWN_RPC); break;
    }
}

static void improvTask(void* /*arg*/) {
    // Driver install. UART0 is already configured at 115200-8N1 by the
    // bootloader; we just claim the interrupt + RX FIFO. RX buf 256 is
    // plenty (Improv RPC payloads max out around 96 bytes).
    uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0);

    improvSetStatus("listening");

    ImprovFrameParser parser;  // owns its 128-byte payload buffer; ~150 B on stack
    uint8_t b;
    for (;;) {
        int n = uart_read_bytes(UART_NUM_0, &b, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        switch (parser.feed(b)) {
            case ImprovFeedResult::NeedMore:
                break;
            case ImprovFeedResult::FrameReady:
                improvDispatchFrame(parser);
                break;
            case ImprovFeedResult::BadChecksum:
                improvSendError(improv::ERROR_INVALID_RPC);
                break;
            case ImprovFeedResult::OversizePayload:
                // Length byte > 128 — almost certainly noise / bit-flip; resync silently.
                break;
        }
    }
}

} // anonymous namespace

bool improvProvisioningInit(const ImprovDeviceInfo& info,
                            char* ssidOut, size_t ssidOutLen,
                            char* passwordOut, size_t passwordOutLen,
                            std::atomic<bool>* ready,
                            char* statusBuf, size_t statusBufLen) {
    if (!info.name || !info.chipFamily || !info.firmwareVersion ||
        !ssidOut || ssidOutLen == 0 ||
        !passwordOut || passwordOutLen == 0 ||
        !ready || !statusBuf || statusBufLen == 0) {
        return false;
    }
    std::strncpy(g_improv.name, info.name, sizeof(g_improv.name) - 1);
    std::strncpy(g_improv.chipFamily, info.chipFamily, sizeof(g_improv.chipFamily) - 1);
    std::strncpy(g_improv.firmwareVersion, info.firmwareVersion, sizeof(g_improv.firmwareVersion) - 1);
    g_improv.ssidOut = ssidOut;
    g_improv.ssidOutLen = ssidOutLen;
    g_improv.passwordOut = passwordOut;
    g_improv.passwordOutLen = passwordOutLen;
    g_improv.ready = ready;
    g_improv.statusBuf = statusBuf;
    g_improv.statusBufLen = statusBufLen;

    // 6 KB stack: parser is small, scan response uses std::vector + std::string
    // (some short-string-optimised, some heap). Priority 4 — below OTA (5),
    // above idle. Single task per device; not pinned to a core.
    BaseType_t ok = xTaskCreate(&improvTask, "improv", 6144, nullptr, 4, nullptr);
    if (ok != pdPASS) {
        improvSetStatus("error: task create failed");
        return false;
    }
    return true;
}

} // namespace mm::platform

#else // MM_NO_WIFI — Ethernet-only build: no Improv listener.

#include <atomic>
#include <cstdio>

namespace mm::platform {

// Stub for link parity. ImprovProvisioningModule guards the call with
// `if constexpr (hasImprov)`, which evaluates false on MM_NO_WIFI builds —
// so this is never invoked at runtime. Kept as a symbol so the platform.h
// declaration links cleanly on every build profile.
bool improvProvisioningInit(const ImprovDeviceInfo& /*info*/,
                            char* /*ssidOut*/,    size_t /*ssidOutLen*/,
                            char* /*passwordOut*/, size_t /*passwordOutLen*/,
                            std::atomic<bool>* /*ready*/,
                            char* statusBuf, size_t statusBufLen) {
    if (statusBuf && statusBufLen > 0) {
        std::snprintf(statusBuf, statusBufLen, "not supported (no WiFi)");
    }
    return false;
}

} // namespace mm::platform

#endif // MM_NO_WIFI
