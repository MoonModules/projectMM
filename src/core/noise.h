#pragma once

#include <cstdint>

// Gradient noise: a smooth, deterministic pseudo-random field, the staple "organic motion" source
// for LED effects. inoise8 returns a 0..255 value that varies smoothly across space, so
// neighboring coordinates give similar values (unlike a raw hash). Sample it across a grid for
// clouds/plasma/fire-like fields; scroll a coordinate (or pass a time offset) to animate. 1D, 2D
// and 3D share one gradient set and one interpolation.
//
// The algorithm is Perlin's improved noise (Ken Perlin, "Improving Noise", SIGGRAPH 2002): a
// pseudo-random GRADIENT at every lattice corner (one of the twelve cube-edge directions, chosen by
// a hash), the dot product of that gradient with the offset from the corner, a quintic fade
// 6t⁵ − 15t⁴ + 10t³ on the fraction, and a linear blend across the corners. Gradient noise is zero
// at every lattice point and has no value bias toward the corners, which is why it reads as smooth
// and isotropic where value noise (a random VALUE per corner) reads blocky and axis-aligned at low
// frequency. 2D is 3D with the z offset at zero (inoise8(x, y) == inoise8(x, y, 0)); 1D takes ±1
// gradients of its own, since the cube-edge set projected onto one axis is zero for six codes of
// sixteen. The cost scales with the corners: 2, 4 and 8 corner dot products.
//
// Output uses the full range. The gradient dot product is signed offsets added, so the blend spans
// ±one cell rather than the ±√3/2 the unit-vector bound suggests: an exact search over every
// gradient choice at every fraction gives ±half a cell in 1D, ±one cell in 2D and ±1.035 cells in
// 3D. Each tier halves the raw blend onto its range, so the midpoint is the field's mean and 0 and
// the top value are both reached; the clamp is a guard only the 3D extreme touches. Written fresh,
// integer throughout: the hash and the fade table are ours, the gradient trick is Perlin's.
//
// Coordinates are 16.0 fixed scaled however the caller likes: the high byte selects the noise
// CELL, the low byte the interpolation position within it. So a larger coordinate step per pixel is
// finer noise (more cells across the grid); a smaller step is broader and smoother.

