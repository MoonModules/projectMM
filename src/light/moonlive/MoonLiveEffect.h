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

    // The effect carries its SCRIPT SOURCE as an editable, persisted text control, plus a control
    // for every variable the script DECLARED (`uint8_t speed = 50; // @control 0..99`). The
    // engine exposes the declared list after a compile; each becomes a real uint8 control bound by
    // reference to the engine's live control-arena slot, so a slider write lands in the slot the
    // next render tick reads — no recompile (the live-edit guarantee). Editing the source
    // recompiles (the script-editor loop), which re-derives the control set.
    void onBuildControls() override {
        controls_.addTextArea("source", source_, sizeof(source_));
        uint8_t n = 0;
        const moonlive::DeclaredControl* decls = engine_.declaredControls(n);
        for (uint8_t i = 0; i < n; i++) {
            uint8_t* slot = engine_.controlSlot(decls[i].offset);
            if (!slot) continue;   // engine not compiled yet (first sweep) — controls appear after onBuildState
            // The declared name is a span into source_ (not NUL-terminated); copy it into a stable
            // member pool so the control descriptor's borrowed name pointer stays valid.
            uint8_t len = decls[i].nameLen < kMaxName - 1 ? decls[i].nameLen : kMaxName - 1;
            std::memcpy(ctrlNames_[i], decls[i].name, len);
            ctrlNames_[i][len] = '\0';
            controls_.addUint8(ctrlNames_[i], *slot,
                               static_cast<uint8_t>(decls[i].min), static_cast<uint8_t>(decls[i].max));
        }
    }

    // A `source` edit must recompile — route it through the onBuildState rebuild sweep so a new
    // script swaps in live (the script-editor loop). A SCRIPTED CONTROL's value change must NOT
    // recompile: it just updates an arena byte the running native code reads next tick. So only
    // "source" triggers a rebuild; every scripted control returns false (the live-edit path).
    bool controlChangeTriggersBuildState(const char* controlName) const override {
        return std::strcmp(controlName, "source") == 0;
    }

    // Compile the source on the cold rebuild path. A failed compile (parse error or no exec
    // memory) surfaces in the module status and leaves loop() a no-op — the effect renders
    // dark, the device keeps running (robustness + no-reboot). A *source* edit re-enters here and
    // recompiles, so a new script swaps in live; a broken edit just shows its diagnostic.
    void onBuildState() override {
        if (engine_.compile(source_, moonlive::lightBuiltins())) {
            clearStatus();
        } else {
            setStatus(engine_.error(), Severity::Error);
        }
        // The compile re-derives the declared-control set, so rebuild the control list to surface
        // it (the same rebuildControls() pattern NetworkModule uses when a state change reshapes
        // its controls). Each scripted control re-binds to its (stable-address) arena slot.
        rebuildControls();
        // Report the exec block as the module's heap use (codeCap, the word-rounded allocation),
        // so the UI card's "+ dynamic" reflects the JIT'd program — 0 when the compile failed.
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
    static constexpr uint8_t kMaxName = 16;   // longest scripted control name (NUL-terminated)
    moonlive::MoonLive engine_;
    char source_[128] = "fill(0, 0, 255);";   // default script — solid blue, from parsed source
    // Stable NUL-terminated copies of the script-declared control names (the control descriptor
    // borrows the pointer; the decl span into source_ is not NUL-terminated).
    char ctrlNames_[moonlive::kMaxCtrls][kMaxName] = {};
};

}  // namespace mm
