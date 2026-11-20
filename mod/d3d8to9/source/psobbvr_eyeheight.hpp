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
// anchor), both gated on [vr] eye_height_auto. The same blend gives the
// right fist's channel, the line a melee weapon's handle rests along
// (weapongrip::MeleeSlide).

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

// BEGIN hand tables (generated)
// The fist channel of each class's seven proportion shape bodies: the line
// a round handle rests along in the closed right hand, in the grip point's
// rest frame (bone 48: +x out of the palm, -y from the wrist toward the
// fingers, +z the thumb side), game units. The point is where the line
// crosses z = 0 (x, y); the direction is a unit vector with z positive.
// Generated: do not edit by hand.
constexpr float PROPORTION_SHAPE_HOLE_POINT[12][7][2] = {
    {{0.05360f, 0.03610f}, {0.03510f, -0.24690f}, {0.03250f, -0.13500f}, {0.04070f, 0.00680f}, {-0.08670f, -0.07440f}, {0.00490f, 0.03730f}, {0.12080f, -0.01020f}},  // HUmar
    {{-0.15110f, -0.31390f}, {-0.14150f, -0.19090f}, {-0.06440f, -0.38520f}, {-0.06010f, -0.21620f}, {-0.07550f, -0.19470f}, {0.01510f, -0.18870f}, {0.06480f, -0.24360f}},  // HUnewearl
    {{-0.23110f, -0.05170f}, {-0.00630f, -0.21710f}, {-0.06070f, -0.42820f}, {0.00670f, 0.28620f}, {-0.14990f, 0.04670f}, {-0.05640f, 0.02970f}, {0.02580f, 0.02770f}},  // HUcast
    {{0.10090f, 0.06760f}, {0.05070f, 0.06520f}, {0.07240f, 0.18530f}, {0.03650f, 0.07920f}, {0.02700f, 0.07710f}, {0.11140f, 0.15250f}, {0.11230f, 0.28710f}},  // RAmar
    {{-0.02460f, 0.30680f}, {0.16300f, 0.13570f}, {0.18990f, 0.09330f}, {0.17810f, 0.04540f}, {-0.01860f, -0.09950f}, {0.14990f, 0.14900f}, {0.27940f, 0.29470f}},  // RAcast
    {{0.15580f, 0.33400f}, {0.25200f, 0.42300f}, {0.11850f, 0.39780f}, {0.06310f, 0.22050f}, {0.05340f, 0.20190f}, {0.04880f, 0.20840f}, {0.11440f, 0.31620f}},  // RAcaseal
    {{-0.05050f, -0.24020f}, {-0.07150f, -0.08070f}, {-0.09140f, -0.08360f}, {-0.06090f, -0.09610f}, {-0.03240f, -0.02840f}, {-0.03930f, -0.03660f}, {-0.05210f, -0.00980f}},  // FOmarl
    {{0.15540f, -0.06330f}, {0.15540f, -0.11470f}, {0.08920f, -0.10650f}, {0.15800f, -0.10770f}, {0.12850f, -0.04140f}, {0.04770f, -0.16450f}, {0.07770f, -0.25660f}},  // FOnewm
    {{-0.02170f, -0.06820f}, {0.05220f, 0.15240f}, {-0.04920f, -0.07470f}, {-0.02600f, -0.05020f}, {-0.01570f, -0.07520f}, {0.06030f, -0.01750f}, {-0.05340f, -0.03390f}},  // FOnewearl
    {{0.09100f, -0.12440f}, {0.00920f, -0.09210f}, {0.22190f, -0.11850f}, {0.01690f, -0.06510f}, {0.09040f, -0.03240f}, {0.10150f, 0.04480f}, {0.05590f, -0.00350f}},  // HUcaseal
    {{0.06020f, -0.35850f}, {0.05560f, -0.12900f}, {0.08190f, -0.17330f}, {0.06170f, -0.26060f}, {0.03450f, 0.23090f}, {0.06580f, 0.08590f}, {0.18820f, -0.10810f}},  // FOmar
    {{0.04850f, -0.30280f}, {-0.00720f, -0.38350f}, {0.10630f, -0.03900f}, {0.06120f, -0.27110f}, {0.21560f, -0.15860f}, {0.12300f, -0.27620f}, {0.00840f, -0.22700f}},  // RAmarl
};

