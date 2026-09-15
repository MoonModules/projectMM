#pragma once

#include "light/drivers/DriverBase.h"

#include "light/ColorLight5A75Packet.h"   // the first wire format (byte layout lives there)
#include "platform/platform.h"

namespace mm {

/// Output driver: streams the buffer to LED panel cards over **raw Ethernet frames**, below IP.
/// These cards take a sender-card feed rather than a pixel protocol — row-addressed data plus a
/// sync frame that latches — so they need an L2 seam (`platform::ethSendRaw`) rather than a socket.
/// The wire format lives in ColorLight5A75Packet.h; this driver owns the window, the correction and
/// the chunking, exactly as NetworkSendDriver does for ArtNet/E1.31/DDP.
///
/// **The board renders and sends.** Effects, layers and MoonLive run here, so a device with this
/// driver is a complete panel controller. Taking ArtNet in and forwarding it is one application of
/// the same driver (add a NetworkReceiveEffect), not a prerequisite.
///
/// **The vendor vocabulary**, since the datasheets and LEDVision use it: an LED wall is driven by a
/// *sending card* (a PC's PCI-E board, or a standalone box) that feeds *receiving cards*, one in each
/// cabinet, which decode the signal and drive the panels. This driver puts the board in the sending
/// card's place, and the cards it talks to are receiving cards. "Panel card" is the plainer name for
/// the same hardware, and the one that keeps reading correctly when a second vendor's format lands.
///
/// **No geometry controls.** The panel arrangement belongs to the Layout — `PanelsLayout` states
/// how many panels there are, their size, their wiring order and snaking, and maps every light to
/// an (x, y). This driver reads the finished picture and cuts it into card rows, so a wall is
/// described in exactly one place.
///
/// **No IP involved.** No address, no port, no DHCP lease: the frames carry their own destination
/// MAC and EtherType. A board whose DHCP never completes still drives panels, which is also why the
/// driver reports the link state itself rather than trusting `ethConnected()`.
///
/// The deep dives are under *More info*, below the attribute/method lists:
/// @xref{why-a-gigabit-link|why these cards need a gigabit link},
/// @xref{running-this-on-a-host|running this on a desktop or a Pi},
/// @xref{other-card-vendors|other card vendors, and how to add one}.
/// @card PanelCardDriver.png
///
/// @moreinfo
///
/// ## Why a gigabit link
///
/// The cards require a **1000 Mbps** link, and the reason is wire time rather than bandwidth. Even a
/// 256×256 panel at 40 fps is only ~65 Mbit/s of payload, which 100 Mbit would seem to carry. But
/// the cards are dumb receivers with no buffering and no flow control: they latch on the sync frame,
/// so an entire frame must arrive inside the inter-frame window. At 100 Mbit the same bytes take ten
/// times as long on the wire — a 256×256 frame is ~16 ms of transmission against ~1.6 ms at gigabit
/// — which overruns the frame budget and breaks the timing the sync depends on.
///
/// The failure mode is the confusing part: nothing errors. Frames go out, the link is up, and the
/// panels tear, show wrong rows, or never latch. That is why this driver reads the NEGOTIATED speed
/// (`platform::ethLinkSpeedMbps`) and says so in its status rather than letting a slow link look
/// like a format bug. It still SENDS at 100 Mbit — a small panel may be fine, and the measurement
/// is more useful than a refusal.
///
/// ## Running this on a host
///
/// The desktop build sends real frames too, via `platform::ethBindRawInterface` — so a Raspberry
/// Pi, a Mac or a Windows PC running projectMM is a panel controller, which is the deployment this
/// replaces. Linux uses AF_PACKET and macOS BPF, both needing root or CAP_NET_RAW; Windows has no
/// kernel path for raw L2 at all and goes through Npcap, resolved at run time so the binary still
/// builds and runs without it. Without the privilege or the driver, or with `interface` left blank,
/// the host records frames instead of sending them, which is what lets the unit tests pin the wire
/// format with no hardware and no privileges.
///
/// `interface` names the NIC: the kernel name on Linux and macOS (`eth0`, `en0`), and on Windows any
/// distinctive part of the adapter description (`Realtek`), because a capture device there is spelled
/// `\Device\NPF_{GUID}` and does not fit a control a human types into.
///
/// On ESP32 `interface` is ignored: the chip has one MAC.
///
/// ## Other card vendors
///
/// ColorLight is one of several receiver-card makers, which is why this driver is named for the
/// category and carries a `format` selector rather than being a ColorLight driver:
///
/// | vendor | position in the market |
/// |---|---|
/// | **[NovaStar](https://www.novastar.tech)** | the global leader; large-scale displays and stage events |
/// | **[ColorLight](https://www.colorlightinside.com)** | cost-effective, strong outdoor and fine-pitch support (the format implemented here) |
/// | **[Linsn](https://www.linsnled.com)** | affordable and stable, common on budget installations |
/// | **[Mooncell](https://www.mooncell.com.cn)** | full-color high-refresh niche |
/// | **[Huidu](https://www.huidu.cn)** | small and mid projects, storefront signage, mostly ASYNCHRONOUS |
/// | **DBstar**, **Xixun** | also in the field |
///
/// **Contributions welcome.** Adding one is a `*Packet.h` beside ColorLight5A75Packet.h plus an
/// entry in `kFormatOptions`: the window, the correction, the chunking and the platform seam are
/// already shared, and the desktop capture path lets the byte layout be pinned by unit tests with no
/// hardware. What it actually costs is the research, not the code.
///
/// Two things to know before starting. Each vendor speaks its **own proprietary L2 protocol**, so a
/// ColorLight frame will not drive a NovaStar card and the byte layout has to be obtained per
/// vendor. And whether another format fits this driver's row-plus-sync model is **unverified**: the
/// shape is an invitation, not a promise, and a format that addresses panels differently may need
/// the driver to grow rather than just gain a packet file.
///
/// Huidu is the one to approach with care: its controllers are largely asynchronous, playing from
/// onboard storage rather than being fed live, which is a different product category from a
/// real-time sender.
// Prior art: FPP (Falcon Player), the show player that drives these receiving cards from a
// Raspberry Pi. Seeing an FPP rig feed a wall of panels is what prompted this driver: a board
// already rendering those frames can send them itself, which removes the host from the
// installation. The wire format is the ColorLight 5A-75 documented byte layout, not FPP's code.
class PanelCardDriver : public DriverBase {
public:
    /// Panel cards are RGB, so this references the "RGB" preset rather than the strips' "GRB" —
    /// same per-driver default the network sinks use. The user can still pick any preset.
    PanelCardDriver() { setDefaultPresetName("RGB"); }

