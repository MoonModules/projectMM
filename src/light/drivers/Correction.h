#pragma once

#include <cmath>   // powf: the gamma presets, cold path only

#include <cstdint>

#include "light/ChannelRole.h"
#include "light/FixtureChannels.h"   // kMotionBase + forEachMotionSlot: the layer-slot packing

namespace mm {

// A light's wire format — its channel order and whether it carries a white channel — is described by
// a ChannelRole array (roles[i] = what channel i emits), resolved from the LightPresets library into
// this Correction at cold-path rebuild time (see LightPresetsModule). Correction has one rebuild that
// takes that role array; there is no built-in preset enum here, because the curated wire orders live
// as seeded rows in the library, not as a second hard-coded list in core.

// White-derivation mode for RGBW lights. Effects write RGB only, so a driver feeding
// an RGBW fixture must SYNTHESIZE the white channel from RGB — and there is more than
// one accepted algorithm, so the method is a mode, not a fixed formula (the WLED
// "auto white" feature: None / Brighter / Accurate). None leaves white at 0 (the
// effect drives it, or the fixture's white is unused). Min takes the common white
// component min(R,G,B) — cheap, slightly desaturating. Accurate also subtracts that
// white back out of R/G/B so the total emitted color matches the RGB target rather
// than washing brighter. Applied only when the light carries a white channel
// (offWhite != kAbsent); ignored otherwise.
enum class WhiteMode : uint8_t { None, Min, Accurate };

inline constexpr const char* kWhiteModeOptions[] = {"None", "Min", "Accurate"};
inline constexpr uint8_t kWhiteModeCount =
    sizeof(kWhiteModeOptions) / sizeof(kWhiteModeOptions[0]);


// Output correction applied per-light by each physical driver as it reads the shared
// source buffer: brightness scale, channel reorder, and (for RGBW lights) white
// derivation. Each driver owns one Correction (DriverBase), rebuilds it on a
// brightness / preset / role change (cheap, cold path), and apply() is the hot-path
// per-light transform. Today NetworkSendDriver and the WS2812 LED drivers consume it.
//
// Channel model: a light is a run of `channelsPerLight` channels, each with a role
// (Red/Green/Blue/White/Pan/…). The canonical description is the driver's dynamic
// ChannelRole array — sized to the fixture, no fixed cap — which rebuild() reads to
// DERIVE the hot-path color offsets (offRed/offGreen/offBlue/offWhite): the byte
// position of each color role, or kAbsent if the light doesn't carry it. That derive
// is cold-path (once per config change), so apply() stays a branchless indexed store
// per channel — the same build-a-table-cold, read-it-hot shape as the brightness LUT.
// Non-color roles (pan/tilt/…) live in the role array for the fixture/preview to read;
// apply() only writes the color roles it derived offsets for.
//
// Brightness uses a single 256-entry LUT applied to every channel. Gamma /
// white-balance (which need a per-channel R/G/B split) are deliberately not here
// yet — when they land, briLut becomes three tables. The name stays brightness-
// neutral (`briLut`) so the gamma addition is a fill-logic change, not a rename.
struct Correction {
    static constexpr uint8_t kAbsent = 255;   // color role not carried by this light

    /// The perceptual curve the output LUT is filled through.
    ///
    /// `Cie` is the default and the standards answer: CIE 1931 lightness (CIE 15 / ISO 11664-4)
    /// models how the eye responds to luminance, which is exactly what a brightness control should
    /// be uniform in. hzeller's rpi-rgb-led-matrix, the reference HUB75 implementation, builds its
    /// table the same way.
    ///
    /// `Linear` is not a fallback but a REQUIREMENT for two cases: a downstream device that applies
    /// its own curve (correcting twice darkens as the square of the setting, the same reason the
    /// dimmer channel is held open below), and any measurement or calibration that needs the value
    /// on the wire to mean duty cycle.
    ///
    /// Gamma 2.2 is the DISPLAY convention (sRGB's effective exponent) and belongs to content that
    /// is genuinely encoded that way. Gamma 2.8 is the stage-lighting convention: it emulates the
    /// feel of a tungsten dimmer, whose flux rises as roughly the 3.4th power of voltage, rather
    /// than modeling perception. Both are offered because curve choice is a legitimate preference,
    /// and both are labeled for what they actually do.
    enum class Curve : uint8_t { Cie = 0, Gamma22, Gamma28, Linear };

