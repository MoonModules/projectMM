#pragma once

#include "core/MoonModule.h"
#include "light/light_types.h" // lengthType, nrOfLightsType, Dim

namespace mm {

class ModifierBase : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Modifier; }

    // A modifier control change alters the mapping, so the owning Layer must rebuild
    // it — the pipeline-wide rebuild path. See MoonModule::onUpdate.
    bool controlChangeTriggersBuildState(const char* /*controlName*/) const override { return true; }

    // Which axes the modifier can transform. Defaults to D3 — a modifier that
    // touches the mapping is assumed to work in 3D unless it declares otherwise.
    // The UI uses this to render the 📏/🟦/🧊 chip so the user can see at a
    // glance whether a modifier will do anything along z. Distinct from
    // EffectBase::dimensions() (which controls Layer extrusion); here it is
    // purely an advisory chip, never read in the render path.
    virtual Dim dimensions() const { return Dim::D3; }

    // --- Fold interface (composable modifiers) ---------------------------------
    // A modifier is a coordinate transform. The Layer builds its mapping by walking
    // the PHYSICAL lights and folding each through every enabled modifier in child
    // order — the result is the chain M₁∘M₂∘…∘Mₙ collapsed into one mapping, so
    // ordering several modifiers on a Layer "just works" (Region then Multiply-
    // mirror then Rotate). Three tiers, each a no-op by default so a modifier
    // implements only the ones it needs.
    //
    // Prior art: the two building blocks are the textbook image-warping pattern —
    // bake a coordinate transform into a precomputed spatial LUT (geometry is
    // static, so compute the table once and only read it per frame), and build that
    // table by BACKWARD mapping (walk the destinations, find each one's source) so
    // no output pixel is ever left unmapped. What is specific here — and credited to
    // MoonLight (M_MoonLight.h), the prior engine this is modelled on — is collapsing
    // a whole *chain* of discrete pixel folds into ONE index table. A PC node graph
    // (TouchDesigner, shader graphs) gives each node its own frame buffer; an ESP32
    // can't spare a buffer per modifier, so the chain is folded into a single LUT
    // and the hot path stays one gather. That MCU-budget synthesis is the borrowed
    // idea, written fresh against our MappingLUT here.

    // STATIC, build-time, once per rebuild in child order: fold the logical box.
    // Multiply divides it, Region crops it, a mask leaves it. `size` is the running
    // logical box (starts at the physical box; each modifier reshapes it for the
    // next). A modifier that needs the box in its per-light fold STASHES it here
    // (the MoonLight `modifierSize` pattern) — so modifyLogical reads its own stage's
    // box from itself, and the Layer needs no per-stage box array. Non-const: the
    // stash mutates the modifier.
    virtual void modifyLogicalSize(Coord3D& /*size*/) {}

    // STATIC, build-time, per physical light in child order: fold a coordinate into
    // this stage's logical space (in place). The coord enters in the box this
    // modifier saw at modifyLogicalSize time and leaves in the box it produced, so
    // the next modifier in the chain continues from here. Return false to REJECT —
    // the coordinate has no logical source (a mask drops it, a region light falls
    // outside the crop). A bool, not a sentinel coord: a later modifier's `% size`
    // can't alias a sentinel back into range. The modifier reads any box it needs
    // from its own stash (see modifyLogicalSize).
    virtual bool modifyLogical(Coord3D& /*pos*/) const { return true; }

    // DYNAMIC, per-frame at render time: remap a coordinate without rebuilding the
    // mapping (smooth rotation/scroll). The Layer runs this pass ONLY when some
    // enabled modifier overrides it (hasModifyLive()), so a static-only chain pays
    // nothing per frame — the render path stays at full speed. `logical` is the box.
    virtual void modifyLive(Coord3D& /*pos*/, const Coord3D& /*logical*/) const {}

    // True iff this modifier does per-frame work (overrides modifyLive). The Layer
    // sums this across enabled modifiers at build time to gate the per-frame pass;
    // a modifier that animates returns true so the seam runs only when needed.
    virtual bool hasModifyLive() const { return false; }

    // A modifier whose mapping changes on a timer (RandomMap reshuffles on a beat)
    // sets a flag in its loop(); the Layer polls this once per frame across all its
    // enabled modifiers and rebuilds the mapping ONCE if any returns true — so
    // several dynamic modifiers ticking together coalesce to a single rebuild rather
    // than each re-entering onBuildState(). Returns true at most once per change,
    // clearing the flag. Default false: a static modifier never asks for a rebuild.
    virtual bool consumeNeedsRebuild() { return false; }
};

} // namespace mm
