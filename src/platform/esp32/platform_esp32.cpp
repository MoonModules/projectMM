// platform_esp32.cpp — ESP32 platform layer core (plan-23 shape).
//
// Contains: system primitives (time, alloc, restart, chip info),
//           network (Ethernet + WiFi STA/AP + mDNS),
//           sockets (TcpServer, TcpConnection, UdpSocket).
//
// Three subsystems live in sibling files since they're self-contained
// — each owns its private state and talks to this core file only
// through public symbols declared in platform.h:
//   - LittleFS    → platform_esp32_fs.cpp
//   - OTA         → platform_esp32_ota.cpp
//   - Improv WiFi → platform_esp32_improv.cpp
//
// Network stayed here because Eth + WiFi + sockets + mDNS share
// file-scope state (the event handler, the netif pointers, the
// init-done flags) — splitting would require either an internal
// header with `extern` declarations or a singleton refactor. That's
// a separate plan when it earns its keep.

#include "platform/platform.h"

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_idf_version.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"     // for esp_ota_get_running_partition (sysInfo)
#include "esp_image_format.h"
#include "esp_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#ifdef CONFIG_IDF_TARGET_ESP32P4
#include "esp_eth_phy_ip101.h"   // P4-NANO PHY — managed component espressif/ip101
#endif
#ifndef MM_NO_WIFI
#include "esp_wifi.h"
#endif
#include "esp_log.h"
#include "esp_rom_sys.h"     // esp_rom_delay_us (delayUs)
#include "mdns.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/uio.h>
#include <unistd.h>

