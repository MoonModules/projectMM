#pragma once

#include "light/drivers/DriverBase.h"
#include "light/drivers/Hub75Slots.h"
#include "light/layers/Layer.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// Output driver: HUB75 panels driven directly from the board's own pins, with no receiving card.
///
/// The sibling of `PanelCardDriver`, which drives the same panels the other way:
/// it emits ColorLight frames to a 5A-75B/E receiving card over raw Ethernet. That path is the right
/// one above roughly 16,384 pixels. Below it the card is overhead: a Windows tool to configure, a
/// dedicated Ethernet link, and a board that could have driven the panel from its own GPIO. This
/// driver closes that gap.
///
/// **A HUB75 panel is scanned, not addressed.** Two rows light at once (upper through R1/G1/B1,
/// lower through R2/G2/B2), selected by a row address, and the controller walks every address in
/// turn. Brightness is TIME rather than amplitude: a pixel is a switch, so intensity comes from
/// showing bit plane p for 2^p time units. Both facts live in the encoder
/// (`Hub75Slots.h`), which is pure data and host-tested with no panel.
///
/// **Output is continuous.** A WS2812 strand latches a frame and holds it; a HUB75 panel holds
/// nothing and is lit only while being clocked. So the platform arms one scan and re-sends the same
/// buffer forever, and this driver writes the next frame into that buffer between scans. There is no
/// per-frame transmit and no wait, which is why tick() looks unlike every other driver's.
///
/// Prior art: the HUB75 lineage generally (mrcodetastic/ESP32-HUB75-MatrixPanel-DMA,
/// hzeller/rpi-rgb-led-matrix, ESPHome's hub75 component). The scan and bit-plane structure belongs
/// to the panel rather than to any library; studied, not copied.
/// ## Pin configurations for known boards
///
/// The `board` select supplies the pins, defaulting to MoonHub75 and prefilling all fourteen on
/// first definition, because a soldered line must never be guessed from nothing. These are the
/// maps for boards that wire a HUB75 connector, taken from each board's own published source
/// rather than from a tutorial. A panel's ribbon numbers its color lines R1/G1/B1 (upper half)
/// and R2/G2/B2 (lower half); some board docs call the same pairs R0/G0/B0 and R1/G1/B1, which is
/// the one naming trap worth knowing before wiring.
///
/// **MoonHub75 PCB** (Lilygo T7-S3 + passive adapter, designed by Sören / lost-hope).
/// A MoonModules board: https://moonmodules.org/projects/hardware/#moonhub75-pcb
/// Map from the hardware repository (MOONHUB75/README.md), whose R0/G0/B0 is this driver's r1/g1/b1:
///
///     r1  1    g1  5    b1  6
///     r2  7    g2 13    b2  9
///     a  16    b  48    c  47    d  21    e  38
///     clk 18   lat 8    oe  4
///
/// The same board carries an INMP441 microphone socket on IO10/11/12 (DA/CK/WS), so an audio-
/// reactive effect and a panel run together on it.
///
/// **Adafruit MatrixPortal S3**, from the board's own CircuitPython definition
/// (ports/espressif/boards/adafruit_matrixportal_s3/pins.c):
///
///     r1 42    g1 41    b1 40
///     r2 38    g2 39    b2 37
///     a  45    b  36    c  48    d  35    e  21
///     clk 2    lat 47   oe 14
///
/// A 1/32-scan panel needs `e`; on 1/16 panels that pin is free.
///
/// **Four of the MatrixPortal's lines sit on pins the generic S3 free set excludes**: b2 = 37,
/// b = 36 and d = 35 are octal-PSRAM pins, and a = 45 is a boot strap. That is not an error in the
/// map, it is what a board designer can do and a user cannot: the MatrixPortal ships quad-PSRAM,
/// which frees 33-37, and its strapping level is fixed by the board rather than by whatever the
/// panel drives. It is worth knowing because [PinsModule](../../core/system.md) flags those pins
/// from the chip-level table and will mark them, correctly, as a conflict it cannot see past.
///
/// **Waveshare ESP32-S3-RGB-Matrix** (SKU 34422), from WLED's `WAVESHARE_S3_PINOUT`:
///
///     r1  4    g1  5    b1  6
///     r2  7    g2 15    b2 16
///     a  18    b   8    c   3    d 42    e  9
///     clk 41   lat 40   oe  2
///
/// Do not confuse it with the Waveshare **ESP32-S3-Matrix**, a different product with an onboard
/// 8x8 WS2812 matrix and no HUB75 connector.
///
/// **One ordering trap runs through all of these.** WLED's array is
/// `{R1,G1,B1,R2,G2,B2,A,B,C,D,E,LAT,OE,CLK}`: latch and output-enable come BEFORE the clock, so a
/// map transcribed as `...CLK,LAT,OE` silently swaps three lines. The maps above are written in
/// this driver's own control order (clk, lat, oe) with that conversion already applied.
///
class Hub75Driver : public DriverBase {
public:
    /// Which board's wiring to use. Picking one PREFILLS the fourteen pins below; they stay visible
    /// and editable, because a prefill is a starting point rather than a lock, and a user with a
    /// hand-wired panel or a board variant needs to change one line without losing the other
    /// thirteen. Switching to Custom leaves whatever is there, so a tweak survives the switch.
    ///
    /// The `-generic` entries are not boards: they are a working set of pins from the chip's own
    /// free list, for someone wiring a bare module. Shown per chip, because a P4's free GPIOs are
    /// not an S3's.
    uint8_t boardSel = 0;    // index into kBoardOptions; 0 = MoonHub75

