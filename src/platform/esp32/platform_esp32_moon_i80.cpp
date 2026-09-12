// Parallel WS2812 output over the ESP32-S3/P4 LCD_CAM i80 peripheral, driven by OUR OWN DMA
// sequencing instead of IDF's esp_lcd component — the peripheral half of MoonLedDriver
// (src/light/drivers/MoonLedDriver.h), which does all the domain work: applies Correction and
// 3-slot-encodes every light into the DMA frame buffer (ParallelSlots.h). This file owns only the
// peripheral — the LCD_CAM registers, the GDMA channel + descriptor chain, the frame buffer(s),
// transmit + wait, and the loopback test's TX side. No domain logic here.
//
// **Why this exists next to platform_esp32_i80.cpp** (the esp_lcd sibling — read that file first;
// it is the behavioural reference this one matches function for function). esp_lcd re-arms the
// peripheral on EVERY transaction: lcd_start_transaction() does lcd_ll_reset() + lcd_ll_fifo_reset()
// + a hard-coded 4 µs busy-wait before each one (esp_lcd_panel_io_i80.c:772-796). An LCD panel does
// not care; WS2812 is one unbroken self-clocked bit stream, so a mid-frame reset garbles everything
// after it. That makes a frame split across several esp_lcd transactions impossible to send
// gaplessly at any chunk size — which forces the whole frame into ONE transaction, and THAT is what
// caps the driver: the DMA must stream the entire frame from one contiguous DMA-reachable block.
//
// The hardware never demanded this. The LCD peripheral has NO data-length register —
// lcd_ll_set_phase_cycles() takes `data_cycles` as a boolean enable, and IDF's own comment reads
// "Number of data phase cycles are controlled by DMA buffer length" (esp_lcd_panel_io_i80.c:778).
// So the peripheral clocks out exactly what the DMA feeds it and stops when the chain ends: ONE
// gdma_start() over an arbitrarily long descriptor chain + ONE lcd_ll_start() is a single gapless
// stream across as many buffers as we like. This backend takes that, built on IDF's HAL + GDMA
// link-list APIs (one level below esp_lcd — not raw registers; IDF's own drivers use these same
// APIs, which is what keeps this a recognisable construct rather than a bespoke register poke).
//
// Both implementations ship: the esp_lcd one is the reference, this one is the measured
// alternative, and selecting between them is a module swap in the UI.
//
// Gated on SOC_LCDCAM_I80_LCD_SUPPORTED — the NARROW macro, unlike the esp_lcd sibling's broad
// SOC_LCD_I80_SUPPORTED: this backend pokes LCD_CAM registers through hal/lcd_ll.h, which does not
// exist on the classic ESP32 (whose i80 is the I2S peripheral in LCD mode, a different register
// file entirely). Inert stubs otherwise, since the CMake SRCS list is unconditional.

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_LCDCAM_I80_LCD_SUPPORTED

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_private/cache_utils.h"   // spi_flash_cache_enabled — the cache-safe ISR's defer guard
#include "esp_clk_tree.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"   // SIG_GPIO_OUT_IDX — detaches a matrix route (destroyState)
#include "esp_rom_sys.h"     // esp_rom_delay_us — the DMA-to-FIFO settle before lcd_ll_start
#include "esp_timer.h"       // esp_timer_get_time — ISR-safe wire-time stamp
#include "driver/gpio.h"
#include "soc/io_mux_reg.h"  // PIN_FUNC_GPIO — the IO-MUX function the matrix routes through
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "hal/gdma_channel.h"          // SOC_GDMA_TRIG_PERIPH_LCD0 / _BUS (the GDMA_MAKE_TRIGGER operands)
#include "hal/lcd_hal.h"               // lcd_hal_context_t / lcd_hal_init
#include "hal/lcd_ll.h"                // the LCD_CAM low-level register API
#include "hal/lcd_periph.h"            // soc_lcd_i80_signals — the GPIO-matrix signal indices
#include "esp_private/gdma.h"          // channel alloc / connect / strategy / transfer / start
#include "esp_private/gdma_link.h"     // the descriptor link list (esp_lcd uses exactly this)
#include "esp_private/esp_dma_utils.h" // esp_dma_calculate_node_count
#include "esp_private/gpio.h"          // gpio_func_sel — the IO-MUX select esp_lcd's GPIO setup uses
#include "esp_private/periph_ctrl.h"   // PERIPH_RCC_ACQUIRE_ATOMIC / PERIPH_RCC_ATOMIC
#include "esp_private/esp_clk_tree_common.h"  // esp_clk_tree_enable_src — power the LCD clock source

#include <atomic>      // std::atomic_thread_fence — orders the frontier splice's two descriptor writes
#include <cstring>
#include <functional>  // the transmit callback passed to the shared frame loopback
#include <new>         // std::nothrow

namespace mm::platform {

// Defined in platform_esp32_rmt.cpp — the plain-GPIO continuity pre-check the
// RMT loopback uses; the wire question is identical here.
namespace detail { bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio); }

namespace {

static const char* MOON_I80_TAG = "mm_moon_i80";

// 3 slots per WS2812 bit (the ParallelSlots.h contract): 2.67 MHz pclk = 375 ns
// slots, "0" = 1 slot HIGH, "1" = 2 slots HIGH. 375 ns and not the lineage's
// usual 416 ns: newer WS2812B revisions spec T0H max ≈ 380 ns, and on a
// direct 3.3 V data line (no level shifter) a longer "0" pulse gets misread
// as "1" — the strip washes out white. 375 ns sits inside every revision's
// window; the 160 MHz LCD clock divides to it exactly (/60).
constexpr uint32_t kPclkHz = 2'666'666;

// Pixel clock with a 74HCT595 expander fitted. **The constraint that matters is the WS2812 SLOT
// DURATION, not the elegance of the divider.** A '595 shifts each slot out over `clockMultiplier`
// bus words, so:
//
//     slot = clockMultiplier / pclk        and the slot IS the "0" pulse (T0H).
//
// WS2812B spec: T0H 200-380 ns (newer revisions cap ~380 — see lessons.md #5, the max-white
// flicker), T1H 580-1000 ns. The direct path picks 2.67 MHz for a 375 ns slot, right at that edge.
// 20 MHz ("because it divides exactly") gives 8 × 50 ns = 400 ns, OVER the T0H max, and the strands
// wash out white. An exact divider that produces an out-of-spec waveform is worthless.
//
// 26.67 MHz (prescale 3 off the 80 MHz bus resolution — still an exact divide):
//     slot = 8 / 26.67 MHz  = 300 ns   T0H 300 (spec 200-380 ✓)  T1H 600 (spec 580-1000 ✓)
//
// The '595 shift clock (SRCLK = the i80 WR) is the RUNTIME `shiftClockDiv` prescale off the 80 MHz bus
// resolution. **Default 4 = 20 MHz — the reliability point, wall-verified on two rigs.** The window:
//   div 3 (26.67 MHz): T0H 300 ns (strict spec) but a 28%-short bit (900 ns) and a shift rate marginal
//     '595 strands (long runs, capacitive load) can't track — specific panels scramble while clean ones
//     survive, and drive-strength CAP_3 does NOT help (bandwidth-bound, not edge-bound; both measured).
//     The overclock option for short-wired rigs chasing the higher fps ceiling (151 vs 118 at 48×256).
//   div 4 (20 MHz): T0H 400 ns (soft-max +6%, fine on modern WS2812), bit 1200 ns (near-spec), and the
//     '595 gets 1.33× more shift margin — Yves (hpwit) ships his S3 driver at ~19.2 MHz for this reason.
//   div 5 (16 MHz): T0H 500 ns crosses the WS2812's 0-vs-1 threshold — every bit reads "1", all-white.
// Per-wall calibration: sweep up from the default until marginal strands are clean; all-white = one too far.
constexpr uint8_t  kShiftClockDivDefault = 4;   // 80 MHz / 4 = 20 MHz
constexpr uint32_t kShiftBusResolutionHz = 80'000'000;   // PLL160M / kClockPreScale — the prescale base
// The live shift-clock prescale off the 80 MHz bus resolution, set per-build via moonI80SetShiftClockDiv
// before an init. A file-static (not threaded through every Init signature) — it is a single global
// peripheral-tuning knob, like a clock register, that the one MoonLed driver owns.
uint8_t g_shiftClockDiv = kShiftClockDivDefault;

// WS2812 latch/reset LOW: the spec is >=280-300 us; hpwit's rule is anything <150 us is read as a PAUSE
// (data continues) not a reset. 350 us clears both with margin. The ring guarantees this as idle-LOW time
// between the frame's stop (lastStopUs) and the next arm, NOT as clocked zero buffers — so it holds at ANY
// ringRows (a small-ringRows tail buffer is far under this) with ZERO extra RAM (see MoonI80State::lastStopUs).
constexpr int64_t kResetLowUs = 350;

// Pure-zero flush slices the lapping frame writes past its close slice (index nSlices), so the frame ends
// at slice `nSlices + kTailBufs`. File-scope so the ISR completion and the wait backstop agree on the
// frame's last slice — a local copy in each would silently diverge if this ever changes.
constexpr uint32_t kTailBufs = 1;

// The LCD_CAM group clock divider esp_lcd applies (LCD_PERIPH_CLOCK_PRE_SCALE in
// esp_lcd/priv_include/esp_lcd_common.h:28 — a PRIVATE header, so the constant is restated here
// rather than included). It is the minimum divider the peripheral accepts, and with the default
// PLL160M source it makes the bus resolution 160/2 = 80 MHz, off which every pclk above is an exact
// integer prescale. Kept identical to esp_lcd's so both backends produce the same waveform.
constexpr uint32_t kClockPreScale = 2;

// Max bytes one GDMA descriptor carries (LCD_DMA_DESCRIPTOR_BUFFER_MAX_SIZE, same private header).
// A 144 KB frame therefore needs ~37 nodes ≈ 444 B of descriptor memory — the chain is free.
// The value itself lives in platform.h (kRingNodeMaxBytes) — the driver's auto geometry shares it.
constexpr size_t kDmaNodeMaxBytes = kRingNodeMaxBytes;

// The LCD_CAM i80 bus index. Both the S3 and the P4 have exactly one (LCD_LL_I80_BUS_NUM == 1), and
// the whole point of this backend is that WE own the peripheral for the frame's duration.
constexpr int kBusId = 0;

// --- Ring mode (moonI80Ws2812*Ring) — how a frame too big for internal RAM is streamed ---------------
//
// The whole-frame path above needs the entire encoded frame in one DMA-reachable block; in shift mode
// that lands in PSRAM above ~96 lights/strand, and the S3's GDMA cannot sustain a PSRAM read at the
// expander's 26.67 MHz clock (see createState's measurement note). The ring sidesteps it: a closed
// descriptor chain over a few small INTERNAL buffers, refilled by the CPU as the DMA drains them, so
// the DMA never reads PSRAM at the shift clock at all. The encoder reads the tiny (internal) Layer
// buffer instead, ~24x smaller than the encoded frame. See platform.h.

// **The ring's geometry is RUNTIME, not a constant** — `rowsPerBuf` (lights per DMA buffer) and
// `ringBufs` (pool depth) arrive as parameters and live on MoonI80State. The driver exposes both as
// controls, because the optimum is a measurement, not a derivation: RAM is the ONLY axis that wants a
// small rowsPerBuf, and three others want it big.
//
//   RAM              = rowsPerBuf × ringBufs × rowBytes. Only at rowsPerBuf=1 does it stop scaling with
//                      strand length (~18 KB flat, any length) — the sole reason a per-light ring exists,
//                      since a 48×256 frame is 144 KB contiguous internal and that does not exist.
//   per-call cost    = the encode seam's fixed overhead, amortised over rowsPerBuf. At 1 it is paid per
//                      light, inside the ISR.
//   interrupt rate   = lights/rowsPerBuf per frame (one EOF per buffer). 256 lights at 100 fps is 25.6k
//                      int/s at rowsPerBuf=1 vs 1.6k at 16 — and a busy core-0 ISR starves the network
//                      stack (measured: a ~19 ms encode killed the W5500 ethernet on the LC16).
//   lap-time runway  = rowsPerBuf × ringBufs × wire-µs-per-light: how long a WiFi preemption may last
//                      before the DMA laps a buffer the ISR is still refilling. 1×32 ≈ 690 µs; 16×12 ≈ 4.1 ms.
//
// Bench history worth keeping: at rowsPerBuf=16 the pool only ever held ~12 buffers (~140 KB; 16 buffers
// = 176 KB never fit the S3's ~160 KB free internal DMA heap), which caps that geometry near 240
// lights/strand.
//
// **The "scatter above ~8 slices" hunt (2026-07-17) — RESOLVED.** It read as an unknown mechanism at the
// time, but it was two knowable, per-hardware faults the ring counters are structurally blind to: the
// inter-slice pad LATCHING the strand (a LOW gap over the strip's reset threshold resets the address
// pointer, repainting LEDs 0..ringRows-1 per slice — see ringPadUs), and the producer/consumer headroom
// (bufs must lead slices; the near-prime pool is the fix). With ringAuto's geometry + a latch-safe pad,
// 48×256 streams clean (wall-verified). The lasting lesson: with owner_check=false there is no handshake,
// so a torn/short read raises NO error (descErr stays 0, timings look healthy) — a correct geometry drawn
// in SCATTERED DOTS reads as a latch/reset fault, not a data fault. Do not trust a ring counter for that
// class; the wall (or the loopback bit-verify) is the instrument.
// A depth of 2 (IDF's RGB-LCD bounce-buffer count) BROKE transport: the loopback failed at bit 0, because
// our chain runs owner_check=false and lacks the owner gate IDF's 2-buffer scheme relies on. Hence the
// floor of 2 is a hard minimum, not a useful setting.
// kRingBufsMax/kRingBufsMin live in platform.h — the shared bounds the driver's auto geometry and
// control range must agree with; here they size ring[] and gate InitRing's depth check.

// Backstop for a transmit that arrives while the previous frame is still on the wire (the async
// double-buffer's normal case — see moonI80Ws2812Transmit). It bounds a WEDGED peripheral, nothing
// more: a healthy frame clears the wire in single-digit milliseconds, and the driver's own
// frame-derived timeout (ParallelLedDriver::waitBudgetMs) is what actually governs a stalled bus.
// This only exists so a broken DMA cannot hang the render thread forever.
constexpr uint32_t kWireFreeTimeoutMs = 200;

// The frame buffer(s), the peripheral, and the DMA chain that streams one into the other.
//
// Two DMA frame buffers for the async deferred-wait double-buffer: the driver encodes frame N+1
// into buf[1-active] while the GDMA clocks frame N out of buf[active]. buf[1] is null when the
// second allocation didn't fit (single-buffer mode). Each buffer has its OWN done-semaphore so a
// wait targets the right transfer.
//
// The GDMA completes transfers in START order and its EOF event carries no per-transfer token, so a
// 2-slot in-order completion FIFO of started buffer indices, popped in the EOF callback, routes each
// done-signal to the buffer that actually finished. (The textbook completion FIFO for an in-order
// DMA queue; depth 2 because at most two transfers — one per buffer — are ever outstanding.)
//
// Unlike esp_lcd there is no transaction queue: the peripheral is ours, so a transmit programs the
// chain and starts the hardware directly. `busy` is therefore the honest state — a second transmit
// while one is in flight is a caller error (the driver waits before reusing a buffer), and is
// rejected rather than silently queued behind a peripheral reset.
struct MoonI80State {
    lcd_hal_context_t hal = {};
    gdma_channel_handle_t dma = nullptr;
    gdma_link_list_handle_t link = nullptr;
    SemaphoreHandle_t done[2] = {nullptr, nullptr};
    // Signals "the wire is free" — given by the EOF ISR, taken by a transmit that arrives while a
    // transfer is still clocking out. SEPARATE from done[]: the driver owns those (it waits on the
    // buffer it wants to reuse), and a transmit must not consume a signal the driver is still going to
    // wait for. One producer (the ISR), one consumer (the transmit), so a binary semaphore is exactly
    // the right primitive.
    SemaphoreHandle_t wireFree = nullptr;
    uint8_t* buf[2] = {nullptr, nullptr};
    size_t cap = 0;             // shared per-buffer capacity (both buffers equal)
    size_t busWidth = 8;        // 8 or 16 data lines
    uint32_t prescale = 1;      // pixel-clock prescale off the 80 MHz bus resolution
    bool clockAcquired = false; // the PERIPH_RCC bus-clock reference this state holds
    // The GPIO-matrix routes configureGpio established, kept so destroyState can tear them down — a
    // deleted driver (not rebuilt) must not leave data/WR signals routed to a freed peripheral.
    uint16_t routedPins[16] = {};   // data GPIOs routed to the bus (first `routedPinCount`)
    uint8_t  routedPinCount = 0;
    int32_t  routedWrGpio = -1;      // WR GPIO if routed (shift mode), else -1
    // In-order completion FIFO of started buffer indices (0/1). The transmit pushes at head; the
    // EOF ISR pops at tail. Only ever 0..2 entries (one per buffer).
    volatile uint8_t fifo[2] = {0, 0};
    volatile uint8_t fifoHead = 0;   // next write slot (mod 2)
    volatile uint8_t fifoTail = 0;   // next read slot (mod 2)
    // Wire-time KPI: the hardware-start timestamp of each in-flight transfer, and the last measured
    // duration. Paired with the FIFO, so it tracks the transfer the next EOF completes.
    volatile int64_t txStartUs[2] = {0, 0};
    volatile uint32_t lastTransmitUs = 0;
    // Absolute time (esp_timer) the ring peripheral was last STOPPED — the moment the strand starts idling
    // LOW, i.e. the WS2812 reset begins. startRingTransfer holds the next arm until >=kResetLowUs has
    // elapsed since this, so the reset is a real >=300 us LOW at ANY ringRows (a small ringRows tail buffer
    // alone is < the 150 us the WS2812 reads as a reset — hpwit: "less than 150us ... like it was sent just
    // after"; below that the strand never latches and the frame FREEZES). Zero extra RAM, pool-size-safe.
    volatile int64_t lastStopUs = 0;
    volatile bool busy = false;      // a transfer is clocking out right now

