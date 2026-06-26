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
#include "esp_cache.h"        // esp_cache_msync — I-cache sync after writing MoonLive code to IRAM
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
// W5500 over SPI: the internal EMAC is absent on the S3, so when the SPI-Ethernet
// driver is enabled (CONFIG_ETH_USE_SPI_ETHERNET, from sdkconfig.defaults.eth-spi)
// AND there is no on-chip EMAC, pull the W5500 MAC/PHY ctors. IDF v6 removed the
// per-PHY SPI drivers from esp_eth core into managed components — these headers
// come from espressif/eth_w5500 (idf_component.yml, gated to the S3). The marker
// MM_ETH_W5500 keeps the rest of the file from repeating this compound condition.
#if defined(CONFIG_ETH_USE_SPI_ETHERNET) && !defined(CONFIG_ETH_USE_ESP32_EMAC)
#define MM_ETH_W5500 1
#include "driver/spi_master.h"   // W5500 SPI Ethernet (S3 boards) — bus + device config
#include "esp_eth_mac_w5500.h"   // espressif/eth_w5500 managed component
#include "esp_eth_phy_w5500.h"
#endif
#ifndef MM_NO_WIFI
#include "esp_wifi.h"
#if defined(CONFIG_IDF_TARGET_ESP32P4)
// On the P4 (WiFi build only — this is inside #ifndef MM_NO_WIFI), esp_wifi_* is
// forwarded to the on-board ESP32-C6 by esp_wifi_remote / esp_hosted. esp_hosted
// self-initialises at boot via a constructor, so no bring-up call is needed (see
// ensureWifiInit); this header is only for the read-only coprocessorWifi() query
// that reports the C6's slave-firmware version. Matches the guard on that function.
#include "esp_hosted.h"
#endif
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