    /// The card's own gain, held at full. Our Correction has already applied brightness to the pixel
    /// bytes, so scaling again on the card would compound the two. Named once because both the
    /// brightness frame and the sync frame carry it.
    static constexpr uint8_t kCardGain = 0xFF;

    /// Wire formats. One entry today; the control exists because the category is panel cards rather
    /// than one vendor, and a second format is a packet file plus an entry here.
    static constexpr const char* kFormatOptions[] = {"ColorLight 5A-75"};
    static constexpr uint8_t kFormatCount = 1;

    /// Wire format (index into kFormatOptions).
    uint8_t format = 0;

    /// Card firmware generation. v13 and newer act on the SECOND copy of the brightness and sync
    /// frames, so both go out twice; v12 and older act on the FIRST and take the second sync as
    /// another latch, aborting a refresh already in progress. Measured on a 5A-75 v8.0 downgraded
    /// to 11.09: the panel updated once every few seconds until the duplicate was dropped.
    ///
    /// A control rather than a probe because THIS DRIVER is send-only, not because the protocol is.
    /// The cards do answer: a discovery reply carries the major version in `data[2]`, which is how
    /// FPP auto-detects it and how ColorLight's own LEDUpgrade reports a card as "5A 13.17". Probing
    /// needs a receive seam beside platform::ethSendRaw, which does not exist yet. FPP keeps the
    /// manual setting regardless, as its FIRST source, falling back to discovery only when unset.
    /// v12-and-older FIRST, and the default. A stock card ships on v13, but v13 on v8.x hardware
    /// has a flicker defect with no sending-side workaround, so the documented path is to downgrade
    /// the card (how-to/panel-cards.md). Defaulting to the generation the guide leaves you on
    /// means the setting is already right when you finish, rather than being the last unexplained
    /// step between a downgraded card and a wall that updates once every few seconds.
    static constexpr const char* kFirmwareOptions[] = {"v12 and older", "v13 and newer"};
    static constexpr uint8_t kFirmwareCount = 2;