namespace mm {
namespace noise {

/// Lattice hash: a corner's gradient index, 0..15. Each coordinate times its own odd constant,
/// xored (the products of x and x + 1 are shared across a cell's corners, so this part is six
/// multiplies per 3D sample rather than 24), then one xorshift, one multiply, and the top nibble
/// of the product, its best-mixed part. Four bits is all a gradient needs, so this is the whole
/// hash: a byte-wide avalanche spent three multiplies per corner on bits nothing read. Checked
/// against that avalanche: the sixteen codes are uniform to 0.6%, and adjacent corners are
/// independent to the same 5% the avalanche managed (a linear pre-mix with one multiply was NOT:
/// adjacent corners correlated six-fold, which reads as lattice patterns). Both tiers share it, so
/// the 16-bit field is the 8-bit one at finer resolution rather than an unrelated field.
constexpr uint32_t corner(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t h = (x * 0x8da6b343u) ^ (y * 0xd8163841u) ^ (z * 0xcb1ab31fu);
    h ^= h >> 16;
    h *= 0x7feb352du;
    return h >> 28;
}

/// Perlin's twelve cube-edge gradients (every signed permutation of (1, 1, 0)), padded to sixteen
/// with four repeats so the hash's low nibble indexes them by a mask rather than a modulo. A table
/// rather than Perlin's select expression because the selects compile to branches, and a core
/// without a branch predictor pays for each one: the select form was 44 branches per 2D sample on
/// Xtensa, the table is loads and small multiplies.
inline constexpr int8_t kGradient[16][3] = {
    {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0},
    {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
    {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1},
    {1, 1, 0}, {0, -1, 1}, {-1, 1, 0}, {0, -1, -1},
};

/// Dot product of corner gradient `h` (0..15) with the offset, over the axes the arity samples.
template <int Dims>
constexpr int32_t grad(uint32_t h, int32_t x, int32_t y, int32_t z) {
    // 1D takes ±1 gradients of its own: six of the sixteen cube-edge gradients have no x component,
    // which would leave a 1D cell flat wherever both corners drew one.
    if constexpr (Dims == 1) return (h & 1u) ? -x : x;
    const int8_t* g = kGradient[h];
    int32_t d = g[0] * x;
    if constexpr (Dims > 1) d += g[1] * y;
    if constexpr (Dims > 2) d += g[2] * z;
    return d;
}

/// The quintic fade 6t⁵ − 15t⁴ + 10t³ as a table over t = i/256, i = 0..256, in 0.16 fixed
/// (0..65536). The 16-bit tier interpolates between entries on its low byte; the 8-bit tier
/// indexes it directly. A table because the polynomial needs a 40-bit intermediate per sample.
struct FadeTable {
    uint32_t v[257];
    constexpr FadeTable() : v{} {
        for (uint64_t i = 0; i <= 256; i++) {
            // (6i⁵ − 3840i⁴ + 655360i³) / 2^24 == (6t⁵ − 15t⁴ + 10t³) · 65536 for t = i/256.
            const uint64_t i3 = i * i * i;
            v[i] = static_cast<uint32_t>((6u * i3 * i * i - 3840u * i3 * i + 655360u * i3) >> 24);
        }
    }
};
inline constexpr FadeTable kFade{};

/// fade for an 8-bit fraction: a table lookup.
constexpr uint32_t fade8(uint8_t t) { return kFade.v[t]; }

/// fade for a 16-bit fraction: the table on the high byte, linear on the low byte.
constexpr uint32_t fade16(uint16_t t) {
    const uint32_t a = kFade.v[t >> 8], b = kFade.v[(t >> 8) + 1];
    return a + (((b - a) * (t & 0xFFu)) >> 8);
}

/// The two tiers as policies for the one core below: the fraction width, the fade of a fraction, and the blend a→b by a 0.16 weight sized to the tier's operand range.
struct Tier8 {
    static constexpr int kFrac = 8;
    static constexpr uint32_t fade(int32_t f) { return fade8(static_cast<uint8_t>(f)); }
    /// Operands stay within ±2^11 (three offsets of a 256 cell), so the product fits 32 bits.
    static constexpr int32_t blend(int32_t a, int32_t b, uint32_t w) {
        return a + (((b - a) * static_cast<int32_t>(w)) >> 16);
    }
};
struct Tier16 {
    static constexpr int kFrac = 16;
    static constexpr uint32_t fade(int32_t f) { return fade16(static_cast<uint16_t>(f)); }
    /// Operands reach 2^18 against a 2^16 weight: a widening multiply (two on a 32-bit core).
    static constexpr int32_t blend(int32_t a, int32_t b, uint32_t w) {
        return a + static_cast<int32_t>((static_cast<int64_t>(b - a) * static_cast<int64_t>(w)) >> 16);
    }
};

/// The core: gradient noise over the corners of one lattice cell. `x/y/z` are fixed point with
/// `Tier::kFrac` fraction bits (the whole part selects the cell); an unused axis is passed as 0.
/// One instantiation per arity so each compiles to straight-line code over exactly its corners.
/// `layer` is one z-slice (2 or 4 corners with the blends between them) and 3D is that slice at z
/// and z + 1 blended by the z fade: written this way because one body with eight corners in
/// flight spilled half its registers on a 16-register window (52 stack stores per sample on
/// Xtensa), while the slice form keeps at most four. Returns the raw blend in cell units: ±half a
/// cell in 1D, ±one cell in 2D, ±1.035 cells in 3D (the exact extremes, from a search over every
/// gradient choice at every fraction).
template <class Tier, int Dims>
constexpr int32_t layer(uint32_t ix, uint32_t iy, uint32_t iz, int32_t fx, int32_t fy, int32_t fz, uint32_t wx, uint32_t wy) {
    constexpr int32_t kCell = 1 << Tier::kFrac;
    int32_t v = Tier::blend(grad<Dims>(corner(ix, iy, iz),     fx,         fy, fz),
                            grad<Dims>(corner(ix + 1, iy, iz), fx - kCell, fy, fz), wx);
    if constexpr (Dims > 1) {
        v = Tier::blend(v, Tier::blend(grad<Dims>(corner(ix, iy + 1, iz),     fx,         fy - kCell, fz),
                                       grad<Dims>(corner(ix + 1, iy + 1, iz), fx - kCell, fy - kCell, fz), wx), wy);
    }
    return v;
}

template <class Tier, int Dims>
constexpr int32_t raw(uint32_t x, uint32_t y, uint32_t z) {
    constexpr int32_t kCell = 1 << Tier::kFrac;
    const uint32_t ix = x >> Tier::kFrac, iy = y >> Tier::kFrac, iz = z >> Tier::kFrac;
    const int32_t fx = static_cast<int32_t>(x & (kCell - 1));
    const int32_t fy = static_cast<int32_t>(y & (kCell - 1));
    const int32_t fz = static_cast<int32_t>(z & (kCell - 1));
    const uint32_t wx = Tier::fade(fx);
    const uint32_t wy = Dims > 1 ? Tier::fade(fy) : 0;
    int32_t v = layer<Tier, Dims>(ix, iy, iz, fx, fy, fz, wx, wy);
    if constexpr (Dims > 2) v = Tier::blend(v, layer<Tier, Dims>(ix, iy, iz + 1, fx, fy, fz - kCell, wx, wy), Tier::fade(fz));
    return v;
}

/// Map a raw blend onto the tier's unsigned range, centered on the midpoint.
///
/// The scale is per ARITY, because the extreme is: ±half a cell in 1D, ±one cell in 2D and ±1.035
/// in 3D (the exact figures, from a search over every gradient choice at every fraction). Halving
/// regardless is right for 2D and 3D and wrong for 1D, which then spans only the middle half of the
/// range: measured 64..192 of 0..255, so a 1D field read washed out against the same field sampled
/// in 2D. The clamp is a guard that only the 3D extreme past one cell reaches.
template <class Tier, int Dims>
constexpr uint32_t out(int32_t raw) {
    constexpr int32_t kCell = 1 << Tier::kFrac;
    const int32_t v = kCell / 2 + (Dims == 1 ? raw : (raw >> 1));
    return static_cast<uint32_t>(v < 0 ? 0 : (v > kCell - 1 ? kCell - 1 : v));
}

/// Linear interpolate a→b by t/65536, unsigned 16-bit endpoints. The 16-bit tier's general lerp,
/// kept for callers that blend field values rather than gradients.
constexpr uint16_t lerp16(uint16_t a, uint16_t b, uint16_t t) {
    const int32_t delta = static_cast<int32_t>(b) - static_cast<int32_t>(a);
    // 64-bit intermediate: `delta * t` reaches 4.29e9 against an INT32_MAX of 2.15e9, so the 32-bit
    // form was signed overflow (undefined behavior) on roughly a quarter of samples. It happened to
    // produce the right low bits on wrap-around hardware, which is what kept it invisible.
    return static_cast<uint16_t>(static_cast<int32_t>(a)
                                 + static_cast<int32_t>((static_cast<int64_t>(delta) * t) >> 16));
}

/// Octave sums narrow, and this is what widens them back.
///
/// fbm divides its octave sum by the sum of the AMPLITUDES, which is what keeps the result inside
/// range. But octaves are near-independent, so their spread grows like the root of the sum of
/// squares rather than like the sum: dividing by the latter shrinks the field every octave added.
/// Measured on the shipped noise, 4 octaves spanned 54..199 of 0..255, so a caller stretching the
/// top of the field (a contrast window, a threshold) could never reach full brightness however it
/// was configured, and every fbm read flatter than the noise underneath it.
///
/// The gain is that ratio per octave count, in 8.8 fixed, applied around the midpoint so the field
/// stays centered. One octave needs none and the series settles by about six.
inline constexpr uint16_t kFbmGain[9] = {256, 256, 343, 391, 417, 430, 437, 440, 442};

/// Re-widen an octave sum around the midpoint. `mid` is 128 at the 8-bit tier, 32768 at 16-bit.
constexpr int32_t fbmWiden(int32_t v, int32_t mid, uint8_t octaves) {
    const uint16_t g = kFbmGain[octaves > 8 ? 8 : octaves];
    return mid + (((v - mid) * g) >> 8);
}

}  // namespace noise

// 1D gradient noise: x is a 16.0 fixed coordinate (high byte = cell, low byte = position).
constexpr uint8_t inoise8(uint32_t x) {
    return static_cast<uint8_t>(noise::out<noise::Tier8, 1>(noise::raw<noise::Tier8, 1>(x, 0, 0)));
}

// 2D gradient noise over the 4 cell corners.
constexpr uint8_t inoise8(uint32_t x, uint32_t y) {
    return static_cast<uint8_t>(noise::out<noise::Tier8, 2>(noise::raw<noise::Tier8, 2>(x, y, 0)));
}

// 3D gradient noise over the 8 cube corners.
constexpr uint8_t inoise8(uint32_t x, uint32_t y, uint32_t z) {
    return static_cast<uint8_t>(noise::out<noise::Tier8, 3>(noise::raw<noise::Tier8, 3>(x, y, z)));
}

// --- 16-bit noise ---------------------------------------------------------------------------
//
// The same gradient noise at 16 bits. math16.h states why the tier exists: 256 levels band visibly
// on a large fixture, so everything an effect writes against is 16-bit. The 8-bit forms above stay
// for the cases where a byte is what the caller needs anyway (a palette index, a brightness).

/// 1D gradient noise at 16 bits. `x` is 16.16 fixed point: the whole part selects the cell, the
/// fraction interpolates within it.
constexpr uint16_t inoise16(uint32_t x) {
    return static_cast<uint16_t>(noise::out<noise::Tier16, 1>(noise::raw<noise::Tier16, 1>(x, 0, 0)));
}

/// 2D gradient noise at 16 bits: the common case for a panel.
constexpr uint16_t inoise16(uint32_t x, uint32_t y) {
    return static_cast<uint16_t>(noise::out<noise::Tier16, 2>(noise::raw<noise::Tier16, 2>(x, y, 0)));
}

/// 3D gradient noise at 16 bits. `z` is the axis a 2D effect uses as time, so the field evolves in
/// place instead of scrolling past.
constexpr uint16_t inoise16(uint32_t x, uint32_t y, uint32_t z) {
    return static_cast<uint16_t>(noise::out<noise::Tier16, 3>(noise::raw<noise::Tier16, 3>(x, y, z)));
}

/// Fractal Brownian motion at 16 bits: `octaves` samples at doubling frequency, halving amplitude.
inline uint16_t fbm16(uint32_t x, uint32_t y, uint8_t octaves) {
    if (octaves == 0) return 32768;                     // no octaves: flat mid-field
    // The sum is 64-bit so each octave keeps its full 16 bits. Shifting each sample down by 8 to fit
    // a 32-bit accumulator made the result 8-bit wearing a 16-bit type: measured at 195 distinct
    // values over 20,000 samples, low byte never set: which is exactly the banding this tier is for.
    uint64_t sum = 0;
    uint32_t norm = 0, amp = 32768;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        sum  += static_cast<uint64_t>(inoise16(x, y)) * amp;
        norm += amp;
        x <<= 1; y <<= 1;                               // double the frequency
        amp >>= 1;                                      // halve the contribution
    }
    if (!norm) return 32768;
    const int32_t widened = noise::fbmWiden(static_cast<int32_t>(sum / norm), 32768, octaves);
    return static_cast<uint16_t>(widened < 0 ? 0 : (widened > 65535 ? 65535 : widened));
}

/// 3D fbm at 16 bits: the same sum with a z axis, so a volumetric fixture samples a real field
/// rather than a plane repeated along z, and a 2D effect can use z as time.
inline uint16_t fbm16(uint32_t x, uint32_t y, uint32_t z, uint8_t octaves) {
    if (octaves == 0) return 32768;
    uint64_t sum = 0;
    uint32_t norm = 0, amp = 32768;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        sum  += static_cast<uint64_t>(inoise16(x, y, z)) * amp;
        norm += amp;
        x <<= 1; y <<= 1; z <<= 1;
        amp >>= 1;
    }
    if (!norm) return 32768;
    const int32_t widened = noise::fbmWiden(static_cast<int32_t>(sum / norm), 32768, octaves);
    return static_cast<uint16_t>(widened < 0 ? 0 : (widened > 65535 ? 65535 : widened));
}

