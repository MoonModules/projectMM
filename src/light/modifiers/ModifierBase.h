#pragma once

#include "core/MoonModule.h"
#include "light/light_types.h" // lengthType, nrOfLightsType, Dim

namespace mm {

class ModifierBase : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Modifier; }
    virtual bool isStatic() const { return true; }

    // A modifier control change alters the LUT shape, so the owning Layer must rebuild
    // its LUT — the pipeline-wide rebuild path. See MoonModule::onUpdate. (Even a
    // future dynamic modifier is harmless to rebuild on a control change.)
    bool controlChangeTriggersBuildState(const char* /*controlName*/) const override { return true; }

    // Which axes the modifier can transform. Defaults to D3 — a modifier that
    // touches the LUT is assumed to work in 3D unless it declares otherwise.
    // The UI uses this to render the 📏/🟦/🧊 chip so the user can see at a
    // glance whether a modifier will do anything along z. Distinct from
    // EffectBase::dimensions() (which controls Layer extrusion); here it is
    // purely an advisory chip, never read in the render path.
    virtual Dim dimensions() const { return Dim::D3; }

    // Max physical destinations per logical light (for LUT allocation estimate)
    virtual uint8_t maxMultiplier() const { return 8; }

    // Compute logical dimensions given physical dimensions
    virtual void logicalDimensions(lengthType physW, lengthType physH, lengthType physD,
                                   lengthType& logW, lengthType& logH, lengthType& logD) const = 0;

    // Map a logical coordinate to physical positions
    // outPhysicals: caller-provided array (stack, max 8 for XYZ mirror)
    // outCount: set to number of physical positions written
    virtual void mapToPhysical(lengthType lx, lengthType ly, lengthType lz,
                               lengthType physW, lengthType physH, lengthType physD,
                               nrOfLightsType* outPhysicals, nrOfLightsType& outCount,
                               nrOfLightsType maxOut) const = 0;
};

} // namespace mm
