#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"   // layer()->buffer()
#include "light/Palette.h"        // colorFromPalette, Palettes::active()
#include "light/draw.h"           // draw::pixel, draw::fade
#include "core/math8.h"           // beat8
#include "core/AudioModule.h"     // AudioModule::latestFrame()
#include "core/AudioFrame.h"      // AudioFrame::bands[16]

#include <cstring>                // memset (clear the per-band state on (re)build)

namespace mm {

// Freq Saws: one vertical "saw" per audio band, each rising and falling like a tilt-shifted sawtooth
// whose run-rate is driven by that band's loudness. Each frame the buffer fades a little (motion
// trail), then every band sets a target speed from its current loudness; the band's speed RISES
// instantly to a loud hit (max with the target) and DECAYS slowly back toward zero when the sound
// stops, so a struck band keeps sawing for a while and a quiet one winds down. The current speed
// becomes a BPM (0..bpmMax), and that BPM picks the Y position of the lit pixel via one of three
// methods:
//   0 "Chaos"      — y straight off beat8(bpm): the BPM jumps with the band, so the saw teleports
//                    (visually chaotic, the original look).
//   1 "Chaos fix"  — same beat8(bpm) but a per-band phase offset is carried so a BPM change continues
//                    from the current sawtooth position instead of snapping (a smoother chaos).
//   2 "BandPhases" — a per-band phase accumulator integrated from the BPM each frame (deltaMs-scaled),
//                    so the saw advances continuously and never jumps (the default, smoothest).
// The band physics run ONCE per tick (a loop over the 16 bands), caching each band's Y; the column
// loop then just maps x→band and draws that band's cached Y — so a band spanning many columns on a
// wide panel integrates exactly once per frame, not once per column (WLED's per-band-per-frame
// physics). `invert` mirrors every other column (x even) top-to-bottom for a woven look; `keepOn`
// keeps a band drawing even once its speed has fully decayed (so the panel never goes fully dark
// between hits).
//
// Prior art: MoonLight's FreqSaws (E_MoonModules / MoonModules), an audio-reactive matrix effect. The
// per-band rise/decay physics, the three position methods, the bpmMax / increaser / decreaser knobs,
// and the per-band phase bookkeeping are reproduced exactly here, written fresh on projectMM's
// EffectBase + the shared draw / palette / beat8 primitives. Reads AudioModule::latestFrame();
// silence → every band decays → flat → dark, safe on any target and grid size.
// Author: @TroyHacks (MoonLight / WLED MoonModules) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
class FreqSawsEffect : public EffectBase {
public:
    const char* tags() const override { return "💫📊"; }  // MoonLight origin · audio
    Dim dimensions() const override { return Dim::D2; }   // writes only the z=0 slice; extrude fills z

    // Defaults match MoonLight's FreqSaws exactly.
    uint8_t fade      = 4;     // per-frame fade-to-black amount (motion trail)
    uint8_t increaser = 211;   // gain from band loudness into target speed
    uint8_t decreaser = 18;    // decay rate when a band falls silent (0 = never decays)
    uint8_t bpmMax    = 198;   // top BPM a fully-sped band maps to
    bool    invert    = false; // mirror every even column top↔bottom
    bool    keepOn    = false; // keep drawing a band whose speed has decayed to zero
    uint8_t method    = 2;     // 0 Chaos, 1 Chaos fix, 2 BandPhases

    void onBuildControls() override {
        controls_.addUint8("fade", fade, 0, 255);
        controls_.addUint8("increaser", increaser, 0, 255);
        controls_.addUint8("decreaser", decreaser, 0, 255);
        controls_.addUint8("bpmMax", bpmMax, 0, 255);
        controls_.addBool("invert", invert);
        controls_.addBool("keepOn", keepOn);
        static constexpr const char* kMethodOptions[] = {"Chaos", "Chaos fix", "BandPhases"};
        controls_.addSelect("method", method, kMethodOptions, 3);
    }

    // Per-band state is a fixed 16-element set (one per GEQ channel, NOT per light), so it stays a
    // small inline member — the "no large inline members" rule targets per-light buffers sized to
    // nrOfLights, which this isn't. Cleared on every (re)build so a grid/control change starts the
    // bands from rest.
    void onBuildState() override {
        clearState();
        MoonModule::onBuildState();
    }

