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
#include "driver/gpio.h"        // gpio_num_t / GPIO_NUM_NC for the unit's pin map
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstring>
#include <functional>  // the transmit callback passed to the shared frame loopback
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
    cfg.shift_edge = PARLIO_SHIFT_EDGE_POS;   // v6.1 renamed sample_edge → shift_edge
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
    // (the same constraint as the LCD driver — platform::alloc prefers PSRAM and
    // is wrong here). Zeroed so the trailing latch pad holds lines LOW. Internal
    // for now even though the Parlio GDMA *can* burst from PSRAM (access_ext_mem):
    // moving big frames there to free scarce DRAM is a tracked follow-up (backlog
    // § LCD/Parlio DMA frame buffer → PSRAM), needing the wider ext-mem alignment
    // (not this fixed 64) + on-hardware proof, so not done here.
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

// loopbackJumperOk + captureAndVerifyFrame live in platform_esp32_rmt.cpp (the
// shared continuity check and the shared capture+bit-verify all three loopback
// rigs reuse); declared here so this TU can call them.
namespace detail {
bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio);
void captureAndVerifyFrame(uint16_t rxGpio, size_t frameBytes, size_t dataBytes,
                           uint8_t rowBits, uint32_t pclkHz, const char* tag,
                           const std::function<void()>& transmitOnce,
                           RmtLoopbackResult& r);
}

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

    // The Parlio-specific transmit: ship one frame (length in BITS, not bytes)
    // and wait for its done-callback. Everything else (capture, cadence, bit-
    // verify) is the shared helper.
    parlio_transmit_config_t xcfg = {};
    xcfg.idle_value = 0;   // lines rest LOW between frames (the latch)
    auto transmitOnce = [st, frameBytes, &xcfg]() {
        // Loopback self-test path (not the render hot path): a failed enqueue or a
        // done-callback timeout would otherwise be silent and just surface later as
        // a capture mismatch — log it so the real cause is visible in the verdict.
        const esp_err_t err = parlio_tx_unit_transmit(st->unit, st->buf,
                                                      frameBytes * 8, &xcfg);
        if (err != ESP_OK) {
            ESP_LOGE(PAR_TAG, "loopback: tx enqueue failed (%s)", esp_err_to_name(err));
            return;   // nothing to wait for
        }
        if (xSemaphoreTake(st->done, pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(PAR_TAG, "loopback: tx done-callback timed out");
    };
    detail::captureAndVerifyFrame(rxGpio, frameBytes, dataBytes, rowBits, kPclkHz,
                                  PAR_TAG, transmitOnce, r);
    destroyState(st);
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
