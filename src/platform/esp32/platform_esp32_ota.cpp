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
#include "psa/crypto.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>           // std::nothrow for the OtaTaskParams alloc below

namespace mm::platform {

namespace {

// Heap-allocated task parameters. Task owns this and frees it on exit.
struct OtaTaskParams {
    char url[512];
    char* statusBuf;
    size_t statusBufLen;
    uint32_t* bytesReadOut;   // current bytes downloaded
    uint32_t* bytesTotalOut;  // image size; 0 until esp_https_ota reports it
    std::atomic<bool>* inFlightFlag = nullptr;
    char expectedSha256[65];
    uint32_t expectedSize = 0;
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    bool hashActive = false;
    uint32_t hashedBytes = 0;
};

// Write to the status buffer with bounded length. snprintf truncates safely.
void otaSetStatus(OtaTaskParams* p, const char* fmt, ...) {
    if (!p->statusBuf || p->statusBufLen == 0) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(p->statusBuf, p->statusBufLen, fmt, args);
    va_end(args);
}

void sha256Hex(const unsigned char in[32], char out[65]) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = kHex[in[i] >> 4];
        out[i * 2 + 1] = kHex[in[i] & 0x0f];
    }
    out[64] = 0;
}

void otaHashFinish(OtaTaskParams* p) {
    if (!p || !p->hashActive) return;
    psa_hash_abort(&p->sha);
    p->hashActive = false;
}

void otaTaskFinish(OtaTaskParams* p, bool clearInFlight = true) {
    if (!p) return;
    otaHashFinish(p);
    if (clearInFlight && p->inFlightFlag) {
        p->inFlightFlag->store(false, std::memory_order_release);
    }
    delete p;
}

