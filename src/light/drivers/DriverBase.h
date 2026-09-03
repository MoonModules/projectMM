#pragma once

// Include this one file to write a driver: it brings DriverBase plus the output-buffer, correction, and
// platform pieces a driver needs to push the finished image to hardware or the network, so a driver is a
// single include:
//
//   #pragma once
//   #include "light/drivers/DriverBase.h"
//   namespace mm {
//   class MyDriver : public DriverBase { ... };
//   }
//
// DriverBase reads the source buffer (Layer/Buffer) and applies the output Correction over the platform
// layer, plus the small standard headers every driver's status / control-name / buffer handling uses. A
// hardware driver adds its peripheral seam (platform::rmt* / platform::parlio* / a socket); a network
// driver adds its packet header — those stay per-driver, since they differ by transport.

#include "core/MoonModule.h"
#include "core/ScratchBuffer.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/drivers/Correction.h"
#include "light/drivers/LedPeripheral.h"         // LedHwBlock — the peripheral-block claim guard's vocabulary
#include "light/drivers/LightPresetsModule.h"   // the shared preset library a driver references by id
#include "platform/platform.h"

#include <cstdio>       // std::snprintf for status strings
#include <cstring>     // std::strcmp (onControlChanged) / memset (buffer clears)
#include <cstdint>    // fixed-width ints
#include <algorithm> // std::min / max / clamp (chunk loops, size clamps)

namespace mm {

/// Base class for one driver — a consumer that reads the shared source buffer and
/// emits it to a destination (a physical LED output, a network sink, or the preview).
///
/// A driver optionally reads dimensions from an active Layer, optionally applies the
/// shared output correction, and optionally restricts its output to a contiguous
/// *window* of the source buffer (see `addWindowControls`). It plays the same
/// zero-state role for drivers that EffectBase does for effects.
class DriverBase : public MoonModule {
public:
    /// A driver must not be destroyed while its container's worker thread might still tick it — that
    /// is a vptr race (the destructor rewrites the vtable while the worker calls through it). A base
    /// destructor CANNOT prevent it: C++ destroys derived-before-base, so by the time ~DriverBase runs
    /// the derived vptr is already gone. The guarantee therefore lives with the OWNER, which must
    /// release()/removeChild() before destroying — production always does (the HTTP delete path calls
    /// release() then deleteTree; removeChild() quiesces). A stack-declared driver in a test must be
    /// declared BEFORE its Drivers, so reverse-declaration order destroys the container (and stops its
    /// worker) first. TSan enforces this.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Driver; }
    virtual void setSourceBuffer(Buffer* buf) = 0;

    /// The hardware peripheral block this driver drives, for the parallel-driver claim guard (two live
    /// drivers on one block corrupt each other). Only ParallelLedDriver overrides it; every other driver
    /// (RMT, NetworkSend, Hue, Preview) drives no shared parallel block and keeps None. Virtual, not
    /// RTTI — ESP32 builds compile -fno-rtti, so the guard reads siblings through this, never a cast.
    virtual LedHwBlock hwBlock() const { return LedHwBlock::None; }

    /// Template method: every driver card leads with the per-driver output correction
    /// (localBrightness / lightPreset / whiteMode / Custom offsets), added once here in the base
    /// so no driver re-implements the placement (the No-duplication rule) — then the driver's own
    /// controls via defineDriverControls(). A driver overrides defineDriverControls(), NOT this.
    /// A driver that emits raw RGB and ignores correction (Preview) or fixes it internally (Hue)
    /// opts out by returning false from hasCorrectionControls().
    void defineControls() final {
        if (hasCorrectionControls()) defineCorrectionControls();
        defineDriverControls();
    }

    /// A driver's own controls (its pins, protocol, IP, window, …), added AFTER the correction
    /// block. This is what a driver overrides instead of defineControls(). Default: none.
    virtual void defineDriverControls() {}