// --- Field composition ------------------------------------------------------------------------
//
// One noise sample is a smooth blur; the looks people actually recognize come from COMPOSING
// samples. Three standard compositions cover most of it, and each is a few lines over `inoise8`
// rather than a new field generator:
//
//   fbm     : sum octaves at doubling frequency and halving amplitude. Turns the blur into
//              cloud/terrain/smoke structure: large shapes with fine detail on them.
//   turbulence: the same sum over |noise|, whose creases read as billows and flame.
//   warp    : sample noise at a coordinate that noise itself displaced (domain warping). This
//              is the one that produces the flowing, marbled, liquid look; Iñigo Quilez's
//              "warping" article is the canonical description.
//
// Cost is stated per call because it is the thing that decides whether an effect fits: each
// octave is one `inoise8`, so fbm(3) costs three samples, and warp costs its own samples PLUS the
// field it then samples. On a large fixture that multiplies by pixel count: see the per-target
// budget in the power-function docs before reaching for octaves on a 128x128 wall.

/// Fractal Brownian motion: `octaves` samples at doubling frequency, halving amplitude, returned
/// normalized to 0..255. octaves=1 is plain noise; 3-4 is the usual cloud look.
inline uint8_t fbm8(uint32_t x, uint32_t y, uint8_t octaves) {
    if (octaves == 0) return 128;                       // no octaves: flat mid-field
    uint32_t sum = 0, norm = 0, amp = 128;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        sum  += static_cast<uint32_t>(inoise8(x, y)) * amp;
        norm += amp;
        x <<= 1; y <<= 1;                               // double the frequency
        amp >>= 1;                                      // halve the contribution
    }
    if (!norm) return 128;
    const int32_t widened = noise::fbmWiden(static_cast<int32_t>(sum / norm), 128, octaves);
    return static_cast<uint8_t>(widened < 0 ? 0 : (widened > 255 ? 255 : widened));
}

