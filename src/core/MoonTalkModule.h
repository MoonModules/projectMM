// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @file MoonTalkModule.h
/// MoonTalk: a public message board between projectMM devices.
///
/// A MoonCloud child with its OWN consent: agreeing to share a chip model says nothing about wanting
/// to publish messages.
///
/// **Everything sent here is public and permanent.** No private message, no recipient, no delete,
/// and the module never posts on its own: a message exists because somebody typed it.
///
/// **Two consents.** `consent` allows posting at all; `shareName` is separate and OFF by default, because
/// "MM-A094" says nothing while "Ewoud's bedroom" says a great deal. Without it a message is
/// attributed to the first 8 characters of the installation id.
///
/// Reading needs no consent and sends nothing. There is no authentication, so a sender id can be
/// fabricated by anyone posting by hand: acceptable for a board where nothing is gated on identity,
/// and stated in the privacy policy rather than left to be discovered.

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
    void defineControls() override {
        controls_.clear();

        // A checkbox: publishing is on or off, and a third state said nothing extra.
        controls_.addControl("consent", consent_);

        // Default OFF: a device name is the one field here that identifies a person.
        controls_.addControl("shareName", shareName_);

        controls_.addText("message", message_, sizeof(message_));
        // Below the text it sends, and the ONLY thing that publishes.
        controls_.addButton("send");
    }

    /// A message is published when the user presses `send`, and never before.
    ///
    /// The first shape sent on a `message` write, which a Text control emits on every debounced
    /// KEYSTROKE: typing "hello" published "hel" and "hell" as messages of their own. A settle
    /// window then guessed when typing had stopped, and guessing wrong cleared a half-typed
    /// message. A button removes the guess entirely: a keystroke is just a keystroke.
    void onControlChanged(const char* name) override {
        if (!name || std::strcmp(name, "send") != 0) return;
        if (!consent_) return;           // never publish without consent
        if (message_[0] == 0) return;    // nothing typed

        send();
        message_[0] = 0;                 // the box empties once the message is on its way
        markDirty();
    }

    /// Read from SystemModule at SEND time, not cached: a setter was never called, so every message
    /// went out unnamed while `shareName` said otherwise. Renaming the device now takes effect on
    /// the next message rather than the next reboot.
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

    bool consent() const { return consent_; }
    bool sharesName() const { return shareName_ && consent_; }
    const char* message() const { return message_; }

    /// Set the two consents directly, so a test can walk the matrix without a UI write.
    void setConsentForTest(bool yes) { consent_ = yes; }
    void setShareNameForTest(bool share) { shareName_ = share; }

    /// Put text in the box the way a control write does, so a test can press send without a UI.
    void setMessageForTest(const char* text) {
        std::snprintf(message_, sizeof(message_), "%s", text ? text : "");
    }

private:
    void send() {
        char id[kInstallationIdChars + 1] = {};
        installationId(id);
        if (!id[0]) return;

        JsonSink body;
        body.append("{\"sender\":");
        body.writeJsonString(id);
        // Only when both consents allow it, and omitted rather than sent empty.
        const char* name = deviceName();
        if (shareName_ && name && name[0]) {
            body.append(",\"name\":");
            body.writeJsonString(name);
        }
        body.append(",\"text\":");
        body.writeJsonString(message_);
        body.append("}");
        body.flush();

        // Sent through the container, which owns the address.
        if (auto* cloud = static_cast<const MoonCloudModule*>(parent())) {
            (void)cloud->post("/api/talk", body.data());
        }
    }

    bool consent_ = false;
    bool shareName_ = false;
    char message_[192] = {};
};

}  // namespace mm