    /// Whether this driver exposes the per-driver correction controls. True for physical drivers
    /// that apply the reorder/white/brightness; false for Preview (shows the raw logical buffer)
    /// and Hue (RGB-fixed, converts to HSV — a reorder would corrupt the hue).
    virtual bool hasCorrectionControls() const { return true; }
    /// Set the active Layer this driver reads dimensions from. Optional: drivers that
    /// need dimensions (such as PreviewDriver describing the LED grid in the WebSocket
    /// frame) call `layer_` for current physical width/height/depth; ArtNet doesn't need
    /// it — it just streams bytes.
    ///
    /// This is the *active* layer for dimension queries, not a 1:1 wiring constraint:
    /// each driver outputs to a single physical fixture, and the Drivers container hands
    /// it one Layer for dimensions regardless of how many layers feed the output buffer.
    void setLayer(Layer* layer) { layer_ = layer; }

    /// First light of the configured window. Public for tests pinning the slice
    /// arithmetic; production reads via `windowSlice()`. See `start_`/`count_` below.
    uint16_t windowStart() const { return start_; }
    /// Number of lights in the configured window (0 = to end of buffer).
    uint16_t windowCount() const { return count_; }
    /// Set the window directly (the UI sets it via the start/count controls; this is
    /// for code-wiring a driver's slice and for tests). Takes effect on the next config
    /// parse / loop, like a control edit.
    void setWindow(uint16_t start, uint16_t count) { start_ = start; count_ = count; }
    /// Resolve the window against a buffer of `bufN` lights → its length (0 if it starts past the
    /// end). Public thin wrapper over the protected windowSlice, so a test can pin the slice math
    /// directly (e.g. the default kWindowAll drives ALL lights on a >65535 buffer). Same public-for-
    /// tests convention as windowStart()/windowCount().
    nrOfLightsType resolveWindowLenForTest(nrOfLightsType bufN) const {
        nrOfLightsType outStart = 0, outLen = 0;
        windowSlice(bufN, outStart, outLen);
        return outLen;
    }
    /// The active Layer this driver reads dimensions from — null when no Layer is wired
    /// (such as after the last Layer was deleted). Drivers must tolerate null here.
    Layer* layer() const { return layer_; }

    /// Rebuild this driver's own output correction, baking the global brightness the
    /// Drivers container passes together with this driver's local brightness into one LUT
    /// (`global × local`, both 0..255). Called by Drivers on setup and on a global
    /// brightness / power change, and by the driver itself when one of its own correction
    /// controls (lightPreset, whiteMode, offsets, localBrightness) changes. The multiply
    /// happens once here on the cold path, so the hot-path apply() stays a single LUT
    /// lookup per channel — the per-driver correction costs nothing extra per light.
    /// Preview-style drivers read the raw logical buffer and ignore correction; the
    /// couple of bytes of unused Correction they carry is cheaper than a virtual opt-out.
    void rebuildCorrection(uint8_t globalBrightness) {
        lastGlobalBrightness_ = globalBrightness;   // remembered for self-triggered rebuilds
        // The brightness LUT bakes global × local so BOTH sliders reach the output — a localBrightness
        // change alone re-scales the LUT here (its own onControlChanged routes through this), and a
        // global change from the container does too. Applied unconditionally, so brightness works even
        // if the preset library isn't up yet (the roles then stay at correction_'s default until the
        // library resolves). One cold-path multiply; the hot path is unchanged.
        const uint8_t effective =
            static_cast<uint8_t>((globalBrightness * localBrightness_) / 255);
        correction_.whiteMode = static_cast<WhiteMode>(whiteMode_);
        correction_.curve = static_cast<Correction::Curve>(curveSel_);
        // Resolve the referenced preset's channel-role wiring from the library into our flat
        // Correction (cold path). On a missing id (a deleted preset) fall back to the library default
        // so a driver degrades to a valid RGB output rather than crashing — Robust-to-any-input.
        if (auto* lib = LightPresetsModule::active()) {
            if (presetId_ == 0) presetId_ = lib->defaultId();
            if (!lib->deriveCorrection(presetId_, effective, correction_)) {
                presetId_ = lib->defaultId();                       // dangling → re-point to default
                lib->deriveCorrection(presetId_, effective, correction_);
            }
        } else {
            // No library yet (early boot / a unit test without one): still apply brightness, so the
            // LUT reflects the sliders. deriveCorrection would have rebuilt the LUT; do it here too.
            correction_.rebuildBrightness(effective);
        }
        onCorrectionChanged();      // let a driver resize its correction-applied buffer
    }

