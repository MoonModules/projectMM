#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <cstdio>

#include "core/types.h"

extern void mm_main(volatile bool& keepRunning, mm::lengthType gridW, mm::lengthType gridH);

static const char* TAG = "mmv3";
static constexpr int ETH_CONNECTED_BIT = BIT0;
static EventGroupHandle_t ethEventGroup;

static void eth_event_handler(void* arg, esp_event_base_t base,
                              int32_t id, void* data) {
    if (base == ETH_EVENT) {
        if (id == ETHERNET_EVENT_CONNECTED) {
            ESP_LOGI(TAG, "Ethernet link up");
        } else if (id == ETHERNET_EVENT_DISCONNECTED) {
            ESP_LOGI(TAG, "Ethernet link down");
            xEventGroupClearBits(ethEventGroup, ETH_CONNECTED_BIT);
        } else if (id == ETHERNET_EVENT_START) {
            ESP_LOGI(TAG, "Ethernet started");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(ethEventGroup, ETH_CONNECTED_BIT);
    }
}

static void eth_init() {
    ethEventGroup = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default Ethernet netif
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t* eth_netif = esp_netif_new(&netif_cfg);

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
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &eth_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &eth_event_handler, nullptr));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "Waiting for Ethernet connection...");
    xEventGroupWaitBits(ethEventGroup, ETH_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
}

static volatile bool running = true;

extern "C" void app_main() {
    // Initialize NVS (required by some drivers)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Print heap before network init
    std::printf("Heap free: %lu bytes\n",
                static_cast<unsigned long>(esp_get_free_heap_size()));

    eth_init();

    // Print heap after network init
    std::printf("Heap free after Ethernet: %lu bytes\n",
                static_cast<unsigned long>(esp_get_free_heap_size()));

    mm_main(running, mm::defaultGridSize, mm::defaultGridSize);
}