    /// Card firmware generation (index into kFirmwareOptions): 0 is v12-and-older, 1 is v13+.
    uint8_t firmware = 0;
    /// Host NIC to send from: a Select over the DETECTED interfaces (platform::rawInterfaces),
    /// row 0 = capture-only. Persisted by LABEL, not index: a NIC keeps its identity across
    /// reboots and Npcap reinstalls (the index-mismatch trap a Windows tester reported). Old
    /// configs that stored a typed name load unchanged: the Select apply path matches labels.
    /// Not built on ESP32, which has one MAC and nothing to pick.
    uint8_t interfaceSel_ = 0;
    /// The LABEL behind interfaceSel_, so a re-enumeration that reorders the list can restore the
    /// same NIC rather than whatever now sits at that index. Sized to the enumeration's own cap.
    char chosenIf_[64] = {};
    /// The row the last rebuild settled on. Its only job is to distinguish a user's pick (the
    /// index moved) from an OS re-enumeration (the index did not), which decides whether the
    /// label above may overrule the index. Starts equal to interfaceSel_, so the first rebuild
    /// reads as "nothing picked yet".
    uint8_t lastResolvedSel_ = 0;
    /// Send-rate ceiling (Hz); tick() rate-limits so a fast render tick doesn't saturate the link.
    uint8_t fps = 40;

    /// This driver's controls, after the correction block DriverBase places at the top of every
    /// driver card: format, panel geometry, the host interface, the shared window, then the cap.
    void defineDriverControls() override {
        controls_.addSelect("format", format, kFormatOptions, kFormatCount);
        controls_.addSelect("firmware", firmware, kFirmwareOptions, kFirmwareCount);
        // Desktop/Raspberry-Pi only: which host NIC to open a raw socket on, re-enumerated on
        // every rebuild so a hot-plugged NIC appears (the audio device Select's pattern).
        if constexpr (platform::hasNamedNetInterfaces) {
            const char* const* ifOptions = nullptr;
            const size_t n = platform::rawInterfaces(&ifOptions);
            // The list is re-enumerated on every rebuild, and the OS does not promise a stable
            // order: a hot-plugged NIC can shift the rest. So the remembered LABEL re-points the
            // index before the Select binds to it. Persisting by label (below) covers reboots;
            // this covers the same list changing under a running session, which would otherwise
            // silently send panel data out of a different adapter.
            //
            // But only ONE of those two things can have moved, and restoring in the wrong case is
            // destructive rather than merely useless. Which moved is knowable: the index changing
            // since we last resolved one means the USER picked a row, while the index standing
            // still means the list did. Without this test the restore reverted every pick to the
            // previously remembered adapter, prepare() then rewrote the label from that, and the
            // wrong value became self-sustaining. Measured on a Windows bench: no selection could
            // be made at all, the field springing back to "none (capture only)" every time.
            const bool userPicked = (interfaceSel_ != lastResolvedSel_);
            if (!userPicked && chosenIf_[0] && ifOptions) {
                // Compare the STABLE HEAD, the part before ", ": a label may carry the adapter's
                // live link speed after it ("Realtek PCIe GbE, 1 Gb"), and a renegotiated link
                // would otherwise read as a different NIC and drop the selection to row 0.
                const char* mySep = std::strstr(chosenIf_, ", ");
                const size_t mine = mySep ? static_cast<size_t>(mySep - chosenIf_)
                                          : std::strlen(chosenIf_);
                // Back to capture-only FIRST: if the remembered adapter is gone, the old index
                // now points at whatever took its place, and the driver would send panel data
                // out of a NIC the user never chose. No match means no NIC, explicitly.
                interfaceSel_ = 0;
                for (size_t i = 0; i < n && i < 255; i++) {
                    if (!ifOptions[i]) continue;
                    const char* sep = std::strstr(ifOptions[i], ", ");
                    const size_t head = sep ? static_cast<size_t>(sep - ifOptions[i])
                                            : std::strlen(ifOptions[i]);
                    if (head == mine && std::strncmp(ifOptions[i], chosenIf_, head) == 0) {
                        interfaceSel_ = static_cast<uint8_t>(i);
                        break;
                    }
                }
            }
            // Record what this rebuild settled on, so the next one can tell a pick from a
            // re-enumeration, and keep the remembered label in step with it. prepare() writes the
            // same label; doing it here too means a rebuild that never reaches prepare (a pick on
            // a disabled driver) still leaves the two agreeing.
            lastResolvedSel_ = interfaceSel_;
            if (ifOptions && interfaceSel_ < n && ifOptions[interfaceSel_])
                std::snprintf(chosenIf_, sizeof(chosenIf_), "%s", ifOptions[interfaceSel_]);
            controls_.addSelect("interface", interfaceSel_, ifOptions,
                                static_cast<uint8_t>(n < 255 ? n : 255));
            controls_.setPersistLabel(controls_.count() - 1);
        }
        addWindowControls();   // start / count — which slice of the shared buffer this sink sends
        controls_.addControl("fps", fps, 1, 120);
    }