    /// Base onControlChanged: rebuild this driver's correction when one of ITS correction
    /// controls (order/white/brightness/Custom offsets) changed, reusing the last global
    /// brightness the container pushed. A driver overriding onControlChanged() for its own
    /// controls chains to this so it doesn't re-implement the correction branch (the
    /// Complexity-lives-in-core rule: the correction rule lives here, once, for every driver).
    void onControlChanged(const char* name) override {
        // A lightPreset Select change: map the chosen INDEX to a stable preset id (what we store +
        // persist), so the reference survives a later reorder/delete of other presets.
        if (std::strcmp(name, "lightPreset") == 0) {
            if (auto* lib = LightPresetsModule::active()) {
                presetId_ = lib->idAt(presetSel_);
                std::snprintf(presetRef_, sizeof(presetRef_), "%s", lib->nameAt(presetSel_));  // persist the name
            }
        }
        if (isCorrectionControl(name)) rebuildCorrection(lastGlobalBrightness_);
        MoonModule::onControlChanged(name);
    }

    /// Notified when this driver's Correction outChannels may have changed (a lightPreset
    /// switch RGB↔RGBW, or a Custom channel-count edit). Default no-op; physical drivers
    /// that own an intermediate correction-applied buffer override to resize it OFF the
    /// hot path. Topology changes (light count) already flow through prepare — this hook
    /// is just for the preset-driven channel-count change that doesn't rebuild structure.
    virtual void onCorrectionChanged() {}

    /// Clear every shared status string on release — fail buffer, config error, and config
    /// warning — so a stopped driver leaves nothing behind (frees the owned `failBuf_`; retracts
    /// the warning the same "clear only MY status" way as the error), then chain to
    /// MoonModule::release() so any ScratchBuffer the driver holds frees on disable and children
    /// recurse. A driver that overrides release() for its own peripheral cleanup chains to this
    /// afterwards: `deinit(); DriverBase::release();`.
    void release() override {
        freeWire();          // the shared correction scratch — owned here, so no driver re-frees it
        clearFailBuf();
        clearConfigErr();
        setConfigWarn(nullptr);
        MoonModule::release();
    }

    /// Test-only: the driver's own Correction, mutable so a test can set a wiring directly (via the
    /// `mm::test::rebuildFromPreset` helper, or `rebuild(255, roles, n)`) instead of going through the
    /// container. Production wiring rebuilds via rebuildCorrection(); this is the injection
    /// point the encoder/packet tests use in place of the removed shared-pointer setter.
    Correction& correctionForTest() { return correction_; }

    /// The resolved correction, so the light domain can learn where this fixture keeps its motion
    /// channels. Read-only: the driver owns it and rebuilds it when the preset or sliders change.
    const Correction& correction() const { return correction_; }

    /// Park or release this driver's motion: the ONE correction field the container sets directly.
    /// Everything else in there is derived by rebuildCorrection from the preset and the global
    /// brightness, and a caller reaching in to change those would be overwritten by the next
    /// rebuild. The hold is different: it is a transmission decision the Drivers container owns and
    /// re-asserts every second, so it is set rather than derived. Narrow by construction rather than
    /// by docstring: handing out the whole Correction let a caller mutate a derived field too.
    void setMotionHeld(bool held) { correction_.motionHeld = held; }
    bool motionHeld() const { return correction_.motionHeld; }

protected:
    Layer* layer_ = nullptr;