constexpr float PROPORTION_SHAPE_HOLE_DIR[12][7][3] = {
    {{0.00750f, -0.20120f, 0.97950f}, {0.10240f, -0.29730f, 0.94930f}, {0.01410f, -0.26290f, 0.96470f}, {-0.00500f, -0.20160f, 0.97950f}, {0.00270f, -0.20240f, 0.97930f}, {0.00600f, -0.20040f, 0.97970f}, {0.04290f, -0.23790f, 0.97040f}},  // HUmar
    {{-0.02710f, -0.34670f, 0.93760f}, {-0.01560f, -0.38280f, 0.92370f}, {-0.00840f, -0.21040f, 0.97760f}, {-0.03650f, -0.36100f, 0.93190f}, {-0.04230f, -0.40260f, 0.91440f}, {-0.01300f, -0.38200f, 0.92410f}, {0.01440f, -0.34590f, 0.93820f}},  // HUnewearl
    {{-0.00380f, -0.24560f, 0.96940f}, {0.03530f, -0.27160f, 0.96180f}, {0.14370f, -0.43520f, 0.88880f}, {0.00180f, -0.23210f, 0.97270f}, {0.00050f, -0.22250f, 0.97490f}, {0.00230f, -0.24270f, 0.97010f}, {0.00640f, -0.24680f, 0.96900f}},  // HUcast
    {{0.00200f, -0.18240f, 0.98320f}, {0.00530f, -0.26870f, 0.96320f}, {0.00420f, -0.26250f, 0.96490f}, {0.01320f, -0.24720f, 0.96890f}, {0.01190f, -0.24050f, 0.97060f}, {0.01420f, -0.30870f, 0.95110f}, {-0.43030f, -0.12520f, 0.89400f}},  // RAmar
    {{0.19420f, -0.11360f, 0.97440f}, {0.25240f, -0.01580f, 0.96750f}, {0.05970f, -0.09980f, 0.99320f}, {0.12740f, -0.14130f, 0.98170f}, {0.00050f, -0.15060f, 0.98860f}, {0.27350f, -0.05070f, 0.96050f}, {0.31450f, 0.00690f, 0.94920f}},  // RAcast
    {{-0.47610f, -0.14300f, 0.86770f}, {-0.59190f, -0.12770f, 0.79580f}, {-0.35110f, -0.16180f, 0.92230f}, {-0.26310f, -0.19010f, 0.94580f}, {-0.31000f, -0.17440f, 0.93460f}, {-0.20760f, -0.17860f, 0.96180f}, {-0.43810f, -0.12450f, 0.89020f}},  // RAcaseal
    {{-0.20010f, -0.03350f, 0.97920f}, {-0.20020f, -0.02310f, 0.97950f}, {-0.11980f, -0.07340f, 0.99010f}, {-0.19390f, -0.03360f, 0.98040f}, {-0.19200f, -0.06190f, 0.97940f}, {-0.19900f, -0.05850f, 0.97820f}, {-0.09280f, -0.09810f, 0.99080f}},  // FOmarl
    {{-0.01360f, -0.34220f, 0.93950f}, {-0.01270f, -0.36160f, 0.93230f}, {-0.00600f, -0.39910f, 0.91690f}, {-0.02130f, -0.35290f, 0.93540f}, {-0.00580f, -0.34270f, 0.93940f}, {-0.01130f, -0.36190f, 0.93220f}, {0.00510f, -0.41650f, 0.90910f}},  // FOnewm
    {{-0.21530f, -0.07020f, 0.97400f}, {0.12990f, -0.20970f, 0.96910f}, {-0.18010f, -0.04170f, 0.98280f}, {-0.17840f, -0.09310f, 0.97950f}, {-0.21160f, -0.09900f, 0.97230f}, {-0.18830f, -0.09410f, 0.97760f}, {-0.17670f, -0.06030f, 0.98240f}},  // FOnewearl
    {{0.05660f, -0.32380f, 0.94440f}, {0.00550f, -0.26660f, 0.96380f}, {0.01180f, -0.35750f, 0.93380f}, {0.00270f, -0.25640f, 0.96660f}, {0.07090f, -0.31730f, 0.94570f}, {0.03290f, -0.26240f, 0.96440f}, {0.03260f, -0.38400f, 0.92270f}},  // HUcaseal
    {{-0.15730f, 0.07640f, 0.98460f}, {-0.19220f, 0.08380f, 0.97780f}, {-0.19880f, 0.13700f, 0.97040f}, {-0.14210f, 0.04240f, 0.98890f}, {-0.03910f, -0.24240f, 0.96940f}, {-0.09610f, -0.06370f, 0.99330f}, {-0.10310f, -0.14470f, 0.98410f}},  // FOmar
    {{0.02170f, -0.28950f, 0.95690f}, {0.02160f, -0.31350f, 0.94940f}, {0.05990f, -0.29400f, 0.95390f}, {0.00810f, -0.26700f, 0.96370f}, {0.25510f, 0.10120f, 0.96160f}, {0.04250f, -0.40440f, 0.91360f}, {0.07540f, -0.27180f, 0.95940f}},  // RAmarl
};

