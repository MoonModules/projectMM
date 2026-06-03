// @module MoonModule
// @also GridLayout, MirrorModifier, NoiseEffect, Drivers

// Pins the selective control-change rebuild gate. `handleSetControl` rebuilds
// the pipeline only when `controlChangeTriggersBuildState()` returns true.
// Layout + Modifier opt in (their controls reshape physical dims / LUT shape);
// Effects and Drivers opt out (their controls are values read in the hot path).
// Regression target: a change here that made effect controls rebuild would
// re-introduce the slider stutter we just fixed.

#include "doctest.h"
#include "light/drivers/Drivers.h"
#include "light/layouts/GridLayout.h"
#include "light/modifiers/MirrorModifier.h"
#include "light/effects/NoiseEffect.h"

// Layout and Modifier modules opt in to rebuild on a control change (their controls reshape the pipeline).
TEST_CASE("controlChangeTriggersBuildState: Layout and Modifier opt in") {
    mm::GridLayout layout;
    mm::MirrorModifier modifier;
    CHECK(layout.controlChangeTriggersBuildState("width"));
    CHECK(modifier.controlChangeTriggersBuildState("mirrorX"));
}

// Effects and Drivers opt out — their controls are values read directly in the hot path, no rebuild needed (prevents slider stutter).
TEST_CASE("controlChangeTriggersBuildState: effects and Drivers do NOT rebuild") {
    mm::NoiseEffect effect;
    mm::Drivers drivers;
    CHECK_FALSE(effect.controlChangeTriggersBuildState("scale"));
    CHECK_FALSE(drivers.controlChangeTriggersBuildState("brightness"));
}
