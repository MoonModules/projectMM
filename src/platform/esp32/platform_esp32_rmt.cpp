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
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "rom/ets_sys.h"   // ets_delay_us for the reset gap

#include <cstdlib>
#include <cstring>
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
    // while sending — the classic anti-glitch shape. 64 symbols/block is the
    // classic-ESP32 size; the copy encoder streams from our buffer regardless.
    txCfg.mem_block_symbols = 64;
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

void rmtWs2812Show(RmtWs2812Handle& h, const uint32_t* symbols, size_t symbolCount,
                   uint32_t resetUs) {
    auto* st = static_cast<RmtTxState*>(h.impl);
    if (!st || !symbols || symbolCount == 0) return;

    rmt_transmit_config_t txCfg = {};
    txCfg.loop_count = 0;   // single shot, no hardware loop

    // Our symbols are already rmt_symbol_word_t-shaped; the copy encoder takes a
    // byte size. Transmit, then block until the strand has fully clocked out.
    rmt_transmit(st->channel, st->encoder, symbols, symbolCount * sizeof(uint32_t), &txCfg);
    rmt_tx_wait_all_done(st->channel, -1 /* wait forever */);

    // Inter-frame reset: hold the line idle-LOW. invert_out (if set at init) is
    // applied by the peripheral, so a plain busy-wait with the channel idle is
    // the latch. ets_delay_us is fine for a few hundred us off any task context.
    if (resetUs) ets_delay_us(resetUs);
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
    // The RX channel's internal memory block must be even and >= 64 symbols
    // (IDF requirement). Round maxSymbols up to that floor; the actual capture
    // buffer (outSymbols / maxSymbols) is separate and can be smaller.
    size_t memBlock = static_cast<size_t>(maxSymbols);
    if (memBlock < 64) memBlock = 64;
    if (memBlock & 1) memBlock++;
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
    if (rmt_enable(rxChan) == ESP_OK
        && rmt_receive(rxChan, outSymbols, maxSymbols * sizeof(uint32_t), &rcfg) == ESP_OK) {
        RxDone d = {};
        if (xQueueReceive(q, &d, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
            got = d.numSymbols;
        }
        rmt_disable(rxChan);
    }

    vQueueDelete(q);
    rmt_del_channel(rxChan);
    return got;
}

} // namespace mm::platform
