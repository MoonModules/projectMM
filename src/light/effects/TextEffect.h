#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"      // layer()->buffer()
#include "light/Palette.h"           // colorFromPalette, Palettes::active()
#include "light/draw.h"              // draw::text / draw::glyph / draw::fill
#include "light/fonts.h"             // fonts::kAll — the selectable bitmap fonts
#include "core/math8.h"              // beat8-style time (elapsed())

#include <cstring>                   // strlen, strchr

namespace mm {

// Text: renders a multi-line string on the grid in a selectable bitmap font. By DEFAULT the text is
// STATIC — laid out from the top-left, each `\n` dropping one font-height, clipped where it runs off
// the grid. Turn on `scroll` to march the whole block leftwards as a marquee (wrapping), at `speed`.
// The colour comes from the active palette (one index, so it follows the global palette control).
//
// Multi-line entry uses the shared TextArea control (the same widget MoonLive's `source` uses — a
// real <textarea>), so a user types several lines and each renders on its own row. The bitmap glyph
// blitter (draw::text / draw::glyph, carrying the public raster-fonts data) does the per-pixel work,
// so this effect stays short.
//
// Prior art: MoonLight's Scrolling Text (E_MoonLight) — the font set (4x6 / 6x8) and the scroll idea
// are carried; extended here to multi-line static-by-default text with a scroll toggle, written fresh
// on projectMM's EffectBase + the shared draw/font primitives. (MoonLight's IP/FPS/uptime presets are
// a separate follow-up — see backlog — kept out so a light effect doesn't reach into system state.)
// Author: projectMM original, on MoonLight's Scrolling Text — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
class TextEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }   // MoonLight origin
    Dim dimensions() const override { return Dim::D2; }   // draws on the z=0 plane; extrude fills z

    char    text_[128] = "projectMM";     // the (multi-line) string to show; TextArea preserves '\n'
    bool    scroll = false;               // false = static top-left; true = horizontal marquee
    uint8_t font   = 1;                   // index into fonts::kAll (0 = 4x6, 1 = 6x8)
    uint8_t speed  = 30;                  // marquee speed (pixels/sec-ish); only used when scrolling
    uint8_t hue    = 0;                   // palette index for the text colour

    void onBuildControls() override {
        controls_.addTextArea("text", text_, sizeof(text_));
        controls_.addBool("scroll", scroll);
        static constexpr const char* kFontOptions[] = {"4x6", "6x8"};
        controls_.addSelect("font", font, kFontOptions, fonts::kCount);
        controls_.addUint8("speed", speed, 1, 255);
        controls_.addUint8("hue", hue, 0, 255);
    }

    void loop() override {
        const int w = width();
        const int h = height();
        if (w <= 0 || h <= 0 || channelsPerLight() < 3) return;

        Buffer& buf = layer()->buffer();
        const Coord3D dims{static_cast<lengthType>(w), static_cast<lengthType>(h), depthDim()};
        const fonts::Font& f = fonts::kAll[font < fonts::kCount ? font : 0];
        const RGB colour = colorFromPalette(*Palettes::active(), hue);

        draw::fill(buf, {0, 0, 0});   // text is redrawn whole each frame; clear first

        if (!scroll) {
            // Static: top-left, newlines wrap down one font-height. draw::text handles '\n'.
            draw::text(buf, dims, f, text_, 0, 0, colour);
            return;
        }

        // Marquee: scroll the WHOLE block leftwards. The x offset advances with time and wraps over
        // the block's pixel width + the grid width, so the text runs off the left and re-enters from
        // the right seamlessly. Each source line scrolls on its own row (newlines still wrap down).
        const int blockW = pixelWidth(text_, f);
        const int span = blockW + w;                          // one full cycle: block clears then re-enters
        const int off = span > 0 ? static_cast<int>((elapsed() * static_cast<uint32_t>(speed) / 1000u) % span) : 0;
        // startX runs from +w (just off the right edge) down to -blockW (fully scrolled off the left).
        const lengthType startX = static_cast<lengthType>(w - off);
        draw::text(buf, dims, f, text_, startX, 0, colour);
    }

private:
    lengthType depthDim() const { return depth() > 0 ? depth() : 1; }

    // The pixel width of the widest line of `str` in font `f` (glyphs advance f.width each; '\n'
    // starts a new line). Used to size the marquee's wrap cycle.
    static int pixelWidth(const char* str, const fonts::Font& f) {
        int widest = 0, cur = 0;
        for (const char* p = str; *p; p++) {
            if (*p == '\n') { if (cur > widest) widest = cur; cur = 0; }
            else cur += f.width;
        }
        return cur > widest ? cur : widest;
    }
};

}  // namespace mm