    // --- Shared per-row correction scratch (the buffer `correction_.apply()` writes into) ---
    // Every physical driver needs a small heap buffer to hold one light's (or one row's) corrected
    // wire bytes before it encodes them for its peripheral. The SIZE differs per driver — RmtLedDriver
    // needs `outChannels` (one light), ParallelLedDriver needs `kMaxLanes × outChannels` (one row across
    // every lane) — so the caller passes the byte count; the grow-only allocate/free lifecycle is the
    // same, and lives here once. Grow-only: a shrink keeps the larger block rather than churning the
    // heap on a channel-count flip. Allocation failure leaves `wire_` null and `wireCap_` 0, which each
    // driver's tick() already treats as "not ready" and idles — never a null deref (the robustness rule).
    // Cold path only (prepare / a preset channel-count change), never the render loop.
    uint8_t* wire_ = nullptr;
    size_t   wireCap_ = 0;   // bytes actually allocated (0 when the allocation failed)

    /// Grow `wire_` to at least `bytes`. Keeps the existing block when it is already big enough.
    /// Leaves `wire_` null on allocation failure (the caller's tick() then idles).
    /// Internal-RAM-first: this scratch is the encoder's hottest data — written AND read back per light
    /// (a parallel encoder touches ~100 of its bytes per light), so a PSRAM-resident block multiplies
    /// the whole encode by PSRAM latency. It is tiny (≤ kMaxStrands × channels), so internal always
    /// has room; plain alloc() is only the degraded fallback.
    void ensureWire(size_t bytes) {
        if (wire_ && wireCap_ >= bytes) return;
        freeWire();
        wire_ = static_cast<uint8_t*>(platform::allocInternal(bytes));
        if (!wire_) wire_ = static_cast<uint8_t*>(platform::alloc(bytes));
        wireCap_ = wire_ ? bytes : 0;
        publishHeapBytes();   // the scratch grew — refresh the memory readout
    }
    /// Release the scratch (on the true teardown — release(), not a mid-life reinit).
    void freeWire() {
        if (wire_) { platform::free(wire_); wire_ = nullptr; wireCap_ = 0; publishHeapBytes(); }
    }

    // --- Driver heap accounting (the per-module memory readout) ---
    // A driver owns several raw-alloc buffers off the ScratchBuffer path — the correction scratch, the
    // streaming snapshot, the RMT symbol buffer, the preview stage. `ScratchBuffer` auto-accounts its
    // own resizes, but these raw allocs don't, so without this they were INVISIBLE to dynamicBytes()
    // (a 36 KB snapshot reading as 0). Rather than paste setDynamicBytes into every alloc site (the
    // per-module duplication CLAUDE.md forbids — one forgotten site and the readout drifts), each driver
    // reports its live heap total HERE, and DriverBase publishes it once. driverHeapBytes() is the one
    // hook a driver overrides to sum its buffers; publishHeapBytes() pushes that sum to the module readout
    // (setDynamicBytes, the same channel Drivers uses for its output buffer). Called on every (re)alloc
    // and free — all cold-path — so the readout tracks the real footprint with no per-site bookkeeping.
    //
    // Base sum: the shared correction scratch, the one buffer DriverBase itself owns. A subclass override
    // ADDS its own buffers and chains to this (`DriverBase::driverHeapBytes() + snapshotCap_ + …`).
    virtual size_t driverHeapBytes() const { return wireCap_; }
    void publishHeapBytes() { setDynamicBytes(driverHeapBytes()); }