    // --- Ring mode. Null/zero on a whole-frame handle; populated only by moonI80Ws2812InitRing. ------
    bool isRing = false;
    uint8_t* ring[kRingBufsMax] = {};   // the internal-RAM slice buffers the DMA loops over (first `ringBufs` used)
    uint8_t  ringBufs = 0;           // pool depth in use — the live count; kRingBufsMax is only the array bound
    uint32_t rowsPerBuf = 0;         // rows one ring buffer holds (the last SLICE may be shorter)
    uint32_t totalRows = 0;          // strand length in rows — the frame ends after this many
    size_t   linkItemCap = 0;        // descriptor-pool capacity — the mount loop must not exceed it (IDF wraps silently)
    uint32_t consumedItems = 0;      // descriptor items the mount loop actually used (diagnostic; == linkItemCap when sized right)
    // Descriptor nodes per ring buffer (a buffer larger than kDmaNodeMaxBytes spans >1 node). Buffer b's LAST
    // node is (b+1)*itemsPerBuf - 1 — the ISR needs this to splice the self-terminating NULL at the right node.
    uint8_t  itemsPerBuf = 1;
    // Self-terminating chain (hpwit): the descriptor node the prime-only path mounts NULL-terminated to end
    // THIS frame (`bufLastNode[termBuf]`), captured for the `termNodeDiag` stat. -1 = no terminator (the
    // lapping path's looping chain, stopped clock-keyed). The prime-only chain is mounted terminated at arm
    // time (hazard-free): the DMA self-terminates there instead of a mid-frame gdma_stop racing the
    // prefetcher. Diagnostic only — no splice/restore reads this; the chain is rebuilt each arm.
    int32_t  termNode = -1;
    // Each buffer's ACTUAL last descriptor node, captured from gdma_link_mount_buffers' endIdx during the
    // mount. The splice keys off THIS, not arithmetic: gdma_link_mount_buffers may allocate a different node
    // count than esp_dma_calculate_node_count predicts (alignment/rounding), so `(b+1)*itemsPerBuf-1` was
    // WRONG — it terminated a buffer early (bench: ld=7 for a 10-slice frame, node 21 landed in buffer 7 not
    // 10). The mount's own endIdx is the ground truth.
    int32_t  bufLastNode[kRingBufsMax] = {};
    // Per-buffer "constants gone" flag for the encode seam's needsPrefill (see MoonI80EncodeFn in
    // platform.h): true at pool build and after any platform-side memset of the buffer; cleared when the
    // encode has been told once. Lets a uniform-lane encoder skip the per-refill prefill (~1/3 of the ISR
    // encode cost) on every recycled buffer whose constants are still intact.
    bool     bufNeedsPrefill[kRingBufsMax] = {};
    size_t   ringRowBytes = 0;       // encoded bytes per row (encode writes rowsPerBuf × this per buffer)
    MoonI80EncodeFn   encode = nullptr;   // the domain's slice encoder (platform.h seam)
    void*             encodeUser = nullptr;
    // THE CLOCK ORACLE (lapping). The looping DMA free-runs at the crystal-exact bus byte rate, so the
    // drain position is a function of TIME, not of interrupt arrivals: the DMA is draining slice
    // `elapsed / slice-duration` right now, whatever the interrupt latch did. The EOF interrupt is a
    // LATCH, not a queue — two EOFs under load coalesce into one firing — so any counter incremented
    // per-firing undercounts and any cursor advanced per-firing drifts (the shifted-frame artifact).
    // Deriving both the refill target and the frame end from `(now − armUs) / sliceNs` makes a coalesced
    // interrupt change only WHEN work happens, never WHAT gets written.
    volatile int64_t  armUs = 0;          // esp_timer time at gdma_start — the oracle's epoch, per frame
    uint32_t          sliceNs = 0;        // one slice's wire duration incl. the pad (bytes × 37.5 ns + padUs)
    // The refill cursor is a SLICE INDEX (slice s lives in buffer s % ringBufs — the mount order fixes the
    // congruence). Batch-advanced by the ISR toward the oracle's writable window; reset per frame by
    // startRingTransfer (priming writes slices 0..ringBufs-1).
    volatile uint32_t lastWrittenSlice = 0;   // highest slice index already encoded (or zero-filled) this frame
    // Interleaved shared zero-pad (the deadline stretch, hpwit's _DMA_EXTENSTION written fresh): one zero
    // block every pad node points at; padUs of LOW after each buffer reads as a pause (< the ~150 µs
    // latch threshold), stretching the per-slice refill deadline at a linear frame-time cost. 0 = no pads.
    uint8_t* zeroPad = nullptr;
    size_t   zeroPadBytes = 0;
    uint8_t  padUs = 0;
    // Cache-sync facts hoisted out of the refill: the ring buffers are fixed internal allocations, so the
    // line size is a per-pool constant — querying it per refill was a flash call inside the ISR (illegal
    // once the channel is cache-safe, and a needless icache miss before that).
    size_t   ringCacheLine = 0;
    uint32_t          nSlices = 0;        // total slices in the frame = ceil(totalRows / rowsPerBuf)
    volatile uint32_t drainCount = 0;     // DIAGNOSTIC ONLY: the oracle position the last EOF observed
    // DIAGNOSTIC counters (exposed via moonI80Ws2812RingStats → the driver's ringDbg control): lifetime
    // EOF interrupts, lifetime frame completions, and the drainCount the last EOF saw. Bumped in the ISR
    // (volatile, no lock — a best-effort diagnostic, not a contract). These distinguish "EOFs fire but the
    // last-slice done isn't given" from "no EOFs at all" when the ≥256 reuse boundary stalls.
    volatile uint32_t dbgEofTotal = 0;
    volatile uint32_t dbgDoneGiven = 0;
    volatile uint32_t dbgLastDrain = 0;
    // WHITE-FLASH DIAGNOSTIC (2026-07-20): the EOF ISR defers (returns without refilling) whenever the
    // flash cache is disabled — a SPI-flash write (NVS/config-save) or the WiFi driver's flash access.
    // If that write outlasts the pool's lead, the DMA laps un-refilled buffers → a corrupt frame. Random
    // timing, invisible to `late` (a deferred refill is a SKIPPED write, not a stale one). Count them, and
    // the worst run of consecutive defers, to test whether the random ~few-second white flash correlates.
    volatile uint32_t dbgCacheOffDefers = 0;
    volatile uint32_t dbgCacheOffRun = 0;      // current consecutive-defer streak
    volatile uint32_t dbgCacheOffMaxRun = 0;   // worst streak seen (≈ how many buffers the DMA could lap)
    // Frames the wait backstop finalized because the DMA self-terminated at the write FRONTIER during a
    // cache-off stall (no further EOF fires once the engine halts, so the ISR can never give `done` for
    // such a frame). Each one is a partially-updated frame the strand held for one frame period — the
    // benign outcome the frontier terminator exists to produce (instead of replaying stale slices).
    volatile uint32_t dbgStallAbandons = 0;
    // B1-DISCRIMINATOR (diagnostic): a GDMA descriptor-error count. The researcher's leading hypothesis is
    // that the in-ISR encode writes outside ring[slot] and smashes the descriptor pool → the GDMA fetches a
    // garbage descriptor and halts SILENTLY (TX_DESC_ERROR is not a registered interrupt today). Registering
    // on_descr_err and counting it here turns that silent halt into a visible signal: descErr > 0 at the
    // stall == B1 confirmed (memory corruption), descErr == 0 == look elsewhere (B2 underrun-wedge / B3).
    volatile uint32_t dbgDescErr = 0;
    // REUSE-RACE INSTRUMENTATION (diagnostic): is the ISR refill LOSING the race at deep reuse (256)?
    // dbgMaxEncodeUs = worst-case time one ISR refill (encodeRingSlice) took. dbgMaxIsrGapUs = worst gap
    // between two consecutive EOFs (how fast the DMA drains a buffer — the deadline the refill must beat).
    // If dbgMaxEncodeUs approaches/exceeds dbgMaxIsrGapUs, the refill can't keep pace (a PACE problem);
    // if it's well under and 256 still fails, it's a LOGIC/off-by-one (a CURSOR problem, not timing).
    volatile uint32_t dbgMaxEncodeUs = 0;
    // The AVERAGE is the pace number (can the producer keep up?); the MAX above is the jitter number
    // (how bad is the worst spike?). Conflating them cost a day: a 63 µs max read as "the encode floor"
    // when the typical refill may be far cheaper. Sum+count, divided at readout — no ISR division.
    volatile uint32_t dbgEncSumUs = 0;
    volatile uint32_t dbgEncCount = 0;
    volatile uint32_t dbgEncAvgUs = 0;   // LAST FRAME's average, latched at frame end — the readout target
                                         // (reading sum/count mid-frame races the per-frame reset: the 1 s
                                         // KPI tick correlates with the frame cycle and kept landing in the
                                         // freshly-reset window, reading a false 0)
    volatile uint32_t dbgMaxIsrGapUs = 0;
    volatile int64_t  dbgLastEofUs = 0;
    // The machine's scatter meter: slices the batch refill wrote AFTER the oracle said their drain had
    // begun — each one was stale on the wire for part or all of its slot. A clean soak is dbgLate == 0;
    // any increment is a deadline miss whether or not the eye catches it on the wall.
    volatile uint32_t dbgLate = 0;
};

// B1-DISCRIMINATOR (diagnostic): GDMA descriptor-error callback. Registered alongside on_trans_eof so a
// descriptor-fetch fault (the silent-halt class the researcher suspects) is COUNTED instead of ignored.
// IRAM_ATTR + trivial (one volatile increment) — ISR-safe.
bool IRAM_ATTR moonI80DescErrCb(gdma_channel_handle_t, gdma_event_data_t*, void* user) {
    auto* st = static_cast<MoonI80State*>(user);
    st->dbgDescErr = st->dbgDescErr + 1u;
    return false;
}

// Forward decl: the ring branch of the EOF ISR below refills the drained buffer inline by calling this
// (defined further down with the ring code). The move to an ISR-driven refill is the reuse-race fix.
void encodeRingSlice(MoonI80State* st, uint8_t slot, uint32_t firstRow, uint32_t count);
// The shared slice-fill (IRAM, defined with the ring code below); the EOF ISR refill calls it too. The
// IRAM_ATTR goes on the DEFINITION only — repeating it here conflicts the .iram1 section (matches
// encodeRingSlice's forward decl above). Returns true iff it ran a real encode (the ISR times only those).
bool fillSlice(MoonI80State* st, uint8_t slot, uint32_t sliceIdx);

// The chain node buffer b ENDS at — its pad node when the pool is padded (mounted immediately after the
// data node, so always bufLastNode[b] + 1), else its data node. This is the node the frontier terminator
// rests on and the node an inter-buffer link leaves from. Trivial → inlines into the (IRAM) callers.
inline int ringTailNode(const MoonI80State* st, uint8_t b) {
    return st->bufLastNode[b] + (st->zeroPad ? 1 : 0);
}

// GDMA transfer-EOF callback: the descriptor chain hit its EOF node — pop the oldest started buffer
// index, record the wire duration, and release THAT buffer's waiter.
//
// IDF notes that the TX EOF fires when the DMA has pushed the last bytes into the LCD FIFO, which
// can be a few pclk cycles BEFORE the last bits leave the pins (gdma_strategy_config_t's
// `eof_till_data_popped` exists precisely to close that gap). That is fine for this contract: a wait
// gates *reusing a buffer*, not *reading the output* — and the DMA is provably finished reading a
// buffer at EOF, which is exactly the question the caller asks. The residual few-hundred-nanosecond
// error in the wire-time KPI is far below its resolution.
//
// IRAM_ATTR: this runs in the GDMA interrupt, and the RING branch calls encodeRingSlice → the domain
// encode. That whole chain is IRAM-resident (MM_RAMFUNC), and the ring channel sets
// `chanCfg.flags.isr_cache_safe = true` (see initRingDma) — a deliberate, shipped hardening: the ISR
// fires at the wire rate, so a flash-cache miss inside it would blow the refill deadline. Because it is
// cache-safe, the ISR may fire *while the flash cache is disabled* (a SPI-flash write — OTA/NVS), so the
// slice encode MUST NOT touch flash-resident code/data: `startRingTransfer`/the refill defer when
// `spi_flash_cache_enabled()` is false (see the guard below), and the batch catches up afterward. Do NOT
// remove that guard or move the encode back to flash — either reintroduces the measured Cache-error panic.
// The whole-frame branch is also IRAM-safe (esp_timer_get_time reads a hardware counter,
// xSemaphoreGiveFromISR, plain member stores). Prior art: IDF's RGB-LCD bounce-buffer EOF refill
// (esp_lcd_panel_rgb.c) does the same real work in-ISR under CONFIG_LCD_RGB_ISR_IRAM_SAFE.
bool IRAM_ATTR moonI80EofCb(gdma_channel_handle_t, gdma_event_data_t*, void* user) {
    auto* st = static_cast<MoonI80State*>(user);

    // Ring mode. PRIME-ONLY (nSlices <= ringBufs): every slice was encoded before arming and only the
    // TERMINATOR node carries mark_eof, so the single EOF that reaches this ISR IS the frame's end — the
    // chain has already self-terminated at the mount-time NULL (never gdma_stop; that mid-frame stop
    // racing the prefetcher is the bug the prime-only redesign removed).
    //
    // LAPPING (nSlices > ringBufs): every data node carries mark_eof, and the ISR is driven by THE CLOCK
    // ORACLE, not its own arrival count. The GDMA interrupt is a latch — two EOFs under load coalesce
    // into one firing — so nothing here may count firings: the drain position is derived from elapsed
    // time (the DMA free-runs at the crystal-exact wire rate), each firing BATCH-refills every slice the
    // writable window allows AND advances the frontier terminator to the last written slice, and the frame
    // ends when the CLOCK says the frame + tail has drained (or, on a stall, when the DMA reaches the
    // frontier NULL and the wait backstop finalizes it). A coalesced interrupt then changes only when work
    // happens, never what gets written. hpwit's trailing-refill pool + early terminator, with the oracle
    // replacing his per-buffer bookkeeping.
    if (st->isRing) {
        BaseType_t high = pdFALSE;
        // Cache-off guard (the pattern IDF's own cache-safe ISRs use): the channel registers this handler
        // ESP_INTR_FLAG_IRAM, so it FIRES during a SPI-flash write — but the refill reads data the flash
        // cache maps (the driver object and any PSRAM-resident source live behind the same cache a flash
        // write disables; measured: a Cache-error panic in the trampoline during a config save). Deferring
        // is free by design: the clock-oracle batch below refills everything owed on the NEXT firing, and
        // the pool's lead absorbs the write's duration. So: fire, notice the cache is off, come back.
        if (!spi_flash_cache_enabled()) {
            // Cache off: this firing refills NOTHING and does NOT advance the frontier. Count it + track the
            // consecutive-defer streak (buffers drained un-refilled while the write holds). If the write
            // outlasts the pool's lead, the DMA reaches the frontier NULL and HALTS — the wait backstop then
            // finalizes the held frame (dbgStallAbandons). No stale-slice replay: the frontier is the guard.
            st->dbgCacheOffDefers = st->dbgCacheOffDefers + 1u;
            st->dbgCacheOffRun = st->dbgCacheOffRun + 1u;
            if (st->dbgCacheOffRun > st->dbgCacheOffMaxRun) st->dbgCacheOffMaxRun = st->dbgCacheOffRun;
            return false;
        }
        st->dbgCacheOffRun = 0;                          // a live firing ends the streak
        st->dbgEofTotal = st->dbgEofTotal + 1u;         // ISR INSTRUMENTATION (diagnostic)
        const int64_t eofNow = esp_timer_get_time();
        if (st->dbgLastEofUs != 0) {
            const uint32_t gap = static_cast<uint32_t>(eofNow - st->dbgLastEofUs);
            if (gap > st->dbgMaxIsrGapUs) st->dbgMaxIsrGapUs = gap;
        }
        st->dbgLastEofUs = eofNow;

        const bool primeOnly = st->nSlices <= st->ringBufs;
        if (!primeOnly && st->busy) {
            // The oracle: slices whose drain has COMPLETED. sliceNs is exact (bytes × 37.5 ns + pad), so
            // the only error source is esp_timer resolution — microseconds against a >100 µs slice.
            const uint32_t drainPos = static_cast<uint32_t>(
                ((eofNow - st->armUs) * 1000) / st->sliceNs);
            st->drainCount = drainPos;      // DIAGNOSTIC (dbgLastDrain mirrors the old counter's slot)
            st->dbgLastDrain = drainPos;

            // BATCH REFILL toward the writable window. Slice s occupies buffer s % ringBufs; its previous
            // occupant (slice s − ringBufs) is provably drained once drainPos ≥ s − ringBufs + 1, so with a
            // kLead safety margin the window is s ≤ drainPos + ringBufs − kLead. Capped per firing to bound
            // ISR duration — EOFs keep arriving every slice, so capped batches still converge; the POOL is
            // the jitter buffer (a worst-case encode spike borrows the pool's lead and the next batches
            // repay it — only the AVERAGE encode must beat the slice duration).
            constexpr uint32_t kLead = 2;
            constexpr uint32_t kBatchMax = 4;
            // kTailBufs (file scope): one pure-zero flush slice past the frame-close slice, so a stop firing
            // on the second-to-last EOF (oracle floor at a slice boundary) can only truncate zeros, never the
            // frame-close latch (which streams a whole slice earlier).
            const uint32_t windowEnd = drainPos + st->ringBufs - kLead;
            // The frame's last writes are the frame-close slice (index nSlices: zeros + the closing latch
            // word) and kTailBufs pure-zero flush slices. The frontier terminator then RESTS on the final
            // node as the frame's hardware end — the DMA cannot pass a NULL, so nothing past the frame is
            // ever re-read (no lap of zero-fill needed beyond it).
            const uint32_t lastSlice = st->nSlices + kTailBufs;
            for (uint32_t n = 0; n < kBatchMax; n++) {
                const uint32_t s = st->lastWrittenSlice + 1u;
                if (s > windowEnd || s > lastSlice) break;
                const uint32_t slot = s % st->ringBufs;
                // Stale-on-the-wire detector: the drain of slice s begins at drainPos == s, so filling at
                // or after that moment means the wire already clocked (part of) the OLD contents.
                if (drainPos >= s) st->dbgLate = st->dbgLate + 1u;
                const int64_t encStart = esp_timer_get_time();   // REUSE-RACE INSTRUMENTATION (diagnostic)
                const bool realEncode = fillSlice(st, static_cast<uint8_t>(slot), s);
                // Time only REAL encodes (fillSlice==true) — the `ea`/max pace numbers must reflect the
                // refill that has to beat the slice deadline, not the cheap past-frame zero-fills.
                if (realEncode) {
                    const uint32_t encUs = static_cast<uint32_t>(esp_timer_get_time() - encStart);
                    if (encUs > st->dbgMaxEncodeUs) st->dbgMaxEncodeUs = encUs;
                    st->dbgEncSumUs = st->dbgEncSumUs + encUs;    // average = sum/count at readout
                    st->dbgEncCount = st->dbgEncCount + 1u;
                }
                // THE MOVING FRONTIER TERMINATOR (hpwit's early splice, generalized to every refill): the
                // chain always ENDS at the last written slice. Terminate the new frontier FIRST, then extend
                // the old frontier into it — in that order (with a release fence between the two descriptor
                // writes) there is never an instant where the chain runs past written data into a stale
                // link. If the refill stalls (the cache-off defer above), the DMA reaches the frontier's
                // NULL and the HARDWARE halts: lines idle LOW, the strand latches a partially-updated frame
                // — instead of lapping the pool and re-clocking stale address-multiplexed slices as
                // max-bright garbage (the measured WiFi white-flash). gdma_link_concat writes through the
                // list's non-cacheable alias, so the edit needs no cache sync; it runs only on this cache-ON
                // path (flash-resident is fine here — the cache-off firing defers before reaching it). This
                // is safe where the old one-shot end-splice wedged (`ld=5`): the splice lands ~(ringBufs −
                // kLead) buffers AHEAD of the drain, far outside the descriptor-prefetch window, and the
                // node index is the mount's own endIdx (bufLastNode), not arithmetic.
                gdma_link_concat(st->link, ringTailNode(st, static_cast<uint8_t>(slot)), nullptr, 0);
                std::atomic_thread_fence(std::memory_order_release);
                gdma_link_concat(st->link, ringTailNode(st, static_cast<uint8_t>((s - 1u) % st->ringBufs)),
                                 st->link, st->bufLastNode[slot]);
                st->lastWrittenSlice = s;
            }

            // FRAME END. The HARDWARE ends the frame: the frontier terminator sits on the final flush slice
            // (index lastSlice), and the DMA HALTS at its NULL when it drains there — no software stop, no
            // post-frame EOF to catch. So completion is reported the instant the LAST slice is WRITTEN (this
            // is the firing whose batch just planted lastSlice's terminator): from here the frame's fate is
            // sealed — the DMA will drain the remaining written slices at the crystal-exact wire rate and
            // stop itself. Reporting on written-not-drained is what dodges the EOF-coalescing trap that
            // wedged the old drain-keyed stop (two late EOFs near frame end latch into one, and the single
            // firing that would have observed drainPos ≥ lastSlice never arrives — a drain-gated `done` then
            // deadlocks: the halted engine fires no further EOF, the semaphore is never given, and the driver
            // stops sending frames; bench-measured as flicker-then-"no LED output"). The engine is NOT
            // stopped here — the terminator halts it; stopping it now would truncate the still-draining wire.
            // The next arm's kResetLowUs guard holds off until the wire has idled LOW, keyed to lastStopUs =
            // the deterministic wire-end time (armUs + the whole frame's wire duration), not this instant.
            // Fires exactly once per frame: this whole block is gated on st->busy, and the completion
            // clears it — later firings (the batch loop can run several after the last slice is written)
            // see busy == false and skip.
            //
            // Why this can't report a stall-truncated frame as a clean completion (the accounting `ab`
            // relies on): reaching this branch means an EOF fired, and an EOF fires only while the engine is
            // RUNNING. A cache-off stall long enough to truncate the wire is one where the DMA reached the
            // frontier NULL and HALTED — a halted engine fires no further EOF, so this ISR never runs again
            // for that frame and its `done` comes from the wait backstop (counted in `ab`), never here. The
            // batch above can only advance lastWrittenSlice to lastSlice while EOFs keep arriving, i.e. while
            // the wire is genuinely still streaming — so a completion here is always a real, un-truncated one.
            if (st->lastWrittenSlice >= lastSlice) {
                st->busy = false;
                // Latch this frame's encode average and reset the window — at frame END, when every
                // refill has landed, so a readout never sees a half-filled (or just-reset) window.
                st->dbgEncAvgUs = st->dbgEncCount ? st->dbgEncSumUs / st->dbgEncCount : 0;
                st->dbgEncSumUs = 0;
                st->dbgEncCount = 0;
                // The wire finishes at armUs + the whole frame's wire time; the reset LOW begins THERE, not
                // at this (earlier) written-moment. The next arm's kResetLowUs guard reads lastStopUs, so it
                // still waits out the real wire tail before re-arming — no truncation. The frame's wire
                // carries nSlices data slices PLUS the kTailBufs flush slice — omitting the tail here would
                // start the reset clock one slice early (shaving the WS2812 LOW hold) and release the
                // prime barrier below while the tail slices are still draining.
                const uint32_t frameWireUs = static_cast<uint32_t>(
                    (static_cast<uint64_t>(st->nSlices + kTailBufs) * st->sliceNs) / 1000u);
                st->lastStopUs = st->armUs + frameWireUs;
                st->lastTransmitUs = frameWireUs;
                st->dbgDoneGiven = st->dbgDoneGiven + 1u;
                xSemaphoreGiveFromISR(st->done[0], &high);   // the ring reports completion on slot 0
            }
        } else if (primeOnly && st->busy) {
            // The terminator's EOF — the frame's one interrupt. The chain has self-terminated at NULL.
            st->drainCount = st->nSlices;
            st->dbgLastDrain = st->nSlices;
            st->busy = false;
            st->lastStopUs = eofNow;
            st->lastTransmitUs = static_cast<uint32_t>(eofNow - st->txStartUs[0]);
            st->dbgDoneGiven = st->dbgDoneGiven + 1u;
            xSemaphoreGiveFromISR(st->done[0], &high);
        }
        return high == pdTRUE;
    }

    // Whole-frame mode: pop the oldest started buffer, record its wire time, release its waiter.
    // Guard on a non-empty FIFO: if the wait-timeout backstop already drained this entry (fifoTail ==
    // fifoHead) and the lost EOF then arrives late, popping would read a stale slot, hand out a spurious
    // `done`, and desync tail past head — so a firing against a structurally empty FIFO is dropped.
    if (st->fifoTail == st->fifoHead) return false;
    const uint8_t slot = st->fifoTail;
    const uint8_t b = st->fifo[slot] & 1u;
    const int64_t now = esp_timer_get_time();
    st->lastTransmitUs = static_cast<uint32_t>(now - st->txStartUs[slot]);
    st->fifoTail = (st->fifoTail + 1u) & 1u;
    st->busy = false;
    BaseType_t high = pdFALSE;
    xSemaphoreGiveFromISR(st->done[b], &high);
    BaseType_t highWire = pdFALSE;
    xSemaphoreGiveFromISR(st->wireFree, &highWire);   // release a transmit blocked on the wire
    return (high == pdTRUE) || (highWire == pdTRUE);
}

void destroyState(MoonI80State* st) {
    if (!st) return;
    // GDMA teardown FIRST: the refill now runs in the EOF ISR, so stopping + deleting the channel (which
    // disables the interrupt) is what guarantees no further refill fires into a buffer about to be freed.
    // gdma_stop halts the engine; gdma_del_channel detaches the ISR. After this no EOF handler can run, so
    // the buffer frees below are safe.
    if (st->dma) {
        gdma_stop(st->dma);
        gdma_disconnect(st->dma);
        gdma_del_channel(st->dma);
    }
    if (st->link) gdma_del_link_list(st->link);
    // Detach the GPIO-matrix routes configureGpio established, so a deleted driver leaves no data/WR
    // signal pointing at this (now torn-down) peripheral. Route each back to plain GPIO (SIG_GPIO_OUT_IDX
    // = no peripheral). Safe after the GDMA barrier above — nothing is clocking these pins any more, and
    // a route that was never made (routedPinCount 0, routedWrGpio -1) is simply skipped.
    for (uint8_t i = 0; i < st->routedPinCount; i++)
        esp_rom_gpio_connect_out_signal(st->routedPins[i], SIG_GPIO_OUT_IDX, false, false);
    if (st->routedWrGpio >= 0)
        esp_rom_gpio_connect_out_signal(static_cast<uint16_t>(st->routedWrGpio), SIG_GPIO_OUT_IDX,
                                        false, false);
    if (st->hal.dev) {
        lcd_ll_stop(st->hal.dev);
        PERIPH_RCC_ATOMIC() {
            lcd_ll_enable_clock(st->hal.dev, false);
        }
    }
    // Release the bus-clock reference taken in createState (the peripheral powers down when the last
    // holder — us or an esp_lcd bus — lets go). Mirrors esp_lcd_del_i80_bus, esp_lcd_panel_io_i80.c:305.
    if (st->clockAcquired) {
        PERIPH_RCC_ACQUIRE_ATOMIC(soc_lcd_i80_signals[kBusId].module, ref_count) {
            if (ref_count == 0) lcd_ll_enable_bus_clock(kBusId, false);
        }
    }
    for (auto* b : st->buf) if (b) heap_caps_free(b);
    for (auto* b : st->ring) if (b) heap_caps_free(b);   // ring buffers (null on a whole-frame handle)
    if (st->zeroPad) heap_caps_free(st->zeroPad);        // the shared inter-buffer pad block
    for (auto* s : st->done) if (s) vSemaphoreDelete(s);
    if (st->wireFree) vSemaphoreDelete(st->wireFree);
    delete st;
}

// Bring up the LCD_CAM peripheral in i80 mode for a pure data phase. Replicates
// esp_lcd_new_i80_bus (esp_lcd_panel_io_i80.c:135-245) + lcd_i80_select_periph_clock (:642) +
// lcd_i80_switch_devices' per-device register writes (:800-819), minus everything a WS2812 frame
// does not use: no LCD interrupt (the GDMA EOF is our completion), no transaction queue, no format
// buffer, no PM lock, no sleep retention.
bool initPeripheral(MoonI80State* st, uint32_t pclkHz) {
    // Bus resolution = source clock / the group prescale. Ask the clock tree rather than assuming
    // 160 MHz, so a future default-source change can't silently retune the WS2812 waveform.
    uint32_t srcHz = 0;
    if (esp_clk_tree_enable_src(static_cast<soc_module_clk_t>(LCD_CLK_SRC_DEFAULT), true) != ESP_OK)
        return false;
    if (esp_clk_tree_src_get_freq_hz(static_cast<soc_module_clk_t>(LCD_CLK_SRC_DEFAULT),
                                     ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &srcHz) != ESP_OK)
        return false;
    const uint32_t resolutionHz = srcHz / kClockPreScale;   // 80 MHz with the PLL160M default
    const uint32_t prescale = resolutionHz / pclkHz;
    // The prescale is an integer register field: an inexact pclk rounds DOWN into a LONGER slot, and
    // a too-long "0" pulse is read as a "1" (the max-white washout). Both supported rates divide 80
    // MHz exactly, so reject anything that doesn't rather than emit a wrong waveform.
    if (prescale == 0 || prescale > LCD_LL_PCLK_DIV_MAX) return false;
    st->prescale = prescale;

    // Power the peripheral's APB/bus clock. Reference-counted: esp_lcd may hold the same peripheral
    // for the sibling driver, so the reset only fires for the first holder.
    PERIPH_RCC_ACQUIRE_ATOMIC(soc_lcd_i80_signals[kBusId].module, ref_count) {
        if (ref_count == 0) {
            lcd_ll_enable_bus_clock(kBusId, true);
            lcd_ll_reset_register(kBusId);
        }
    }
    st->clockAcquired = true;

    lcd_hal_init(&st->hal, kBusId);
    lcd_cam_dev_t* dev = st->hal.dev;

    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_clock(dev, true);
        lcd_ll_select_clk_src(dev, LCD_CLK_SRC_DEFAULT);
        // Integer division only (0/0 for the fractional part) — a fractional group divider adds
        // clock jitter, which on a self-clocked WS2812 stream is bit error.
        lcd_ll_set_group_clock_coeff(dev, static_cast<int>(kClockPreScale), 0, 0);
    }