namespace mm::platform {

// Test-only override for millis(); 0 means "use the real clock". Honoured on
// ESP32 too so a hardware scenario run can freeze time the same way unit tests
// do (no separate desktop-vs-ESP32 mocking surface).
static std::atomic<uint32_t> testNowMs{0};

void setTestNowMs(uint32_t ms) { testNowMs.store(ms, std::memory_order_relaxed); }

uint32_t millis() {
    uint32_t override_ = testNowMs.load(std::memory_order_relaxed);
    if (override_) return override_;
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

uint32_t micros() {
    return static_cast<uint32_t>(esp_timer_get_time());
}

void* alloc(size_t bytes) {
#ifdef CONFIG_SPIRAM
    // Try PSRAM first, fall back to internal RAM
    void* ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) return ptr;
#endif
    return heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

void free(void* ptr) {
    heap_caps_free(ptr);
}

void yield() {
    vTaskDelay(pdMS_TO_TICKS(1));
}

void delayMs(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void delayUs(uint32_t us) {
    // Busy-wait — fine for the few-hundred-µs protocol gaps this exists for
    // (e.g. the WS2812 inter-frame latch), off any latency-critical context.
    esp_rom_delay_us(us);
}

void reboot() {
    esp_restart();
}

size_t freeHeap() {
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

size_t freeInternalHeap() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Test-only cap on the reported largest-free block; 0 = no cap. atomic to match
// the desktop seam's cross-thread contract. It only ever LOWERS the reported
// value (min with the real block) — a cap can't claim more contiguous heap than
// the device actually has, so a forced-paging test stays honest.
static std::atomic<size_t> testMaxBlock{0};
void setTestMaxAllocBlock(size_t bytes) { testMaxBlock.store(bytes, std::memory_order_relaxed); }

size_t maxAllocBlock() {
    size_t real = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t cap = testMaxBlock.load(std::memory_order_relaxed);
    return (cap != 0 && cap < real) ? cap : real;
}

size_t maxInternalAllocBlock() {
    // MALLOC_CAP_INTERNAL excludes PSRAM. The internal heap is the scarce
    // resource (WiFi, TCP/IP, FreeRTOS stacks all draw from it); PSRAM is
    // huge by construction so its largest-free-block tells you nothing
    // about memory pressure.
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t totalHeap() {
    return heap_caps_get_total_size(MALLOC_CAP_8BIT);
}

size_t totalInternalHeap() {
    return heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void getMacAddress(uint8_t mac[6]) {
    esp_efuse_mac_get_default(mac);
}

const char* chipModel() {
    esp_chip_info_t info;
    esp_chip_info(&info);
    switch (info.model) {
        case CHIP_ESP32:   return "ESP32";
        case CHIP_ESP32S2: return "ESP32-S2";
        case CHIP_ESP32S3: return "ESP32-S3";
        case CHIP_ESP32C3: return "ESP32-C3";
        case CHIP_ESP32P4: return "ESP32-P4";
        default:           return "ESP32-?";
    }
}

const char* hostIp() {
    // The device IP belongs to NetworkModule (WiFi/Ethernet), not the platform
    // layer — it isn't known until an interface comes up. Empty here.
    return "";
}

const char* sdkVersion() {
    return esp_get_idf_version();
}

const char* resetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_EXT:        return "EXT";
        case ESP_RST_SW:         return "SW";
        case ESP_RST_PANIC:      return "PANIC";
        case ESP_RST_INT_WDT:    return "INT_WDT";
        case ESP_RST_TASK_WDT:   return "TASK_WDT";
        case ESP_RST_WDT:        return "WDT";
        case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_SDIO:       return "SDIO";
        default:                 return "UNKNOWN";
    }
}

size_t firmwareSize() {
    // Get actual running image size from the image header
    const esp_partition_t* part = esp_ota_get_running_partition();
    if (!part) return 0;
    esp_partition_pos_t partPos = { .offset = part->address, .size = part->size };
    esp_image_metadata_t metadata;
    if (esp_image_get_metadata(&partPos, &metadata) == ESP_OK) {
        return metadata.image_len;
    }
    return 0;
}

size_t firmwarePartition() {
    const esp_partition_t* part = esp_ota_get_running_partition();
    if (part) return part->size;
    return 0;
}

size_t flashChipSize() {
    uint32_t chipSize = 0;
    esp_flash_get_size(nullptr, &chipSize);
    return chipSize;
}


// -----------------------------------------------------------------------
// Network
// -----------------------------------------------------------------------

static const char* NET_TAG = "mm_net";

// Connection state tracked by event handlers
#ifndef MM_NO_ETH
static bool ethLinkUp_ = false;
static bool ethConnected_ = false;
static esp_netif_t* ethNetif_ = nullptr;
#endif
static bool netifInitDone_ = false;

#ifndef MM_NO_WIFI
// WiFi-only state — absent in the Ethernet-only build.
static bool wifiStaConnected_ = false;
static bool wifiApActive_ = false;
static esp_netif_t* staNetif_ = nullptr;
static esp_netif_t* apNetif_ = nullptr;
static bool wifiInitDone_ = false;
#endif

static void ensureNetifInit() {
    if (!netifInitDone_) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        netifInitDone_ = true;
    }
}

#ifndef MM_NO_ETH

static void ethEventHandler(void* /*arg*/, esp_event_base_t base,
                            int32_t id, void* data) {
    if (base == ETH_EVENT) {
        if (id == ETHERNET_EVENT_CONNECTED) {
            ESP_LOGI(NET_TAG, "Ethernet link up");
            ethLinkUp_ = true;
        } else if (id == ETHERNET_EVENT_DISCONNECTED) {
            ethLinkUp_ = false;
            ESP_LOGI(NET_TAG, "Ethernet link down");
            ethConnected_ = false;
        } else if (id == ETHERNET_EVENT_START) {
            ESP_LOGI(NET_TAG, "Ethernet started");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(NET_TAG, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ethConnected_ = true;
    }
}

bool ethInit() {
    ensureNetifInit();

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    ethNetif_ = esp_netif_new(&netif_cfg);

    // RMII / PHY pins come from the per-target ethPins config (platform_config.h)
    // — the Olimex map by default, the P4-NANO's IP101 map on the P4. ethPins is
    // a compile-time constant, so the unused branch (and its PHY ctor) is dropped.
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.clock_config.rmii.clock_mode =
        ethPins.rmiiClockExtIn ? EMAC_CLK_EXT_IN : EMAC_CLK_OUT;
    emac_config.clock_config.rmii.clock_gpio =
        static_cast<gpio_num_t>(ethPins.rmiiClockGpio);
    if (ethPins.mdcGpio >= 0)  emac_config.smi_gpio.mdc_num  = ethPins.mdcGpio;
    if (ethPins.mdioGpio >= 0) emac_config.smi_gpio.mdio_num = ethPins.mdioGpio;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ethPins.phyAddr;
    phy_config.reset_gpio_num = ethPins.rstGpio;

    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    // IP101 (P4-NANO) is a managed-component PHY ctor (espressif/ip101 in
    // idf_component.yml; removed from esp_eth core in IDF v6); the generic ctor
    // (Olimex LAN8720) stays in core. The IP101 symbol is only declared on the
    // P4 build (its header include is #ifdef'd), so the *call* must be guarded
    // at the preprocessor level too — `if constexpr` discards a dead branch but
    // still requires it to compile, and an undeclared symbol won't. ethPins is
    // constexpr, so on P4 the runtime branch still folds away.
    esp_eth_phy_t* phy;
#ifdef CONFIG_IDF_TARGET_ESP32P4
    if (ethPins.isIp101) phy = esp_eth_phy_new_ip101(&phy_config);
    else                 phy = esp_eth_phy_new_generic(&phy_config);
#else
    phy = esp_eth_phy_new_generic(&phy_config);
#endif

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = nullptr;
    esp_err_t err = esp_eth_driver_install(&eth_config, &eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "Ethernet driver install failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_ERROR_CHECK(esp_netif_attach(ethNetif_, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &ethEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &ethEventHandler, nullptr));

    err = esp_eth_start(eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(NET_TAG, "Ethernet init done (non-blocking)");
    return true;
}

bool ethLinkUp() {
    return ethLinkUp_;
}

bool ethConnected() {
    return ethConnected_;
}

void ethGetIP(char* buf, size_t len) {
    if (!ethNetif_ || len == 0) { if (len > 0) buf[0] = 0; return; }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(ethNetif_, &info) == ESP_OK) {
        std::snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    } else {
        buf[0] = 0;
    }
}

#else // MM_NO_ETH — firmware excludes EMAC support (chip-side or sdkconfig fragment
      // wasn't layered. Provide stubs matching the desktop platform's no-eth
      // behaviour so NetworkModule's cascade falls straight to WiFi (or AP).

bool ethInit()                          { return false; }
bool ethLinkUp()                        { return false; }
bool ethConnected()                     { return false; }
void ethGetIP(char* buf, size_t len)    { if (len > 0) buf[0] = 0; }

#endif // MM_NO_ETH

#ifndef MM_NO_WIFI

// WiFi event handler
static void wifiEventHandler(void* /*arg*/, esp_event_base_t base,
                             int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGI(NET_TAG, "WiFi STA disconnected");
            wifiStaConnected_ = false;
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            ESP_LOGI(NET_TAG, "WiFi AP client connected");
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            ESP_LOGI(NET_TAG, "WiFi AP client disconnected");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(NET_TAG, "WiFi STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifiStaConnected_ = true;
    }
}

// Returns true on success. Failures must propagate up to wifiStaInit /
// wifiApInit so NetworkModule's state machine can react (typically: fall
// back to whatever path doesn't need WiFi). The pre-fix used
// ESP_ERROR_CHECK, which aborts the device on any failure — fatal when the
// heap is too fragmented for esp_wifi_init to claim its RX buffers, which
// is precisely the case where AP-fallback was meant to kick in. Now WiFi
// init failure is a recoverable runtime error, not a panic.
static bool ensureWifiInit() {
    if (wifiInitDone_) return true;
    ensureNetifInit();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "esp_wifi_init failed: %s (heap %u, largest %u)",
                 esp_err_to_name(err),
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        return false;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &wifiEventHandler, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "WIFI_EVENT register failed: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return false;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &wifiEventHandler, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "IP_EVENT register failed: %s", esp_err_to_name(err));
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler);
        esp_wifi_deinit();
        return false;
    }

    wifiInitDone_ = true;
    return true;
}

bool wifiStaInit(const char* ssid, const char* password) {
    if (!ssid || ssid[0] == 0) return false;

    // Guard against repeated init leaking the previous netif (the cascade can
    // call wifiStaInit again after an Ethernet drop without a prior stop).
    // Stop before ensureWifiInit() — wifiStaStop() deinits the WiFi driver.
    if (staNetif_) wifiStaStop();
    if (!ensureWifiInit()) return false;   // out-of-memory / event register failure

    staNetif_ = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password && password[0] != 0) {
        std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password, sizeof(wifi_config.sta.password) - 1);
    }

    // From here every call can fail for transient runtime reasons (mode
    // conflict, driver-state mismatch, etc.). Log + clean up + return false
    // so NetworkModule's state machine can fall back rather than panic.
    esp_err_t err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi STA set_mode failed: %s", esp_err_to_name(err));
        wifiStaStop();
        return false;
    }
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config)) != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi STA set_config failed: %s", esp_err_to_name(err));
        wifiStaStop();
        return false;
    }
    if ((err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi STA start failed: %s", esp_err_to_name(err));
        wifiStaStop();
        return false;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi STA connect failed: %s", esp_err_to_name(err));
        wifiStaStop();   // tear down the driver/netif we just stood up
        return false;
    }

    ESP_LOGI(NET_TAG, "WiFi STA init done (non-blocking), SSID: %s", ssid);
    return true;
}