    // Every pin defaults to the board picked above. A soldered line is never guessed from nothing:
    // a blind default would land on octal-PSRAM, USB, UART0 or a strapping pin, and a 1/16 port
    // takes thirteen of the S3's sixteen clean LED-lane GPIOs. A board's published map is a
    // different matter, and that is what these presets carry.
    int8_t r1 = -1, g1 = -1, b1 = -1;
    int8_t r2 = -1, g2 = -1, b2 = -1;
    int8_t addrA = -1, addrB = -1, addrC = -1, addrD = -1, addrE = -1;
    int8_t clk = -1, lat = -1, oe = -1;

    /// The panel's own scan rate, which is NOT derivable from its size: two panels of identical
    /// dimensions can scan differently, so the user reads it off the panel and says which.
    uint8_t scanSel = 1;                 // index into kScanOptions

    /// Bit depth, and therefore the refresh tradeoff. Every plane costs a full scan of the panel, so
    /// this is the one control that trades color precision against flicker. It is the user's call
    /// rather than the driver's, and the docs carry the predicted refresh per geometry.
    uint8_t bitDepth = 4;

    /// Which silicon block drives the panel, where the chip offers a choice. NOT an automatic
    /// decision: a P4 has both LCD_CAM and Parlio and only one of each, so a board driving WS2812
    /// strips from one needs the panel on the other. Which way round is the user's business, and the
    /// sibling claim guard cannot guess it. Same shape as ParallelLedDriver's `peripheral`.
    uint8_t peripheralSel_ = 0;

    const char* tags() const override { return "🟦"; }   // 2D

