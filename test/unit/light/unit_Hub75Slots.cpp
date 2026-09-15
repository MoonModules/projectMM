// @module Hub75Driver
// @also Hub75Slots

#include "doctest.h"
#include "light/drivers/Hub75Slots.h"

#include <cstring>
#include <vector>

// The success spec for the HUB75 bit-plane encode: a known RGB frame -> the exact
// bus-byte stream. Pins the three things a panel cannot tell us it got wrong, and
// which no amount of looking at a wall diagnoses:
//
//   1. the row address rides EVERY column slot, not only the blanking one
//   2. a bit plane reads the HIGH bits, so depth < 8 loses precision not range
//   3. a panel driving more than two rows per address step encodes every pair
//   4. every slot is a 16-BIT bus word, so the address/latch/OE lines above bit 7
//      actually reach the wire (an 8-bit slot is a panel that never lights)
//   5. every plane is stored once: the 2^p weighting is the peripheral's OE window,
//      not 2^p copies in the buffer (that would be a megabyte for one panel)
//
// Pure data, no platform: this runs in CI with no ESP32 and no panel, which is the
// whole point of keeping the encoder a free function on plain buffers.

namespace {

// A frame of `w * h` lights, every channel zero, so a test sets only what it means.
std::vector<uint8_t> blackFrame(uint16_t w, uint16_t h) {
    return std::vector<uint8_t>(static_cast<size_t>(w) * h * 3, 0);
}

void setPixel(std::vector<uint8_t>& f, uint16_t w, uint16_t x, uint16_t y,
              uint8_t r, uint8_t g, uint8_t b) {
    const size_t i = (static_cast<size_t>(y) * w + x) * 3;
    f[i + 0] = r; f[i + 1] = g; f[i + 2] = b;
}

/// Slot `n` as the 16-bit bus word the peripheral latches, little-endian. Every check below
/// reads through this rather than indexing bytes: a slot is a WORD, and testing one byte of
/// it is how an encoder that drops the high lines passes its own test.
uint16_t slot(const std::vector<uint8_t>& out, size_t n) {
    return static_cast<uint16_t>(out[n * 2] | (out[n * 2 + 1] << 8));
}

}  // namespace

TEST_CASE("HUB75 encode: the frame is bit-plane major, one blanking byte per row") {
    mm::Hub75Geometry geo;
    geo.width = 8; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 2;

    // 2 planes x 2 scan rows x (8 columns x 1 pair + 1 blank) = 36 slots, and 2 bytes
    // each because the bus is 16 bits wide.
    CHECK(geo.frameSlots() == 36);
    CHECK(geo.frameBytes() == 72);

    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0xAA);
    CHECK(mm::hub75Encode(rgb.data(), out.data(), geo) == geo.frameBytes());
}

TEST_CASE("HUB75 encode: the row address rides every column byte") {
    // The failure this prevents: addressing only on the blanking byte lets the panel
    // see the address change while color data is still clocking, which ghosts the
    // previous row into this one. It looks like a blur on the wall and like nothing
    // at all in the buffer, so it is pinned here.
    mm::Hub75Geometry geo;
    geo.width = 4; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 2;
    mm::Hub75Layout lay;   // the DEFAULT layout: a=8, b=9, above the low byte

    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // The default layout is the one that must work: its address, latch and OE sit at bits
    // 8-14, so an encoder emitting a BYTE per slot drops them and the panel never lights.
    // Reading the whole 16-bit word is what catches that, and only the whole word does.
    // Row 0 of plane 0: columns carry no address bit.
    for (uint16_t x = 0; x < geo.width; x++) CHECK((slot(out, x) & (1u << lay.a)) == 0);
    // Row 1: every column slot carries address bit 0, not only the blank.
    const size_t row1 = geo.width + 1;   // past row 0's columns and its blanking slot
    for (uint16_t x = 0; x < geo.width; x++) {
        CHECK((slot(out, row1 + x) & (1u << lay.a)) != 0);
    }

    // A board is free to re-map the lines into the low byte, and that must keep working:
    // Hub75Layout exists so a different wiring costs a struct rather than an encoder.
    mm::Hub75Layout low;
    low.r1 = 0; low.g1 = 1; low.b1 = 2; low.r2 = 3; low.g2 = 4; low.b2 = 5;
    low.a = 6; low.b = 7; low.c = 7; low.d = 7; low.e = 7;
    low.lat = 6; low.oe = 7;
    std::vector<uint8_t> out2(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out2.data(), geo, low) == geo.frameBytes());
    for (uint16_t x = 0; x < geo.width; x++) CHECK((slot(out2, x) & (1u << low.a)) == 0);
    for (uint16_t x = 0; x < geo.width; x++) {
        CHECK((slot(out2, row1 + x) & (1u << low.a)) != 0);
    }
}

