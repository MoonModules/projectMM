#pragma once

#include "core/moonlive/MoonLive.h"
#include "light/moonlive/MoonLiveScript.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "light/Palette.h"

// MoonLivePalette: a PALETTE authored as a script rather than stored as data.
//
// The fifth binding, and the one that is not a shape the other four have. An effect writes a color
// per light, a layout a position, a modifier a coordinate, a service a control. A palette writes
// SIXTEEN ENTRIES, once per frame, and every effect in that frame then samples them.
//
// **Why a script and not a gradient.** A stop list is frozen the moment it is saved: it cannot
// follow the music, drift, or be computed. A palette that is code can do all three, which is the
// whole reason this exists rather than a JSON palette file. The audio builtins are already in the
// shared table, so `audioBand()` in a palette costs nothing to expose and is the case that
// justifies the design.
//
// **Cost is independent of rig size**, which is what makes running it in the render path
// reasonable. An effect body runs once per light (256 times on a 16x16 grid); a palette runs a
// sixteen-iteration loop whatever the wall is. So the per-frame cost of a scripted palette is
// closer to a single light's effect work than to a frame of it.
//
// **It fills a SCRATCH palette, not the live one.** Effects sample `Palettes::active()` on the same
// thread, between palette ticks, but a script writing the global directly would leave a half-written
// table visible for the length of its own loop. Filling a local and assigning the 48 bytes once
// means an effect sees either the previous palette or the new one, never a mixture, and it removes
// the whole class of problem rather than narrowing the window.
//
// **A broken script keeps the last good palette.** The other bindings degrade to dark, which is the
// honest answer when the script IS the picture. Here it is not: the effects still run, so dropping
// the palette to black would blame the effect for the palette's fault. The last good table stays,
// and the error goes to the status line.

namespace mm {

/// A palette whose sixteen entries are computed by a MoonLive script, once per frame.
class MoonLivePalette {
public:
    /// Point the palette at a script. The next prepare() compiles it.
    void setScript(const char* name) { script_.setName(name); }
    const char* scriptName() const { return script_.name(); }

    /// Compile if the named file changed. Reports status and dynamic bytes through `owner`, the
    /// same way every other binding does, so a scripted palette's errors appear on a card.
    void prepare(MoonModule& owner) {
        script_.sync(moonlive::effectSysVars(), owner, moonlive::lightBuiltins());
    }

    /// Publish the script's controls, so a scripted palette is configurable without editing it.
    void publishControls(ControlList& controls) { script_.publishDeclaredControls(controls); }

    bool ok() const { return script_.ok(); }

    void release() {
        script_.engine().free();
        script_.invalidate();
    }

    /// Run the script for this frame and install the result. Returns false when there is nothing to
    /// run, so the caller can leave the built-in palette alone.
    ///
    /// `t` is the elapsed milliseconds every other binding is handed, so `beat` and `beatsin` mean
    /// the same thing in a palette as in an effect.
    bool tick(uint32_t nowMs) MM_NONBLOCKING {
        if (!script_.ok()) return false;
        if (!script_.engine().hasEntry(moonlive::kEntryTick)) return false;

        // Seeded from the palette currently active, so a script that writes only some entries
        // leaves the rest as they were rather than showing whatever was on the stack.
        Palette scratch = *Palettes::active();
        moonlive::setPalSink([](void* ctx, uint8_t i, uint8_t r, uint8_t g, uint8_t b) {
            static_cast<Palette*>(ctx)->entry[i] = RGB{r, g, b};
        }, &scratch);
        // runValue, not run(): run() refuses a call with no light buffer, which is the right
        // precondition for an effect and wrong here, because a palette paints nothing. Same reason
        // MoonLiveService uses it.
        script_.engine().runValue(moonlive::kEntryTick, moonlive::RetType::Void, 0,
                                  nullptr, 0, 0, nowMs);
        moonlive::setPalSink(nullptr, nullptr);

        // One assignment of 48 bytes: an effect samples a whole palette or the previous one.
        Palettes::setActiveDirect(scratch);
        return true;
    }

    /// The palette script the frame should run, or nullptr.
    ///
    /// A static seam, the same shape `Palettes::active()` and `AudioService::latestFrame()` use, and
    /// for the same reason: the script is OWNED by Drivers (which owns the palette control), but it
    /// has to run BEFORE the effects sample the palette, and Drivers ticks after them. A seam is one
    /// pointer rather than a wire from the effects container to a driver container it otherwise
    /// knows nothing about.
    static MoonLivePalette* active() { return active_; }
    static void setActiveInstance(MoonLivePalette* p) { active_ = p; }
    /// Detach, for an owner whose storage is about to go away. Takes the caller's own instance so a
    /// departing owner cannot unpublish somebody else's, exactly as LivePalettes::clear does: this
    /// is a static pointer INTO a Drivers member, and `Effects::tick` dereferences it every frame,
    /// so a Drivers released or destroyed while still published leaves the render path running a
    /// script in freed memory.
    static void clearActiveInstance(const MoonLivePalette* p) { if (active_ == p) active_ = nullptr; }

    /// Run the active palette script, if there is one. Called once per frame before the layers.
    static void tickActive(uint32_t nowMs) MM_NONBLOCKING {
        if (active_) active_->tick(nowMs);
    }

private:
    moonlive::MoonLiveScript script_;
    static inline MoonLivePalette* active_ = nullptr;
};

}  // namespace mm