    /// Geometry and the window change how much of the buffer is corrected, so both re-run the
    /// prepare sweep; `interface` re-binds the raw socket, which also happens off the hot path.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "interface") == 0
               || isWindowControl(name) || isCorrectionControl(name);
    }

    /// Drop back to capture mode so a disabled driver holds no raw socket, then chain to the base.
    void release() override {
        if (claimed_) { platform::ethClaimRawL2(false); claimed_ = false; }
        platform::ethBindRawInterface(nullptr);
        DriverBase::release();
    }

    /// Take the shared source buffer and size the corrected_ buffer for it.
    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
        resizeCorrected();
    }

    /// Pure build: bind the raw interface (host only), size corrected_, and publish the status the
    /// card shows. All the checks that would otherwise be guesses in tick() happen here.
    void prepare() override {
        resizeCorrected();

        // Tell the network layer this interface is ours: we drive it below IP, so its DHCP cascade
        // should not report a leaseless link as a fault. Claimed HERE rather than on first send —
        // stating it before any frame goes out cannot race the cascade's own timeout. Idempotent
        // across re-prepares via claimed_.
        if (!claimed_) { platform::ethClaimRawL2(true); claimed_ = true; }

        // A host needs a named NIC to reach the wire; ESP32 accepts and ignores it. Every prepare
        // re-syncs the binding, INCLUDING the blank case — clearing the field must return the host
        // to capture-only rather than leaving the last interface bound until release(). A bind
        // failure is a Warning rather than an Error: the driver still runs and still records frames,
        // which is what a test or a dry run wants — it just is not driving panels.
        const char* ifName = nullptr;
        if constexpr (platform::hasNamedNetInterfaces) {
            // Remember the adapter behind the current row, so a later rebuild that re-enumerates
            // in a different order can find this same NIC again rather than trusting the index.
            const char* const* ifOptions = nullptr;
            const size_t n = platform::rawInterfaces(&ifOptions);
            // chosenIf_ and lastResolvedSel_ are a PAIR: together they say "this row, by this
            // name, is what we last settled on". Writing one without the other is what makes a
            // later rebuild misread a re-enumeration as a fresh pick, or the reverse. A pick
            // re-prepares (affectsPrepare) without necessarily rebuilding the controls, so this
            // is a place the pair has to be kept in step, not only defineDriverControls().
            lastResolvedSel_ = interfaceSel_;
            if (ifOptions && interfaceSel_ < n && ifOptions[interfaceSel_])
                std::snprintf(chosenIf_, sizeof(chosenIf_), "%s", ifOptions[interfaceSel_]);
            ifName = platform::rawInterfaceName(interfaceSel_);
        }
        if (!platform::ethBindRawInterface(ifName)) {
            // Two very different causes reach here and the fixes are opposite: a name that matches
            // no adapter (a typo, or an OS naming the NIC differently) versus the privilege raw L2
            // needs. Blaming root for a typo sends the reader to sudo, which cannot help. Name the
            // string we failed to match so the likelier cause is the one they read first.
            if (ifName) {
                std::snprintf(statusBuf_, sizeof(statusBuf_),
                              "cannot open '%s' - no adapter matches, or needs root", ifName);
                setStatus(statusBuf_, Severity::Warning);
            } else {
                setStatus("cannot open interface (needs root?)", Severity::Warning);
            }
            return;
        }

        writeLinkStatus();
    }

    /// Refresh the link status once a second: a cable plugged in after prepare() ran would
    /// otherwise leave the card reporting "no ethernet link" on a link that is up.
    void tick1s() MM_NONBLOCKING override {
        writeLinkStatus();
        MoonModule::tick1s();
    }

    /// A preset toggle changes correction_.outChannels without a structural rebuild.
    void onCorrectionChanged() override { resizeCorrected(); }

    /// Rate-limit, apply this driver's correction, then emit the window row by row and latch it
    /// with one sync frame. One packet buffer, reused; no allocation on this path.
    void tick() MM_NONBLOCKING override {
        if (!sourceBuffer_ || !sourceBuffer_->data()) return;
        if (fps == 0) return;

        const uint32_t interval = 1000 / fps;
        const uint32_t now = platform::millis();
        if (now - lastSendTime_ < interval) return;
        lastSendTime_ = now;

        // The wall comes from the Layout — a PanelsLayout already states how many panels there are,
        // how big each one is, and how they are wired, and it has mapped every light to an (x, y).
        // So this driver needs no panel geometry of its own: it reads the finished picture and cuts
        // it into card rows. Restating the panel arrangement here would be a second place to get it
        // wrong, and the layout's version is richer (wiring order, snaking, per-panel offsets).
        if (!layer()) return;                       // no dimensions to send against
        const lengthType wallW = layer()->physicalWidth();
        const lengthType wallH = layer()->physicalHeight();
        if (wallW == 0 || wallH == 0) return;

        // The window slice this sink owns, then correction into corrected_ (sized off the hot path).
        // Same three guards and the same passthrough fallback as the other sinks: a stale or
        // unwired correction must degrade to raw bytes, never overrun the allocation.
        nrOfLightsType winStart, nLights;
        windowSlice(sourceBuffer_->count(), winStart, nLights);
        if (nLights == 0) return;

        const uint8_t* data;
        uint8_t stride;
        const uint8_t outCh = correction_.outChannels;
        if (outCh != 0 && corrected_.data()
            && corrected_.count() >= nLights
            && corrected_.channelsPerLight() >= outCh) {
            const uint8_t* src = sourceBuffer_->data();
            const uint8_t srcCh = sourceBuffer_->channelsPerLight();
            uint8_t* dst = corrected_.data();
            for (nrOfLightsType i = 0; i < nLights; i++) {
                // srcCh carries a wide light's motion channels through, as NetworkSendDriver does.
                correction_.apply(src + (winStart + i) * srcCh, dst + i * outCh, srcCh);
            }
            data = dst;
            stride = outCh;
        } else {
            const uint8_t srcCh = sourceBuffer_->channelsPerLight();
            data = sourceBuffer_->data() + static_cast<size_t>(winStart) * srcCh;
            stride = srcCh;
        }

        // Nothing to send if the buffer covers no row at all — bail before putting the brightness
        // pair on the wire, so a misconfigured window is silent rather than emitting frames per tick.
        if (nLights < static_cast<nrOfLightsType>(wallW)) return;

        // How many copies of the brightness and sync frames this card wants: see `firmware`. Sending
        // two to a card that acts on the first is not harmless, which is why this is a choice and
        // not a constant.
        // Index 1 is v13-and-newer, which acts on the SECOND copy, so it needs both sent.
        const int frameCopies = (firmware == 1) ? 2 : 1;

        // Brightness first, ahead of the rows — the order the cards expect. Advisory: older card
        // firmware ignores it, and the driver never depends on it having landed (our own Correction
        // has already applied brightness to the pixel data, so this only sets the card's own gain).
        {
            const size_t len = buildColorLightBrightnessPacket(packet_, kCardGain);
            // Once or twice, per `firmware` (frameCopies above).
            //
            // Counted like the rows: `dropped` and the platform's per-cause totals must describe the
            // SAME set of frames, or comparing them tells you nothing.
            for (int i = 0; i < frameCopies; i++) {
                if (platform::ethSendRaw(packet_, len)) framesSent_++;
                else framesDroppedTotal_++;
            }
        }

        // One card row per wall row, in order. The card's own row numbering runs across its outputs
        // (a stack of panels is several outputs), and that matches wall rows top to bottom — which is
        // what the vendor tool configured the card to expect.
        bool anyRowSent = false;
        for (lengthType row = 0; row < wallH; row++) {
            if (static_cast<size_t>(row) * wallW >= nLights) break;   // buffer ran out before the wall
            for (lengthType off = 0; off < wallW; off += COLORLIGHT_MAX_PIXELS_PER_PACKET) {
                lengthType n = static_cast<lengthType>(wallW - off);
                if (n > COLORLIGHT_MAX_PIXELS_PER_PACKET) n = COLORLIGHT_MAX_PIXELS_PER_PACKET;

                const size_t first = static_cast<size_t>(row) * wallW + off;
                if (first >= nLights) break;                       // buffer ran out before the wall
                if (first + n > nLights) n = static_cast<lengthType>(nLights - first);
                if (n == 0) break;

                const size_t len = packRow(static_cast<uint16_t>(row), static_cast<uint16_t>(off),
                                           static_cast<uint16_t>(n), data + first * stride, stride);
                // A failed frame is dropped, not retried: the cards have no acknowledgement to wait
                // for, and stalling the render tick to retry would cost the next frame too.
                // Set on a frame that actually reached the wire, not on reaching the row: a link
                // that drops mid-frame fails every send, and latching then would blank the panels
                // — which is the case the sync guard below exists to prevent.
                if (platform::ethSendRaw(packet_, len)) { framesSent_++; anyRowSent = true; }
                else framesDroppedTotal_++;
            }
        }

        // The sync frame latches everything above, so it goes LAST and only if something was sent —
        // latching an empty frame would blank the panels on a misconfigured window.
        if (anyRowSent) {
            const size_t len = buildColorLightSyncPacket(packet_, kCardGain);
            // Same copy count as the brightness frame above, and this is the one that matters: a
            // second sync is a second LATCH, not a harmless repeat.
            for (int i = 0; i < frameCopies; i++) {
                if (platform::ethSendRaw(packet_, len)) framesSent_++;
                else framesDroppedTotal_++;
            }
        }
        // One wall frame is complete: hand the whole burst to the wire. Where a platform batches
        // (a Windows host, via the pcap send queue) this is the single call that transmits it,
        // and the cards need the burst inside one inter-frame window rather than trickled. A
        // no-op where each frame already went out as it was handed over.
        platform::ethFlushRaw();
    }

    /// Report what the wire is doing: no link, a link too slow for the cards, or the packet rate
    /// reaching it. A user debugging a dark panel needs to tell those apart. Sends regardless — the
    /// class note says why a slow link is reported rather than refused. Runs on the 1 Hz path, so
    /// the snprintf here is the same accepted trade SystemModule makes.
    void writeLinkStatus() MM_NONBLOCKING {
        // A failed restart outranks everything below: it is the one state a user cannot resolve by
        // plugging a cable back in. Cleared only when frames flow again (see the re-arm below).
        if (restartFailed_) {
            setStatus("ethernet restart failed - restart the device", Severity::Error);
            framesReported_ = framesSent_;
            return;
        }

        // The WEDGE is checked before the link, because a wedged transmit path is defined by sends
        // failing, which is knowable whatever the link claims. Checking the link first hid this
        // branch entirely on any platform reporting no link (the desktop stub among them), which is
        // how the recovery path shipped unreachable by its own tests.
        //
        // A short streak is ordinary back-pressure: the DMA ring fills while we push a whole frame
        // in one tick, the frame is dropped, the next one goes. Measured on a 128x128 wall at ~1900
        // packets/s, streaks of 1-4 come and go and always clear themselves — reporting those as an
        // error cries wolf and would bury the case below.
        //
        // A LONG streak is the wedge worth naming: esp_eth_transmit refuses every frame while the IDF
        // driver's own link flag reads down, which can outlive the PHY event that set it, and then
        // nothing recovers without a restart. The threshold sits above one frame's worth of packets,
        // so it can only be reached by failures spanning frames rather than one burst.
        static constexpr uint32_t kWedgedStreak = 500;
        const uint32_t failStreak = platform::ethSendFailStreak();
        if (failStreak >= kWedgedStreak) {
            if (!restartTried_) {
                restartTried_ = true;
                // A failed restart leaves the driver STOPPED: no CONNECTED event can follow, so the
                // link reads down forever and nothing else here can tell that apart from an unplugged
                // cable. Report it as its own error rather than letting it masquerade as one.
                if (platform::ethRestartTx()) {
                    setStatus("transmit wedged - restarting ethernet", Severity::Warning);
                } else {
                    // LATCHED: after a failed restart the driver is stopped, so no frame is sent, the
                    // streak stops growing, and the next tick would otherwise fall through to the
                    // link branch and report a plain "no ethernet link", indistinguishable from an
                    // unplugged cable, for a board that needs a power cycle.
                    restartFailed_ = true;
                    setStatus("ethernet restart failed - restart the device", Severity::Error);
                }
                framesReported_ = framesSent_;
                return;
            }
            std::snprintf(statusBuf_, sizeof(statusBuf_), "transmit wedged (%u refused, link %u Mbit)",
                          static_cast<unsigned>(failStreak),
                          static_cast<unsigned>(platform::ethLinkSpeedMbps()));
            setStatus(statusBuf_, Severity::Error);
            framesReported_ = framesSent_;
            return;
        }

        if (!platform::ethLinkUp()) {
            // On a host the same "link down" reads back for an unplugged cable AND for an `interface`
            // that matches no adapter, which is the far more common mistake and is invisible from the
            // status alone. Name the field in that case so the message carries its own fix; an ESP32
            // has one MAC and no such ambiguity, so it keeps the plain wording.
            if constexpr (platform::hasNamedNetInterfaces) {
                const char* boundName = platform::rawInterfaceName(interfaceSel_);
                if (!boundName) {
                    setStatus("no ethernet link - pick an 'interface' adapter", Severity::Warning);
                } else {
                    std::snprintf(statusBuf_, sizeof(statusBuf_),
                                  "no ethernet link - cable, or no adapter matches '%s'", boundName);
                    setStatus(statusBuf_, Severity::Warning);
                }
            } else {
                setStatus("no ethernet link", Severity::Warning);
            }
            framesReported_ = framesSent_;
            return;
        }
        const uint16_t mbps = platform::ethLinkSpeedMbps();
        // Total frames the MAC refused since boot. A dropped frame is tolerated (the cards have no
        // acknowledgement, so a retry would cost the next frame instead), but it is NOT invisible:
        // a rising total says the sender is outrunning the wire, which is a real thing to know when
        // a wall looks like it is stuttering. Shown only once something has actually dropped.
        const uint32_t dropped = framesDroppedTotal_;
        // Frames actually handed to the MAC since the last report — the one number that separates
        // "the driver is not sending" from "the driver sends and the card ignores it".
        const uint32_t sent = framesSent_ - framesReported_;
        framesReported_ = framesSent_;
        if (mbps && mbps < 1000) {
            std::snprintf(statusBuf_, sizeof(statusBuf_),
                          "%u Mbit (needs 1 Gbit) - %u packets/s",
                          static_cast<unsigned>(mbps), static_cast<unsigned>(sent));
            setStatus(statusBuf_, Severity::Warning);
            return;
        }
        // Re-arm the one-shot only on evidence of FLOW: frames sent this second and no failure
        // streak at all. Re-arming merely because the link reads healthy would fire again while a
        // wedge is still rebuilding its streak, bouncing the interface every ~20 s: the loop the
        // one-shot exists to prevent.
        if (sent > 0 && failStreak == 0) { restartTried_ = false; restartFailed_ = false; }

        if (dropped) {
            // Split by cause: a flapping link and a full TX ring are different faults with
            // different fixes, and one total cannot tell them apart.
            uint32_t linkDown = 0, ringFull = 0;
            platform::ethSendFailCounts(linkDown, ringFull);
            std::snprintf(statusBuf_, sizeof(statusBuf_),
                          "%u Mbit - %u pkt/s, %u lost (%u link, %u ring)",
                          static_cast<unsigned>(mbps), static_cast<unsigned>(sent),
                          static_cast<unsigned>(dropped),
                          static_cast<unsigned>(linkDown), static_cast<unsigned>(ringFull));
        } else {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%u Mbit - %u packets/s",
                          static_cast<unsigned>(mbps), static_cast<unsigned>(sent));
        }
        setStatus(statusBuf_, Severity::Status);
    }

    /// Test-only accessor for the correction-applied buffer, pinning the no-allocation-in-loop
    /// contract (same public-for-tests convention as NetworkSendDriver::correctedBuffer).
    const Buffer& correctedBuffer() const { return corrected_; }