    // --- Per-driver output correction (references a shared preset; brightness + white are local) ---
    // Each physical driver owns its Correction — the flat hot-path cache apply() reads — but the
    // channel-role WIRING comes from a NAMED PRESET in the LightPresets library, referenced by a
    // stable id. A driver stores only presetId_ (+ its own localBrightness / whiteMode); it holds no
    // role array. rebuildCorrection() resolves the id via LightPresetsModule::active()->deriveCorrection
    // ONCE on the cold path into correction_; the render loop never touches the library. So building a
    // wiring once (in the LightPresets card) makes it reusable across every driver, and a preset's
    // brightness/order still costs exactly one LUT lookup per channel per light.
    Correction correction_;
    uint32_t presetId_ = 0;          // stable id into the LightPresets library (0 → resolve to default)
    uint8_t presetSel_ = 0;          // the preset Select's chosen INDEX (mapped to an id in onControlChanged)
    uint8_t whiteMode_ = static_cast<uint8_t>(WhiteMode::Min);  // index into kWhiteModeOptions
    uint8_t localBrightness_ = 255;  // per-driver dim, multiplied with the global brightness
    /// Which perceptual curve the output LUT is built through. CIE 1931 lightness by default: it is
    /// the standards answer (CIE 15 / ISO 11664-4) and models the thing actually being corrected,
    /// the eye's response to luminance. hzeller's rpi-rgb-led-matrix builds its HUB75 table the same
    /// way. The alternatives are offered because curve choice is a legitimate preference, and each
    /// is named for what it really does rather than presented as equivalent.
    uint8_t curveSel_ = 0;           // index into kCurveOptions; 0 = CIE
    static constexpr uint8_t kCurveCount = 4;
    static constexpr const char* kCurveOptions[kCurveCount] = {
        "CIE 1931",        // perceptual, the standard
        "gamma 2.2",       // the DISPLAY convention (sRGB's effective exponent)
        "gamma 2.8",       // the STAGE convention: emulates a tungsten dimmer's feel
        "linear"           // no curve: for a downstream device that corrects its own output
    };
    uint8_t lastGlobalBrightness_ = 0;  // last global brightness the container pushed (for self-rebuilds)
    // The PERSISTED reference: a preset id is a runtime handle (reassigned each boot), so what
    // survives a reboot is the preset NAME — stable, human-readable, and reorder-proof. A hidden
    // persistable text control round-trips it; on load, rebuildCorrection resolves name → live id.
    char presetRef_[16] = {};        // referenced preset's name (the durable reference)

    /// Add the correction controls: localBrightness first (the setting a user reaches for most),
    /// then the preset Select (its options are the LightPresets library's names; the chosen index
    /// maps to a stable preset id), then whiteMode. A driver calls this from its defineDriverControls
    /// via the DriverBase::defineControls template method. The Select is rebuilt from the library on
    /// every defineControls (which re-runs on a control change), so adding/renaming a preset shows up.
    void defineCorrectionControls() {
        controls_.addControl("localBrightness", localBrightness_, 0, 255);
        // The perceptual curve, per driver rather than global, because whether one belongs depends
        // on what is DOWNSTREAM: a panel card or fixture that corrects its own pixels needs Linear
        // here, or the picture is corrected twice and darkens as the square of the setting.
        controls_.addSelect("curve", curveSel_, kCurveOptions, kCurveCount);
        buildPresetOptions();                        // fill presetOptions_ from the library, sync id/sel/ref
        controls_.addSelect("lightPreset", presetSel_, presetOptions_, presetOptionCount_);
        controls_.addSelect("whiteMode", whiteMode_, kWhiteModeOptions, kWhiteModeCount);
        // whiteMode only applies when the REFERENCED preset carries a channel apply() synthesises
        // from RGB (White / WarmWhite / Yellow / UV) — there's nothing to synthesise on a plain
        // RGB/GRB strip, so hide it otherwise. defineControls re-runs on a preset change, so this
        // tracks the live reference.
        auto* lib = LightPresetsModule::active();
        controls_.setHidden(controls_.count() - 1, !(lib && lib->presetHasSynthChannel(presetId_)));
        // The durable reference (the preset NAME) persists but isn't shown — the lightPreset Select
        // above is the user-facing control; presetRef_ just carries the reference across a reboot.
        controls_.addText("presetRef", presetRef_, sizeof(presetRef_));
        controls_.setHidden(controls_.count() - 1, true);
    }