    /// CIE 1931 lightness, inverted: a 0..255 control position to a 0..1 luminance fraction.
    ///
    /// The constants are load-bearing rather than tuning: 8 and 903.3 place the linear toe at
    /// (6/29)^3 so the two segments meet with a continuous SLOPE, and the cube root alone has
    /// infinite slope at zero, which is both numerically unstable and wrong for near-black. NOTE
    /// 903.3 and 116: the widely copied LED snippets carry 902.3 and 119 from a Wikipedia typo,
    /// including hzeller's own table, and the reported effect is a visibly worse low end.
    static float cieLuminance(float control255) {
        const float L = control255 * 100.0f / 255.0f;
        return (L <= 8.0f) ? (L / 903.3f)
                           : ((L + 16.0f) / 116.0f) * ((L + 16.0f) / 116.0f) * ((L + 16.0f) / 116.0f);
    }


    uint8_t briLut[256] = {};       // briLut[v] = curve(v * brightness / 255)
    /// Which perceptual curve rebuildBrightness fills through. A DRIVER's setting, because whether
    /// a curve belongs depends on what is downstream: a panel card that corrects its own pixels
    /// needs Linear here or the picture is corrected twice.
    Curve curve = Curve::Cie;
    // Derived hot-path cache: the output-byte position of each color role. Source is
    // always RGB (src[0]=R, src[1]=G, src[2]=B); the offset says where in `out` that
    // role's byte lands. Recomputed from the role array by rebuild(); GRB by default.
    uint8_t offRed = 1;
    uint8_t offGreen = 0;
    uint8_t offBlue = 2;
    uint8_t offWhite = kAbsent;     // derived white at this offset (kAbsent = light has no white)
    // Extra emitters a fixture may carry beside cold white. The theory (why each is derived the way
    // it is; the honest limits) — a full fixture model with per-emitter spectral targets is the
    // proper home, see the light backlog:
    //   • WarmWhite is a BROADBAND ILLUMINATION emitter, like cold White — a low-CCT (~2700K)
    //     phosphor white. Its achromatic basis is real, so `min(R,G,B)` (the white component) is a
    //     sound approximation; warm vs cold differ only in phosphor CCT, which a byte value can't
    //     express, so from an RGB target both get the same min(R,G,B). Rides `whiteMode` with White.
    //   • Yellow/Amber is a SATURATED NARROW-BAND HUE (~590 nm real amber die, common in RGBA/RGBAW
    //     PARs), NOT illumination. It has no honest RGB pre-image: `min(R,G)` (the R+G overlap) is a
    //     crude stand-in that reads greener than a true amber AND fires on far too much (any red+green
    //     content — yellows, whites, skin tones — muddying the fixture). So it's a "light it up to
    //     eyeball the wiring" placeholder, not a correct render.
    //   • UV (~400 nm) is OUT OF the RGB gamut entirely (no pre-image at all). It reads to the eye as
    //     deep violet, so the blue excess `max(0, B - max(R,G))` (fires on blues/purples, dark on
    //     warm colors) is a deliberate eyeball hack, honest about being one.
    // apply() drives all three off the SAME `whiteMode` gate today (None zeroes them, else the
    // approximation above). That's expedient, not right: White/WarmWhite belong under whiteMode (real
    // achromatic extraction, subtraction-aware); Yellow/UV are targetable emitters an effect should
    // drive DIRECTLY via the fixture model, not synthesize from RGB. See backlog-light § fixture model.
    uint8_t offWarmWhite = kAbsent;
    // A fixture's MASTER DIMMER channel (moving heads, and any "intensity + RGB" light). Held
    // fully open, because the per-light brightness is already in the color values via briLut:
    // dimming twice would darken the fixture as the square of the setting. It must be WRITTEN
    // though, since a linear dimmer left at 0 means the fixture emits nothing at all however
    // correct its color channels are (bench: a moving head stayed dark with a perfect RGB map).
    uint8_t offDimmer = kAbsent;
    // The FIXTURE's motion channels (pan/tilt/zoom/rotate/gobo), which are not where the layer
    // keeps them: apply() maps the layer's packed slots onto these. Never scaled by briLut, since
    // brightness is a light-output setting and scaling pan by it would swing a moving head toward
    // 0/0 as the rig dims. They come from the effect (setPan and friends) rather than being
    // synthesized from color, which is the whole point of a wide light: one buffer carries aim too.
    uint8_t offPan = kAbsent, offTilt = kAbsent, offZoom = kAbsent;
    uint8_t offRotate = kAbsent, offGobo = kAbsent;
    // "This fixture has at least one motion channel", resolved once at rebuild so the hot path
    // never scans the five offsets to discover they are all absent.
    bool hasMotion = false;
    /// Hold the rig's aim: motion stops being written to the wire, so a fixture keeps the last
    /// position it was sent. Set while the rig has been powered off long enough to be considered
    /// parked (Drivers::motionHold), and cleared the moment power returns.
    ///
    /// Here rather than upstream because this is where motion reaches the wire at all: the effect
    /// keeps running and the buffer keeps changing, so the show stays on its clock and the rig
    /// rejoins it where it now is. Freezing the WRITE instead would have stopped the show and left
    /// the buffer holding a stale cue.
    /// Written by Drivers::updateMotionHold on the render thread, read by apply() which in split
    /// mode runs on the core-1 encode task. A plain bool rather than an atomic: it is byte-sized on
    /// every supported target so a read cannot tear, and the only cost of observing the previous
    /// value is that a park or release lands one frame late against a timeout measured in tens of
    /// seconds. An atomic load here would sit in the per-light loop, which is the one place this
    /// project does not spend cycles for a race whose worst outcome is 20 ms of latency.
    bool motionHeld = false;
    uint8_t offYellow = kAbsent;
    uint8_t offUV = kAbsent;
    uint8_t outChannels = 3;        // bytes emitted per light (= channelsPerLight of the wiring)
    WhiteMode whiteMode = WhiteMode::Min;   // how white is synthesized from RGB (white lights only)