    lcd_ll_reset(dev);
    lcd_ll_fifo_reset(dev);

    // No LCD interrupt is installed at all: esp_lcd needs one to dispatch its transaction queue, we
    // do not (the GDMA EOF is the only completion event this driver has). Mask the peripheral's
    // interrupts and clear anything the previous owner left pending, so a stale TRANS_DONE can't
    // fire into an ISR that isn't ours.
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_interrupt(dev, LCD_LL_EVENT_I80, false);
    }
    lcd_ll_clear_interrupt_status(dev, UINT32_MAX);

    lcd_ll_enable_rgb_mode(dev, false);        // i80 (command/data) mode, not RGB timing mode
    lcd_ll_enable_color_convert(dev, false);   // no RGB/YUV conversion — the frame is raw slot words
    lcd_ll_set_dma_read_stride(dev, st->busWidth);   // bytes the FIFO pulls per bus word
    lcd_ll_set_data_wire_width(dev, st->busWidth);   // data lines actually driven
    // "output always on" is what makes the data-phase length come from the DMA chain rather than a
    // cycle count — the property this whole backend is built on (esp_lcd_panel_io_i80.c:226).
    lcd_ll_enable_output_always_on(dev, true);
    lcd_ll_set_swizzle_mode(dev, LCD_LL_SWIZZLE_AB2BA);  // mode select only; the swizzle stays OFF below
    lcd_ll_enable_swizzle(dev, false);              // byte order as encoded — ParallelSlots writes bus words directly
    lcd_ll_reverse_dma_data_bit_order(dev, false);  // bit L of the word IS data line L (the encoder's contract)
    lcd_ll_swap_dma_data_byte_order(dev, false);

    lcd_ll_set_pixel_clock_prescale(dev, prescale);
    lcd_ll_set_clock_idle_level(dev, false);   // WR rests LOW, like the data lines (pclk_idle_low)
    lcd_ll_set_pixel_clock_edge(dev, false);   // data latched on the rising edge
    // DC is a peripheral-mandated line the strands ignore; park it LOW in every phase so it cannot
    // toggle a neighbouring strand if a board wires it to one.
    lcd_ll_set_dc_level(dev, /*idle=*/false, /*cmd=*/false, /*dummy=*/false, /*data=*/false);
    return true;
}

