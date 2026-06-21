// Improv-serial listener — UART0 + native-USB RPC dispatch.
//
// Cut out of platform_esp32.cpp (plan-23) for size + readability. Self-contained:
// owns the g_improv state in an anonymous namespace, reaching back into the rest of
// the platform layer only through public accessors declared in platform.h.
//
// Runs on EVERY ESP32 target, including Ethernet-only builds (MM_NO_WIFI). The serial
// transport + the vendor RPCs (SET_DEVICE_MODEL, SET_TX_POWER, APPLY_OP — "Improv =
// REST over serial") need no WiFi, so the installer can push a device-model's config
// over serial to an eth device too. Only the WiFi-PROVISIONING RPCs (WIFI_SETTINGS,
// GET_WIFI_NETWORKS) and their `esp_wifi_*` calls are `#ifndef MM_NO_WIFI`-guarded —
// on eth those commands aren't offered (there's no WiFi STA to provision), and
// GET_CURRENT_STATE reports based on the Ethernet link instead.

#include "platform/platform.h"

#include "core/ImprovFrame.h"
#include "core/ImprovOpReassembler.h"

#include "driver/uart.h"
#include "esp_log.h"
#ifndef MM_NO_WIFI
#include "esp_wifi.h"   // only the WiFi-provisioning RPCs touch esp_wifi_*
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "improv.h"
#include "soc/soc_caps.h"

// USB-Serial-JTAG: ESP32-S3 / S2 / C3 / C6 have a built-in USB-Serial-JTAG
// peripheral that exposes a USB-CDC endpoint without an external bridge chip.
// Many cheap S3 dev boards wire the USB-C port to
// this peripheral, not to UART0 — meaning Improv RPC bytes from the host
// arrive on USB-Serial-JTAG, not UART0. Listen on BOTH so the same firmware
// works on boards with an external USB-Serial bridge (UART0) AND boards
// with native USB (USB-Serial-JTAG). Soft-fail: if the driver install
// errors (rare; usually means the secondary console grabbed it first), we
// just skip the JTAG path and keep UART0.
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#endif

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

    // Vendor SET_DEVICE_MODEL RPC (command 0xFE): module-owned deviceModel buffer
    // sized by SystemModule::deviceModel_ at the caller (see deviceModelOutLen). The
    // Improv handler caps str_len dynamically against deviceModelOutLen so the
    // wire spec adapts when the buffer resizes. Same producer/consumer
    // dance as ssid/password.
    // Nullable: opt out by leaving null (desktop stub doesn't pass any).
    char* deviceModelOut = nullptr;
    size_t deviceModelOutLen = 0;
    std::atomic<bool>* deviceModelReady = nullptr;

    // Vendor SET_TX_POWER RPC (command 0xFD): pre-association TX-power cap in
    // whole dBm for brown-out-prone boards. Same producer/consumer dance.
    uint8_t* txPowerOut = nullptr;
    std::atomic<bool>* txPowerReady = nullptr;

    // Vendor APPLY_OP RPC (command 0xFC): one REST operation as JSON, pushed over
    // serial during provisioning ("Improv = REST over serial"). The frame carries
    // [0xFC][seq][last][chunk bytes…]; chunks are appended to opOut until last=1,
    // then opReady is set and the module's loop applies the op on the MAIN loop
    // (never the Improv task). Same producer/consumer dance as deviceModel; the
    // buffer is module-owned and sized to hold the largest op (a long pins list).
    // Chunk reassembly + the sequence guard live in mm::ImprovOpReassembler, bound
    // to opOut in the handler — only the buffer + the ready flag are shared state here.
    char* opOut = nullptr;
    size_t opOutLen = 0;
    std::atomic<bool>* opReady = nullptr;
};
static ImprovTaskState g_improv;  // single global — only one Improv task per device

static const char* IMPROV_TAG = "mm_improv";  // ESP_LOG* tag for the Improv task

static void improvSetStatus(const char* fmt, ...) {
    if (!g_improv.statusBuf || g_improv.statusBufLen == 0) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(g_improv.statusBuf, g_improv.statusBufLen, fmt, args);
    va_end(args);
}

