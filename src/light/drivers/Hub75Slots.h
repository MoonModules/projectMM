#pragma once

#include <cstddef>   // size_t
#include <cstdint>

namespace mm {

/// @defgroup Hub75Slots HUB75 scan encoder: the bit-plane wire format
/// @{
///
/// HUB75 encode: the contract between Hub75Driver (domain) and a HUB75 port
/// (peripheral), named for the wire unit it builds: one pixel clock = one SLOT =
/// one byte on the parallel bus. Sibling of ParallelSlots.h, which does the same
/// job for WS2812. Pure data transform, no platform include, so the host test
/// (unit_Hub75Slots.cpp) pins it with no ESP32 and no panel.
///
/// **A HUB75 panel is scanned, not addressed.** Two rows are lit at once: the
/// upper half-panel through R1/G1/B1 and the lower through R2/G2/B2, selected by
/// a row address on A/B/C(/D/E). The controller walks every scan row in turn, and
/// persistence of vision does the rest. So a 64-row panel has 32 scan rows, and
/// row `r` drives panel rows `r` and `r + 32` together.
///
/// **Brightness is time, not amplitude.** A HUB75 pixel is a switch: on or off.
/// Intensity is meant to come from BINARY CODED MODULATION: plane `p` displayed for
/// 2^p time units, so a value lights its planes for a total proportional to itself.
/// That weighting is the peripheral's output-enable window, and it is NOT built yet:
/// today every plane shows for the same time, so the ramp is flat and bit 7 is worth
/// no more than bit 0 (backlog-light.md carries the fix).
///
/// Either way the encoder stores each plane ONCE. Emitting plane `p` 2^p times is the
/// obvious reading and it is wrong: 255 passes at 8-bit is 1,060,800 bytes for a single
/// 64x64 panel, where storing once is 33,280. Depth costs refresh and memory linearly:
/// every plane is one full scan of the panel.
///
/// Wire layout of one encoded frame, outermost first:
///
///   for each bit plane p (0 = least significant)
///     for each scan row r
///       for each column x          -> one bus byte per column: the six color
///                                     bits for (x, r) and (x, r + rows)
///       one blanking byte           -> OE high (dark) while the row address
///                                     changes and the shift register latches
///
/// The row address and the control lines ride the SAME bus byte as the color
/// bits, because the peripheral clocks one word per slot and a HUB75 panel wants
/// address + data simultaneously. `Hub75Layout` says which bus bit each line sits
/// on, which is the one thing that differs between a board's wiring and ours.
///
/// Prior art: the HUB75 lineage generally (mrcodetastic/ESP32-HUB75-MatrixPanel-DMA,
/// hzeller/rpi-rgb-led-matrix, ESPHome's hub75 component). The scan/BCM structure
/// is the panel's, not any library's; the encoder below is written from the panel
/// behavior rather than transcribed.

/// Which bus bit each HUB75 line occupies. The peripheral drives one byte per
/// slot, so every line is a bit position in that byte rather than a GPIO here:
/// the platform layer maps bit -> GPIO when it builds the bus.
///
/// Defaults are the conventional order and cost nothing to override: a board that
/// wires the panel differently changes these, and the encoder is unchanged.
struct Hub75Layout {
    uint8_t r1 = 0, g1 = 1, b1 = 2;    // upper half-panel color bits
    uint8_t r2 = 3, g2 = 4, b2 = 5;    // lower half-panel color bits
    uint8_t a = 8, b = 9, c = 10, d = 11, e = 12;   // row address bits
    uint8_t lat = 13;                  // latch: shift register -> output drivers
    uint8_t oe = 14;                   // output enable, ACTIVE LOW (high = dark)
};

/// The geometry one encode needs. `scanRate` is the panel's own (8, 16 or 32) and
/// is NOT derivable from the height: two panels of identical dimensions can scan
/// differently, which is why it is a user control rather than a calculation.
struct Hub75Geometry {
    /// One pixel clock carries one 16-bit bus word. Sixteen rather than eight because the
    /// control lines live at bits 8-14 (see Hub75Layout): an 8-bit slot drops the row
    /// address, the latch and OE, which is a panel that never lights.
    static constexpr size_t kBytesPerSlot = 2;

    uint16_t width = 64;
    uint16_t height = 64;
    uint8_t  scanRate = 16;    // 1/8, 1/16 or 1/32
    uint8_t  bitDepth = 4;     // 2..4; every plane costs a full scan pass

    /// Scan rows: how many address steps one plane walks. The panel's scanRate IS
    /// that count: a 1/16 panel steps 16 addresses whatever its height.
    uint16_t scanRows() const { return scanRate; }

    /// Rows driven per address step. Two on the common panel (the upper/lower pair),
    /// but a 64-row 1/16 panel drives FOUR: two pairs, the second offset by 2 x
    /// scanRate. Encoding only the first pair would leave three quarters of such a
    /// panel dark, so this is what the encoder walks rather than assuming two.
    uint16_t rowsPerScan() const { return scanRate ? height / scanRate : 0; }

    /// Is this geometry encodable? The rows must divide evenly into address steps,
    /// and each step must drive an even number of rows (the panel lights an upper
    /// and a lower row together, so an odd count has no pair for the last one).
    bool valid() const {
        if (width == 0 || height == 0) return false;
        // Capped at 4 while every plane shows for the same time: an unweighted plane 5 and
        // above costs a full scan pass and a fifth of the frame buffer for a difference the
        // eye cannot find. The cap lifts when the planes are weighted (backlog-light.md).
        if (bitDepth < 2 || bitDepth > 4) return false;
        if (scanRate == 0 || height % scanRate != 0) return false;
        return rowsPerScan() % 2 == 0;
    }

