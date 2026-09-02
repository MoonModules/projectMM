#pragma once

// OSC control ingest: turns a fader move in Resolume, TouchDesigner, TouchOSC or a DIY
// Arduino-over-Ethernet rig into a control write on this device.
//
// It owns no surface of its own. ControlModule already has the pads, encoders and faders, laid out
// to match a Mackie-style desk, and everything here lands in Scheduler::setControl, the same entry
// point the HTTP API and the UI use. So OSC gains no privilege: every validator still runs, and
// there is no second copy of the device's state to keep in step.
//
// Addresses (the public contract, so they stay small and boring):
//
//   /mm/fader/1                     f 0..1 or i 0..255  ->  ControlModule fader1
//   /mm/encoder/3                       f 0..1 or i 0..255  ->  ControlModule encoder3
//   /mm/switch/2                    f 0..1 or i 0..255  ->  ControlModule switch2 (nonzero = on)
//   /mm/hello                       (any or none)       ->  resend every value to the sender
//   /mm/control/Drivers/brightness  f 0..1 or i 0..255  ->  that module's control directly
//
// The /mm/control/ form is what makes projectMM useful to TouchDesigner on day one, without
// waiting for someone to bind a fader.
//
// NOT here: sending OSC (nothing we own consumes it: the X-Touch and QCon are Mackie desks, see
// reference/control-surfaces.md), bundles, and address wildcards. See the plan.
// Author: projectMM original

#include "core/ControlModule.h"
#include "core/ControlSurface.h"
#include "core/MoonModule.h"
#include "core/OscPacket.h"
#include "core/Scheduler.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

/// Service: receives OSC and writes it onto the device's controls.
/// @card OscModule.png
class OscModule : public MoonModule, public ControlSurface {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Off by default: this opens an unauthenticated UDP port that writes controls, so it is a
    /// capability a user turns on rather than one every device carries.
    bool enabledOsc = false;
    uint16_t port = osc::kDefaultPort;

    /// Mirror control changes back to the surface. Off by default like `listen`: it sends unasked-for
    /// UDP to whatever last talked to us, which is a capability a user turns on.
    ///
    /// It is what makes a surface a SURFACE rather than a one-way remote. Open Stage Control shows a
    /// stale widget without it, and a motorised fader cannot move at all.
    bool feedback = false;
    /// Where the CLIENT listens, which is not where we do. Open Stage Control has its own `osc-port`
    /// setting for exactly this, and sending to our own `port` would just talk to ourselves. 9001 by
    /// convention: one above the de-facto receive port.
    uint16_t feedbackPort = 9001;


    void defineControls() override {
        MoonModule::defineControls();
        controls_.addControl("listen", enabledOsc);
        controls_.addControl("port", port, 1, 65535);
        controls_.addControl("feedback", feedback);
        controls_.addText("feedbackTo", feedbackTo_, sizeof(feedbackTo_));
        controls_.addControl("feedbackPort", feedbackPort, 1, 65535);
    }

    void onControlChanged(const char* name) override {
        // A port or listen change reopens the socket: the setting applies live, no reboot
        // (architecture.md's live-reconfiguration rule).
        if (std::strcmp(name, "port") == 0 || std::strcmp(name, "listen") == 0) closeSocket();
        // Turning feedback on, or pointing it somewhere else, means the receiver knows nothing:
        // change-detection would leave it wrong until something happened to move, which on a quiet
        // rig is never. Push everything once instead.
        if (std::strcmp(name, "feedback") == 0 || std::strcmp(name, "feedbackTo") == 0
            || std::strcmp(name, "feedbackPort") == 0) {
            if (feedback) resendAll_ = true;
        }
    }

    void release() override {
        // Detach BEFORE the socket closes: ControlModule walks its surface list from the render
        // thread, and an entry pointing at a destroyed module is a use-after-free on the next tick.
        if (auto* c = ControlModule::active()) c->removeSurface(this);
        attached_ = false;
        closeSocket();
    }

    // --- ControlSurface -----------------------------------------------------------------------
    //
    // Only sendValue is implemented: OSC carries numbers. The ring, color and label verbs keep
    // their no-op defaults, which is the point of them having defaults, and a future MIDI transport
    // overrides what its hardware can actually drive.

