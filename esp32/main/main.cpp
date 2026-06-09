#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include <cstdio>

extern void mm_main(volatile bool& keepRunning, uint16_t httpPort);

static volatile bool running = true;

#ifdef MM_LED_LOOPBACK_TEST
// Hardware-gated RMT WS2812 loopback test (test/device/device_RmtLoopback.cpp).
// When built with -DMM_LED_LOOPBACK_TEST the firmware runs ONLY this test and
// prints PASS/FAIL — it does not start the normal app. Jumper GPIO 4 -> GPIO 5.
extern "C" void mm_led_loopback_test();
#endif

extern "C" void app_main() {
#ifdef MM_LED_LOOPBACK_TEST
    mm_led_loopback_test();
    return;   // test-only firmware — don't start the full app
#endif

    // Initialize NVS (required by WiFi and other drivers)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Print heap before network init
    std::printf("Heap free: %lu bytes\n",
                static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));

    // Network init moved to platform layer (NetworkModule calls ethInit/wifiStaInit/wifiApInit)

    mm_main(running, 80);
}