// Route the peripheral's signals onto real pins through the GPIO matrix.
//
// **We route only what a strand actually reads, which for WS2812 is the data lines and nothing else.**
// The GPIO matrix is a routing fabric, not a broadcast: a peripheral signal that is never connected to
// a pad simply stays inside the peripheral. So the lanes the board doesn't use, and the two bus control
// lines, cost zero GPIOs:
//
//   - **Spare data lanes.** The peripheral clocks all 8/16 lines whatever the pin count, but the ones
//     past `laneCount` go nowhere. (`esp_lcd` cannot do this — it rejects an NC data pin, which is why
//     it must park spares on a real "ghost" GPIO. Owning the routing is what removes that tax.)
//   - **DC.** An LCD panel needs it to separate command from data; WS2812 has no such concept, and
//     configureBus() nails DC to a constant level in every phase. It emits nothing a strand could read.
//   - **WR.** The peripheral must still GENERATE it — it is the pixel clock that shifts each bus word
//     out, and it drives the '595 shift clock — but only a shift register consumes it. WS2812 is
//     self-clocked, so in direct mode the strips ignore WR entirely and it needs no pad.
//
// Hence: dcGpio is never routed, and wrGpio is routed ONLY when a '595 expander needs the shift clock
// on a pin (`routeWr`). A direct-mode board therefore spends its GPIOs on strands alone — the same
// budget hpwit's hand-rolled driver has always had, and the reason an LCD-derived driver looked two
// pins more expensive than it is.
void configureGpio(MoonI80State* st, const uint16_t* dataPins, uint8_t laneCount, uint16_t wrGpio,
                   bool routeWr) {
    const uint8_t n = laneCount < 16 ? laneCount : 16;
    for (size_t i = 0; i < laneCount; i++) {
        gpio_func_sel(static_cast<gpio_num_t>(dataPins[i]), PIN_FUNC_GPIO);
        esp_rom_gpio_connect_out_signal(dataPins[i], soc_lcd_i80_signals[kBusId].data_sigs[i],
                                        false, false);
        // MAX drive strength (~40 mA) on every routed pin — data AND the '595 latch (which rides a data
        // lane). Strong pads = sharp edges, which is what a 74HCT595 needs to sample cleanly at the
        // shift-clock rate over real strand wiring: marginal strands (long runs, capacitive load) lose the
        // most signal margin, and the strongest edge buys it back. hpwit sets GPIO_DRIVE_CAP_3 on all
        // '595 pins for the same reason.
        gpio_set_drive_capability(static_cast<gpio_num_t>(dataPins[i]), GPIO_DRIVE_CAP_3);
        if (i < n) st->routedPins[i] = dataPins[i];   // recorded for teardown (see destroyState)
    }
    st->routedPinCount = n;
    if (routeWr) {
        gpio_func_sel(static_cast<gpio_num_t>(wrGpio), PIN_FUNC_GPIO);
        esp_rom_gpio_connect_out_signal(wrGpio, soc_lcd_i80_signals[kBusId].wr_sig, false, false);
        gpio_set_drive_capability(static_cast<gpio_num_t>(wrGpio), GPIO_DRIVE_CAP_3);   // SRCLK edge
        st->routedWrGpio = wrGpio;
    }
}

// GDMA channel + descriptor chain. Replicates lcd_i80_init_dma_link (esp_lcd_panel_io_i80.c:670-712)
// with two deliberate differences, both central to this backend:
//
//  1. **check_owner = false.** esp_lcd leaves owner-checking ON, which makes gdma_link_mount_buffers
//     walk from index 0 and refuse at the first descriptor the DMA still owns — the `lli full
//     need=N avail=M` failure that makes the esp_lcd path unusable with a large ('595-expanded)
//     frame. We own the chain outright and rebuild it from scratch on every transmit, so the check
//     protects nothing and only fails. (hpwit's S3 driver disables it for the same reason.)
//  2. The completion callback is the GDMA's own on_trans_eof, not an LCD interrupt.
bool initDma(MoonI80State* st, size_t bufferBytes) {
    gdma_channel_alloc_config_t chanCfg = {};
    // The S3's LCD hangs off the AHB GDMA, the P4's off the AXI one, and the descriptor alignment
    // differs with the bus. Same selection esp_lcd makes (esp_lcd_panel_io_i80.c:19-27), including
    // its `defined(...) &&` guard — on a chip with no AXI GDMA the AXI symbol is simply absent, and
    // an unguarded `== SOC_GDMA_BUS_AXI` would compare against the preprocessor's 0 and match.
#if defined(SOC_GDMA_BUS_AXI) && (SOC_GDMA_TRIG_PERIPH_LCD0_BUS == SOC_GDMA_BUS_AXI)
    constexpr size_t kDescAlign = 8;
    if (gdma_new_axi_channel(&chanCfg, &st->dma, nullptr) != ESP_OK) return false;
#else
    constexpr size_t kDescAlign = 4;
    if (gdma_new_ahb_channel(&chanCfg, &st->dma, nullptr) != ESP_OK) return false;
#endif
    if (gdma_connect(st->dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0)) != ESP_OK) return false;

    gdma_strategy_config_t strategy = {};
    strategy.auto_update_desc = true;
    strategy.owner_check = false;   // see (1) above — we own the chain
    if (gdma_apply_strategy(st->dma, &strategy) != ESP_OK) return false;

    gdma_transfer_config_t transfer = {};
    transfer.max_data_burst_size = 64;   // the burst esp_lcd's callers ask for; keeps PSRAM reads efficient
    transfer.access_ext_mem = true;      // the frame usually lives in PSRAM (LCD_CAM GDMA reaches it)
    if (gdma_config_transfer(st->dma, &transfer) != ESP_OK) return false;

    size_t intAlign = 0, extAlign = 0;
    if (gdma_get_alignment_constraints(st->dma, &intAlign, &extAlign) != ESP_OK) return false;
    const size_t bufAlign = intAlign > extAlign ? intAlign : extAlign;

    gdma_link_list_config_t linkCfg = {};
    linkCfg.item_alignment = kDescAlign;
    linkCfg.num_items = esp_dma_calculate_node_count(bufferBytes, bufAlign, kDmaNodeMaxBytes);
    linkCfg.flags.check_owner = false;   // see (1)
    if (gdma_new_link_list(&linkCfg, &st->link) != ESP_OK) return false;

    gdma_tx_event_callbacks_t cbs = {};
    cbs.on_trans_eof = moonI80EofCb;
    return gdma_register_tx_event_callbacks(st->dma, &cbs, st) == ESP_OK;
}