/// 3D fbm: the same sum with a z axis, so a 2D effect can use z as time for a field that evolves
/// in place rather than scrolling past.
inline uint8_t fbm8(uint32_t x, uint32_t y, uint32_t z, uint8_t octaves) {
    if (octaves == 0) return 128;
    uint32_t sum = 0, norm = 0, amp = 128;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        sum  += static_cast<uint32_t>(inoise8(x, y, z)) * amp;
        norm += amp;
        x <<= 1; y <<= 1; z <<= 1;
        amp >>= 1;
    }
    if (!norm) return 128;
    const int32_t widened = noise::fbmWiden(static_cast<int32_t>(sum / norm), 128, octaves);
    return static_cast<uint8_t>(widened < 0 ? 0 : (widened > 255 ? 255 : widened));
}

/// Turbulence: fbm over |noise - 128|, which creases the field where it crosses the midpoint. The
/// creases are what read as billowing smoke and flame rather than soft cloud.
inline uint8_t turbulence8(uint32_t x, uint32_t y, uint8_t octaves) {
    if (octaves == 0) return 0;
    uint32_t sum = 0, norm = 0, amp = 128;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        const int16_t v = static_cast<int16_t>(inoise8(x, y)) - 128;
        sum  += static_cast<uint32_t>(v < 0 ? -v : v) * 2u * amp;
        norm += amp;
        x <<= 1; y <<= 1;
        amp >>= 1;
    }
    const uint32_t r = norm ? sum / norm : 0;
    return static_cast<uint8_t>(r > 255 ? 255 : r);
}

