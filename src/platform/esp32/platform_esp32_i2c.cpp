// I2C master — generic register read/write for sensor modules.
//
// Self-contained sibling of platform_esp32.cpp (plan-23 shape). GyroDriver
// and future I2C peripherals call only the platform.h surface; MPU6050 register
// knowledge stays in the module.

#include "platform/platform.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

namespace mm::platform {
namespace {

static const char* I2C_TAG = "mm_i2c";

static i2c_master_bus_handle_t busHandle_ = nullptr;
static i2c_master_dev_handle_t devHandle_ = nullptr;
static uint8_t devAddr_ = 0;

static bool ensureDevice(uint8_t devAddr) {
    if (devHandle_ && devAddr_ == devAddr) return true;

    if (devHandle_) {
        i2c_master_bus_rm_device(devHandle_);
        devHandle_ = nullptr;
        devAddr_ = 0;
    }

    i2c_device_config_t devCfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = devAddr,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,  // 0 = driver default timeout
        .flags = {
            .disable_ack_check = 0,
        },
    };
    esp_err_t err = i2c_master_bus_add_device(busHandle_, &devCfg, &devHandle_);
    if (err != ESP_OK) {
        ESP_LOGE(I2C_TAG, "add device 0x%02X failed: %s", devAddr, esp_err_to_name(err));
        return false;
    }
    devAddr_ = devAddr;
    return true;
}

} // namespace

bool i2cInit(uint8_t sdaPin, uint8_t sclPin) {
    if (busHandle_) return true;

    i2c_master_bus_config_t busCfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = static_cast<gpio_num_t>(sdaPin),
        .scl_io_num = static_cast<gpio_num_t>(sclPin),
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
            .allow_pd = false,
        },
    };

    esp_err_t err = i2c_new_master_bus(&busCfg, &busHandle_);
    if (err != ESP_OK) {
        ESP_LOGE(I2C_TAG, "bus init failed (SDA=%u SCL=%u): %s",
                 static_cast<unsigned>(sdaPin), static_cast<unsigned>(sclPin),
                 esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(I2C_TAG, "I2C bus ready (SDA=%u SCL=%u)",
             static_cast<unsigned>(sdaPin), static_cast<unsigned>(sclPin));
    return true;
}

bool i2cWriteReg(uint8_t devAddr, uint8_t reg, uint8_t value) {
    if (!busHandle_ || !ensureDevice(devAddr)) return false;
    uint8_t buf[2] = { reg, value };
    esp_err_t err = i2c_master_transmit(devHandle_, buf, sizeof(buf), 100);
    if (err != ESP_OK) {
        ESP_LOGW(I2C_TAG, "write 0x%02X reg 0x%02X failed: %s",
                 devAddr, reg, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool i2cReadRegs(uint8_t devAddr, uint8_t reg, uint8_t* buf, size_t len) {
    if (!busHandle_ || !buf || len == 0 || !ensureDevice(devAddr)) return false;
    esp_err_t err = i2c_master_transmit_receive(devHandle_, &reg, 1, buf, len, 100);
    if (err != ESP_OK) {
        ESP_LOGW(I2C_TAG, "read 0x%02X reg 0x%02X len %u failed: %s",
                 devAddr, reg, static_cast<unsigned>(len), esp_err_to_name(err));
        return false;
    }
    return true;
}

} // namespace mm::platform
