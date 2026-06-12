// RMT WS2812 LED output — the peripheral half of the LED driver.
//
// The driver (src/light/drivers/RmtLedDriver.h) does all the domain work:
// applies Correction and encodes each pixel into RMT symbols. This file owns
// only the peripheral — channel setup, the copy-encoder that streams the
// pre-built symbols, transmit + wait, and the RX side the on-device loopback
// test uses. No domain logic here.
//
// Pre-encoded path: the driver hands us a flat array of WS2812 symbols already
// in rmt_symbol_word_t layout (our makeRmtSymbol() in RmtSymbol.h packs exactly
// that 32-bit format), so the TX path uses a *copy* encoder — it just DMAs the
// bytes out, no per-call symbol generation.

#include "platform/platform.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"
#include "driver/gpio.h"   // continuity pre-check in the loopback self-test
#include "soc/soc_caps.h"  // SOC_RMT_MEM_WORDS_PER_CHANNEL (64 classic, 48 S3)
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"   // ets_delay_us for the reset gap

#include "esp_heap_caps.h"   // capture buffer alloc for the shared frame loopback
#include "esp_timer.h"       // timed first transmit
#include "esp_log.h"

#include <cstdlib>
#include <cstring>
#include <functional>  // the transmit callback the shared frame loopback takes
#include <new>      // std::nothrow