bool wifiStaConnected() {
    return wifiStaConnected_;
}

void wifiStaGetIP(char* buf, size_t len) {
    if (!staNetif_ || len == 0) { if (len > 0) buf[0] = 0; return; }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(staNetif_, &info) == ESP_OK) {
        std::snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    } else {
        buf[0] = 0;
    }
}

void wifiStaStop() {
    esp_wifi_disconnect();
    esp_wifi_stop();
    // Unregister event handlers before deinit so subsequent init/stop cycles
    // don't accumulate duplicate registrations. Guard on wifiInitDone_ since
    // ensureWifiInit() bails before the registration step if init failed.
    if (wifiInitDone_) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler);
    }
    esp_wifi_deinit();
    if (staNetif_) {
        esp_netif_destroy_default_wifi(staNetif_);
        staNetif_ = nullptr;
    }
    wifiStaConnected_ = false;
    wifiInitDone_ = false;
    ESP_LOGI(NET_TAG, "WiFi STA stopped + deinit");
}

int wifiStaRssi() {
    if (!wifiStaConnected_) return 0;
    wifi_ap_record_t info{};
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) return 0;
    return info.rssi;
}

bool wifiApInit(const char* apName, const char* ip) {
    // Guard against repeated init leaking the previous AP netif.
    // Stop before ensureWifiInit() — wifiApStop() deinits the WiFi driver.
    if (apNetif_) wifiApStop();
    if (!ensureWifiInit()) return false;   // out-of-memory / event register failure

    apNetif_ = esp_netif_create_default_wifi_ap();

    // Set static IP for AP
    if (ip && ip[0] != 0) {
        esp_netif_dhcps_stop(apNetif_);
        esp_netif_ip_info_t ipInfo = {};
        esp_netif_str_to_ip4(ip, &ipInfo.ip);
        ipInfo.gw = ipInfo.ip;
        IP4_ADDR(&ipInfo.netmask, 255, 255, 255, 0);
        esp_netif_set_ip_info(apNetif_, &ipInfo);
        esp_netif_dhcps_start(apNetif_);
    }

    wifi_config_t wifi_config = {};
    if (apName) {
        std::strncpy(reinterpret_cast<char*>(wifi_config.ap.ssid), apName, sizeof(wifi_config.ap.ssid) - 1);
        wifi_config.ap.ssid_len = static_cast<uint8_t>(std::strlen(apName));
    }
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    esp_err_t err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi AP set_mode failed: %s", esp_err_to_name(err));
        wifiApStop();
        return false;
    }
    if ((err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config)) != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi AP set_config failed: %s", esp_err_to_name(err));
        wifiApStop();
        return false;
    }
    if ((err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(NET_TAG, "WiFi AP start failed: %s", esp_err_to_name(err));
        wifiApStop();
        return false;
    }

    wifiApActive_ = true;
    ESP_LOGI(NET_TAG, "WiFi AP started: %s @ %s", apName ? apName : "?", ip ? ip : "?");
    return true;
}

