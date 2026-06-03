#pragma once

// BoardModule — owns the physical-hardware identity key (e.g.
// "olimex-esp32-gateway-rev-g"). The device cannot self-identify the
// board, so the key is INJECTED by MoonDeck (today) and the web installer
// (Step 2 of the multi-commit board-injection plan).
//
// Injection uses the standard POST /api/control route — `board` is a Text
// control, so the existing control-write path copies into the buffer,
// triggers FilesystemModule's debounced save to /.config/BoardModule.json,
// and the value survives reboots. No bespoke setter, no bespoke route, no
// bespoke persistence — the framework already does all of it.
//
// The "UI shouldn't edit board" property is convention, not enforcement.
// If a user does edit it manually, MoonDeck's next discover pushes the
// catalog value back, so it's self-healing. (Future Step 3 — Improv RPC
// injection — uses the same control-write path on the device side, just
// invoked from the Improv command dispatcher instead of HTTP.)
//
// Lives as a code-wired child of SystemModule, mirroring how Improv lives
// under NetworkModule. Today this is a single-field module; future commits
// add per-board pin maps + default module-config overrides as additional
// controls (see docs/plan.md § Runtime board presets).

#include "core/MoonModule.h"

namespace mm {

class BoardModule : public MoonModule {
public:
    // Identity-class data: belongs visible even when the module is "disabled"
    // (matches SystemModule's same call — diagnostics shouldn't vanish).
    bool respectsEnabled() const override { return false; }

    void onBuildControls() override {
        controls_.addText("board", boardKey_, sizeof(boardKey_));
        // Mark display-only in the UI — `board` is pushed by tooling
        // (MoonDeck, web installer), never edited by the user at the device.
        // Bound as Text (not ReadOnly) because Text is auto-persisted by
        // FilesystemModule; the readonly flag is a separate UI-only hint
        // that doesn't change the persistence or HTTP-write semantics.
        controls_.setReadOnly(controls_.count() - 1, true);
    }

    const char* board() const { return boardKey_; }

private:
    char boardKey_[24] = {};
};

} // namespace mm