esp_err_t otaHttpEvent(esp_http_client_event_t* evt) {
    auto* p = static_cast<OtaTaskParams*>(evt->user_data);
    if (!p || !p->hashActive || evt->event_id != HTTP_EVENT_ON_DATA ||
        !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    const int status = esp_http_client_get_status_code(evt->client);
    if (status >= 200 && status < 300) {
        const psa_status_t hashStatus =
            psa_hash_update(&p->sha,
                            static_cast<const uint8_t*>(evt->data),
                            static_cast<size_t>(evt->data_len));
        if (hashStatus != PSA_SUCCESS) {
            otaSetStatus(p, "error: sha update %d",
                         static_cast<int>(hashStatus));
            return ESP_FAIL;
        }
        p->hashedBytes += static_cast<uint32_t>(evt->data_len);
    }
    return ESP_OK;
}

void otaTask(void* arg) {
    auto* p = static_cast<OtaTaskParams*>(arg);

    otaSetStatus(p, "downloading");
    *p->bytesReadOut = 0;
    *p->bytesTotalOut = 0;   // unknown until esp_https_ota reports it
    p->hashedBytes = 0;
    p->hashActive = p->expectedSha256[0] != 0;
    if (p->hashActive) {
        const psa_status_t initStatus = psa_crypto_init();
        if (initStatus != PSA_SUCCESS) {
            otaSetStatus(p, "error: sha init %d", static_cast<int>(initStatus));
            p->hashActive = false;
            otaTaskFinish(p);
            vTaskDelete(nullptr);
            return;
        }
        p->sha = psa_hash_operation_init();
        const psa_status_t hashStatus = psa_hash_setup(&p->sha, PSA_ALG_SHA_256);
        if (hashStatus != PSA_SUCCESS) {
            otaSetStatus(p, "error: sha setup %d", static_cast<int>(hashStatus));
            psa_hash_abort(&p->sha);
            p->hashActive = false;
            otaTaskFinish(p);
            vTaskDelete(nullptr);
            return;
        }
    }

    // `esp_crt_bundle_attach` enables the bundled-trust-anchor mode for TLS verification — the same
    // mechanism Chrome/curl use for general HTTPS (api.github.com, objects.githubusercontent.com, …).
    // No baked cert. It's attached unconditionally: for an https URL it verifies the server; for a
    // plain-http LAN OTA (MoonDeck serving a local build) it goes unused, but its presence satisfies
    // esp_https_ota_begin's "server verification enabled" check, so the fetch proceeds over plain TCP.
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
    http_config.event_handler = otaHttpEvent;
    http_config.user_data = p;

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
        otaTaskFinish(p);
        vTaskDelete(nullptr);
        return;
    }

    int total = esp_https_ota_get_image_size(handle);
    if (total > 0) {
        // Publish the real total so the UI can render "X KB / Y KB".
        // FirmwareUpdateModule's tick1s() rebuildControls picks this up on
        // the next 1 Hz poll (re-binds the progress descriptor with the new
        // total snapshot).
        *p->bytesTotalOut = static_cast<uint32_t>(total);
        if (p->expectedSize > 0 && static_cast<uint32_t>(total) != p->expectedSize) {
            otaSetStatus(p, "error: size mismatch");
            esp_https_ota_abort(handle);
            otaTaskFinish(p);
            vTaskDelete(nullptr);
            return;
        }
    }
    otaSetStatus(p, "flashing");

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int got = esp_https_ota_get_image_len_read(handle);
        if (got >= 0) *p->bytesReadOut = static_cast<uint32_t>(got);
    }
    if (err != ESP_OK) {
        otaSetStatus(p, "error: ota perform %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        otaTaskFinish(p);
        vTaskDelete(nullptr);
        return;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        otaSetStatus(p, "error: incomplete download");
        esp_https_ota_abort(handle);
        otaTaskFinish(p);
        vTaskDelete(nullptr);
        return;
    }

    if (p->expectedSize > 0 && p->hashedBytes != p->expectedSize) {
        otaSetStatus(p, "error: size mismatch");
        esp_https_ota_abort(handle);
        otaTaskFinish(p);
        vTaskDelete(nullptr);
        return;
    }
    if (p->hashActive) {
        unsigned char digest[32];
        size_t digestLen = 0;
        char actual[65];
        const psa_status_t hashStatus =
            psa_hash_finish(&p->sha, digest, sizeof(digest), &digestLen);
        p->hashActive = false;
        if (hashStatus != PSA_SUCCESS || digestLen != sizeof(digest)) {
            otaSetStatus(p, "error: sha finish %d",
                         static_cast<int>(hashStatus));
            esp_https_ota_abort(handle);
            otaTaskFinish(p);
            vTaskDelete(nullptr);
            return;
        }
        sha256Hex(digest, actual);
        if (std::strcmp(actual, p->expectedSha256) != 0) {
            otaSetStatus(p, "error: sha256 mismatch");
            esp_https_ota_abort(handle);
            otaTaskFinish(p);
            vTaskDelete(nullptr);
            return;
        }
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        // After finish, abort isn't valid — handle is consumed. Surface and exit.
        otaSetStatus(p, "error: ota finish %s", esp_err_to_name(err));
        otaTaskFinish(p);
        vTaskDelete(nullptr);
        return;
    }

    // Final byte count match — pull from the OTA handle one last time so the
    // UI's last frame before reboot shows a clean "Y KB / Y KB".
    if (*p->bytesTotalOut > 0) *p->bytesReadOut = *p->bytesTotalOut;
    otaSetStatus(p, "rebooting");
    otaTaskFinish(p, false);
    // 600 ms delay gives the HTTP response time to make it back to the browser
    // before the device drops the socket on restart.
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

}  // anonymous namespace

bool http_fetch_to_ota_checked(const char* url, const char* expectedSha256,
                               uint32_t expectedSize,
                               char* statusBuf, size_t statusBufLen,
                               uint32_t* bytesReadOut, uint32_t* bytesTotalOut,
                               std::atomic<bool>* inFlightFlag) {
    auto clearInFlight = [&]() {
        if (inFlightFlag) inFlightFlag->store(false, std::memory_order_release);
    };
    if (!url || !statusBuf || statusBufLen == 0 || !bytesReadOut || !bytesTotalOut) {
        clearInFlight();
        return false;
    }

    // Reject oversize URLs explicitly rather than silently truncating with
    // strncpy — a truncated URL almost always 404s or fetches the wrong
    // file, with no clue in the status surface.
    size_t urlLen = std::strlen(url);
    constexpr size_t kUrlMax = sizeof(OtaTaskParams::url) - 1;
    if (urlLen > kUrlMax) {
        std::snprintf(statusBuf, statusBufLen,
                      "error: url too long (%zu > %zu)", urlLen, kUrlMax);
        clearInFlight();
        return false;
    }

    // std::nothrow so OOM doesn't abort the process. Status string carries
    // the failure back to the route, which returns 500 to the browser.
    auto* p = new (std::nothrow) OtaTaskParams{};
    if (!p) {
        std::snprintf(statusBuf, statusBufLen, "error: out of memory");
        clearInFlight();
        return false;
    }
    std::memcpy(p->url, url, urlLen + 1);   // includes NUL; size already verified
    p->statusBuf = statusBuf;
    p->statusBufLen = statusBufLen;
    p->bytesReadOut = bytesReadOut;
    p->bytesTotalOut = bytesTotalOut;
    p->inFlightFlag = inFlightFlag;
    if (expectedSha256 && expectedSha256[0]) {
        std::snprintf(p->expectedSha256, sizeof(p->expectedSha256), "%s", expectedSha256);
    }
    p->expectedSize = expectedSize;

    // 12 KB stack matches v1's working number (TLS handshake + HTTPS body
    // buffering inside esp_https_ota). Priority 5 = above idle, below
    // FreeRTOS critical drivers.
    BaseType_t ok = xTaskCreate(&otaTask, "urlOta", 12288, p, 5, nullptr);
    if (ok != pdPASS) {
        otaSetStatus(p, "error: task create failed");
        otaTaskFinish(p);
        return false;
    }
    return true;
}

