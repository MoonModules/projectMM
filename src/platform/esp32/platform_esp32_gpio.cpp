// GPIO capability introspection for the pin ownership map (PinsModule) — see platform::gpioCapability
// in platform.h. Two data sources, combined:
//   - the IDF's own GPIO_IS_VALID_GPIO / GPIO_IS_VALID_OUTPUT_GPIO / rtc_gpio_is_valid_gpio, the
//     textbook always-correct queries for valid / output-capable / RTC-domain, and
//   - a small per-chip strap/reserved table, because the SDK has NO "is this a boot strap or a
//     flash/PSRAM pin" query — that is board/datasheet knowledge. The table mirrors
//     docs/reference/gpio-usage.md (its single documented source); keep the two in sync.
// No chip type escapes this file (the platform-boundary rule); the module gets a plain GpioCapability.

#include "platform/platform.h"

#include "sdkconfig.h"        // CONFIG_IDF_TARGET_* — selects the per-chip strap/reserved table
#include "soc/gpio_num.h"     // GPIO_IS_VALID_GPIO / GPIO_IS_VALID_OUTPUT_GPIO
#include "driver/gpio.h"      // gpio_get_level / gpio_get_drive_capability — the live-state reads
#include "driver/rtc_io.h"    // rtc_gpio_is_valid_gpio
#include "esp_heap_caps.h"    // heap_caps_get_total_size(MALLOC_CAP_SPIRAM) — detect PSRAM without a new
                              // component dep (the heap component is always linked; esp_psram is not,
                              // and adding it to REQUIRES would switch main to strict mode, hiding the
                              // implicitly-available components the other platform files rely on)

#include <cstddef>
#include <cstdint>

namespace mm::platform {

namespace {

// Per-chip strap + reserved (flash/PSRAM/USB) pins, from docs/reference/gpio-usage.md. Reserved
// pins corrupt the device if used; straps change boot mode if driven at reset. The set is keyed on
// the build's CONFIG_IDF_TARGET (the same discriminator platform_config.h / platform_esp32.cpp use),
// so an octal-PSRAM S3 build sees its 33-37 reserved while a no-PSRAM part would not — the build IS
// the chip variant. A gpio not listed here is neither a strap nor reserved (the SDK queries still
// decide valid/output/rtc). Both helpers are plain linear scans over tiny fixed arrays.
bool inList(uint8_t gpio, const uint8_t* list, size_t n) {
    for (size_t i = 0; i < n; i++) if (list[i] == gpio) return true;
    return false;
}

// Some pins are reserved ONLY when the module has PSRAM — and PSRAM is a runtime fact (the SAME esp32 /
// esp32s3 firmware runs on a bare-WROOM board with none AND a WROVER/octal-PSRAM board with it), so a
// static table can't decide. kReservedIfPsram lists those pins; they're added to the reserved set only
// when esp_psram_is_initialized() is true at runtime. A plain WROOM (e.g. the Olimex ESP32-Gateway) has
// no PSRAM, so its 16/17 stay free — flagging them would be a false positive.
#if defined(CONFIG_IDF_TARGET_ESP32)
// Classic ESP32: flash 6-11 always; 16/17 are the extra flash/PSRAM bus on WROVER modules only.
constexpr uint8_t kReserved[]        = {6, 7, 8, 9, 10, 11};
constexpr uint8_t kReservedIfPsram[] = {16, 17};
constexpr uint8_t kStrap[]           = {0, 2, 5, 12, 15};
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
// ESP32-S3: flash 26-32 always; 33-37 are octal-PSRAM's SPIIO4-7 + DQS, reserved only on an octal-PSRAM
// module (N16R8/R8). Straps 0,45,46 (GPIO3 is a soft strap). JTAG/UART0/USB are role-conflicts, not
// reserved — they stay usable as GPIO, so they are NOT flagged reserved here (a claim on them is legal).
constexpr uint8_t kReserved[]        = {26, 27, 28, 29, 30, 31, 32};
constexpr uint8_t kReservedIfPsram[] = {33, 34, 35, 36, 37};
constexpr uint8_t kStrap[]           = {0, 3, 45, 46};
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
// ESP32-P4: flash/PSRAM are module-internal (the SDK's valid-GPIO query already excludes the
// bonded ones on a given package); straps 34-38.
constexpr uint8_t kReserved[]        = {};
constexpr uint8_t kReservedIfPsram[] = {};
constexpr uint8_t kStrap[]           = {34, 35, 36, 37, 38};
#elif defined(CONFIG_IDF_TARGET_ESP32S31)
// ESP32-S31: flash/PSRAM module-internal (SDK query excludes them). Board-wired peripheral pins
// (RGMII/codec/SD) are a per-board concern the catalog owns, not a chip strap/reserved fact.
constexpr uint8_t kReserved[]        = {};
constexpr uint8_t kReservedIfPsram[] = {};
constexpr uint8_t kStrap[]           = {};
#else
constexpr uint8_t kReserved[]        = {};
constexpr uint8_t kReservedIfPsram[] = {};
constexpr uint8_t kStrap[]           = {};
#endif

// PSRAM presence is a RUNTIME fact — query it once and cache (it can't change after boot). The pins in
// kReservedIfPsram are only really reserved when PSRAM is present; on a bare-WROOM board they're free.
// Use the heap-caps total for the SPIRAM region: 0 = no PSRAM (the heap component is always linked,
// unlike esp_psram — see the include note).
bool psramPresent() {
    static const bool present = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    return present;
}

}  // namespace

GpioCapability gpioCapability(uint8_t gpio) {
    GpioCapability c;
    c.validGpio     = GPIO_IS_VALID_GPIO(gpio);
    c.outputCapable = GPIO_IS_VALID_OUTPUT_GPIO(gpio);
    c.rtc           = rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(gpio));
    c.strap         = inList(gpio, kStrap, sizeof(kStrap));
    c.reserved      = inList(gpio, kReserved, sizeof(kReserved)) ||
                      (psramPresent() && inList(gpio, kReservedIfPsram, sizeof(kReservedIfPsram)));
    return c;
}

