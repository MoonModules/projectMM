#pragma once

#include <cstdint>

namespace mm {

// Light preset = the physical wire format a driver emits: channel order plus
// whether the light has a white channel. The order in this enum is index-aligned
// with kLightPresetOptions below (the Select control's option list), so the
// control's uint8 value casts straight to LightPreset. RGBW includes every
// 4-channel permutation so controllers can use white-first RGBW pixels
// without a driver-specific swap.
enum class LightPreset : uint8_t {
    RGB, RBG, GRB, GBR, BRG, BGR,
    RGBW, RBGW, GRBW, GBRW, BRGW, BGRW,
    RWGB, RWBG, GWRB, GWBR, BWRG, BWGR,
    WRGB, WRBG, WGRB, WGBR, WBRG, WBGR,
};

inline constexpr const char* kLightPresetOptions[] =
    {"RGB", "RBG", "GRB", "GBR", "BRG", "BGR",
     "RGBW", "RBGW", "GRBW", "GBRW", "BRGW", "BGRW",
     "RWGB", "RWBG", "GWRB", "GWBR", "BWRG", "BWGR",
     "WRGB", "WRBG", "WGRB", "WGBR", "WBRG", "WBGR"};
inline constexpr uint8_t kLightPresetCount =
    sizeof(kLightPresetOptions) / sizeof(kLightPresetOptions[0]);

inline constexpr uint8_t kLightPresetChannels[kLightPresetCount] = {
    3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4,
};

// Source-channel indices per output byte. Source is logical RGB plus W at index
// 3 for RGBW presets. W is explicit when the layer carries 4 channels; otherwise
// it is derived as min(R,G,B) so RGB-only effects still light RGBW strips.
inline constexpr uint8_t kLightPresetOrder[kLightPresetCount][4] = {
    {0, 1, 2, 3}, {0, 2, 1, 3}, {1, 0, 2, 3},
    {1, 2, 0, 3}, {2, 0, 1, 3}, {2, 1, 0, 3},
    {0, 1, 2, 3}, {0, 2, 1, 3}, {1, 0, 2, 3},
    {1, 2, 0, 3}, {2, 0, 1, 3}, {2, 1, 0, 3},
    {0, 3, 1, 2}, {0, 3, 2, 1}, {1, 3, 0, 2},
    {1, 3, 2, 0}, {2, 3, 0, 1}, {2, 3, 1, 0},
    {3, 0, 1, 2}, {3, 0, 2, 1}, {3, 1, 0, 2},
    {3, 1, 2, 0}, {3, 2, 0, 1}, {3, 2, 1, 0},
};

// Output correction applied per-light by each physical driver as it reads the
// shared source buffer: brightness scale, channel reorder, and (for RGBW presets)
// explicit/derived white. The Drivers container owns one Correction instance,
// rebuilds it on a brightness / light-preset change (cheap, cold path), and hands
// a const pointer to each driver child. apply() is the hot-path per-light transform.
//
// Today only NetworkSendDriver consumes it; future LED drivers (WS2812 via RMT,
// APA102 via SPI) apply the same correction before their protocol encode.
//
// Brightness uses a single 256-entry LUT applied to every channel. Gamma /
// white-balance (which need a per-channel R/G/B split) are deliberately not here
// yet — when they land, briLut becomes three tables. The name stays brightness-
// neutral (`briLut`) so the gamma addition is a fill-logic change, not a rename.
struct Correction {
    uint8_t briLut[256] = {};       // briLut[v] = (v * brightness) / 255 (scale8)
    uint8_t order[4] = {0, 1, 2, 3}; // source-channel index for each output position
    uint8_t outChannels = 3;        // 3 (RGB family) or 4 (RGBW family)
    bool    deriveWhite = false;    // RGBW presets: W = src[3] when present, else min(r,g,b)

    // Cold path: recompute the LUT + preset-derived layout. Called from Drivers on
    // setup, on a structural rebuild, and on a brightness / light-preset onUpdate.
    void rebuild(uint8_t brightness, LightPreset preset) {
        for (int v = 0; v < 256; v++) {
            briLut[v] = static_cast<uint8_t>((v * brightness) / 255);
        }
        uint8_t idx = static_cast<uint8_t>(preset);
        if (idx >= kLightPresetCount) idx = static_cast<uint8_t>(LightPreset::RGB);
        for (uint8_t i = 0; i < 4; i++) order[i] = kLightPresetOrder[idx][i];
        outChannels = kLightPresetChannels[idx];
        deriveWhite = outChannels == 4;
    }

    // Hot path: transform one source light into `out` (`outChannels` bytes).
    // Brightness via LUT, then reorder, then white. RGBW layers provide an
    // explicit W byte; RGB layers keep the legacy derived-white fallback.
    // No allocation, integer-only.
    inline void apply(const uint8_t* src, uint8_t* out, uint8_t srcChannels = 3) const {
        const uint8_t r = briLut[src[0]];
        const uint8_t g = briLut[src[1]];
        const uint8_t b = briLut[src[2]];
        if (deriveWhite) {
            const uint8_t w = srcChannels >= 4
                ? briLut[src[3]]
                : (r < g ? (r < b ? r : b) : (g < b ? g : b));  // min(r,g,b)
            const uint8_t v[4] = {r, g, b, w};
            for (uint8_t i = 0; i < outChannels; i++) out[i] = v[order[i]];
        } else {
            const uint8_t v[3] = {r, g, b};
            for (uint8_t i = 0; i < outChannels; i++) out[i] = v[order[i]];
        }
    }
};

} // namespace mm
