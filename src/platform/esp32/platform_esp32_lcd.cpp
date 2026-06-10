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
// SOC_LCD_I80_SUPPORTED with inert stubs otherwise (classic ESP32 builds it
// too; the driver never calls in thanks to platform::lcdLanes == 0).

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_LCD_I80_SUPPORTED

#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstring>
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

    // The frame buffer must be DMA-capable internal RAM with the bus's
    // alignment — the esp_lcd helper handles both. Zeroed so the trailing
    // latch pad (and any unwritten tail) holds the lines LOW.
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

    // Capture at 40 MHz: a slot is 15 ticks, so "0" ≈ 15 and "1" ≈ 30
    // high ticks — threshold midway at 25. One symbol per WS2812 bit; the
    // frame's zeroed latch pad is the >100 µs idle that ends the capture.
    constexpr uint32_t kCapResHz = 40'000'000;
    const size_t kBits = dataBytes / 3;
    const size_t capMax = kBits + 16;
    auto* rxSymbols = static_cast<uint32_t*>(heap_caps_aligned_alloc(
        64, capMax * sizeof(uint32_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!rxSymbols) {
        ESP_LOGE(LCD_TAG, "loopback: capture buffer alloc failed (%u B)",
                 (unsigned)(capMax * sizeof(uint32_t)));
        destroyState(st);
        return r;
    }

    struct Cap {
        uint8_t rxGpio; uint32_t* buf; size_t max;
        volatile size_t got = 0; volatile bool done = false;
    } cap{static_cast<uint8_t>(rxGpio), rxSymbols, capMax};
    auto rxTask = [](void* arg) {
        auto* c = static_cast<Cap*>(arg);
        c->got = rmtWs2812RxCapture(c->rxGpio, kCapResHz, c->buf, c->max, 1000);
        c->done = true;
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(rxTask, "lcdlb", 4096, &cap, 5, nullptr) == pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        // First transmit timed — the wall time of a known byte count measures
        // the GRANTED pixel clock (esp_lcd doesn't expose it).
        {
            const int64_t t0 = esp_timer_get_time();
            esp_err_t err = esp_lcd_panel_io_tx_color(st->io, -1, st->buf, frameBytes);
            const bool ok = xSemaphoreTake(st->done, pdMS_TO_TICKS(1000)) == pdTRUE;
            const int64_t dt = esp_timer_get_time() - t0;
            ESP_LOGI(LCD_TAG, "loopback: tx_color err=%d done=%d %u bytes in %lld us "
                              "(expect ~%u us at %u Hz)",
                     (int)err, (int)ok, (unsigned)frameBytes, (long long)dt,
                     (unsigned)(frameBytes * 1000000ull / kPclkHz), (unsigned)kPclkHz);
        }
        // Back-to-back frames, exactly the render loop's transmit/wait cadence.
        for (int i = 0; i < 100 && !cap.done; i++) {
            esp_lcd_panel_io_tx_color(st->io, -1, st->buf, frameBytes);
            xSemaphoreTake(st->done, pdMS_TO_TICKS(1000));
        }
        for (int i = 0; i < 200 && !cap.done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(LCD_TAG, "loopback: rx captured %u symbols (need %u), idle rx level=%d",
             (unsigned)cap.got, (unsigned)kBits,
             gpio_get_level(static_cast<gpio_num_t>(rxGpio)));
    destroyState(st);

    if (cap.done && cap.got >= kBits) {
        // Verify EVERY bit of the frame against the per-row pattern (sent[],
        // zero-padded for RGBW rows), not just the first light.
        size_t mismatch = SIZE_MAX;
        uint16_t minH[2] = {0x7FFF, 0x7FFF}, maxH[2] = {0, 0};
        for (size_t b = 0; b < kBits; b++) {
            const uint16_t high = static_cast<uint16_t>(rxSymbols[b] & 0x7FFF);
            const uint8_t bit = (high >= 25) ? 1 : 0;
            if (high < minH[bit]) minH[bit] = high;
            if (high > maxH[bit]) maxH[bit] = high;
            const uint8_t rowPos = static_cast<uint8_t>(b % rowBits);
            const uint8_t expByte = (rowPos / 8u) < 3 ? r.sent[rowPos / 8u] : 0x00;
            const uint8_t exp = (expByte >> (7 - (rowPos & 7))) & 1u;
            if (bit != exp && mismatch == SIZE_MAX) mismatch = b;
        }
        // got[] reports the row holding the first mismatch (row 0 when clean).
        const size_t rowStart = (mismatch == SIZE_MAX)
                                    ? 0 : mismatch - (mismatch % rowBits);
        for (size_t b = rowStart; b < rowStart + 24 && b < cap.got; b++) {
            const uint8_t bit = ((rxSymbols[b] & 0x7FFF) >= 25) ? 1 : 0;
            r.got[(b - rowStart) / 8] =
                static_cast<uint8_t>((r.got[(b - rowStart) / 8] << 1) | bit);
        }
        r.pass = mismatch == SIZE_MAX;
        ESP_LOGI(LCD_TAG, "loopback: high ticks — 0-bits %u..%u, 1-bits %u..%u (25ns/tick)",
                 (unsigned)minH[0], (unsigned)maxH[0], (unsigned)minH[1], (unsigned)maxH[1]);
        if (!r.pass) {
            ESP_LOGE(LCD_TAG, "loopback: first bad bit %u (light %u, bit-in-row %u)",
                     (unsigned)mismatch, (unsigned)(mismatch / rowBits),
                     (unsigned)(mismatch % rowBits));
            for (size_t i = (mismatch > 3 ? mismatch - 3 : 0);
                 i < mismatch + 4 && i < cap.got; i++) {
                ESP_LOGE(LCD_TAG, "  sym[%u]: lvl0=%u dur0=%u  lvl1=%u dur1=%u",
                         (unsigned)i,
                         (unsigned)((rxSymbols[i] >> 15) & 1), (unsigned)(rxSymbols[i] & 0x7FFF),
                         (unsigned)((rxSymbols[i] >> 31) & 1), (unsigned)((rxSymbols[i] >> 16) & 0x7FFF));
            }
        }
    }
    heap_caps_free(rxSymbols);
    return r;
}

} // namespace mm::platform

#else  // !SOC_LCD_I80_SUPPORTED — inert stubs so classic ESP32 links

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

#endif  // SOC_LCD_I80_SUPPORTED
