#pragma once

#include "light/effects/EffectBase.h"
#include "core/moonlive/MoonLive.h"
#include <cstring>

// MoonLiveEffect — a scripted effect rendered by the MoonLive engine (§3.3 of
// livescripts-analysis-top-down.md). The thin binding side of the engine/binding seam: it
// IS a first-class EffectBase (role, controls, lifecycle, generic UI), and its loop()
// delegates to a compiled MoonLive over this effect's own buffer.
//
// Stage 1a: no script source yet — the engine compiles one fixed routine (fill the layer
// a colour) so the load-bearing path (emit → allocExec → call → write the buffer) is proven
// end to end on real Xtensa. The `source` text control, the language, and script-declared
// controls arrive in later stages; this is the smallest thing that lights an LED via native
// code we generated.

namespace mm {

class MoonLiveEffect : public EffectBase {
public:
    const char* tags() const override { return "📝"; }   // scripted
    Dim dimensions() const override { return Dim::D2; }

    // Stage 2: the effect carries its SCRIPT SOURCE as an editable, persisted text control.
    // Editing it recompiles live (the script-editor loop), the same way any control edit
    // reshapes a compiled module — no bespoke path. The default is the fill program the
    // spike proves end to end.
    void onBuildControls() override {
        controls_.addText("source", source_, sizeof(source_));
    }

    // A `source` edit must recompile — route it through the onBuildState rebuild sweep so a
    // new script swaps in live (the script-editor loop). Without this the edit would change
    // only the stored text, not the running code.
    bool controlChangeTriggersBuildState(const char* controlName) const override {
        return std::strcmp(controlName, "source") == 0;
    }

    // Compile the source on the cold rebuild path. A failed compile (parse error or no exec
    // memory) surfaces in the module status and leaves loop() a no-op — the effect renders
    // dark, the device keeps running (robustness + no-reboot). A *source* edit re-enters
    // here and recompiles, so a new script swaps in live; a broken edit just shows its
    // diagnostic and the layer goes dark until it's fixed.
    void onBuildState() override {
        if (engine_.compile(source_)) {
            clearStatus();
        } else {
            setStatus(engine_.error(), Severity::Error);
        }
        EffectBase::onBuildState();
    }

    void loop() override {
        if (engine_.ok()) engine_.run(buffer(), nrOfLights(), channelsPerLight(), elapsed());
    }

    void teardown() override {
        engine_.free();   // release the exec block — the destructor role
        EffectBase::teardown();
    }

private:
    moonlive::MoonLive engine_;
    char source_[64] = "fill(0, 0, 255);";   // default script — solid blue, from parsed source
};

}  // namespace mm
