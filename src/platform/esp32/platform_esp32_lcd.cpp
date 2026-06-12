// LCD_CAM parallel WS2812 output — the peripheral half of the LCD LED driver
// (ESP32-S3). The driver (src/light/drivers/LcdLedDriver.h) does all the
// domain work: applies Correction and 3-slot-encodes every light into the DMA
// frame buffer (LcdSlots.h). This file owns only the peripheral — the esp_lcd
// i80 bus, the IO device, the DMA-capable frame buffer, transmit + wait, and
// the loopback test's TX side. No domain logic here.
//
// Design: the whole frame is pre-encoded into ONE buffer and sent as ONE
// gapless GDMA stream (tx_color with lcd_cmd = -1 → pure data phase). Once
// started, no CPU work remains until the done callback — there is no refill
// deadline for WiFi to miss, which is the deliberate difference from the
// ISR-refilled rings in the hpwit/FastLED LCD drivers this design studied.
//
// The file compiles on every ESP32 chip: everything is under
// SOC_LCDCAM_I80_LCD_SUPPORTED with inert stubs otherwise (classic ESP32 builds
// it too; the driver never calls in thanks to platform::lcdLanes == 0). Gate on
// SOC_LCDCAM_I80_LCD_SUPPORTED, NOT SOC_LCD_I80_SUPPORTED: the classic ESP32 sets
// the latter for its unrelated I2S-LCD peripheral, which wired this driver onto a
// chip with no LCD_CAM and hung its boot (see platform_config.h + decisions.md).

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

// SOC_LCDCAM_I80_LCD_SUPPORTED, not SOC_LCD_I80_SUPPORTED: the classic ESP32
// sets the latter for its I2S-LCD peripheral, which is NOT the LCD_CAM i80 bus
// esp_lcd drives here — compiling this body for the classic chip wired the
// driver onto it and hung its boot. Mirror the lcdLanes gate in
// platform_config.h. (esp_lcd headers below only exist where LCD_CAM does.)
#if SOC_LCDCAM_I80_LCD_SUPPORTED

#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstring>
#include <functional>  // the transmit callback passed to the shared frame loopback
#include <new>      // std::nothrow

namespace mm::platform {

// Defined in platform_esp32_rmt.cpp — the plain-GPIO continuity pre-check the
// RMT loopback uses; the wire question is identical here.
namespace detail { bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio); }

namespace {

static const char* LCD_TAG = "mm_lcd";

// 3 slots per WS2812 bit (the LcdSlots.h contract): 2.67 MHz pclk = 375 ns
// slots, "0" = 1 slot HIGH, "1" = 2 slots HIGH. 375 ns and not the lineage's
// usual 416 ns: newer WS2812B revisions spec T0H max ≈ 380 ns, and on a
// direct 3.3 V data line (no level shifter) a longer "0" pulse gets misread
// as "1" — the strip washes out white. 375 ns sits inside every revision's
// window; the 160 MHz LCD clock divides to it exactly (/60).
constexpr uint32_t kPclkHz = 2'666'666;

struct LcdState {
    esp_lcd_i80_bus_handle_t bus = nullptr;
    esp_lcd_panel_io_handle_t io = nullptr;
    SemaphoreHandle_t done = nullptr;
    uint8_t* buf = nullptr;
    size_t cap = 0;
};

// Done-callback: the GDMA stream finished — release the waiter.
bool lcdDoneCb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* user) {
    auto* st = static_cast<LcdState*>(user);
    BaseType_t high = pdFALSE;
    xSemaphoreGiveFromISR(st->done, &high);
    return high == pdTRUE;
}

void destroyState(LcdState* st) {
    if (!st) return;
    if (st->io) esp_lcd_panel_io_del(st->io);
    if (st->bus) esp_lcd_del_i80_bus(st->bus);
    if (st->buf) heap_caps_free(st->buf);
    if (st->done) vSemaphoreDelete(st->done);
    delete st;
}

