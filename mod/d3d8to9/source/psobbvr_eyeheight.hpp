#pragma once

// Dynamic eye height: the character's head height in game units (~16 on
// a HUmar), taken from the character instead of derived from a fixed
// meters value, so it fits every build and survives world-scale changes.
//
// Primary source, the game's proportion law: the class byte and the two
// creation-pad floats pick three of seven shape bodies and blend them. The
// table below holds the shapes' standing head heights, so the height is
// known the moment the entity exists, before any model is built or drawn.
//
// Fallback and cross-check, the sampled head: the game draws the head
// under its own world matrix, set from the same character-model site as
// the body root (0x844AFD; two matrices per character there - root at the
// feet, head ~16 units up). partmap fingerprints the head matrix and
// feeds its height here once per BeginScene pass. The head animates, so
// the capture is a frozen STANDING height: the median of recent samples,
// taken when gamecam's spawn settle window ends and kept across anchor
// resets until the next capture. It is used only while the proportions
// cannot be read.
//
// Used by gamecam::Apply (eye point) and trim::OnFrame (learn-zone
// anchor), both gated on [vr] eye_height_auto.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "psobbvr_log.hpp"
#include "psobbvr_vr.hpp"

namespace eyeheight {

// Median window of 48 per-pass samples (~1.6 s; the character draws on
// every other pass at idle). Below MIN_SAMPLES (e.g. a cutscene camera
// looking away) a capture is deferred until enough have arrived.
constexpr int RING = 48;
constexpr int MIN_SAMPLES = 6;

inline float ring[RING] = {};
inline int ring_pos = 0;
inline int ring_count = 0;

// This pass's sample; the game may set the head matrix several times per
// pass, the last one wins.
inline bool pass_sample_valid = false;
inline float pass_sample = 0.0f;
inline float last_sample = 0.0f;

inline bool captured_valid = false;
inline float captured_units = 0.0f;
inline bool capture_pending = false;


// Float height offset under the whole body: the player draw routine
// 0x689DEC translates the body root to the feet plus [entity+0x6D4] on y.
constexpr uintptr_t ENTITY_HEIGHT_OFFSET = 0x6D4;
// The character's visual block (entity +0x940): class byte +0x961 (0..11
// in the game's class order, HUmar first), proportion sliders +0x978 /
// +0x97C (floats, the creation pad's x and y).
constexpr uintptr_t ENTITY_CLASS_OFFSET = 0x961;
constexpr uintptr_t ENTITY_PROPORTION_OFFSET = 0x978;
constexpr int CLASS_COUNT = 12;
// The creation screen confines the pair to a disc of radius 0.5 around
// (0.5, 0.5); a value outside these bounds is not a character's pair.
constexpr float PROPORTION_MIN = -0.5f;
constexpr float PROPORTION_MAX = 1.5f;
// The idle animation holds the head a little below the rest pose: idle
// head 16.04 against rest 16.32 on a HUmar at default proportions, 12.4
// against 12.50 on a short FOmarl.
constexpr float IDLE_HEAD_FACTOR = 0.985f;

// The creation pad's nine corners (x, y): the game's table at 0x9B1680,
// completed at run time by the writes at 0x4E9692.
constexpr float PAD_CORNERS[9][2] = {
    {0.0f, 0.0f}, {1.0f / 3, 0.0f}, {2.0f / 3, 0.0f},
    {1.0f / 3, 0.5f}, {0.0f, 1.0f}, {1.0f / 3, 1.0f},
    {2.0f / 3, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
// The pad's eight triangles, three corner indices each (0x9B16E0).
constexpr int PAD_SECTORS[8][3] = {{1, 3, 0}, {0, 4, 3}, {3, 5, 4}, {1, 3, 2},
                                   {2, 6, 3}, {3, 5, 6}, {2, 6, 7}, {8, 7, 2}};

// Bone-58 rest height of each class's seven proportion shape bodies
// (client/data/plZsmpnj.afs, archive order = class order, slot order):
// each shape's bone tree composed in its rest pose.
constexpr float PROPORTION_SHAPE_HEIGHTS[12][7] = {
    {17.7621f, 17.8714f, 17.5196f, 16.3164f, 13.0935f, 13.1344f, 13.1113f},  // HUmar
    {16.0896f, 16.0896f, 16.0898f, 14.6301f, 12.2153f, 12.1918f, 12.1948f},  // HUnewearl
    {19.7172f, 19.6922f, 19.6922f, 18.0457f, 16.4843f, 16.4168f, 16.4168f},  // HUcast
    {17.3005f, 17.3039f, 17.2237f, 15.8924f, 13.7463f, 13.7616f, 13.8089f},  // RAmar
    {18.3875f, 18.5582f, 18.3837f, 16.6287f, 14.8586f, 15.1471f, 14.8111f},  // RAcast
    {17.4082f, 17.4082f, 17.3849f, 14.8602f, 12.9105f, 12.9105f, 12.9424f},  // RAcaseal
    {15.3165f, 15.3153f, 15.3165f, 13.8572f, 12.4897f, 12.4891f, 12.4452f},  // FOmarl
    {17.6037f, 17.8010f, 17.7994f, 16.2049f, 14.1804f, 14.3434f, 14.3434f},  // FOnewm
    {15.5431f, 15.5431f, 15.5431f, 14.0383f, 11.8758f, 11.8749f, 11.8758f},  // FOnewearl
    {16.3164f, 16.3164f, 16.3164f, 14.6098f, 11.7029f, 12.0038f, 12.5297f},  // HUcaseal
    {17.6574f, 17.8339f, 17.7209f, 16.0678f, 12.4764f, 12.9620f, 12.7976f},  // FOmar
    {16.0000f, 16.0300f, 15.7275f, 14.5409f, 12.1881f, 12.1506f, 12.1103f},  // RAmarl
};

// The character the seed was computed for (entity, class byte and pair)
// and its standing head height in units.
inline bool seed_valid = false;
inline float seed_units = 0.0f;
inline uintptr_t seed_entity = 0;
inline int seed_class = 0;
inline float seed_proportion[2] = {};
// The last entity whose proportions were unusable, so it is logged once.
inline uintptr_t seed_fail_entity = 0;

// The proportion seed when valid, else the sampled capture.
inline bool Valid() { return seed_valid || captured_valid; }
inline float Units() { return seed_valid ? seed_units : captured_units; }

// A head-matrix set that passed partmap's fingerprint (the local player's
// head only).
inline void OnSample(float dy_units) {
    pass_sample = dy_units;
    pass_sample_valid = true;
}

inline float MedianUnits() {  // 0 when the ring is empty
    if (ring_count == 0)
        return 0.0f;
    float sorted[RING];
    memcpy(sorted, ring, sizeof(float) * ring_count);
    std::nth_element(sorted, sorted + ring_count / 2, sorted + ring_count);
    return sorted[ring_count / 2];
}

inline void CaptureStanding(const char* reason) {
    if (ring_count < MIN_SAMPLES) {
        capture_pending = true;
        diag::Log("eyeheight: capture (%s) deferred - %d/%d samples so far",
                  reason, ring_count, MIN_SAMPLES);
        return;
    }
    captured_units = MedianUnits();
    captured_valid = true;
    capture_pending = false;
    diag::Log("eyeheight: captured standing head %.2f units (%.3f m at scale %.2f), %d samples (%s)",
              captured_units, captured_units / vrmod::config.world_scale,
              vrmod::config.world_scale, ring_count, reason);
    if (seed_valid)
        diag::Log("eyeheight: sampled median %.2f vs proportions %.2f (diff %+.2f)",
                  captured_units, seed_units, captured_units - seed_units);
}

// A corner's shape body: its own index, except corners 7 and 8, which
// reuse shapes 4 and 0 (0x4E7E54).
inline int CornerShape(int corner) { return corner == 7 ? 4 : corner == 8 ? 0 : corner; }

// The pad triangle holding (x, y), tested in the order of 0x4E877C.
inline int PadSector(float x, float y) {
    constexpr float third = 1.0f / 3, two_thirds = 2.0f / 3;
    if (x > two_thirds)
        return (x - two_thirds) * 3.0f <= y ? 6 : 7;
    if (x <= third) {
        if (two_thirds * y <= x)
            return 0;
        return x <= (y - 1.0f) * -two_thirds ? 1 : 2;
    }
    if (x <= (y - 1.0f) * -two_thirds)
        return 3;
    return y * two_thirds <= x ? 4 : 5;
}

// The class's rest head height at (x, y): the sector's three shape heights
// blended with the barycentric weights of 0x4E86E4.
inline float RestHeight(int class_id, float x, float y, int sector) {
    const int* corner = PAD_SECTORS[sector];
    const float* p0 = PAD_CORNERS[corner[0]];
    const float* p1 = PAD_CORNERS[corner[1]];
    const float* p2 = PAD_CORNERS[corner[2]];
    const float c = (x - p0[0]) / (p2[0] - p0[0]);
    const float b = ((y - p0[1]) - c * (p2[1] - p0[1])) / (p1[1] - p0[1]);
    const float a = 1.0f - (b + c);
    const float* h = PROPORTION_SHAPE_HEIGHTS[class_id];
    return a * h[CornerShape(corner[0])] + b * h[CornerShape(corner[1])] +
           c * h[CornerShape(corner[2])];
}

inline bool OnPad(float v) {
    return std::isfinite(v) && v >= PROPORTION_MIN && v <= PROPORTION_MAX;
}

// Computes the character's standing head height from its class byte and
// proportion pair. False, with the seed dropped so the sampled height
// stands in, when they are unreadable or out of range.
inline bool SeedFromProportions(uintptr_t entity) {
    int class_id = -1;
    float pair[2] = {0.0f, 0.0f};
    float height_offset = 0.0f;
    const bool readable =
        entity != 0 &&
        diag::Accessible(entity + ENTITY_HEIGHT_OFFSET,
                         ENTITY_PROPORTION_OFFSET + sizeof(pair) - ENTITY_HEIGHT_OFFSET, false);
    if (readable) {
        class_id = *reinterpret_cast<const uint8_t*>(entity + ENTITY_CLASS_OFFSET);
        memcpy(pair, reinterpret_cast<const void*>(entity + ENTITY_PROPORTION_OFFSET),
               sizeof(pair));
        height_offset = *reinterpret_cast<const float*>(entity + ENTITY_HEIGHT_OFFSET);
    }
    if (!readable || class_id >= CLASS_COUNT || !OnPad(pair[0]) || !OnPad(pair[1])) {
        seed_valid = false;
        if (entity != seed_fail_entity) {
            seed_fail_entity = entity;
            diag::Log("eyeheight: proportions unreadable or out of range (class %d, %.3f / %.3f); "
                      "using the sampled height",
                      class_id, pair[0], pair[1]);
        }
        return false;
    }
    // A damaged offset would carry NaN into the eye point.
    if (!std::isfinite(height_offset))
        height_offset = 0.0f;
    const int sector = PadSector(pair[0], pair[1]);
    const float rest = RestHeight(class_id, pair[0], pair[1], sector);
    seed_units = rest * IDLE_HEAD_FACTOR + height_offset;
    seed_entity = entity;
    seed_class = class_id;
    memcpy(seed_proportion, pair, sizeof(seed_proportion));
    seed_valid = true;
    seed_fail_entity = 0;
    diag::Log("eyeheight: seeded from the proportions: class %d pair %.3f / %.3f -> sector %d, "
              "rest %.2f x %.3f + %.2f = %.2f units (%.3f m at scale %.2f)",
              class_id, pair[0], pair[1], sector, rest, IDLE_HEAD_FACTOR, height_offset,
              seed_units, seed_units / vrmod::config.world_scale, vrmod::config.world_scale);
    return true;
}

// Called every gamecam::Apply. Recomputes when there is no seed or the
// entity, its class byte or either proportion float (byte for byte)
// differs from the seed's: a new character can be given the old one's
// entity address. Unchanged, it is one readability check and two reads.
inline void OnApply(uintptr_t entity) {
    if (seed_valid && entity == seed_entity &&
        diag::Accessible(entity + ENTITY_CLASS_OFFSET,
                         ENTITY_PROPORTION_OFFSET + sizeof(seed_proportion) - ENTITY_CLASS_OFFSET,
                         false) &&
        *reinterpret_cast<const uint8_t*>(entity + ENTITY_CLASS_OFFSET) == seed_class &&
        memcmp(reinterpret_cast<const void*>(entity + ENTITY_PROPORTION_OFFSET),
               seed_proportion, sizeof(seed_proportion)) == 0)
        return;
    SeedFromProportions(entity);
}

// Fresh spawn: empty the ring so areas and poses don't mix. The captured
// value is kept until the next settle re-captures.
inline void OnAnchorReset() {
    ring_count = 0;
    ring_pos = 0;
    pass_sample_valid = false;
    capture_pending = false;
}

// Once per BeginScene pass (from partmap::OnFrame): pushes the last
// pass's sample into the ring and services the developer tools' requests.
// aim_dy_units =
// the entity aim point's height above the feet (entity +0x78, logged for
// comparison), or <= -999 when unreadable.
inline void OnFrame(float aim_dy_units) {
    if (pass_sample_valid) {
        ring[ring_pos] = pass_sample;
        ring_pos = (ring_pos + 1) % RING;
        if (ring_count < RING)
            ring_count++;
        last_sample = pass_sample;
    }
    pass_sample_valid = false;
    if (capture_pending && ring_count >= MIN_SAMPLES)
        CaptureStanding("deferred");
}

} // namespace eyeheight
