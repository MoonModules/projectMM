#pragma once

#include <limits>
#include <cstdint>
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType

namespace mm {

// A tiled grid of full matrix panels: an M×N arrangement of panels, each panel a
// serpentine matrix in its own right. Two levels of wiring — an OUTER walk over
// the panel grid (which panel, in what order) and an INNER walk over each panel's
// lights (PanelLayout's matrix) — so both the panel-to-panel order and the
// light-to-light order inside a panel are independently configurable. Each panel
// at grid cell (px, py) is offset by px·panelWidth in X and py·panelHeight in Y,
// so the panels tile edge-to-edge into one large matrix. Every light is at an
// integer (x, y, 0); this is a 2D layout.
//
// Prior art: MoonLight PanelsLayout (Node "Panels", tags 🚥), which nests a
// `panel` Wiring inside a `panels` Wiring and walks both with `iterate()`. We
// reproduce the geometry — the two-level nesting, the axis-order table shared by
// both levels, the snake-on-odd-outer serpentine, and the per-panel offset
// (coordsP[axis]·panel.size[axis]) — and the control set, but drop MoonLight's
// pin/wiring plumbing (the Wiring pin count, panelsPerPin, nextPin()): a projectMM
// layout emits coordinates only; the driver owns pins. tags 💫 marks the MoonLight
// lineage. The single-panel case is the sibling PanelLayout (Node "Panel"); this
// module tiles that panel across a grid.
//
// The MoonLight `Wiring`/`iterate` implementation is not in the ported source (only
// the Panels usage site is), so the iteration is reconstructed from that usage plus
// the control labels/defaults — identically to PanelLayout's already-shipped
// reconstruction. The reconstructed logic is marked // RECONSTRUCTED and, on the
// defaults, cross-checks against a plain serpentine tiling.
class PanelsLayout : public LayoutBase {
public:
    // Outer level — the panel grid (MoonLight `panels`, default 2×2 panels).
    lengthType horizontalPanels = 2;   // panels along X (MoonLight panels.size[0])
    lengthType verticalPanels   = 2;   // panels along Y (MoonLight panels.size[1])
    uint8_t    wiringOrderP     = 0;   // panel-grid wiring order: 0="XY", 1="YX"
    bool       incXP            = true;  // "X++P": panel-grid X direction
    bool       incYP            = true;  // "Y++P": panel-grid Y direction
    bool       snakeP           = false; // "snakeP": snake the panel grid (default off)

    // Inner level — each panel (MoonLight `panel`, default 16×16, snake on Y).
    lengthType panelWidth  = 16;   // panel extent along X (MoonLight panel.size[0])
    lengthType panelHeight = 16;   // panel extent along Y (MoonLight panel.size[1])
    uint8_t    wiringOrder = 0;    // per-panel wiring order: 0="XY", 1="YX"
    bool       incX        = true; // "X++": per-panel X direction
    bool       incY        = true; // "Y++": per-panel Y direction
    bool       snake       = true; // "snake": snake each panel's inner loop (default on)

    void onBuildControls() override {
        // Panel grid (outer). MoonLight ranges 1..32; clamped to lengthType (int16_t).
        controls_.addInt16("horizontalPanels", horizontalPanels, 1, 32);
        controls_.addInt16("verticalPanels",   verticalPanels,   1, 32);
        controls_.addSelect("wiringOrderP", wiringOrderP, kWiringOptions, kWiringCount);
        controls_.addBool("X++P",  incXP);
        controls_.addBool("Y++P",  incYP);
        controls_.addBool("snakeP", snakeP);

        // Per-panel (inner). MoonLight ranges 1..65536; clamped to int16_t max (512-safe).
        controls_.addInt16("panelWidth",  panelWidth,  1, 512);
        controls_.addInt16("panelHeight", panelHeight, 1, 512);
        controls_.addSelect("wiringOrder", wiringOrder, kWiringOptions, kWiringCount);
        controls_.addBool("X++",   incX);
        controls_.addBool("Y++",   incY);
        controls_.addBool("snake", snake);
    }

    const char* tags() const override { return "💫"; }

