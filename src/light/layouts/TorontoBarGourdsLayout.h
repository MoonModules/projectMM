#pragma once

#include <cstdint>
#include "light/layouts/Layouts.h"
#include "light/light_types.h"  // lengthType, nrOfLightsType, Coord3D

namespace mm {

// The Toronto Bar "gourds" installation: 24 gourd fixtures placed at fixed
// positions in a 3D grid, each gourd rendered at one of three granularities.
// A gourd is a small five-sided body (front/back/left/right/bottom + a middle
// point); the granularity control trades physical fidelity for light count:
//
//   0 "One Gourd One Light"  — nrOfLightsPerGourd lights, all stacked on the
//                              gourd's raw grid position (a whole gourd = one
//                              logical point, replicated).
//   1 "One Side One Light"   — gourdLength 3: five sides, each 12 LEDs mapped to
//                              the same virtual pixel, + 1 middle LED = 61/gourd.
//   2 "One LED One Light"    — gourdLength 7: five cube faces, each a 12-pixel
//                              perimeter ring, + 1 middle LED = 61/gourd.
//
// Prior art: MoonLight's TorontoBarGourdsLayout (Node "Toronto Bar Gourds",
// tags 🚥). The gourd grid, all three per-mode Coord3D tables, the gourdLength
// constants (3 and 7), and every +offset are reproduced verbatim; the geometry
// IS the spec. MoonLight's pin plumbing (nextPin() every 10 gourds) is dropped —
// a projectMM layout emits coordinates only; the driver owns pins. tags 💫 marks
// the MoonLight lineage.
//
// A single walk() is the one source of truth for the geometry: lightCount()
// runs it with a no-op callback to tally, forEachCoord() runs it to emit, so the
// count and the emitted set can never disagree (the RingLayout/SphereLayout
// pattern). Integer math throughout; this is the cold build path.
// Author: projectMM / custom fixture
class TorontoBarGourdsLayout : public LayoutBase {
public:
    // Mode 0 only: how many stacked lights one gourd contributes. MoonLight
    // default 61 (= 5*12+1, matching the 61 lights modes 1 and 2 emit).
    uint8_t nrOfLightsPerGourd = 61;
    // Granularity select (0/1/2). MoonLight default 2 ("One LED One Light").
    uint8_t granularity = 2;

    void onBuildControls() override {
        controls_.addSelect("granularity", granularity, kGranularityOptions, kGranularityCount);
        // Mode 0 only; MoonLight range 1..128.
        controls_.addUint8("nrOfLightsPerGourd", nrOfLightsPerGourd, 1, 128);
    }

    const char* tags() const override { return "💫"; }

    nrOfLightsType lightCount() const override {
        nrOfLightsType n = 0;
        walk([](void*, nrOfLightsType, lengthType, lengthType, lengthType) {}, nullptr, &n);
        return n;
    }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        walk(cb, ctx, nullptr);
    }

private:
    static constexpr const char* kGranularityOptions[] = {
        "One Gourd One Light", "One Side One Light", "One LED One Light"};
    static constexpr uint8_t kGranularityCount = 3;

    // Small emitter that either invokes cb with a running index or just tallies,
    // so every mode below writes one line per light regardless of which path
    // (emit vs count) is active. Mirrors MoonLight's addLight(Coord3D).
    struct Emit {
        CoordCallback cb;
        void* ctx;
        nrOfLightsType idx = 0;
        void add(lengthType x, lengthType y, lengthType z) {
            if (cb) cb(ctx, idx, x, y, z);
            idx++;
        }
    };

    // One Gourd One Light: every one of the gourd's lights on the same position.
    void addGourd(Emit& e, Coord3D pos) const {
        for (int i = 0; i < nrOfLightsPerGourd; i++)
            e.add(pos.x, pos.y, pos.z);  // all gourd lights on the same position
    }

    // One Side One Light: each side can be mapped in a 3*3*3 grid (27 LEDs),
    // 5 sides + 1 middle LED; each side's 12 LEDs share one virtual pixel.
    void addGourdSides(Emit& e, Coord3D pos) const {
        const int gourdLength = 3;
        const Coord3D sides[] = {
            Coord3D{1, 1, 0}, Coord3D{2, 1, 1}, Coord3D{1, 1, 2},
            Coord3D{0, 1, 1}, Coord3D{1, 2, 1}};

        for (Coord3D side : sides) {  // 5 sides
            for (int i = 0; i < 12; i++)  // 12 LEDs each, all on the same virtual pixel
                e.add(static_cast<lengthType>(pos.x * gourdLength + side.x),
                      static_cast<lengthType>(pos.y * gourdLength + side.y),
                      static_cast<lengthType>(pos.z * gourdLength + side.z));
        }

        // + middleLED
        const Coord3D side{1, 1, 1};
        e.add(static_cast<lengthType>(pos.x * gourdLength + side.x),
              static_cast<lengthType>(pos.y * gourdLength + side.y),
              static_cast<lengthType>(pos.z * gourdLength + side.z));
    }

