// OTA — fetch firmware from a URL and flash it to the next OTA partition.
//
// Cut out of platform_esp32.cpp (plan-23) for size + readability. The
// file owns the OtaTaskParams + otaTask shape in an anonymous namespace;
// the rest of the platform layer talks to it only through the public
// mm::platform::http_fetch_to_ota symbol declared in platform.h. Move
// was a code-organisation change with no API delta.

#include "platform/platform.h"

#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mm::platform {

namespace {

// Heap-allocated task parameters. Task owns this and frees it on exit.
struct OtaTaskParams {
    char url[512];
    char* statusBuf;
    size_t statusBufLen;
    uint32_t* bytesReadOut;   // current bytes downloaded
    uint32_t* bytesTotalOut;  // image size; 0 until esp_https_ota reports it
};

// Write to the status buffer with bounded length. snprintf truncates safely.
void otaSetStatus(OtaTaskParams* p, const char* fmt, ...) {
    if (!p->statusBuf || p->statusBufLen == 0) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(p->statusBuf, p->statusBufLen, fmt, args);
    va_end(args);
}

void otaTask(void* arg) {
    auto* p = static_cast<OtaTaskParams*>(arg);

    otaSetStatus(p, "downloading");
    *p->bytesReadOut = 0;
    *p->bytesTotalOut = 0;   // unknown until esp_https_ota reports it

    // `esp_crt_bundle_attach` enables the bundled-trust-anchor mode for TLS
    // verification — the same mechanism Chrome/curl use for general HTTPS
    // (api.github.com, objects.githubusercontent.com, …). No baked cert.
    esp_http_client_config_t http_config = {};
    http_config.url = p->url;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.timeout_ms = 10000;
    // GitHub release-asset URLs 302-redirect to objects.githubusercontent.com.
    // Default redirect handling is off in esp_http_client; force-follow.
    http_config.disable_auto_redirect = false;
    http_config.max_redirection_count = 10;
    // ESP-IDF's default HTTP header buffer is 512 bytes per direction. GitHub's
    // 302 redirect response includes a multi-KB `content-security-policy`
    // header that overflows it ("HTTP_CLIENT: Out of buffer") and the OTA
    // fails before the .bin download even starts. Raising both sides to 4 KB
    // covers GitHub's longest headers with room to spare; the cost is ~7 KB
    // of heap during the OTA fetch, freed when the OTA task exits.
    http_config.buffer_size = 4096;
    http_config.buffer_size_tx = 4096;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;
    // Performs partial-image-write + commit + boot-pointer flip internally.

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        // esp_https_ota_begin collapses ~6 distinct failures (DNS, TLS,
        // HTTP, partition init, header-buffer overflow) into one ESP_FAIL,
        // so the only useful detail is in the IDF log on the serial console.
        // We surface the IDF error name plus a pointer to the log.
        otaSetStatus(p, "error: ota begin %s (see serial log)",
                     esp_err_to_name(err));
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    int total = esp_https_ota_get_image_size(handle);
    if (total > 0) {
        // Publish the real total so the UI can render "X KB / Y KB".
        // FirmwareUpdateModule's loop1s() rebuildControls picks this up on
        // the next 1 Hz poll (re-binds the progress descriptor with the new
        // total snapshot).
        *p->bytesTotalOut = static_cast<uint32_t>(total);
    }
    otaSetStatus(p, "flashing");

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int got = esp_https_ota_get_image_len_read(handle);
        if (got >= 0) *p->bytesReadOut = static_cast<uint32_t>(got);
    }
    if (err != ESP_OK) {
        otaSetStatus(p, "error: ota perform %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        otaSetStatus(p, "error: incomplete download");
        esp_https_ota_abort(handle);
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        // After finish, abort isn't valid — handle is consumed. Surface and exit.
        otaSetStatus(p, "error: ota finish %s", esp_err_to_name(err));
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    // Final byte count match — pull from the OTA handle one last time so the
    // UI's last frame before reboot shows a clean "Y KB / Y KB".
    if (*p->bytesTotalOut > 0) *p->bytesReadOut = *p->bytesTotalOut;
    otaSetStatus(p, "rebooting");
    delete p;
    // 600 ms delay gives the HTTP response time to make it back to the browser
    // before the device drops the socket on restart.
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

}  // anonymous namespace

bool http_fetch_to_ota(const char* url,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    if (!url || !statusBuf || statusBufLen == 0 || !bytesReadOut || !bytesTotalOut) {
        return false;
    }

    auto* p = new OtaTaskParams{};
    std::strncpy(p->url, url, sizeof(p->url) - 1);
    p->statusBuf = statusBuf;
    p->statusBufLen = statusBufLen;
    p->bytesReadOut = bytesReadOut;
    p->bytesTotalOut = bytesTotalOut;

    // 12 KB stack matches v1's working number (TLS handshake + HTTPS body
    // buffering inside esp_https_ota). Priority 5 = above idle, below
    // FreeRTOS critical drivers.
    BaseType_t ok = xTaskCreate(&otaTask, "urlOta", 12288, p, 5, nullptr);
    if (ok != pdPASS) {
        otaSetStatus(p, "error: task create failed");
        delete p;
        return false;
    }
    return true;
}

} // namespace mm::platform
