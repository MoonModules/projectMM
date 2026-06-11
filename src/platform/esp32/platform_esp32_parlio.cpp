// Parlio (Parallel IO) WS2812 output — the peripheral half of the Parlio LED
// driver (ESP32-P4). The driver (src/light/drivers/ParlioLedDriver.h) does all
// the domain work: applies Correction and 3-slot-encodes every light into the
// DMA frame buffer (LcdSlots.h, the SAME encoder the LCD_CAM driver uses — one
// bus byte per slot, bit L = data line L). This file owns only the peripheral:
// the Parlio TX unit, the DMA frame buffer, transmit + wait. No domain logic.
//
// Design mirrors the LCD_CAM driver: the whole frame is pre-encoded into ONE
// buffer and sent as ONE autonomous DMA transfer (single-shot, NOT Parlio's
// loop-transmission mode) — once started no CPU work remains until the done
// callback, so there is no refill deadline for WiFi to miss.
//
// Simpler than i80: Parlio takes the data GPIOs directly (no sacrificial WR/DC
// lines — it generates the pixel clock internally) and the bus is always 8
// lanes wide to match the encoder's 8-bit bus byte; lanes the driver doesn't
// use get GPIO -1 so they're simply not driven (no all-pins-required rule).
//
// Compiles on every ESP32 chip: everything is under SOC_PARLIO_SUPPORTED with
// inert stubs otherwise; the driver never calls in (platform::parlioLanes == 0).

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_PARLIO_SUPPORTED

#include "driver/parlio_tx.h"
#include "driver/gpio.h"        // gpio_get_level for the post-capture idle log
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"          // esp_timer_get_time for the timed first transmit
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"      // xTaskCreate / vTaskDelay for the RX task

#include <cstring>
#include <new>      // std::nothrow

namespace mm::platform {

namespace {

static const char* PAR_TAG = "mm_parlio";

// The Parlio bus is always 8 data lines wide so each encoded bus byte maps
// byte→lane directly (the LcdSlots.h encoder writes 8-bit words, bit L = lane
// L). data_width must be a power of two ≤ SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH;
// 8 satisfies that. Unused lanes get GPIO -1.
constexpr size_t kBusWidth = 8;

// WS2812 slot rate (375 ns @ 2.67 MHz), same value the driver passes at init —
// the loopback creates its own private unit and needs the constant directly.
constexpr uint32_t kPclkHz = 2'666'666;

struct ParlioState {
    parlio_tx_unit_handle_t unit = nullptr;
    SemaphoreHandle_t done = nullptr;
    uint8_t* buf = nullptr;
    size_t cap = 0;
};

// Done-callback: the DMA transfer finished — release the waiter.
bool IRAM_ATTR parlioDoneCb(parlio_tx_unit_handle_t, const parlio_tx_done_event_data_t*,
                            void* user) {
    auto* st = static_cast<ParlioState*>(user);
    BaseType_t high = pdFALSE;
    xSemaphoreGiveFromISR(st->done, &high);
    return high == pdTRUE;
}

void destroyState(ParlioState* st) {
    if (!st) return;
    if (st->unit) {
        parlio_tx_unit_disable(st->unit);
        parlio_del_tx_unit(st->unit);
    }
    if (st->buf) heap_caps_free(st->buf);
    if (st->done) vSemaphoreDelete(st->done);
    delete st;
}

// One TX unit + zeroed DMA buffer. pclkHz is the WS2812 slot rate (2.67 MHz).
ParlioState* createState(const uint16_t* dataPins, uint8_t laneCount,
                         uint32_t pclkHz, size_t bufferBytes) {
    auto* st = new (std::nothrow) ParlioState();
    if (!st) return nullptr;

    parlio_tx_unit_config_t cfg = {};
    cfg.clk_src = PARLIO_CLK_SRC_DEFAULT;     // PLL_F160M → /60 = 2.67 MHz
    cfg.clk_in_gpio_num = GPIO_NUM_NC;        // internal clock, not external
    cfg.output_clk_freq_hz = pclkHz;
    cfg.data_width = kBusWidth;
    for (size_t i = 0; i < kBusWidth; i++) cfg.data_gpio_nums[i] = GPIO_NUM_NC;
    for (uint8_t i = 0; i < laneCount && i < kBusWidth; i++)
        cfg.data_gpio_nums[i] = static_cast<gpio_num_t>(dataPins[i]);
    cfg.clk_out_gpio_num = GPIO_NUM_NC;       // WS2812 ignores the clock line
    cfg.valid_gpio_num = GPIO_NUM_NC;
    cfg.trans_queue_depth = 1;                // single full-frame transfer
    cfg.max_transfer_size = bufferBytes;
    cfg.dma_burst_size = 64;
    cfg.sample_edge = PARLIO_SAMPLE_EDGE_POS;
    cfg.bit_pack_order = PARLIO_BIT_PACK_ORDER_MSB;
    if (parlio_new_tx_unit(&cfg, &st->unit) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }

    st->done = xSemaphoreCreateBinary();
    if (!st->done) { destroyState(st); return nullptr; }
    parlio_tx_event_callbacks_t cbs = {};
    cbs.on_trans_done = parlioDoneCb;
    if (parlio_tx_unit_register_event_callbacks(st->unit, &cbs, st) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }
    if (parlio_tx_unit_enable(st->unit) != ESP_OK) { destroyState(st); return nullptr; }

    // DMA-capable INTERNAL RAM: Parlio streams from internal SRAM at full rate
    // (the same constraint as the LCD driver — platform::alloc prefers PSRAM
    // and is wrong here). Zeroed so the trailing latch pad holds lines LOW.
    st->buf = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        64, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!st->buf) { destroyState(st); return nullptr; }
    std::memset(st->buf, 0, bufferBytes);
    st->cap = bufferBytes;
    return st;
}

}  // namespace

