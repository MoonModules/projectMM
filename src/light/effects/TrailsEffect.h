#pragma once

#include "core/oscillators.h"        // OscillatorBank: the emitters' motion and the breathing
#include "light/effects/EffectBase.h"

namespace mm {

// Trails: dots thrown into a moving medium, leaving tails that the flow carries and bends.
//
// The composition is three power functions and nothing else: an EMITTER draws a few bright points,
// `draw::advect` carries the whole plane along a velocity field, and `draw::decay16` dims it by a
// half-life. Run every frame, that loop is what a trail IS. Nothing here paints a tail: the tail is
// last frame's dots, moved and dimmed, which is why the shape of the flow is visible in it.
//
// This is the advection idiom the way Aurora is the shader idiom. The vocabulary it demonstrates is
// transport rather than sampling: a field that says where the medium is going, applied to whatever
// happens to be there. Prior art: the flow-field family (4wheeljive's FlowFields, from a Stefan
// Petrick concept), and Stam's backward advection for the transport step itself.
//
// **The plane is 16-BIT, and that is the point.** A trail is a value multiplied by slightly less
// than one, hundreds of times a second. At 8 bits that either truncates to nothing (the tail dies
// early: measured, 48 of 64 cells) or rounds back up to where it started (the trail never fades and
// the effect turns solid). Both were measured; see `draw::decay`. So the plane the effect owns is
// wider than the layer it writes to, and narrows once on the way out. The precision belongs in the
// ACCUMULATOR, not in the frame buffer.
//
// Cost: one advect (a bilinear sample per light) plus one noise sample per light for the flow, so
// the flow rule dominates. Volumetric: on a cube every slice is carried, and the flow's third
// component moves the trail through the volume rather than repeating a plane.
// @card TrailsEffect.png
/// Effect: bright dots thrown into a flowing medium, leaving tails the flow carries and bends.
class TrailsEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D3; }   // volumetric: the flow moves through z

    static constexpr uint8_t kMaxDots = 8;

    uint8_t speed      = 40;   // how fast the medium moves, and with it every tail
    uint8_t dots       = 3;    // how many emitters are throwing light in
    uint8_t scale      = 30;   // the flow field's cell size: low = broad sweeps, high = eddies
    uint8_t persistence = 90;  // how long a tail survives, as a half-life (see halfLifeMs)
    uint8_t breathe    = 40;   // how much the flow's strength rises and falls

    void defineControls() override {
        controls_.addControl("speed", speed, 0, 255);
        controls_.addControl("dots", dots, 1, kMaxDots);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("persistence", persistence, 0, 255);
        controls_.addControl("breathe", breathe, 0, 255);
    }

    void prepare() override {
        // The plane is the effect's own state and holds THREE channels per light whatever the
        // layer's width is: a trail is about its own history, not about the fixture's wiring.
        const lengthType w = width(), h = height(), d = depth();
        const size_t needed = static_cast<size_t>(w) * h * d * 3;
        const size_t had = plane_.count();
        plane_.resize(needed);
        // Same sample count but a different shape (8x16 -> 16x8, or a cube reshaped): resize() kept
        // the old samples, and they are laid out for the old geometry, so they would smear. Clear.
        if (needed > 0 && needed == had && (w != planeW_ || h != planeH_ || d != planeD_))
            std::memset(plane_.data(), 0, plane_.bytes());
        scratch_.resize(needed);
        planeW_ = w; planeH_ = h; planeD_ = d;
    }

    void tick() MM_NONBLOCKING override {
        if (!plane_ || !scratch_) return;               // a zero grid, or an allocation that failed
        const lengthType w = width(), h = height(), d = depth();
        const uint32_t dt = elapsed() - lastMs_;
        lastMs_ = elapsed();

        // Two oscillators: one walks the emitters around, one breathes the flow's strength so the
        // composition swells and settles instead of running at one rate forever.
        bank_.set(0, {.rate = static_cast<uint16_t>(8 + speed / 8), .low = 0, .high = 65535,
                      .phaseOffset = 0, .wave = Wave::Saw});
        bank_.set(1, {.rate = 7, .low = static_cast<int32_t>(256 - breathe),
                      .high = static_cast<int32_t>(256 + breathe),
                      .phaseOffset = 0, .wave = Wave::Sine});
        bank_.advance(dt);

        // Which buffer currently HOLDS the trail alternates: a ScratchBuffer is deliberately fixed
        // to its module (non-movable, it owns a slot in the module's free list), so the ping-pong
        // swaps a flag rather than the buffers.
        ScratchBuffer<uint16_t>& src = front_ ? plane_ : scratch_;
        ScratchBuffer<uint16_t>& dst = front_ ? scratch_ : plane_;

        // 1. Transport: carry what is already there along the flow. Backward-sampled, so this both
        //    moves the trail and is what bends it, since neighboring pixels take different paths.
        const uint32_t t = elapsed();
        const uint32_t cells = static_cast<uint32_t>(scale) * 256u;
        const int32_t strength = static_cast<int32_t>(bank_.value(1));   // the breathing multiplier
        const uint32_t step = (static_cast<uint32_t>(speed) * dt) / 8u;  // sub-pixels this frame
        draw::advect16(dst.data(), src.data(), w, h, d,
                       [&](lengthType x, lengthType y, lengthType z,
                           draw::pos_t& vx, draw::pos_t& vy) {
                           flowAt(x, y, z, t, cells, strength, step, vx, vy);
                       }, draw::Edge::Clamp);
        front_ = !front_;                       // the destination now holds the trail
        ScratchBuffer<uint16_t>& moved = front_ ? plane_ : scratch_;

        // 2. Decay: a half-life, so the tail is the same length in SECONDS on any device. This is
        //    the step that needs the wide plane (draw::decay's own note has the measurements).
        draw::decay16(moved.data(), moved.count(), halfLifeMs(), dt);

        // 3. Emit: the bright heads, drawn after the transport so this frame's dots are sharp and
        //    only the previous ones have been carried.
        emitDots(moved.data(), w, h, d);

        // 4. Narrow onto the layer: the one place the wide plane meets the fixture's width.
        blit(moved.data(), w, h, d);
    }

