#include "doctest.h"
#include "light/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/CheckerboardEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/PlasmaPaletteEffect.h"
#include "light/effects/RipplesEffect.h"
#include "light/effects/GlowParticlesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/effects/FireEffect.h"
#include "light/effects/ParticlesEffect.h"

// Pin the "Effects must work at every grid size" rule. A 0-light layout is a
// real configuration — a modifier can shrink the logical grid to 0,0,0 or
// every layout child can be disabled. Effects' loop() must be a clean no-op
// in that case (no div-by-zero, no OOB writes, no crash).

namespace {

template <typename Effect>
void run_with_empty_layout() {
    mm::Layouts layouts;  // no children → totalLightCount() == 0
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    Effect e;
    layer.addChild(&e);
    layouts.onAllocateMemory();
    layer.onAllocateMemory();  // logical/physical dims all zero, no buffer
    // The real assertion is "doesn't crash" — if loop() reaches a divide-by-zero
    // or an OOB write the process dies before we get here.
    layer.loop();
    CHECK(layer.width() == 0);
    CHECK(layer.height() == 0);
    CHECK(layer.depth() == 0);
}

} // namespace

TEST_CASE("RainbowEffect on 0,0,0 grid")     { run_with_empty_layout<mm::RainbowEffect>(); }
TEST_CASE("NoiseEffect on 0,0,0 grid")       { run_with_empty_layout<mm::NoiseEffect>(); }
TEST_CASE("PlasmaEffect on 0,0,0 grid")      { run_with_empty_layout<mm::PlasmaEffect>(); }
TEST_CASE("CheckerboardEffect on 0,0,0 grid"){ run_with_empty_layout<mm::CheckerboardEffect>(); }
TEST_CASE("SpiralEffect on 0,0,0 grid")      { run_with_empty_layout<mm::SpiralEffect>(); }
TEST_CASE("MetaballsEffect on 0,0,0 grid")   { run_with_empty_layout<mm::MetaballsEffect>(); }
TEST_CASE("PlasmaPaletteEffect on 0,0,0 grid"){run_with_empty_layout<mm::PlasmaPaletteEffect>(); }
TEST_CASE("RipplesEffect on 0,0,0 grid")     { run_with_empty_layout<mm::RipplesEffect>(); }
TEST_CASE("GlowParticlesEffect on 0,0,0 grid"){run_with_empty_layout<mm::GlowParticlesEffect>(); }
TEST_CASE("LavaLampEffect on 0,0,0 grid")    { run_with_empty_layout<mm::LavaLampEffect>(); }
TEST_CASE("FireEffect on 0,0,0 grid")        { run_with_empty_layout<mm::FireEffect>(); }
TEST_CASE("ParticlesEffect on 0,0,0 grid")   { run_with_empty_layout<mm::ParticlesEffect>(); }