// One bus + IO device + zeroed DMA buffer. Shared by the runtime init and the
// loopback's private 1-lane bus.
LcdState* createState(const uint16_t* dataPins, uint8_t laneCount,
                      uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes) {
    auto* st = new (std::nothrow) LcdState();
    if (!st) return nullptr;

    esp_lcd_i80_bus_config_t busCfg = {};
    busCfg.dc_gpio_num = static_cast<gpio_num_t>(dcGpio);
    busCfg.wr_gpio_num = static_cast<gpio_num_t>(wrGpio);
    busCfg.clk_src = LCD_CLK_SRC_DEFAULT;
    busCfg.bus_width = 8;
    for (size_t i = 0; i < ESP_LCD_I80_BUS_WIDTH_MAX; i++) {
        busCfg.data_gpio_nums[i] = GPIO_NUM_NC;
    }
    for (uint8_t i = 0; i < laneCount && i < 8; i++) {
        busCfg.data_gpio_nums[i] = static_cast<gpio_num_t>(dataPins[i]);
    }
    busCfg.max_transfer_bytes = bufferBytes;
    busCfg.dma_burst_size = 64;
    if (esp_lcd_new_i80_bus(&busCfg, &st->bus) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }

    st->done = xSemaphoreCreateBinary();
    if (!st->done) {
        destroyState(st);
        return nullptr;
    }

    esp_lcd_panel_io_i80_config_t ioCfg = {};
    ioCfg.cs_gpio_num = GPIO_NUM_NC;    // no chip select — we own the bus
    ioCfg.pclk_hz = kPclkHz;
    ioCfg.trans_queue_depth = 1;        // synchronous full-frame: one in flight
    ioCfg.on_color_trans_done = lcdDoneCb;
    ioCfg.user_ctx = st;
    ioCfg.lcd_cmd_bits = 0;             // no command phase ever (tx_color cmd = -1)
    ioCfg.lcd_param_bits = 0;
    ioCfg.flags.pclk_idle_low = 1;      // WR rests LOW like the data lines
    if (esp_lcd_new_panel_io_i80(st->bus, &ioCfg, &st->io) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }

    // DMA-capable INTERNAL RAM with the bus's alignment — the esp_lcd helper
    // handles both. Zeroed so the trailing latch pad (and any unwritten tail)
    // holds the lines LOW. Internal (not PSRAM) for now: the i80 GDMA *can* burst
    // from PSRAM (access_ext_mem), so moving big frames there to free scarce DRAM
    // is a tracked follow-up (backlog § LCD/Parlio DMA frame buffer → PSRAM) — it
    // needs the wider ext-mem alignment + on-hardware proof, so not done here.
    st->buf = static_cast<uint8_t*>(esp_lcd_i80_alloc_draw_buffer(
        st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!st->buf) {
        destroyState(st);
        return nullptr;
    }
    std::memset(st->buf, 0, bufferBytes);
    st->cap = bufferBytes;
    return st;
}

} // namespace

bool lcdWs2812Init(LcdWs2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                   uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes) {
    if (!dataPins || laneCount == 0 || bufferBytes == 0) return false;
    // Keep the platform memory reserve intact — degrade (init failure → driver
    // idles with a status error) rather than starve WiFi/HTTP of internal RAM.
    if (heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
        < bufferBytes + HEAP_RESERVE) {
        return false;
    }
    LcdState* st = createState(dataPins, laneCount, wrGpio, dcGpio, bufferBytes);
    if (!st) return false;
    h.impl = st;
    return true;
}

uint8_t* lcdWs2812Buffer(const LcdWs2812Handle& h) {
    auto* st = static_cast<LcdState*>(h.impl);
    return st ? st->buf : nullptr;
}

size_t lcdWs2812BufferCapacity(const LcdWs2812Handle& h) {
    auto* st = static_cast<LcdState*>(h.impl);
    return st ? st->cap : 0;
}

bool lcdWs2812Transmit(LcdWs2812Handle& h, size_t bytes) {
    auto* st = static_cast<LcdState*>(h.impl);
    if (!st || bytes == 0 || bytes > st->cap) return false;
    // lcd_cmd = -1: no command phase — the transfer is one continuous GDMA
    // data stream, gapless at the pclk rate from internal SRAM.
    return esp_lcd_panel_io_tx_color(st->io, -1, st->buf, bytes) == ESP_OK;
}

void lcdWs2812Wait(LcdWs2812Handle& h, uint32_t timeoutMs) {
    auto* st = static_cast<LcdState*>(h.impl);
    if (!st) return;
    // Finite timeout, same self-healing stance as rmtWs2812Wait: a timed-out
    // frame is dropped and the driver re-encodes the whole frame next tick.
    xSemaphoreTake(st->done, pdMS_TO_TICKS(timeoutMs));
}

void lcdWs2812Deinit(LcdWs2812Handle& h) {
    auto* st = static_cast<LcdState*>(h.impl);
    if (!st) return;
    destroyState(st);
    h.impl = nullptr;
}

