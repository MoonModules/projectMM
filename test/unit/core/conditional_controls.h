// Shared helpers for testing CONDITIONAL CONTROLS — controls whose UI visibility
// (the `hidden` flag) depends on another control's value. Used by every module
// that has them (NetworkModule's static-IP fields, RmtLedDriver's loopbackRxPin).
//
// The invariant these helpers pin (see docs/architecture.md § Conditional controls):
//   1. A conditional control is ALWAYS bound (present in the control list) so
//      persistence can load its value regardless of the live conditional state.
//   2. Its `hidden` flag correctly reflects the conditioning control's value.
//   3. rebuildControls() re-evaluates the flag — the mechanism the UI relies on
//      to reveal/hide a control live (HttpServerModule rebuilds after every change).
//
// Two real bugs motivated these tests: RmtLedDriver's rxPin not showing when the
// test was enabled, and a UI re-render loop when NetworkModule's static fields
// toggled. The host-side half (binding + flag) is what a unit test can prove.

#pragma once

#include "doctest.h"
#include "core/MoonModule.h"
#include <cstring>
#include <cstdint>

namespace mm::test {

// Index of a control by name in a module's current control list, or -1 if absent
// (not bound at all). Use to assert a conditional control stays bound.
inline int controlIndex(mm::MoonModule& m, const char* name) {
    const auto& cs = m.controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, name) == 0) return static_cast<int>(i);
    }
    return -1;
}

// True if `name` is bound AND not hidden (i.e. the UI would render it).
inline bool isVisible(mm::MoonModule& m, const char* name) {
    int i = controlIndex(m, name);
    return i >= 0 && !m.controls()[static_cast<uint8_t>(i)].hidden;
}

// True if `name` is bound but hidden.
inline bool isHidden(mm::MoonModule& m, const char* name) {
    int i = controlIndex(m, name);
    return i >= 0 && m.controls()[static_cast<uint8_t>(i)].hidden;
}

// Write a value through a control's bound variable pointer, by name. Lets a test
// flip a conditioning control (e.g. NetworkModule's addressing) without a public
// setter — the same pointer the persistence/API paths write. Returns false if the
// control isn't bound. Caller must pass the matching integer width for the type.
template <typename T>
inline bool setControlValue(mm::MoonModule& m, const char* name, T value) {
    int i = controlIndex(m, name);
    if (i < 0) return false;
    void* p = m.controls()[static_cast<uint8_t>(i)].ptr;
    if (!p) return false;
    *static_cast<T*>(p) = value;
    return true;
}

// Assert a conditional control obeys the full invariant across both states of its
// condition. `setCondition` flips the conditioning value to the given bool; the
// test toggles it both ways, rebuilds, and checks the dependent control is
// always bound and only visible when expected.
//
//   visibleWhenTrue: does the dependent control show when the condition is true?
template <typename SetCondition>
inline void checkConditionalControl(mm::MoonModule& m, const char* dependent,
                                    SetCondition setCondition, bool visibleWhenTrue) {
    // Condition TRUE → dependent visible (or hidden, per visibleWhenTrue), still bound.
    setCondition(true);
    m.rebuildControls();
    CHECK_MESSAGE(controlIndex(m, dependent) >= 0,
                  "conditional control must stay BOUND even when its condition is true: ", dependent);
    CHECK(isVisible(m, dependent) == visibleWhenTrue);

    // Condition FALSE → visibility flips, STILL bound (persistence can load it).
    setCondition(false);
    m.rebuildControls();
    CHECK_MESSAGE(controlIndex(m, dependent) >= 0,
                  "conditional control must stay BOUND even when hidden (persistence needs it): ", dependent);
    CHECK(isVisible(m, dependent) == !visibleWhenTrue);
}

} // namespace mm::test