// Tracks whether the USB-Serial-JTAG read driver is up. Set by improvTask
// after a successful install; gates the JTAG TX in improvSend so a board
// without the driver (install failed, or ESP32-classic) doesn't write
// into a dead peripheral. Read+write happen on the same task so plain bool
// is fine — no cross-task memory ordering concerns.
#if SOC_USB_SERIAL_JTAG_SUPPORTED
static bool g_jtagReady = false;
#endif

// Source-transport routing for replies. improvSend uses this to send a
// reply ONLY on the transport that received the request that triggered
// it, rather than broadcasting to both. The Improv task is single-
// threaded and processes one frame at a time, so a single static here
// is sufficient — set before improvDispatchFrame fires, read by every
// improvSend* call within the dispatch (including the synchronous
// pre-WiFi-result sends inside improvHandleProvision's 30 s wait, since
// that wait blocks the same task and no other dispatch can start).
// Broadcast-to-both stays available as the SourceBoth value for use
// during init (improvSetStatus("listening") etc. — no specific source).
enum class ImprovSource : uint8_t { Both, Uart, Jtag };
static ImprovSource g_replySource = ImprovSource::Both;

// Send a framed Improv message. ImprovFrameType values match the upstream
// improv::ImprovSerialType numerically (we just don't include improv.h in
// the host-side test path, so the host-only header has its own enum).
// Routes to the transport that received the request being replied to
// (g_replySource, set at the top of improvDispatchFrame). Falls back to
// broadcast on both transports when no specific source is set — used
// during init for status-state broadcasts that aren't replies to a
// specific request.
static void improvSend(ImprovFrameType type, const std::vector<uint8_t>& payload) {
    uint8_t frame[6 + 1 + 1 + 1 + kImprovMaxPayload + 1];
    size_t n = buildImprovFrame(type, payload.data(), payload.size(),
                                frame, sizeof(frame));
    if (n == 0) return;  // oversize payload — caller bug, silently drop
    const bool toUart =
#if SOC_USB_SERIAL_JTAG_SUPPORTED
        (g_replySource != ImprovSource::Jtag);
#else
        true;
#endif
    if (toUart) {
        uart_write_bytes(UART_NUM_0, reinterpret_cast<const char*>(frame), n);
    }
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    if (g_jtagReady && g_replySource != ImprovSource::Uart) {
        // Non-blocking: a host that opened the JTAG endpoint but isn't
        // draining must not stall the Improv task — the task also services
        // UART0 reads (10 ms blocking poll) and any TX-side wait here
        // would compound that into laggy RX. usb_serial_jtag_write_bytes
        // returns the byte count actually queued; on a backed-up host we
        // drop the reply silently. Improv on USB-JTAG is opportunistic —
        // the caller (web installer) retries on timeout via its own SDK
        // path. ticks_to_wait=0 means "fill what fits in the TX FIFO
        // headroom, drop the rest". Replies are small (<128 B) and fit
        // in one transaction on any healthy host.
        usb_serial_jtag_write_bytes(frame, n, 0);
    }
#endif
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

#ifndef MM_NO_WIFI
// --- WiFi-provisioning RPCs: only on WiFi builds. On Ethernet-only (MM_NO_WIFI)
// these aren't offered (no STA to provision) and the esp_wifi_* calls aren't linked. ---

static void improvSendWifiNetworks() {
    // Synchronous-ish scan. Replies one network per RPC frame per the Improv
    // spec, then a final empty payload to mark end-of-list. Limit to 10
    // entries to keep the response set bounded.
    //
    // P4 remote-WiFi note: esp_wifi_scan_start needs the WiFi driver started. On
    // native ESP32/S3 the driver is up by the time a user provisions. On the P4 the
    // radio lives on the C6 and only comes up after the esp_hosted prelude in
    // ensureWifiInit() (triggered by wifiApInit / wifiStaInit). If a scan is ever
    // requested on a P4 that has not yet initialised WiFi, this returns an error
    // cleanly (no crash) rather than scanning a cold link — acceptable for now;
    // bench-verify whether a P4 provisioned from cold needs the link brought up
    // here first, and if so route through the public wifiAp/wifiSta path.
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

    // Wait up to 30 s for a usable IP. Polls existing platform state — no extra
    // wiring. Loop until the lease actually lands (a non-zero address), not merely
    // until association: DHCP completes shortly after WIFI_EVENT_STA_CONNECTED, so
    // breaking on wifiStaConnected() alone could read 0.0.0.0.
    uint8_t ip[4] = {};
    for (int i = 0; i < 300; i++) {  // 30 s @ 100 ms
        vTaskDelay(pdMS_TO_TICKS(100));
        if (wifiStaConnected()) {
            wifiStaGetIPv4(ip);
            if (ip[0] || ip[1] || ip[2] || ip[3]) break;   // have a real address
        }
    }
    // No usable lease (never associated, or associated but DHCP never completed) is a
    // failure, same as the timeout — http://0.0.0.0/ would be worse than an honest error.
    if (!ip[0] && !ip[1] && !ip[2] && !ip[3]) {
        improvSetStatus("error: no IP after 30s");
        improvSendError(improv::ERROR_UNABLE_TO_CONNECT);
        return;
    }
    improvSetStatus("connected: %s", cmd.ssid.c_str());
    // Success frame: RPC response carrying the device URL. Format the dotted-quad
    // inline (platform layer doesn't pull core/Control.h's formatDottedQuad).
    char url[64];
    std::snprintf(url, sizeof(url), "http://%u.%u.%u.%u/", ip[0], ip[1], ip[2], ip[3]);
    std::vector<std::string> urls = { url };
    auto rpc = improv::build_rpc_response(improv::WIFI_SETTINGS, urls, false);
    improvSend(ImprovFrameType::RpcResponse, rpc);
    improvSendCurrentState(improv::STATE_PROVISIONED);
}

#endif // MM_NO_WIFI — end WiFi-provisioning RPCs

// SET_DEVICE_MODEL vendor RPC (command 0xFE) — Step 3 of the deviceModel-injection plan.
// The web installer's orchestrator sends this after WiFi provisioning so the
// device persists its physical-board name (e.g. "LOLIN D32") without needing
// MoonDeck or an HTTP fetch (which is blocked by mixed-content on Pages).
//
// Frame payload layout (after the standard Improv frame header):
//   [0xFE]              command
//   [data_len]          number of bytes that follow (= 1 + str_len)
//   [str_len]           1..(deviceModel buffer - 1), length of deviceModel name in bytes
//   [str_bytes...]      ASCII-printable 0x20..0x7E only
//
// We parse the raw payload directly instead of going through
// improv::parse_improv_data — that helper is WIFI_SETTINGS-shaped (n
// length-prefixed strings into cmd.ssid/cmd.password) and may default-empty
// the fields for unknown command IDs. Single-bytestring vendor commands are
// cleaner to handle inline.
//
// On valid: write into g_improv.deviceModelOut, set deviceModelReady. The module's
// loop1s() picks it up and calls SystemModule::setDeviceModel which arms
// FilesystemModule's debounced save. RpcResponse fires immediately —
// validation already passed.
//
// On invalid: ErrorState 0x80 (ERROR_INVALID_DEVICE_MODEL, vendor error code).
static constexpr uint8_t IMPROV_CMD_SET_DEVICE_MODEL = 0xFE;
static constexpr uint8_t IMPROV_ERROR_INVALID_DEVICE_MODEL = 0x80;

static void improvHandleSetDeviceModel(const uint8_t* payload, uint8_t len) {
    if (!g_improv.deviceModelOut || !g_improv.deviceModelReady) {
        // Module didn't opt in (no SystemModule wired). Mostly defensive —
        // production wires it in main.cpp; failing-safe here keeps the dispatch
        // path well-defined.
        improvSendError(improv::ERROR_UNKNOWN_RPC);
        return;
    }
    if (len < 3) {
        improvSendError(static_cast<improv::Error>(IMPROV_ERROR_INVALID_DEVICE_MODEL));
        return;
    }
    // payload[0] is the command byte (0xFE) — we already dispatched on it.
    // payload[1] is data_len (RPC framing); payload[2] is str_len.
    // Cross-check the three lengths so a malformed frame (e.g. data_len
    // disagreeing with str_len, or extra trailing bytes inside the
    // framing-level payload) is rejected rather than silently accepted.
    // The outer framing parser already validated `len` against the
    // wire-level length byte; these checks enforce internal consistency.
    uint8_t dataLen = payload[1];
    uint8_t strLen = payload[2];
    if (strLen == 0 || strLen >= g_improv.deviceModelOutLen
            || dataLen != static_cast<uint8_t>(1u + strLen)
            || len != static_cast<size_t>(3u + strLen)) {
        improvSendError(static_cast<improv::Error>(IMPROV_ERROR_INVALID_DEVICE_MODEL));
        return;
    }
    for (uint8_t i = 0; i < strLen; i++) {
        uint8_t b = payload[3 + i];
        if (b < 0x20 || b > 0x7E) {
            improvSendError(static_cast<improv::Error>(IMPROV_ERROR_INVALID_DEVICE_MODEL));
            return;
        }
    }
    std::memcpy(g_improv.deviceModelOut, payload + 3, strLen);
    g_improv.deviceModelOut[strLen] = 0;
    // release-store: pairs with the module's acquire-load in loop1s() so the
    // buffer write is visible before the consumer sees ready=true.
    g_improv.deviceModelReady->store(true, std::memory_order_release);
    // Empty-payload RpcResponse for command 0xFE — signals success. The
    // browser orchestrator can treat any RpcResponse with cmd=0xFE as ack.
    auto rpc = improv::build_rpc_response(
        static_cast<improv::Command>(IMPROV_CMD_SET_DEVICE_MODEL),
        std::vector<std::string>{}, false);
    improvSend(ImprovFrameType::RpcResponse, rpc);
}

// SET_TX_POWER vendor RPC (command 0xFD) — the pre-association escape hatch
// for boards whose LDO browns out at full TX power (weak-powered boards). Their
// deviceModels.json cap (Network.txPowerSetting) normally arrives over HTTP after
// the device is online — which a browning-out board can never reach: it fails
// WiFi auth at 20 dBm before any HTTP exists (proven on the bench,
// 2026-06-10). This RPC carries the cap over the same serial channel as the
// credentials, so it persists BEFORE the first association attempt.
//
// Frame payload layout (after the standard Improv frame header):
//   [0xFD]              command
//   [data_len]          number of bytes that follow (= 1)
//   [dBm]               0..21 whole dBm; 0 = no cap (lift)
//
// On valid: write into g_improv.txPowerOut, set txPowerReady. The module's
// loop1s() forwards to NetworkModule::setTxPowerSetting (persist + apply).
static constexpr uint8_t IMPROV_CMD_SET_TX_POWER = 0xFD;
static constexpr uint8_t IMPROV_ERROR_INVALID_TX_POWER = 0x81;

static void improvHandleSetTxPower(const uint8_t* payload, uint8_t len) {
    if (!g_improv.txPowerOut || !g_improv.txPowerReady) {
        improvSendError(improv::ERROR_UNKNOWN_RPC);
        return;
    }
    // payload[0] = command (dispatched on), payload[1] = data_len, payload[2] = dBm.
    if (len != 3 || payload[1] != 1 || payload[2] > 21) {
        improvSendError(static_cast<improv::Error>(IMPROV_ERROR_INVALID_TX_POWER));
        return;
    }
    *g_improv.txPowerOut = payload[2];
    // release-store pairs with the module's acquire-load in loop1s().
    g_improv.txPowerReady->store(true, std::memory_order_release);
    auto rpc = improv::build_rpc_response(
        static_cast<improv::Command>(IMPROV_CMD_SET_TX_POWER),
        std::vector<std::string>{}, false);
    improvSend(ImprovFrameType::RpcResponse, rpc);
}

// APPLY_OP vendor RPC (command 0xFC) — "Improv = REST over serial". Carries ONE
// REST operation as JSON (the same shape an HTTP /api/modules or /api/control body
// has): {"op":"add",…} / {"op":"set",…} / {"op":"clearChildren",…}. The installer
// pushes these during provisioning while it owns the serial port, so device-model
// defaults apply over serial — no HTTP, no mixed-content, no browser pull/handoff.
//
// Frame payload layout (after the standard Improv frame header):
//   [0xFC]              command
//   [seq]               chunk index, 0-based (seq 0 resets the reassembly buffer)
//   [last]              1 if this is the final chunk, else 0
//   [chunk bytes…]      a slice of the op JSON (≤ kImprovMaxPayload-3 bytes)
// Most ops fit one frame (seq 0, last 1); a long value (e.g. a big pins list)
// chunks. On last=1 the reassembled JSON is NUL-terminated and opReady is set; the
// module's loop applies it on the MAIN loop (the factory/tree mutation must not run
// on the Improv task). Ack each frame so the installer can pace + retry.
static constexpr uint8_t IMPROV_CMD_APPLY_OP = 0xFC;
static constexpr uint8_t IMPROV_ERROR_INVALID_OP = 0x82;

static void improvHandleApplyOp(const uint8_t* payload, uint8_t len) {
    if (!g_improv.opOut || !g_improv.opReady) {
        improvSendError(improv::ERROR_UNKNOWN_RPC);
        return;
    }
    // Single-buffered: refuse a new op while the module hasn't consumed the previous
    // one (opReady still set), so a fast installer can't overwrite an unapplied op.
    // The installer treats this error as "retry shortly" and re-sends. Acquire-load
    // pairs with the module's release-store when it clears the flag after applying.
    if (g_improv.opReady->load(std::memory_order_acquire)) {
        improvSendError(static_cast<improv::Error>(IMPROV_ERROR_INVALID_OP));
        return;
    }
    // [0xFC][seq][last] header = 3 bytes; chunk is the rest.
    if (len < 3) {
        improvSendError(static_cast<improv::Error>(IMPROV_ERROR_INVALID_OP));
        return;
    }
    uint8_t seq = payload[1];
    uint8_t last = payload[2];
    const uint8_t* chunk = payload + 3;
    size_t chunkLen = static_cast<size_t>(len) - 3;

    // Chunk reassembly + the out-of-order/duplicate sequence guard live in
    // mm::ImprovOpReassembler (core, desktop-unit-tested) so the algorithm is proven
    // without hardware; this handler keeps only the serial I/O around it. The
    // reassembler is bound once to g_improv.opOut (the module-owned buffer).
    static mm::ImprovOpReassembler reasm(g_improv.opOut, g_improv.opOutLen);
    switch (reasm.feed(seq, last, chunk, chunkLen)) {
        case mm::ImprovOpReassembler::Result::Error:
            improvSendError(static_cast<improv::Error>(IMPROV_ERROR_INVALID_OP));
            return;
        case mm::ImprovOpReassembler::Result::Continue:
            break;
        case mm::ImprovOpReassembler::Result::Ready:
            // release-store pairs with the module's acquire-load before it applies.
            g_improv.opReady->store(true, std::memory_order_release);
            break;
    }
    // Ack every chunk (empty RpcResponse for 0xFC) so the installer awaits each.
    auto rpc = improv::build_rpc_response(
        static_cast<improv::Command>(IMPROV_CMD_APPLY_OP),
        std::vector<std::string>{}, false);
    improvSend(ImprovFrameType::RpcResponse, rpc);
}

// Dispatch a completed frame from the parser. Only RPC frames carry commands
// we care about; the spec lets the other types through silently.
static void improvDispatchFrame(const ImprovFrameParser& parser) {
    if (parser.lastType() != improv::TYPE_RPC) return;
    // SET_DEVICE_MODEL short-circuits the standard improv::parse_improv_data path
    // because that helper is WIFI_SETTINGS-shaped (n length-prefixed strings).
    // Peek at the command byte first; vendor-RPC parsing handles its own payload.
    const uint8_t* raw = parser.lastPayload();
    uint8_t rawLen = parser.lastPayloadLen();
    if (rawLen >= 1 && raw[0] == IMPROV_CMD_SET_DEVICE_MODEL) {
        improvHandleSetDeviceModel(raw, rawLen);
        return;
    }
    if (rawLen >= 1 && raw[0] == IMPROV_CMD_SET_TX_POWER) {
        improvHandleSetTxPower(raw, rawLen);
        return;
    }
    if (rawLen >= 1 && raw[0] == IMPROV_CMD_APPLY_OP) {
        improvHandleApplyOp(raw, rawLen);
        return;
    }
    improv::ImprovCommand cmd = improv::parse_improv_data(
        parser.lastPayload(), parser.lastPayloadLen(), false);
    switch (cmd.command) {
        case improv::GET_CURRENT_STATE: {
            // "Connected" means: on WiFi, the STA has an IP; on Ethernet-only, the eth
            // link is up with a DHCP lease. Either way report PROVISIONED + the device
            // URL (the way ESPHome does — makes the protocol self-describing on every
            // reconnect; observable via improv_probe.py). Not connected → AUTHORIZED.
            uint8_t ip[4] = {};
            bool connected = false;
#ifndef MM_NO_WIFI
            if (wifiStaConnected()) { wifiStaGetIPv4(ip); connected = true; }
            else
#endif
            if (ethConnected()) { ethGetIPv4(ip); connected = true; }
            if (connected) {
                improvSendCurrentState(improv::STATE_PROVISIONED);
                if (ip[0] || ip[1] || ip[2] || ip[3]) {
                    char url[64];
                    std::snprintf(url, sizeof(url), "http://%u.%u.%u.%u/", ip[0], ip[1], ip[2], ip[3]);
                    std::vector<std::string> urls = { url };
                    improvSend(ImprovFrameType::RpcResponse,
                               improv::build_rpc_response(improv::WIFI_SETTINGS, urls, false));
                }
            } else {
                improvSendCurrentState(improv::STATE_AUTHORIZED);
            }
            break;
        }
        case improv::GET_DEVICE_INFO: improvSendDeviceInfo(); break;
#ifndef MM_NO_WIFI
        case improv::GET_WIFI_NETWORKS:
            // Refuse scans while WiFi STA is connected — esp_wifi_scan_start puts the
            // radio into scan mode for 2-5 s, dropping inbound ArtNet (a visible glitch
            // on a 16K-LED rig). GET_CURRENT_STATE already reports online.
            if (wifiStaConnected()) improvSendError(improv::ERROR_UNABLE_TO_CONNECT);
            else                    improvSendWifiNetworks();
            break;
        case improv::WIFI_SETTINGS: improvHandleProvision(cmd); break;
#endif
        default:                    improvSendError(improv::ERROR_UNKNOWN_RPC); break;
    }
}

// Feed one byte into the parser and dispatch / error as needed. The
// `source` argument identifies which transport the byte arrived on;
// improvSend reads it via g_replySource to route the reply back to the
// requesting transport only, avoiding broadcast to the silent side.
// Reset to Both after dispatch so any subsequent unsolicited send
// (e.g. from a future async path) broadcasts as before.
static void improvFeedByte(ImprovFrameParser& parser, uint8_t b, ImprovSource source) {
    switch (parser.feed(b)) {
        case ImprovFeedResult::NeedMore:
            break;
        case ImprovFeedResult::FrameReady:
            g_replySource = source;
            improvDispatchFrame(parser);
            g_replySource = ImprovSource::Both;
            break;
        case ImprovFeedResult::BadChecksum:
            g_replySource = source;
            improvSendError(improv::ERROR_INVALID_RPC);
            g_replySource = ImprovSource::Both;
            break;
        case ImprovFeedResult::OversizePayload:
            // Length byte > 128 — almost certainly noise / bit-flip; resync silently.
            break;
    }
}

static void improvTask(void* /*arg*/) {
    // UART0 driver install. UART0 is already configured at 115200-8N1 by
    // the bootloader; we just claim the interrupt + RX FIFO. RX buf 256 is
    // plenty (Improv RPC payloads max out around 96 bytes).
    bool uartReady = false;
    esp_err_t uart_err = uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0);
    if (uart_err == ESP_OK) {
        uartReady = true;
    } else {
        // Don't park the task: if USB-Serial-JTAG works on this board,
        // the task is still useful — Improv just won't reach via UART.
        // ESP_LOGW lands in the serial log so a developer reading the
        // monitor sees the cause; the compound status set below also
        // surfaces it in the UI's `provision_status` control.
        ESP_LOGW(IMPROV_TAG, "uart_driver_install failed: %s",
                 esp_err_to_name(uart_err));
    }

#if SOC_USB_SERIAL_JTAG_SUPPORTED
    // USB-Serial-JTAG driver install. On native-USB boards (S3 / S2 / C3
    // etc.) this is the interface the host actually talks to; UART0
    // is unwired. Best-effort install — secondary console may have grabbed
    // the peripheral first on some sdkconfig combinations; if so, skip and
    // rely on UART0.
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t jtag_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t jtag_err = usb_serial_jtag_driver_install(&jtag_cfg);
        if (jtag_err == ESP_OK) {
            g_jtagReady = true;
        } else {
            ESP_LOGW(IMPROV_TAG, "usb_serial_jtag_install failed: %s",
                     esp_err_to_name(jtag_err));
        }
    } else {
        // Someone else already installed it (rare). We can still read +
        // write through it.
        g_jtagReady = true;
    }