bool parlioWs2812Init(ParlioWs2812Handle& h, const uint16_t* dataPins,
                      uint8_t laneCount, uint32_t pclkHz, size_t bufferBytes) {
    if (!dataPins || laneCount == 0 || bufferBytes == 0) return false;
    // Keep the platform memory reserve intact — degrade (init failure → driver
    // idles with a status error) rather than starve the system of internal RAM.
    if (heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
        < bufferBytes + HEAP_RESERVE) {
        return false;
    }
    ParlioState* st = createState(dataPins, laneCount, pclkHz, bufferBytes);
    if (!st) return false;
    h.impl = st;
    return true;
}

uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle& h) {
    auto* st = static_cast<ParlioState*>(h.impl);
    return st ? st->buf : nullptr;
}

size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle& h) {
    auto* st = static_cast<ParlioState*>(h.impl);
    return st ? st->cap : 0;
}

bool parlioWs2812Transmit(ParlioWs2812Handle& h, size_t bytes) {
    auto* st = static_cast<ParlioState*>(h.impl);
    if (!st || bytes == 0 || bytes > st->cap) return false;
    parlio_transmit_config_t xcfg = {};
    xcfg.idle_value = 0;   // lines rest LOW between/after the frame (the latch)
    // payload length is in BITS; the buffer is bytes × 8 lanes-worth of slots.
    return parlio_tx_unit_transmit(st->unit, st->buf, bytes * 8, &xcfg) == ESP_OK;
}

void parlioWs2812Wait(ParlioWs2812Handle& h, uint32_t timeoutMs) {
    auto* st = static_cast<ParlioState*>(h.impl);
    if (!st) return;
    // Finite timeout, self-healing: a timed-out frame is dropped and the whole
    // frame re-encoded next tick (same stance as rmt/lcd Wait).
    parlio_tx_unit_wait_all_done(st->unit, static_cast<int>(timeoutMs));
}

void parlioWs2812Deinit(ParlioWs2812Handle& h) {
    auto* st = static_cast<ParlioState*>(h.impl);
    if (!st) return;
    destroyState(st);
    h.impl = nullptr;
}

// ---------------------------------------------------------------------------
// Loopback self-test: a private Parlio TX unit on the driver's data pins
// transmits the CALLER'S real frame — full size, real DMA transfer, real latch
// pad — back to back like the render loop, while an RMT RX channel
// (rmtWs2812RxCapture with the DMA backend — transmitter-agnostic, reused from
// the RMT/LCD rigs) captures the WHOLE frame off the jumpered rxGpio and
// verifies every bit. This is the LCD loopback (platform_esp32_lcd.cpp) with
// the i80 transmit swapped for Parlio's: no WR/DC pins, and the payload goes
// out via parlio_tx_unit_transmit (length in BITS) instead of
// esp_lcd_panel_io_tx_color. The RX capture half is byte-for-byte identical —
// the wire signal is the same WS2812 the encoder produced for either bus.
// ---------------------------------------------------------------------------

// loopbackJumperOk lives in platform_esp32_rmt.cpp (defined there, declared in
// platform_esp32_lcd.cpp) — the same plain-GPIO continuity check all three
// loopback rigs share; declared here too so this TU can call it.
namespace detail { bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio); }

