#pragma once

#include "core/moonlive/MoonLive.h"
#include "light/modifiers/ModifierBase.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include <cstdio>
#include <cstring>

// MoonLiveModifier — a scripted MODIFIER: a coordinate transform authored live as a script instead
// of compiled in as a C++ class.
//
// This is the second binding of the MoonLive engine after MoonLiveEffect, and it is what shows the
// engine is domain-neutral: it needed no engine, IR, grammar or backend change. An effect script
// writes a COLOUR per light; a modifier script writes a POSITION. Both are "three values stored at
// an index", which is what the engine's StoreElem already emits.
//
// **The script does not loop, because the Layer already does.** `modifyLogical` is called once per
// physical light by the Layer's fold walk (Layer.h, the mapping build), so a script only ever
// transforms ONE coordinate. That is exactly the shape the grammar has today — `call(expr, …)` —
// which is why a modifier is the binding that fits the language as it stands. A scripted LAYOUT, by
// contrast, has to place N different positions in a single pass with no per-light call to ride on,
// so it needs a loop the language does not have yet; that is why this comes first.
//
// **How the script reads the coordinate.** `x`, `y` and `z` are ordinary script controls, declared
// by this module rather than by the author: prepare() prepends three `uint8_t` declarations to the
// user's source, so a bare `x` in an expression compiles to the same LoadCtrl any control read
// uses. Before each call the binding writes the light's position into those arena slots. No new IR
// op, no compiler special case — the existing control mechanism carries the inputs.
//
// **Coordinates are bytes, so an axis spans 0..255.** A control slot is one byte, which is the
// price of reusing the control path for inputs. That covers every grid we drive today; a wall
// longer than 255 on one axis (the 48x256 wall is exactly at it) needs the 16-bit element store
// that is backlogged with the same reason.

namespace mm {

/// Modifier whose coordinate transform is a live-authored MoonLive script.
class MoonLiveModifier : public ModifierBase {
public:
    const char* tags() const override { return "📝"; }   // scripted

    void defineControls() override {
        controls_.addTextArea("source", source_, sizeof(source_));
        // The controls the SCRIPT declared, minus the three the binding injects: `x`, `y` and `z`
        // are inputs the Layer writes per light, not sliders a user sets, so they stay out of the
        // UI while still being ordinary arena slots to the compiler.
        uint8_t n = 0;
        const moonlive::DeclaredControl* decls = engine_.declaredControls(n);
        for (uint8_t i = kInputCount; i < n; i++) {
            uint8_t* slot = engine_.controlSlot(decls[i].offset);
            if (!slot) continue;
            std::memcpy(ctrlNames_[i], decls[i].name, decls[i].nameLen);
            ctrlNames_[i][decls[i].nameLen] = '\0';
            controls_.addUint8(ctrlNames_[i], *slot, decls[i].min, decls[i].max);
        }
    }

    /// Compile the script, with the coordinate inputs prepended.
    ///
    /// ModifierBase::affectsPrepare returns true for every control, which is right here: a source
    /// edit and a scripted-control move both change where lights land, and the Layer has to rebuild
    /// its mapping either way.
    void prepare() override {
        // Prepend the input declarations. The user's script sees `x`, `y`, `z` as ordinary control
        // reads; the binding sees three arena slots it can write per light.
        std::snprintf(full_, sizeof(full_),
                      "uint8_t x = 0;\nuint8_t y = 0;\nuint8_t z = 0;\n"
                      "uint8_t width = 0;\nuint8_t height = 0;\nuint8_t depth = 0;\n%s", source_);

        if (engine_.compile(full_, moonlive::lightBuiltins())) {
            clearStatus();
        } else {
            setStatus(engine_.error(), Severity::Error);
        }
        rebuildControls();
        setDynamicBytes(engine_.ok() ? engine_.codeCap() : 0);
        // Ask for a rebuild ONLY when the script actually changed. modifyLogical is the static
        // hook — it runs while the Layer builds its mapping — so an edit is invisible until the
        // Layer rebuilds. But the rebuild the Layer performs IS applyState(), which calls prepare()
        // again: setting the flag unconditionally makes the two call each other forever, the
        // mapping is rebuilt every frame, and the fixture renders nothing at all. Comparing the
        // compiled source is what breaks that cycle.
        if (std::strcmp(full_, compiled_) != 0) {
            std::snprintf(compiled_, sizeof(compiled_), "%s", full_);
            needsRebuild_ = true;
        }
    }