    void loop() override {
        const int sizeX = width();
        const int sizeY = height();
        if (sizeX <= 0 || sizeY <= 0 || channelsPerLight() < 3) return;

        const AudioFrame* f = AudioModule::latestFrame();
        if (!f) return;   // null-safe (latestFrame returns silence, never null, but guard regardless)

        Buffer& buf = layer()->buffer();
        const Coord3D dims{static_cast<lengthType>(sizeX), static_cast<lengthType>(sizeY), depthDim()};

        layer()->fadeToBlackBy(fade);

        // deltaMs since the last frame — drives the band decay and the BandPhases integrator.
        const unsigned long now = elapsed();
        const unsigned long deltaMs = now - lastTime;
        lastTime = now;

        // Advance the 16-band physics ONCE per tick and cache each band's Y + active flag, so a band
        // spanning several columns on a wide panel integrates once per frame (not once per column).
        bool    bandActive[NUM_GEQ_CHANNELS] = {};
        uint8_t bandY[NUM_GEQ_CHANNELS]      = {};
        for (int band = 0; band < NUM_GEQ_CHANNELS; band++) {
            const uint8_t volume = f->bands[band];
            // targetSpeed = volume * increaser * 257 — scaled into the 16-bit speed space (≈ ×65535).
            const uint32_t targetSpeed = static_cast<uint32_t>(volume) * increaser * 257u;

            if (volume > 0) {
                // Rise instantly to a loud hit: take the louder of current and target speed.
                if (targetSpeed > bandSpeed[band])
                    bandSpeed[band] = static_cast<uint16_t>(targetSpeed > 65535u ? 65535u : targetSpeed);
            } else if (decreaser > 0 && bandSpeed[band] > 0) {
                // Decay back toward zero when silent, proportional to elapsed time and 1/decreaser.
                uint32_t decay = (static_cast<uint32_t>(bandSpeed[band]) * deltaMs) /
                                 (static_cast<uint32_t>(decreaser) * 10u);
                if (decay < 1) decay = 1;
                bandSpeed[band] = decay >= bandSpeed[band] ? 0
                                  : static_cast<uint16_t>(bandSpeed[band] - decay);
            }

            if (bandSpeed[band] > 1 || keepOn) {
                bandActive[band] = true;
                // Current speed → a BPM in 0..bpmMax.
                const uint8_t bpm = static_cast<uint8_t>(imap(bandSpeed[band], 0, 65535, 0, bpmMax));

                if (method == 0) {
                    // Chaos: y straight off the beat — jumps as the BPM changes.
                    bandY[band] = static_cast<uint8_t>(imap(beat8(bpm, now), 0, 255, 0, sizeY - 1));
                } else if (method == 1) {
                    // Chaos fix: carry a per-band phase offset so a BPM change continues from the
                    // current sawtooth position instead of snapping.
                    if (bpm != lastBpm[band]) {
                        const uint8_t currentPos = static_cast<uint8_t>(beat8(lastBpm[band], now) + phaseOffset[band]);
                        const uint8_t newPos = beat8(bpm, now);
                        phaseOffset[band] = static_cast<uint8_t>(currentPos - newPos);
                        lastBpm[band] = bpm;
                    }
                    bandY[band] = static_cast<uint8_t>(imap(static_cast<uint8_t>(beat8(bpm, now) + phaseOffset[band]),
                                                            0, 255, 0, sizeY - 1));
                } else {
                    // BandPhases: integrate a per-band phase accumulator from the BPM each frame
                    // (deltaMs-scaled), halved, so the saw advances continuously with no jumps.
                    // phaseInc = (bpm * deltaMs * 65536) / (60 * 1000); phaseInc /= 2.
                    uint32_t phaseInc = (static_cast<uint32_t>(bpm) * static_cast<uint32_t>(deltaMs) * 65536u) /
                                        (60u * 1000u);
                    phaseInc /= 2u;
                    bandPhase[band] = static_cast<uint16_t>(bandPhase[band] + phaseInc);
                    bandY[band] = static_cast<uint8_t>(imap(bandPhase[band] >> 8, 0, 255, 0, sizeY - 1));
                }
            }
        }

        // Column loop: map each x onto its band and draw that band's cached Y. Per-column concerns
        // (invert mirroring, palette colour) stay here; the band physics already ran above.
        for (int x = 0; x < sizeX; x++) {
            // Map this column onto one of the 16 GEQ bands (band = map(x, 0, sizeX, 0, 16)).
            int band = imap(x, 0, sizeX, 0, NUM_GEQ_CHANNELS);
            if (band < 0) band = 0;
            if (band > NUM_GEQ_CHANNELS - 1) band = NUM_GEQ_CHANNELS - 1;

            if (!bandActive[band]) continue;

            const uint8_t y = bandY[band];
            // invert mirrors every even column (x % 2 == 0) top-to-bottom.
            const int drawY = (invert && (x % 2 == 0)) ? (sizeY - 1 - y) : y;
            const uint8_t colorIndex = static_cast<uint8_t>(imap(x, 0, sizeX - 1, 0, 255));
            const RGB col = colorFromPalette(*Palettes::active(), colorIndex);
            draw::pixel(buf, dims, {static_cast<lengthType>(x), static_cast<lengthType>(drawY), 0}, col);
        }
    }

private:
    static constexpr int NUM_GEQ_CHANNELS = 16;

    // Standard integer map (MoonLight's ::map), used for the band/colour/position remaps. Guards a
    // zero input span so a degenerate grid (sizeX/sizeY <= 1) can't divide by zero.
    static int imap(int v, int inLo, int inHi, int outLo, int outHi) {
        const int den = inHi - inLo;
        if (den == 0) return outLo;
        return (v - inLo) * (outHi - outLo) / den + outLo;
    }

    lengthType depthDim() const { return depth() > 0 ? depth() : 1; }

    void clearState() {
        std::memset(bandSpeed, 0, sizeof(bandSpeed));
        std::memset(bandPhase, 0, sizeof(bandPhase));
        std::memset(lastBpm, 0, sizeof(lastBpm));
        std::memset(phaseOffset, 0, sizeof(phaseOffset));
        lastTime = 0;
    }

    // Per-band physics state (one entry per GEQ channel, fixed 16). Small enough to stay inline.
    uint16_t bandSpeed[NUM_GEQ_CHANNELS]   = {};   // current saw run-rate (16-bit speed space)
    uint16_t bandPhase[NUM_GEQ_CHANNELS]   = {};   // BandPhases (method 2) phase accumulator
    uint8_t  lastBpm[NUM_GEQ_CHANNELS]     = {};   // Chaos fix (method 1) previous BPM
    uint8_t  phaseOffset[NUM_GEQ_CHANNELS] = {};   // Chaos fix (method 1) carried phase offset
    unsigned long lastTime                 = 0;    // elapsed() at the previous frame (for deltaMs)
};

} // namespace mm
