// I2C bus diagnostics — the platform::i2cScan seam (declared in platform.h).
// Domain-neutral: probes any I2C bus and reports which addresses ACK, the
// standard `i2cdetect` operation. The I2cScanModule (src/core/I2cScanModule.h)
// surfaces it; any I2C bring-up (a codec, a sensor) uses it to confirm wiring.
//
// Self-contained: opens a temporary master bus on the given pins, probes every
// 7-bit address, tears the bus down. It does not hold a bus another driver owns
// (the ES8311 codec owns its own bus in platform_esp32_es8311.cpp) — the scan
// is a momentary diagnostic, so it allocates and frees its bus per call.
//
// Gated on SOC_I2C_SUPPORTED with an inert stub otherwise, so any I2C-less
// target links (the module then reports "no I2C on this target").

#include "platform/platform.h"

#include "soc/soc_caps.h"

#include <cstddef>
#include <cstdint>

#if SOC_I2C_SUPPORTED

#include "driver/i2c_master.h"
#include "esp_log.h"

namespace mm::platform {

namespace {
const char* I2C_TAG = "mm_i2c";
}  // namespace

size_t i2cScan(uint16_t sda, uint16_t scl, uint8_t* out, size_t maxOut) {
    if (!out || maxOut == 0) return 0;

    i2c_master_bus_config_t busCfg = {};
    busCfg.i2c_port = I2C_NUM_0;
    busCfg.sda_io_num = static_cast<gpio_num_t>(sda);
    busCfg.scl_io_num = static_cast<gpio_num_t>(scl);
    busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
    busCfg.glitch_ignore_cnt = 7;
    busCfg.flags.enable_internal_pullup = true;

    // A failure here is most often "port already in use" — another driver (the
    // ES8311 codec on I2C_NUM_0) currently holds the bus. Report that distinctly
    // so the UI shows "bus in use", not a misleading "0 devices found".
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&busCfg, &bus) != ESP_OK) {
        ESP_LOGW(I2C_TAG, "i2c bus unavailable (sda %u scl %u) — already in use?", sda, scl);
        return kI2cBusUnavailable;
    }

    // Probe the 7-bit address range (0x01–0x77; 0x00 and 0x78+ are reserved).
    // A 50 ms per-address timeout is ample on a quiet bus and keeps a full scan
    // well under a second — this runs from a UI button, off the render path.
    size_t found = 0;
    for (uint8_t addr = 0x01; addr < 0x78 && found < maxOut; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) out[found++] = addr;
    }

    i2c_del_master_bus(bus);
    return found;
}

}  // namespace mm::platform

#else  // !SOC_I2C_SUPPORTED — inert stub so an I2C-less target links

namespace mm::platform {

size_t i2cScan(uint16_t, uint16_t, uint8_t*, size_t) { return 0; }

}  // namespace mm::platform

#endif  // SOC_I2C_SUPPORTED