bool wifiApConnected() {
    return wifiApActive_;
}

void wifiApStop() {
    esp_wifi_stop();
    // Mirror wifiStaStop(): unregister the event handlers before deinit so
    // re-init doesn't accumulate duplicate registrations.
    if (wifiInitDone_) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler);
    }
    esp_wifi_deinit();
    if (apNetif_) {
        esp_netif_destroy_default_wifi(apNetif_);
        apNetif_ = nullptr;
    }
    wifiApActive_ = false;
    wifiInitDone_ = false;
    ESP_LOGI(NET_TAG, "WiFi AP stopped + deinit");
}

int wifiTxPower() {
    if (!wifiInitDone_) return 0;
    int8_t power = 0;
    if (esp_wifi_get_max_tx_power(&power) != ESP_OK) return 0;
    // ESP-IDF returns TX power in units of 0.25 dBm; round to nearest whole dBm.
    return (power + 2) / 4;
}

bool wifiSetTxPower(int8_t quarterDbm) {
    if (quarterDbm == 0) return true;       // 0 = "no override", caller-friendly skip
    if (!wifiInitDone_) return false;       // esp_wifi_set_max_tx_power requires the stack started
    // ESP-IDF accepts 8..84 (2..21 dBm); clamp into range so a bad injected
    // value doesn't make esp_wifi_set_max_tx_power return ESP_ERR_INVALID_ARG
    // and leave the radio at default power without anyone noticing.
    if (quarterDbm < 8)  quarterDbm = 8;
    if (quarterDbm > 84) quarterDbm = 84;
    esp_err_t err = esp_wifi_set_max_tx_power(quarterDbm);
    if (err != ESP_OK) {
        ESP_LOGW(NET_TAG, "WiFi set TX power %d (q-dBm) failed: %s", quarterDbm, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(NET_TAG, "WiFi TX power capped to %d (q-dBm) ≈ %d dBm", quarterDbm, (quarterDbm + 2) / 4);
    return true;
}

#else // MM_NO_WIFI — Ethernet-only build: WiFi compiled out.

// Stub definitions so the linker is satisfied (platform.h declares these and
// NetworkModule's discarded `if constexpr (hasWiFi)` branch still ODR-uses them).
// With hasWiFi==false the calls are not code-generated, so --gc-sections drops
// these stubs from the final image.
bool wifiStaInit(const char* /*ssid*/, const char* /*password*/) { return false; }
bool wifiStaConnected() { return false; }
void wifiStaGetIP(char* buf, size_t len) { if (len > 0) buf[0] = 0; }
void wifiStaStop() {}
int wifiStaRssi() { return 0; }
bool wifiApInit(const char* /*apName*/, const char* /*ip*/) { return false; }
bool wifiApConnected() { return false; }
void wifiApStop() {}
int wifiTxPower() { return 0; }
// Match the API contract: 0 is a successful no-op even when WiFi isn't
// compiled in. Any non-zero value (actual cap attempt) returns false
// because there's no radio to set.
bool wifiSetTxPower(int8_t quarterDbm) { return quarterDbm == 0; }

#endif // MM_NO_WIFI

bool mdnsInit(const char* deviceName) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = mdns_hostname_set(deviceName);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(NET_TAG, "mDNS started: %s.local", deviceName);
    return true;
}

void mdnsStop() {
    mdns_free();
}

// UdpSocket

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open() {
    if (fd_ >= 0) return true;
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    return fd_ >= 0;
}

bool UdpSocket::connect(const char* ip, uint16_t port) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return false;
    return ::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool UdpSocket::sendTo(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    return ::send(fd_, data, len, 0) >= 0;
}

bool UdpSocket::bind(uint16_t port) {
    if (fd_ < 0) return false;
    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    // Non-blocking so the render loop's drain never stalls waiting for a packet.
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    return true;
}

int UdpSocket::recvFrom(uint8_t* buf, size_t maxLen, uint8_t srcIp[4]) {
    if (fd_ < 0) return -1;
    sockaddr_in src{};
    socklen_t srcLen = sizeof(src);
    auto n = ::recvfrom(fd_, buf, maxLen, 0,
                        reinterpret_cast<sockaddr*>(&src), &srcLen);
    // 0-byte datagrams and EWOULDBLOCK both mean "nothing usable pending".
    if (n <= 0) return -1;
    if (srcIp) std::memcpy(srcIp, &src.sin_addr.s_addr, 4);   // network order = octets
    return static_cast<int>(n);
}

bool UdpSocket::sendToAddr(const uint8_t ip[4], uint16_t port,
                           const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::memcpy(&addr.sin_addr.s_addr, ip, 4);
    return ::sendto(fd_, data, len, 0,
                    reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) >= 0;
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        lwip_close(fd_);
        fd_ = -1;
    }
}