    /// A driver sets its default referenced preset by NAME in its constructor (e.g. a WS2812 LED
    /// driver "GRB", a network sink / Hue "RGB"). Until the library resolves it to a live id
    /// (buildPresetOptions), this is the durable reference; a user can still pick any preset.
    void setDefaultPresetName(const char* name) { std::snprintf(presetRef_, sizeof(presetRef_), "%s", name); }

    /// True if `name` is one of the correction controls — a driver folds this into its
    /// affectsPrepare() and its correction rebuilds in onControlChanged (both handled by DriverBase).
    static bool isCorrectionControl(const char* name) {
        return std::strcmp(name, "lightPreset") == 0 || std::strcmp(name, "localBrightness") == 0
            || std::strcmp(name, "whiteMode") == 0 || std::strcmp(name, "curve") == 0;
    }

private:
    // The preset Select's options: pointers to the library's preset names, refreshed each
    // defineControls. addSelect borrows these pointers, so they must outlive the control list — the
    // library owns the name storage (it lives for the device's life), and this array of borrowed
    // pointers is a stable member. Also (re)syncs presetSel_ to the row that currently holds presetId_
    // so the Select renders at the referenced preset even after a reorder.
    const char* presetOptions_[LightPresetsModule::kMaxPresets] = {};
    uint8_t presetOptionCount_ = 0;
    void buildPresetOptions() {
        presetOptionCount_ = 0;
        auto* lib = LightPresetsModule::active();
        if (!lib) { presetOptions_[0] = "(none)"; presetOptionCount_ = 1; presetSel_ = 0; return; }
        const uint8_t n = lib->presetCount();
        for (uint8_t i = 0; i < n; i++) presetOptions_[i] = lib->nameAt(i);
        presetOptionCount_ = n;
        // Reconcile the three representations (Select index / stable id / persisted name), preferring
        // whichever is the freshest source of truth in this rebuild:
        //   1. A USER PICK: the Select index points at a valid preset whose id differs from ours —
        //      the user just chose it. Adopt the Select's id. (This rebuild runs BEFORE
        //      onControlChanged in Scheduler::setControl, so it MUST honour the new Select value, or
        //      the pick is reverted before onControlChanged can read it.)
        //   2. Otherwise the persisted NAME (survives reboot) resolves to an id.
        //   3. Else the current id, or the library default for a fresh driver.
        // Then sync all three so they agree.
        // `presetId_ == 0` is the fresh-driver default (no real preset has id 0), NOT a user pick — a
        // Select index at 0 whose preset has a nonzero id would otherwise look like a "pick" and shadow
        // the persisted NAME (e.g. a constructor default like "GRB" stored in presetRef_). So only treat
        // a nonzero presetId_'s mismatch as a user pick; a zero id falls through to the presetRef_ path.
        if (presetSel_ < n && presetId_ != 0 && lib->idAt(presetSel_) != presetId_) {
            presetId_ = lib->idAt(presetSel_);                 // user picked a different preset
        } else if (presetRef_[0]) {
            for (uint8_t i = 0; i < n; i++)
                if (std::strcmp(lib->nameAt(i), presetRef_) == 0) { presetId_ = lib->idAt(i); break; }
        }
        if (presetId_ == 0) presetId_ = lib->defaultId();
        presetSel_ = lib->indexOfId(presetId_);
        std::snprintf(presetRef_, sizeof(presetRef_), "%s", lib->nameAt(presetSel_));
    }

protected:

    // --- Shared source-buffer window (start, count) ---------------------------
    /// Every driver reads the SAME shared source buffer (Drivers hands the one
    /// `Buffer*` to each child) and outputs a contiguous slice of it: lights
    /// `[start_, start_+count_)`. This is how light distribution is made *explicit*
    /// and order-independent — a second driver on a different slice (such as an
    /// onboard status LED at index 0, the main strip from index 1) just sets its
    /// own window, rather than the buffer being split by driver order. The default
    /// `count_` is the uint16 max (65535), which `windowSlice` clamps to the buffer
    /// length — so a fresh driver's window is "all lights" without a magic 0. A user
    /// setting a real window types a real number; there's no confusing "0 = all".
    /// NetworkSendDriver's universe maps onto the same window; the LED drivers'
    /// pins/ledsPerPin distribute lights *within* the window. It is the alternative
    /// to a "split the buffer by sibling order" model some controllers use — here the
    /// user (or catalog) says which slice goes where.
    static constexpr uint16_t kWindowAll = 65535;  ///< count default: clamped to buffer length = all lights
    uint16_t start_ = 0;   ///< First source-buffer light this driver outputs (default 0).
    uint16_t count_ = kWindowAll;   ///< Lights from start_; default kWindowAll (clamped to buffer = all).

    /// Add the two window controls — call from a driver's defineControls(). Kept a
    /// helper (not auto-added) so a driver opts in by calling it where its other
    /// controls go, keeping control *order* in the driver's hands.
    void addWindowControls() {
        controls_.addControl("start", start_);
        controls_.addControl("count", count_);
    }

    /// True if `name` is one of the window controls — a driver folds this into its
    /// affectsPrepare() so editing the slice re-runs its config.
    static bool isWindowControl(const char* name) {
        return std::strcmp(name, "start") == 0 || std::strcmp(name, "count") == 0;
    }

    /// Resolve the window against a buffer of `bufN` lights: writes the clamped first
    /// light to `outStart` and the slice length to `outLen` (0 if the window starts
    /// past the end — the driver then idles, no out-of-bounds read). The textbook
    /// `[start, start+count)` clamp — every driver calls this instead of reading from
    /// light 0.
    void windowSlice(nrOfLightsType bufN, nrOfLightsType& outStart,
                     nrOfLightsType& outLen) const {
        outStart = start_ < bufN ? start_ : bufN;
        const nrOfLightsType avail = static_cast<nrOfLightsType>(bufN - outStart);
        // "All lights" is either sentinel: the default kWindowAll (65535), OR 0 (a zero-length
        // window would drive nothing, never the intent — so setWindow(start, 0) still means "to
        // the end"). kWindowAll must be matched EXPLICITLY, not via `count_ > avail`: on a PSRAM
        // board nrOfLightsType is uint32, so a buffer can exceed 65535 lights, and treating the
        // default as a literal 65535 count would silently cap output there. An explicit real count
        // (< kWindowAll) always clamps to the buffer, unchanged.
        outLen = (count_ == 0 || count_ == kWindowAll || count_ > avail)
                     ? avail
                     : static_cast<nrOfLightsType>(count_);
    }

    // --- Shared status-string lifecycle for the physical LED drivers (RMT / LCD /
    // Parlio). They report two kinds of transient status that must clear cleanly
    // without stomping an unrelated status set by something else:
    //   configErr_ — a borrowed string literal (a parse-error message);
    //   failBuf_   — an owned, on-demand char buffer (a formatted loopback/init
    //                failure with numbers in it).
    // Both follow the same "clear only MY status" rule: only call clearStatus() if
    // the status currently shown is the one this driver set. This was triplicated
    // verbatim across the three drivers; it lives here once (the No-duplication
    // rule). Preview-style drivers never touch these, so the cost is a couple of
    // null pointers they ignore.
    const char* configErr_ = nullptr;
    const char* configWarn_ = nullptr;
    char* failBuf_ = nullptr;
    // 64: the widest verdict is the loopback's worst-case "bad bit N/M (light K)" with
    // full-range counters — GCC's -Wformat-truncation proves 48 can clip it.
    static constexpr size_t kFailBufLen = 64;