#endif

    // If both installs failed, the task has nothing to do. Park it.
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    const bool anyReady = uartReady || g_jtagReady;
#else
    const bool anyReady = uartReady;
#endif
    if (!anyReady) {
        improvSetStatus("error: no transport (uart + jtag install both failed)");
        vTaskDelete(nullptr);
        return;
    }

    // Compound status so a user inspecting `provision_status` can see
    // partial failures (one transport up, the other failed). Without this
    // the listening-state status overwrites any prior warn line and the
    // failure is invisible in the UI.
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    if (uartReady && g_jtagReady)         improvSetStatus("listening");
    else if (uartReady)                   improvSetStatus("listening (jtag unavailable)");
    else                                   improvSetStatus("listening (uart unavailable)");
#else
    improvSetStatus("listening");
#endif

    // One parser per transport. Each parser keeps its own framing state
    // and 128-byte payload buffer (~150 B per instance on stack). With a
    // shared parser, a partial frame on UART would be corrupted by bytes
    // arriving on JTAG (and vice versa) — the parser's state machine
    // doesn't know they came from different sources. Two parsers keep
    // the framing per-transport so a half-received frame on one side
    // can't be confused by traffic on the other.
    ImprovFrameParser parser_uart;
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    ImprovFrameParser parser_jtag;
#endif
    uint8_t b;
    for (;;) {
        // Symmetric non-blocking poll of both transports. Each round
        // drains up to 64 bytes from whichever side has data, then yields
        // 10 ms once if both came up empty. Previous shape (10 ms blocking
        // on UART, 0 ms drain on JTAG) introduced lumpy throughput on
        // dual-transport boards because UART's blocking wait paused JTAG
        // drainage; symmetric polling reads either side promptly without
        // either starving the other.
        bool anyRead = false;
        if (uartReady) {
            for (int drained = 0; drained < 64; ++drained) {
                int n = uart_read_bytes(UART_NUM_0, &b, 1, 0);
                if (n <= 0) break;
                improvFeedByte(parser_uart, b, ImprovSource::Uart);
                anyRead = true;
            }
        }
#if SOC_USB_SERIAL_JTAG_SUPPORTED
        if (g_jtagReady) {
            for (int drained = 0; drained < 64; ++drained) {
                int n = usb_serial_jtag_read_bytes(&b, 1, 0);
                if (n <= 0) break;
                improvFeedByte(parser_jtag, b, ImprovSource::Jtag);
                anyRead = true;
            }
        }
#endif
        if (!anyRead) {
            // Nothing on either side — yield so FreeRTOS can schedule the
            // idle task and lower-priority work. 10 ms is the same wait
            // the previous UART-blocking-poll achieved; we're trading
            // an interrupt-driven wait for a scheduled delay, which is
            // identical from the task's perspective.
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

} // anonymous namespace

bool improvProvisioningInit(const ImprovDeviceInfo& info,
                            char* ssidOut, size_t ssidOutLen,
                            char* passwordOut, size_t passwordOutLen,
                            std::atomic<bool>* ready,
                            char* statusBuf, size_t statusBufLen,
                            char* deviceModelOut, size_t deviceModelOutLen,
                            std::atomic<bool>* deviceModelReady,
                            uint8_t* txPowerOut,
                            std::atomic<bool>* txPowerReady,
                            char* opOut, size_t opOutLen,
                            std::atomic<bool>* opReady) {
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
    // SET_DEVICE_MODEL opt-in: caller may pass null/0/null to skip vendor-RPC support.
    g_improv.deviceModelOut = deviceModelOut;
    g_improv.deviceModelOutLen = deviceModelOutLen;
    g_improv.deviceModelReady = deviceModelReady;
    // SET_TX_POWER opt-in, same shape.
    g_improv.txPowerOut = txPowerOut;
    g_improv.txPowerReady = txPowerReady;
    // APPLY_OP opt-in, same shape (the op reassembly buffer + ready flag).
    g_improv.opOut = opOut;
    g_improv.opOutLen = opOutLen;
    g_improv.opReady = opReady;

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