TEST_CASE("HUB75 encode: a color bit lands on its own line, for both half-panels") {
    // A walking-one per color line: the test that catches a swapped pair (red where
    // green should be) and an upper/lower mix-up. A panel shows both as "wrong
    // colors", which is the least diagnostic symptom there is.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 4; geo.scanRate = 2; geo.bitDepth = 4;
    mm::Hub75Layout lay;

    struct Case { const char* name; uint8_t r, g, b; uint8_t bit; bool lower; };
    const Case cases[] = {
        {"upper red",   0xFF, 0, 0,    lay.r1, false},
        {"upper green", 0, 0xFF, 0,    lay.g1, false},
        {"upper blue",  0, 0, 0xFF,    lay.b1, false},
        {"lower red",   0xFF, 0, 0,    lay.r2, true},
        {"lower green", 0, 0xFF, 0,    lay.g2, true},
        {"lower blue",  0, 0, 0xFF,    lay.b2, true},
    };

    for (const auto& c : cases) {
        CAPTURE(c.name);
        auto rgb = blackFrame(geo.width, geo.height);
        // Scan row 0 drives panel row 0 (upper) and row 0 + height/2 = 2 (lower).
        setPixel(rgb, geo.width, 0, c.lower ? 2 : 0, c.r, c.g, c.b);

        std::vector<uint8_t> out(geo.frameBytes(), 0);
        REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

        // At full value every plane carries the bit; column 0 of scan row 0 of plane 0.
        CHECK((slot(out, 0) & (1u << c.bit)) != 0);
        // and no OTHER color line is set by it.
        const uint8_t colorBits = static_cast<uint8_t>(
            (1u << lay.r1) | (1u << lay.g1) | (1u << lay.b1) |
            (1u << lay.r2) | (1u << lay.g2) | (1u << lay.b2));
        CHECK((slot(out, 0) & colorBits) == (1u << c.bit));
    }
}

TEST_CASE("HUB75 encode: depth below 8 keeps the HIGH bits") {
    // Dropping the low bits costs precision; dropping the high ones would cost RANGE,
    // so a bright pixel would come out dim. The difference is invisible in a buffer
    // dump and obvious on a wall, which is the wrong way round.
    mm::Hub75Geometry geo;
    geo.width = 1; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    mm::Hub75Layout lay;

    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 0, 0xC0, 0, 0);   // 1100 0000: both top bits set

    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // Both planes of a 2-bit depth read bits 6 and 7, so both carry red.
    const size_t plane0 = 0;
    // Plane 1 starts after plane 0's single pass (2^0 = 1), in SLOTS.
    const size_t plane1 = geo.scanRows() * (geo.width + 1);
    CHECK((slot(out, plane0) & (1u << lay.r1)) != 0);
    CHECK((slot(out, plane1) & (1u << lay.r1)) != 0);

    // 0x30 (0011 0000) is in the LOW half: at 2-bit depth it reads as black, which is
    // the precision loss the depth control trades for refresh.
    auto dim = blackFrame(geo.width, geo.height);
    setPixel(dim, geo.width, 0, 0, 0x30, 0, 0);
    std::vector<uint8_t> out2(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(dim.data(), out2.data(), geo, lay) == geo.frameBytes());
    CHECK((slot(out2, plane0) & (1u << lay.r1)) == 0);
    CHECK((slot(out2, plane1) & (1u << lay.r1)) == 0);
}

TEST_CASE("HUB75 encode: a panel driving four rows per address step encodes every pair") {
    // A 64-row 1/16 panel steps 16 addresses and drives FOUR rows each: two pairs, the
    // second offset by 2 x scanRate. An encoder assuming two rows would leave three
    // quarters of the panel dark, and the geometry is common enough to matter.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 8; geo.scanRate = 2; geo.bitDepth = 4;
    mm::Hub75Layout lay;

    CHECK(geo.rowsPerScan() == 4);
    // 4 planes x 2 scan rows x (2 columns x 2 pairs + 1 blank) = 40 slots, 80 bytes.
    CHECK(geo.frameSlots() == 40);
    CHECK(geo.frameBytes() == 80);

    auto rgb = blackFrame(geo.width, geo.height);
    setPixel(rgb, geo.width, 0, 2, 0xFF, 0, 0);   // row 2 = address step 0, pair 1, upper

    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    // Pair 0 (rows 0 and 4) is black; pair 1 (rows 2 and 6) carries the red.
    CHECK((slot(out, 0) & (1u << lay.r1)) == 0);              // pair 0, column 0
    CHECK((slot(out, geo.width) & (1u << lay.r1)) != 0);      // pair 1, column 0
}

