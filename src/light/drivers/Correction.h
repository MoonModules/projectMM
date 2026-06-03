#pragma once

#include <cstdint>

namespace mm {

// Light preset = the physical wire format a driver emits: channel order plus
// whether the light has a white channel. The order in this enum is index-aligned
// with kLightPresetOptions below (the Select control's option list), so the
// control's uint8 value casts straight to LightPreset.
enum class LightPreset : uint8_t { RGB, RBG, GRB, GBR, BRG, BGR, RGBW, GRBW };

inline constexpr const char* kLightPresetOptions[] =
    {"RGB", "RBG", "GRB", "GBR", "BRG", "BGR", "RGBW", "GRBW"};
inline constexpr uint8_t kLightPresetCount =
    sizeof(kLightPresetOptions) / sizeof(kLightPresetOptions[0]);

// Output correction applied per-light by each physical driver as it reads the
// shared source buffer: brightness scale, channel reorder, and (for RGBW presets)
// white derivation. The Drivers container owns one Correction instance, rebuilds
// it on a brightness / light-preset change (cheap, cold path), and hands a const
// pointer to each driver child. apply() is the hot-path per-light transform.
//
// Today only ArtNetSendDriver consumes it; future LED drivers (WS2812 via RMT,
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
    bool    deriveWhite = false;    // RGBW presets: W = min(r, g, b)

    // Cold path: recompute the LUT + preset-derived layout. Called from Drivers on
    // setup, on a structural rebuild, and on a brightness / light-preset onUpdate.
    void rebuild(uint8_t brightness, LightPreset preset) {
        for (int v = 0; v < 256; v++) {
            briLut[v] = static_cast<uint8_t>((v * brightness) / 255);
        }
        // Clamp out-of-range presets (corrupt persisted lightPreset cast to
        // the enum) to RGB BEFORE the switch — that way the switch stays
        // exhaustive, the compiler warns on a missing enumerator if we add
        // one without handling it (-Wswitch), and apply() always reads
        // initialised order/outChannels/deriveWhite.
        if (static_cast<uint8_t>(preset) >= kLightPresetCount) {
            preset = LightPreset::RGB;
        }
        // order[] holds the SOURCE channel index to place at each OUTPUT position.
        // Source is always RGB (indices 0=R, 1=G, 2=B), white at index 3.
        switch (preset) {
            case LightPreset::RGB:  order[0]=0; order[1]=1; order[2]=2; outChannels=3; deriveWhite=false; break;
            case LightPreset::RBG:  order[0]=0; order[1]=2; order[2]=1; outChannels=3; deriveWhite=false; break;
            case LightPreset::GRB:  order[0]=1; order[1]=0; order[2]=2; outChannels=3; deriveWhite=false; break;
            case LightPreset::GBR:  order[0]=1; order[1]=2; order[2]=0; outChannels=3; deriveWhite=false; break;
            case LightPreset::BRG:  order[0]=2; order[1]=0; order[2]=1; outChannels=3; deriveWhite=false; break;
            case LightPreset::BGR:  order[0]=2; order[1]=1; order[2]=0; outChannels=3; deriveWhite=false; break;
            case LightPreset::RGBW: order[0]=0; order[1]=1; order[2]=2; order[3]=3; outChannels=4; deriveWhite=true; break;
            case LightPreset::GRBW: order[0]=1; order[1]=0; order[2]=2; order[3]=3; outChannels=4; deriveWhite=true; break;
        }
    }

    // Hot path: transform one source light (3-channel RGB at `src`) into `out`
    // (`outChannels` bytes). Brightness via LUT, then reorder, then white. No
    // allocation, integer-only.
    inline void apply(const uint8_t* src, uint8_t* out) const {
        const uint8_t r = briLut[src[0]];
        const uint8_t g = briLut[src[1]];
        const uint8_t b = briLut[src[2]];
        if (deriveWhite) {
            const uint8_t w = r < g ? (r < b ? r : b) : (g < b ? g : b);  // min(r,g,b)
            const uint8_t v[4] = {r, g, b, w};
            for (uint8_t i = 0; i < outChannels; i++) out[i] = v[order[i]];
        } else {
            const uint8_t v[3] = {r, g, b};
            for (uint8_t i = 0; i < outChannels; i++) out[i] = v[order[i]];
        }
    }
};

} // namespace mm