RmtLoopbackResult parlioWs2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                       uint16_t rxGpio, const uint8_t* frame,
                                       size_t frameBytes, size_t dataBytes,
                                       uint8_t rowBits) {
    RmtLoopbackResult r;
    r.sent[0] = 0xA5; r.sent[1] = 0x00; r.sent[2] = 0xFF;  // pattern in every row
    if (!dataPins || laneCount == 0 || !frame || frameBytes == 0
        || dataBytes < 3 || dataBytes > frameBytes || rowBits < 8) return r;
    const uint16_t txGpio = dataPins[0];   // lane 0 carries the pattern

    r.jumperDetected = detail::loopbackJumperOk(static_cast<uint8_t>(txGpio),
                                                static_cast<uint8_t>(rxGpio));
    if (!r.jumperDetected) return r;

    // The continuity check above reset txGpio's GPIO matrix route; the TX unit
    // creation below re-claims it.
    ParlioState* st = createState(dataPins, laneCount, kPclkHz, frameBytes);
    if (!st) {
        ESP_LOGE(PAR_TAG, "loopback: private TX unit creation failed");
        return r;
    }
    std::memcpy(st->buf, frame, frameBytes);

    // Capture at 40 MHz: a slot is 15 ticks, so "0" ≈ 15 and "1" ≈ 30 high
    // ticks — threshold midway at 25. One symbol per WS2812 bit; the frame's
    // zeroed latch pad is the >100 µs idle that ends the capture. (Same 2.67 MHz
    // slot rate as the LCD bus, so the identical thresholds apply.)
    constexpr uint32_t kCapResHz = 40'000'000;
    const size_t kBits = dataBytes / 3;
    const size_t capMax = kBits + 16;
    auto* rxSymbols = static_cast<uint32_t*>(heap_caps_aligned_alloc(
        64, capMax * sizeof(uint32_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!rxSymbols) {
        ESP_LOGE(PAR_TAG, "loopback: capture buffer alloc failed (%u B)",
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
    if (xTaskCreate(rxTask, "parlb", 4096, &cap, 5, nullptr) == pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        parlio_transmit_config_t xcfg = {};
        xcfg.idle_value = 0;   // lines rest LOW between frames (the latch)
        // First transmit timed — the wall time of a known byte count confirms
        // the granted pixel clock matches the configured 2.67 MHz slot rate.
        {
            const int64_t t0 = esp_timer_get_time();
            esp_err_t err = parlio_tx_unit_transmit(st->unit, st->buf, frameBytes * 8, &xcfg);
            const bool ok = xSemaphoreTake(st->done, pdMS_TO_TICKS(1000)) == pdTRUE;
            const int64_t dt = esp_timer_get_time() - t0;
            ESP_LOGI(PAR_TAG, "loopback: transmit err=%d done=%d %u bytes in %lld us "
                              "(expect ~%u us at %u Hz)",
                     (int)err, (int)ok, (unsigned)frameBytes, (long long)dt,
                     (unsigned)(frameBytes * 1000000ull / kPclkHz), (unsigned)kPclkHz);
        }
        // Back-to-back frames, exactly the render loop's transmit/wait cadence.
        for (int i = 0; i < 100 && !cap.done; i++) {
            parlio_tx_unit_transmit(st->unit, st->buf, frameBytes * 8, &xcfg);
            xSemaphoreTake(st->done, pdMS_TO_TICKS(1000));
        }
        for (int i = 0; i < 200 && !cap.done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(PAR_TAG, "loopback: rx captured %u symbols (need %u), idle rx level=%d",
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
        r.bitsChecked = static_cast<uint32_t>(kBits);
        r.firstBadBit = (mismatch == SIZE_MAX) ? static_cast<uint32_t>(kBits)
                                               : static_cast<uint32_t>(mismatch);
        ESP_LOGI(PAR_TAG, "loopback: high ticks — 0-bits %u..%u, 1-bits %u..%u (25ns/tick)",
                 (unsigned)minH[0], (unsigned)maxH[0], (unsigned)minH[1], (unsigned)maxH[1]);
        if (!r.pass) {
            ESP_LOGE(PAR_TAG, "loopback: first bad bit %u (light %u, bit-in-row %u)",
                     (unsigned)mismatch, (unsigned)(mismatch / rowBits),
                     (unsigned)(mismatch % rowBits));
        }
    }
    heap_caps_free(rxSymbols);
    return r;
}

}  // namespace mm::platform

#else  // !SOC_PARLIO_SUPPORTED — inert stubs so classic ESP32 / S3 link

namespace mm::platform {

bool parlioWs2812Init(ParlioWs2812Handle&, const uint16_t*, uint8_t, uint32_t, size_t) {
    return false;
}
uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle&) { return nullptr; }
size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle&) { return 0; }
bool parlioWs2812Transmit(ParlioWs2812Handle&, size_t) { return false; }
void parlioWs2812Wait(ParlioWs2812Handle&, uint32_t) {}
void parlioWs2812Deinit(ParlioWs2812Handle&) {}
RmtLoopbackResult parlioWs2812Loopback(const uint16_t*, uint8_t, uint16_t,
                                       const uint8_t*, size_t, size_t, uint8_t) {
    return {};
}

}  // namespace mm::platform

#endif  // SOC_PARLIO_SUPPORTED