// Allocate one DMA-capable frame buffer, honouring the GDMA's alignment constraints (both the
// address and the length must be a cache-line multiple on the P4 / on PSRAM, or the cache
// write-back before a transfer would touch neighbouring allocations).
uint8_t* allocFrame(MoonI80State* st, size_t bufferBytes, bool psram) {

    size_t intAlign = 0, extAlign = 0;
    gdma_get_alignment_constraints(st->dma, &intAlign, &extAlign);
    const size_t align = psram ? extAlign : intAlign;
    const uint32_t caps = MALLOC_CAP_8BIT | MALLOC_CAP_DMA
                        | (psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
    // Round the size up to the alignment too — gdma_link_mount_buffers checks the LENGTH against the
    // same constraint, and a short tail would fail the mount. The pad is zeroed, so it just extends
    // the frame's trailing latch gap (the lines already idle LOW there).
    const size_t size = align > 1 ? ((bufferBytes + align - 1) / align) * align : bufferBytes;
    return static_cast<uint8_t*>(heap_caps_aligned_calloc(align ? align : 4, 1, size, caps));
}

// One peripheral + GDMA chain + frame buffer(s). `wantSecond` allocates the async double-buffer's
// second frame buffer (best-effort — null if it won't fit); false allocates buffer 0 only. Shared by
// the runtime init and the loopback (which passes false — one transfer).
MoonI80State* createState(const uint16_t* dataPins, uint8_t laneCount,
                          uint16_t wrGpio, size_t bufferBytes, bool wantSecond,
                          uint8_t clockMultiplier) {
    auto* st = new (std::nothrow) MoonI80State();
    if (!st) return nullptr;

    // Bus width is power-of-two only (8 or 16), derived from the lane count: ≤8 → 8, 9..16 → 16.
    st->busWidth = laneCount <= 8 ? 8 : 16;

    // A '595 expander shifts each WS2812 slot out over `clockMultiplier` bus words, so the bus must
    // clock proportionally faster to keep the slot inside the WS2812 bit window. See kShiftClockDivDefault.
    // Shift mode: the '595 SRCLK = the 80 MHz bus resolution / the runtime shiftClockDiv (default 4 =
    // 20 MHz). initPeripheral recomputes the exact prescale from the clock tree, so this only needs
    // to be the intended rate. Direct mode is unaffected (kPclkHz).
    const uint32_t pclkHz = (clockMultiplier > 1) ? (kShiftBusResolutionHz / g_shiftClockDiv) : kPclkHz;
    if (!initPeripheral(st, pclkHz) || !initDma(st, bufferBytes)) {
        destroyState(st);
        return nullptr;
    }
    // WR reaches a pad only when a '595 needs it as the shift clock; a direct-mode strand ignores it,
    // and DC never reaches a pad at all. See configureGpio.
    configureGpio(st, dataPins, laneCount, wrGpio, /*routeWr=*/clockMultiplier > 1);

    st->done[0] = xSemaphoreCreateBinary();
    st->wireFree = xSemaphoreCreateBinary();
    if (!st->done[0] || !st->wireFree) {
        destroyState(st);
        return nullptr;
    }

    // PSRAM first: the LCD_CAM GDMA bursts straight out of PSRAM (access_ext_mem), the 2.67 MHz
    // pixel clock is easy to sustain from it, and keeping a large frame out of the scarce internal
    // DMA heap is the right trade. Internal is the fallback when PSRAM is absent or full.
    //
    // **Where the frame lives is decided by the PIXEL CLOCK, not by its size — measured on this
    // backend, board B, 2026-07-14, and it is the cleanest result of the whole investigation.**
    //
    // Same board, same PSRAM, same descriptor chain, same driver; the ONLY variable is the clock:
    //
    //   direct mode  (2.67 MHz pclk):  a PSRAM frame streams fine   — 2048 lights, 7,712 µs, driving
    //   shift mode   (26.67 MHz pclk): a PSRAM frame NEVER completes — at ANY size, 54 KB or 144 KB
    //
    // So it is not PSRAM, and it is not the frame size: **the S3's GDMA cannot sustain a PSRAM read at
    // the expander's clock.** A '595 is serial-in, so each WS2812 slot is shifted out over 8 bus words
    // and the bus must run 10× faster — which is exactly the rate PSRAM cannot feed.
    //
    // **This backend is what proved it, which is exactly why it was built.** The esp_lcd path failed
    // here with thousands of `lli full` descriptor-mount errors, and those pointed hard at esp_lcd's
    // own descriptor handling. This backend removes that mechanism entirely — we own the chain, mount
    // it once, owner-checking off — and the mount errors are GONE, yet the transfer still never
    // completes. So the `lli full` storm was a symptom, not the cause: the hypothesis is dead, killed
    // by a controlled experiment with a working control condition (direct mode, same PSRAM, drives
    // fine). That is the measurement the whole-frame phase exists to produce.
    //
    // Hence internal RAM first in shift mode, and PSRAM first otherwise. This is not a workaround
    // inherited from the sibling; it is what the measurement says. On this WHOLE-FRAME path it caps the
    // expander at what fits internal DMA RAM (~110 KB → ~96 lights/strand). Above that, the driver does
    // not use this path at all: it builds the STREAMING RING instead (moonI80Ws2812InitRing below), which
    // never materialises the frame — the DMA loops a small pool of internal buffers refilled by the CPU
    // as it drains them, so it never reads PSRAM at the expander's clock. See the ring section further
    // down; this whole-frame path remains the direct-mode path and the sub-96 shift path.
    //
    // PSRAM remains the fallback in shift mode: a frame too big for internal RAM still drives (badly)
    // rather than refusing to start, and the driver's dead-frame guard keeps a stalled bus from
    // starving the device.
    //
    // buf[0] is deliberately NOT reserve-guarded, unlike buf[1] below. The reserve protects the
    // WiFi/HTTP heap from an OPTIONAL allocation; buf[0] is the frame itself, so refusing it to keep
    // the reserve intact would decline to drive the LEDs at all — degrading the essential thing to
    // protect a nice-to-have, the inverse of the allocate-and-degrade policy.
    // **With a PIN EXPANDER there is NO PSRAM fallback: internal RAM or nothing.** A '595 clocks at
    // clockMultiplier x the pixel rate, and the LCD DMA cannot sustain PSRAM at that rate — measured on an
    // S3: a 256-light frame placed in PSRAM (0x3c...) reports "no LED output" and burns ~219 ms per tick
    // timing out, while the same frame in internal RAM (0x3f...) drives fine. So a PSRAM fallback does not
    // degrade, it WEDGES: busInit still returns true, the driver reports "driving N of M lights", and every
    // tick times out its wait and its wire-free token. Returning null instead surfaces the failure through
    // the normal path — and the real fallback for an oversize frame is the streaming RING, which never
    // needs the frame contiguous at all.
    //
    // Direct mode keeps PSRAM as its PRIMARY: one bus word per pixel clock is a rate PSRAM sustains, and
    // that is what lets a direct-mode board drive 16K lights it could never hold internally.
    const bool pinExpanderMode = clockMultiplier > 1;
    st->buf[0] = allocFrame(st, bufferBytes, /*psram=*/!pinExpanderMode);
    if (!st->buf[0] && !pinExpanderMode) st->buf[0] = allocFrame(st, bufferBytes, /*psram=*/false);
    if (!st->buf[0]) {
        destroyState(st);
        return nullptr;
    }
    st->cap = bufferBytes;

    // Second buffer for the async double-buffer, ONLY when asked. Allocate-and-degrade:
    // if it fits, arm double-buffer mode (buf[1] + its semaphore); if it doesn't, leave buf[1] null
    // and the driver runs single-buffer. The internal fallback additionally must leave HEAP_RESERVE
    // intact — the second buffer is a nice-to-have and must never eat the WiFi/HTTP reserve.
    //
    // **The reserve guards the INTERNAL attempt, whichever attempt that is.** The preference order
    // flips with the mode (shift wants internal first, direct wants PSRAM first), so binding the guard
    // to a fixed branch would put it on the wrong one half the time — in shift mode it would leave the
    // internal allocation unguarded (free to eat the WiFi/HTTP reserve) while pointlessly gating the
    // PSRAM retry on internal free space. A small lambda keeps the rule with the thing it guards.
    if (wantSecond) {
        st->done[1] = xSemaphoreCreateBinary();
        if (st->done[1]) {
            auto tryAlloc = [&](bool psram) -> uint8_t* {
                if (!psram
                    && heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
                           < bufferBytes + HEAP_RESERVE) {
                    return nullptr;   // internal, and it would eat the reserve — refuse
                }
                return allocFrame(st, bufferBytes, psram);
            };
            // Same rule as buf[0]: with a pin expander it is internal-or-nothing. A PSRAM buf[1] is worse than no
            // second buffer at all — tickAsync would alternate a working internal buf[0] with a PSRAM
            // buf[1] that never completes, giving exactly one frame at boot and then ~207 ms per tick
            // forever. Refusing it leaves buf[1] null, which the driver reads as "run single-buffered".
            st->buf[1] = tryAlloc(/*psram=*/!pinExpanderMode);
            if (!st->buf[1] && !pinExpanderMode) st->buf[1] = tryAlloc(/*psram=*/false);
            if (!st->buf[1]) {
                vSemaphoreDelete(st->done[1]);
                st->done[1] = nullptr;
            }
        }
    }
    return st;
}

// Program the chain and start the ONE gapless transaction. This is the whole point of the backend:
// esp_lcd's per-transaction reset happens exactly once here, at the START of the single transfer
// that carries the entire frame, never between chunks of it.
bool startTransfer(MoonI80State* st, uint8_t buffer, size_t bytes) {
    lcd_cam_dev_t* dev = st->hal.dev;

    // Write the encoded frame back from cache to physical memory — the GDMA reads DRAM/PSRAM, not
    // the CPU's cache. (esp_lcd does this inside tx_color, esp_lcd_panel_io_i80.c:576.)
    if (esp_cache_get_line_size_by_addr(st->buf[buffer]) > 0) {
        esp_cache_msync(st->buf[buffer], bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    // Mount the WHOLE frame in ONE call: the link list splits it across as many 4095-byte nodes as
    // it needs and chains them, so the DMA walks the frame end to end without CPU involvement.
    // mark_eof fires our completion callback on the last node; mark_final NULLs its next-pointer so
    // the DMA stops there instead of wrapping to the head.
    gdma_buffer_mount_config_t mount = {};
    mount.buffer = st->buf[buffer];
    mount.length = bytes;
    mount.flags.mark_eof = true;
    mount.flags.mark_final = GDMA_FINAL_LINK_TO_NULL;
    if (gdma_link_mount_buffers(st->link, 0, &mount, 1, nullptr) != ESP_OK) return false;

    // Data phase only: no command cycles, no dummy cycles, `data_cycles = 1` is a boolean ENABLE —
    // the peripheral clocks whatever the DMA chain feeds it and stops when the chain ends
    // ("Number of data phase cycles are controlled by DMA buffer length", esp_lcd_panel_io_i80.c:778).
    lcd_ll_set_phase_cycles(dev, /*cmd=*/0, /*dummy=*/0, /*data=*/1);
    lcd_ll_set_blank_cycles(dev, 1, 1);
    lcd_ll_reset(dev);
    lcd_ll_fifo_reset(dev);   // discard any FIFO residue from the previous frame

    // GDMA first: the LCD only starts consuming once data has reached its FIFO. The 1 µs settle is
    // esp_lcd's 4 µs (esp_lcd_panel_io_i80.c:793) shortened — at these pixel clocks (2.67-26.7 MHz,
    // vs the tens of MHz an LCD panel runs) the FIFO fills far faster than one word period, so 1 µs
    // is ample and keeps the inter-frame gap short. Skipping it entirely would risk the first word
    // clocking out of an empty FIFO.
    if (gdma_start(st->dma, gdma_link_get_head_addr(st->link)) != ESP_OK) return false;
    esp_rom_delay_us(1);
    lcd_ll_start(dev);
    return true;
}

// --- Ring mode ---------------------------------------------------------------------------------------

// Encode one ring slice into buffer `slot`: rows [firstRow, firstRow + count) of the frame, straight into
// the internal buffer the DMA is about to read.
//
// **A ring buffer holds rows and nothing else.** There is no latch pad here — the WS2812 reset comes from
// STOPPING the peripheral and letting the lines idle LOW (moonI80EofCb), never from a pad inside a
// circulating buffer, and the buffers are allocated rows-only to match. So the seam is passed
// `closeFrame=false` for every slice: a pad written here would land past the allocation.
//
// Cache sync is a no-op for internal RAM (line size 0), but kept for symmetry with the whole-frame path
// and correctness if a ring buffer ever lands cache-mapped.
//
// IRAM_ATTR: this is the ISR encode chain's entry (moonI80EofCb → here → the domain encode via
// MM_RAMFUNC), and the WHOLE chain lives in IRAM for throughput, not just flash-write safety: flash-
// resident code shares one instruction cache between both cores, and the render core's effect/HTTP churn
// evicts this path between EOF firings — every refill then pays flash refetch on top of the encode.
void IRAM_ATTR encodeRingSlice(MoonI80State* st, uint8_t slot, uint32_t firstRow, uint32_t count) {
    // Hand the encoder the buffer-lifecycle fact its prefill-skip hangs on, and consume it: after this
    // call the buffer's constants are laid (or were already), until a memset invalidates them again.
    const bool needsPrefill = st->bufNeedsPrefill[slot];
    st->bufNeedsPrefill[slot] = false;
    st->encode(st->encodeUser, st->ring[slot], firstRow, count, /*closeFrame=*/false, needsPrefill);
    // ringCacheLine is a per-pool constant hoisted to createRingState: the per-refill line-size query was
    // a flash call inside the ISR (illegal on a cache-safe channel, and a needless icache miss before).
    if (st->ringCacheLine > 0) {
        esp_cache_msync(st->ring[slot], static_cast<size_t>(count) * st->ringRowBytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
}

// Fill buffer `slot` with slice `sliceIdx` — the ONE place the slice-fill rule lives, called by both the
// prime (buffer b = slice b, one lap) and the EOF-ISR refill (slot = s % ringBufs, slice s, any lap). A
// buffer holding a real slice gets its rows encoded (tail zeroed on the short last slice, flagged for
// re-prefill); a buffer PAST the frame's slices is fully zeroed so the loop clocks clean LOW, and the
// FIRST past-frame buffer (sliceIdx == nSlices) carries the frame-closing latch word at its head — one
// more latch so the register's final slot presents before the idle-LOW reset (see MoonI80EncodeFn's
// close call; pinned by the loopback bit-verify). IRAM: reached from the ISR encode chain.
// Returns TRUE when it ran a REAL encode (not a zero-fill) — the ISR times only those (the `ea` pace
// number is "average real refill cost", which must beat the slice deadline; cheap zero-fills would dilute it).
bool IRAM_ATTR fillSlice(MoonI80State* st, uint8_t slot, uint32_t sliceIdx) {
    const uint32_t firstRow = sliceIdx * st->rowsPerBuf;
    if (firstRow < st->totalRows) {
        uint32_t count = st->rowsPerBuf;
        bool shortSlice = false;
        if (firstRow + count >= st->totalRows) {
            count = st->totalRows - firstRow;
            if (count < st->rowsPerBuf) {
                std::memset(st->ring[slot] + static_cast<size_t>(count) * st->ringRowBytes, 0,
                            static_cast<size_t>(st->rowsPerBuf - count) * st->ringRowBytes);
                shortSlice = true;
            }
        }
        encodeRingSlice(st, slot, firstRow, count);
        // AFTER the encode (it consumed this use's prefill flag): the tail memset erased those rows'
        // constants, so the buffer's next full use must re-prefill.
        if (shortSlice) st->bufNeedsPrefill[slot] = true;
        return true;
    }
    std::memset(st->ring[slot], 0, static_cast<size_t>(st->rowsPerBuf) * st->ringRowBytes);
    st->bufNeedsPrefill[slot] = true;   // zero-filled: constants gone until re-prefilled
    if (sliceIdx == st->nSlices)
        st->encode(st->encodeUser, st->ring[slot], st->totalRows, 0,
                   /*closeFrame=*/true, /*needsPrefill=*/false);
    return false;
}

// THE PRIME-vs-DRAIN BARRIER. `done` is written-gated (the ISR's frame-end block), so it leads the wire's
// actual drain by most of the frame: the lapping frame's LAST slices (30..37 at 48x256) live in the FIRST
// buffers (s % ringBufs = 0..7), which are exactly the buffers the next prime rewrites first — so priming
// before the wire drains repaints the bottom rows with next-frame bytes, and NO counter sees it (ld only
// watches ISR refills). lastStopUs is the deterministic wire end (armUs + the whole frame's wire duration,
// tail flush included), set at completion; holding the prime until then closes the race at its only entry
// point. Normally waits ~0 (the render tick + snapshot already elapsed most of the wire); back-to-back
// frames busy-wait the small remainder, same bounded pattern as the arm's kResetLowUs guard. Do NOT
// "fix" this by drain-gating `done` in the ISR instead — that deadlocks (the frontier-halted DMA fires no
// completing EOF; bench-measured as flicker-then-"no LED output").
void waitWireDrained(MoonI80State* st) {
    if (st->lastStopUs == 0) return;   // first-ever frame: no prior wire to drain
    const int64_t now = esp_timer_get_time();
    if (now < st->lastStopUs) esp_rom_delay_us(static_cast<uint32_t>(st->lastStopUs - now));
}

// Prime ring buffers [bufLo, bufHi) — each buffer is INDEPENDENT (buffer b holds slice b's rows), so two
// cores prime DISJOINT ranges concurrently (the dual-core fork-join). No latch pad: the reset comes from
// the stop, never a pad in a node. Each caller (either fork half, or the serial whole-pool call) takes the
// wire barrier itself — both halves waiting out the same deadline in parallel is idempotent and keeps the
// barrier a per-call contract rather than a caller obligation.
void primeRingRange(MoonI80State* st, uint8_t bufLo, uint8_t bufHi) {
    waitWireDrained(st);
    if (bufHi > st->ringBufs) bufHi = st->ringBufs;
    for (uint8_t primed = bufLo; primed < bufHi; primed++)
        fillSlice(st, primed, primed);   // first lap: buffer index IS the slice index
}

// Arm the primed ring: peripheral setup, the timed WS2812-reset guard, the oracle epoch, gdma_start.
// Every buffer must be primed (primeRingRange over the whole pool — serial or fork-joined) BEFORE this
// runs; the caller owns that ordering (the driver's join is the fence).
bool armRingTransfer(MoonI80State* st) {
    lcd_cam_dev_t* dev = st->hal.dev;
    st->drainCount = 0;
    st->busy = true;
    // Priming wrote slices 0..ringBufs-1 (real ones encoded, the rest zero-filled) — the batch refill's
    // cursor continues from there.
    st->lastWrittenSlice = st->ringBufs - 1u;

    // LAPPING: rebuild the chain LINEAR and terminate it at the prime frontier before every arm. Buffer b
    // links to b+1 for the whole pool, and the last primed buffer (ringBufs-1) gets a NULL terminator — so
    // the freshly-armed chain ENDS exactly where written data ends, and the ISR's moving frontier extends
    // it slice by slice from there. Re-linking each frame (not once at mount) is what lets the terminator
    // travel: the previous frame left the NULL wherever its last refill put it, and this restores the
    // known start state. Prime-only chains are self-terminated at mount and need no re-link (their frontier
    // never moves).
    if (st->nSlices > st->ringBufs) {
        // The previous frame's DMA halted itself at the frontier NULL; force it fully stopped before we
        // rewrite its descriptors so no in-flight prefetch reads a half-edited link. gdma_start(head)
        // below re-arms from the head cleanly. (First arm: the channel is idle already — a no-op stop.)
        gdma_stop(st->dma);
        for (uint8_t b = 0; b + 1u < st->ringBufs; b++)
            gdma_link_concat(st->link, ringTailNode(st, b), st->link, st->bufLastNode[b + 1u]);
        gdma_link_concat(st->link, ringTailNode(st, st->ringBufs - 1u), nullptr, 0);   // frontier NULL
    }

    lcd_ll_set_phase_cycles(dev, /*cmd=*/0, /*dummy=*/0, /*data=*/1);
    lcd_ll_set_blank_cycles(dev, 1, 1);
    lcd_ll_reset(dev);
    lcd_ll_fifo_reset(dev);

    // Guarantee the WS2812 reset: hold the arm until the strand has idled LOW for >=kResetLowUs since the
    // last frame stopped. At normal frame rates the render loop's own inter-frame gap already exceeds this
    // (frames are ms apart), so this waits ZERO in the common case — it only busy-waits the tiny remainder
    // when frames come back-to-back at small ringRows, exactly the case whose short tail buffer would
    // otherwise read as a PAUSE not a reset (the frozen-frame wedge). Sizing the reset by TIME here, not by
    // tail-buffer count, is what makes small ringRows (the small-pool 48x256 path) render at all.
    if (st->lastStopUs != 0) {
        const int64_t lowSoFar = esp_timer_get_time() - st->lastStopUs;
        if (lowSoFar < kResetLowUs) esp_rom_delay_us(static_cast<uint32_t>(kResetLowUs - lowSoFar));
    }

    st->txStartUs[0] = esp_timer_get_time();
    st->armUs = st->txStartUs[0];   // the clock oracle's epoch: drain position = (now − armUs) / sliceNs
    if (gdma_start(st->dma, gdma_link_get_head_addr(st->link)) != ESP_OK) {
        st->busy = false;
        return false;
    }
    esp_rom_delay_us(1);
    lcd_ll_start(dev);
    return true;
}

// The serial combo (prime everything, then arm) — the single-core path, and what the dual-core driver
// falls back to when its helper is unavailable.
bool startRingTransfer(MoonI80State* st) {
    primeRingRange(st, 0, st->ringBufs);
    return armRingTransfer(st);
}


// GDMA channel + the link list the ring circulates. Mirrors initDma (channel alloc / connect / strategy /
// transfer / EOF callback); the chain itself is described where it is mounted, in createRingState.
bool initRingDma(MoonI80State* st) {
    gdma_channel_alloc_config_t chanCfg = {};
    // The lapping deadline race is run at interrupt-dispatch granularity, so the ring's interrupt gets
    // hpwit's setup: LEVEL-3 priority (a render-thread level-1/2 interrupt can't delay the refill) and a
    // cache-safe (ESP_INTR_FLAG_IRAM) registration so a SPI-flash write (persistence, OTA) DELAYS nothing —
    // the whole handler chain is IRAM-resident (moonI80EofCb + encodeRingSlice + the MM_RAMFUNC encoders),
    // which is what makes this registration legal. CONFIG_GDMA_CTRL_FUNC_IN_IRAM covers the gdma_stop the
    // ISR calls at frame end.
    chanCfg.intr_priority = 3;
    chanCfg.flags.isr_cache_safe = true;
#if defined(SOC_GDMA_BUS_AXI) && (SOC_GDMA_TRIG_PERIPH_LCD0_BUS == SOC_GDMA_BUS_AXI)
    constexpr size_t kDescAlign = 8;
    if (gdma_new_axi_channel(&chanCfg, &st->dma, nullptr) != ESP_OK) return false;
#else
    constexpr size_t kDescAlign = 4;
    if (gdma_new_ahb_channel(&chanCfg, &st->dma, nullptr) != ESP_OK) return false;
#endif
    if (gdma_connect(st->dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0)) != ESP_OK) return false;

    gdma_strategy_config_t strategy = {};
    // auto_update_desc = FALSE, matching hpwit's proven S3 LCD_CAM ring (I2SClocklessVirtualLedDriver).
    // With it TRUE the GDMA writes back / clears each descriptor's owner bit as it consumes the node; on a
    // chain whose buffers are refilled behind the DMA (the ring), that leaves the engine gating on owner
    // bits it cleared and halting POLITELY (no descriptor error) once it reaches a node it now thinks the
    // CPU owns — the exact "stops cleanly after N EOFs, descErr=0" symptom measured at ≥192 lights. The ISR
    // refill rewrites buffer CONTENTS, never the descriptor, so it never re-arms an owner bit — hpwit's fix
    // is to never let the hardware clear them (auto_update_desc off) rather than re-arm per lap. owner_check
    // stays off (we never want an owner gate at all).
    strategy.auto_update_desc = false;
    strategy.owner_check = false;
    if (gdma_apply_strategy(st->dma, &strategy) != ESP_OK) return false;

    gdma_transfer_config_t transfer = {};
    transfer.max_data_burst_size = 64;
    transfer.access_ext_mem = false;   // ring buffers are INTERNAL — the whole point (no PSRAM at the shift clock)
    if (gdma_config_transfer(st->dma, &transfer) != ESP_OK) return false;

    size_t intAlign = 0, extAlign = 0;
    if (gdma_get_alignment_constraints(st->dma, &intAlign, &extAlign) != ESP_OK) return false;

    st->nSlices = (st->totalRows + st->rowsPerBuf - 1) / st->rowsPerBuf;

    // **FRONTIER-TERMINATED chain (hpwit's early self-terminating splice, made continuous).** The pool is a
    // fixed set of exactly ringBufs node-runs, one per physical buffer, mounted here LINEAR (buffer b → b+1)
    // with a spare tail the arm re-links. It is NOT a loop: the chain always ENDS at a NULL terminator that
    // sits on the last-WRITTEN slice's node — armRingTransfer plants it at the prime edge, and the EOF ISR
    // advances it slice by slice as it refills (moonI80EofCb). So the DMA can never run past written data:
    // reach the end of the encoded slices and it HALTS at the frontier NULL (a stalled refill = a clean
    // hardware stop, lines idle LOW, the strand latches a partially-updated frame), instead of the earlier
    // LOOPING chain's failure — a circulating DMA that a cache-off refill defer let lap the pool and
    // re-clock stale slices as max-bright garbage (the WiFi white-flash). This is also why the original
    // LINEAR nSlices-node chain stalled at ≥192 lights: it revisited BUFFERS with no live terminator to
    // advance, so the DMA reached a node it could not pass and halted mid-frame (descErr=0, not corruption).
    // A moving frontier is the fix that a fixed terminator (prime-only) and a bare loop (the old lapping
    // path) both lacked. Every node is ROWS-ONLY (rowsPerBuf × rowBytes, NO latch pad): the bit stream must
    // flow CONTINUOUSLY buffer-to-buffer (a trailing LOW pad on any mid-chain buffer is a ≥300 µs gap that
    // latches the strand — the scrambled-image bug). The WS2812 reset gap is NOT in a buffer; it is the
    // idle-LOW after the frontier halt / frame-end stop, held ≥kResetLowUs by the next arm.
    const size_t rowsOnlyBytes = static_cast<size_t>(st->rowsPerBuf) * st->ringRowBytes;
    const size_t itemsPerBuf = esp_dma_calculate_node_count(rowsOnlyBytes, intAlign, kDmaNodeMaxBytes);
    st->itemsPerBuf = static_cast<uint8_t>(itemsPerBuf);   // the ISR splices the self-terminating NULL by node index
    // One extra node per buffer when the inter-buffer zero-pad is on (each pad node re-mounts the SAME
    // shared zero block; zeroPadBytes ≤ kDmaNodeMaxBytes by the kRingPadMaxUs bound, so one node each).
    const size_t padItems = st->zeroPad ? st->ringBufs : 0;
    const size_t numItems = itemsPerBuf * st->ringBufs + padItems;

    st->linkItemCap = numItems;

    gdma_link_list_config_t linkCfg = {};
    linkCfg.item_alignment = kDescAlign;
    linkCfg.num_items = numItems;
    linkCfg.flags.check_owner = false;
    if (gdma_new_link_list(&linkCfg, &st->link) != ESP_OK) return false;

    gdma_tx_event_callbacks_t cbs = {};
    cbs.on_trans_eof = moonI80EofCb;
    cbs.on_descr_err = moonI80DescErrCb;   // B1-DISCRIMINATOR (diagnostic): catch the silent descriptor-fetch halt
    return gdma_register_tx_event_callbacks(st->dma, &cbs, st) == ESP_OK;
}

// Bring up the peripheral + N internal ring buffers + the linear chain. The peripheral setup mirrors
// createState (same clock, GPIO routing, DC/WR handling) — only the DMA + buffers differ. Returns null
// (and the caller falls back to the whole-frame path) if any internal buffer or the chain won't fit.
MoonI80State* createRingState(const uint16_t* dataPins, uint8_t laneCount, uint16_t wrGpio,
                              size_t rowBytes, uint32_t totalRows,
                              uint32_t rowsPerBuf, uint8_t ringBufs, uint8_t padUs,
                              uint8_t clockMultiplier, MoonI80EncodeFn encode, void* user) {
    auto* st = new (std::nothrow) MoonI80State();
    if (!st) return nullptr;
    st->isRing = true;
    st->busWidth = laneCount <= 8 ? 8 : 16;
    st->ringRowBytes = rowBytes;
    st->totalRows = totalRows;
    st->padUs = padUs;
    // **One ring buffer = ONE DMA descriptor node — hpwit's structural rule, enforced here.** His driver's
    // buffer struct IS a single lldesc_t (I2SClocklessVirtualLedDriver.h ~438, buffer size capped to one
    // descriptor's max), so his splice/terminate logic never meets a buffer that spans nodes. Ours did:
    // a buffer > kDmaNodeMaxBytes spans 2+ nodes, and the per-buffer mount + mark_final + re-link interaction
    // then breaks the self-terminating NULL (bench: rows=8, ipb=2 — the NULL sat correctly on node 33 yet
    // the DMA stopped at ~node 25; with ipb=1 the identical logic is clean). Clamping rowsPerBuf so the
    // buffer fits one node deletes that bug class instead of patching it — and small buffers are the
    // small-pool 48x256 direction anyway. The clamp is a floor of 1 row (a single row larger than a node
    // cannot ring at all; init fails downstream and the driver falls back to whole-frame).
    const uint32_t maxRowsPerNode = rowBytes ? static_cast<uint32_t>(kDmaNodeMaxBytes / rowBytes) : 1u;
    const uint32_t rowsClamped = rowsPerBuf > maxRowsPerNode ? (maxRowsPerNode ? maxRowsPerNode : 1u)
                                                             : rowsPerBuf;
    // The geometry, before initRingDma — it derives nSlices and the descriptor-pool size from both.
    st->rowsPerBuf = rowsClamped;
    st->ringBufs = ringBufs;

    st->encode = encode;
    st->encodeUser = user;
    st->cap = st->rowsPerBuf * rowBytes;   // reported buffer capacity (one ring buffer — ROWS ONLY, no pad)

    // Shift mode: the '595 SRCLK = the 80 MHz bus resolution / the runtime shiftClockDiv (default 4 =
    // 20 MHz). initPeripheral recomputes the exact prescale from the clock tree, so this only needs
    // to be the intended rate. Direct mode is unaffected (kPclkHz).
    const uint32_t pclkHz = (clockMultiplier > 1) ? (kShiftBusResolutionHz / g_shiftClockDiv) : kPclkHz;
    // The clock oracle's tick: one slice's exact wire duration in ns. The bus clocks busWidth/8 bytes per
    // pclk, so ns = bytes × 8e9 / (busWidth × pclkHz); the inter-buffer pad (below) extends every slice's
    // slot by padUs. Exact integer math — the oracle's only error source is esp_timer resolution (µs).
    const uint64_t sliceWireNs = (static_cast<uint64_t>(st->rowsPerBuf) * rowBytes * 8u * 1'000'000'000ull)
                                 / (static_cast<uint64_t>(st->busWidth) * pclkHz);
    st->sliceNs = static_cast<uint32_t>(sliceWireNs);   // + the pad below, once it is actually mounted
    // The geometry initRingDma sizes the pool from — computed here too because the pad decision needs it.
    st->nSlices = (st->totalRows + st->rowsPerBuf - 1u) / st->rowsPerBuf;
    // The shared zero-pad block (lapping only — prime-only has no refill deadline to stretch). padUs of
    // bus time in bytes, rounded down to an 8-byte-aligned DMA length; one block, referenced by every pad
    // node. Zeros on the '595 bus are a strand-level pause (the row boundary before the pad has already
    // presented the pulse tail, all lanes LOW), and padUs ≤ kRingPadMaxUs keeps the pause far under the
    // ~300 µs latch threshold.
    if (padUs > 0 && st->nSlices > st->ringBufs) {
        const uint64_t padBytes64 = (static_cast<uint64_t>(padUs) * pclkHz * st->busWidth) / (8u * 1'000'000ull);
        st->zeroPadBytes = static_cast<size_t>(padBytes64) & ~size_t{7};
        if (st->zeroPadBytes > 0) {
            st->zeroPad = allocFrame(st, st->zeroPadBytes, /*psram=*/false);
            if (!st->zeroPad) {   // pad requested but unfittable: fail loud, caller falls back
                ESP_LOGE(MOON_I80_TAG, "ring init failed: zero-pad alloc (%u B)", (unsigned)st->zeroPadBytes);
                destroyState(st);
                return nullptr;
            }
            // The oracle's tick includes the pad's EXACT wire time (the mounted bytes, not the requested
            // µs — they differ by the alignment round-down).
            st->sliceNs += static_cast<uint32_t>(
                (static_cast<uint64_t>(st->zeroPadBytes) * 8u * 1'000'000'000ull)
                / (static_cast<uint64_t>(st->busWidth) * pclkHz));
        } else {
            st->zeroPadBytes = 0;
        }
    }
    if (!initPeripheral(st, pclkHz)) {
        ESP_LOGE(MOON_I80_TAG, "ring init failed: peripheral (LCD_CAM) setup");
        destroyState(st);
        return nullptr;
    }
    if (!initRingDma(st)) {
        ESP_LOGE(MOON_I80_TAG, "ring init failed: GDMA channel/descriptor setup");
        destroyState(st);
        return nullptr;
    }
    configureGpio(st, dataPins, laneCount, wrGpio, /*routeWr=*/clockMultiplier > 1);

    st->done[0] = xSemaphoreCreateBinary();   // the render thread's frame-complete wait (given on the last slice)
    if (!st->done[0]) {
        ESP_LOGE(MOON_I80_TAG, "ring init failed: done semaphore");
        destroyState(st);
        return nullptr;
    }

    // N internal ring buffers, each one slice + the latch pad (the LAST slice appends the pad; sizing
    // every buffer to hold it keeps them uniform and lets any buffer be the last one).
    const size_t bufBytes = static_cast<size_t>(st->rowsPerBuf) * rowBytes;
    for (uint8_t i = 0; i < st->ringBufs; i++) {
        st->ring[i] = allocFrame(st, bufBytes, /*psram=*/false);
        if (!st->ring[i]) {
            ESP_LOGE(MOON_I80_TAG, "ring init failed: buffer %u/%u alloc (%u B, free DMA %u B)",
                     (unsigned)i, (unsigned)st->ringBufs, (unsigned)bufBytes,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            destroyState(st);
            return nullptr;
        }
        st->bufNeedsPrefill[i] = true;   // fresh (zeroed) buffer: no constants laid yet
    }
    // Cache-line size is a per-pool constant (all buffers come from the same internal-DMA heap) — hoisted
    // here so the refill never makes the (flash-resident) query inside the now cache-safe ISR.
    st->ringCacheLine = esp_cache_get_line_size_by_addr(st->ring[0]);

    // Mount the LOOPING chain: exactly ringBufs node-runs, node i → ring[i], the LAST looping back to the
    // HEAD (GDMA_FINAL_LINK_TO_HEAD) so the DMA circles the buffer pool forever (hpwit's proven S3 ring).
    // Every node carries mark_eof → every drain fires the refill ISR (moonI80EofCb), which re-encodes the
    // drained buffer with the next unencoded slice and stops the engine on the drain counter. Each node is
    // mounted at the FULL slice size (rows + latch pad): the frame's real last slice lands in whatever buffer
    // the loop is on when the drain count is reached, so any buffer must be able to carry the pad. The pad
    // after a NON-last slice is a ≥300 µs LOW gap that would latch the strand mid-frame — so encodeRingSlice
    // zeroes the pad on non-last refills and only the last slice writes the latch word (unchanged).
    // SELF-TERMINATING CHAIN, mounted at BUILD time (hpwit's mechanism, our esp_lcd link API — the clean
    // version). A PRIME-ONLY geometry (nSlices <= ringBufs) has a fixed frame length, so its terminator is
    // fixed too: buffer `nSlices` (the zero reset-tail just past the last real slice) is mounted with
    // GDMA_FINAL_LINK_TO_NULL, so the DMA clocks [slice 0 .. slice nSlices-1][one zero tail] and
    // SELF-TERMINATES — no mid-frame gdma_stop racing the prefetcher (the small-ringRows flicker), and NO
    // runtime gdma_link_concat splice (which raced the still-walking DMA and was index-fragile: last
    // session's ld=5 wedge). The chain is self-terminating from creation; re-arm is a plain gdma_start(head).
    // A LAPPING geometry (nSlices > ringBufs, e.g. 256 lights in a small pool) re-reads buffers, so it can
    // have no fixed terminator here. It is mounted LINEAR (buffer b → b+1, the last buffer's own tail left
    // DEFAULT-linked and harmless — armRingTransfer re-links the whole chain and plants the frontier NULL
    // at the prime edge before every frame, and the ISR then advances that NULL slice by slice). This is
    // hpwit's early self-terminating splice, made continuous: the chain always ends at written data, so a
    // stalled refill halts the DMA at the frontier instead of lapping the pool into stale slices.
    const bool primeOnly = st->nSlices <= st->ringBufs;
    const uint32_t termBuf = primeOnly
        ? (st->nSlices < st->ringBufs ? st->nSlices : st->ringBufs - 1u)   // the zero reset-tail buffer
        : st->ringBufs;   // sentinel "none" for the lapping case (no buffer gets LINK_TO_NULL)
    const size_t rowsOnly = static_cast<size_t>(st->rowsPerBuf) * rowBytes;   // node length: rows, NO pad
    // Mount up to AND INCLUDING the terminator buffer, then STOP. gdma_link_mount_buffers is called one
    // buffer at a time, and each call re-links the PREVIOUS node to the one it mounts — so mounting buffer
    // termBuf+1 would overwrite the NULL `next` we set on termBuf, and the chain would loop forever instead
    // of self-terminating (bench: ld=230, the DMA lapped ~23x and `done` fired on stale looped buffers = the
    // scatter). The DMA never reaches buffers past the terminator, so leaving them unmounted is correct.
    const uint8_t mountCount = primeOnly ? static_cast<uint8_t>(termBuf + 1u) : st->ringBufs;
    // LAPPING + pad: interleave one pad node after every data node — data → pad → data → pad → … — every
    // pad node re-mounting the SAME shared zero block (gdma_link_mount_buffers takes an explicit start
    // index, so re-mounting one buffer at many offsets is just more mount calls). The pad reads as a
    // strand-level pause after each slice, stretching the refill deadline by its wire time; mark_eof stays
    // on the DATA nodes (the pad drain needs no interrupt), and the loop-to-HEAD moves to the LAST pad.
    const bool padded = !primeOnly && st->zeroPad != nullptr;
    int idx = 0;
    bool mountOk = true;
    for (uint8_t b = 0; b < mountCount && mountOk; b++) {
        const bool last = (b == mountCount - 1);
        gdma_buffer_mount_config_t mount = {};
        mount.buffer = st->ring[b];
        mount.length = rowsOnly;   // rows only — continuous stream, no inter-buffer LOW gap (see initRingDma)
        // PRIME-ONLY: interrupt ONCE per frame, on the terminator only. The whole frame is primed before
        // arming, so the ISR has no per-buffer work — and counting per-buffer EOFs to detect frame-end is
        // UNSOUND: the GDMA interrupt status is a latch bit, not a queue, so two EOFs landing while the ISR
        // is delayed (a large /api/state serialise, WiFi) coalesce into ONE invocation and the drain count
        // undercounts — `done` then never fires and the driver gives up (bench: every big-frame config died
        // within ~20 frames with ld stuck a few short of nSlices+1; small frames rarely coalesced). One EOF
        // per frame makes the undercount impossible and cuts the interrupt load ~nSlices-fold (the
        // sub-hot-path rule). hpwit does the same: his prime/arm node carries suc_eof=0 — he too interrupts
        // only where it means something. LAPPING keeps per-buffer EOFs — its ISR genuinely refills per drain.
        mount.flags.mark_eof = primeOnly ? (b == termBuf) : true;
        // Prime-only: the reset-tail buffer self-terminates (NULL); every other node → next (DEFAULT).
        // Lapping: every node → next (DEFAULT). The lapping chain never loops to HEAD — armRingTransfer
        // re-links it linear and plants the frontier NULL at the prime edge, and the ISR advances that NULL.
        // A DEFAULT link on the last mounted node is a don't-care: armRingTransfer overwrites it before arm.
        mount.flags.mark_final = (b == termBuf) ? GDMA_FINAL_LINK_TO_NULL : GDMA_FINAL_LINK_TO_DEFAULT;
        int endIdx = 0;
        if (idx >= static_cast<int>(st->linkItemCap)) mountOk = false;
        else if (gdma_link_mount_buffers(st->link, idx, &mount, 1, &endIdx) != ESP_OK) mountOk = false;
        else st->bufLastNode[b] = endIdx;   // buffer b's real last node (kept for the lapping ISR splice)
        idx = endIdx + 1;
        if (padded && mountOk) {
            gdma_buffer_mount_config_t pad = {};
            pad.buffer = st->zeroPad;
            pad.length = st->zeroPadBytes;
            pad.flags.mark_eof = false;   // the pad's drain is dead time; the data node's EOF drives the ISR
            // Every pad node → next (DEFAULT). The last one is a don't-care too — armRingTransfer re-links
            // the chain (through each buffer's tail node = its pad, ringTailNode) and plants the frontier.
            pad.flags.mark_final = GDMA_FINAL_LINK_TO_DEFAULT;
            (void)last;
            int padEnd = 0;
            if (idx >= static_cast<int>(st->linkItemCap)) mountOk = false;
            else if (gdma_link_mount_buffers(st->link, idx, &pad, 1, &padEnd) != ESP_OK) mountOk = false;
            idx = padEnd + 1;
        }
    }
    st->consumedItems = static_cast<uint32_t>(idx);   // diagnostic: exposed via moonI80Ws2812RingStats
    // SELF-TERM DIAG (temp): the actual mount-time NULL terminator node, for the ringDbg readout.
    st->termNode = primeOnly ? st->bufLastNode[termBuf] : -1;
    if (!mountOk) { destroyState(st); return nullptr; }

    // No refill task: the EOF ISR encodes the next slice inline as each buffer drains (see moonI80EofCb).
    // That is the reuse-race fix — an interrupt-priority refill always beats the DMA lapping into a reused
    // buffer, which a lower-priority task could lose to wake latency at >8 slices (≥192 lights/strand).
    return st;
}

// Abandon the in-flight transfer and return the peripheral to a clean idle — the ONE recovery both the
// ring and whole-frame wait-timeout backstops share. A lost/coalesced EOF leaves `busy` stuck true with
// no interrupt coming to clear it; without this the bus wedges permanently (every later transmit blocks
// its full timeout on the stuck busy, and the driver's give-up retry re-arms into the same stuck state).
// The single owner of "the EOF didn't come, unstick the bus": stop the LCD + GDMA, clear busy, and mark
// the strand as idling LOW now (the WS2812 reset begins here) so the next arm holds the reset window.
// The CONDITION for finalizing, and any mode-specific residue (the ring re-links its chain and latches
// encode stats on the next arm; the whole-frame path drains its completion FIFO), stay at the call sites —
// only the shared stop-and-clear lives here, so the two paths can't drift in how they leave the hardware.
void finalizeStalledTransfer(MoonI80State* st) {
    lcd_ll_stop(st->hal.dev);
    gdma_stop(st->dma);
    st->busy = false;
    st->lastStopUs = esp_timer_get_time();
    st->dbgStallAbandons = st->dbgStallAbandons + 1u;
}

} // namespace

bool moonI80Ws2812Init(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                       uint16_t wrGpio, size_t bufferBytes,
                       bool wantSecondBuffer, uint8_t clockMultiplier) {
    if (!dataPins || laneCount == 0 || bufferBytes == 0 || clockMultiplier == 0) return false;
    // Pre-check that the frame can land SOMEWHERE before touching the peripheral. createState
    // allocates PSRAM-first (direct mode) or internal-first (shift mode) and falls back to the other,
    // so init is fine when EITHER region fits. The HEAP_RESERVE floor guards only INTERNAL RAM (the
    // WiFi/HTTP reserve); a PSRAM buffer doesn't touch it. Degrade (return false → the driver idles
    // with a status) when neither region fits.
    //
    // The PSRAM capacity query uses MALLOC_CAP_SPIRAM ALONE: no registered heap is tagged BOTH
    // SPIRAM and DMA, so the combined query returns 0 even on an S3 whose GDMA reaches PSRAM
    // perfectly well, and gating on it would silently cap the driver at the internal heap. (The
    // *alloc* does pass both caps, which is correct — that's what IDF itself does.)
    // LARGEST BLOCK, not total free: this is ONE contiguous frame, so a fragmented heap reporting
    // megabytes free with no run big enough would pass a total-free test and then fail the alloc. The
    // sibling i80 backend tests the same way, for the same reason — a single-buffer path must ask
    // "is there a run this big?", never "is there this much in total". (The ring's own check in
    // moonI80Ws2812InitRing deliberately uses free SIZE: it makes many small allocations, so no single
    // run of the total is needed.)
    const bool fitsInternal =
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
            >= bufferBytes + HEAP_RESERVE;
    const bool fitsPsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= bufferBytes;
    if (!fitsInternal && !fitsPsram) return false;
    MoonI80State* st = createState(dataPins, laneCount, wrGpio, bufferBytes,
                                   wantSecondBuffer, clockMultiplier);
    if (!st) return false;
    h.impl = st;
    return true;
}

bool moonI80Ws2812InitRing(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                           uint16_t wrGpio, size_t rowBytes, uint32_t totalRows,
                           uint32_t rowsPerBuf, uint8_t ringBufs, uint8_t padUs,
                           uint8_t clockMultiplier, MoonI80EncodeFn encode, void* user) {
    if (!dataPins || laneCount == 0 || rowBytes == 0 || totalRows == 0 || clockMultiplier == 0
        || !encode) return false;
    // The geometry is a user control, so clamp it here rather than trusting the caller: a 0 would divide
    // by zero in the nSlices math, and a depth past the array bound would overrun `ring[]`. Depth 2 is the
    // hard floor (see kRingBufsMax's note: IDF's 2-buffer bounce scheme relies on an owner gate this chain
    // does not run).
    if (rowsPerBuf == 0 || ringBufs < 2 || ringBufs > kRingBufsMax) {
        ESP_LOGE(MOON_I80_TAG, "ring init rejected: rowsPerBuf=%u ringBufs=%u (bounds %u..%u)",
                 (unsigned)rowsPerBuf, (unsigned)ringBufs, (unsigned)kRingBufsMin, (unsigned)kRingBufsMax);
        return false;
    }
    if (padUs > kRingPadMaxUs) padUs = kRingPadMaxUs;   // the latch-threshold bound (see platform.h)
    // The ring's N internal buffers must fit internal DMA RAM while leaving the WiFi/HTTP reserve — this
    // is the whole reason the ring exists (the frame would NOT fit; the small buffers do). If even the
    // ring won't fit, fail so the caller falls back to the whole-frame path (which then idles with a
    // status if IT can't fit either — same degrade as always).
    // **ROWS ONLY — a ring buffer carries no latch pad.** The mount sets `mount.length = rowsOnly`, so a
    // pad inside a buffer would be allocated, encoded, cache-synced and NEVER CLOCKED — 43% of the ring's
    // RAM for nothing. Worse, it decided whether the ring ran AT ALL: sizing every buffer rows+pad pushed
    // the pool past the internal DMA heap, so this guard returned false and the driver silently fell back
    // to whole-frame even with the ring explicitly selected. The WS2812 reset comes from stopping the peripheral and
    // letting the lines idle LOW (moonI80EofCb), never from a pad inside a circulating node.
    //
    // Free SIZE, not largest block: the ring makes `ringBufs` separate small allocations, so no single
    // contiguous run of this size is needed (unlike moonI80Ws2812InternalFits, which sizes ONE frame and
    // must test the largest block). The per-allocation heap overhead (~8-12 B/block) is not modelled —
    // negligible at kilobyte buffers, but it is a real fraction of a small rowsPerBuf=1 buffer.
    // Clamp to the one-node row limit HERE, before the heap-fit math, so the pre-check prices the
    // geometry createRingState actually builds (it applies the same clamp): an oversized ringRows must
    // not inflate `need` into a spurious whole-frame fallback. Same floor of 1 row.
    const uint32_t maxRowsPerNode = static_cast<uint32_t>(kDmaNodeMaxBytes / rowBytes);
    const uint32_t rowsEffective = rowsPerBuf > maxRowsPerNode ? (maxRowsPerNode ? maxRowsPerNode : 1u)
                                                              : rowsPerBuf;
    const size_t bufBytes = static_cast<size_t>(rowsEffective) * rowBytes;
    // The pad block is ~3.5 KB at the ceiling — priced in so a pad-tight heap fails here, not mid-create.
    const size_t padBytes = padUs ? (static_cast<size_t>(padUs) * 27u * 2u) : 0;   // upper bound, 16-bit bus
    const size_t need = bufBytes * ringBufs + padBytes + HEAP_RESERVE;
    const size_t freeDma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (freeDma < need) {   // fall back to whole-frame (or the loopback's pool step-down)
        ESP_LOGW(MOON_I80_TAG, "ring init rejected: need %u B internal DMA (bufs=%u x %u B + reserve), free %u B",
                 (unsigned)need, (unsigned)ringBufs, (unsigned)bufBytes, (unsigned)freeDma);
        return false;
    }
    MoonI80State* st = createRingState(dataPins, laneCount, wrGpio, rowBytes, totalRows,
                                       rowsEffective, ringBufs, padUs, clockMultiplier, encode, user);
    if (!st) return false;
    h.impl = st;
    return true;
}

// Set the '595 shift-clock prescale (off the 80 MHz bus resolution) for the NEXT init: 4 = 20 MHz
// (default, the reliability point), 3 = 26.67 MHz (overclock), 5 = 16 MHz. A strip whose '595s can't
// shift reliably at a faster rate steps the divider up (slower clock = more shift margin, longer T0H).
// Takes effect on the next bus (re)build; the driver makes its shiftClockDiv control a rebuild trigger.
// Clamped to the valid prescale range.
void moonI80SetShiftClockDiv(uint8_t div) {
    if (div < 3) div = 3;                      // 80/3 = 26.67 MHz is the fastest in-spec-T0H rate
    if (div > LCD_LL_PCLK_DIV_MAX) div = LCD_LL_PCLK_DIV_MAX;
    g_shiftClockDiv = div;
}

void moonI80Ws2812PrimeRange(MoonI80Ws2812Handle& h, uint8_t bufLo, uint8_t bufHi) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || !st->isRing || st->busy) return;   // never prime under a live transfer
    primeRingRange(st, bufLo, bufHi);
}

bool moonI80Ws2812ArmRing(MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || !st->isRing || st->busy) return false;
    return armRingTransfer(st);
}

bool moonI80Ws2812TransmitRing(MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || !st->isRing) return false;
    // Serial on the wire like the whole-frame path: a strand receives one frame at a time. If a previous
    // ring frame is still clocking out, its completion gives done[0]; the driver waits on slot 0 before
    // calling here again (tickRing), so busy should already be clear — guard anyway.
    if (st->busy) return false;
    return startRingTransfer(st);
}

bool moonI80Ws2812IsRing(const MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    return st && st->isRing;
}

bool moonI80Ws2812InternalFits(size_t bytes) {
    // Does a whole `bytes`-sized frame fit internal DMA RAM as ONE CONTIGUOUS BLOCK? The whole-frame path
    // allocates the frame in a single `heap_caps_aligned_calloc`, so what matters is the LARGEST FREE
    // BLOCK, not total free — a fragmented heap can have megabytes free yet no 144 KB contiguous block.
    // Using total-free here was a bug: at 16 strands a 144 KB shift frame reported "fits" on total-free,
    // so wantsRing() said false, the whole-frame alloc then FAILED the contiguous 144 KB, fell back to
    // PSRAM, and STALLED at the expander clock (the exact failure this test exists to route around). The
    // largest-block test is what the alloc actually faces, and it also leaves HEAP_RESERVE for WiFi/HTTP.
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t freeTotal = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    return largest >= bytes && freeTotal >= bytes + HEAP_RESERVE;
}

uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle& h, uint8_t buffer) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st) return nullptr;
    // A ring handle has no whole-frame buf[]; the domain never encodes into a ring buffer itself (the
    // platform's refill task does, via the encode seam). But the base uses busBuffer(0) as its non-null
    // "inited" sentinel (dmaBuf_) and busBuffer(1) to detect double-buffer mode. So report ring[0] for
    // slot 0 (a real, non-null pointer → inited) and null for slot 1 (a ring is never the double-buffer).
    if (st->isRing) return buffer == 0 ? st->ring[0] : nullptr;
    return buffer < 2 ? st->buf[buffer] : nullptr;
}

