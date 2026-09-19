#pragma once

#include "core/module/MoonModule.h"
#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveBuiltins_service.h"
#include "light/moonlive/MoonLiveScript.h"        // the control seam it drives holds a draw::Canvas

#include <cstring>

namespace mm {

/// A scripted service: the input twin of a scripted effect, and the flexible half of input.
///
/// The compiled services are lists of mappings, wrong for anything with a condition in it.
/// A script holds the edge state a row cannot, and decides between outcomes.
/// So a sensor nobody wrote a module for needs a datasheet and eight lines, not a release.
/// @card MoonLiveService.png
///
/// @moreinfo
///
/// ## The same relationship effects already have
///
/// A compiled effect and a scripted one are interchangeable, and so are the input services.
/// The engine is identical, so a script author already knows the language.
/// Factory-registered like the others: added under the container, then pointed at a file.
///
/// ## It runs on the slow tick
///
/// A contact closes for tens of milliseconds and a sensor answers at its own rate.
/// The render rate would sample thousands of times to learn the same thing.
/// It also means a heavy script costs its own tick rather than stuttering the lights.
///
/// What it reaches: the pins for hardware, and the control surface for output.
/// That is the two-step model the mapping rows use, so a script drives the surface alone.
/// A script therefore cannot rewrite a driver's pins by naming it.
class MoonLiveService : public MoonModule {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Declare the script's name, then whatever controls the script itself declared.
    void defineControls() override {
        // The name, not the text: that lives in a file, so a module costs bytes not a kilobyte.
        controls_.addFilePath("script", script_.buffer(), script_.bufferSize(),
                              moonlive::kServicePick);
        // Each bound to its live slot, so a slider move lands with no recompile.
        script_.publishDeclaredControls(controls_);
        MoonModule::defineControls();
    }

    /// Only naming a different script recompiles, a value change updating a byte the tick reads.
    bool affectsPrepare(const char* controlName) const override {
        return std::strcmp(controlName, "script") == 0;
    }

    /// Compile the script where the file has changed, then surface what it declares.
    void prepare() override {
        // A hash answers whether what is compiled still matches the file.
        script_.sync(moonlive::serviceSysVars(), *this, moonlive::serviceBuiltins());
        // The compile re-derives the declared set, so rebuild the list to surface it.
        rebuildControls();
    }

    /// The service moment, where a press and a sensor reading both live.
    void tick20ms() MM_NONBLOCKING override {
        if (!script_.ok()) return;
        if (!script_.engine().hasEntry(moonlive::kEntryTick20ms)) return;
        // Not the painting entry, which refuses a service for having no light buffer.
        script_.engine().runValue(moonlive::kEntryTick20ms, moonlive::RetType::Void, 0,
                                  nullptr, 0, 0, platform::millis());
    }

    /// Free the compiled block and forget it, so re-enabling rebuilds from the file.
    void release() override {
        script_.engine().free();      // release the exec block
        script_.invalidate();         // and forget what was compiled, so re-enabling rebuilds it
        script_.releaseReporting(*this);
        MoonModule::release();
    }

    /// Point the module at a script, which the next build compiles, as a UI edit would.
    void setScript(const char* name) { script_.setName(name); }

private:
    moonlive::MoonLiveScript script_;
};

}  // namespace mm