/// 3D turbulence: the same creased sum with a z axis.
inline uint8_t turbulence8(uint32_t x, uint32_t y, uint32_t z, uint8_t octaves) {
    if (octaves == 0) return 0;
    uint32_t sum = 0, norm = 0, amp = 128;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        const int16_t v = static_cast<int16_t>(inoise8(x, y, z)) - 128;
        sum  += static_cast<uint32_t>(v < 0 ? -v : v) * 2u * amp;
        norm += amp;
        x <<= 1; y <<= 1; z <<= 1;
        amp >>= 1;
    }
    const uint32_t r = norm ? sum / norm : 0;
    return static_cast<uint8_t>(r > 255 ? 255 : r);
}

/// Domain warp: displace the sample coordinate by a noise field, then sample there. `strength` is
/// how far the displacement reaches, in the same fixed-point units as the coordinates.
///
/// This is the primitive behind the flowing/marbled look: the field stops looking like a texture
/// laid on the grid and starts looking like something moving through it. Two extra samples.

/// 3D domain warp: the same displacement with a z axis, so the field flows through a volume rather
/// than through a plane. Three probes rather than two, each offset by its own constant so the axes
/// displace independently: sampling one field three times would move everything along a diagonal.
///
/// `octaves` has NO default here, unlike the 2D form. With one, `warp8(x, y, strength, octaves)` and
/// `warp8(x, y, z, strength)` are both viable at four arguments and every existing call becomes
/// ambiguous. Requiring it makes the arity say which field the caller means.
///
/// Shares one body with the 2D form through `Dims`, so the two cannot drift apart, and each still
/// compiles to exactly its own arity: the 2D instantiation samples 2D noise over four corners and
/// never touches z. Calling the 3D body with z = 0 would have been simpler and measured 1.73x
/// slower, because every probe and the inner fbm then walk eight corners to reach the same answer.
template <int Dims>
inline uint8_t warpImpl(uint32_t x, uint32_t y, uint32_t z, uint16_t strength, uint8_t octaves) {
    // Offset the probe fields so the axes displace independently: sampling one field twice would
    // move everything along a diagonal.
    int32_t dx, dy, dz = 0;
    if constexpr (Dims > 2) {
        dx = (static_cast<int32_t>(inoise8(x, y, z)) - 128) * strength / 128;
        dy = (static_cast<int32_t>(inoise8(x + 0x9E37u, y + 0x7C15u, z)) - 128) * strength / 128;
        // The z probe rides z ITSELF rather than a constant offset, so a fixture with no depth
        // displaces nothing along an axis it does not have. That is what makes the 2D instantiation
        // below the same field as this one at z = 0, rather than merely a similar one.
        if (z) dz = (static_cast<int32_t>(inoise8(x + 0x6A09u, y + 0xBB67u, z)) - 128) * strength / 128;
    } else {
        dx = (static_cast<int32_t>(inoise8(x, y)) - 128) * strength / 128;
        dy = (static_cast<int32_t>(inoise8(x + 0x9E37u, y + 0x7C15u)) - 128) * strength / 128;
    }
    // The displacement is added in UNSIGNED arithmetic. Casting the coordinate to int32_t first
    // was signed overflow (undefined behavior) for any coordinate past 2^31, which a scaled field
    // reaches easily: `r * zoom + drift` on a large fixture is already past it. Unsigned wrapping is
    // defined, and wrapping is what a noise coordinate wants anyway.
    const uint32_t sx = x + static_cast<uint32_t>(dx);
    const uint32_t sy = y + static_cast<uint32_t>(dy);
    if constexpr (Dims > 2) {
        return fbm8(sx, sy, z + static_cast<uint32_t>(dz), octaves);
    } else {
        return fbm8(sx, sy, octaves);
    }
}