size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    return st ? st->cap : 0;
}

bool moonI80Ws2812Transmit(MoonI80Ws2812Handle& h, uint8_t buffer, size_t bytes) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || buffer >= 2 || !st->buf[buffer] || bytes == 0 || bytes > st->cap) return false;
    // **The peripheral holds exactly one transfer, and the caller is allowed to hand us the next one
    // while it is still clocking out — so wait for the wire here rather than refusing.**
    //
    // Unlike esp_lcd there is no transaction queue to absorb a second transfer, and reprogramming the
    // link list while the DMA is mid-walk would garble the frame. But refusing a busy bus would be
    // wrong: the async double-buffer's whole design is that the driver waits only on the buffer it is
    // about to ENCODE into (`tickAsync` → `busWaitIfBusy(active_)`), then transmits it — at which
    // point the OTHER buffer's transfer is quite legitimately still on the wire. Refusing there would
    // drop every second frame and eventually trip the driver's dead-frame guard.
    //
    // Waiting here costs nothing the double-buffer was buying: its win is that the ENCODE of frame
    // N+1 overlapped the wire time of frame N, and that has already happened by the time we are
    // called. What is left is the wire itself, which is serial on any design — the strand can only
    // receive one frame at a time.
    //
    // Drain a STALE wire-free token first: the previous frame's EOF gives wireFree unconditionally, so
    // a token can sit un-consumed when no transmit was waiting on it. Taking it non-blocking here means
    // the busy-wait below waits for THIS frame's EOF, not a past one.
    xSemaphoreTake(st->wireFree, 0);
    if (st->busy) {
        // Block on the dedicated wire-free signal, not on the in-flight buffer's done[] — that one
        // belongs to the DRIVER (it waits on the buffer it means to reuse), and consuming it here
        // would make that wait miss. The EOF gives both.
        //
        // The bound is a backstop against a wedged peripheral, not a policy: the driver's own
        // frame-derived timeout (ParallelLedDriver::waitBudgetMs) is what actually governs a stalled
        // bus. This exists only so a broken DMA cannot hang the render thread indefinitely.
        if (xSemaphoreTake(st->wireFree, pdMS_TO_TICKS(kWireFreeTimeoutMs)) != pdTRUE) return false;
    }

    // Push the buffer onto the completion FIFO BEFORE starting the hardware, so a fast EOF (which
    // pops it) can never fire before its slot is populated. The push touches slot `fifoHead`; the ISR
    // only ever reads slot `fifoTail`, and with one transfer in flight those are different slots —
    // that disjointness is what makes the push safe without a lock.
    const uint8_t slot = st->fifoHead;
    st->fifo[slot] = buffer;
    // The transfer starts on the wire immediately (we waited above for any predecessor), so stamp the
    // wire-time KPI right here — no deferred stamping is needed, which is a small simplification
    // over the esp_lcd sibling's queued path.
    st->txStartUs[slot] = esp_timer_get_time();
    st->fifoHead = (st->fifoHead + 1u) & 1u;
    st->busy = true;

    if (!startTransfer(st, buffer, bytes)) {
        // Failed to arm — unwind the FIFO push so the ISR count stays balanced. Safe: no transfer
        // started, so no EOF will pop this slot.
        st->fifoHead = (st->fifoHead + 1u) & 1u;
        st->busy = false;
        return false;
    }
    return true;
}

