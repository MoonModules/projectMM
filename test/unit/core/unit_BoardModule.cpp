// @module BoardModule

// Unit tests for BoardModule's control wiring + setBoard() validation.
//
// The module is intentionally minimal. The framework (FilesystemModule +
// /api/control) handles injection from MoonDeck (Step 1). The Improv vendor
// RPC injector (Step 3) calls setBoard() directly — those validation
// boundaries are tested here; the platform task that decodes the RPC
// payload uses ESP-IDF UART APIs and isn't host-testable, so the
// scripts/build/improv_smoke_test.py integration test covers the wire side.

#include "doctest.h"
#include "core/BoardModule.h"

#include <cstring>

// After onBuildControls, BoardModule exposes exactly one `board` control,
// bound as Text to a 32-byte buffer.
TEST_CASE("BoardModule binds `board` as a Text control") {
    mm::BoardModule board;
    board.onBuildControls();

    REQUIRE(board.controls().count() == 1);
    const auto& c = board.controls()[0];
    CHECK(std::strcmp(c.name, "board") == 0);
    CHECK(c.type == mm::ControlType::Text);
    // The control's ptr points at the module's internal boardKey_ buffer;
    // accessor returns the same address so we can confirm wiring.
    CHECK(static_cast<const char*>(c.ptr) == board.board());
    // Buffer is 32 bytes (max 31 chars + NUL); the control's max field
    // carries the buffer size for the persistence + UI layers.
    CHECK(c.max == 32);
    // UI-readonly hint: BoardModule pushes the value via MoonDeck / Improv
    // SET_BOARD, never via direct UI edit. Regressions that flip this back
    // to editable would silently restore the typing-into-the-UI failure
    // mode the readonly flag exists to prevent.
    CHECK(c.readonly == true);
}

// Default state is the empty string — MoonDeck pushes a value on first reach.
TEST_CASE("BoardModule starts empty") {
    mm::BoardModule board;
    board.onBuildControls();
    CHECK(board.board()[0] == '\0');
}

// respectsEnabled() returns false so the `board` value stays visible even
// when the module is disabled — identity-class data shouldn't vanish.
TEST_CASE("BoardModule ignores the enabled flag") {
    mm::BoardModule board;
    CHECK_FALSE(board.respectsEnabled());
}

// setBoard happy path: valid value lands in the buffer + marks dirty so
// FilesystemModule's debounced save picks it up. Mirrors the shape of
// NetworkModule::setWifiCredentials' unit-test pattern.
TEST_CASE("BoardModule::setBoard accepts valid value and marks dirty") {
    mm::BoardModule board;
    board.onBuildControls();
    CHECK_FALSE(board.dirty());

    CHECK(board.setBoard("LOLIN D32"));
    CHECK(std::strcmp(board.board(), "LOLIN D32") == 0);
    CHECK(board.dirty());

    // Re-set with a different value to confirm the copy happens (not just
    // a no-op return). clearDirty between writes so we see the second
    // dirty assertion as a real signal.
    board.clearDirty();
    CHECK(board.setBoard("Generic ESP32 DevKit"));
    CHECK(std::strcmp(board.board(), "Generic ESP32 DevKit") == 0);
    CHECK(board.dirty());
}

// Empty string is rejected — no buffer write, no dirty flag.
TEST_CASE("BoardModule::setBoard rejects empty string") {
    mm::BoardModule board;
    board.onBuildControls();
    CHECK_FALSE(board.setBoard(""));
    CHECK(board.board()[0] == '\0');
    CHECK_FALSE(board.dirty());
}

// 32+ char value is rejected (buffer is 32 bytes including NUL, so 31 max).
TEST_CASE("BoardModule::setBoard rejects over-length value") {
    mm::BoardModule board;
    board.onBuildControls();
    // 32-char value (would need 33 bytes with NUL — exceeds buffer).
    CHECK_FALSE(board.setBoard("12345678901234567890123456789012"));
    CHECK(board.board()[0] == '\0');
    CHECK_FALSE(board.dirty());

    // 31-char value (max valid) accepted as the boundary check.
    CHECK(board.setBoard("1234567890123456789012345678901"));
    CHECK(std::strlen(board.board()) == 31);
}

// Non-printable bytes are rejected. Catches accidental binary smuggling
// (would also break the persistence JSON encoder).
TEST_CASE("BoardModule::setBoard rejects non-printable bytes") {
    mm::BoardModule board;
    board.onBuildControls();
    // Embedded control character (0x01).
    char bad[8] = {'g', 'o', 'o', 'd', 0x01, 'b', 'a', 0};
    CHECK_FALSE(board.setBoard(bad));
    CHECK(board.board()[0] == '\0');
    CHECK_FALSE(board.dirty());

    // High-byte (0x80 — outside ASCII-printable 0x20-0x7E).
    char hi[4] = {'a', static_cast<char>(0x80), 'b', 0};
    CHECK_FALSE(board.setBoard(hi));
    CHECK_FALSE(board.dirty());
}

// nullptr is rejected (defensive — a bogus caller shouldn't crash the device).
TEST_CASE("BoardModule::setBoard rejects nullptr") {
    mm::BoardModule board;
    board.onBuildControls();
    CHECK_FALSE(board.setBoard(nullptr));
    CHECK_FALSE(board.dirty());
}
