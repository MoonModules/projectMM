// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file MoonTalkModule.h
/// MoonTalk: a public message board between projectMM devices, in the shape Meshtastic's channel
/// chat has.
///
/// A second MoonCloud child, with its OWN consent: agreeing to share a chip model says nothing
/// about wanting to publish messages, so the two are separate questions and neither implies the
/// other ([MoonCloudModule.h](MoonCloudModule.h)).
///
/// **Everything sent here is public and permanent.** There is no private message, no recipient and
/// no delete: a message goes on a board anyone can read, and that is what the consent is asking
/// about. Nothing is sent until it says yes, and the module never posts on its own: a message
/// exists because somebody typed it.
///
/// **Two consents, not one.** `consent` allows posting at all. `shareName` is separate and OFF by
/// default, because a device name is a real identifier: "MM-A094" says nothing, "Ewoud's bedroom"
/// says a great deal, and the second is what people actually type. Without it a message is
/// attributed to the first 8 characters of the installation id, which groups one device's messages
/// without naming anyone.
///
/// **Reading needs no consent at all.** The board is public, so fetching it sends nothing about
/// this device, exactly like the MoonCloud Stats aggregates.
///
/// **There is no authentication**, so a sender id can be fabricated by anyone posting by hand.
/// Acceptable for a hobby board where nothing is gated on identity, and stated in the privacy
/// policy rather than left to be discovered.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/MoonCloudModule.h"
#include "core/Scheduler.h"
#include "core/JsonSink.h"
#include "core/MoonModule.h"
#include "platform/platform.h"

namespace mm {

class MoonTalkModule : public MoonModule {
public:
    enum Consent : uint8_t { Unanswered = 0, Yes = 1, Never = 2 };

    void defineControls() override {
        controls_.clear();

        static const char* kConsentOptions[] = {"Not answered", "Yes", "Never"};
        controls_.addSelect("consent", consent_, kConsentOptions, 3);

        // Separate from `consent` and default OFF. A device name is the one field here that
        // identifies a person rather than a machine, so sharing it is its own decision.
        controls_.addControl("shareName", shareName_);

        // What to say. Cleared after a send, so the box is empty for the next message rather than
        // inviting an accidental repost.
        controls_.addText("message", message_, sizeof(message_));

    }

    /// Post whatever is in `message` when the user commits it. Driven by a control write rather
    /// than a tick: a message board that posts on a timer would be a bot.
    void onControlChanged(const char* name) override {
        if (!name || std::strcmp(name, "message") != 0) return;
        if (consent_ != Yes) { message_[0] = 0; return; }   // never send without consent
        if (message_[0] == 0) return;                       // cleared, nothing to do

        // A TEXT CONTROL REPORTS EVERY KEYSTROKE, debounced. That suits a setting, where the last
        // value wins and the ones before it are harmless, and it does not suit a message: typing
        // "hello" published "hel" and then "hell" as messages of their own.
        //
        // So a message is held until it stops growing. Each write restarts a short wait; the send
        // happens in tick1s once nothing has arrived for a moment, which is the same thing a person
        // means by having finished typing. Held HERE rather than in the browser because it is a
        // rule about what a message is, and a device posting from a script or over the API deserves
        // it too.
        pendingMs_ = kSettleMs;
    }

    // Same shape as MoonCloud Stats: the post is a bounded blocking call on the 1 Hz housekeeping
    // tick rather than the per-frame path, and `-Wfunction-effects` warns because the base hook is
    // declared MM_NONBLOCKING for that per-frame case. The frequency is what makes it fine: a
    // message is sent because a person typed one, and the settle check below returns immediately on
    // every tick where nobody did.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        if (pendingMs_ == 0) return;
        pendingMs_ = pendingMs_ > 1000 ? pendingMs_ - 1000 : 0;
        if (pendingMs_ != 0) return;

        if (consent_ == Yes && message_[0]) send();
        message_[0] = 0;   // sent, or refused: the box empties either way
        markDirty();
    }

    /// The device name, read from SystemModule at send time rather than pushed in by whoever knows
    /// it. A setter was the first shape and it was never called, so every message went out unnamed
    /// while `shareName` said otherwise: a copy that nothing updates is worse than no copy.
    ///
    /// Read at SEND time, not cached, so renaming the device renames the sender on the next message
    /// rather than at the next reboot.
    const char* deviceName() const {
        auto* sched = Scheduler::instance();
        if (!sched) return "";
        for (uint8_t i = 0; i < sched->moduleCount(); i++) {
            const MoonModule* m = sched->module(i);
            if (!m || !m->name() || std::strcmp(m->name(), "System") != 0) continue;
            auto& ctrls = m->controls();
            for (uint8_t c = 0; c < ctrls.count(); c++) {
                const ControlDescriptor& d = ctrls[c];
                if (d.ptr && d.name && std::strcmp(d.name, "deviceName") == 0 &&
                    (d.type == ControlType::Text || d.type == ControlType::ReadOnly)) {
                    return static_cast<const char*>(d.ptr);
                }
            }
        }
        return "";
    }

    Consent consent() const { return static_cast<Consent>(consent_); }
    bool sharesName() const { return shareName_ && consent_ == Yes; }

private:
    /// Build and post one message. Same server and scheme rule as MoonCloud Stats: port 443 or
    /// none means the public server over HTTPS, anything else a local one over plain HTTP.
    void send() {
        char id[kInstallationIdChars + 1] = {};
        installationId(id);
        if (!id[0]) return;

        JsonSink body;
        body.append("{\"sender\":");
        body.writeJsonString(id);
        // The name rides ONLY when both consents allow it. Omitted rather than sent empty, so the
        // server stores nothing rather than a blank it would have to interpret.
        const char* name = deviceName();
        if (shareName_ && name && name[0]) {
            body.append(",\"name\":");
            body.writeJsonString(name);
        }
        body.append(",\"text\":");
        body.writeJsonString(message_);
        body.append("}");
        body.flush();

        // Sent through the container, which owns the address and the scheme choice.
        if (auto* cloud = static_cast<const MoonCloudModule*>(parent())) {
            (void)cloud->post("/api/talk", body.data());
        }
    }

    /// How long a message must stop changing before it counts as finished.
    static constexpr uint16_t kSettleMs = 2000;
    uint16_t pendingMs_ = 0;

    uint8_t consent_ = Unanswered;
    bool shareName_ = false;
    char message_[192] = {};
};

}  // namespace mm