private:
    /// The persistence control as a half-life in milliseconds. Deliberately not linear: the
    /// interesting range is short, and a slider that spends half its travel between four and eight
    /// seconds would waste it. 0 is a bare head with no tail at all.
    uint32_t halfLifeMs() const {
        return 20u + static_cast<uint32_t>(persistence) * static_cast<uint32_t>(persistence) / 16u;
    }

    /// The flow: where the medium is going at this point, in sub-pixels this frame.
    ///
    /// A noise field read at two offsets, one per axis, which is the decoupled form: reading ONE
    /// field for both axes moves everything along a diagonal, since the two components would rise
    /// and fall together. The third axis is sampled too, so a cube's slices flow differently rather
    /// than the same plane repeating through the volume.
    void flowAt(lengthType x, lengthType y, lengthType z, uint32_t t, uint32_t cells,
                int32_t strength, uint32_t step, draw::pos_t& vx, draw::pos_t& vy) const {
        const uint32_t fx = static_cast<uint32_t>(x) * cells;
        const uint32_t fy = static_cast<uint32_t>(y) * cells;
        const uint32_t fz = static_cast<uint32_t>(z) * cells + t / 4u;
        // Centered on zero, so the field pushes both ways rather than only along the axes.
        const int32_t nx = static_cast<int32_t>(inoise16(fx, fy, fz)) - 32768;
        const int32_t ny = static_cast<int32_t>(inoise16(fx + 0x9E37u, fy + 0x7C15u, fz)) - 32768;
        // strength is the breathing multiplier in 1/256ths; step is the frame's travel budget.
        const int32_t amp = static_cast<int32_t>(step) * strength / 256;
        vx = static_cast<draw::pos_t>((nx * amp) >> 15);
        vy = static_cast<draw::pos_t>((ny * amp) >> 15);
    }

    /// The heads: a few bright points walking their own paths, each on the palette.
    void emitDots(uint16_t* plane, lengthType w, lengthType h, lengthType d) {
        const uint8_t n = dots < 1 ? 1 : (dots > kMaxDots ? kMaxDots : dots);
        const uint32_t walk = bank_.unitValue(0);
        for (uint8_t i = 0; i < n; i++) {
            // Each dot rides the same clock at its own offset and its own ratio, so they never
            // bunch: a Lissajous walk, which visits the whole grid rather than circling one spot.
            const angle16 a = static_cast<angle16>(walk + i * (65536u / n));
            const angle16 b = static_cast<angle16>(walk * 3u + i * 9973u);
            const lengthType px = mapAxis(sin16(a), w);
            const lengthType py = mapAxis(cos16(b), h);
            const lengthType pz = d > 1 ? mapAxis(sin16(static_cast<angle16>(b * 2u)), d) : 0;
            const RGB c = colorFromPalette(*Palettes::active(),
                                           static_cast<uint8_t>(i * (255u / n) + (walk >> 9)));
            // Written at the plane's width, so a head starts at full precision and the decay has
            // somewhere to go: writing a byte would put the whole tail in the top 8 bits.
            writeWide(plane, w, h, d, px, py, pz, c);
        }
    }

    /// A signed 16-bit sine mapped onto an axis, centered, with the ends reachable.
    static lengthType mapAxis(int16_t s, lengthType extent) {
        if (extent <= 1) return 0;
        const int32_t v = (static_cast<int32_t>(s) + 32768) * (extent - 1) / 65535;
        return static_cast<lengthType>(v);
    }

    /// One light at the plane's full width: an 8-bit color widened by repeating the byte, so 255
    /// becomes 65535 rather than 65280 and a full-brightness head is genuinely full.
    static void writeWide(uint16_t* plane, lengthType w, lengthType h, lengthType d,
                          lengthType x, lengthType y, lengthType z, RGB c) {
        if (x < 0 || y < 0 || z < 0 || x >= w || y >= h || z >= d) return;
        const size_t off = (static_cast<size_t>(z) * h * w + static_cast<size_t>(y) * w + x) * 3;
        plane[off + 0] = static_cast<uint16_t>((c.r << 8) | c.r);
        plane[off + 1] = static_cast<uint16_t>((c.g << 8) | c.g);
        plane[off + 2] = static_cast<uint16_t>((c.b << 8) | c.b);
    }

    /// The wide plane onto the layer, taking each channel's high byte. The one narrowing step.
    void blit(const uint16_t* p, lengthType w, lengthType h, lengthType d) {
        const draw::Canvas cv = canvas();
        const std::size_t samples = static_cast<std::size_t>(w) * h * d * 3;
        std::size_t i = 0;
        for (lengthType z = 0; z < d; z++)
            for (lengthType y = 0; y < h; y++)
                for (lengthType x = 0; x < w; x++, i += 3) {
                    if (i + 2 >= samples) return;
                    draw::pixel(cv, {x, y, z}, RGB{static_cast<uint8_t>(p[i] >> 8),
                                                   static_cast<uint8_t>(p[i + 1] >> 8),
                                                   static_cast<uint8_t>(p[i + 2] >> 8)});
                }
    }

    ScratchBuffer<uint16_t> plane_{*this};     ///< the trail itself, three samples per light
    ScratchBuffer<uint16_t> scratch_{*this};   ///< advect's destination; the two alternate roles
    bool                    front_ = true;     ///< which of the two currently holds the trail
    OscillatorBank<2>       bank_;
    lengthType              planeW_ = 0, planeH_ = 0, planeD_ = 0;
    uint32_t                lastMs_ = 0;
};

}  // namespace mm
