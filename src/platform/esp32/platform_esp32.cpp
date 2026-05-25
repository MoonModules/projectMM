#include "platform/platform.h"

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_idf_version.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_image_format.h"
#include "esp_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#ifndef MM_NO_WIFI
#include "esp_wifi.h"
#endif
#include "esp_log.h"
#include "mdns.h"
#include "esp_littlefs.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

namespace mm::platform {

uint32_t millis() {
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

void reboot() {
    esp_restart();
}

size_t freeHeap() {
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

size_t freeInternalHeap() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t maxAllocBlock() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
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

// LittleFS state
static const char* FS_TAG = "mm_fs";
static const char* FS_PARTITION_LABEL = "spiffs";  // partition label kept for tooling compat; contents are LittleFS
static const char* FS_MOUNT_POINT = "/littlefs";    // VFS mount point; not exposed in API paths
static bool fsMounted_ = false;

// Translate API path "/foo/bar" or "foo/bar" → "/littlefs/foo/bar" into out.
// Returns false on null input, zero-sized output, or truncation; out[0] is set to 0
// on any failure so callers don't accidentally consume a partial path.
static bool fsTranslate(const char* apiPath, char* out, size_t outLen) {
    if (outLen == 0) return false;
    if (!apiPath) { out[0] = 0; return false; }
    const char* sep = (apiPath[0] == '/') ? "" : "/";
    int n = std::snprintf(out, outLen, "%s%s%s", FS_MOUNT_POINT, sep, apiPath);
    if (n < 0 || static_cast<size_t>(n) >= outLen) { out[0] = 0; return false; }
    return true;
}

void fsSetRoot(const char* /*path*/) {
    // No-op on ESP32 — LittleFS is mounted at a fixed partition; the FS_MOUNT_POINT
    // prefix is hard-coded. Provided only so test code can call it portably.
}

bool fsMount() {
    if (fsMounted_) return true;

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = FS_MOUNT_POINT;
    conf.partition_label = FS_PARTITION_LABEL;
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(FS_TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return false;
    }
    fsMounted_ = true;
    ESP_LOGI(FS_TAG, "LittleFS mounted at %s (partition: %s)", FS_MOUNT_POINT, FS_PARTITION_LABEL);
    return true;
}

void fsUnmount() {
    if (!fsMounted_) return;
    esp_vfs_littlefs_unregister(FS_PARTITION_LABEL);
    fsMounted_ = false;
}

bool fsMkdir(const char* path) {
    if (!fsMounted_) return false;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    // mkdir -p: walk components, create each if missing
    char* p = full + std::strlen(FS_MOUNT_POINT) + 1; // skip "/littlefs/"
    while (*p) {
        if (*p == '/') {
            *p = 0;
            mkdir(full, 0775);  // ignore errors; could already exist
            *p = '/';
        }
        p++;
    }
    int rc = mkdir(full, 0775);
    return rc == 0 || errno == EEXIST;
}

bool fsExists(const char* path) {
    if (!fsMounted_) return false;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    struct stat st;
    return stat(full, &st) == 0;
}

bool fsRemove(const char* path) {
    if (!fsMounted_) return false;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    return ::remove(full) == 0;
}

int fsRead(const char* path, char* buf, size_t maxLen) {
    if (!fsMounted_ || !buf || maxLen == 0) return -1;
    char full[128];
    if (!fsTranslate(path, full, sizeof(full))) return -1;
    FILE* f = std::fopen(full, "rb");
    if (!f) return -1;
    size_t n = std::fread(buf, 1, maxLen - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return static_cast<int>(n);
}

bool fsWriteAtomic(const char* path, const char* data, size_t len) {
    if (!fsMounted_) return false;
    char full[128];
    char tmp[136];
    if (!fsTranslate(path, full, sizeof(full))) return false;
    int n = std::snprintf(tmp, sizeof(tmp), "%s.tmp", full);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(tmp)) return false;

    FILE* f = std::fopen(tmp, "wb");
    if (!f) return false;
    size_t written = std::fwrite(data, 1, len, f);
    if (written != len) {
        std::fclose(f);
        ::remove(tmp);
        return false;
    }
    std::fflush(f);
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
    std::fclose(f);

    if (::rename(tmp, full) != 0) {
        ::remove(tmp);
        return false;
    }
    return true;
}

void fsList(const char* dir, FsListCb cb, void* user) {
    if (!fsMounted_ || !cb) return;
    char full[128];
    if (!fsTranslate(dir, full, sizeof(full))) return;
    DIR* d = ::opendir(full);
    if (!d) return;
    struct dirent* ent;
    // Sized to hold full ("/littlefs/..." up to 128) + '/' + max 255-byte d_name + null.
    char childPath[400];
    struct stat st;
    while ((ent = ::readdir(d)) != nullptr) {
        std::snprintf(childPath, sizeof(childPath), "%s/%s", full, ent->d_name);
        bool isDir = stat(childPath, &st) == 0 && S_ISDIR(st.st_mode);
        cb(ent->d_name, isDir, user);
    }
    ::closedir(d);
}

size_t filesystemUsed() {
    if (!fsMounted_) return 0;
    size_t total = 0, used = 0;
    if (esp_littlefs_info(FS_PARTITION_LABEL, &total, &used) != ESP_OK) return 0;
    return used;
}

size_t filesystemTotal() {
    if (!fsMounted_) return 0;
    size_t total = 0, used = 0;
    if (esp_littlefs_info(FS_PARTITION_LABEL, &total, &used) != ESP_OK) return 0;
    return total;
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

    // MAC config — Olimex ESP32-Gateway Rev G: RMII clock output on GPIO17
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    emac_config.clock_config.rmii.clock_gpio = static_cast<int>(GPIO_NUM_17);

    // PHY config — Olimex ESP32-Gateway: LAN8720, addr 0, reset GPIO 5
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 0;
    phy_config.reset_gpio_num = 5;

    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phy_config);

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

#else // MM_NO_ETH — board has no on-chip EMAC, or the EMAC sdkconfig fragment
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

#else // MM_NO_WIFI — Ethernet-only build: WiFi compiled out.

// Stub definitions so the linker is satisfied (platform.h declares these and
// NetworkModule's discarded `if constexpr (hasWiFi)` branch still ODR-uses them).
// With hasWiFi==false the calls are not code-generated, so --gc-sections drops
// these stubs from the final image.
bool wifiStaInit(const char* /*ssid*/, const char* /*password*/) { return false; }
bool wifiStaConnected() { return false; }
void wifiStaGetIP(char* buf, size_t len) { if (len > 0) buf[0] = 0; }
void wifiStaStop() {}
bool wifiApInit(const char* /*apName*/, const char* /*ip*/) { return false; }
bool wifiApConnected() { return false; }
void wifiApStop() {}

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
    return WriteResult::Partial;
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

// -----------------------------------------------------------------------
// OTA — fetch firmware from a URL and flash it to the next OTA partition.
// -----------------------------------------------------------------------

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

// -----------------------------------------------------------------------
// Improv WiFi — provisioning credentials over UART0.
//
// Whole section is compiled out on Ethernet-only builds (MM_NO_WIFI):
//   - The library calls esp_wifi_scan_* / WIFI_AUTH_OPEN etc., which aren't
//     linked when WiFi is excluded.
//   - There's no WiFi STA to provision on an Eth-only device anyway.
//   - hasImprov already evaluates false at the module's call site, so the
//     `if constexpr (hasImprov)` guard at ImprovProvisioningModule.h
//     discards the call. We still need the function symbol to exist for
//     link, hence the stub at the bottom of this section.
// -----------------------------------------------------------------------

} // namespace mm::platform

#ifndef MM_NO_WIFI

#include "driver/uart.h"
#include "improv.h"
#include "core/ImprovFrame.h"

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
            improvSendCurrentState(wifiStaConnected() ? improv::STATE_PROVISIONED : improv::STATE_AUTHORIZED);
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