GpioLiveState gpioLiveState(uint8_t gpio) {
    GpioLiveState s;
    if (!GPIO_IS_VALID_GPIO(gpio)) return s;   // valid=false → the map omits the live columns
    s.valid = true;
    s.level = gpio_get_level(static_cast<gpio_num_t>(gpio)) != 0;   // reads the pad — see the wire
    gpio_drive_cap_t cap = GPIO_DRIVE_CAP_DEFAULT;
    gpio_get_drive_capability(static_cast<gpio_num_t>(gpio), &cap);
    s.driveCap = static_cast<uint8_t>(cap);    // 0..3 = WEAK / MEDIUM / STRONG / STRONGEST
    // Live pin DIRECTION straight off the pad config (not the role's intent): is the output driver /
    // input buffer enabled right now. A role that should drive but reads back !output = the pin isn't
    // being driven (a dead driver / wire fault) — the mismatch the map flags.
    gpio_io_config_t io = {};
    if (gpio_get_io_config(static_cast<gpio_num_t>(gpio), &io) == ESP_OK) {
        s.output = io.oe;
        s.input  = io.ie;
    }
    return s;
}

// --- GPIO as a working input/output -----------------------------------------------------------
// The two above are the pin map's diagnostics. These are the role: a module that owns a pin reads a
// switch or drives a line through them. Thin by design - gpio_config once, then the register-level
// get/set - so a per-tick poll costs a read and the module keeps the policy (debounce, edges).

bool gpioInputBegin(uint8_t gpio, GpioPull pull) {
    if (!GPIO_IS_VALID_GPIO(gpio)) return false;
    // A RESERVED pin is wired to flash, PSRAM or native USB, and routing I/O there corrupts the
    // device (gpioCapability's own words). Refused rather than configured: the caller reports a pin
    // it could not open, where a corrupted flash reports nothing at all.
    if (gpioCapability(gpio).reserved) return false;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << gpio;
    cfg.mode         = GPIO_MODE_INPUT;
    cfg.pull_up_en   = pull == GpioPull::Up   ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = pull == GpioPull::Down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;      // polled, not interrupt-driven: see the seam's docs
    return gpio_config(&cfg) == ESP_OK;
}

bool gpioRead(uint8_t gpio) {
    if (!GPIO_IS_VALID_GPIO(gpio)) return false;
    return gpio_get_level(static_cast<gpio_num_t>(gpio)) != 0;
}

bool gpioWrite(uint8_t gpio, bool high) {
    // Output-capable, not merely valid: the classic ESP32's 34-39 are input-only, and driving one
    // silently does nothing. Refusing here is what lets a caller report the pin rather than wonder.
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) return false;
    // And not RESERVED, the same policy gpioInputBegin applies: a pin wired to flash, PSRAM or
    // native USB corrupts the device when driven, and a relay list is exactly where a wrong number
    // gets typed.
    if (gpioCapability(gpio).reserved) return false;
    // Configured on first use so a caller that owns the pin just writes it. gpio_config is
    // idempotent, and this runs on a control change, never per frame.
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << gpio;
    // INPUT_OUTPUT, not OUTPUT: plain output leaves the input buffer DISABLED, so gpio_get_level
    // reads 0 on a pin that is really driving high, and the pin map's "see the wire" column lies
    // about it. That cost a long debugging round on a relay that was working the whole time. The
    // input buffer costs nothing here and makes a driven pin readable, which is what every other
    // peripheral on this chip already does (an RMT LED pin reports dir=both for the same reason).
    cfg.mode         = GPIO_MODE_INPUT_OUTPUT;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) return false;
    return gpio_set_level(static_cast<gpio_num_t>(gpio), high ? 1 : 0) == ESP_OK;
}

// The test seams are desktop-only: on a board the pins are real, and a test that wants to inject a
// level has the hardware to do it.
void setTestGpioLevel(uint8_t, bool) {}
void clearTestGpioLevel() {}

}  // namespace mm::platform