// TcpConnection

TcpConnection::~TcpConnection() {
    close();
}

int TcpConnection::read(uint8_t* buf, size_t maxLen) {
    if (fd_ < 0) return -1;
    auto n = lwip_read(fd_, buf, maxLen);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
    return 0;
}

bool TcpConnection::write(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    size_t sent = 0;
    while (sent < len) {
        auto n = lwip_write(fd_, data + sent, len - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            vTaskDelay(pdMS_TO_TICKS(1)); // wait for send buffer space
        } else {
            return false; // real error
        }
    }
    return true;
}

WriteResult TcpConnection::writeChunks(const WriteChunk* chunks, int count) {
    if (fd_ < 0) return WriteResult::Error;
    if (count < 1 || count > MAX_WRITE_CHUNKS) return WriteResult::Error;
    struct iovec iov[MAX_WRITE_CHUNKS];
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        iov[i].iov_base = const_cast<uint8_t*>(chunks[i].data);
        iov[i].iov_len = chunks[i].len;
        total += chunks[i].len;
    }
    // Single non-blocking writev — the socket is already O_NONBLOCK.
    ssize_t n = lwip_writev(fd_, iov, count);
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK)
                   ? WriteResult::WouldBlock : WriteResult::Error;
    }
    if (n == 0) return WriteResult::WouldBlock;
    if (static_cast<size_t>(n) == total) return WriteResult::Complete;

    // Partial: some bytes of this WS frame went out, so we MUST finish the rest
    // — a half-sent frame corrupts the stream and the caller would otherwise
    // drop the connection. This happens under backpressure when the link is
    // saturated (e.g. ArtNet + a large preview frame on a slow 128×128 tick):
    // the lwIP send buffer can't take the whole frame at once but drains in
    // microseconds. Drain the unsent tail with a bounded retry loop; only if it
    // still can't complete (a genuinely stuck socket) do we report Partial so
    // the caller closes. lwIP exposes no free-TX-space query (SO_SNDBUF is
    // unimplemented), so a pre-check isn't possible — finishing the write is the
    // way to keep the stream intact without dropping.
    size_t sent = static_cast<size_t>(n);
    // Small cap: a partial tail (a few KB) drains in 1-2 ms as TCP ACKs arrive;
    // 8 ms is plenty for a transient. A genuinely saturated link blows past it —
    // then we give up (report Partial → caller closes, browser reconnects),
    // rather than stall the render tick. Bounds the worst-case tick hit to ~8 ms.
    constexpr int kMaxDrainTries = 8;    // each yields up to 1ms
    for (int tries = 0; sent < total && tries < kMaxDrainTries; ) {
        // Locate the chunk + offset where `sent` lands, write its remaining tail.
        size_t acc = 0;
        const uint8_t* p = nullptr; size_t remain = 0;
        for (int i = 0; i < count; i++) {
            if (sent < acc + chunks[i].len) {
                size_t off = sent - acc;
                p = chunks[i].data + off;
                remain = chunks[i].len - off;
                break;
            }
            acc += chunks[i].len;
        }
        if (!p) break;  // shouldn't happen (sent < total)
        ssize_t w = lwip_write(fd_, p, remain);
        if (w > 0) {
            sent += static_cast<size_t>(w);
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            vTaskDelay(pdMS_TO_TICKS(1));   // buffer full — let it drain
            tries++;
        } else {
            return WriteResult::Error;       // real socket error
        }
    }
    return (sent == total) ? WriteResult::Complete : WriteResult::Partial;
}

void TcpConnection::close() {
    if (fd_ >= 0) {
        lwip_close(fd_);
        fd_ = -1;
    }
}

// TcpServer

TcpServer::~TcpServer() {
    close();
}

bool TcpServer::open(uint16_t port) {
    if (fd_ >= 0) return true;
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    int opt = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        lwip_close(fd_);
        fd_ = -1;
        return false;
    }

    if (listen(fd_, 4) < 0) {
        lwip_close(fd_);
        fd_ = -1;
        return false;
    }

    // Set non-blocking
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    return true;
}

TcpConnection TcpServer::accept() {
    if (fd_ < 0) return TcpConnection();
    int clientFd = ::accept(fd_, nullptr, nullptr);
    if (clientFd < 0) return TcpConnection();

    int flags = fcntl(clientFd, F_GETFL, 0);
    fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);

    return TcpConnection(clientFd);
}

void TcpServer::close() {
    if (fd_ >= 0) {
        lwip_close(fd_);
        fd_ = -1;
    }
}

} // namespace mm::platform
