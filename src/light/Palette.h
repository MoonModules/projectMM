#pragma once

#include "core/color.h"   // RGB, scale8, hsvToRgb
#include "core/JsonSink.h" // paletteOptions emits the dropdown {name,colors} objects

#include <cstddef>
#include <cstdint>

namespace mm {

// A colour palette: the active palette is 16 evenly-spaced RGB entries (the CRGBPalette16 model),
// and colorFromPalette() reads a 0-255 wheel index by interpolating between the two bracketing
// entries. The gradient definitions (a {pos,R,G,B,…} stop list) live in flash and expand into the
// 16 entries on selection, off the hot path; the per-light lookup is then a single scale8 blend.
//
// Prior art: FastLED's gradient palettes (CRGBPalette16 / ColorFromPalette), the convention WLED +
// MoonLight share — the recognisable names + model are carried; this implementation is our own, on
// our RGB/scale8. The gradient *data* in kBuiltinPalettes is from MoonLight's palettes.h (a public
// palette set), reformatted; see docs/backlog/moonlight-palettes-data.md.
struct Palette {
    static constexpr uint8_t kEntries = 16;
    RGB entry[kEntries] = {};

    // Build the 16 entries from a gradient-stop list: {pos0,R,G,B, pos1,R,G,B, …} with pos 0..255,
    // ascending, ending at 255. Each of the 16 evenly-spaced sample positions is the linear blend
    // of the two stops it falls between. Off the hot path (called on selection).
    void fromGradient(const uint8_t* stops, size_t count) {
        const size_t nStops = count / 4;
        if (nStops == 0) { for (auto& e : entry) e = {0, 0, 0}; return; }
        for (uint8_t i = 0; i < kEntries; i++) {
            // The sample position for entry i, spread 0..255 across the 16 entries.
            const uint8_t pos = static_cast<uint8_t>((static_cast<uint16_t>(i) * 255) / (kEntries - 1));
            entry[i] = sampleGradient(stops, nStops, pos);
        }
    }

private:
    // The colour at `pos` (0..255) on the gradient: find the bracketing stops and lerp.
    static RGB sampleGradient(const uint8_t* stops, size_t nStops, uint8_t pos) {
        // Before the first stop (a gradient whose first stop sits above 0): clamp to it, so the
        // `pos - p0` below can't underflow.
        if (pos <= stops[0]) return {stops[1], stops[2], stops[3]};
        // Walk to the last stop whose position <= pos.
        size_t s = 0;
        while (s + 1 < nStops && stops[(s + 1) * 4] <= pos) s++;
        const uint8_t* lo = stops + s * 4;
        if (s + 1 >= nStops) return {lo[1], lo[2], lo[3]};   // at/after the last stop
        const uint8_t* hi = stops + (s + 1) * 4;
        const uint8_t p0 = lo[0], p1 = hi[0];
        if (p1 == p0) return {lo[1], lo[2], lo[3]};
        // Fraction of the way from lo to hi, as 0..255 for scale8.
        const uint8_t frac = static_cast<uint8_t>((static_cast<uint16_t>(pos - p0) * 255) / (p1 - p0));
        return lerpRGB({lo[1], lo[2], lo[3]}, {hi[1], hi[2], hi[3]}, frac);
    }

public:
    // Linear blend a→b by frac (0 = a, 255 = b). Integer-only.
    static RGB lerpRGB(const RGB& a, const RGB& b, uint8_t frac) {
        const uint8_t inv = static_cast<uint8_t>(255 - frac);
        return { static_cast<uint8_t>(scale8(a.r, inv) + scale8(b.r, frac)),
                 static_cast<uint8_t>(scale8(a.g, inv) + scale8(b.g, frac)),
                 static_cast<uint8_t>(scale8(a.b, inv) + scale8(b.b, frac)) };
    }
};

// The per-light lookup: `index` is a 0..255 wheel position (wraps), mapped across the 16 entries;
// blend the two bracketing entries, then scale by `brightness`. Hot-path-cheap (two scale8 + a
// blend). This is the single seam every palette-driven effect calls, so the palette source is
// swappable behind one signature without touching effects.
inline RGB colorFromPalette(const Palette& p, uint8_t index, uint8_t brightness = 255) {
    // Position across 16 entries: the high nibble selects the entry, the low byte the blend.
    const uint8_t hi = static_cast<uint8_t>(index >> 4);                 // 0..15 — bracket start
    const uint8_t frac = static_cast<uint8_t>((index & 0x0F) * 17);      // 0..255 within the bracket
    const RGB& a = p.entry[hi];
    const RGB& b = p.entry[(hi + 1) & 0x0F];                             // wrap 15→0
    RGB c = Palette::lerpRGB(a, b, frac);
    if (brightness != 255) {
        c.r = scale8(c.r, brightness);
        c.g = scale8(c.g, brightness);
        c.b = scale8(c.b, brightness);
    }
    return c;
}

// Cross-fade two colours: `amt`/255 of the way from `a` to `b` (amt 0 = a, 255 = b). The textbook
// RGB lerp, the staple for compositing/transitions. Prior art: FastLED's blend (colorutils).
inline RGB blend(RGB a, RGB b, uint8_t amt) { return Palette::lerpRGB(a, b, amt); }

// Dim a colour toward black by `amt`/255 (amt 0 = unchanged, 255 = black) — the per-frame fade
// that gives effects a decaying trail. Prior art: FastLED's fadeToBlackBy.
inline void fadeToBlackBy(RGB& c, uint8_t amt) {
    const uint8_t keep = static_cast<uint8_t>(255 - amt);
    c.r = scale8(c.r, keep);
    c.g = scale8(c.g, keep);
    c.b = scale8(c.b, keep);
}

// --- Built-in palettes -------------------------------------------------------------------------
// Gradient-stop definitions in flash ({pos,R,G,B,…}). A curated starter set — gradient data from
// MoonLight's palettes.h (reformatted) plus generated ones. More can be added later as pure data.
namespace palettes {

inline constexpr uint8_t kLava[]        = {0,0,0,0, 46,18,0,0, 96,113,0,0, 108,142,3,1, 119,175,17,1, 146,213,44,2, 174,255,82,4, 188,255,115,4, 202,255,156,4, 218,255,203,4, 234,255,255,4, 244,255,255,71, 255,255,255,255};
inline constexpr uint8_t kFierceIce[]   = {0,0,0,0, 59,0,9,45, 119,0,38,255, 149,3,100,255, 180,23,199,255, 217,100,235,255, 255,255,255,255};
inline constexpr uint8_t kSunset[]      = {0,120,0,0, 22,179,22,0, 51,255,104,0, 85,167,22,18, 135,100,0,103, 198,16,0,130, 255,0,0,160};
inline constexpr uint8_t kOceanBreeze[] = {0,1,6,7, 89,1,99,111, 153,144,209,255, 255,0,73,82};
inline constexpr uint8_t kOrangeTeal[]  = {0,0,150,92, 55,0,150,92, 200,255,72,0, 255,255,72,0};
inline constexpr uint8_t kAurora[]      = {0,1,5,45, 64,0,200,23, 128,0,255,0, 170,0,243,45, 200,0,135,7, 255,1,5,45};
inline constexpr uint8_t kAtlantica[]   = {0,0,28,112, 50,32,96,255, 100,0,243,45, 150,12,95,82, 200,25,190,95, 255,40,170,80};
inline constexpr uint8_t kParty[]       = {0,85,0,171, 42,150,0,107, 85,201,0,42, 128,212,32,0, 170,191,98,0, 213,128,160,0, 255,85,212,0};   // FastLED party-colors stops
inline constexpr uint8_t kForest[]      = {0,0,100,0, 64,34,139,34, 128,0,128,0, 192,107,142,35, 255,0,100,0};

// A built-in is a gradient ({stops,len}) or the special "rainbow" (generated via hsvToRgb).
struct Builtin { const char* name; const uint8_t* stops; size_t len; bool rainbow; };

inline constexpr Builtin kBuiltins[] = {
    {"Rainbow",    nullptr,     0,                  true },
    {"Party",      kParty,      sizeof(kParty),     false},
    {"Lava",       kLava,       sizeof(kLava),      false},
    {"Ocean",      kOceanBreeze,sizeof(kOceanBreeze),false},
    {"Forest",     kForest,     sizeof(kForest),    false},
    {"Fierce Ice", kFierceIce,  sizeof(kFierceIce), false},
    {"Sunset",     kSunset,     sizeof(kSunset),    false},
    {"Orange Teal",kOrangeTeal, sizeof(kOrangeTeal),false},
    {"Aurora",     kAurora,     sizeof(kAurora),    false},
    {"Atlantica",  kAtlantica,  sizeof(kAtlantica), false},
};
inline constexpr uint8_t kCount = sizeof(kBuiltins) / sizeof(kBuiltins[0]);

// Names array for the UI select control (parallel to kBuiltins).
inline const char* const kNames[] = {
    "Rainbow", "Party", "Lava", "Ocean", "Forest", "Fierce Ice", "Sunset", "Orange Teal", "Aurora", "Atlantica",
};

}  // namespace palettes

// The global active palette effects read — the AudioModule::latestFrame() static-seam pattern.
// Drivers owns the `palette` select control and calls setActive() on change; effects just call
// colorFromPalette(*Palettes::active(), idx).
class Palettes {
public:
    static const Palette* active() { return &active_; }