    /// Slots one encoded frame occupies: every plane, every scan row, every column, plus one
    /// blanking slot per row. Each plane is stored ONCE. Binary coded modulation weights the
    /// planes in TIME rather than in memory (the OE window for plane p is 2^p long), because
    /// storing plane p 2^p times would multiply this buffer by 255 at 8-bit: over a megabyte
    /// for a single 64x64 panel, which no ESP32 can hold. The weighting is the peripheral's
    /// job, and until it does it every bit is worth the same (see backlog-light.md).
    size_t frameSlots() const {
        const uint16_t pairs = rowsPerScan() / 2;   // color passes per address step
        return static_cast<size_t>(bitDepth) * scanRows() *
               (static_cast<size_t>(width) * pairs + 1);
    }

    /// Bytes one encoded frame occupies. TWO per slot: the bus is 16 bits wide, because
    /// the address, latch and OE lines sit above bit 7 and a byte would leave them off
    /// the wire. This is the number that decides which peripheral can carry the panel
    /// (Parlio caps at 65,535), and it is the one home for the size: the platform asks
    /// rather than recomputing it.
    size_t frameBytes() const { return frameSlots() * kBytesPerSlot; }
};

/// Encode one rendered RGB frame into a HUB75 bit-plane buffer.
///
/// `rgb` is the rendered frame, 3 bytes per light, indexed `(y * width + x) * 3`:
/// the layout every driver receives from the Layer. `out` must hold at least
/// `geo.frameBytes()`.
///
/// Returns the bytes written, or 0 when the geometry is unusable (a zero
/// dimension, a scanRate that does not divide the height, a depth out of range).
/// Zero rather than a partial write: a half-encoded frame displayed on a panel is
/// a worse failure than a dark one, and the caller reports the cause.
inline size_t hub75Encode(const uint8_t* rgb, uint8_t* out,
                          const Hub75Geometry& geo, const Hub75Layout& lay = {}) {
    if (!rgb || !out) return 0;
    if (!geo.valid()) return 0;

    const uint16_t rows = geo.scanRows();
    const uint16_t pairs = geo.rowsPerScan() / 2;   // color passes per address step
    const uint16_t pairSpan = geo.height / 2;       // upper row pairs with row + span
    // The address lines, indexed so a scan row's bits are set by position. Only the
    // first `addrBits` are consulted, so a 1/8 panel never touches D or E and a board
    // that leaves them unwired is unaffected.
    const uint8_t addr[5] = {lay.a, lay.b, lay.c, lay.d, lay.e};
    uint8_t addrBits = 3;                       // 1/8 scan
    if (geo.scanRate > 8) addrBits = 4;         // 1/16
    if (geo.scanRate > 16) addrBits = 5;        // 1/32

    size_t w = 0;
    for (uint8_t plane = 0; plane < geo.bitDepth; plane++) {
        // The bit this plane reads. Depth < 8 keeps the HIGH bits: dropping the low
        // ones costs precision, where dropping the high ones would cost range and
        // make a bright pixel dim.
        const uint8_t shift = static_cast<uint8_t>(8 - geo.bitDepth + plane);
        for (uint16_t r = 0; r < rows; r++) {
            for (uint16_t pair = 0; pair < pairs; pair++)
            for (uint16_t x = 0; x < geo.width; x++) {
                // Pair `p` of address step `r` is panel row r + p*scanRate, paired with
                // the row half a panel below it. On the common two-row panel this is
                // simply r and r + height/2.
                const uint16_t upRow = static_cast<uint16_t>(r + pair * geo.scanRate);
                const size_t up = (static_cast<size_t>(upRow) * geo.width + x) * 3;
                const size_t lo = (static_cast<size_t>(upRow + pairSpan) * geo.width + x) * 3;
                uint16_t word = 0;
                if ((rgb[up + 0] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.r1);
                if ((rgb[up + 1] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.g1);
                if ((rgb[up + 2] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.b1);
                if ((rgb[lo + 0] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.r2);
                if ((rgb[lo + 1] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.g2);
                if ((rgb[lo + 2] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.b2);
                // The row address rides every column byte, not only the blanking one:
                // the address lines must be stable for the whole row, and a panel that
                // sees them change mid-row ghosts the previous row into this one.
                for (uint8_t bit = 0; bit < addrBits; bit++) {
                    if ((r >> bit) & 1) word |= static_cast<uint16_t>(1u << addr[bit]);
                }
                // Little-endian, matching how the peripheral latches a 16-bit bus word.
                out[w++] = static_cast<uint8_t>(word & 0xFF);
                out[w++] = static_cast<uint8_t>((word >> 8) & 0xFF);
            }
            // The blanking byte: OE HIGH (panel dark) while the latch pulses. Dark
            // first, then latch, is what stops the row that is about to be addressed
            // from briefly showing the previous row's data.
            uint16_t blank = static_cast<uint16_t>(1u << lay.oe) |
                             static_cast<uint16_t>(1u << lay.lat);
            for (uint8_t bit = 0; bit < addrBits; bit++) {
                if ((r >> bit) & 1) blank |= static_cast<uint16_t>(1u << addr[bit]);
            }
            out[w++] = static_cast<uint8_t>(blank & 0xFF);
            out[w++] = static_cast<uint8_t>((blank >> 8) & 0xFF);
        }
    }
    return w;
}

/// @}

}  // namespace mm