    // Refresh just the brightness LUT. Split out so a brightness-only change re-scales the LUT
    // without touching the channel offsets, and so a driver can apply brightness even when the role
    // source (the preset library) isn't available yet.
    //
    // ORDER, and it is the whole design: brightness is a LINEAR pre-scale and the curve is applied
    // LAST. A gain only composes correctly in linear light (a 0.8 white balance applied to a curved
    // value yields 0.8^2.2, not 80% of the light), and anything reasoning about physical quantities
    // (current, power) has to read linear values too. Curving the brightness CONTROL as well as the
    // values would correct twice: the fader then feels dead at the bottom and 50% looks like 15%.
    // The slider becomes perceptually uniform on its own precisely because the curve sits after it.
    void rebuildBrightness(uint8_t brightness) {
        for (int v = 0; v < 256; v++) {
            const float linear = static_cast<float>(v) * brightness / 255.0f;   // scale first
            float out = linear;
            switch (curve) {
                case Curve::Cie:     out = cieLuminance(linear) * 255.0f; break;
                case Curve::Gamma22: out = powf(linear / 255.0f, 2.2f) * 255.0f; break;
                case Curve::Gamma28: out = powf(linear / 255.0f, 2.8f) * 255.0f; break;
                case Curve::Linear:  break;
            }
            int q = static_cast<int>(out + 0.5f);
            // A non-zero input never lands on black. Every curve here crushes the low end into
            // zero at 8 bits (CIE maps 1 and 2 to 0, gamma 2.2 maps 1..5 there), so without this a
            // fade-out SNAPS off partway down and the dimmest usable settings are simply missing.
            // FastLED spells the same guard as the `_video` suffix on its gamma helpers; WLED has
            // no equivalent and its table does map 1 to 0. Costs the exactness of black only for
            // inputs that were never black.
            if (q <= 0 && v > 0 && brightness > 0) q = 1;
            briLut[v] = static_cast<uint8_t>(q > 255 ? 255 : q);
        }
    }