TEST_CASE("HUB75 encode: the blanking byte blanks before it latches") {
    // OE high (dark) AND latch in the same byte: the panel must be dark while the
    // shift register hands its row to the output drivers, or the row being addressed
    // briefly shows the previous row's data. That ghost is what this pins.
    mm::Hub75Geometry geo;
    geo.width = 2; geo.height = 2; geo.scanRate = 1; geo.bitDepth = 2;
    mm::Hub75Layout lay;
    lay.lat = 6; lay.oe = 7;   // both in the low byte, so the encoded byte shows them

    auto rgb = blackFrame(geo.width, geo.height);
    std::vector<uint8_t> out(geo.frameBytes(), 0);
    REQUIRE(mm::hub75Encode(rgb.data(), out.data(), geo, lay) == geo.frameBytes());

    const size_t blank = geo.width;   // the byte after the columns
    CHECK((slot(out, blank) & (1u << lay.oe)) != 0);    // dark
    CHECK((slot(out, blank) & (1u << lay.lat)) != 0);   // and latching
}

TEST_CASE("HUB75 encode: an unusable geometry writes nothing") {
    // Zero rather than a partial write. A half-encoded frame on a panel is a worse
    // failure than a dark one, and the driver turns this into a status the user reads.
    auto rgb = blackFrame(8, 8);
    std::vector<uint8_t> out(4096, 0xAA);

    mm::Hub75Geometry odd;      // 3 rows per address step: no pair for the third
    odd.width = 8; odd.height = 6; odd.scanRate = 2; odd.bitDepth = 4;
    CHECK(odd.valid() == false);
    CHECK(mm::hub75Encode(rgb.data(), out.data(), odd) == 0);

    mm::Hub75Geometry indivisible;
    indivisible.width = 8; indivisible.height = 7; indivisible.scanRate = 2;
    CHECK(mm::hub75Encode(rgb.data(), out.data(), indivisible) == 0);

    mm::Hub75Geometry deep;   // above the 4-bit cap, which is what valid() enforces
    deep.width = 8; deep.height = 8; deep.scanRate = 4; deep.bitDepth = 9;
    CHECK(mm::hub75Encode(rgb.data(), out.data(), deep) == 0);

    mm::Hub75Geometry ok;
    ok.width = 8; ok.height = 8; ok.scanRate = 4; ok.bitDepth = 4;
    CHECK(mm::hub75Encode(nullptr, out.data(), ok) == 0);
    CHECK(mm::hub75Encode(rgb.data(), nullptr, ok) == 0);

    // Nothing was written by any of the refusals.
    CHECK(out[0] == 0xAA);
}

TEST_CASE("HUB75 frame size is what decides the peripheral") {
    // The arithmetic the plan's memory table rests on, pinned so a change to the wire
    // format cannot silently move the Parlio cliff. Parlio's single-shot cap is
    // 65,535 bytes; the i80/LCD_CAM path allocates from PSRAM and has no such wall.
    // Two bytes a slot: the bus is 16 bits wide, which is what doubles these against a
    // byte-per-slot encoder that could not reach the address lines at all.
    // 4-bit throughout: the deepest the driver offers while planes are unweighted.
    mm::Hub75Geometry one;    // one 64x64 panel, 1/32 scan
    one.width = 64; one.height = 64; one.scanRate = 32; one.bitDepth = 4;
    CHECK(one.frameBytes() == 4 * 32 * (64 * 1 + 1) * 2);      // 16,640
    CHECK(one.frameBytes() < 65535u);                          // Parlio carries one panel

    mm::Hub75Geometry four;   // 128x128, 1/32 scan
    four.width = 128; four.height = 128; four.scanRate = 32; four.bitDepth = 4;
    CHECK(four.frameBytes() == 4 * 32 * (128 * 2 + 1) * 2);    // 65,792
    CHECK(four.frameBytes() > 65535u);                         // four panels miss the cap by 257 B

    mm::Hub75Geometry sixteen;   // 256x256, 1/32 scan
    sixteen.width = 256; sixteen.height = 256; sixteen.scanRate = 32; sixteen.bitDepth = 4;
    CHECK(sixteen.frameBytes() == 4 * 32 * (256 * 4 + 1) * 2);  // 262,400
    CHECK(sixteen.frameBytes() > 65535u);                       // Parlio cannot carry it
}
