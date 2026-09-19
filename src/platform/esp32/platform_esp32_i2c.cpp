/// @defgroup platform_esp32_i2c I2C bus diagnostics
/// The scan seam: probe any bus and report which addresses answer, the standard i2cdetect operation.
///
/// Domain-neutral, so any bring-up of a codec or a sensor uses it to confirm wiring.
///
/// @moreinfo
///
/// ## Self-contained, and momentary
///
/// It opens a temporary master bus on the given pins, probes every address and tears the bus down.
/// So it never holds a bus another driver owns, and it allocates and frees per call.
/// An inert stub elsewhere, so a target without the peripheral still links and reports as much.

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

// No I2C peripheral on this target — report the bus as unavailable, distinct from a
// successful scan that found nothing (the module shows "bus in use / unavailable").
size_t i2cScan(uint16_t, uint16_t, uint8_t*, size_t) { return kI2cBusUnavailable; }

}  // namespace mm::platform

#endif  // SOC_I2C_SUPPORTED