    // Expand built-in `index` into the active palette (off the hot path — on selection).
    static void setActive(uint8_t index) {
        active_ = fromBuiltin(index);
    }

    // Build the 16-entry palette for built-in `index` (rainbow generated via hsvToRgb, the rest
    // expanded from their gradient stops). Shared by setActive() and the default + the swatches.
    static Palette fromBuiltin(uint8_t index) {
        if (index >= palettes::kCount) index = 0;
        const auto& b = palettes::kBuiltins[index];
        Palette p;
        if (b.rainbow) {
            for (uint8_t i = 0; i < Palette::kEntries; i++)
                p.entry[i] = hsvToRgb(static_cast<uint8_t>((static_cast<uint16_t>(i) * 256) / Palette::kEntries), 255, 255);
        } else {
            p.fromGradient(b.stops, b.len);
        }
        return p;
    }

private:
    // Default to a full rainbow (index 0): always colourful, so an effect renders visible output
    // before any palette is selected. setActive() (Drivers setup) overrides from the saved index.
    static inline Palette active_ = fromBuiltin(0);
};

// Emit the palette dropdown's options for a ControlType::Palette control (the PaletteOptionsFn):
// one {"name":…,"colors":"rrggbb rrggbb …"} object per built-in, the colours being the 16 entries
// as space-separated hex so the UI renders each option as a gradient swatch.
inline void paletteOptions(JsonSink& sink) {
    for (uint8_t i = 0; i < palettes::kCount; i++) {
        const Palette p = Palettes::fromBuiltin(i);
        sink.appendf("%s{\"name\":\"%s\",\"colors\":\"", i > 0 ? "," : "", palettes::kBuiltins[i].name);
        for (uint8_t e = 0; e < Palette::kEntries; e++)
            sink.appendf("%s%02x%02x%02x", e > 0 ? " " : "", p.entry[e].r, p.entry[e].g, p.entry[e].b);
        sink.append("\"}");
    }
}

}  // namespace mm
