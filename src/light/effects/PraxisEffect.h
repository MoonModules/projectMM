#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel
#include "core/math8.h"           // beatsin16

#include <cstdint>

namespace mm {

// Praxis: a flowing, palette-coloured field whose hue at each pixel is driven by two
// independently-oscillating "mutators". A slow macro mutator and a faster micro mutator
// (each a beatsin16 sweeping a tight high range) combine with the pixel's (x, y) position
// and a steadily-advancing hue base, so the colour pattern continually stretches, shears,
// and rolls across the grid. The micro mutator divides the spatial term (so it sets the
// pattern's spatial "frequency"), while the macro mutator multiplies the y·x cross term
// (so it warps the field), and huebase = elapsed/40 scrolls the whole thing through the
// palette over time.
//
// Prior art: MoonLight's Praxis effect (E_MoonModules / MoonModules). The two-mutator
// oscillator model (macro = beatsin16 in [min<<8, max<<8], micro = beatsin16 in [min, max])
// and the per-pixel hue = huebase + (x + y·macro·x)/(micro+1) are reproduced exactly here,
// written fresh on EffectBase + the shared draw / palette / math8 primitives. Our beatsin16
// takes the current time (elapsed()) as its second argument, matching the lib8tion shape
// (bpm, timebase) with the time source threaded in at the domain edge.
class PraxisEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin
    Dim dimensions() const override { return Dim::D2; }  // writes only the z=0 slice; extrude fills depth

    // Controls — MoonLight's exact defaults and ranges.
    uint8_t macroMutatorFreq = 3;    // macro mutator beat frequency (0..15)
    uint8_t macroMutatorMin  = 250;  // macro mutator low end (0..255), scaled <<8 into the 16-bit sweep
    uint8_t macroMutatorMax  = 255;  // macro mutator high end (0..255), scaled <<8
    uint8_t microMutatorFreq = 4;    // micro mutator beat frequency (0..15)
    uint8_t microMutatorMin  = 200;  // micro mutator low end (0..255)
    uint8_t microMutatorMax  = 255;  // micro mutator high end (0..255)

    void onBuildControls() override {
        controls_.addUint8("macroMutatorFreq", macroMutatorFreq, 0, 15);
        controls_.addUint8("macroMutatorMin", macroMutatorMin, 0, 255);
        controls_.addUint8("macroMutatorMax", macroMutatorMax, 0, 255);
        controls_.addUint8("microMutatorFreq", microMutatorFreq, 0, 15);
        controls_.addUint8("microMutatorMin", microMutatorMin, 0, 255);
        controls_.addUint8("microMutatorMax", microMutatorMax, 0, 255);
    }

    void loop() override {
        const int w = width();
        const int h = height();
        if (w <= 0 || h <= 0 || channelsPerLight() < 3) return;

        Buffer& buf = layer()->buffer();
        const Coord3D dims{static_cast<lengthType>(w), static_cast<lengthType>(h), depthDim()};

        const uint32_t now = elapsed();

        // The two oscillating mutators. macro sweeps the high 16-bit range (min<<8 .. max<<8);
        // micro sweeps the raw 0..255 range. beatsin16(bpm, ms, low, high) — our time helper takes
        // elapsed() as the 2nd argument.
        const uint16_t macro = beatsin16(macroMutatorFreq, now,
                                         static_cast<uint16_t>(macroMutatorMin << 8),
                                         static_cast<uint16_t>(macroMutatorMax << 8));
        const uint16_t micro = beatsin16(microMutatorFreq, now,
                                         microMutatorMin, microMutatorMax);

        const uint32_t huebase = now / 40;
        const int64_t  microDiv = static_cast<int64_t>(micro) + 1;  // micro+1, guards a divide-by-zero

        // hue = huebase + (x + y·macro·x) / (micro+1), truncated to the 0..255 palette wheel index.
        // The y·macro·x cross term can reach ~grid · 65280 · grid, so the accumulation runs in 64-bit
        // before the divide and the implicit wrap to a uint8 palette index (MoonLight uses 32-bit int;
        // identical up to ~128² grids, where the product still fits — fidelity-preserving on real grids,
        // overflow-safe on extreme ones).
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const int64_t spatial = static_cast<int64_t>(x)
                                      + static_cast<int64_t>(y) * static_cast<int64_t>(macro) * static_cast<int64_t>(x);
                const uint32_t hue = huebase + static_cast<uint32_t>(spatial / microDiv);
                const RGB c = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(hue), 255);
                draw::pixel(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, c);
            }
        }
    }

private:
    // depth() is the grid's z extent; guard a zero so dims stays valid on a 2D layer.
    lengthType depthDim() const { return depth() > 0 ? depth() : 1; }
};

} // namespace mm