// The shape whose channel direction the build's change is measured from:
// the class's default-proportions body.
constexpr int HOLE_ANCHOR_SHAPE = 3;

// In a sector whose corners do not include shape 1, the FOmar's hand
// keeps the class file's node scale, so it blends from these rows there.
constexpr float PROPORTION_SHAPE_HOLE_POINT_FOMAR_KEPT[7][2] = {{0.07350f, -0.20400f}, {0.05560f, -0.12900f}, {0.09150f, -0.05260f}, {0.06900f, -0.13630f}, {0.03450f, 0.23090f}, {0.06580f, 0.08590f}, {0.18820f, -0.10810f}};
constexpr float PROPORTION_SHAPE_HOLE_DIR_FOMAR_KEPT[7][3] = {{-0.12830f, 0.03430f, 0.99110f}, {-0.19220f, 0.08380f, 0.97780f}, {-0.16900f, 0.03960f, 0.98480f}, {-0.14520f, 0.01400f, 0.98930f}, {-0.03910f, -0.24240f, 0.96940f}, {-0.09610f, -0.06370f, 0.99330f}, {-0.10310f, -0.14470f, 0.98410f}};
// END hand tables (generated)

// The hand used when there is no proportion seed: the HUmar's
// default-proportions body, shape 3 (the pad's centre corner).
constexpr int REFERENCE_HAND_CLASS = 0;
constexpr int REFERENCE_HAND_SHAPE = 3;
// The FOmar's class byte and the shape whose sectors blend its glove scale.
constexpr int FOMAR_CLASS = 10;
constexpr int FOMAR_SCALED_SHAPE = 1;

// The character the seed was computed for (entity, class byte and pair),
// its standing head height in units, and its right hand's fist channel
// (the channel tables blended the same way).
inline bool seed_valid = false;
inline float seed_units = 0.0f;
inline float seed_hole_point[2] = {};
inline float seed_hole_dir[3] = {0.0f, 0.0f, 1.0f};
inline float seed_hole_anchor[3] = {0.0f, 0.0f, 1.0f};
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

// A per-shape measure at (x, y): of seven values in shape slot order, the
// sector's three blended with the barycentric weights of 0x4E86E4, as the
// game blends the shapes' vertices.
inline float BlendShapes(const float* seven, float x, float y, int sector) {
    const int* corner = PAD_SECTORS[sector];
    const float* p0 = PAD_CORNERS[corner[0]];
    const float* p1 = PAD_CORNERS[corner[1]];
    const float* p2 = PAD_CORNERS[corner[2]];
    const float c = (x - p0[0]) / (p2[0] - p0[0]);
    const float b = ((y - p0[1]) - c * (p2[1] - p0[1])) / (p1[1] - p0[1]);
    const float a = 1.0f - (b + c);
    return a * seven[CornerShape(corner[0])] + b * seven[CornerShape(corner[1])] +
           c * seven[CornerShape(corner[2])];
}

// The class's rest head height at (x, y).
inline float RestHeight(int class_id, float x, float y, int sector) {
    return BlendShapes(PROPORTION_SHAPE_HEIGHTS[class_id], x, y, sector);
}

