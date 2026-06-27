#pragma once

#include "light/effects/EffectBase.h"
#include "core/moonlive/MoonLive.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include <cstring>

// MoonLiveEffect — a scripted effect rendered by the MoonLive engine (§3.3 of
// livescripts-analysis-top-down.md). The thin binding side of the engine/binding seam: it
// IS a first-class EffectBase (role, controls, lifecycle, generic UI), and its loop()
// delegates to a compiled MoonLive over this effect's own buffer.
//
// The effect holds a `source` text control; onBuildState compiles it through the engine and
// loop() runs the emitted native code over the buffer (emit → allocExec → call → write). A
// source edit recompiles live; a parse error shows in the module status and the layer goes
// dark — robust, no reboot.

namespace mm {

class MoonLiveEffect : public EffectBase {
public:
    const char* tags() const override { return "📝"; }   // scripted
    Dim dimensions() const override { return Dim::D2; }

    // The effect carries its SCRIPT SOURCE as an editable, persisted text control. Editing it
    // recompiles live (the script-editor loop), the same way any control edit reshapes a
    // compiled module — no bespoke path. The default is a solid-blue fill program.
    void onBuildControls() override {
        controls_.addTextArea("source", source_, sizeof(source_));
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
        if (engine_.compile(source_, moonlive::lightBuiltins())) {
            clearStatus();
        } else {
            setStatus(engine_.error(), Severity::Error);
        }
        // Report the exec block as the module's heap use (the actual allocated, word-rounded
        // size — codeCap, not the raw emitted length), so the UI card's "+ dynamic" reflects the
        // JIT'd program — 0 when the compile failed and freed it.
        setDynamicBytes(engine_.ok() ? engine_.codeCap() : 0);
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
    char source_[128] = "fill(0, 0, 255);";   // default script — solid blue, from parsed source
};

}  // namespace mm
