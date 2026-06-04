#pragma once

// BoardModule — owns the physical-hardware identity key (e.g.
// "olimex-esp32-gateway-rev-g"). The device cannot self-identify the
// board, so the key is INJECTED by MoonDeck (HTTP /api/control), the web
// installer (Improv RPC SET_BOARD), or future Step-4+ injectors.
//
// Two write paths today:
//   1. HTTP /api/control — Text control auto-persisted by FilesystemModule.
//      Used by MoonDeck and any other LAN client.
//   2. setBoard() — direct setter called by ImprovProvisioningModule when
//      the vendor RPC SET_BOARD (0xFE) arrives over Improv Serial. Used by
//      the web installer (the only client speaking Improv to us). Mirrors
//      NetworkModule::setWifiCredentials' shape: copy + markDirty + noteDirty,
//      so FilesystemModule's standard debounced save still owns persistence.
//
// The setBoard path bypasses the `readonly` UI flag on the `board` control
// intentionally — that flag is a UI-rendering hint, not a write gate, same
// as MoonDeck's HTTP write. Both injection paths produce identical persisted
// state.
//
// Lives as a code-wired child of SystemModule, mirroring how Improv lives
// under NetworkModule. Today this is a single-field module; future commits
// add per-board pin maps + default module-config overrides as additional
// controls (see docs/plan.md § Runtime board presets).

#include "core/MoonModule.h"
#include "core/FilesystemModule.h"

#include <cstring>

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

    // External setter for transports that bypass /api/control (today: Improv
    // vendor RPC SET_BOARD from the web installer). Validates: 1..31 chars,
    // ASCII-printable (0x20–0x7E), no embedded NUL. The ASCII-printable
    // floor protects every downstream consumer: JSON serialization (no
    // escaping required for the wire + persistence layer), the device UI
    // (rendered verbatim — control characters would corrupt the rendering),
    // and C-string handling everywhere else (no embedded NUL means strlen
    // / strcpy round-trip cleanly). Returns false on rejection so the
    // Improv handler can map to ErrorState. On accept: copies into
    // boardKey_ and arms FilesystemModule's debounced save — same idiom as
    // NetworkModule::setWifiCredentials.
    bool setBoard(const char* value) {
        if (!value) return false;
        size_t n = std::strlen(value);
        if (n == 0 || n >= sizeof(boardKey_)) return false;
        for (size_t i = 0; i < n; i++) {
            unsigned char b = static_cast<unsigned char>(value[i]);
            if (b < 0x20 || b > 0x7E) return false;
        }
        std::strncpy(boardKey_, value, sizeof(boardKey_) - 1);
        boardKey_[sizeof(boardKey_) - 1] = 0;
        markDirty();
        FilesystemModule::noteDirty();
        return true;
    }

private:
    // 32-byte buffer fits the longest catalog entries with modest headroom.
    // `Olimex ESP32-Gateway Rev G` (26 chars) is the longest current entry
    // and visibly truncated at the original 24-byte size. Cost: 8 extra
    // bytes of DRAM per device vs the original 24. The Improv RPC handler
    // caps `str_len` against this size dynamically (g_improv.boardOutLen
    // mirrors sizeof(boardKey_)), so the wire spec adapts automatically.
    char boardKey_[32] = {};
};

} // namespace mm
