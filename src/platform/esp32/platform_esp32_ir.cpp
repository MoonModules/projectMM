// IR receive — the platform::irRead seam (declared in platform.h), decoding the NEC remote
// protocol off the RMT RX peripheral. A persistent RX channel runs on the IR pin; its done ISR
// only records how many symbols arrived and signals a queue (ISR-minimal: no decode, no driver
// call in interrupt context — the same discipline as rmtWs2812RxCapture). irRead(), on the render
// task, drains that signal non-blocking, decodes the captured symbols, and re-arms the channel.
// InfraredService is the sole caller.
//
// NEC protocol: a 9 ms lead mark + 4.5 ms space, then 32 bits LSB-first (address, ~address,
// command, ~command), each a 560 µs mark followed by a 560 µs space (0) or a 1690 µs space (1),
// then a final 560 µs stop mark. A repeat frame (9 ms mark + 2.25 ms space) is ignored — we
// surface distinct presses, not auto-repeat. Prior art: the ESP-IDF ir_nec_transceiver example;
// the timing is the published NEC standard.

#include "platform/platform.h"

#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "soc/soc_caps.h"   // SOC_RMT_MEM_WORDS_PER_CHANNEL

#include <cstdint>

namespace mm::platform {

namespace {

// NEC timings in µs (RX resolution is 1 µs — see kResolutionHz). ±30 % windows absorb drift.
constexpr uint32_t kLeadMark  = 9000;
constexpr uint32_t kLeadSpace = 4500;
constexpr uint32_t kBitMark   = 560;
constexpr uint32_t kZeroSpace = 560;
constexpr uint32_t kOneSpace  = 1690;
constexpr uint32_t kResolutionHz = 1000000;   // 1 tick = 1 µs

// A symbol duration falls within ±30 % of `target`. Task-context only (called from the decode in
// irRead, never the ISR), so no IRAM requirement.
inline bool nearUs(uint32_t d, uint32_t target) {
    const uint32_t tol = target * 3 / 10;
    return d + tol >= target && d <= target + tol;
}

// Decode NEC symbols → 32-bit code. True + code on a well-formed data frame; false on a repeat
// frame or malformed input. Runs on the render task (from irRead), not the ISR.
bool decodeNec(const rmt_symbol_word_t* sym, size_t n, uint32_t& out) {
    if (n < 34) return false;                                 // repeat (2 symbols) or truncated
    if (!nearUs(sym[0].duration0, kLeadMark)) return false;
    if (!nearUs(sym[0].duration1, kLeadSpace)) return false;  // 4.5 ms space = data (2.25 = repeat)
    uint32_t code = 0;
    for (int i = 0; i < 32; i++) {
        const rmt_symbol_word_t& s = sym[i + 1];
        if (!nearUs(s.duration0, kBitMark)) return false;
        if (nearUs(s.duration1, kOneSpace))        code |= (1u << i);   // LSB-first
        else if (!nearUs(s.duration1, kZeroSpace)) return false;        // neither 0 nor 1 → malformed
    }
    out = code;
    return true;
}

// Persistent channel + state. Opened lazily on the first irRead for a pin; reopened if the pin
// changes. One IR receiver per device, so a single static channel suffices.
rmt_channel_handle_t rxChan_ = nullptr;
QueueHandle_t doneQueue_ = nullptr;    // ISR → task: how many symbols the last frame captured
int currentPin_ = -1;
rmt_symbol_word_t rxBuf_[68];          // NEC = 34 symbols (lead + 32 bits + stop); slack for noise
rmt_receive_config_t rxCfg_ = {};

// ISR: record the symbol count and wake the task. No decode, no rmt_* re-arm here — the task
// re-arms after it has copied the buffer, so the DMA target is never overwritten mid-decode.
bool IRAM_ATTR rxDoneCb(rmt_channel_handle_t, const rmt_rx_done_event_data_t* edata, void*) {
    size_t n = edata->num_symbols;
    BaseType_t high = pdFALSE;
    xQueueSendFromISR(doneQueue_, &n, &high);
    return high == pdTRUE;
}

void closeChannel() {
    if (rxChan_) { rmt_disable(rxChan_); rmt_del_channel(rxChan_); rxChan_ = nullptr; }
    if (doneQueue_) { vQueueDelete(doneQueue_); doneQueue_ = nullptr; }
    currentPin_ = -1;
}

// Arm one receive into rxBuf_. Called from the task (initial arm + re-arm after each frame).
bool arm() { return rmt_receive(rxChan_, rxBuf_, sizeof(rxBuf_), &rxCfg_) == ESP_OK; }

// (Re)open the RX channel on `pin`. Idempotent for an unchanged pin. Returns true if live after.
bool ensureChannel(int pin) {
    if (rxChan_ && currentPin_ == pin) return true;
    closeChannel();
    if (pin < 0) return false;

    rmt_rx_channel_config_t cfg = {};
    cfg.gpio_num = static_cast<gpio_num_t>(pin);
    cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz = kResolutionHz;
    cfg.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;   // one hardware block is ample for NEC
    if (rmt_new_rx_channel(&cfg, &rxChan_) != ESP_OK) { rxChan_ = nullptr; return false; }

    doneQueue_ = xQueueCreate(1, sizeof(size_t));
    if (!doneQueue_) { rmt_del_channel(rxChan_); rxChan_ = nullptr; return false; }

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = rxDoneCb;
    rmt_rx_register_event_callbacks(rxChan_, &cbs, nullptr);

    // A pulse SHORTER than min_ns is filtered as a glitch; a pulse LONGER than max_ns ends the frame
    // (the inter-frame idle). The IDF ir_nec_transceiver reference uses 1250 ns / 12 ms; the previous
    // 200 ns min was too aggressive (sub-µs ringing on the receiver's rising edge registered as a real
    // pulse, so the RMT split the lead burst and completed the frame after ONE symbol — the debug
    // capture showed lastSyms=1, which decodeNec rejects). 1250 ns is well under a 560 µs NEC mark yet
    // above the receiver's edge ringing.
    rxCfg_.signal_range_min_ns = 1250;        // glitch filter — below a 560 µs NEC mark, above edge ring
    rxCfg_.signal_range_max_ns = 12000000;    // > the 9 ms lead — the inter-frame idle ends the frame
    if (rmt_enable(rxChan_) != ESP_OK || !arm()) {
        closeChannel();
        return false;
    }
    currentPin_ = pin;
    return true;
}

}  // namespace

bool irRead(uint16_t pin, uint32_t& codeOut) {
    if (!ensureChannel(static_cast<int>(pin))) return false;

    // Non-blocking: did the ISR signal a completed frame since the last call?
    size_t n = 0;
    if (xQueueReceive(doneQueue_, &n, 0) != pdTRUE) return false;

    // Decode the captured buffer (task context), then re-arm for the next frame. Re-arming here —
    // not in the ISR — guarantees the decode reads a stable buffer the next capture can't clobber.
    const bool ok = decodeNec(rxBuf_, n, codeOut);
    if (!arm()) {
        // Re-arm failed → the channel is enabled but not receiving, and ensureChannel() would
        // treat it as still-open (pin unchanged) and never recover it. Tear it down so the next
        // irRead reopens a fresh channel on this pin.
        closeChannel();
        return false;
    }
    return ok;
}

void irStop() { closeChannel(); }   // release the RX channel + its pin; irRead reopens it lazily

// Open-or-confirm the RX channel and report whether it's live — same lazy open irRead uses, exposed
// so InfraredService can tell "pin set" from "channel actually bound + armed". Fails when the RMT channel
// can't be created (a busy pin, a bad GPIO, no free RMT block).
bool irChannelReady(uint16_t pin) { return ensureChannel(static_cast<int>(pin)); }

}  // namespace mm::platform
