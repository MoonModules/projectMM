// On-device RMT WS2812 loopback test — the HAL-tier proof that a GPIO emits
// CORRECT WS2812 bytes on real silicon. The encoder is already proven in CI
// (unit_RmtLedEncoder.cpp); this proves the peripheral actually puts those bits
// on the wire, by capturing them back on-chip and decoding.
//
// The RMT peripheral is a transceiver: we TX a known byte pattern on TX_GPIO,
// the user jumpers TX_GPIO → RX_GPIO, and RMT-RX captures the pulse widths. We
// decode the captured pulses back to bytes and assert they equal what was sent.
// On-chip and independent of a strand — a $0 jumper, no logic analyzer.
//
// Runs in place of the normal firmware when built with -DMM_LED_LOOPBACK_TEST
// (see esp32/main/main.cpp). Prints a single PASS/FAIL line over serial. The
// sigrok independent-clock cross-check (different clock, real wire) is the next
// tier — this loopback trusts the MCU's own clock, sigrok crosses it.
//
//   WIRING: jumper GPIO 4 (TX) → GPIO 5 (RX). That's the whole rig.

#ifdef MM_LED_LOOPBACK_TEST

#include "platform/platform.h"
#include "light/drivers/RmtSymbol.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdint>

namespace {

constexpr uint8_t  TX_GPIO = 4;
constexpr uint8_t  RX_GPIO = 5;
constexpr uint32_t RES_HZ  = 40'000'000;   // 25 ns/tick — same as RmtLedDriver
// WS2812B timing in ticks at 25 ns/tick: t0h 350ns→14, t1h 700ns→28, period 1250ns→50.
constexpr uint16_t T0H = 14, T1H = 28, PERIOD = 50;

// A recognisable test pattern: 3 bytes (one RGB light), bits spanning 0s and 1s.
constexpr uint8_t  kPattern[3] = {0xA5, 0x00, 0xFF};
constexpr size_t   kBits = sizeof(kPattern) * 8;

// Decode one captured RMT symbol's HIGH duration to a bit: a HIGH pulse closer to
// T1H than T0H is a 1. Midpoint between 14 and 28 ticks is 21.
uint8_t bitFromHigh(uint16_t highTicks) {
    return (highTicks >= ((T0H + T1H) / 2)) ? 1 : 0;
}

} // namespace