    /// The peripheral block this driver holds, for the sibling claim guard in DriverBase: two live
    /// drivers on one LCD_CAM corrupt each other, and the guard already arbitrates that for the
    /// parallel LED drivers. Reported only while the bus is actually up, so a driver that failed to
    /// init does not phantom-claim the block away from a working sibling.
    LedHwBlock hwBlock() const override {
        if (!running_) return LedHwBlock::None;
        // WHICH block, not a guess: the user picks the peripheral, so claiming LcdCam while running
        // on Parlio both frees a block this driver is using and blocks one it is not.
        return backendIndex_[peripheralSel_ < backendOptionCount_ ? peripheralSel_ : 0] ==
                       platform::Hub75Backend::Parlio
                   ? LedHwBlock::Parlio
                   : LedHwBlock::LcdCam;
    }

    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    void defineDriverControls() override {
        buildBoardOptions();
        // Prefill on the FIRST definition, so a fresh driver arrives wired for the default board
        // rather than showing fourteen blanks. Not in the constructor: these option arrays are
        // declared below the members it would set, and C++ initializes in declaration order, so a
        // constructor body's work was overwritten by their own default initializers.
        //
        // Once only, tracked by a flag rather than by "are the pins still -1": a user who
        // deliberately unsets a pin must not have it refilled behind them on the next re-render.
        if (!prefilled_) { prefilled_ = true; applyBoard(); }
        controls_.addSelect("board", boardSel, boardOptions_, boardOptionCount_);

        // The six color lines and the address lines, in the order a HUB75 ribbon numbers them, so
        // the card reads like the connector the user is looking at. Always shown, whichever board is
        // picked: a preset fills them in, and the user can still correct one.
        // HIDDEN when a published board map is selected: those fourteen lines are soldered, so
        // showing them is fourteen rows of noise on a card nobody can act on. The generic sets and
        // Custom keep them, because those exist to be adjusted. Hidden controls stay BOUND: the
        // values still persist and still drive the panel, they are simply not rendered.
        const bool editable = pinsEditable();
        const char* const kPinNames[] = {"r1", "g1", "b1", "r2", "g2", "b2", "a", "b", "c",
                                         "d", "e", "clk", "lat", "oe"};
        int8_t* const kPinVars[] = {&r1, &g1, &b1, &r2, &g2, &b2, &addrA, &addrB, &addrC,
                                    &addrD, &addrE, &clk, &lat, &oe};
        for (size_t i = 0; i < sizeof(kPinNames) / sizeof(kPinNames[0]); i++) {
            controls_.addPin(kPinNames[i], *kPinVars[i]);
            controls_.setHidden(controls_.count() - 1, !editable);
        }

        buildBackendOptions();
        controls_.addSelect("peripheral", peripheralSel_, backendOptions_, backendOptionCount_);
        controls_.addSelect("scanRate", scanSel, kScanOptions, kScanCount);
        controls_.addControl("bitDepth", bitDepth, 2, 4);

        // The MEASURED refresh, not a calculation. The docs carry a predicted table; this is what
        // the panel actually achieved, which is what makes a report of "it flickers" into a number
        // someone can act on.
        controls_.addReadOnly("refresh", refreshStr_, sizeof(refreshStr_));
    }

    /// Picking a board writes its map into the pin controls. Called from onControlChanged so the
    /// card's fields update the moment the choice is made, rather than at the next rebuild.
    void onControlChanged(const char* name) override {
        if (name && std::strcmp(name, "board") == 0) {
            applyBoard();
            // rebuildControls(), not defineControls(): the former CLEARS the list first and fires
            // the schema-resync hook only when the schema really changed. Calling defineControls()
            // bare appended a second set of fourteen pin rows on every board change, because
            // defineControls is documented as "clear + addX" and mine was only doing the addX half.
            rebuildControls();
        }
        DriverBase::onControlChanged(name);
    }

    /// A control that changes the wire format or the geometry forces a rebuild; the rest (a pin
    /// typo being corrected, the depth) are handled by prepare() re-initializing anyway.
    bool affectsPrepare(const char* name) const override {
        static const char* const kRebuild[] = {
            "r1", "g1", "b1", "r2", "g2", "b2", "a", "b", "c", "d", "e",
            "clk", "lat", "oe", "scanRate", "bitDepth", "peripheral", "board",
        };
        for (const char* n : kRebuild) {
            if (std::strcmp(name, n) == 0) return true;
        }
        return false;
    }

    void release() override {
        platform::hub75Deinit(hub_);
        running_ = false;
    }