// ---------------------------------------------------------------------------
// Loopback self-test: a private FULL-width bus (the i80 layer rejects NC data
// pins, so the driver's complete pin set is rebuilt) transmits the CALLER'S
// real frame — full size, real DMA descriptor chain, real latch pad — back to
// back like the render loop, while an RMT RX channel (rmtWs2812RxCapture with
// the DMA backend — transmitter-agnostic, reused from the RMT rig) captures
// the WHOLE frame off the jumpered rxGpio and verifies every bit. A short
// synthetic burst would miss exactly the failures a real frame hits
// (descriptor boundaries, sustained-rate stalls), so the test sends the
// genuine article.
// ---------------------------------------------------------------------------

// The capture + bit-verify half is shared with the Parlio loopback in
// detail::captureAndVerifyFrame (platform_esp32_rmt.cpp); only the i80 transmit
// differs. Declared here so this TU can call it (same pattern as loopbackJumperOk).
namespace detail {
void captureAndVerifyFrame(uint16_t rxGpio, size_t frameBytes, size_t dataBytes,
                           uint8_t rowBits, uint32_t pclkHz, const char* tag,
                           const std::function<void()>& transmitOnce,
                           RmtLoopbackResult& r);
}

RmtLoopbackResult lcdWs2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                    uint16_t wrGpio, uint16_t dcGpio, uint16_t rxGpio,
                                    const uint8_t* frame, size_t frameBytes,
                                    size_t dataBytes, uint8_t rowBits) {
    RmtLoopbackResult r;
    r.sent[0] = 0xA5; r.sent[1] = 0x00; r.sent[2] = 0xFF;  // pattern in every row
    if (!dataPins || laneCount == 0 || !frame || frameBytes == 0
        || dataBytes < 3 || dataBytes > frameBytes || rowBits < 8) return r;
    const uint16_t txGpio = dataPins[0];   // lane 0 carries the pattern

    r.jumperDetected = detail::loopbackJumperOk(static_cast<uint8_t>(txGpio),
                                                static_cast<uint8_t>(rxGpio));
    if (!r.jumperDetected) return r;

    // The continuity check above reset txGpio's GPIO matrix route; bus
    // creation re-claims it.
    LcdState* st = createState(dataPins, laneCount, wrGpio, dcGpio, frameBytes);
    if (!st) {
        ESP_LOGE(LCD_TAG, "loopback: private bus creation failed");
        return r;
    }
    std::memcpy(st->buf, frame, frameBytes);

    // The i80-specific transmit: ship one frame and wait for its done-callback.
    // Everything else (capture, cadence, bit-verify) is the shared helper.
    auto transmitOnce = [st, frameBytes]() {
        // Loopback self-test path (not the render hot path): surface a failed
        // enqueue or a done-callback timeout instead of letting it show up only as
        // a later capture mismatch (same handling as the Parlio sibling).
        const esp_err_t err = esp_lcd_panel_io_tx_color(st->io, -1, st->buf, frameBytes);
        if (err != ESP_OK) {
            ESP_LOGE(LCD_TAG, "loopback: tx enqueue failed (%s)", esp_err_to_name(err));
            return;
        }
        if (xSemaphoreTake(st->done, pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(LCD_TAG, "loopback: tx done-callback timed out");
    };
    detail::captureAndVerifyFrame(rxGpio, frameBytes, dataBytes, rowBits, kPclkHz,
                                  LCD_TAG, transmitOnce, r);
    destroyState(st);
    return r;
}

} // namespace mm::platform

#else  // !SOC_LCDCAM_I80_LCD_SUPPORTED — inert stubs so classic ESP32 links

namespace mm::platform {

bool lcdWs2812Init(LcdWs2812Handle&, const uint16_t*, uint8_t, uint16_t, uint16_t,
                   size_t) {
    return false;
}
uint8_t* lcdWs2812Buffer(const LcdWs2812Handle&) { return nullptr; }
size_t lcdWs2812BufferCapacity(const LcdWs2812Handle&) { return 0; }
bool lcdWs2812Transmit(LcdWs2812Handle&, size_t) { return false; }
void lcdWs2812Wait(LcdWs2812Handle&, uint32_t) {}
void lcdWs2812Deinit(LcdWs2812Handle&) {}
RmtLoopbackResult lcdWs2812Loopback(const uint16_t*, uint8_t, uint16_t, uint16_t,
                                    uint16_t, const uint8_t*, size_t, size_t, uint8_t) {
    return {};
}

} // namespace mm::platform

#endif  // SOC_LCDCAM_I80_LCD_SUPPORTED