bool moonI80Ws2812Wait(MoonI80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || buffer >= 2 || !st->done[buffer]) return true;   // nothing to wait on = not in flight
    // Report whether the transfer actually completed. On a timeout the DMA may still be reading this
    // buffer, so the caller must keep it marked in-flight rather than re-encoding into it — handing a
    // live DMA a half-rewritten buffer is exactly the frame corruption the timeout is meant to avoid.
    if (xSemaphoreTake(st->done[buffer], pdMS_TO_TICKS(timeoutMs)) == pdTRUE) return true;

    // STALL BACKSTOP (lapping ring only). A WiFi/flash cache-off window can outlast the pool's lead: the
    // cache-safe ISR defers, the DMA reaches the frontier terminator and HALTS at its NULL, and — because a
    // halted engine fires no more EOFs — `done` is never given, so the wait above just timed out. This is
    // the DESIGNED benign outcome (the strand held a partially-updated frame for one frame period instead
    // of replaying stale slices), but the frame must still be finalized so the render thread proceeds. Only
    // when the oracle proves the engine can no longer be mid-frame — the whole frame's wire time has
    // elapsed since arm AND the refills never reached the frame's end (a completed frame gives `done` in
    // the ISR, not here) — do we finalize: stop the peripheral, clear busy, count it. The next arm re-links
    // the chain and plants a fresh frontier from scratch, so no residual state carries over.
    if (st->isRing && st->busy && st->nSlices > st->ringBufs
        && st->lastWrittenSlice < st->nSlices + kTailBufs) {   // kTailBufs (file scope), not a bare +1
        const int64_t frameWireUs = (static_cast<int64_t>(st->nSlices) * st->sliceNs) / 1000;
        if (esp_timer_get_time() - st->armUs >= frameWireUs) {
            finalizeStalledTransfer(st);   // the shared stop-and-clear; the next arm re-links the chain
            // Latch whatever this frame's refills managed, so the ea readout isn't stuck at a half window.
            st->dbgEncAvgUs = st->dbgEncCount ? st->dbgEncSumUs / st->dbgEncCount : st->dbgEncAvgUs;
            st->dbgEncSumUs = 0;
            st->dbgEncCount = 0;
        }
    }

    // STALL BACKSTOP (whole-frame path). The EOF interrupt is a latch that can, very rarely, be lost —
    // two firings coalescing, or one racing the next frame's reset — leaving `busy` stuck true with no
    // EOF coming to clear it. Without recovery the bus wedges permanently: every later transmit sees busy
    // and blocks its full wire-free timeout, so the driver's give-up retry re-arms into the same stuck
    // state (the ~5 s-then-dark wedge on a direct strand). The ring branch above finalizes its own stall
    // on the oracle's condition; here the condition is simply "the wait timed out with a transfer in
    // flight." Shared stop-and-clear via finalizeStalledTransfer; the whole-frame residue is draining the
    // completion FIFO so the abandoned entry can't be popped by a late EOF against the next frame.
    if (!st->isRing && st->busy) {
        finalizeStalledTransfer(st);   // stop LCD + GDMA FIRST, so no EOF can fire during the drain below
        st->fifoTail = st->fifoHead;   // then drop the un-completed entry; the ISR guard ignores a late EOF
    }
    return false;   // the caller keeps the buffer in-flight for this frame; the next frame arms fresh
}

uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    return st ? st->lastTransmitUs : 0;
}

MoonI80RingStats moonI80Ws2812RingStats(const MoonI80Ws2812Handle& h) {
    MoonI80RingStats s;
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || !st->isRing) return s;
    s.isRing        = true;
    s.nSlices       = st->nSlices;
    s.ringBufs      = st->ringBufs;
    s.eofTotal      = st->dbgEofTotal;
    s.cacheOffDefers = st->dbgCacheOffDefers;
    s.cacheOffMaxRun = st->dbgCacheOffMaxRun;
    s.stallAbandons = st->dbgStallAbandons;
    s.doneGiven     = st->dbgDoneGiven;
    s.lastDrain     = st->dbgLastDrain;
    s.numItems      = static_cast<uint32_t>(st->linkItemCap);
    s.consumedItems = st->consumedItems;
    s.descErr       = st->dbgDescErr;
    s.maxEncodeUs   = st->dbgMaxEncodeUs;
    s.avgEncodeUs   = st->dbgEncAvgUs;
    s.maxIsrGapUs   = st->dbgMaxIsrGapUs;
    s.itemsPerBuf   = st->itemsPerBuf;
    s.termNodeDiag  = st->termNode;
    s.late          = st->dbgLate;
    return s;
}

void moonI80Ws2812Deinit(MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st) return;
    destroyState(st);
    h.impl = nullptr;
}

// ---------------------------------------------------------------------------
// Loopback self-test: a private full-width peripheral setup on the driver's real pins transmits the
// CALLER'S real frame — full size, real descriptor chain, real latch pad — exactly like the render
// loop, while an RMT RX channel captures the whole frame off the jumpered rxGpio and verifies every
// bit. A short synthetic burst would miss exactly the failures a real frame hits (descriptor
// boundaries, sustained-rate stalls), so the test sends the genuine article.
//
// The capture + bit-verify half is shared with the esp_lcd and Parlio loopbacks in
// detail::captureAndVerifyFrame (platform_esp32_rmt.cpp); only the transmit differs.
// ---------------------------------------------------------------------------

namespace detail {
void captureAndVerifyFrame(uint16_t rxGpio, size_t frameBytes, size_t dataBytes,
                           uint8_t rowBits, uint32_t pclkHz, bool pinExpanderMode, const char* tag,
                           const std::function<void()>& transmitOnce,
                           RmtLoopbackResult& r, bool rideMode = false,
                           uint32_t* rxSymbols = nullptr);
// Pre-allocate the capture buffer captureAndVerifyFrame needs (one contiguous DMA-capable internal
// block, sized from dataBytes) so a caller can grab it BEFORE its own allocations fragment the heap
// (largest-first allocation order). Pass the result as `rxSymbols`; ownership transfers to
// captureAndVerifyFrame regardless of outcome. nullptr on alloc failure is fine to pass through —
// the helper then retries the alloc itself and reports the failure.
uint32_t* allocLoopbackCapture(size_t dataBytes);
}

RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                        uint16_t wrGpio, uint16_t rxGpio,
                                        const uint8_t* frame, size_t frameBytes,
                                        size_t dataBytes, uint8_t rowBits,
                                        uint8_t clockMultiplier,
                                        uint32_t ringRows, uint32_t ringBufs,
                                        bool useRingArg) {
    RmtLoopbackResult r;
    r.sent[0] = 0xA5; r.sent[1] = 0x00; r.sent[2] = 0xFF;  // pattern in every row
    if (!dataPins || laneCount == 0 || !frame || frameBytes == 0
        || dataBytes < 3 || dataBytes > frameBytes || rowBits < 8
        || clockMultiplier == 0) return r;
    const uint16_t txGpio = dataPins[0];   // lane 0 carries the pattern
    const bool pinExpanderMode = clockMultiplier > 1;

    if (pinExpanderMode) {
        // SKIP the continuity pre-check. It drives txGpio and expects rxGpio to follow directly,
        // which is true of a bare jumper but FALSE through a 74HCT595: raising the serial input does
        // not raise an output (that takes 8 shift clocks + a latch). Running it here would report
        // "jumper not detected" on perfectly good wiring. The rx pin is fed from a '595 OUTPUT, so
        // the only proof the wire is right is the bit-verify itself — which is the stronger check
        // anyway (it validates the whole chain: encode → bus → shift → latch → output).
        r.jumperDetected = true;
    } else {
        r.jumperDetected = detail::loopbackJumperOk(static_cast<uint8_t>(txGpio),
                                                    static_cast<uint8_t>(rxGpio));
        if (!r.jumperDetected) return r;
    }

    // **RING vs whole-frame — the loopback follows the SAME rule the render path does.** A shift-mode
    // test frame at kLoopbackTestLights (256) is ~147 KB, which lands in PSRAM and stalls at the expander
    // clock on the whole-frame path — so the loopback would time out and report "0 symbols" for a
    // transport fault, never testing the encode (the backlogged "shift-mode loopback stalls" bug). When
    // the frame won't fit internal, stream it through the RING instead, exactly as the render path does:
    // the bit-verify then validates the ACTUAL ring output at 256 — the instrument the ring needs.
    //
    // The ring's encode seam is a slice PRODUCER; the loopback already holds the whole pre-encoded frame,
    // so its "encode" is a COPY of the matching slice out of `frame`. Deriving the ring geometry from the
    // loopback's parameters: rowBytes = the per-row encoded size, totalRows = the light count.
    // Ride the ring when the driver asked for it (useRingArg — the render path is on the ring, so the
    // self-test must be too), OR legacy auto: when the frame would overflow internal RAM. Either way only
    // in expander mode — direct mode has no ring.
    const bool useRing = pinExpanderMode && (useRingArg || !moonI80Ws2812InternalFits(frameBytes));
    const uint8_t sb = laneCount <= 8 ? 1 : 2;
    // rowBytes = outCh(=rowBits/8) × 8 × 3 × slotBytes × outputsPerPin(=clockMultiplier in shift mode).
    const size_t loopRowBytes = static_cast<size_t>(rowBits) * 3u * sb * clockMultiplier;
    // The ROW COUNT is the strand's light count, and it must come from the STRAND-side units: dataBytes
    // counts strand wire bytes (lights × rowBits/8 × 3, width-independent — see the caller's derivation),
    // and rowBits×3 is one light's strand bytes, so the quotient is `lights`. Dividing by loopRowBytes
    // (BUS bytes per row, which carries ×sb×clockMultiplier) mixes units and shrinks the ring to
    // lights/(sb×multiplier) rows — an expander loopback would build a ring for 1/8th of the frame.
    const uint32_t rowStrandBytes = static_cast<uint32_t>(rowBits) * 3u;
    const uint32_t loopRows = rowStrandBytes ? static_cast<uint32_t>(dataBytes / rowStrandBytes) : 0;

    // Grab the capture buffer FIRST: it is the loopback's one big contiguous DMA block (~4 B per
    // WS2812 bit), and the ring init below fragments internal RAM with its per-buffer nodes — in that
    // order the capture alloc fails on a busy heap and the test dies as "no capture" before
    // transmitting a bit. Largest-first fixes it; ownership passes to captureAndVerifyFrame.
    uint32_t* rxSymbols = detail::allocLoopbackCapture(dataBytes);

    MoonI80State* st = nullptr;
    // The copy-slice "encoder": the frame is already encoded, so a ring slice is a straight memcpy out of
    // it. The seam is a plain function pointer + `void* user`, so the frame geometry rides in `user` (a
    // stack struct that outlives the transmit — the ring runs synchronously within this function).
    // ROWS ONLY, like every ring slice: the pre-built frame's trailing latch pad is NOT copied. A ring
    // buffer holds rows and nothing else (the WS2812 reset comes from stopping the peripheral), so the
    // pad has nowhere to go — and the seam is called with closeFrame=false for every slice anyway.
    struct LoopCopyCtx { const uint8_t* frame; size_t rowBytes; uint32_t rows; size_t slotBytes; };
    LoopCopyCtx ctx{frame, loopRowBytes, loopRows, sb};
    if (useRing && loopRows > 0) {
        auto copySlice = [](void* user, uint8_t* dst, uint32_t firstRow, uint32_t count, bool close,
                            bool /*needsPrefill*/) {   // a full memcpy re-writes constants + data alike
            auto* c = static_cast<LoopCopyCtx*>(user);
            if (count == 0) {
                // The frame-close call: the pre-built frame's pad HEAD is exactly the latch-only word
                // (encodeWs2812ShiftLatchPad wrote it there) — copy that one word.
                if (close) std::memcpy(dst, c->frame + static_cast<size_t>(c->rows) * c->rowBytes, c->slotBytes);
                return;
            }
            std::memcpy(dst, c->frame + static_cast<size_t>(firstRow) * c->rowBytes,
                        static_cast<size_t>(count) * c->rowBytes);
        };
        MoonI80Ws2812Handle h;
        // Geometry: prefer the driver's LIVE ringRows/ringBufs so the self-test streams through the
        // SAME ring the render path is tuned to — then a scattered margin (bufs − nSlices < ~2) shows
        // here as a bit fault at the same slice boundary the eyes see on the wall. 0 → the platform
        // default, so a caller that does not care (direct-mode continuity) still works. Pool depth
        // steps down on a RAM-tight heap (see below).
        const uint32_t loopRingRows = ringRows ? ringRows : kRingRowsDefault;
        // The live pool depth when RAM allows; else STEP DOWN until the init's heap gate accepts — the
        // capture buffer above already holds ~4 B per WS2812 bit of the same internal DMA pool, and on a
        // RAM-tight board live-depth ring + capture do not both fit. A shallower loopback ring still
        // bit-verifies the same encode→bus→'595→latch chain; only the refill margin differs, and the
        // copy-slice refill is far faster than the render encode, so even a minimal pool streams clean.
        uint32_t tryBufs = ringBufs ? ringBufs : kRingBufsDefault;
        while (tryBufs >= kRingBufsMin) {
            if (moonI80Ws2812InitRing(h, dataPins, laneCount, wrGpio, loopRowBytes, loopRows,
                                      loopRingRows, tryBufs, /*padUs=*/0, clockMultiplier, copySlice,
                                      &ctx)) {
                st = static_cast<MoonI80State*>(h.impl);
                break;
            }
            tryBufs /= 2;
        }
    }
    // Whole-frame fallback (direct mode, or a frame that fits internal): the original private bus + copy.
    if (!st) {
        // The continuity check above reset txGpio's GPIO matrix route; createState re-claims it.
        st = createState(dataPins, laneCount, wrGpio, frameBytes, /*wantSecond=*/false, clockMultiplier);
        if (!st) {
            ESP_LOGE(MOON_I80_TAG, "loopback: private peripheral setup failed");
            heap_caps_free(rxSymbols);   // ownership never reached captureAndVerifyFrame
            return r;
        }
        std::memcpy(st->buf[0], frame, frameBytes);   // loopback uses buffer 0 only (single transfer)
    }
    const bool loopIsRing = st->isRing;

    // Ship one frame and wait for its EOF. Everything else (capture, cadence, bit-verify) is the shared
    // helper. This is the runtime path's bookkeeping, minus the handle indirection: surface a failed arm
    // or an EOF timeout instead of letting either show up only as a later capture mismatch (same handling
    // as the esp_lcd and Parlio siblings). The ring and whole-frame transmits differ only in which start
    // they call; both complete on done[0].
    auto transmitOnce = [st, frameBytes, loopIsRing]() {
        bool armed;
        if (loopIsRing) {
            armed = startRingTransfer(st);
        } else {
            st->fifo[st->fifoHead] = 0;
            st->txStartUs[st->fifoHead] = esp_timer_get_time();
            st->fifoHead = (st->fifoHead + 1u) & 1u;
            st->busy = true;
            armed = startTransfer(st, 0, frameBytes);
            if (!armed) {
                st->fifoHead = (st->fifoHead + 1u) & 1u;   // unwind the push
                st->busy = false;
            }
        }
        if (!armed) { ESP_LOGE(MOON_I80_TAG, "loopback: tx arm failed"); return; }
        if (xSemaphoreTake(st->done[0], pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(MOON_I80_TAG, "loopback: tx EOF timed out");
    };
    // `pclkHz` tells the verifier the WS2812 SLOT RATE the strand sees — it derives both the
    // pulse-width threshold ("0" = one slot, "1" = two) and the expected transmit duration from it,
    // so it must describe the STRAND's waveform, not the bus. In shift mode the strand's slot is NOT
    // the bus period: `clockMultiplier` bus words fill one slot, so slot rate = pclk / multiplier
    // (26.67 MHz ÷ 8 = 3.33 MHz → a 300 ns slot). Passing the bus rate here makes the capture expect
    // the wrong pulse width and size the window for a frame 8× too short — a decode that matches
    // nothing on a strand whose LEDs are visibly lighting.
    // The verifier's slot rate must match the LIVE wire — derive it from the runtime divider, not a
    // constant, or a non-default shiftClockDiv makes the capture expect the wrong pulse widths.
    const uint32_t slotHz = pinExpanderMode
        ? (kShiftBusResolutionHz / g_shiftClockDiv / clockMultiplier) : kPclkHz;
    detail::captureAndVerifyFrame(rxGpio, frameBytes, dataBytes, rowBits, slotHz, pinExpanderMode,
                                  MOON_I80_TAG, transmitOnce, r, /*rideMode=*/false, rxSymbols);
    destroyState(st);
    return r;
}

} // namespace mm::platform

#else  // !SOC_LCDCAM_I80_LCD_SUPPORTED — inert stubs so a chip without LCD_CAM links

namespace mm::platform {

bool moonI80Ws2812Init(MoonI80Ws2812Handle&, const uint16_t*, uint8_t, uint16_t, uint16_t,
                       size_t, bool, uint8_t) {
    return false;
}
bool moonI80Ws2812InitRing(MoonI80Ws2812Handle&, const uint16_t*, uint8_t, uint16_t, size_t,
                           uint32_t, uint32_t, uint8_t, uint8_t, uint8_t, MoonI80EncodeFn, void*) {
    return false;
}
bool moonI80Ws2812TransmitRing(MoonI80Ws2812Handle&) { return false; }
void moonI80SetShiftClockDiv(uint8_t) {}
void moonI80Ws2812PrimeRange(MoonI80Ws2812Handle&, uint8_t, uint8_t) {}
bool moonI80Ws2812ArmRing(MoonI80Ws2812Handle&) { return false; }
bool moonI80Ws2812IsRing(const MoonI80Ws2812Handle&) { return false; }
bool moonI80Ws2812InternalFits(size_t) { return false; }
uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle&, uint8_t) { return nullptr; }
size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle&) { return 0; }
bool moonI80Ws2812Transmit(MoonI80Ws2812Handle&, uint8_t, size_t) { return false; }
bool moonI80Ws2812Wait(MoonI80Ws2812Handle&, uint8_t, uint32_t) { return true; }
uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle&) { return 0; }
MoonI80RingStats moonI80Ws2812RingStats(const MoonI80Ws2812Handle&) { return {}; }
void moonI80Ws2812Deinit(MoonI80Ws2812Handle&) {}
RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t*, uint8_t, uint16_t, uint16_t,
                                        uint16_t, const uint8_t*, size_t, size_t, uint8_t,
                                        uint8_t, uint32_t, uint32_t, bool) {
    return {};
}

} // namespace mm::platform

#endif  // SOC_LCDCAM_I80_LCD_SUPPORTED