    // Record a parse/config error: set the status and remember it so clearConfigErr
    // can later retract exactly this one.
    void setConfigErr(const char* err) {
        configErr_ = err;
        setStatus(err, Severity::Error);
    }
    void clearConfigErr() {
        if (configErr_) {
            if (status() == configErr_) clearStatus();
            configErr_ = nullptr;
        }
    }

    // A non-fatal config warning (a borrowed literal, like configErr_): the driver keeps
    // running but flags something the user should see (e.g. a per-pin count clamped to the
    // WS2812 ceiling). `warn` non-null sets it; null retracts a stale one — so a driver calls
    // setConfigWarn(warn) unconditionally each parse and the warning tracks the live state.
    // Same "clear only MY status" rule as configErr_. Factored here so the WS2812 drivers
    // (Rmt / LCD / Parlio) don't each inline it — the No-duplication rule this block records.
    void setConfigWarn(const char* warn) {
        if (warn) {
            configWarn_ = warn;
            setStatus(warn, Severity::Warning);
        } else if (configWarn_) {
            if (status() == configWarn_) clearStatus();
            configWarn_ = nullptr;
        }
    }

    // Lazily allocate the owned fail-message buffer (caller snprintf's into it then
    // setStatus(failBuf_)). Returns null if the allocation fails, in which case the
    // caller falls back to a literal status.
    char* failBufEnsure() {
        if (!failBuf_) failBuf_ = static_cast<char*>(platform::alloc(kFailBufLen));
        return failBuf_;
    }
    void clearFailBuf() {
        if (failBuf_) {
            if (status() == failBuf_) clearStatus();
            platform::free(failBuf_);
            failBuf_ = nullptr;
        }
    }

    // Neutral "driving N of M lights" info status — the running-state readout the user reads to
    // see what a driver actually consumes, instead of guessing from grid × pins. `channels` is the
    // bytes-per-light (correction outChannels); when > 1 the message also reports the total channel
    // count (driven × channels) — the number that matters for a multi-channel fixture (a 15-channel
    // moving head drives far more channels than lights, and that's the real footprint the user sizes
    // a DMX universe against). Formats into the owned failBuf_ (same buffer, same "clear only MY
    // status" lifecycle) at Severity::Status, so an error/warning set elsewhere this parse still wins
    // (callers set info LAST, only when there's nothing more urgent to show). No-op if the buffer
    // can't allocate. The format literal lives here (not a caller-passed fmt) so the message is
    // defined once and both drivers share it verbatim.
    // `mode` is an optional one-word transport qualifier a driver may append — e.g. the streaming ring's
    // "primed" (whole frame encoded before arming: no deadline, pixel-perfect at any load) vs "lapping"
    // (the ISR refills behind the DMA: the deadline regime) — so the user can see which side of a
    // behavioral boundary a config sits on without reading a diagnostic control. Empty/null = no suffix.
    void setDrivingInfo(unsigned driven, unsigned total, unsigned channels = 1,
                        const char* mode = nullptr) {
        if (char* buf = failBufEnsure()) {
            int n;
            if (channels > 1)
                n = std::snprintf(buf, kFailBufLen, "driving %u of %u lights (%u channels)",
                                  driven, total, driven * channels);
            else
                n = std::snprintf(buf, kFailBufLen, "driving %u of %u lights", driven, total);
            if (mode && mode[0] && n > 0 && static_cast<size_t>(n) < kFailBufLen)
                std::snprintf(buf + n, kFailBufLen - static_cast<size_t>(n), ", %s", mode);
            setStatus(buf, Severity::Status);
        }
    }
};

} // namespace mm