/// 3D domain warp: the field flows through a volume rather than through a plane.
inline uint8_t warp8(uint32_t x, uint32_t y, uint32_t z, uint16_t strength, uint8_t octaves) {
    return warpImpl<3>(x, y, z, strength, octaves);
}

/// 2D domain warp: displace the sample coordinate by a noise field, then sample there. `strength` is
/// how far the displacement reaches, in the same fixed-point units as the coordinates.
///
/// This is the primitive behind the flowing/marbled look: the field stops looking like a texture
/// laid on the grid and starts looking like something moving through it. Two extra samples.
inline uint8_t warp8(uint32_t x, uint32_t y, uint16_t strength, uint8_t octaves = 1) {
    return warpImpl<2>(x, y, 0u, strength, octaves);
}

/// Curl of a noise potential: a velocity field that cannot pile up or thin out.
///
/// The perpendicular gradient of a scalar field (Bridson, "Curl-Noise for Procedural Fluid Flow",
/// SIGGRAPH 2007). Taking the gradient of a potential and turning it 90 degrees gives a field whose
/// divergence is zero BY CONSTRUCTION: whatever flows into a region flows out again. That is what
/// separates it from sampling noise straight into a velocity, where the field has sources and sinks
/// and anything carried by it collects in the sinks and drains from the sources, which looks like
/// clumping rather than flow.
///
/// The output is scaled by `strength`, in the caller's own units (sub-pixels per frame for a
/// transport). `eps` is the sampling distance for the central difference, in the same 16.16
/// coordinates as the field: too small and the difference is quantization noise, too large and the
/// curl is of a blurrier field than the one being sampled. The default is a sixteenth of a cell.
inline void curl16(uint32_t x, uint32_t y, uint32_t z, int32_t strength,
                   int32_t& vx, int32_t& vy, uint32_t eps = 4096) {
    // Two central differences of the potential. dP/dy becomes the x component and -dP/dx the y,
    // which is the 90-degree turn: the flow runs ALONG the potential's contours rather than up them.
    const int32_t dy = static_cast<int32_t>(inoise16(x, y + eps, z))
                     - static_cast<int32_t>(inoise16(x, y - eps, z));
    const int32_t dx = static_cast<int32_t>(inoise16(x + eps, y, z))
                     - static_cast<int32_t>(inoise16(x - eps, y, z));
    // The differences span roughly a quarter of the range at the default eps, so >>15 keeps the
    // result near `strength` rather than swamping it. A caller wanting a wilder field raises eps.
    vx = (dy * strength) >> 15;
    vy = (-dx * strength) >> 15;
}

/// The 2D form: the same field at a fixed z, which is what a panel wants.
inline void curl16(uint32_t x, uint32_t y, int32_t strength, int32_t& vx, int32_t& vy,
                   uint32_t eps = 4096) {
    curl16(x, y, 0u, strength, vx, vy, eps);
}

}  // namespace mm