private:
    /// Build one row packet into packet_. Pixels are copied one at a time because the source stride
    /// (3 for RGB, 4 for RGBW) need not match the wire's 3 bytes — the card takes RGB, so a 4-channel
    /// correction contributes its first three channels and the white channel has nowhere to go.
    size_t packRow(uint16_t row, uint16_t pixelOffset, uint16_t pixelCount,
                   const uint8_t* src, uint8_t stride) MM_NONBLOCKING {
        if (stride == COLORLIGHT_BYTES_PER_PIXEL) {
            // Dense already: one memcpy inside the builder.
            return buildColorLightRowPacket(packet_, row, pixelOffset, pixelCount, src);
        }
        // Strided source: write the header with no data, then pack RGB out of each light.
        buildColorLightRowPacket(packet_, row, pixelOffset, pixelCount, nullptr);
        uint8_t* dst = packet_ + COLORLIGHT_ROW_PREFIX;
        for (uint16_t i = 0; i < pixelCount; i++) {
            dst[i * 3 + 0] = src[i * stride + 0];
            dst[i * 3 + 1] = src[i * stride + 1];
            dst[i * 3 + 2] = src[i * stride + 2];
        }
        return COLORLIGHT_ROW_PREFIX + static_cast<size_t>(pixelCount) * COLORLIGHT_BYTES_PER_PIXEL;
    }

    /// Size corrected_ for the current source and correction. Called only off the hot path — that
    /// placement IS the no-allocation-in-the-send-loop contract.
    void resizeCorrected() {
        if (!sourceBuffer_) return;
        nrOfLightsType winStart, n;
        windowSlice(sourceBuffer_->count(), winStart, n);
        const uint8_t ch = correction_.outChannels;
        if (n == 0 || ch == 0) return;
        if (corrected_.count() >= n && corrected_.channelsPerLight() >= ch) return;
        corrected_.allocate(n, ch);
    }

    /// The shared frame this driver reads its window from; borrowed, not owned.
    Buffer* sourceBuffer_ = nullptr;
    /// Owned: source bytes after brightness/order/white. Sized off the hot path.
    Buffer corrected_;
    /// One reused frame buffer, sized for the largest packet the format builds. Static per driver
    /// rather than per send, which is what keeps tick() allocation-free.
    uint8_t packet_[COLORLIGHT_MAX_FRAME] = {};
    /// millis() of the last frame sent — the `fps` limiter's reference.
    uint32_t lastSendTime_ = 0;
    /// Frames the MAC accepted since boot, and the value at the last status write — their
    /// difference is the per-second rate the card shows.
    uint32_t framesSent_ = 0;
    /// framesSent_ at the last status write — subtracting gives the rate without a timer.
    uint32_t framesReported_ = 0;
    /// Frames the MAC refused since boot. Cumulative on purpose: the per-second rate hides a slow
    /// trickle of drops, and a rising total is the signal that the sender is outrunning the wire.
    uint32_t framesDroppedTotal_ = 0;
    /// Backing store for the status line (setStatus does not copy). Sized for the longest one: the
    /// split-drop report, which carries three cumulative counters and reaches ~63 chars at millions
    /// of drops. Truncation would cut the RING count, the number the line exists to show.
    char statusBuf_[96] = {};
    /// Whether a restart has already been attempted for the CURRENT wedge. One attempt per wedge:
    /// a restart cannot fix an unplugged cable, and retrying every second would bounce the interface
    /// under the user. Cleared as soon as frames flow again.
    bool restartTried_ = false;
    /// Set when a recovery attempt itself failed, which leaves the interface stopped. Latched so the
    /// error cannot be overwritten by the softer link warning on the next tick.
    bool restartFailed_ = false;
    /// Whether this driver currently holds the raw-L2 claim, so prepare/release stay balanced
    /// however often the framework calls them.
    bool claimed_ = false;
};

}  // namespace mm