    void sendValue(SurfaceControl kind, uint8_t index, uint8_t value) override {
        if (!feedback || !open_) return;
        const char* bank = kind == SurfaceControl::Fader   ? "fader"
                         : kind == SurfaceControl::Encoder ? "encoder"
                         : kind == SurfaceControl::Switch  ? "switch" : nullptr;
        if (!bank) return;                       // a pad has no OSC address yet (see the plan)
        char addr[32];
        std::snprintf(addr, sizeof(addr), "/mm/%s/%u", bank, static_cast<unsigned>(index) + 1u);
        uint8_t pkt[64];
        // 0..1 float, the form an OSC app expects and the inverse of what toByte() reads.
        const size_t len = osc::encodeFloat(pkt, sizeof(pkt), addr, static_cast<float>(value) / 255.0f);
        if (len == 0) return;
        uint8_t dest[4];
        if (!feedbackDest(dest)) return;
        sock_.sendToAddr(dest, feedbackPort, pkt, len);
    }

    /// The status is time-dependent (a peer goes stale), so it is refreshed on the second, not only
    /// when something arrives. tick1s rather than tick(): a status line changes at human speed.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        // Only when the answer CHANGES, which is twice per client session rather than once a second:
        // the string is identical on every tick in between, and a status line nobody is reading does
        // not need rewriting. snprintf is cheap but this runs on the render thread.
        if (!enabledOsc) return;
        const bool fresh = peerFresh();
        if (fresh != peerWasFresh_) { peerWasFresh_ = fresh; reportPeer(); }
    }

    void tick() MM_NONBLOCKING override {
        if constexpr (!platform::hasNetwork) return;
        if (!enabledOsc) { if (open_) closeSocket(); return; }
        if (!ensureSocket()) return;

        // Bounded non-blocking drain, the shape AudioService::syncReceive uses: a desk moves a
        // handful of controls per frame, so a small cap keeps a flood from owning the tick.
        uint8_t pkt[kMaxPacket];
        for (int i = 0; i < kMaxPerTick; i++) {
            uint8_t src[4] = {};
            const int n = sock_.recvFrom(pkt, sizeof(pkt), src);
            if (n <= 0) break;                       // -1 = nothing pending
            // Learn where to answer. OSC has no discovery, and a controller that just wrote to us is
            // by definition reachable, so the peer costs nothing to remember and spares the user a
            // second address to type. `feedbackTo` overrides it for a fixed receiver.
            //
            // A NEW peer is a client that just arrived, and it knows none of the current values: its
            // widgets sit at whatever its layout file said. So re-seed the whole surface. Without
            // this a client only learns a value when something happens to change it, which on a
            // quiet rig is never, and every widget lies until the user touches it.
            // A CHANGED peer is a client on a new address. This catches a move between machines and
            // a controller that never sends /mm/hello; hello catches a restart on the same address,
            // which this cannot see. Both are cheap, and neither alone is enough.
            lastRecvMs_ = platform::millis();
            if (std::memcmp(peer_, src, 4) != 0) {
                std::memcpy(peer_, src, 4);
                resendAll_ = true;
                peerWasFresh_ = false;   // a new address: let the next tick1s say so
                // REMEMBER it. feedbackTo is a persisted control, so writing the learned address
                // there is what makes a rig survive a reboot: the fallback alone forgets the client
                // on every restart and stays silent until it happens to send something, which for a
                // surface that only transmits on touch can be a long time. A user who typed an
                // address keeps it: this only fills in an empty field.
                if (!feedbackTo_[0]) {
                    std::snprintf(feedbackTo_, sizeof(feedbackTo_), "%u.%u.%u.%u",
                                  src[0], src[1], src[2], src[3]);
                    markDirty();
                    FilesystemModule::noteDirty();
                }
            }
            handle(pkt, static_cast<size_t>(n));
        }
        // After the drain, not inside it: a burst from one client reseeds once rather than per
        // packet, and the sends do not interleave with the reads.
        if (resendAll_) {
            resendAll_ = false;
            if (auto* c = ControlModule::active()) c->resendTo(this);
        }
    }

