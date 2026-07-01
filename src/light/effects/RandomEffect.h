#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer(), nrOfLights()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::fade (whole-buffer fadeToBlackBy)
#include "core/math8.h"           // Random8

namespace mm {

// Random: each frame the whole buffer is dimmed a little, then exactly ONE randomly chosen
// light is lit to a random palette colour. Over many frames this scatters fading sparkles of
// colour across the whole volume — a slow, twinkling field whose density is set by the fade
// amount (less fade = pixels linger and the field fills; more fade = sparse, quick-decaying
// specks).
//
// Prior art: MoonLight's Random effect (E_MoonModules / MoonModules). The behaviour is
// reproduced exactly — one fadeToBlackBy(fade) plus one setRGB(random index, palette[random])
// per frame — written fresh on EffectBase + the shared draw/Palette primitives. The light is
// chosen by a flat light index across all nrOfLights (the engine's native ordering, the direct
// equivalent of MoonLight's index-based setRGB), so it can land anywhere in a 1D/2D/3D layer.
class RandomEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin
    // D3: the single lit light is picked by flat index over the entire volume, so this effect
    // writes into any z slice — it iterates (addresses) every axis the layer has.
    Dim dimensions() const override { return Dim::D3; }

    uint8_t fade = 70;  // per-frame fadeToBlackBy amount (0..255)

    void onBuildControls() override {
        controls_.addUint8("fade", fade, 0, 255);
    }

    void loop() override {
        Buffer& buf = layer()->buffer();
        const nrOfLightsType n = nrOfLights();
        const uint8_t cpl = buf.channelsPerLight();
        if (n == 0 || cpl < 1) return;

        // Dim the whole buffer (source: layer->fadeToBlackBy(fade)).
        draw::fade(buf, fade);

        // Light one random light to a random palette colour (source:
        // setRGB(random16(nrOfLights), ColorFromPalette(pal, random8()))). The index is a flat
        // light index — the engine's native light ordering — so the write goes straight into the
        // buffer at that light, the direct equivalent of MoonLight's index-based setRGB. (There is
        // no flat-index draw primitive; draw::pixel takes a coordinate, hence the byte write here.)
        const nrOfLightsType idx = static_cast<nrOfLightsType>(rng_.next16() % n);
        const RGB c = colorFromPalette(*Palettes::active(), rng_.next8());

        const size_t off = static_cast<size_t>(idx) * cpl;
        if (off + (cpl < 3 ? cpl : 3) > buf.bytes()) return;
        uint8_t* d = buf.data();
        d[off + 0] = c.r;
        if (cpl >= 2) d[off + 1] = c.g;
        if (cpl >= 3) d[off + 2] = c.b;
    }

private:
    Random8 rng_;  // per-effect PRNG (deterministic, independent sequence)
};

} // namespace mm