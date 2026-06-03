// @module BoardModule

// Unit tests for BoardModule's control wiring. The module is intentionally
// minimal — the framework (FilesystemModule + /api/control) handles
// injection and persistence, so the only thing to verify on the module
// itself is that the `board` control is bound correctly.
//
// MoonDeck (today) and the web installer (Step 2) push values via the
// existing POST /api/control route; that path is exercised by the
// HttpServerModule scenario tests when a control-write happens. There's
// nothing module-specific to test on that side.

#include "doctest.h"
#include "core/BoardModule.h"

#include <cstring>

// After onBuildControls, BoardModule exposes exactly one `board` control,
// bound as Text to a 24-byte buffer.
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
    // Buffer is 24 bytes (max 23 chars + NUL); the control's max field
    // carries the buffer size for the persistence + UI layers.
    CHECK(c.max == 24);
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
