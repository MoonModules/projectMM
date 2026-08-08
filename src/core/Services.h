#pragma once

#include "core/MoonModule.h"

namespace mm {

/// Top-level container for the user-added **Service** modules — capability bridges the device
/// provides or consumes (Audio, IR): optional, per-board, added and removed at runtime. It is the
/// core-domain twin of the light domain's `Effects`/`Drivers`: a top-level container holding
/// user-added children of a single role, so the generic add/replace/delete/persistence machinery
/// applies unchanged. Fixed device infrastructure (Tasks, I2cScan, Network, …) lives under
/// `System`, not here — Services is exactly the mutable half of that split.
///
/// A pure grouping node: no controls, no loop, no state of its own (the same shape as `Layouts`
/// minus its coordinate logic). Its one job is to declare which children it accepts; everything
/// else is inherited from `MoonModule`.
/// @card Services.png
class Services : public MoonModule {
public:
    const char* acceptsChildRoles() const override { return "service"; }
};

} // namespace mm
