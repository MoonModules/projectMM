#pragma once

#include <limits>
#include <cstdint>
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

namespace mm {

// A serpentine 2D LED matrix (panel), emitting each light's (x, y, 0) coordinate
// in physical wiring order. The general matrix layout: GridLayout is the simple
// row-major/serpentine case; this adds a configurable axis order (walk X-major or
// Y-major), a per-axis increment direction, and a snake toggle.
//
// Prior art: MoonLight PanelLayout (Node "Panel", tags 🚥), which drives the panel
// off a `Wiring{size, count, inc[], snake[]}` helper and an `iterate()` walk. We
// reproduce the geometry (axis-order table, snake-on-odd-outer serpentine) and the
// control set, but drop MoonLight's pin/wiring plumbing (the Wiring struct's pin
// count, nextPin()) — a projectMM layout emits coordinates only; the driver owns
// pins. tags 💫 marks the MoonLight lineage.
//
// The MoonLight `Wiring`/`iterate` implementation is not in the ported source (only
// the Panel usage site is), so the iteration is reconstructed from that usage plus
// the control labels/defaults; the reconstructed logic is marked // RECONSTRUCTED
// and cross-checks against GridLayout's serpentine on the defaults.
// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Layouts/L_MoonLight.h
class PanelLayout : public LayoutBase {
public:
    // Geometry (verbatim MoonLight defaults): a 16×16 panel.
    lengthType panelWidth  = 16;   // extent along X (MoonLight panel.size[0])
    lengthType panelHeight = 16;   // extent along Y (MoonLight panel.size[1])

    // Wiring order: which axis is the outer loop. 0 = "XY" (Y outer, X inner —
    // the classic row-major panel), 1 = "YX" (X outer, Y inner — column-major).
    // Verbatim from MoonLight's two-value select (index 0 "XY", 1 "YX").
    uint8_t wiringOrder = 0;

    // Per-axis increment direction (MoonLight panel.inc[0]="X++", inc[1]="Y++"):
    // true walks that coordinate 0→max, false walks it max→0. Default both true.
    bool incX = true;   // "X++"
    bool incY = true;   // "Y++"

    // Snake the inner loop on odd outer steps (boustrophedon). MoonLight exposes a
    // single "snake" control = panel.snake[1] (the inner loop in 2D); default true.
    bool snake = true;

    void onBuildControls() override {
        // Geometry only — MoonLight's pin controls (ledPin selects, nextPin) are dropped.
        // Ranges from MoonLight (1..65536), clamped to lengthType's int16_t max (512-safe).
        controls_.addInt16("panelWidth",  panelWidth,  1, 512);
        controls_.addInt16("panelHeight", panelHeight, 1, 512);
        controls_.addSelect("wiringOrder", wiringOrder, kWiringOptions, kWiringCount);
        controls_.addBool("X++", incX);
        controls_.addBool("Y++", incY);
        controls_.addBool("snake", snake);
    }

    const char* tags() const override { return "💫"; }

    nrOfLightsType lightCount() const override {
        // Multiply in uint32_t to detect overflow before casting, per GridLayout.
        uint32_t n = static_cast<uint32_t>(panelWidth) * static_cast<uint32_t>(panelHeight);
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        // MoonLight: axes = axisOrders[wiringOrder]; XY(0) = {1,0} (Y outer, X inner),
        // YX(1) = {0,1} (X outer, Y inner). axes[0] is the outer axis, axes[1] the inner.
        const uint8_t axisOrders[2][2] = {
            {1, 0},  // "XY": Y(1) outer loop, X(0) inner loop
            {0, 1},  // "YX": X(0) outer loop, Y(1) inner loop
        };
        const uint8_t wo = wiringOrder < kWiringCount ? wiringOrder : 0;
        const uint8_t outerAxis = axisOrders[wo][0];
        const uint8_t innerAxis = axisOrders[wo][1];

        const lengthType extent[2] = {panelWidth, panelHeight};  // extent[0]=X, extent[1]=Y
        const bool inc[2] = {incX, incY};                        // inc[0]=X++, inc[1]=Y++

        const uint32_t limit = lightCount();
        uint32_t idx = 0;

        // RECONSTRUCTED: MoonLight's Wiring::iterate is not in the ported source. From the
        // Panel usage — iterate(0,0,i){ iterate(1,i,j){ coords[axes[0]]=i; coords[axes[1]]=j } }
        // — plus the control labels/defaults, the semantics are:
        //   • the outer loop walks axis `outerAxis`, index i in 0..extent[outerAxis]-1;
        //   • direction is inc[axis] (axis-keyed — matches the "X++"/"Y++" labels and Cube's
        //     "X++"/"Y++"/"Z++");
        //   • the outer loop never snakes (its parent index is a constant 0, always even);
        //   • the inner loop reverses (snakes) when `snake` is on and the emitted outer index
        //     is odd. `snake` is the single exposed inner-loop toggle (MoonLight panel.snake[1]);
        //     MoonLight's exact snake-array indexing (physical-axis vs loop-slot) is not
        //     recoverable from the ported source, but both the "snake on the Y-axis" default
        //     comment and this inner-loop reading yield the SAME geometry, the standard
        //     boustrophedon panel, so this is the faithful behaviour.
        // On the defaults (XY, inc all true, snake on) this emits Y=0→X 0..15, Y=1→X 15..0,
        // … identical to GridLayout's serpentine (verified: idx0=(0,0) idx15=(15,0)
        // idx16=(15,1) idx31=(0,1)).
        const lengthType outerN = extent[outerAxis];
        const lengthType innerN = extent[innerAxis];
        const bool outerAsc = inc[outerAxis];
        const bool innerAsc = inc[innerAxis];

        for (lengthType oi = 0; oi < outerN && idx < limit; oi++) {
            // Outer coordinate value along its axis (direction only; no snake at outermost).
            const lengthType outerVal = outerAsc
                ? oi
                : static_cast<lengthType>(outerN - 1 - oi);

            // Inner loop reverses when snaking and the outer coordinate is odd.
            const bool innerReverse = snake && (outerVal & 1);
            const bool innerForward = innerAsc != innerReverse;  // XOR: reverse flips inc

            for (lengthType ii = 0; ii < innerN && idx < limit; ii++) {
                const lengthType innerVal = innerForward
                    ? ii
                    : static_cast<lengthType>(innerN - 1 - ii);

                // Scatter the two loop values back to (x, y) via the chosen axis order,
                // exactly as MoonLight's coords[axes[0]]=i; coords[axes[1]]=j.
                lengthType coord[2] = {0, 0};  // coord[0]=x, coord[1]=y
                coord[outerAxis] = outerVal;
                coord[innerAxis] = innerVal;

                cb(ctx, static_cast<nrOfLightsType>(idx++), coord[0], coord[1], 0);
            }
        }
    }

private:
    static constexpr const char* kWiringOptions[] = {"XY", "YX"};
    static constexpr uint8_t kWiringCount = 2;
};

} // namespace mm