bool http_fetch_to_ota(const char* url,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut,
                       std::atomic<bool>* inFlightFlag) {
    return http_fetch_to_ota_checked(url, nullptr, 0, statusBuf, statusBufLen,
                                     bytesReadOut, bytesTotalOut, inFlightFlag);
}

bool otaWriteStream(FsWriteSrc src, void* user, size_t contentLen,
                    char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut) {
    if (!src || !statusBuf || statusBufLen == 0 || !bytesReadOut) return false;
    auto setStatus = [&](const char* fmt, auto... a) {
        std::snprintf(statusBuf, statusBufLen, fmt, a...);
    };

    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) { setStatus("error: no OTA partition"); return false; }

    setStatus("flashing");
    esp_ota_handle_t handle = 0;
    // OTA_SIZE_UNKNOWN: the upload streams, so we don't pre-declare the exact size (Content-Length
    // is advisory for the UI); esp_ota_begin erases lazily as writes arrive.
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) { setStatus("error: ota begin %s", esp_err_to_name(err)); return false; }

    // Pull the upload body chunk-by-chunk and write each into the partition — the same producer
    // callback fsWriteStream drives, here feeding esp_ota_write instead of a file. `abort` from the
    // caller (an incomplete/timed-out upload) fails the OTA, and esp_ota_abort discards the partial.
    static uint8_t buf[4096];   // static: keep it off this call's stack (4 KB is large for a task frame)
    uint32_t written = 0;
    for (;;) {
        bool abort = false;
        const size_t n = src(reinterpret_cast<char*>(buf), sizeof(buf), user, &abort);
        if (abort) {
            setStatus("error: upload aborted");
            esp_ota_abort(handle);
            return false;
        }
        if (n == 0) break;   // clean EOF — whole body delivered
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            setStatus("error: ota write %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            return false;
        }
        written += static_cast<uint32_t>(n);
        *bytesReadOut = written;
    }
    // Guard a truncated upload: if the client sent fewer bytes than Content-Length, the image is
    // incomplete — don't commit a half-image. (contentLen 0 = unknown; skip the check then.)
    if (contentLen && written < contentLen) {
        setStatus("error: incomplete upload (%u/%u)",
                  static_cast<unsigned>(written), static_cast<unsigned>(contentLen));
        esp_ota_abort(handle);
        return false;
    }

    err = esp_ota_end(handle);   // validates the image (magic/checksum); consumes the handle
    if (err != ESP_OK) { setStatus("error: ota end %s", esp_err_to_name(err)); return false; }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) { setStatus("error: set boot %s", esp_err_to_name(err)); return false; }

    setStatus("rebooting");
    // Image committed + boot pointer flipped. Return to the caller so it can send its HTTP 200
    // BEFORE the reboot (the caller closes the socket + reboots, same sequence as /api/reboot) —
    // that's what lets the browser see a clean "flashed" response instead of an aborted socket.
    return true;
}

int httpGet(const char* url, char* body, size_t bodyLen, uint32_t timeoutMs) {
    if (body && bodyLen) body[0] = 0;
    if (!url || !body || bodyLen == 0) return 0;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = static_cast<int>(timeoutMs);
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 10;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return 0;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return 0;
    }
    esp_http_client_fetch_headers(client);
    int result = esp_http_client_get_status_code(client);
    size_t used = 0;
    if (result >= 200 && result < 300) {
        while (used + 1 < bodyLen) {
            int n = esp_http_client_read(client, body + used,
                                         static_cast<int>(bodyLen - 1 - used));
            if (n < 0) { result = 0; break; }
            if (n == 0) break;
            used += static_cast<size_t>(n);
        }
    }
    body[used] = 0;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

} // namespace mm::platform
