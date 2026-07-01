#pragma once

#include "light/effects/EffectBase.h"
#include "light/layers/Layer.h"      // layer() — the child renders into the same Layer buffer
#include "core/ModuleFactory.h"      // enumerate + create the effects to cycle through
#include "core/math8.h"              // Random8 — the shuffle pick

#include <cstring>                   // strcmp

namespace mm {

// Demo reel: cycles through every OTHER registered effect, one at a time, auto-advancing every
// `interval` seconds. It hosts a single live child effect — created from the ModuleFactory registry,
// parented to this effect's own Layer so the child's layer()/buffer()/width()/elapsed() resolve to
// the same render target — and delegates loop() to it each tick. On the interval it tears the child
// down, deletes it, and instantiates the next effect in the registry (or a random one when
// `shuffle`). This reuses the exact create → onBuildControls → setup → onBuildState → loop lifecycle
// a Layer runs for a normal effect child (HttpServerModule::applyAddModule), so no new machinery:
// the reel is just an effect that swaps which effect it *is* over time.
//
// It deliberately does NOT composite effects (that's what the Layer stack + blend modes already do);
// it plays them in sequence — the FastLED DemoReel100 / WLED preset-cycle pattern. The child's own
// controls aren't surfaced (they run at their defaults); the reel exposes only the cycle controls.
//
// Prior art: FastLED's DemoReel100 sketch (Mark Kriegsman) — the canonical "rotate through a list of
// patterns on a timer" demo; the registry-driven, self-skipping variant is ours.
class DemoReelEffect : public EffectBase {
public:
    const char* tags() const override { return "🎬"; }   // demo reel
    // The hosted child may be any dimensionality; declare D3 so the framework never extrudes on the
    // reel's behalf — the child effect declares its own dimensions() and the Layer extrudes for it
    // when we run its loop() through the same path.
    Dim dimensions() const override { return Dim::D3; }

    uint8_t interval = 8;      // seconds each effect plays before advancing
    bool    shuffle  = false;  // random next-effect pick instead of registry order

    void onBuildControls() override {
        controls_.addUint8("interval", interval, 1, 120);
        controls_.addBool("shuffle", shuffle);
    }

    // Build the eligible-effect list (all Effect-role types except this one) and start the first.
    void onBuildState() override {
        buildEligibleList();
        // Restart the reel from the top on any rebuild (grid resize, control change): tear down the
        // current child (its buffers were sized to the old grid) and re-create against the new one.
        swapTo(cursor_ < eligibleCount_ ? cursor_ : 0);
    }

    void loop() override {
        if (eligibleCount_ == 0 || !current_) return;

        // Advance on the interval. elapsed() is the Layer's monotonic ms clock (same source every
        // effect uses), so the cadence is frame-rate-independent.
        const uint32_t now = elapsed();
        if (now - lastSwitchMs_ >= static_cast<uint32_t>(interval) * 1000u) {
            lastSwitchMs_ = now;
            advance();
        }

        current_->loop();   // render the hosted effect into our Layer's buffer this tick
    }

    void teardown() override {
        destroyCurrent();
        EffectBase::teardown();
    }

    ~DemoReelEffect() override { destroyCurrent(); }

    // Test seams.
    uint8_t eligibleCountForTest() const { return eligibleCount_; }
    const char* currentTypeForTest() const { return current_ ? current_->typeName() : nullptr; }
    void advanceForTest() { advance(); }

private:
    // The registry indices of every Effect-role type that isn't DemoReel itself. Bounded by the
    // registry size; stored inline (a byte per effect, a few dozen at most — no heap needed).
    static constexpr uint8_t kMaxEligible = 64;
    uint8_t eligible_[kMaxEligible] = {};
    uint8_t eligibleCount_ = 0;
    uint8_t cursor_ = 0;                 // index INTO eligible_ of the current effect
    MoonModule* current_ = nullptr;      // the live hosted effect (owned; deleted on swap/teardown)
    uint32_t lastSwitchMs_ = 0;
    Random8 rng_;

    void buildEligibleList() {
        eligibleCount_ = 0;
        const uint8_t n = ModuleFactory::typeCount();
        for (uint8_t i = 0; i < n && eligibleCount_ < kMaxEligible; i++) {
            if (ModuleFactory::typeRole(i) != ModuleRole::Effect) continue;
            const char* name = ModuleFactory::typeName(i);
            if (name && std::strcmp(name, "DemoReelEffect") == 0) continue;  // never host ourselves
            eligible_[eligibleCount_++] = i;
        }
    }

    // Move to the next effect: the following registry entry, or a random one when shuffle.
    void advance() {
        if (eligibleCount_ == 0) return;
        uint8_t next;
        if (shuffle && eligibleCount_ > 1) {
            do { next = rng_.below(eligibleCount_); } while (next == cursor_);  // don't repeat in place
        } else {
            next = static_cast<uint8_t>((cursor_ + 1) % eligibleCount_);
        }
        swapTo(next);
    }

    // Tear down the current child and stand up the effect at eligible_[which], wired to our Layer.
    void swapTo(uint8_t which) {
        destroyCurrent();
        if (which >= eligibleCount_) return;
        cursor_ = which;
        const char* typeName = ModuleFactory::typeName(eligible_[which]);
        MoonModule* mod = ModuleFactory::create(typeName);
        if (!mod) return;
        // Parent to the LAYER, not to us: EffectBase::layer() is static_cast<Layer*>(parent()), so
        // the child must see the Layer as its parent for buffer()/dims/elapsed() to resolve. We hold
        // it privately and drive its loop() ourselves — we do NOT addChild() it (that would make the
        // Layer tick it a second time). Same create→build lifecycle as a normal effect child.
        mod->setParent(layer());
        mod->onBuildControls();
        mod->setup();
        mod->onBuildState();
        current_ = mod;
        lastSwitchMs_ = elapsed();
        refreshStatus();
    }

    void destroyCurrent() {
        if (!current_) return;
        current_->teardown();
        delete current_;              // teardown-then-delete, the Scheduler's ownership pattern
        current_ = nullptr;
    }

    void refreshStatus() {
        // Show which effect is playing (its display name), e.g. "playing: Plasma (3/19)".
        if (current_) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "playing: %s (%u/%u)",
                          current_->name(), static_cast<unsigned>(cursor_ + 1),
                          static_cast<unsigned>(eligibleCount_));
            setStatus(statusBuf_);
        }
    }

    char statusBuf_[48] = {};
};

}  // namespace mm