private:
    static constexpr long   kSurfaceWidth = 8;    // ControlModule's fader/encoder count
    static constexpr size_t kMaxPacket   = 256;   // an address plus a few args; controllers send far less
    static constexpr int    kMaxPerTick  = 16;
    static constexpr uint32_t kOpenRetryMs = 2000;

    /// Route one datagram. Unknown addresses are ignored rather than reported: a controller
    /// blasting its whole layout at us must not fill the log or slow the tick.
    void handle(const uint8_t* pkt, size_t len) {
        osc::Message m;
        if (!osc::parse(pkt, len, m)) return;
        const char* a = m.address;

        if (std::strncmp(a, "/mm/fader/", 10) == 0) {
            writeSurface("fader", a + 10, m);
        } else if (std::strncmp(a, "/mm/encoder/", 12) == 0) {
            writeSurface("encoder", a + 12, m);
        } else if (std::strncmp(a, "/mm/switch/", 11) == 0) {
            // A switch is a BOOL control, so it takes a boolean rather than a byte: `toByte` would
            // scale a float 1.0 to 255, and parseBool accepts only `true` or `1`, so an "on" from a
            // float-sending controller (which is most of them) read as false. Any nonzero value is
            // on, which is what a pad, a toggle and a MIDI note-on all mean.
            writeSurface("switch", a + 11, m, /*asBool=*/true);
        } else if (std::strcmp(a, "/mm/hello") == 0) {
            // "I have just started, tell me everything." A client that reconnects from the SAME
            // address cannot be spotted by the peer-changed check, and Open Stage Control sends
            // nothing of its own on load, so a restart left every widget showing its layout file's
            // defaults until the user happened to move something. One explicit address fixes it,
            // and costs a controller that does not send it nothing.
            resendAll_ = true;
        } else if (std::strncmp(a, "/mm/control/", 12) == 0) {
            writeControl(a + 12, m);
        }
        received_++;
    }

    /// `/mm/fader/N`, `/mm/encoder/N` and `/mm/switch/N`: write ControlModule's own control, so the surface reacts
    /// exactly as it does to a click in the UI and driveFader routes it onward.
    void writeSurface(const char* prefix, const char* indexText, const osc::Message& m,
                      bool asBool = false) {
        // strtol, not atoi: this is unvalidated network input, and atoi cannot tell "0" from
        // "not a number at all". `end` also rejects trailing junk, so /mm/fader/1x is not a fader.
        char* end = nullptr;
        const long idx = std::strtol(indexText, &end, 10);
        if (end == indexText || *end != '\0') return;
        if (idx < 1 || idx > kSurfaceWidth) return;  // the surface is 8 wide; anything else is not ours
        char control[16];
        std::snprintf(control, sizeof(control), "%s%ld", prefix, idx);
        // A switch reads the RAW value, not the scaled byte: toByte rounds a float of 0.001 to 0,
        // so a controller sending a small positive value would turn the switch off while saying on.
        // Any nonzero is on, which is what a pad, a toggle and a MIDI note-on all mean.
        if (asBool) setBool("Control", control, osc::isTruthy(m));
        else        setValue("Control", control, osc::toByte(m));
    }

    /// `/mm/control/<Module>/<control>`: reach any control directly. The module and control names
    /// are taken verbatim, so a typo simply does not resolve, exactly as it would over HTTP.
    void writeControl(const char* rest, const osc::Message& m) {
        const char* slash = std::strchr(rest, '/');
        if (!slash || slash == rest || !slash[1]) return;
        char module[24];
        const size_t n = static_cast<size_t>(slash - rest);
        if (n >= sizeof(module)) return;
        std::memcpy(module, rest, n);
        module[n] = '\0';
        setValue(module, slash + 1, osc::toByte(m));
    }

    /// A BOOL control's body is the JSON literal, not a number: parseBool accepts `true` or `1`,
    /// and a scaled byte (255 for a float 1.0) is neither.
    void setBool(const char* module, const char* control, bool on) {
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char body[32];
        std::snprintf(body, sizeof(body), "{\"value\":%s}", on ? "true" : "false");
        sched->setControl(module, control, body);
    }

    void setValue(const char* module, const char* control, uint8_t value) {
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char body[32];
        std::snprintf(body, sizeof(body), "{\"value\":%u}", static_cast<unsigned>(value));
        sched->setControl(module, control, body);
    }

    /// Open + bind, deferred to the tick path and throttled on failure: the same shape
    /// AudioService uses, so a boot-present module cannot touch lwip before the stack is up and a
    /// busy port cannot burn a socket per tick.
    bool ensureSocket() {
        if (open_) return true;
        if (!platform::networkReady()) return false;
        const uint32_t now = platform::millis();
        if (lastFailMs_ != 0 && now - lastFailMs_ < kOpenRetryMs) return false;
        if (sock_.open() && sock_.bind(port)) {
            open_ = true;
            if (!attached_) {
                if (auto* c = ControlModule::active()) { c->addSurface(this); attached_ = true; }
            }
            lastFailMs_ = 0;
            reportPeer();
            return true;
        }
        sock_.close();
        lastFailMs_ = now == 0 ? 1 : now;
        // A busy port is a real failure, not a note: nothing will ever arrive, and the severity is
        // what makes the card say so rather than looking like a normal state.
        setStatusf(Severity::Error, "port %u busy", static_cast<unsigned>(port));
        return false;
    }

    void closeSocket() {
        if (open_) sock_.close();
        open_ = false;
        lastFailMs_ = 0;
        setStatus(enabledOsc ? "opening" : "off");
    }

    /// Where feedback goes: the configured address when set, else the last peer that wrote to us.
    /// False when neither is known, which is the normal state before anything has connected.
    bool feedbackDest(uint8_t out[4]) const {
        unsigned a, b, c, d;
        int used = 0;
        // %n captures how much was consumed, so trailing junk is rejected: sscanf alone accepts
        // "1.2.3.4nonsense" as a valid address, and silently sending feedback to a mistyped host
        // is worse than falling back to the peer that actually wrote to us.
        if (feedbackTo_[0]
            && std::sscanf(feedbackTo_, "%u.%u.%u.%u%n", &a, &b, &c, &d, &used) == 4
            && feedbackTo_[used] == '\0'
            && a < 256 && b < 256 && c < 256 && d < 256) {
            out[0] = static_cast<uint8_t>(a); out[1] = static_cast<uint8_t>(b);
            out[2] = static_cast<uint8_t>(c); out[3] = static_cast<uint8_t>(d);
            return true;
        }
        if (peer_[0] || peer_[1] || peer_[2] || peer_[3]) { std::memcpy(out, peer_, 4); return true; }
        return false;
    }

    char     feedbackTo_[16] = {};   ///< an override; empty means "answer whoever wrote to us"
    /// The port, and WHO last reached us on it.
    ///
    /// Our own address is not worth reporting: the user got to this card by typing it. Whether a
    /// client is actually getting through is the thing they cannot see, and the first question worth
    /// asking when a surface does not respond.
    void reportPeer() {
        // A peer that has gone QUIET is not a peer: the address alone would still claim a client is
        // there minutes after it stopped, which is worse than saying nothing while someone is
        // debugging a surface that died. Five seconds is long enough to survive an idle controller
        // between gestures and short enough that the card stops lying quickly.
        if (peerFresh())
            setStatusf(Severity::Status, "%u from %u.%u.%u.%u", static_cast<unsigned>(port),
                       peer_[0], peer_[1], peer_[2], peer_[3]);
        else
            setStatusf(Severity::Status, "listening on %u", static_cast<unsigned>(port));
    }

    /// Is a client still talking to us? Five seconds is long enough to survive an idle controller
    /// between gestures, short enough that the card stops claiming a peer that has gone.
    bool peerFresh() const {
        return lastRecvMs_ != 0 && platform::millis() - lastRecvMs_ < kPeerStaleMs;
    }
    static constexpr uint32_t kPeerStaleMs = 5000;

    uint8_t  peer_[4] = {};          ///< the last source address, learned in tick()
    uint32_t lastRecvMs_ = 0;        ///< when we last heard from it, so the status can go stale
    bool     peerWasFresh_ = false;  ///< what the status last said, so it is rewritten only on a change
    bool     attached_ = false;
    bool     resendAll_ = false;   ///< a new peer appeared; push every value once
    platform::UdpSocket sock_;
    bool     open_ = false;
    uint32_t lastFailMs_ = 0;
    uint32_t received_ = 0;
    /// setStatus takes a BORROWED pointer, so the formatted text lives here rather than in a
    /// temporary. The base class owns the status itself, including its severity.
    char     statusStr_[32] = "off";

    /// Format into that buffer and report it, the shape ControlModule uses for the same reason.
    void setStatusf(Severity sev, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(statusStr_, sizeof(statusStr_), fmt, ap);
        va_end(ap);
        setStatus(statusStr_, sev);
    }
};

} // namespace mm
