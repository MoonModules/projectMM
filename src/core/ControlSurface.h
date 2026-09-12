#pragma once

// A control surface: a transport that MIRRORS ControlModule's surface state, in both directions.
//
// ControlModule owns the state (switch1..8, encoder1..8, fader1..8, and what each one drives); a
// surface is a view of it. That is what lets two attach at once, a phone running Open Stage Control
// and a desk on the rack, and stay in step: each mirrors one state rather than syncing with the
// other. It is also why inbound is not a method here. A surface writes through the same control
// path the HTTP API and the UI use, so it gains no privilege and there is no second copy of state.
//
// FEEDBACK IS NOT AN ECHO. On a motorised desk the motors move only because the host sends
// positions back; on an Akai APC the pads have no meaning at all until the host lights them. So the
// surface is often the OUTPUT device, and this interface is how it gets driven.
//
// Four verbs, because the hardware genuinely has four kinds of feedback and one would force every
// transport to pretend:
//
//   sendValue   a position: a motor, an Open Stage Control widget, an LED bar
//   sendRing    a position AND what the ring draws; the APC40 mk2 (CC 0x18/0x38) and the X-Touch
//               MINI (CC 1-8) both carry the style on a separate CC, with near-identical enums
//   sendColor   a color, for an RGB grid; per-vendor SysEx with no shared structure
//   sendLabel   text, where the transport has somewhere to put it (a scribble strip)
//
// Everything past sendValue has a default, so a transport implements what its hardware can actually
// do: OSC overrides one method, a GPIO surface lights an LED, an APC transport overrides the color
// verbs. See docs/work/past/plans/Plan-20260830 - Two-way control surfaces.md.
//
// Only sendValue has an implementer today, OSC being the only transport that exists, and the three
// others were reviewed as speculative on that basis. Kept deliberately: the shapes come from the
// hardware, not from a guess, and a default each means a transport that cannot do one writes no
// code for it. The alternative is rediscovering the same four when the MIDI transport lands.
// Author: projectMM original

#include <cstdint>

namespace mm {

/// Which bank a surface control belongs to. The names match ControlModule's own controls
/// (`switch1`, `encoder1`, `fader1`), so an index plus a kind names exactly one control.
enum class SurfaceControl : uint8_t { Switch, Encoder, Fader, Pad };

/// What an LED ring should DRAW, not just where it points. A ring showing a pan control fills from
/// the center; a volume ring fills from one end. Both the APC40 mk2 and the X-Touch MINI carry this
/// on a separate CC from the value, which is why it is a separate argument here rather than folded
/// into one number.
enum class RingStyle : uint8_t { Single, Volume, Pan, Fan, Off };

/// One attached surface. Implemented by a transport (OscModule today; a MIDI or GPIO module later),
/// registered with ControlModule, and called when the state it mirrors changes.
class ControlSurface {
public:
    virtual ~ControlSurface() = default;

    /// A control's position changed. `value` is 0..255, the unit every surface control uses, and
    /// the transport scales it to whatever its wire wants (0..1 for OSC, 14-bit for an MCU fader).
    virtual void sendValue(SurfaceControl kind, uint8_t index, uint8_t value) = 0;

    /// An encoder's ring: where it points and what it draws. Defaults to the position alone, so a
    /// transport without rings needs no code and one with rings cannot forget the style.
    virtual void sendRing(uint8_t index, uint8_t value, RingStyle style) {
        (void)style;
        sendValue(SurfaceControl::Encoder, index, value);
    }

    /// A pad's color. Does nothing by default: most transports have no color at all.
    virtual void sendColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
        (void)index; (void)r; (void)g; (void)b;
    }

    /// Several colors at once, `rgb` being `count` RGB triples. The default loops sendColor; a
    /// transport with a bulk message overrides it, which is the difference between one frame and
    /// 192 bytes of per-pad messages that would choke a USB-MIDI link.
    virtual void sendColors(const uint8_t* rgb, uint8_t count) {
        if (!rgb) return;
        for (uint8_t i = 0; i < count; i++)
            sendColor(i, rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    }

    /// A name for a control, where the transport can show one. Does nothing by default.
    virtual void sendLabel(uint8_t index, const char* text) { (void)index; (void)text; }
};

}  // namespace mm