    // Cold path: refresh the brightness LUT and DERIVE the color-role offsets from the light's
    // channel-role array (`roles`, `nChannels` entries: the driver's dynamic array, canonical).
    // A role appearing at channel i sets that color's offset to i; a color role not present stays
    // kAbsent (apply() skips it). outChannels becomes the channel count. Motion roles set the
    // motion offsets and hasMotion, which is what apply() reads to decide whether to remap.
    void rebuild(uint8_t brightness, const ChannelRole* roles, uint8_t nChannels) {
        rebuildBrightness(brightness);
        offRed = offGreen = offBlue = offWhite = kAbsent;
        offWarmWhite = offYellow = offUV = offDimmer = kAbsent;
        offPan = offTilt = offZoom = offRotate = offGobo = kAbsent;
        hasMotion = false;
        for (uint8_t i = 0; i < nChannels; i++) {
            switch (roles[i]) {
                case ChannelRole::Red:       offRed = i;       break;
                case ChannelRole::Green:     offGreen = i;     break;
                case ChannelRole::Blue:      offBlue = i;      break;
                case ChannelRole::White:     offWhite = i;     break;
                case ChannelRole::WarmWhite: offWarmWhite = i; break;
                case ChannelRole::Yellow:    offYellow = i;    break;
                case ChannelRole::UV:        offUV = i;        break;
                case ChannelRole::Dimmer:    offDimmer = i;    break;
                case ChannelRole::Pan:       offPan = i;       break;
                case ChannelRole::Tilt:      offTilt = i;      break;
                case ChannelRole::Zoom:      offZoom = i;      break;
                case ChannelRole::Rotate:    offRotate = i;    break;
                case ChannelRole::Gobo:      offGobo = i;      break;
                default: break;   // ChannelRole::None: a channel this fixture does not use
            }
        }
        hasMotion = offPan != kAbsent || offTilt != kAbsent || offZoom != kAbsent ||
                    offRotate != kAbsent || offGobo != kAbsent;
        outChannels = nChannels;
    }

