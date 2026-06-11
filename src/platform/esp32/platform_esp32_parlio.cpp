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
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

// Round 4 implements the loopback (Parlio RX unit, or RMT-RX capture of lane 0
// like the LCD/RMT rigs). Round 2 ships the stub so the driver's control
// surface compiles and reports "not implemented".
RmtLoopbackResult parlioWs2812Loopback(const uint16_t*, uint8_t, uint16_t,
                                       const uint8_t*, size_t, size_t, uint8_t) {
    return {};
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