    nrOfLightsType lightCount() const override {
        // Total lights = (panels in grid) × (lights per panel). Multiply in
        // uint32_t/uint64_t to detect overflow before casting, per GridLayout/PanelLayout.
        uint32_t panelCount  = static_cast<uint32_t>(horizontalPanels) * static_cast<uint32_t>(verticalPanels);
        uint32_t perPanel    = static_cast<uint32_t>(panelWidth) * static_cast<uint32_t>(panelHeight);
        uint64_t n = static_cast<uint64_t>(panelCount) * perPanel;
        constexpr uint64_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        // MoonLight: axes = axisOrders[wiringOrder]; XY(0) = {1,0} (Y outer, X inner),
        // YX(1) = {0,1} (X outer, Y inner). axes[0] is the outer axis, axes[1] the inner.
        // The SAME table drives both the panel-grid walk and the per-panel walk.
        const uint8_t axisOrders[2][2] = {
            {1, 0},  // "XY": axis 1 (Y) outer loop, axis 0 (X) inner loop
            {0, 1},  // "YX": axis 0 (X) outer loop, axis 1 (Y) inner loop
        };

        const uint32_t limit = lightCount();
        Emit e{cb, ctx, panelWidth, panelHeight, limit, 0};

        // ---- Outer walk: the panel grid. ----
        // MoonLight: panels.iterate(0,0,a){ panels.iterate(1,a,b){ coordsP[axes[0]]=a; coordsP[axes[1]]=b; ... } }
        // coordsP is the (px, py) panel cell; for each cell the inner panel is emitted.
        const uint8_t woP = wiringOrderP < kWiringCount ? wiringOrderP : 0;
        const uint8_t outerAxisP = axisOrders[woP][0];
        const uint8_t innerAxisP = axisOrders[woP][1];
        const lengthType extentP[2] = {horizontalPanels, verticalPanels};  // extentP[0]=X, [1]=Y
        const bool incP[2] = {incXP, incYP};

        // Per-panel axis order, resolved once (constant across all panels).
        const uint8_t wo = wiringOrder < kWiringCount ? wiringOrder : 0;
        const uint8_t outerAxis = axisOrders[wo][0];
        const uint8_t innerAxis = axisOrders[wo][1];
        const lengthType extent[2] = {panelWidth, panelHeight};  // extent[0]=X, [1]=Y
        const bool inc[2] = {incX, incY};

        // RECONSTRUCTED: Wiring::iterate is not in the ported source. Same reading as
        // PanelLayout.h — the outer loop walks its axis with direction inc[axis] and
        // never snakes (its parent index is a constant 0, always even); the inner loop
        // reverses (snakes) when the level's snake toggle is on and the emitted outer
        // coordinate is odd. Applied at BOTH levels here: (snakeP, incP) for the panel
        // grid and (snake, inc) for each panel. On the defaults (XY, all inc true,
        // panel grid snake off, per-panel snake on) this tiles four 16×16 serpentine
        // panels edge-to-edge into a 32×32 matrix.
        const lengthType outerNP = extentP[outerAxisP];
        const lengthType innerNP = extentP[innerAxisP];
        const bool outerAscP = incP[outerAxisP];
        const bool innerAscP = incP[innerAxisP];

        for (lengthType oip = 0; oip < outerNP && e.idx < limit; oip++) {
            const lengthType outerValP = outerAscP
                ? oip
                : static_cast<lengthType>(outerNP - 1 - oip);

            const bool innerReverseP = snakeP && (outerValP & 1);
            const bool innerForwardP = innerAscP != innerReverseP;  // XOR: reverse flips inc

            for (lengthType iip = 0; iip < innerNP && e.idx < limit; iip++) {
                const lengthType innerValP = innerForwardP
                    ? iip
                    : static_cast<lengthType>(innerNP - 1 - iip);

                // Scatter the two panel-grid loop values back to (px, py) via the
                // chosen axis order — MoonLight's coordsP[axes[0]]=a; coordsP[axes[1]]=b.
                lengthType coordsP[2] = {0, 0};  // coordsP[0]=px, coordsP[1]=py
                coordsP[outerAxisP] = outerValP;
                coordsP[innerAxisP] = innerValP;

                // ---- Inner walk: this panel's matrix, offset into the tiled grid. ----
                emitPanel(e, coordsP[0], coordsP[1],
                          outerAxis, innerAxis, extent, inc, limit);
            }
        }
    }

private:
    // Shared emission state threaded through the two nested walks — keeps the
    // running physical index continuous across all panels (wiring order).
    struct Emit {
        CoordCallback cb;
        void* ctx;
        lengthType panelWidth;
        lengthType panelHeight;
        uint32_t limit;
        uint32_t idx;
    };

    // Emit one panel's lights, offset by (px·panelWidth, py·panelHeight). This is
    // PanelLayout's per-panel serpentine matrix (the same RECONSTRUCTED iterate
    // reading), with the tile offset added to each emitted coordinate.
    void emitPanel(Emit& e, lengthType px, lengthType py,
                   uint8_t outerAxis, uint8_t innerAxis,
                   const lengthType extent[2], const bool inc[2],
                   uint32_t limit) const {
        const lengthType offsetX = static_cast<lengthType>(px * e.panelWidth);
        const lengthType offsetY = static_cast<lengthType>(py * e.panelHeight);

        const lengthType outerN = extent[outerAxis];
        const lengthType innerN = extent[innerAxis];
        const bool outerAsc = inc[outerAxis];
        const bool innerAsc = inc[innerAxis];

        for (lengthType oi = 0; oi < outerN && e.idx < limit; oi++) {
            const lengthType outerVal = outerAsc
                ? oi
                : static_cast<lengthType>(outerN - 1 - oi);

            const bool innerReverse = snake && (outerVal & 1);
            const bool innerForward = innerAsc != innerReverse;  // XOR: reverse flips inc

            for (lengthType ii = 0; ii < innerN && e.idx < limit; ii++) {
                const lengthType innerVal = innerForward
                    ? ii
                    : static_cast<lengthType>(innerN - 1 - ii);

                // Scatter the two loop values back to (x, y) via the panel axis order,
                // exactly as MoonLight's coords[axes[0]]=i; coords[axes[1]]=j, then add
                // the tile offset (MoonLight: coordsP[0]*panel.size[0] + coords[0], …).
                lengthType coord[2] = {0, 0};  // coord[0]=x, coord[1]=y (panel-local)
                coord[outerAxis] = outerVal;
                coord[innerAxis] = innerVal;

                e.cb(e.ctx, static_cast<nrOfLightsType>(e.idx++),
                     static_cast<lengthType>(offsetX + coord[0]),
                     static_cast<lengthType>(offsetY + coord[1]),
                     0);
            }
        }
    }

    static constexpr const char* kWiringOptions[] = {"XY", "YX"};
    static constexpr uint8_t kWiringCount = 2;
};

} // namespace mm