// Called from app_main under MM_LED_LOOPBACK_TEST. Never returns to the firmware.
extern "C" void mm_led_loopback_test() {
    std::printf("\n[loopback] RMT WS2812 TX/RX test — jumper GPIO %d -> GPIO %d\n",
                TX_GPIO, RX_GPIO);

    // STEP 0 — jumper continuity check, BEFORE any RMT. Drive TX as a plain digital
    // output and read RX as a plain digital input: if the GPIO 4 -> GPIO 5 jumper
    // is connected, RX follows TX. Kept as a permanent first step so a wiring fault
    // is reported as "JUMPER NOT DETECTED" instead of being mistaken for an RMT bug
    // (it separates "is the wire right" from "is the RMT code right" — the exact
    // ambiguity that bit us bringing this test up).
    {
        gpio_set_direction(static_cast<gpio_num_t>(TX_GPIO), GPIO_MODE_OUTPUT);
        gpio_set_direction(static_cast<gpio_num_t>(RX_GPIO), GPIO_MODE_INPUT);
        gpio_set_pull_mode(static_cast<gpio_num_t>(RX_GPIO), GPIO_PULLDOWN_ONLY);
        gpio_set_level(static_cast<gpio_num_t>(TX_GPIO), 1);
        vTaskDelay(pdMS_TO_TICKS(2));
        int hi = gpio_get_level(static_cast<gpio_num_t>(RX_GPIO));
        gpio_set_level(static_cast<gpio_num_t>(TX_GPIO), 0);
        vTaskDelay(pdMS_TO_TICKS(2));
        int lo = gpio_get_level(static_cast<gpio_num_t>(RX_GPIO));
        std::printf("[wiring] drove GPIO %d high->RX read %d, low->RX read %d  => %s\n",
                    TX_GPIO, hi, lo,
                    (hi == 1 && lo == 0) ? "JUMPER OK" : "JUMPER NOT DETECTED");
        // Reset both pins so RMT can reconfigure them below.
        gpio_reset_pin(static_cast<gpio_num_t>(TX_GPIO));
        gpio_reset_pin(static_cast<gpio_num_t>(RX_GPIO));
    }

    // Encode the known pattern (one light, 3 channels) into symbols, exactly as
    // RmtLedDriver does — same encoder, so this also exercises the real encode path.
    uint32_t txSymbols[kBits];
    mm::encodeWs2812Symbols(kPattern, sizeof(kPattern), T0H, T1H, PERIOD, txSymbols);

    mm::platform::RmtWs2812Handle tx;
    if (!mm::platform::rmtWs2812Init(tx, TX_GPIO, RES_HZ, /*invert=*/false)) {
        std::printf("[loopback] FAIL: rmtWs2812Init(TX) failed\n");
        return;
    }

    // Capture buffer: kBits of data + a small margin. (rmtWs2812RxCapture rounds
    // the channel's internal mem block up to the IDF-required 64-even floor; this
    // is just the host-side result buffer.)
    constexpr size_t kCapMax = kBits + 8;
    static uint32_t rxSymbols[kCapMax];

    // Run the capture in its own task: rmtWs2812RxCapture blocks waiting for a
    // frame, so it must be listening while the main context transmits. The task
    // publishes its result through `cap`, which the main context polls.
    struct Cap { volatile size_t got = 0; volatile bool done = false; } cap;
    auto rxTask = [](void* arg) {
        auto* c = static_cast<Cap*>(arg);
        c->got = mm::platform::rmtWs2812RxCapture(RX_GPIO, RES_HZ, rxSymbols, kCapMax, 1000);
        c->done = true;
        vTaskDelete(nullptr);
    };
    xTaskCreate(rxTask, "rxcap", 4096, &cap, 5, nullptr);

    // Give the RX task time to create + enable its channel and reach rmt_receive,
    // then TX the frame REPEATEDLY until RX reports done. A single 24-bit burst is
    // only ~30 us, easy for the capture arming to miss by a scheduling hair — so
    // we resend (with the reset gap between) until the receiver latches one frame
    // or we give up. The driver's TX itself is already proven in the full firmware;
    // this loop just removes the one-shot timing race from the test rig.
    vTaskDelay(pdMS_TO_TICKS(50));   // let RX arm
    for (int i = 0; i < 50 && !cap.done; i++) {
        mm::platform::rmtWs2812Show(tx, txSymbols, kBits, /*resetUs=*/300);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Wait for the capture to complete (timeout inside RxCapture is 1s).
    for (int i = 0; i < 200 && !cap.done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    mm::platform::rmtWs2812Deinit(tx);

    if (!cap.done) { std::printf("[loopback] FAIL: capture never completed\n"); return; }
    if (cap.got < kBits) {
        std::printf("[loopback] FAIL: captured %u symbols, expected >= %u "
                    "(is the GPIO %d -> %d jumper connected?)\n",
                    (unsigned)cap.got, (unsigned)kBits, TX_GPIO, RX_GPIO);
        return;
    }

    // Decode the first kBits captured symbols back to bytes, MSB-first.
    uint8_t decoded[sizeof(kPattern)] = {};
    for (size_t b = 0; b < kBits; b++) {
        uint16_t high = static_cast<uint16_t>(rxSymbols[b] & 0x7FFF);  // duration0
        uint8_t bit = bitFromHigh(high);
        decoded[b / 8] = static_cast<uint8_t>((decoded[b / 8] << 1) | bit);
    }

    bool ok = true;
    for (size_t i = 0; i < sizeof(kPattern); i++) if (decoded[i] != kPattern[i]) ok = false;

    if (ok) {
        std::printf("[loopback] PASS: %u bytes round-tripped correct "
                    "(sent %02X %02X %02X, got %02X %02X %02X)\n",
                    (unsigned)sizeof(kPattern),
                    kPattern[0], kPattern[1], kPattern[2],
                    decoded[0], decoded[1], decoded[2]);
    } else {
        std::printf("[loopback] FAIL: byte mismatch "
                    "(sent %02X %02X %02X, got %02X %02X %02X)\n",
                    kPattern[0], kPattern[1], kPattern[2],
                    decoded[0], decoded[1], decoded[2]);
    }
}

#endif // MM_LED_LOOPBACK_TEST