// Whether a pad sector blends the FOmar's glove scale: some corner shape of
// the sector is the shape that carries it. Where none does, the class file's
// scale stays and the hand is built differently (the kept rows).
inline bool UsesKeptRows(int class_id, int sector) {
    if (class_id != FOMAR_CLASS)
        return false;
    for (int i = 0; i < 3; i++)
        if (CornerShape(PAD_SECTORS[sector][i]) == FOMAR_SCALED_SHAPE)
            return false;
    return true;
}

// A class's channel row set for a pad sector: point rows [7][2] and
// direction rows [7][3].
struct ChannelRows {
    const float (*point)[2];
    const float (*dir)[3];
};
inline ChannelRows ChannelRowsFor(int class_id, int sector) {
    if (UsesKeptRows(class_id, sector))
        return {PROPORTION_SHAPE_HOLE_POINT_FOMAR_KEPT, PROPORTION_SHAPE_HOLE_DIR_FOMAR_KEPT};
    return {PROPORTION_SHAPE_HOLE_POINT[class_id], PROPORTION_SHAPE_HOLE_DIR[class_id]};
}

inline void Normalize3(float* v) {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 0.0f)
        for (int i = 0; i < 3; i++)
            v[i] /= len;
}

// A class's fist channel at pad position (x, y): the rows' points and
// directions blended with the shape weights (the direction renormalized),
// and the anchor, the same rows' direction at the default-proportions body.
inline void BlendChannel(int class_id, float x, float y, int sector, float point[2],
                         float dir[3], float anchor[3]) {
    const ChannelRows rows = ChannelRowsFor(class_id, sector);
    const int* corner = PAD_SECTORS[sector];
    const float* p0 = PAD_CORNERS[corner[0]];
    const float* p1 = PAD_CORNERS[corner[1]];
    const float* p2 = PAD_CORNERS[corner[2]];
    const float c = (x - p0[0]) / (p2[0] - p0[0]);
    const float b = ((y - p0[1]) - c * (p2[1] - p0[1])) / (p1[1] - p0[1]);
    const float weight[3] = {1.0f - (b + c), b, c};
    for (int i = 0; i < 2; i++)
        point[i] = 0.0f;
    for (int i = 0; i < 3; i++)
        dir[i] = 0.0f;
    for (int n = 0; n < 3; n++) {
        const int shape = CornerShape(corner[n]);
        for (int i = 0; i < 2; i++)
            point[i] += weight[n] * rows.point[shape][i];
        for (int i = 0; i < 3; i++)
            dir[i] += weight[n] * rows.dir[shape][i];
    }
    Normalize3(dir);
    for (int i = 0; i < 3; i++)
        anchor[i] = rows.dir[HOLE_ANCHOR_SHAPE][i];
    Normalize3(anchor);
}

// The shapes and weights the seeded character's hand blends (the same three
// corner shapes and barycentric weights as BlendChannel), and whether its
// class takes the kept rows there. False with no seed, or when the seed is
// for another class.
inline bool SeedPadWeights(int class_id, int shape[3], float w[3], bool& kept) {
    if (!seed_valid || seed_class != class_id)
        return false;
    const float x = seed_proportion[0], y = seed_proportion[1];
    const int sector = PadSector(x, y);
    const int* corner = PAD_SECTORS[sector];
    const float* p0 = PAD_CORNERS[corner[0]];
    const float* p1 = PAD_CORNERS[corner[1]];
    const float* p2 = PAD_CORNERS[corner[2]];
    const float c = (x - p0[0]) / (p2[0] - p0[0]);
    const float b = ((y - p0[1]) - c * (p2[1] - p0[1])) / (p1[1] - p0[1]);
    w[0] = 1.0f - (b + c);
    w[1] = b;
    w[2] = c;
    for (int n = 0; n < 3; n++)
        shape[n] = CornerShape(corner[n]);
    kept = UsesKeptRows(class_id, sector);
    return true;
}

// A channel direction as two turns of the handle's +z, in radians: about x
// by tilt (positive toward -y, the fingers), then about y by yaw (positive
// toward +x, out of the palm).
inline void ChannelTurns(const float d[3], float& tilt, float& yaw) {
    tilt = atan2f(-d[1], sqrtf(d[0] * d[0] + d[2] * d[2]));
    yaw = atan2f(d[0], d[2]);
}