    void prepare() override {
        release();
        if (!layer_) {
            setStatus("no layer", Severity::Warning);
            return;
        }
        geo_.width = static_cast<uint16_t>(layer_->physicalWidth());
        geo_.height = static_cast<uint16_t>(layer_->physicalHeight());
        geo_.scanRate = kScanRates[scanSel < kScanCount ? scanSel : 0];
        geo_.bitDepth = bitDepth;

        if (!geo_.valid()) {
            // Say WHICH constraint failed. "Invalid geometry" sends a user round the houses; the
            // scan rate is nearly always the one they have wrong, because it is the one fact about
            // the panel that its dimensions do not reveal.
            std::snprintf(statusBuf_, sizeof(statusBuf_),
                          "%ux%u does not divide into 1/%u scan row pairs",
                          geo_.width, geo_.height, geo_.scanRate);
            setStatus(statusBuf_, Severity::Error);
            return;
        }

        platform::Hub75Pins pins;
        pins.r1 = toPin(r1); pins.g1 = toPin(g1); pins.b1 = toPin(b1);
        pins.r2 = toPin(r2); pins.g2 = toPin(g2); pins.b2 = toPin(b2);
        pins.a = toPin(addrA); pins.b = toPin(addrB); pins.c = toPin(addrC);
        pins.d = toPin(addrD); pins.e = toPin(addrE);
        pins.clk = toPin(clk); pins.lat = toPin(lat); pins.oe = toPin(oe);

        const platform::Hub75Backend backend =
            backendIndex_[peripheralSel_ < backendOptionCount_ ? peripheralSel_ : 0];
        if (!platform::hub75Init(hub_, backend, pins, geo_.width, geo_.height,
                                 geo_.scanRate, geo_.bitDepth)) {
            const char* why = platform::hub75LastError();
            setStatus(why ? why : "HUB75 init failed", Severity::Error);
            return;
        }
        if (!platform::hub75Start(hub_)) {
            const char* why = platform::hub75LastError();
            setStatus(why ? why : "the panel scan would not start", Severity::Error);
            platform::hub75Deinit(hub_);
            return;
        }
        // The correction scratch: one whole frame of PACKED RGB, because the encoder needs the
        // frame rather than one light at a time, and it reads three bytes a light whatever the
        // wiring emits. Cold path, and grow-only across rebuilds.
        ensureWire(static_cast<size_t>(geo_.width) * geo_.height * 3);
        if (!wire_) {
            setStatus("not enough memory for the correction buffer", Severity::Error);
            platform::hub75Deinit(hub_);
            return;
        }

        running_ = true;
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%ux%u, 1/%u scan, %u-bit (%s)",
                      geo_.width, geo_.height, geo_.scanRate, geo_.bitDepth,
                      platform::hub75Backend(hub_));
        setStatus(statusBuf_, Severity::Status);
    }

    void tick() MM_NONBLOCKING override {
        if (!running_ || !sourceBuffer_ || !sourceBuffer_->data()) return;
        uint8_t* out = platform::hub75Buffer(hub_);
        if (!out) return;

        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        const uint8_t outCh = correction_.outChannels;
        const nrOfLightsType lights =
            static_cast<nrOfLightsType>(geo_.width) * geo_.height;
        if (srcCh == 0 || outCh < 3 || lights == 0) return;
        if (sourceBuffer_->count() < lights) return;     // frame smaller than the panel
        if (!wire_ || wireCap_ < static_cast<size_t>(lights) * 3) return;   // not ready

        // Correct every light into the scratch, then encode the whole frame: the encoder needs the
        // packed frame rather than one light at a time, because a bit plane reads the pixel at
        // (x, r) and the one half a panel below it in the same pass.
        //
        // PACKED RGB, three bytes a light, whatever the wiring emits. A wider one (RGBW and up)
        // corrects into a one-light scratch and keeps the first three, the shape NdiDriver uses for
        // the same mismatch: correcting at an outCh stride while encoding at a 3-byte one reads
        // every light after the first from the wrong offset.
        const uint8_t* src = sourceBuffer_->data();
        if (outCh == 3) {
            for (nrOfLightsType i = 0; i < lights; i++) {
                correction_.apply(src + static_cast<size_t>(i) * srcCh,
                                  wire_ + static_cast<size_t>(i) * 3, srcCh);
            }
        } else {
            uint8_t scratch[8];   // outChannels is bounded by the wiring; 8 covers RGBWW and up
            const uint8_t n = outCh <= sizeof(scratch) ? outCh : static_cast<uint8_t>(sizeof(scratch));
            for (nrOfLightsType i = 0; i < lights; i++) {
                correction_.apply(src + static_cast<size_t>(i) * srcCh, scratch, srcCh);
                uint8_t* d = wire_ + static_cast<size_t>(i) * 3;
                d[0] = scratch[0]; d[1] = scratch[1]; d[2] = n > 2 ? scratch[2] : 0;
            }
        }

        // Encode straight into the DMA buffer the panel is scanning. There is no double buffer: the
        // scan is continuous, so a frame written mid-scan shows its old top and new bottom for one
        // pass. That tear is far cheaper than a second 256 KB buffer, and at 300+ Hz it is invisible.
        hub75Encode(wire_, out, geo_);
    }

    /// The scratch holds packed RGB, so outChannels does not size it: a wider wiring is compacted
    /// to three bytes a light on the way in. Kept as an override because the geometry can change
    /// under it, and resizing before the next tick reads it is the rule RmtLedDriver follows.
    void onCorrectionChanged() override {
        if (!running_) return;
        ensureWire(static_cast<size_t>(geo_.width) * geo_.height * 3);
    }

    /// Publishes the measured refresh. The annotation STAYS: the base declares tick1s
    /// non-blocking, so dropping it here widens the contract and the compiler rejects the override.
    /// The snprintf under it is the same accepted trade PanelCardDriver's writeLinkStatus makes:
    /// a formatted status once a second, on a hook that fires at 1 Hz.
    void tick1s() MM_NONBLOCKING override {
        if (!running_) return;
        const uint16_t hz = platform::hub75RefreshHz(hub_);
        if (hz == lastRefresh_) return;      // never re-serialize an unchanged value on the 1 Hz tick
        lastRefresh_ = hz;
        std::snprintf(refreshStr_, sizeof(refreshStr_), "%u Hz", hz);
    }

