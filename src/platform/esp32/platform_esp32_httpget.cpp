// Short-timeout outbound HTTP GET for device discovery (DevicesModule's scan).
// Distinct from platform_esp32_ota.cpp's http_fetch_to_ota: this is a synchronous
// fetch-into-a-small-buffer for LAN device probing — plain HTTP (no TLS / cert
// bundle, devices are local), a short per-request timeout so a dead IP doesn't
// stall the sweep, and a status-code return so the caller can identify the device.
// Kept in its own file (same split rationale as the OTA cut) so the probe doesn't
// pull the OTA / TLS machinery into a build that only wants discovery.

#include "platform/platform.h"

#include "esp_http_client.h"
#include "esp_log.h"

#include <cstring>

namespace mm::platform {

int httpGet(const char* url, uint32_t timeoutMs, char* body, size_t bodyLen) {
    if (body && bodyLen) body[0] = '\0';
    if (!url || !url[0]) return 0;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = static_cast<int>(timeoutMs);
    cfg.method = HTTP_METHOD_GET;
    // LAN devices, plain HTTP — no redirect-following needed (a probe target
    // either answers its own API or it doesn't); keep it minimal.
    cfg.disable_auto_redirect = true;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return 0;

    int status = 0;
    // open(0) sends the request headers with no body; then read the response.
    if (esp_http_client_open(client, 0) == ESP_OK) {
        // Must fetch headers before status/length are valid.
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (body && bodyLen > 1) {
            int total = 0;
            const int cap = static_cast<int>(bodyLen) - 1;  // leave room for NUL
            while (total < cap) {
                int n = esp_http_client_read(client, body + total, cap - total);
                if (n <= 0) break;   // 0 = done, <0 = error/closed
                total += n;
            }
            body[total] = '\0';
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

}  // namespace mm::platform