namespace mm::platform {

namespace {

// Per-channel peripheral state, hidden behind RmtWs2812Handle::impl so the
// domain header never sees an ESP type. One TX channel + the copy encoder it
// streams symbols through, both allocated once at init.
struct RmtTxState {
    rmt_channel_handle_t channel = nullptr;
    rmt_encoder_handle_t encoder = nullptr;
    uint32_t resolutionHz = 0;
};

} // namespace

bool rmtWs2812Init(RmtWs2812Handle& h, uint8_t gpio, uint32_t resolutionHz, bool invert) {
    auto* st = new (std::nothrow) RmtTxState();
    if (!st) return false;

    rmt_tx_channel_config_t txCfg = {};
    txCfg.gpio_num = static_cast<gpio_num_t>(gpio);
    txCfg.clk_src = RMT_CLK_SRC_DEFAULT;
    txCfg.resolution_hz = resolutionHz;
    // Two memory blocks of symbols ping-pong so the DMA-less channel can refill
    // while sending — the classic anti-glitch shape. The per-channel block size
    // is a chip fact (64 words classic, 48 on the S3 — a hardcoded 64 makes
    // rmt_new_tx_channel reject S3); the copy encoder streams from our buffer
    // regardless.
    txCfg.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
    txCfg.trans_queue_depth = 4;
    txCfg.flags.invert_out = invert ? 1 : 0;

    if (rmt_new_tx_channel(&txCfg, &st->channel) != ESP_OK) {
        delete st;
        return false;
    }

    rmt_copy_encoder_config_t copyCfg = {};
    if (rmt_new_copy_encoder(&copyCfg, &st->encoder) != ESP_OK) {
        rmt_del_channel(st->channel);
        delete st;
        return false;
    }

    if (rmt_enable(st->channel) != ESP_OK) {
        rmt_del_encoder(st->encoder);
        rmt_del_channel(st->channel);
        delete st;
        return false;
    }

    st->resolutionHz = resolutionHz;
    h.impl = st;
    return true;
}

uint32_t rmtWs2812Resolution(const RmtWs2812Handle& h) {
    auto* st = static_cast<RmtTxState*>(h.impl);
    return st ? st->resolutionHz : 0;
}

bool rmtWs2812Transmit(RmtWs2812Handle& h, const uint32_t* symbols, size_t symbolCount) {
    auto* st = static_cast<RmtTxState*>(h.impl);
    if (!st || !symbols || symbolCount == 0) return false;

    rmt_transmit_config_t txCfg = {};
    txCfg.loop_count = 0;   // single shot, no hardware loop

    // Our symbols are already rmt_symbol_word_t-shaped; the copy encoder takes a
    // byte size. This only *starts* the transfer — channels started back-to-back
    // clock out concurrently, which is what makes a multi-pin frame cost the
    // longest strand instead of the sum. The caller pairs this with
    // rmtWs2812Wait and owns the inter-frame latch after the last wait.
    return rmt_transmit(st->channel, st->encoder, symbols,
                        symbolCount * sizeof(uint32_t), &txCfg) == ESP_OK;
}

void rmtWs2812Wait(RmtWs2812Handle& h, uint32_t timeoutMs) {
    auto* st = static_cast<RmtTxState*>(h.impl);
    if (!st) return;
    // Finite timeout so a wedged DMA can't hang the render tick forever. Even the
    // longest realistic frame (thousands of pixels) clocks out well under 1 s; a
    // timeout here means the peripheral is stuck, and the driver re-encodes the
    // whole frame next tick anyway, so a dropped frame self-heals.
    //
    // We deliberately do NOT cancel a timed-out transfer with rmt_disable(): on
    // classic ESP32, rmt_disable() while a transmission is still active triggers an
    // interrupt-WDT panic (espressif/esp-idf#17692, classic-only — S3/C6/P4 are
    // unaffected). A panic is a worse failure than a dropped frame, so we leave the
    // stuck transfer alone. It self-heals safely: the next tick re-encodes symbols_
    // and calls rmt_transmit again; if the channel is still busy, rmt_transmit
    // returns an error, rmtWs2812Transmit returns false, and RmtLedDriver::loop()
    // skips waiting on that channel (its started[] guard) — no crash, no corruption.
    rmt_tx_wait_all_done(st->channel, timeoutMs);
}

void rmtWs2812Deinit(RmtWs2812Handle& h) {
    auto* st = static_cast<RmtTxState*>(h.impl);
    if (!st) return;
    if (st->channel) {
        rmt_disable(st->channel);
        rmt_del_channel(st->channel);
    }
    if (st->encoder) rmt_del_encoder(st->encoder);
    delete st;
    h.impl = nullptr;
}

// ---------------------------------------------------------------------------
// RX loopback capture — on-device test only. Opens a one-shot RX channel on the
// jumpered pin, captures raw pulse symbols, returns how many landed. The test
// decodes those symbols back to bytes and asserts == sent.
// ---------------------------------------------------------------------------

namespace {

// done-callback hands the received symbol count to the waiting capture call via
// a 1-deep queue. IRAM so it survives a cache-disabled window.
struct RxDone { size_t numSymbols; };

bool IRAM_ATTR rmtRxDoneCb(rmt_channel_handle_t, const rmt_rx_done_event_data_t* edata,
                           void* user) {
    QueueHandle_t q = static_cast<QueueHandle_t>(user);
    RxDone d = { edata->num_symbols };
    BaseType_t high = pdFALSE;
    xQueueSendFromISR(q, &d, &high);
    return high == pdTRUE;
}

} // namespace

size_t rmtWs2812RxCapture(uint8_t gpio, uint32_t resolutionHz,
                          uint32_t* outSymbols, size_t maxSymbols, uint32_t timeoutMs) {
    if (!outSymbols || maxSymbols == 0) return 0;

    rmt_rx_channel_config_t rxCfg = {};
    rxCfg.gpio_num = static_cast<gpio_num_t>(gpio);
    rxCfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rxCfg.resolution_hz = resolutionHz;
    // The RX channel's internal memory block must be even and >= one hardware
    // block (IDF requirement; 64 words classic, 48 on the S3 — a hardcoded 64
    // would silently claim part of a second S3 channel's memory). Round
    // maxSymbols up to that floor; the actual capture buffer (outSymbols /
    // maxSymbols) is separate and can be smaller.
    size_t memBlock = static_cast<size_t>(maxSymbols);
    if (memBlock < SOC_RMT_MEM_WORDS_PER_CHANNEL) memBlock = SOC_RMT_MEM_WORDS_PER_CHANNEL;
    if (memBlock & 1) memBlock++;
#if SOC_RMT_SUPPORT_DMA
    // A capture larger than one hardware block (whole-frame captures, e.g. the
    // LCD loopback's full-frame check) uses the DMA backend, which can stream
    // an arbitrarily large mem_block. Caller's buffer must then be DMA-capable
    // internal RAM.
    rxCfg.flags.with_dma = maxSymbols > SOC_RMT_MEM_WORDS_PER_CHANNEL;
#else
    // No RMT DMA (classic ESP32): mem_block_symbols larger than one hardware
    // channel silently claims neighbouring channels' memory and fails to
    // allocate ("no free rx channels"). Cap to a single channel — the caller
    // gets at most one channel's worth of symbols per capture. A whole-frame
    // check on such a chip must therefore use a frame that fits one channel
    // (the frame loopback sizes itself to maxLaneLights accordingly).
    if (memBlock > SOC_RMT_MEM_WORDS_PER_CHANNEL)
        memBlock = SOC_RMT_MEM_WORDS_PER_CHANNEL;
#endif
    rxCfg.mem_block_symbols = memBlock;

    rmt_channel_handle_t rxChan = nullptr;
    if (rmt_new_rx_channel(&rxCfg, &rxChan) != ESP_OK) return 0;

    QueueHandle_t q = xQueueCreate(1, sizeof(RxDone));
    if (!q) { rmt_del_channel(rxChan); return 0; }

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = rmtRxDoneCb;
    rmt_rx_register_event_callbacks(rxChan, &cbs, q);

    // Accept WS2812 pulse widths: anything from a fraction of T0H up to well past
    // a bit cell, so glitches are filtered but real 0/1 pulses pass.
    rmt_receive_config_t rcfg = {};
    rcfg.signal_range_min_ns = 100;       // shorter than any real WS2812 edge
    rcfg.signal_range_max_ns = 100000;    // longer than a bit cell; ends the frame

    size_t got = 0;
    if (rmt_enable(rxChan) == ESP_OK) {
        // Once enabled, the channel must be disabled before delete — even if
        // rmt_receive or the wait fails — or rmt_del_channel rejects it.
        if (rmt_receive(rxChan, outSymbols, maxSymbols * sizeof(uint32_t), &rcfg) == ESP_OK) {
            RxDone d = {};
            if (xQueueReceive(q, &d, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
                got = d.numSymbols;
            }
        }
        rmt_disable(rxChan);
    }

    vQueueDelete(q);
    rmt_del_channel(rxChan);
    return got;
}

// ---------------------------------------------------------------------------
// Loopback self-test (runnable from the live firmware via RmtLedDriver's
// loopbackTest control). TX a known WS2812 pattern on txGpio, capture it back on
// rxGpio (user jumpers them), decode, compare. The WS2812 symbol build is inlined
// here (trivial — two symbol shapes) so the platform stays self-contained and
// src/light/ keeps no platform dependency.
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kLoopbackResHz = 40'000'000;  // 25 ns/tick, same as the driver
constexpr uint16_t kT0H = 14, kT1H = 28, kPeriod = 50;  // 350/700/1250 ns in ticks

} // namespace

namespace detail {

// Plain-GPIO continuity check: drive tx, read rx. Separates "wire wrong" from
// "RMT/LCD wrong" so a failed jumper is reported clearly. Shared with the LCD
// loopback in platform_esp32_lcd.cpp (declared there), hence not anonymous.
bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio) {
    gpio_set_direction(static_cast<gpio_num_t>(txGpio), GPIO_MODE_OUTPUT);
    gpio_set_direction(static_cast<gpio_num_t>(rxGpio), GPIO_MODE_INPUT);
    gpio_set_pull_mode(static_cast<gpio_num_t>(rxGpio), GPIO_PULLDOWN_ONLY);
    gpio_set_level(static_cast<gpio_num_t>(txGpio), 1);
    ets_delay_us(2000);
    int hi = gpio_get_level(static_cast<gpio_num_t>(rxGpio));
    gpio_set_level(static_cast<gpio_num_t>(txGpio), 0);
    ets_delay_us(2000);
    int lo = gpio_get_level(static_cast<gpio_num_t>(rxGpio));
    gpio_reset_pin(static_cast<gpio_num_t>(txGpio));
    gpio_reset_pin(static_cast<gpio_num_t>(rxGpio));
    return hi == 1 && lo == 0;
}

// Shared frame-capture + bit-verify for the two parallel LED loopbacks (LCD_CAM
// i80 and Parlio). They differ only in the transmit call (esp_lcd_panel_io_tx_color
// vs parlio_tx_unit_transmit) and the private-bus state type; everything else —
// the capture buffer, the RX task, the timed-first/back-to-back transmit cadence,
// and the whole per-bit verification — was byte-for-byte identical, so it lives
// here once. The caller has already done the jumper pre-check and built its
// private TX bus on the data pins; it passes `transmitOnce` (transmit the frame
// AND wait for its done-callback) and the params needed to size the capture and
// log the granted clock. `r` is filled in place (jumperDetected already set).
void captureAndVerifyFrame(uint16_t rxGpio, size_t frameBytes, size_t dataBytes,
                           uint8_t rowBits, uint32_t pclkHz, const char* tag,
                           const std::function<void()>& transmitOnce,
                           RmtLoopbackResult& r) {
    // Capture at 40 MHz: a slot is 15 ticks, so "0" ≈ 15 and "1" ≈ 30 high ticks
    // — threshold midway at 25. One symbol per WS2812 bit; the frame's zeroed
    // latch pad is the >100 µs idle that ends the capture.
    constexpr uint32_t kCapResHz = 40'000'000;
    const size_t kBits = dataBytes / 3;
    const size_t capMax = kBits + 16;
    auto* rxSymbols = static_cast<uint32_t*>(heap_caps_aligned_alloc(
        64, capMax * sizeof(uint32_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!rxSymbols) {
        ESP_LOGE(tag, "loopback: capture buffer alloc failed (%u B)",
                 (unsigned)(capMax * sizeof(uint32_t)));
        return;
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
    if (xTaskCreate(rxTask, "lblb", 4096, &cap, 5, nullptr) == pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        // First transmit timed — the wall time of a known byte count confirms the
        // granted pixel clock matches the configured slot rate (the bus driver
        // doesn't expose the granted clock directly).
        {
            const int64_t t0 = esp_timer_get_time();
            transmitOnce();
            const int64_t dt = esp_timer_get_time() - t0;
            ESP_LOGI(tag, "loopback: %u bytes in %lld us (expect ~%u us at %u Hz)",
                     (unsigned)frameBytes, (long long)dt,
                     (unsigned)(frameBytes * 1000000ull / pclkHz), (unsigned)pclkHz);
        }
        // Back-to-back frames, exactly the render loop's transmit/wait cadence.
        for (int i = 0; i < 100 && !cap.done; i++) transmitOnce();
        for (int i = 0; i < 200 && !cap.done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(tag, "loopback: rx captured %u symbols (need %u), idle rx level=%d",
             (unsigned)cap.got, (unsigned)kBits,
             gpio_get_level(static_cast<gpio_num_t>(rxGpio)));

    if (cap.done && cap.got >= kBits) {
        // Verify EVERY bit of the frame against the per-row pattern (r.sent[],
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
        // r.got[] reports the row holding the first mismatch (row 0 when clean).
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
        ESP_LOGI(tag, "loopback: high ticks — 0-bits %u..%u, 1-bits %u..%u (25ns/tick)",
                 (unsigned)minH[0], (unsigned)maxH[0], (unsigned)minH[1], (unsigned)maxH[1]);
        if (!r.pass) {
            ESP_LOGE(tag, "loopback: first bad bit %u (light %u, bit-in-row %u)",
                     (unsigned)mismatch, (unsigned)(mismatch / rowBits),
                     (unsigned)(mismatch % rowBits));
        }
    }
    heap_caps_free(rxSymbols);
}

} // namespace detail

RmtLoopbackResult rmtWs2812Loopback(uint8_t txGpio, uint8_t rxGpio) {
    RmtLoopbackResult r;
    r.sent[0] = 0xA5; r.sent[1] = 0x00; r.sent[2] = 0xFF;  // recognisable pattern

    r.jumperDetected = detail::loopbackJumperOk(txGpio, rxGpio);
    if (!r.jumperDetected) return r;   // no point running RMT through a dead wire

    // Build 24 symbols (3 bytes × 8 bits, MSB-first) for the pattern.
    const uint32_t sym0 = static_cast<uint32_t>(kT0H) | (1u << 15)
                        | (static_cast<uint32_t>(kPeriod - kT0H) << 16);
    const uint32_t sym1 = static_cast<uint32_t>(kT1H) | (1u << 15)
                        | (static_cast<uint32_t>(kPeriod - kT1H) << 16);
    constexpr size_t kBits = 24;
    uint32_t txSymbols[kBits];
    size_t s = 0;
    for (int b = 0; b < 3; b++)
        for (int bit = 7; bit >= 0; bit--)
            txSymbols[s++] = (r.sent[b] & (1u << bit)) ? sym1 : sym0;

    RmtWs2812Handle tx;
    if (!rmtWs2812Init(tx, txGpio, kLoopbackResHz, /*invert=*/false)) return r;

    // RX must be listening while we transmit; run the (blocking) capture in a task
    // and resend the short frame until the receiver latches one or we give up.
    constexpr size_t kCapMax = kBits + 8;
    static uint32_t rxSymbols[kCapMax];
    // Pass rxGpio through the arg struct (the task fn is a plain C pointer — no captures).
    struct Cap { uint8_t rxGpio; volatile size_t got = 0; volatile bool done = false; } cap{rxGpio};
    auto rxTask = [](void* arg) {
        auto* c = static_cast<Cap*>(arg);
        c->got = rmtWs2812RxCapture(c->rxGpio, kLoopbackResHz, rxSymbols, kCapMax, 1000);
        c->done = true;
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(rxTask, "rmtlb", 4096, &cap, 5, nullptr) == pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        for (int i = 0; i < 50 && !cap.done; i++) {
            rmtWs2812Transmit(tx, txSymbols, kBits);
            rmtWs2812Wait(tx, 1000);
            ets_delay_us(300);   // inter-frame latch
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        for (int i = 0; i < 200 && !cap.done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    rmtWs2812Deinit(tx);

    if (cap.done && cap.got >= kBits) {
        // Decode the first 24 captured symbols → bytes (HIGH closer to T1H = 1).
        for (size_t b = 0; b < kBits; b++) {
            uint16_t high = static_cast<uint16_t>(rxSymbols[b] & 0x7FFF);
            uint8_t bit = (high >= ((kT0H + kT1H) / 2)) ? 1 : 0;
            r.got[b / 8] = static_cast<uint8_t>((r.got[b / 8] << 1) | bit);
        }
        r.pass = (r.got[0] == r.sent[0] && r.got[1] == r.sent[1] && r.got[2] == r.sent[2]);
    }
    r.bitsChecked = static_cast<uint32_t>(kBits);
    r.firstBadBit = r.pass ? static_cast<uint32_t>(kBits) : 0;
    return r;
}

// Whole-frame variant: transmit a real `lights`-light frame back to back and
// bit-verify the WHOLE capture. The per-light pattern is 0xA5/0x00/0xFF (the
// sent[] bytes), zero-padded for any 4th (white) channel, repeated for every
// light. Unlike the 24-bit burst above, this drives the sustained DMA path and
// a long wire under whatever RF the device is doing — so it catches the
// frame-rate corruption and interference the short test is blind to.
RmtLoopbackResult rmtWs2812LoopbackFrame(uint8_t txGpio, uint8_t rxGpio,
                                         uint16_t lights, uint8_t channels) {
    RmtLoopbackResult r;
    r.sent[0] = 0xA5; r.sent[1] = 0x00; r.sent[2] = 0xFF;
    if (lights == 0 || channels < 3 || channels > 4) return r;

    r.jumperDetected = detail::loopbackJumperOk(txGpio, rxGpio);
    if (!r.jumperDetected) return r;

    const uint32_t sym0 = static_cast<uint32_t>(kT0H) | (1u << 15)
                        | (static_cast<uint32_t>(kPeriod - kT0H) << 16);
    const uint32_t sym1 = static_cast<uint32_t>(kT1H) | (1u << 15)
                        | (static_cast<uint32_t>(kPeriod - kT1H) << 16);
    const uint8_t bitsPerLight = static_cast<uint8_t>(channels * 8);
#if !SOC_RMT_SUPPORT_DMA
    // No RMT DMA (classic ESP32): the RX capture can hold at most one hardware
    // channel's symbols, so cap the verified frame to what fits whole lights in
    // that block. The frame is still transmitted back to back (the sustained-
    // output stress that exposes RF interference); we just verify a prefix that
    // the no-DMA receiver can actually capture.
    const uint16_t maxLights =
        static_cast<uint16_t>(SOC_RMT_MEM_WORDS_PER_CHANNEL / bitsPerLight);
    if (lights > maxLights) lights = maxLights ? maxLights : 1;
#endif
    const size_t kBits = static_cast<size_t>(lights) * bitsPerLight;

    // One real frame's worth of symbols, DMA-capable internal RAM (the same
    // place the driver's own symbol buffer lives). Off the hot path — this is
    // a control-driven self-test.
    auto* txSymbols = static_cast<uint32_t*>(heap_caps_malloc(
        kBits * sizeof(uint32_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    const size_t capMax = kBits + 16;
    auto* rxSymbols = static_cast<uint32_t*>(heap_caps_aligned_alloc(
        64, capMax * sizeof(uint32_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!txSymbols || !rxSymbols) {
        heap_caps_free(txSymbols);
        heap_caps_free(rxSymbols);
        return r;
    }
    size_t s = 0;
    for (uint16_t light = 0; light < lights; light++)
        for (uint8_t ch = 0; ch < channels; ch++) {
            const uint8_t byte = ch < 3 ? r.sent[ch] : 0x00;
            for (int bit = 7; bit >= 0; bit--)
                txSymbols[s++] = (byte & (1u << bit)) ? sym1 : sym0;
        }

    RmtWs2812Handle tx;
    if (!rmtWs2812Init(tx, txGpio, kLoopbackResHz, /*invert=*/false)) {
        heap_caps_free(txSymbols);
        heap_caps_free(rxSymbols);
        return r;
    }

    struct Cap {
        uint8_t rxGpio; uint32_t* buf; size_t max;
        volatile size_t got = 0; volatile bool done = false;
    } cap{rxGpio, rxSymbols, capMax};
    auto rxTask = [](void* arg) {
        auto* c = static_cast<Cap*>(arg);
        c->got = rmtWs2812RxCapture(c->rxGpio, kLoopbackResHz, c->buf, c->max, 1000);
        c->done = true;
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(rxTask, "rmtlbf", 4096, &cap, 5, nullptr) == pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        // Back-to-back frames, the render loop's cadence. The capture latches
        // one whole frame; we keep resending so it can't miss the window.
        for (int i = 0; i < 100 && !cap.done; i++) {
            rmtWs2812Transmit(tx, txSymbols, kBits);
            rmtWs2812Wait(tx, 1000);
            ets_delay_us(300);   // inter-frame WS2812 latch
        }
        for (int i = 0; i < 200 && !cap.done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    rmtWs2812Deinit(tx);

    if (cap.done && cap.got >= kBits) {
        size_t mismatch = SIZE_MAX;
        for (size_t b = 0; b < kBits; b++) {
            const uint16_t high = static_cast<uint16_t>(rxSymbols[b] & 0x7FFF);
            const uint8_t bit = (high >= ((kT0H + kT1H) / 2)) ? 1 : 0;
            const uint8_t pos = static_cast<uint8_t>(b % bitsPerLight);
            const uint8_t expByte = (pos / 8u) < 3 ? r.sent[pos / 8u] : 0x00;
            const uint8_t exp = (expByte >> (7 - (pos & 7))) & 1u;
            if (bit != exp && mismatch == SIZE_MAX) mismatch = b;
        }
        r.pass = (mismatch == SIZE_MAX);
        r.bitsChecked = static_cast<uint32_t>(kBits);
        r.firstBadBit = (mismatch == SIZE_MAX) ? static_cast<uint32_t>(kBits)
                                               : static_cast<uint32_t>(mismatch);
        // got[] = the light holding the first mismatch (light 0 when clean).
        const size_t badLight = (mismatch == SIZE_MAX) ? 0 : mismatch / bitsPerLight;
        const size_t lightStart = badLight * bitsPerLight;
        for (size_t b = 0; b < 24 && lightStart + b < cap.got; b++) {
            const uint8_t bit = ((rxSymbols[lightStart + b] & 0x7FFF)
                                 >= ((kT0H + kT1H) / 2)) ? 1 : 0;
            r.got[b / 8] = static_cast<uint8_t>((r.got[b / 8] << 1) | bit);
        }
    }
    heap_caps_free(txSymbols);
    heap_caps_free(rxSymbols);
    return r;
}

} // namespace mm::platform
