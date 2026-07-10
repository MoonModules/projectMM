#include "platform/platform.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#if defined(CONFIG_MM_BLE_PROVISIONING) && defined(CONFIG_BT_ENABLED) && !defined(MM_NO_WIFI) && !defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_err.h"
#include "esp_log.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#endif

namespace mm::platform {

#if defined(CONFIG_MM_BLE_PROVISIONING) && defined(CONFIG_BT_ENABLED) && !defined(MM_NO_WIFI) && !defined(CONFIG_IDF_TARGET_ESP32P4)

namespace {

constexpr const char* TAG = "mm-ble-prov";

struct BleProvisioningState {
    char serviceName[32] = {};
    char pop[8] = {};
    char pendingSsid[33] = {};
    char pendingPassword[64] = {};
    char* ssidOut = nullptr;
    size_t ssidOutLen = 0;
    char* passwordOut = nullptr;
    size_t passwordOutLen = 0;
    std::atomic<bool>* ready = nullptr;
    char* statusBuf = nullptr;
    size_t statusBufLen = 0;
    bool initialized = false;
    bool started = false;
};

BleProvisioningState g_bleProv;

void setStatus(const char* fmt, ...) {
    if (!g_bleProv.statusBuf || g_bleProv.statusBufLen == 0) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(g_bleProv.statusBuf, g_bleProv.statusBufLen, fmt, ap);
    va_end(ap);
}

void copyBounded(char* dst, size_t dstLen, const uint8_t* src, size_t srcCap) {
    if (!dst || dstLen == 0) return;
    size_t n = 0;
    while (n + 1 < dstLen && n < srcCap && src[n] != 0) {
        dst[n] = static_cast<char>(src[n]);
        n++;
    }
    dst[n] = 0;
}

void copyPendingToOutputs() {
    if (!g_bleProv.ssidOut || !g_bleProv.passwordOut || !g_bleProv.ready) return;
    std::snprintf(g_bleProv.ssidOut, g_bleProv.ssidOutLen, "%s", g_bleProv.pendingSsid);
    std::snprintf(g_bleProv.passwordOut, g_bleProv.passwordOutLen, "%s", g_bleProv.pendingPassword);
    g_bleProv.ready->store(true, std::memory_order_release);
}

void makeIdentity(const ImprovDeviceInfo& info) {
    uint8_t mac[6] = {};
    getMacAddress(mac);

    const char* name = (info.name && info.name[0]) ? info.name : "projectMM";
    std::snprintf(g_bleProv.serviceName, sizeof(g_bleProv.serviceName),
                  "%.20s-%02X%02X%02X", name, mac[3], mac[4], mac[5]);
    std::snprintf(g_bleProv.pop, sizeof(g_bleProv.pop),
                  "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

void bleProvEventCb(void* /*user_data*/, network_prov_cb_event_t event, void* event_data) {
    switch (event) {
        case NETWORK_PROV_START:
            setStatus("BLE: %s PoP %s", g_bleProv.serviceName, g_bleProv.pop);
            ESP_LOGI(TAG, "BLE provisioning started, service=%s pop=%s",
                     g_bleProv.serviceName, g_bleProv.pop);
            break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            auto* cfg = static_cast<wifi_sta_config_t*>(event_data);
            if (!cfg) break;
            copyBounded(g_bleProv.pendingSsid, sizeof(g_bleProv.pendingSsid),
                        cfg->ssid, sizeof(cfg->ssid));
            copyBounded(g_bleProv.pendingPassword, sizeof(g_bleProv.pendingPassword),
                        cfg->password, sizeof(cfg->password));
            setStatus("BLE credentials received: %s", g_bleProv.pendingSsid);
            ESP_LOGI(TAG, "BLE credentials received for SSID %s", g_bleProv.pendingSsid);
            break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            copyPendingToOutputs();
            setStatus("BLE connected: %s", g_bleProv.pendingSsid);
            ESP_LOGI(TAG, "BLE provisioning connected: %s", g_bleProv.pendingSsid);
            break;
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            const auto* reason = static_cast<network_prov_wifi_sta_fail_reason_t*>(event_data);
            const bool auth = reason && *reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR;
            setStatus("BLE error: %s", auth ? "auth failed" : "AP not found");
            std::memset(g_bleProv.pendingPassword, 0, sizeof(g_bleProv.pendingPassword));
            network_prov_mgr_reset_wifi_sm_state_on_failure();
            ESP_LOGW(TAG, "BLE provisioning failed: %s", auth ? "auth failed" : "AP not found");
            break;
        }
        case NETWORK_PROV_END:
            if (!g_bleProv.ready || !g_bleProv.ready->load(std::memory_order_acquire)) {
                setStatus("BLE provisioning stopped");
            }
            network_prov_mgr_deinit();
            g_bleProv.started = false;
            g_bleProv.initialized = false;
            break;
        case NETWORK_PROV_DEINIT:
            g_bleProv.started = false;
            g_bleProv.initialized = false;
            break;
        default:
            break;
    }
}

} // namespace

bool bleProvisioningInit(const ImprovDeviceInfo& info,
                         char* ssidOut, size_t ssidOutLen,
                         char* passwordOut, size_t passwordOutLen,
                         std::atomic<bool>* ready,
                         char* statusBuf, size_t statusBufLen) {
    if (g_bleProv.started) return true;
    if (!ssidOut || ssidOutLen == 0 || !passwordOut || passwordOutLen == 0 || !ready) {
        return false;
    }

    g_bleProv.ssidOut = ssidOut;
    g_bleProv.ssidOutLen = ssidOutLen;
    g_bleProv.passwordOut = passwordOut;
    g_bleProv.passwordOutLen = passwordOutLen;
    g_bleProv.ready = ready;
    g_bleProv.statusBuf = statusBuf;
    g_bleProv.statusBufLen = statusBufLen;
    makeIdentity(info);

    network_prov_mgr_config_t config = {};
    config.scheme = network_prov_scheme_ble;
    config.scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
    config.app_event_handler = network_prov_event_handler_t{
        .event_cb = bleProvEventCb,
        .user_data = nullptr,
    };
    config.network_prov_wifi_conn_cfg = network_prov_wifi_conn_cfg_t{
        .wifi_conn_attempts = 3,
    };

    esp_err_t err = network_prov_mgr_init(config);
    if (err != ESP_OK) {
        setStatus("BLE init failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "network_prov_mgr_init failed: %s", esp_err_to_name(err));
        return false;
    }
    g_bleProv.initialized = true;

    network_prov_security1_params_t* secParams = g_bleProv.pop;
    err = network_prov_mgr_start_provisioning(
        NETWORK_PROV_SECURITY_1,
        static_cast<const void*>(secParams),
        g_bleProv.serviceName,
        nullptr);
    if (err != ESP_OK) {
        setStatus("BLE start failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "network_prov_mgr_start_provisioning failed: %s", esp_err_to_name(err));
        network_prov_mgr_deinit();
        g_bleProv.initialized = false;
        return false;
    }

    g_bleProv.started = true;
    setStatus("BLE: %s PoP %s", g_bleProv.serviceName, g_bleProv.pop);
    return true;
}

void bleProvisioningStop() {
    if (!g_bleProv.initialized) return;
    if (g_bleProv.started) {
        network_prov_mgr_stop_provisioning();
    } else {
        network_prov_mgr_deinit();
        g_bleProv.initialized = false;
    }
    g_bleProv.started = false;
}

#else

bool bleProvisioningInit(const ImprovDeviceInfo& /*info*/,
                         char* /*ssidOut*/, size_t /*ssidOutLen*/,
                         char* /*passwordOut*/, size_t /*passwordOutLen*/,
                         std::atomic<bool>* /*ready*/,
                         char* statusBuf, size_t statusBufLen) {
    if (statusBuf && statusBufLen > 0) {
        std::snprintf(statusBuf, statusBufLen, "BLE provisioning unavailable");
    }
    return false;
}

void bleProvisioningStop() {}

#endif

} // namespace mm::platform