    /// Hot path: transform one source light (`srcChannels` bytes at `src`) into `out`
    /// (`outChannels` bytes). Brightness via LUT, then place each present color role at its
    /// derived offset, then synthesize white per whiteMode. No allocation, integer-only.
    /// A color role the light doesn't carry (offset == kAbsent) is simply not written, so a
    /// wiring that omits, say, red just doesn't emit it.
    ///
    /// `srcChannels` is the SOURCE light's width. Every driver passes the width it has; whether
    /// motion is carried is decided HERE, from `hasMotion` (derived in rebuild from the fixture's
    /// own roles). A sink with no motion channels never enters that branch, so it needs no say in
    /// the matter: the preset describes the fixture, and the pipeline carries whatever it declares.
    /// That is also what lets a moving-head preset be driven by an LED driver, which emits its
    /// motion bytes like any other channel: unusual, but the honest result of the wiring asked for.
    ///
    /// This is a REMAP, not a copy: motion is read from the LAYER's packed slots (kMotionBase
    /// onward, in pan/tilt/zoom/rotate/gobo order) and written to the FIXTURE's own offsets, which
    /// are usually different. On the mini moving head the fixture's pan is CH1 while the layer
    /// keeps it at slot 4, because a layer light always begins with RGB(W) and CH1 there is the
    /// red byte. Two layouts, mapped here. 0 means an RGB(W)-only source: no motion to carry.
    inline void apply(const uint8_t* src, uint8_t* out, uint8_t srcChannels) const {
        // Master dimmer wide open: brightness lives in the color values below, and a fixture whose
        // dimmer sits at 0 is simply dark. Written every frame like any other role, so a preset
        // that declares one cannot be silently unlit.
        if (offDimmer != kAbsent) out[offDimmer] = 255;
        // Motion passes through UNSCALED and by ASSIGNMENT, never additively. Two rules, both
        // borrowed from MoonLight's compositeTo ("additive semantics don't apply to positional
        // signals"): brightness must not touch these, or dimming the rig would drag every head
        // toward 0/0; and adding two layers' pan values would aim at neither of them.
        // hasMotion is precomputed at rebuild, so a fixture WITHOUT motion channels (every LED
        // strip and PAR) pays exactly one predictable branch here, not a five-slot scan per light
        // per frame. Motion support must cost nothing on the rigs that do not use it.
        // `motionHeld` parks the rig: skipping the remap leaves the fixture on its last aim, which
        // is what makes a device that has been switched off go quiet instead of sweeping in the
        // dark. Costs nothing on a rig with no motion, which never enters this branch anyway.
        if (hasMotion && srcChannels != 0 && !motionHeld) {
            // Read the LAYER slot, write the FIXTURE channel. The layer packs motion after RGBW in
            // a fixed order (FixtureChannels::kMotionBase); the fixture puts it wherever its preset
            // says. Two layouts, mapped here, which is what keeps an effect's pan write off the red
            // byte it would otherwise share.
            const bool present[5] = {offPan != kAbsent, offTilt != kAbsent, offZoom != kAbsent,
                                     offRotate != kAbsent, offGobo != kAbsent};
            const uint8_t chan[5] = {offPan, offTilt, offZoom, offRotate, offGobo};
            FixtureChannels::forEachMotionSlot(present, [&](uint8_t role, uint8_t slot) {
                if (slot < srcChannels) out[chan[role]] = src[slot];
            });
        }
        // The white math runs on the LINEAR source, and the curve is applied to what comes OUT of
        // it. min() and the Accurate subtraction are ordinary arithmetic: performed on curved
        // values they no longer mean what they say, because the amount subtracted from red does not
        // correspond to the light the white emitter adds back. Same rule that puts the curve last
        // in the pipeline, one level down.
        uint8_t r = src[0];
        uint8_t g = src[1];
        uint8_t b = src[2];
        // Every synthesized emitter (white + warm-white/yellow/UV) is gated by the ONE whiteMode:
        // None zeroes them (never a stale value — corrected_ is reused, not re-zeroed, frame to
        // frame), otherwise each is a best-effort approximation from RGB. Accurate additionally
        // subtracts the WHITE component back out of RGB (the standard RGBW auto-white behavior);
        // the other emitters are additive stand-ins only (no colorimetric model yet), so they don't
        // subtract. See the offWarmWhite/offYellow/offUV field comment for the approximation rationale.
        if (whiteMode == WhiteMode::None) {
            if (offWhite != kAbsent)     out[offWhite] = 0;
            if (offWarmWhite != kAbsent) out[offWarmWhite] = 0;
            if (offYellow != kAbsent)    out[offYellow] = 0;
            if (offUV != kAbsent)        out[offUV] = 0;
        } else {
            const uint8_t w = r < g ? (r < b ? r : b) : (g < b ? g : b);  // min(r,g,b): the white component
            // The additive stand-ins (warm-white/yellow/UV) approximate from the CORRECTED RGB — the
            // values BEFORE Accurate pulls white out below. Compute them here, off the pre-subtraction
            // r/g/b, so Accurate's `r -= w` (which only rebalances the RGB emitters) can't corrupt them.
            // warm white ≈ the white component (same as cold white for a warm-white-only strip).
            if (offWarmWhite != kAbsent) out[offWarmWhite] = briLut[w];
            // yellow ≈ min(R,G) (the shared red+green component).
            if (offYellow != kAbsent)    out[offYellow] = briLut[r < g ? r : g];
            // UV is out of gamut (no RGB pre-image), but it reads to the eye as deep blue/violet, so
            // drive it from the BLUE component that has no red/green to pair with — the violet-ish
            // excess `max(0, B - max(R,G))`. So UV fires on blues/purples, stays dark on warm colors.
            if (offUV != kAbsent) {
                const uint8_t rg = r > g ? r : g;
                out[offUV] = briLut[b > rg ? static_cast<uint8_t>(b - rg) : 0];
            }
            // White last: it's the only emitter that (in Accurate) rebalances RGB, so it must run
            // after the stand-ins have read the pre-subtraction values.
            if (offWhite != kAbsent) {
                if (whiteMode == WhiteMode::Accurate) { r -= w; g -= w; b -= w; }  // pull white out of RGB
                out[offWhite] = briLut[w];
            }
        }
        // The curve, applied ONCE, to each emitter as it is written. Everything above this line is
        // linear light, which is what lets min(), the subtraction and the stand-in approximations
        // mean what they say.
        if (offRed != kAbsent)   out[offRed] = briLut[r];
        if (offGreen != kAbsent) out[offGreen] = briLut[g];
        if (offBlue != kAbsent)  out[offBlue] = briLut[b];
    }
};

} // namespace mm