private:
    /// Offer only the backends this silicon has AND that can carry this geometry. Both questions at
    /// once, because a user cannot act on them separately: Parlio exists on a P4 but caps at 65,535
    /// bytes a transfer, so listing it for a 16-panel wall would offer a choice that fails at init.
    ///
    /// Rebuilt on every defineDriverControls, and the selection re-points by LABEL rather than index,
    /// so a geometry change that removes a backend cannot silently select a different one.
    void buildBackendOptions() {
        const char* current = (peripheralSel_ < backendOptionCount_)
                                  ? backendOptions_[peripheralSel_] : nullptr;
        // The frame this geometry would need. Zero when the layer is not known yet, which asks the
        // platform only whether the silicon exists.
        size_t bytes = 0;
        if (layer_) {
            Hub75Geometry probe;
            probe.width = static_cast<uint16_t>(layer_->physicalWidth());
            probe.height = static_cast<uint16_t>(layer_->physicalHeight());
            probe.scanRate = kScanRates[scanSel < kScanCount ? scanSel : 0];
            probe.bitDepth = bitDepth;
            if (probe.valid()) bytes = probe.frameBytes();
        }

        backendOptionCount_ = 0;
        for (uint8_t i = 0; i < kBackendCount; i++) {
            const auto b = static_cast<platform::Hub75Backend>(i);
            if (!platform::hub75BackendAvailable(b, bytes)) continue;
            backendOptions_[backendOptionCount_] = platform::hub75BackendLabel(b);
            backendIndex_[backendOptionCount_] = b;
            backendOptionCount_++;
        }
        if (backendOptionCount_ == 0) {      // no usable backend (desktop, classic ESP32)
            backendOptions_[0] = "(none)";
            backendIndex_[0] = platform::Hub75Backend::LcdCam;
            backendOptionCount_ = 1;
        }
        uint8_t sel = 0;
        if (current) {
            for (uint8_t k = 0; k < backendOptionCount_; k++) {
                if (std::strcmp(backendOptions_[k], current) == 0) { sel = k; break; }
            }
        }
        peripheralSel_ = sel;
    }

    /// One board's HUB75 wiring, in ribbon order. `-1` means the board leaves that line to the user.
    struct BoardPins {
        const char* label;
        int8_t r1, g1, b1, r2, g2, b2;
        int8_t a, b, c, d, e;
        int8_t clk, lat, oe;
        bool (*onThisChip)();   // null = offered everywhere
        /// Are these pins the user's to change? False for a published board map: those lines are
        /// soldered, so showing fourteen uneditable rows is noise on the card. True for the generic
        /// sets and Custom, which exist precisely to be adjusted.
        bool editable;
    };

    // The platform already names the chip; an earlier draft inferred it from GPIO counts, which was
    // a guess dressed as a test.
    static bool chipIsS3()  { return platform::isEsp32S3; }
    static bool chipIsP4()  { return platform::isEsp32P4; }
    static bool chipIsS31() { return platform::isEsp32S31; }

    /// The boards this driver knows, and the per-chip generic sets.
    ///
    /// MoonHub75 leads because it is MoonModules' own board (a passive adapter from a Lilygo T7-S3),
    /// it is the one the request naming these boards calls out, and its map is the only one taken
    /// from a schematic this project controls. That is a reason to default to it, not evidence that
    /// it is the most used: nobody has counted.
    ///
    /// The `-generic` rows are NOT boards. They are fourteen pins from the chip's own documented free
    /// list (gpio-usage.md), so someone wiring a bare module has a set that works rather than a blank
    /// card. They are still editable, because a generic set cannot know what else is on the board.
    static constexpr BoardPins kBoards[] = {
        // MoonHub75 PCB (Lilygo T7-S3). MOONHUB75/README.md names the upper half R0/G0/B0.
        {"MoonHub75",   1,  5,  6,   7, 13,  9,  16, 48, 47, 21, 38,  18,  8,  4, chipIsS3, false},
        // Adafruit MatrixPortal S3, from the board's own CircuitPython pins.c.
        {"MatrixPortal S3", 42, 41, 40,  38, 39, 37,  45, 36, 48, 35, 21,   2, 47, 14, chipIsS3, false},
        // Waveshare ESP32-S3-RGB-Matrix (SKU 34422), from WLED's WAVESHARE_S3_PINOUT constant.
        // Its gpio array is {R1,G1,B1,R2,G2,B2,A,B,C,D,E,LAT,OE,CLK}, so LAT and OE come BEFORE CLK,
        // so reading it as ...CLK,LAT,OE swaps all three.
        {"Waveshare RGB Matrix", 4,  5,  6,   7, 15, 16,  18,  8,  3, 42,  9,  41, 40,  2, chipIsS3, false},
        // Generic sets: the first fourteen of each chip's free GPIOs, in ribbon order.
        {"S3 generic",  4,  5,  6,   7,  8,  9,  10, 11, 12, 13, 14,  15, 16, 17, chipIsS3, true},
        {"P4 generic", 20, 21, 22,  23, 24, 25,  26, 27, 32, 33, 39,  40, 41, 42, chipIsP4, true},
        // The S31's free set is board-specific (its header is committed to on-board Ethernet, codec,
        // SD and USB-host), and the hardware reference says not to guess. So it is offered with the
        // pins left unset rather than with invented ones.
        {"S31 generic", -1, -1, -1,  -1, -1, -1,  -1, -1, -1, -1, -1,  -1, -1, -1, chipIsS31, true},
        // Custom keeps whatever is in the fields: the escape hatch for a hand-wired panel.
        {"Custom",     -1, -1, -1,  -1, -1, -1,  -1, -1, -1, -1, -1,  -1, -1, -1, nullptr, true},
    };
    static constexpr uint8_t kBoardCount = sizeof(kBoards) / sizeof(kBoards[0]);

    /// Offer the boards that make sense on this chip: a P4-generic row on an S3 would be fourteen
    /// pins that chip does not have.
    void buildBoardOptions() {
        const char* current = (boardSel < boardOptionCount_) ? boardOptions_[boardSel] : nullptr;
        boardOptionCount_ = 0;
        for (uint8_t i = 0; i < kBoardCount; i++) {
            // A build that is none of the three chips is a desktop build, and there the per-chip
            // filter would hide EVERY generic row (correct on hardware), and it makes the feature
            // invisible exactly where people configure a device before flashing one. So a desktop
            // offers all of them; real silicon still sees only its own.
            //
            // Derived rather than asked: there is no isDesktop flag, and adding one to name a
            // negative would be a worse seam than reading the three that already exist.
            constexpr bool anyKnownChip =
                platform::isEsp32S3 || platform::isEsp32P4 || platform::isEsp32S31;
            if (anyKnownChip && kBoards[i].onThisChip && !kBoards[i].onThisChip()) continue;
            boardOptions_[boardOptionCount_] = kBoards[i].label;
            boardIndex_[boardOptionCount_] = i;
            boardOptionCount_++;
        }
        // Re-point by LABEL, so a filtered list cannot silently select a different board.
        uint8_t sel = 0;
        if (current) {
            for (uint8_t k = 0; k < boardOptionCount_; k++) {
                if (std::strcmp(boardOptions_[k], current) == 0) { sel = k; break; }
            }
        }
        boardSel = sel;
    }

    /// Are the pin controls the user's to edit, for the board currently selected?
    bool pinsEditable() const {
        if (boardSel >= boardOptionCount_) return true;   // no board resolved: never hide
        return kBoards[boardIndex_[boardSel]].editable;
    }

    /// Write the chosen board's map into the pin controls. Custom writes nothing: it is the "leave
    /// my wiring alone" choice, so switching to it after an edit keeps the edit.
    void applyBoard() {
        if (boardSel >= boardOptionCount_) return;
        const BoardPins& b = kBoards[boardIndex_[boardSel]];
        if (std::strcmp(b.label, "Custom") == 0) return;
        r1 = b.r1; g1 = b.g1; b1 = b.b1;
        r2 = b.r2; g2 = b.g2; b2 = b.b2;
        addrA = b.a; addrB = b.b; addrC = b.c; addrD = b.d; addrE = b.e;
        clk = b.clk; lat = b.lat; oe = b.oe;
    }

    const char* boardOptions_[kBoardCount] = {};
    uint8_t boardIndex_[kBoardCount] = {};
    uint8_t boardOptionCount_ = 0;
    bool prefilled_ = false;   // the board map is written once, not on every re-render

    static constexpr uint8_t kBackendCount = 2;
    const char* backendOptions_[kBackendCount + 1] = {};
    platform::Hub75Backend backendIndex_[kBackendCount + 1] = {};
    uint8_t backendOptionCount_ = 0;

    static constexpr uint8_t kScanCount = 3;
    static constexpr const char* kScanOptions[kScanCount] = {"1/8", "1/16", "1/32"};
    static constexpr uint8_t kScanRates[kScanCount] = {8, 16, 32};

    /// A Pin control is int8_t with -1 for unset; the platform seam takes uint16_t with 0xFFFF,
    /// because a GPIO number can exceed 127 on the P4. One conversion, in one place.
    static uint16_t toPin(int8_t p) {
        return p < 0 ? 0xFFFF : static_cast<uint16_t>(p);
    }

    platform::Hub75Handle hub_{};
    Hub75Geometry geo_{};
    Buffer* sourceBuffer_ = nullptr;
    bool running_ = false;
    uint16_t lastRefresh_ = 0xFFFF;      // not 0: the first real 0 must still publish
    char statusBuf_[64] = {};
    char refreshStr_[16] = {};
};

}  // namespace mm