// A fist channel ready for the seat: the point, the blend's turns, the
// anchor's turns and the turns the seat uses. The seat's turns move
// `follow` of the way from the anchor's (the default build's hand) to the
// blend's: 1 = the blend itself, 0 = the default build's angle on every
// build. The point stays the blend's.
struct ChannelSeat {
    float point[2];
    float blend_tilt, blend_yaw;
    float anchor_tilt, anchor_yaw;
    float tilt, yaw;
};
inline ChannelSeat ResolveChannel(const float point[2], const float dir[3],
                                  const float anchor[3], float follow) {
    ChannelSeat s;
    s.point[0] = point[0];
    s.point[1] = point[1];
    ChannelTurns(dir, s.blend_tilt, s.blend_yaw);
    ChannelTurns(anchor, s.anchor_tilt, s.anchor_yaw);
    if (follow == 1.0f) {
        s.tilt = s.blend_tilt;
        s.yaw = s.blend_yaw;
    } else {
        s.tilt = s.anchor_tilt + follow * (s.blend_tilt - s.anchor_tilt);
        s.yaw = s.anchor_yaw + follow * (s.blend_yaw - s.anchor_yaw);
    }
    return s;
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
    BlendChannel(class_id, pair[0], pair[1], sector, seed_hole_point, seed_hole_dir,
                 seed_hole_anchor);
    seed_entity = entity;
    seed_class = class_id;
    memcpy(seed_proportion, pair, sizeof(seed_proportion));
    seed_valid = true;
    seed_fail_entity = 0;
    diag::Log("eyeheight: seeded from the proportions: class %d pair %.3f / %.3f -> sector %d, "
              "rest %.2f x %.3f + %.2f = %.2f units (%.3f m at scale %.2f); "
              "fist channel point %.4f / %.4f units, direction %.4f / %.4f / %.4f",
              class_id, pair[0], pair[1], sector, rest, IDLE_HEAD_FACTOR, height_offset,
              seed_units, seed_units / vrmod::config.world_scale, vrmod::config.world_scale,
              seed_hole_point[0], seed_hole_point[1], seed_hole_dir[0], seed_hole_dir[1],
              seed_hole_dir[2]);
    const ChannelSeat seat = ResolveChannel(seed_hole_point, seed_hole_dir, seed_hole_anchor,
                                            vrmod::config.melee_angle_follow);
    const float r2d = 180.0f / 3.14159265f;
    diag::Log("eyeheight: fist channel for class %d pad %.3f / %.3f: point %.4f / %.4f, blend "
              "tilt %.2f yaw %.2f deg, default build tilt %.2f yaw %.2f, followed (%.2f) tilt "
              "%.2f yaw %.2f",
              class_id, pair[0], pair[1], seat.point[0], seat.point[1], seat.blend_tilt * r2d,
              seat.blend_yaw * r2d, seat.anchor_tilt * r2d, seat.anchor_yaw * r2d,
              vrmod::config.melee_angle_follow, seat.tilt * r2d, seat.yaw * r2d);
    return true;
}

// The seeded character's fist channel in the grip point's rest frame: the
// point (x, y at z = 0, game units), the unit direction and the anchor
// direction (the class's default-proportions body), blended for its build.
// False with no proportion seed (the sampled height says nothing about the
// hand).
inline bool HandChannel(float point[2], float dir[3], float anchor[3]) {
    if (!seed_valid)
        return false;
    memcpy(point, seed_hole_point, sizeof(seed_hole_point));
    memcpy(dir, seed_hole_dir, sizeof(seed_hole_dir));
    memcpy(anchor, seed_hole_anchor, sizeof(seed_hole_anchor));
    return true;
}

// The reference hand's channel, the stand-in without a proportion seed: the
// reference body's own row, which is also its anchor.
inline void HandChannelReference(float point[2], float dir[3], float anchor[3]) {
    for (int i = 0; i < 2; i++)
        point[i] = PROPORTION_SHAPE_HOLE_POINT[REFERENCE_HAND_CLASS][REFERENCE_HAND_SHAPE][i];
    for (int i = 0; i < 3; i++)
        dir[i] = PROPORTION_SHAPE_HOLE_DIR[REFERENCE_HAND_CLASS][REFERENCE_HAND_SHAPE][i];
    Normalize3(dir);
    for (int i = 0; i < 3; i++)
        anchor[i] = dir[i];
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