    // One LED One Light: gourd is a 7-wide cube shell; each of 5 faces is a
    // 12-pixel perimeter ring (the pixels[] table, a clockwise walk of a 4x4
    // border), + 1 middle LED. In the MoonLight source the pixel table uses
    // two-arg Coord3D (z defaults to 0); the .z is never read here — each face
    // loop scatters pixel.x/pixel.y into the two varying axes and sets the
    // constant axis directly, exactly as the source does.
    void addGourdPixels(Emit& e, Coord3D pos) const {
        const int gourdLength = 7;
        const Coord3D pixels[] = {  // 12 pixels each
            Coord3D{0, 0, 0}, Coord3D{1, 0, 0}, Coord3D{2, 0, 0}, Coord3D{3, 0, 0},
            Coord3D{3, 1, 0}, Coord3D{3, 2, 0}, Coord3D{3, 3, 0}, Coord3D{2, 3, 0},
            Coord3D{1, 3, 0}, Coord3D{0, 3, 0}, Coord3D{0, 2, 0}, Coord3D{0, 1, 0}};

        // back and front: z constant, increasing x and y
        for (Coord3D pixel : pixels)  // front
            e.add(static_cast<lengthType>(pos.x * gourdLength + pixel.x),
                  static_cast<lengthType>(pos.y * gourdLength + pixel.y),
                  static_cast<lengthType>(pos.z * gourdLength));
        for (Coord3D pixel : pixels)  // back
            e.add(static_cast<lengthType>(pos.x * gourdLength + pixel.x),
                  static_cast<lengthType>(pos.y * gourdLength + pixel.y),
                  static_cast<lengthType>(pos.z * gourdLength + gourdLength - 1));

        // left and right: x constant, increasing y and z
        for (Coord3D pixel : pixels)  // left
            e.add(static_cast<lengthType>(pos.x * gourdLength),
                  static_cast<lengthType>(pos.y * gourdLength + pixel.x),
                  static_cast<lengthType>(pos.z * gourdLength + pixel.y));
        for (Coord3D pixel : pixels)  // right
            e.add(static_cast<lengthType>(pos.x * gourdLength + gourdLength - 1),
                  static_cast<lengthType>(pos.y * gourdLength + pixel.x),
                  static_cast<lengthType>(pos.z * gourdLength + pixel.y));

        // bottom : y constant, increasing x and z
        for (Coord3D pixel : pixels)  // bottom
            e.add(static_cast<lengthType>(pos.x * gourdLength + pixel.x),
                  static_cast<lengthType>(pos.y * gourdLength + gourdLength - 1),
                  static_cast<lengthType>(pos.z * gourdLength + pixel.y));

        // + middleLED
        const Coord3D middle{3, 3, 3};
        e.add(static_cast<lengthType>(pos.x * gourdLength + middle.x),
              static_cast<lengthType>(pos.y * gourdLength + middle.y),
              static_cast<lengthType>(pos.z * gourdLength + middle.z));
    }

    // Single source of truth for the layout: place all 24 gourds in the fixed
    // grid order, emitting each gourd via the selected granularity. cb non-null
    // emits; count non-null tallies (reusing the exact same walk).
    void walk(CoordCallback cb, void* ctx, nrOfLightsType* count) const {
        // The gourd order and positions in a 3D grid space (MoonLight, verbatim).
        const Coord3D gourds[] = {
            Coord3D{0, 0, 0}, Coord3D{1, 0, 0}, Coord3D{2, 0, 0}, Coord3D{3, 0, 0},
            Coord3D{0, 1, 0}, Coord3D{1, 1, 0}, Coord3D{2, 1, 0}, Coord3D{3, 1, 0},
            Coord3D{0, 2, 0}, Coord3D{1, 2, 0}, Coord3D{2, 2, 0}, Coord3D{3, 2, 0},
            Coord3D{0, 0, 1}, Coord3D{1, 0, 1}, Coord3D{2, 0, 1}, Coord3D{3, 0, 1},
            Coord3D{0, 1, 1}, Coord3D{1, 1, 1}, Coord3D{2, 1, 1}, Coord3D{3, 1, 1},
            Coord3D{0, 2, 1}, Coord3D{1, 2, 1}, Coord3D{2, 2, 1}, Coord3D{3, 2, 1}};

        Emit e{cb, ctx};
        for (Coord3D gourd : gourds) {
            if (granularity == 0) {  // one gourd one light
                addGourd(e, gourd);
            } else if (granularity == 1) {  // one side one light
                addGourdSides(e, gourd);
            } else if (granularity == 2) {  // one LED one light
                addGourdPixels(e, gourd);
            }
        }
        if (count) *count = e.idx;
    }
};

} // namespace mm