// Executable memory for MoonLive's emitted code (Xtensa or RISC-V). MALLOC_CAP_EXEC forces
// an allocation from IRAM (instruction-bus-fetchable). nullptr when IRAM is exhausted — the
// caller degrades (the scripted module reports "no memory", runs dark), never crashes. IRAM
// competes with WiFi/driver IRAM, so a failure here is expected on a busy device and must be
// handled, not asserted. The request is rounded up to a 4-byte word: writeExec's final
// partial-word store and the esp_cache_msync length both round up to a word, so the block
// must hold that whole word even when the caller's len isn't a multiple of 4.
void* allocExec(size_t bytes) {
    if (bytes == 0) return nullptr;
    size_t padded = (bytes + 3) & ~size_t(3);
    return heap_caps_malloc(padded, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
}

void freeExec(void* ptr, size_t /*bytes*/) {
    heap_caps_free(ptr);   // size is the desktop munmap's; IRAM free needs only the ptr
}

void writeExec(void* dst, const void* src, size_t len) {
    if (!dst || !src || !len) return;
    // IRAM is writable only by 32-bit-aligned WORD stores (a byte/halfword store to
    // IRAM faults), so copy word-by-word, padding the final partial word with the
    // bytes already there — never a sub-word store. allocExec returns 4-byte-aligned
    // IRAM, so dst is aligned; src may not be, so read it bytewise into the word.
    auto* d = static_cast<volatile uint32_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    size_t words = len / 4;
    for (size_t i = 0; i < words; i++) {
        uint32_t w = static_cast<uint32_t>(s[i*4]) | (static_cast<uint32_t>(s[i*4+1]) << 8) |
                     (static_cast<uint32_t>(s[i*4+2]) << 16) | (static_cast<uint32_t>(s[i*4+3]) << 24);
        d[i] = w;
    }
    size_t rem = len % 4;
    if (rem) {
        uint32_t w = d[words];                       // preserve the untouched high bytes
        for (size_t b = 0; b < rem; b++) {
            w &= ~(0xFFu << (b*8));
            w |= static_cast<uint32_t>(s[words*4 + b]) << (b*8);
        }
        d[words] = w;
    }
    // Make the freshly-written code visible to instruction fetch. The bytes went in via
    // DATA-bus stores, so on a cache-backed exec region (the P4) they may still sit in the
    // data cache — two steps are needed, in order:
    //   1. write the data cache back to RAM (TYPE_DATA, C2M) so RAM holds the code, and
    //   2. invalidate the instruction cache for the range so the core refetches it.
    // A single TYPE_INST msync only does step 2 — on the P4 that refetches STALE RAM (the
    // bytes never left the data cache) and the core decodes garbage → illegal instruction.
    // On the S3, MALLOC_CAP_EXEC is directly-executable SRAM so this is belt-and-suspenders,
    // but it is correct on both. UNALIGNED because the code block isn't cache-line sized.
    const size_t paddedLen = (len + 3) & ~size_t(3);
    esp_cache_msync(dst, paddedLen,
                    ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(dst, paddedLen,
                    ESP_CACHE_MSYNC_FLAG_TYPE_INST | ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
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

// Read a netif's current IPv4 as raw octets (out[0..3]); all-zero on no IP /
// null netif. esp_ip4_addr_t.addr is little-endian-packed (octet i = byte i),
// matching IP2STR's `(addr >> (8*i)) & 0xff` — so this is the byte-form of the
// same value the old IPSTR getters printed. Shared by ethGetIPv4/wifiStaGetIPv4.
static void netifIPv4(esp_netif_t* netif, uint8_t out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!netif) return;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return;
    const uint32_t a = info.ip.addr;
    out[0] = static_cast<uint8_t>(a & 0xff);
    out[1] = static_cast<uint8_t>((a >> 8) & 0xff);
    out[2] = static_cast<uint8_t>((a >> 16) & 0xff);
    out[3] = static_cast<uint8_t>((a >> 24) & 0xff);
}

const char* sdkVersion() {
    return esp_get_idf_version();
}

const char* coprocessorWifi() {
#if defined(CONFIG_IDF_TARGET_ESP32P4) && !defined(MM_NO_WIFI)
    // The P4's WiFi runs on the on-board ESP32-C6 via esp_hosted. Ask the host API
    // what slave firmware version the C6 actually reported over the link. A version
    // of 0.0.0 (or an error) means the slave never completed its handshake — the
    // signature of absent / incompatible C6 slave firmware, which is exactly the
    // case we want to surface rather than infer.
    static char buf[24] = "querying…";
    esp_hosted_coprocessor_fwver_t ver = {};
    if (esp_hosted_get_coprocessor_fwversion(&ver) == ESP_OK
        && (ver.major1 || ver.minor1 || ver.patch1)) {
        std::snprintf(buf, sizeof(buf), "C6 fw %u.%u.%u",
                      static_cast<unsigned>(ver.major1),
                      static_cast<unsigned>(ver.minor1),
                      static_cast<unsigned>(ver.patch1));
    } else {
        std::snprintf(buf, sizeof(buf), "not detected");
    }
    return buf;
#else
    return "";   // native-radio targets have no WiFi co-processor
#endif
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
// Retained so a live W5500 reconfigure (ethStop → re-init) can tear the driver
// down cleanly. eth_handle is the running driver (set on both RMII and W5500 init);
// ethSpiActive_ records that the SPI bus was initialised (so ethStop frees it) and
// so only exists on W5500 builds — gating it keeps the classic/P4 (RMII-only) build
// free of an unused-variable warning under -Werror.
static esp_eth_handle_t ethHandle_ = nullptr;
#ifdef MM_ETH_W5500
static bool ethSpiActive_ = false;
#endif
#endif
static bool netifInitDone_ = false;

// DHCP hostname (option 12) pushed by NetworkModule before bring-up; applied to each
// netif before its DHCP client starts (see setHostname's contract in platform.h).
// 32 = the ESP-IDF lwIP hostname cap; empty means "leave the IDF default".
//
// Threading / ordering contract: setHostname() is called from the app task in
// NetworkModule::setup(), BEFORE ethInit() / wifiStaInit() bring an interface up.
// applyHostname() (the only reader) runs later — from the eth link-up event handler
// or right after esp_wifi_start — so hostname_[] is fully written before any reader
// can execute, and no lock is needed. Do NOT call setHostname() concurrently with, or
// after, bring-up (e.g. from another task or an event callback) without adding a
// mutex / std::atomic; the single-writer-before-readers ordering is the whole safety.
static char hostname_[32] = {};

void setHostname(const char* name) {
    if (!name) { hostname_[0] = 0; return; }
    std::strncpy(hostname_, name, sizeof(hostname_) - 1);
    hostname_[sizeof(hostname_) - 1] = 0;
}

// Apply the stored hostname (DHCP option 12) to a netif so it rides the DISCOVER.
// Call AFTER the interface is started (set_hostname returns IF_NOT_READY before).
// Order is the crux — esp_netif_set_hostname only takes on a STOPPED DHCP client;
// setting it while the client is running (which it is on Ethernet, started at
// link-up) is ignored, so the DISCOVER goes out nameless and the router logs the
// lease with a blank hostname. So: stop the client, set the name, start it — the
// fresh DISCOVER then carries option 12. Stopping an already-stopped client is a
// benign ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED, which we ignore. No-op when unset.
static void applyHostname(esp_netif_t* netif) {
    if (!netif || !hostname_[0]) return;
    esp_netif_dhcpc_stop(netif);    // must be stopped for set_hostname to take; ignore ALREADY_STOPPED
    esp_err_t e = esp_netif_set_hostname(netif, hostname_);
    if (e != ESP_OK) ESP_LOGW(NET_TAG, "set_hostname('%s') failed: %s", hostname_, esp_err_to_name(e));
    else ESP_LOGI(NET_TAG, "DHCP hostname: %s", hostname_);
    // Restart the DHCP client and check the result — if it fails, the interface has
    // no DHCP client and will never acquire an IP, so surface it rather than silently
    // leaving the device offline. (Don't return on stop/set failure above: we still
    // must restart the client we stopped.)
    esp_err_t se = esp_netif_dhcpc_start(netif);
    if (se != ESP_OK)
        ESP_LOGW(NET_TAG, "dhcpc_start after set_hostname failed: %s", esp_err_to_name(se));
}

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
            // Set the DHCP hostname HERE, on link-up, not in ethInit(): IDF's default
            // eth netif starts the DHCP client from its own CONNECTED handler, so a
            // hostname set earlier (in ethInit, before link-up) is clobbered when that
            // client (re)starts nameless — the lease lands blank. Bouncing the client
            // here (after the netif is started, when set_hostname takes) makes the
            // DISCOVER carry the name. WiFi doesn't need this: its DHCP client only
            // starts on association, well after we set the name in wifiStaInit.
            applyHostname(ethNetif_);
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

// Runtime eth pin/PHY config. Seeded with the per-chip default (ethConfigDefault)
// so an un-provisioned board still comes up on its historical pins; NetworkModule
// overrides it via setEthConfig() with the board's deviceModels.json values before
// ethInit(). The DRIVER for each phyType is compiled in per chip (RMII for
// classic/P4, W5500 SPI for S3 — sdkconfig); this only selects pins + which to use.
static EthPinConfig ethConfig_ = ethConfigDefault;

void setEthConfig(const EthPinConfig& cfg) { ethConfig_ = cfg; }

// Internal-EMAC RMII path — only on chips with an on-chip EMAC (classic ESP32,
// P4). The S3 has no EMAC, so esp_eth_mac_new_esp32 / eth_esp32_emac_config_t /
// EMAC_CLK_* don't exist there; gating on CONFIG_ETH_USE_ESP32_EMAC keeps this
// function out of the S3 build (where Ethernet is W5500-over-SPI instead).
#ifdef CONFIG_ETH_USE_ESP32_EMAC
static bool ethInitRmii() {
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    ethNetif_ = esp_netif_new(&netif_cfg);

    // RMII / PHY pins from the runtime ethConfig_ (the default LAN8720 map by default, the
    // P4-NANO's IP101 map on the P4, or a board override pushed from deviceModels.json).
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.clock_config.rmii.clock_mode =
        ethConfig_.rmiiClockExtIn ? EMAC_CLK_EXT_IN : EMAC_CLK_OUT;
    emac_config.clock_config.rmii.clock_gpio =
        static_cast<gpio_num_t>(ethConfig_.rmiiClockGpio);
    if (ethConfig_.mdcGpio >= 0)  emac_config.smi_gpio.mdc_num  = ethConfig_.mdcGpio;
    if (ethConfig_.mdioGpio >= 0) emac_config.smi_gpio.mdio_num = ethConfig_.mdioGpio;
    // NOTE: the RMII *data* GPIOs (TX_EN/TXD0/TXD1/CRS_DV/RXD0/RXD1) are left at the
    // ETH_ESP32_EMAC_DEFAULT_CONFIG() defaults. On the classic ESP32 they're fixed in
    // silicon; on the P4 the macro already defaults them to 49/34/35/28/29/30 (the
    // NANO wiring) — the proven round-1 P4 build relied on exactly these defaults, so
    // we don't override them. (deviceModels.json doesn't carry them either.)

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ethConfig_.phyAddr;
    phy_config.reset_gpio_num = ethConfig_.rstGpio;

    // Helper to unwind whatever was created so far on any failure — ethInit
    // runs once at boot, but a clean teardown means a broken PHY/cable degrades
    // (returns false → the WiFi/AP cascade takes over) instead of leaking the
    // netif + MAC/PHY drivers.
    auto fail = [&](const char* what, esp_eth_mac_t* m, esp_eth_phy_t* p) -> bool {
        ESP_LOGE(NET_TAG, "Ethernet %s", what);
        if (p) p->del(p);
        if (m) m->del(m);
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        return false;
    };

    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (!mac) return fail("MAC create failed", nullptr, nullptr);
    // IP101 (P4-NANO) is a managed-component PHY ctor (espressif/ip101 in
    // idf_component.yml; removed from esp_eth core in IDF v6); the generic ctor
    // (LAN8720) stays in core. The IP101 symbol is only declared on the
    // P4 build (its header include is #ifdef'd), so the runtime phyType branch
    // below must be wrapped in `#ifdef CONFIG_IDF_TARGET_ESP32P4` — otherwise the
    // non-P4 build would fail to compile the undeclared esp_eth_phy_new_ip101 call.
    esp_eth_phy_t* phy;
#ifdef CONFIG_IDF_TARGET_ESP32P4
    if (ethConfig_.phyType == ethIp101) phy = esp_eth_phy_new_ip101(&phy_config);
    else                                phy = esp_eth_phy_new_generic(&phy_config);
#else
    phy = esp_eth_phy_new_generic(&phy_config);   // LAN8720 / generic RMII
#endif
    if (!phy) return fail("PHY create failed", mac, nullptr);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = nullptr;
    esp_err_t err = esp_eth_driver_install(&eth_config, &eth_handle);
    if (err != ESP_OK) {
        return fail(esp_err_to_name(err), mac, phy);
    }
    // From here the driver owns mac+phy (driver_uninstall frees them); the
    // remaining failure paths uninstall the driver instead of del-ing mac/phy.
    ESP_ERROR_CHECK(esp_netif_attach(ethNetif_, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &ethEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &ethEventHandler, nullptr));

    err = esp_eth_start(eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethEventHandler);
        esp_eth_driver_uninstall(eth_handle);  // frees mac + phy
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        return false;
    }
    // DHCP hostname is set in the ETHERNET_EVENT_CONNECTED handler, not here — see
    // the comment there (IDF starts the eth DHCP client on link-up, which would
    // clobber a name set at init time).

    ethHandle_ = eth_handle;   // retained (ethStop is W5500-only today, but keep it set)
    ESP_LOGI(NET_TAG, "Ethernet init done (RMII, non-blocking)");
    return true;
}
#endif // CONFIG_ETH_USE_ESP32_EMAC

// W5500 external Ethernet over SPI — the S3 path (no internal EMAC). The whole
// function is compiled only where MM_ETH_W5500 is set (SPI-eth driver enabled via
// sdkconfig.defaults.eth-spi AND no on-chip EMAC — see the include block); the W5500
// ctors come from the espressif/w5500 managed component, absent otherwise. The
// ethInit() dispatch only calls it under the same guard, so gating the definition
// keeps the classic/P4 (RMII-only) build free of an unused-function warning under
// -Werror. Reads the SPI pins from the runtime ethConfig_ (a W5500 board MUST set
// them via deviceModels.json — no universal default). Returns false (→ WiFi cascade) on
// any failure, including no W5500 present, so a build with the driver in but no
// module attached degrades cleanly.
#ifdef MM_ETH_W5500
static bool ethInitSpi() {
    if (ethConfig_.spiMiso < 0 || ethConfig_.spiMosi < 0 ||
        ethConfig_.spiSck < 0 || ethConfig_.spiCs < 0) {
        ESP_LOGW(NET_TAG, "W5500 selected but SPI pins unset — skipping (set them in deviceModels.json)");
        return false;
    }
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    ethNetif_ = esp_netif_new(&netif_cfg);

    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num = ethConfig_.spiMiso;
    buscfg.mosi_io_num = ethConfig_.spiMosi;
    buscfg.sclk_io_num = ethConfig_.spiSck;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    constexpr spi_host_device_t kSpiHost = SPI2_HOST;
    if (spi_bus_initialize(kSpiHost, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(NET_TAG, "W5500 SPI bus init failed");
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        return false;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.mode = 0;
    devcfg.clock_speed_hz = 20 * 1000 * 1000;   // 20 MHz — W5500 spec ceiling for stable SPI
    devcfg.spics_io_num = ethConfig_.spiCs;
    devcfg.queue_size = 20;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(kSpiHost, &devcfg);
    w5500_config.int_gpio_num = ethConfig_.spiIrq;   // -1 → W5500 driver falls back to polling

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ethConfig_.phyAddr;
    phy_config.reset_gpio_num = ethConfig_.rstGpio;   // -1 if the module self-resets

    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_config);
    auto fail = [&](const char* what) -> bool {
        ESP_LOGE(NET_TAG, "W5500 %s", what);
        if (phy) phy->del(phy);
        if (mac) mac->del(mac);
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        spi_bus_free(kSpiHost);
        return false;
    };
    if (!mac) return fail("MAC create failed");
    if (!phy) return fail("PHY create failed");

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = nullptr;
    if (esp_eth_driver_install(&eth_config, &eth_handle) != ESP_OK) return fail("driver install failed");

    // W5500 has no factory MAC — derive one from the chip's efuse base MAC so the
    // netif has a unique address (IDF requirement for SPI Ethernet).
    uint8_t mac_addr[6];
    esp_read_mac(mac_addr, ESP_MAC_ETH);
    esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr);

    ESP_ERROR_CHECK(esp_netif_attach(ethNetif_, esp_eth_new_netif_glue(eth_handle)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethEventHandler, nullptr));

    if (esp_eth_start(eth_handle) != ESP_OK) {
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethEventHandler);
        esp_eth_driver_uninstall(eth_handle);
        if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
        spi_bus_free(kSpiHost);
        return false;
    }
    // DHCP hostname is set in the ETHERNET_EVENT_CONNECTED handler (see note there).
    ethHandle_ = eth_handle;   // retained for a live reconfigure (ethStop)
    ethSpiActive_ = true;
    ESP_LOGI(NET_TAG, "Ethernet init done (W5500 SPI, non-blocking)");
    return true;
}
#endif // MM_ETH_W5500

// Tear down a running Ethernet driver so a fresh ethInit() can bring it up with
// new config — the live-reconfigure path. Today only the W5500 SPI driver uses
// this (clean stop/uninstall/free-bus); RMII keeps apply-on-next-init (its
// teardown is fiddlier — backlog "live RMII reconfigure"). Safe to call when
// nothing is running. After this, ethInit() can be called again.
void ethStop() {
    if (!ethHandle_) return;
    esp_eth_stop(ethHandle_);
    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethEventHandler);
    esp_eth_driver_uninstall(ethHandle_);
    ethHandle_ = nullptr;
    if (ethNetif_) { esp_netif_destroy(ethNetif_); ethNetif_ = nullptr; }
#ifdef MM_ETH_W5500
    if (ethSpiActive_) { spi_bus_free(SPI2_HOST); ethSpiActive_ = false; }
#endif
    ethLinkUp_ = false;
    ethConnected_ = false;
}

bool ethInit() {
    ensureNetifInit();
    // Dispatch on the board's PHY type (runtime, from deviceModels.json via setEthConfig).
    // Each path returns false on any failure (incl. no PHY present) so NetworkModule
    // cascades to WiFi — a default build with a driver compiled in but no PHY wired
    // just falls through, no GPIO grab, no hang. A PHY whose driver isn't compiled
    // into this chip's firmware (e.g. ethW5500 on a classic build, or RMII on the
    // S3) returns false the same way — the case is gated to where the ctor exists.
    switch (ethConfig_.phyType) {
#ifdef MM_ETH_W5500
        case ethW5500:   return ethInitSpi();
#endif
#ifdef CONFIG_ETH_USE_ESP32_EMAC
        case ethLan8720:
        case ethIp101:   return ethInitRmii();
#endif
        default:         return false;   // ethNone, or a PHY this firmware can't drive
    }
}

bool ethLinkUp() {
    return ethLinkUp_;
}

bool ethConnected() {
    return ethConnected_;
}

void ethGetIPv4(uint8_t out[4]) {
    netifIPv4(ethNetif_, out);
}

#else // MM_NO_ETH — firmware excludes EMAC support (chip-side or sdkconfig fragment
      // wasn't layered. Provide stubs matching the desktop platform's no-eth
      // behaviour so NetworkModule's cascade falls straight to WiFi (or AP).

void setEthConfig(const EthPinConfig&)  {}
void ethStop()                          {}
bool ethInit()                          { return false; }
bool ethLinkUp()                        { return false; }
bool ethConnected()                     { return false; }
void ethGetIPv4(uint8_t out[4])         { out[0] = out[1] = out[2] = out[3] = 0; }

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

    // P4 note: the P4 has no native radio — WiFi runs on the on-board ESP32-C6 via
    // esp_wifi_remote / esp_hosted (the esp32p4-eth-wifi build). No bring-up code is
    // needed here: esp_hosted self-initialises at boot via a constructor
    // (ESP_SYSTEM_INIT_FN → esp_hosted_init, the `host_init: ESP Hosted` boot line),
    // which sets up the SDIO transport, RPC, and the wifi-remote channels and
    // connects to the C6. After that the esp_wifi_* calls below are forwarded to the
    // C6 unchanged. Do NOT call esp_hosted_init()/esp_hosted_connect_to_slave() here:
    // init is already done (idempotent no-op), and connect_to_slave() is actually a
    // transport *reconfigure* that resets the slave (GPIO 54) and re-inits SDIO —
    // which on a live link fails (`sdmmc_card_init failed`) and tears down the
    // working boot-time connection. Proven on the P4-NANO bench (2026-06-12).

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
    // DHCP hostname (option 12) — after esp_wifi_start: the STA netif isn't "ready"
    // (set_hostname returns IF_NOT_READY) until the WiFi driver glue starts it.
    // Association + DHCP happen later still, so the name lands in the lease request.
    applyHostname(staNetif_);

    // Disable WiFi modem power-save. IDF defaults to WIFI_PS_MIN_MODEM, which
    // DTIM-sleeps the radio between beacons — that sleep causes intermittent
    // multi-hundred-ms stalls in TCP socket handling (the HTTP server wedges
    // while UDP/DDP keeps flowing) and the LED-pause class of glitch. The whole
    // lineage (WLED, v1/v2) turns it off for the same reason; a wall-powered LED
    // controller has no battery to save. Non-fatal if it fails (older IDF / odd
    // chip) — log and carry on.
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) {
        ESP_LOGW(NET_TAG, "WiFi power-save disable failed: %s", esp_err_to_name(err));
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

void wifiStaGetIPv4(uint8_t out[4]) {
    netifIPv4(staNetif_, out);
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
void wifiStaGetIPv4(uint8_t out[4])      { out[0] = out[1] = out[2] = out[3] = 0; }
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

// Bring the mDNS stack up (idempotent) and ADVERTISE this device as <deviceName>.local.
// Advertising is gated by the user's mDNS toggle; the stack init is NOT — browse needs
// the stack regardless (see mdnsBrowseStart), so mdns_init stays even when the toggle is
// off. mdns_init is safe to call when already running (returns an already-init error we
// treat as fine).
static bool mdnsStackUp_ = false;

static bool ensureMdnsStack() {
    if (mdnsStackUp_) return true;
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return false;
    }
    mdnsStackUp_ = true;
    return true;
}

bool mdnsInit(const char* deviceName) {
    if (!ensureMdnsStack()) return false;
    esp_err_t err = mdns_hostname_set(deviceName);
    if (err != ESP_OK) {
        ESP_LOGE(NET_TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        return false;
    }
    // Advertise an `_http._tcp` service so other devices DISCOVER us by browsing the
    // service type (not just by resolving our hostname) — the standard, push-style way
    // a web device announces itself (WLED, ESPHome, Hue all advertise `_http._tcp`).
    // This is what lets two projectMM devices find each other over mDNS with no subnet
    // sweep. The instance name is the deviceName; the port is the HTTP server's (80).
    // Branch on whether the service already exists (a reconnect re-runs this) rather
    // than treating ANY mdns_service_add failure as "already there" — an add failure
    // could be OOM/invalid-state, which must surface, not be silently logged as started.
    esp_err_t svcErr = mdns_service_exists("_http", "_tcp", nullptr)
        ? mdns_service_instance_name_set("_http", "_tcp", deviceName)
        : mdns_service_add(deviceName, "_http", "_tcp", 80, nullptr, 0);
    if (svcErr != ESP_OK) {
        ESP_LOGE(NET_TAG, "mDNS _http._tcp advertise failed: %s", esp_err_to_name(svcErr));
        return false;   // hostname is set, but advertising failed — report it
    }
    // Tag the service with a `mm=1` TXT record so a browsing projectMM peer can tell us
    // apart from a generic `_http._tcp` web box (WLED/ESPHome/Hue all share that service)
    // WITHOUT an HTTP probe — DevicesModule reads this to classify the peer as projectMM
    // straight from the browse. Idempotent: set on both the add and reconnect paths. A
    // TXT failure is non-fatal (advertising still works; the peer just falls back to the
    // HTTP scan to classify us), so it's logged, not returned.
    esp_err_t txtErr = mdns_service_txt_item_set("_http", "_tcp", "mm", "1");
    if (txtErr != ESP_OK)
        ESP_LOGW(NET_TAG, "mDNS _http._tcp TXT mm=1 set failed: %s", esp_err_to_name(txtErr));
    ESP_LOGI(NET_TAG, "mDNS started: %s.local (advertising _http._tcp:80, mm=1)", deviceName);
    return true;
}

void mdnsStop() {
    // Tearing the stack down would also kill browse. The toggle-off path wants to stop
    // ADVERTISING, not lose discovery — so keep the stack but drop both the advertised
    // hostname AND the _http._tcp service record; full mdns_free only on teardown
    // (where everything stops anyway). mdns_service_remove is a no-op if not added.
    if (mdnsStackUp_) {
        mdns_service_remove("_http", "_tcp");
        mdns_hostname_set("");
    }
}

// Full stack teardown — only on module teardown, where browse stops too.
void mdnsShutdown() {
    if (mdnsStackUp_) { mdns_free(); mdnsStackUp_ = false; }
}

// --- mDNS service browse (synchronous, bounded) ---

// One synchronous PTR browse for `service`/`proto`, blocking up to `timeoutMs`, then it
// frees everything it allocated before returning. Self-contained ON PURPOSE: the earlier
// async API (mdns_query_async_new + poll-the-handle-across-ticks) raced the mDNS task's
// own search-expiry timer — when a query's window lapsed, the component freed the search's
// internal queue, and our next-tick poll asserted on it (xQueueSemaphoreTake on a null
// queue, crashing on a UI refresh). Holding no handle across ticks closes that window by
// construction. The cost is a bounded blocking call: DevicesModule calls this on loop1s
// (not the render hot path) for ONE service type per tick with a small timeout, the
// standard mDNS-query pattern (WLED/ESPHome do the same), so the tick budget is fine.
bool mdnsBrowse(const char* service, const char* proto, uint32_t timeoutMs,
                MdnsHostCb cb, void* user) {
    // Browse needs only the mDNS stack, not advertising — bring it up regardless of the
    // advertise toggle (mdnsStop clears the hostname but keeps the stack), so a device
    // that doesn't advertise can still discover others.
    if (!ensureMdnsStack()) return false;
    mdns_result_t* results = nullptr;
    if (mdns_query_ptr(service, proto, timeoutMs, 16, &results) != ESP_OK) return false;
    for (mdns_result_t* r = results; r && cb; r = r->next) {
        MdnsHost h{};
        // A PTR/service browse gives the friendly service *instance* name in
        // `instance_name` (what we advertise — the deviceName, e.g. "Bench-P4") and the
        // lower-level host record in `hostname`. Prefer the instance name so a peer shows
        // the device's name, not its `.local` host; fall back to hostname if absent.
        const char* name = (r->instance_name && r->instance_name[0]) ? r->instance_name
                          : (r->hostname ? r->hostname : nullptr);
        if (name) std::snprintf(h.hostname, sizeof(h.hostname), "%s", name);
        h.port = r->port;
        // Scan the service's TXT records for our `mm=1` marker — a projectMM device tags
        // its _http._tcp advertisement with it (see mdnsInit), so a peer browsing the
        // generic _http._tcp service can classify us without an HTTP probe.
        for (size_t i = 0; i < r->txt_count; i++) {
            if (r->txt[i].key && std::strcmp(r->txt[i].key, "mm") == 0
                && r->txt_value_len[i] == 1 && r->txt[i].value && r->txt[i].value[0] == '1') {
                h.isProjectMM = true;
                break;
            }
        }
        // First IPv4 address in the result's addr list.
        for (mdns_ip_addr_t* a = r->addr; a; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                uint32_t v = a->addr.u_addr.ip4.addr;   // little-endian packed (octet i = byte i)
                h.ip[0] = v & 0xff; h.ip[1] = (v >> 8) & 0xff;
                h.ip[2] = (v >> 16) & 0xff; h.ip[3] = (v >> 24) & 0xff;
                break;
            }
        }
        cb(h, user);
    }
    if (results) mdns_query_results_free(results);
    return true;
}

// UdpSocket

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open() {
    if (fd_ >= 0) return true;
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    // Allow sends to a broadcast address (e.g. 255.255.255.255 for an Art-Net /
    // E1.31 spray to every device on the LAN). Without SO_BROADCAST the stack
    // rejects such a send; it has no effect on unicast/multicast sends.
    const int on = 1;
    setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    return true;
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

int TcpConnection::writeSome(const uint8_t* data, size_t len) {
    if (fd_ < 0) return -1;
    if (len == 0) return 0;
    ssize_t n = lwip_write(fd_, data, len);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  // buffer full — try later
    return -1;                                              // real socket error
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
