#pragma once

#include "core/MoonModule.h"
#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveBuiltins_service.h"
#include "light/moonlive/MoonLiveScript.h"        // the file/compile/status half, domain-neutral

#include <cstring>

namespace mm {

/// A scripted SERVICE: the input twin of `MoonLiveEffect`, and the flexible half of this device's
/// input story.
///
/// **Why a script and not another module.** `ButtonService` and `InfraredService` are lists of
/// mappings, which is the right shape for "this pin drives that control" and the wrong one for
/// anything with a condition in it. A list row cannot say "when the distance drops under 50 cm",
/// cannot hold the edge state that stops it firing every tick, and cannot decide between two
/// presets. A script can, and it is how a sensor nobody wrote a module for gets supported: with a
/// datasheet and eight lines, rather than a firmware release.
///
/// **The same relationship effects already have.** A compiled effect and a scripted one are
/// interchangeable; so are a compiled input service and a scripted one. `ButtonService` is the fast,
/// shipped path for the buttons a board is built with, and this is the path for everything else. The
/// engine is identical, so a script author already knows the language.
///
/// **It runs on tick20ms, not on the render tick.** A contact closes for tens of milliseconds and a
/// sensor answers at its own rate, so the render rate would sample either thousands of times a
/// second to learn the same thing. It also means a heavy script costs its own tick rather than
/// stuttering the lights at the frame rate.
///
/// **What it can reach.** `gpioRead` / `gpioWrite` for the hardware, and `setControl` for the
/// output, which writes the CONTROL SURFACE and nothing else. That is the same two-step model the
/// mapping rows use: a script drives the surface, the surface drives everything. One path to a
/// driver rather than two, and a script cannot rewrite a driver's pin list by naming it.
///
/// **Not auto-wired.** Factory-registered like the other services: added under `Services`, then
/// pointed at a `.mls` file.
/// @card MoonLiveService.png
class MoonLiveService : public MoonModule {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    void defineControls() override {
        // The script NAME, not the script: the text lives in a file the UI edits through /api/file,
        // so a module costs ~32 bytes rather than a resident kilobyte. Same as every other binding.
        controls_.addFilePath("script", script_.buffer(), script_.bufferSize(),
                              moonlive::kServicePick);
        // Every control the script declared, bound to its live arena slot: a slider move lands in
        // the byte the next tick reads, with no recompile.
        script_.publishDeclaredControls(controls_);
        MoonModule::defineControls();
    }

    /// Naming a different script recompiles; a scripted control's value change must NOT, because it
    /// only updates an arena byte the running code reads next tick.
    bool affectsPrepare(const char* controlName) const override {
        return std::strcmp(controlName, "script") == 0;
    }

    void prepare() override {
        // sync() answers "is what is compiled still what the file says" from a hash, so an unchanged
        // script costs a read rather than a re-JIT. It reports status and dynamic bytes itself.
        script_.sync(moonlive::serviceSysVars(), *this, moonlive::serviceBuiltins());
        // The compile re-derives the declared-control set, so rebuild the list to surface it.
        rebuildControls();
    }

    /// The service moment: 50 Hz, where a press and a sensor reading both live.
    void tick20ms() MM_NONBLOCKING override {
        if (!script_.ok()) return;
        if (!script_.engine().hasEntry(moonlive::kEntryTick20ms)) return;
        // runValue, not run(): run() refuses a call with no light buffer (`!buf || nLights == 0 ||
        // cpl < 3`), which is the right precondition for an effect and fatal for a service, because
        // a service paints nothing by definition. It was a SILENT refusal, so the script compiled,
        // reported its size, and never executed a line. runValue calls the same emitted block with
        // the same prologue and asks only for the arena, which is what a service actually needs.
        //
        // `t` (kArg3) carries the elapsed milliseconds a script reads for timing.
        script_.engine().runValue(moonlive::kEntryTick20ms, moonlive::RetType::Void, 0,
                                  nullptr, 0, 0, platform::millis());
    }

    void release() override {
        script_.engine().free();      // release the exec block: the destructor role
        script_.invalidate();         // and forget what was compiled, so re-enabling rebuilds it
        script_.releaseReporting(*this);
        MoonModule::release();
    }

    /// Point the module at a script. The next prepare() compiles it, which is the path a UI edit
    /// takes too, so a test and a user exercise identical code.
    void setScript(const char* name) { script_.setName(name); }

private:
    moonlive::MoonLiveScript script_;
};

}  // namespace mm