    /// The Layer polls this after ticking its modifiers and rebuilds its mapping once if any asks.
    bool consumeNeedsRebuild() override {
        const bool r = needsRebuild_;
        needsRebuild_ = false;
        return r;
    }

    /// The Layer hands every modifier the running logical box before it folds any coordinate.
    /// Stash it so the script can read `width`/`height`/`depth`.
    void modifyLogicalSize(Coord3D& size) override { box_ = size; }

    /// Transform one coordinate. Called by the Layer once per physical light while it builds the
    /// mapping — the cold path, not per frame.
    bool modifyLogical(Coord3D& pos) const override {
        if (!engine_.ok()) return true;   // a broken script passes coordinates through unchanged
        // A control slot is a byte: a coordinate outside 0..255 cannot be represented, so it is
        // passed through untransformed rather than silently wrapping to a wrong position.
        if (pos.x < 0 || pos.x > 255 || pos.y < 0 || pos.y > 255 || pos.z < 0 || pos.z > 255)
            return true;

        auto* self = const_cast<MoonLiveModifier*>(this);
        uint8_t* sx = self->engine_.controlSlot(0);
        uint8_t* sy = self->engine_.controlSlot(1);
        uint8_t* sz = self->engine_.controlSlot(2);
        if (!sx || !sy || !sz) return true;
        *sx = static_cast<uint8_t>(pos.x);
        *sy = static_cast<uint8_t>(pos.y);
        *sz = static_cast<uint8_t>(pos.z);
        // The box, clamped into the byte a control slot holds. A grid wider than 255 reports 255,
        // which is wrong but bounded — and that axis already cannot be scripted at all (the input
        // guard above passes it straight through), so no script sees the clamped value.
        auto clamp255 = [](lengthType v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
        if (uint8_t* sw = self->engine_.controlSlot(3)) *sw = clamp255(box_.x);
        if (uint8_t* sh = self->engine_.controlSlot(4)) *sh = clamp255(box_.y);
        if (uint8_t* sd = self->engine_.controlSlot(5)) *sd = clamp255(box_.z);

        // One light's worth of destination. The script addresses it as index 0 today; the
        // index argument is real (setXYZ(index, x, y, z), the same shape as setRGB), so a
        // script written against a future `for` loop uses the identical call.
        uint8_t out[3] = {*sx, *sy, *sz};   // seeded with the input, so a script that writes
                                            // nothing leaves the coordinate untouched
        self->engine_.run(out, 1, 3, 0);

        pos.x = static_cast<lengthType>(out[0]);
        pos.y = static_cast<lengthType>(out[1]);
        pos.z = static_cast<lengthType>(out[2]);
        return true;
    }

    const char* sourceForTest() const { return source_; }
    Coord3D boxForTest() const { return box_; }

    void release() override {
        engine_.free();
        ModifierBase::release();
    }

    /// Replace the script. The next prepare() compiles it — the same path a UI edit takes, so a
    /// test and a user exercise identical code.
    void setSource(const char* s) {
        if (!s) return;
        std::snprintf(source_, sizeof(source_), "%s", s);
    }

private:
    /// The declarations prepended to every script, in arena order: the coordinate being folded,
    /// then the box it lives in. A script needs the EXTENT to write a mirror at all — reflecting
    /// around a hard-coded 255 sends every light outside a 16-wide grid, the Layer drops them as
    /// out of bounds, and the fixture goes black.
    static constexpr uint8_t kInputCount = 6;

    mutable moonlive::MoonLive engine_;

    // Default script — a mirror on x. Chosen because it is instantly readable on a bench strand
    // (the pattern runs the other way) and is a modifier people actually reach for, so a working
    // binding looks like something rather than like nothing.
    char source_[384] = "setXYZ(0, width - 1 - x, y, z);";

    // The user's source with the input declarations prepended — what actually gets compiled.
    char full_[512] = {};
    // The source the CURRENT mapping was built from; a rebuild is needed only when it changes.
    char compiled_[512] = {};

    char ctrlNames_[moonlive::kMaxCtrls][moonlive::kMaxControlName] = {};

    bool needsRebuild_ = false;   // a recompile happened; the Layer's mapping is stale
    Coord3D box_{0, 0, 0};        // the logical box, from modifyLogicalSize
};

}  // namespace mm
